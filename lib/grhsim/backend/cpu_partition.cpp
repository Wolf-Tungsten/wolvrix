#include "grhsim/backend/cpu.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>
#include <variant>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        constexpr uint32_t absent = std::numeric_limits<uint32_t>::max();
        using Clusters = std::vector<std::vector<uint32_t>>;

        const Parameter *findParameter(const GrhSimModel &model, std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const auto &parameter : parameters)
                if (model.text(parameter.name) == name) return &parameter;
            return nullptr;
        }

        bool isMemWriteOp(std::string_view type) noexcept
        {
            return type == "core.state.memWrite" || type == "core.state.memFill" ||
                   type == "core.state.memAssign" || type == "core.state.memWriteSeq";
        }

        // The op's event act set A(op): its event_acts cluster indices,
        // sorted and deduplicated; empty for event-free ops.
        std::vector<int64_t> readEventActs(const GrhSimModel &model, const SimOp &op)
        {
            const Parameter *parameter = findParameter(model, model.parameters(op), "event_acts");
            if (!parameter) return {};
            const auto *indices = std::get_if<std::vector<int64_t>>(&parameter->value);
            if (!indices) throw std::runtime_error("event_acts must be an int64 array");
            auto acts = *indices;
            std::sort(acts.begin(), acts.end());
            acts.erase(std::unique(acts.begin(), acts.end()), acts.end());
            return acts;
        }

        bool unionInto(std::vector<int64_t> &target, const std::vector<int64_t> &source)
        {
            if (source.empty()) return false;
            const auto middle = target.size();
            target.insert(target.end(), source.begin(), source.end());
            std::inplace_merge(target.begin(), target.begin() + middle, target.end());
            target.erase(std::unique(target.begin(), target.end()), target.end());
            return target.size() != middle;
        }

        // Merge-time event domain state of a node/cluster (six-phase model,
        // P_general supernode event domain constraint): hasEvent marks the
        // presence of event-carrying ops, uniform tracks rule 1 (all event
        // act sets equal), acts is the common set K, and influence is the
        // E(.) downstream act closure union.
        struct EventDomainInfo
        {
            bool hasEvent = false;
            bool uniform = true;
            std::vector<int64_t> acts;
            std::vector<int64_t> influence;
        };

        EventDomainInfo combineEventDomains(const EventDomainInfo &lhs, const EventDomainInfo &rhs)
        {
            EventDomainInfo result;
            result.hasEvent = lhs.hasEvent || rhs.hasEvent;
            result.uniform = lhs.uniform && rhs.uniform &&
                             (!lhs.hasEvent || !rhs.hasEvent || lhs.acts == rhs.acts);
            result.acts = lhs.hasEvent ? lhs.acts : rhs.acts;
            result.influence = lhs.influence;
            unionInto(result.influence, rhs.influence);
            return result;
        }

        // The merge prohibition: an event-carrying supernode must keep one
        // common act set K (rule 1) whose downstream closure introduces no
        // other acts (rule 2). Pure combinational/latch-only clusters are
        // exempt from both rules.
        bool eventDomainAllowed(const EventDomainInfo &info)
        {
            return !info.hasEvent || (info.uniform && info.influence == info.acts);
        }

        std::vector<EventDomainInfo> clusterEventDomains(const Clusters &clusters,
                                                         std::span<const EventDomainInfo> nodeInfos)
        {
            std::vector<EventDomainInfo> result(clusters.size());
            for (uint32_t i = 0; i < clusters.size(); ++i)
                for (const auto node : clusters[i])
                    result[i] = combineEventDomains(result[i], nodeInfos[node]);
            return result;
        }

        PartitionId phasePartition(const CpuPartitionTree &tree, CpuPhase phase)
        {
            for (auto id : tree.partitions[tree.root.index - 1].children)
                if (tree.partitions[id.index - 1].attrs.phase == phase) return id;
            throw std::runtime_error("CPU phase partition is missing");
        }

        PartitionId addPartition(CpuPartitionTree &tree, PartitionId parent, CpuPartitionKind kind)
        {
            if (tree.partitions.size() >= absent) throw std::overflow_error("CPU partition ID overflow");
            const PartitionId id{static_cast<uint32_t>(tree.partitions.size() + 1), 0};
            CpuPartition partition;
            partition.id = id;
            partition.parent = parent;
            partition.attrs.kind = kind;
            tree.partitions.push_back(std::move(partition));
            if (parent) tree.partitions[parent.index - 1].children.push_back(id);
            return id;
        }

        void attach(CpuPartitionTree &tree, PartitionId parent, PartitionId child)
        {
            tree.partitions[parent.index - 1].children.push_back(child);
            tree.partitions[child.index - 1].parent = parent;
        }

        std::vector<OpId> partitionOps(const CpuPartitionTree &tree, PartitionId root)
        {
            std::vector<OpId> result;
            std::vector<PartitionId> stack{root};
            while (!stack.empty())
            {
                const auto &partition = tree.partitions[stack.back().index - 1];
                stack.pop_back();
                result.insert(result.end(), partition.ops.begin(), partition.ops.end());
                stack.insert(stack.end(), partition.children.rbegin(), partition.children.rend());
            }
            return result;
        }

        struct ComputeGraph
        {
            std::vector<OpId> producer;
            std::vector<uint32_t> useOffsets;
            std::vector<OpId> uses;
            std::vector<bool> compute;
            std::vector<OpId> topo;

            ComputeGraph(const GrhSimModel &model, std::span<const OpId> ops)
                : producer(model.values().size() + 1), useOffsets(model.values().size() + 2),
                  compute(model.operations().size() + 1)
            {
                for (auto op : ops) compute[op.index] = true;
                for (const auto &op : model.operations())
                {
                    for (auto value : model.results(op)) producer[value.index] = op.id;
                    for (auto value : model.operands(op)) ++useOffsets[value.index + 1];
                }
                std::partial_sum(useOffsets.begin(), useOffsets.end(), useOffsets.begin());
                uses.resize(useOffsets.back());
                auto cursor = useOffsets;
                for (const auto &op : model.operations())
                    for (auto value : model.operands(op)) uses[cursor[value.index]++] = op.id;
                std::vector<uint32_t> indegree(compute.size());
                std::vector<OpId> ready;
                for (auto id : ops)
                {
                    const auto operands = model.operands(model.operations()[id.index - 1]);
                    for (auto value : operands)
                    {
                        if (!compute[producer[value.index].index])
                            throw std::runtime_error("compute op depends on a commit result");
                        ++indegree[id.index];
                    }
                    if (operands.empty()) ready.push_back(id);
                }
                std::reverse(ready.begin(), ready.end());
                topo.reserve(ops.size());
                while (!ready.empty())
                {
                    const auto id = ready.back(); ready.pop_back();
                    topo.push_back(id);
                    for (auto value : model.results(model.operations()[id.index - 1]))
                        for (uint32_t i = useOffsets[value.index]; i < useOffsets[value.index + 1]; ++i)
                            if (compute[uses[i].index] && --indegree[uses[i].index] == 0) ready.push_back(uses[i]);
                }
                if (topo.size() != ops.size()) throw std::runtime_error("CPU compute graph contains a combinational cycle");
            }
        };

        struct Edge
        {
            uint32_t source, target, value;
            friend auto operator<=>(const Edge &, const Edge &) = default;
        };

        struct ClusterGraph
        {
            std::vector<Edge> edges;
            std::vector<std::vector<uint32_t>> successors, predecessors;

            ClusterGraph(const Clusters &clusters, std::span<const Edge> nodeEdges, std::size_t nodeCount)
                : successors(clusters.size()), predecessors(clusters.size())
            {
                std::vector<uint32_t> owner(nodeCount);
                for (uint32_t i = 0; i < clusters.size(); ++i)
                    for (auto node : clusters[i]) owner[node] = i;
                edges.reserve(nodeEdges.size());
                for (auto edge : nodeEdges)
                    if (owner[edge.source] != owner[edge.target])
                        edges.push_back({owner[edge.source], owner[edge.target], edge.value});
                std::sort(edges.begin(), edges.end());
                edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
                for (auto edge : edges)
                {
                    auto &succ = successors[edge.source];
                    if (!succ.empty() && succ.back() == edge.target) continue;
                    succ.push_back(edge.target);
                    predecessors[edge.target].push_back(edge.source);
                }
            }

            std::vector<uint32_t> order() const
            {
                std::vector<uint32_t> degree(predecessors.size()), ready, result;
                for (uint32_t i = 0; i < degree.size(); ++i)
                {
                    degree[i] = predecessors[i].size();
                    if (degree[i] == 0) ready.push_back(i);
                }
                std::reverse(ready.begin(), ready.end());
                while (!ready.empty())
                {
                    const auto id = ready.back(); ready.pop_back(); result.push_back(id);
                    for (auto target : successors[id]) if (--degree[target] == 0) ready.push_back(target);
                }
                return result;
            }
        };

        bool orderClusters(Clusters &clusters, std::span<const Edge> edges, std::size_t nodeCount)
        {
            const auto order = ClusterGraph(clusters, edges, nodeCount).order();
            if (order.size() != clusters.size()) return false;
            Clusters ordered;
            ordered.reserve(clusters.size());
            for (auto id : order) ordered.push_back(std::move(clusters[id]));
            clusters = std::move(ordered);
            return true;
        }

        std::vector<Edge> nodeEdges(const GrhSimModel &model, const ComputeGraph &graph,
                                     std::span<const uint32_t> nodeOfOp)
        {
            std::vector<Edge> edges;
            for (auto id : graph.topo)
                for (auto value : model.operands(model.operations()[id.index - 1]))
                {
                    const auto source = nodeOfOp[graph.producer[value.index].index];
                    const auto target = nodeOfOp[id.index];
                    if (source != target) edges.push_back({source, target, value.index});
                }
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            return edges;
        }

        struct NodeStats
        {
            uint64_t nodes = 0;
            uint64_t boundaryEdges = 0;
        };

        NodeStats formNodes(const GrhSimModel &model, CpuPartitionTree &tree, PartitionId phase,
                            std::vector<OpId> ops, uint32_t maxOps)
        {
            ComputeGraph graph(model, ops);
            std::vector<uint32_t> owner(model.operations().size() + 1, absent), sizes;
            // Reverse-topological cone absorption stops at shared values and commit boundaries.
            // No source cloning: every op remains owned by exactly one mapping leaf.
            for (auto it = graph.topo.rbegin(); it != graph.topo.rend(); ++it)
            {
                const auto &op = model.operations()[it->index - 1];
                const auto type = model.text(op.opType);
                bool absorb = type.starts_with("core.compute.") || type == "core.input.read" ||
                              type == "core.state.read" || type == "core.state.memRead";
                uint32_t target = absent;
                for (auto value : model.results(op))
                    for (uint32_t i = graph.useOffsets[value.index]; i < graph.useOffsets[value.index + 1]; ++i)
                    {
                        const auto user = graph.uses[i];
                        if (!graph.compute[user.index]) { absorb = false; continue; }
                        if (target == absent) target = owner[user.index];
                        else if (target != owner[user.index]) absorb = false;
                    }
                if (!absorb || target == absent || sizes[target] >= maxOps)
                {
                    target = sizes.size(); sizes.push_back(0);
                }
                owner[op.id.index] = target;
                ++sizes[target];
            }
            std::vector<std::vector<OpId>> nodeOps(sizes.size());
            for (auto id : graph.topo) nodeOps[owner[id.index]].push_back(id);
            const auto edges = nodeEdges(model, graph, owner);
            Clusters clusters(nodeOps.size());
            for (uint32_t i = 0; i < clusters.size(); ++i) clusters[i].push_back(i);
            if (!orderClusters(clusters, edges, nodeOps.size()))
                throw std::runtime_error("CPU node contraction introduced a dependency cycle");
            for (const auto &cluster : clusters)
            {
                const auto id = addPartition(tree, phase, CpuPartitionKind::Node);
                tree.partitions[id.index - 1].ops = std::move(nodeOps[cluster.front()]);
            }
            return {sizes.size(), edges.size()};
        }

        void buildNodes(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t maxOps,
                        diag::Diagnostics &diagnostics)
        {
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::Compute);
            auto ops = std::move(tree.partitions[phase.index - 1].ops);
            const auto stats = formNodes(model, tree, phase, std::move(ops), maxOps);
            diagnostics.info("compute_nodes=" + std::to_string(stats.nodes) +
                             " boundary_value_targets=" + std::to_string(stats.boundaryEdges), "cpu.st.build-compute-nodes");
        }

        // Six-phase variant (cpu.st.build-general-nodes): the General branch is
        // an empty shell after split-phases, so node formation works on the
        // model's General-phase op list. reg/latch write ops and General-phase
        // system.task/dpi.call are mergeable sinks: they anchor nodes (they
        // are never absorbed) while their single-consumer operand cones absorb
        // into the sink's node.
        void buildGeneralNodes(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t maxOps,
                               diag::Diagnostics &diagnostics)
        {
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::General);
            std::vector<OpId> ops;
            for (const auto &op : model.operations())
                if (op.phase == SimPhase::General) ops.push_back(op.id);
            const auto stats = formNodes(model, tree, phase, std::move(ops), maxOps);
            diagnostics.info("general_nodes=" + std::to_string(stats.nodes) +
                             " boundary_value_targets=" + std::to_string(stats.boundaryEdges), "cpu.st.build-general-nodes");
        }

        uint64_t clusterSize(const std::vector<uint32_t> &cluster, std::span<const uint32_t> sizes)
        {
            uint64_t result = 0;
            for (auto node : cluster) result += sizes[node];
            return result;
        }

        bool coarsen(Clusters &clusters, std::span<const Edge> edges, std::span<const uint32_t> sizes,
                     uint32_t maxOps, unsigned mode)
        {
            ClusterGraph graph(clusters, edges, sizes.size());
            std::vector<uint32_t> parent(clusters.size());
            std::iota(parent.begin(), parent.end(), 0);
            std::vector<uint64_t> weights;
            for (const auto &cluster : clusters) weights.push_back(clusterSize(cluster, sizes));
            const auto find = [&](uint32_t id) {
                while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
                return id;
            };
            bool changed = false;
            const auto merge = [&](uint32_t a, uint32_t b) {
                a = find(a); b = find(b);
                if (a == b || weights[a] + weights[b] > maxOps) return false;
                if (a > b) std::swap(a, b);
                parent[b] = a; weights[a] += weights[b]; changed = true;
                return true;
            };
            if (mode == 2)
            {
                std::map<std::vector<uint32_t>, uint32_t> anchors;
                for (uint32_t i = 0; i < clusters.size(); ++i)
                {
                    if (graph.predecessors[i].empty()) continue;
                    auto [it, inserted] = anchors.emplace(graph.predecessors[i], i);
                    if (!inserted && !merge(it->second, i)) it->second = i;
                }
            }
            else
            {
                struct Candidate { uint32_t source, target, weight; };
                std::vector<Candidate> candidates;
                for (auto edge : graph.edges)
                {
                    if (mode == 0 ? graph.successors[edge.source].size() != 1 : graph.predecessors[edge.target].size() != 1)
                        continue;
                    if (!candidates.empty() && candidates.back().source == edge.source && candidates.back().target == edge.target)
                        ++candidates.back().weight;
                    else candidates.push_back({edge.source, edge.target, 1});
                }
                std::sort(candidates.begin(), candidates.end(), [](auto a, auto b) {
                    if (a.weight != b.weight) return a.weight > b.weight;
                    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
                });
                for (auto candidate : candidates) merge(candidate.source, candidate.target);
            }
            if (!changed) return false;
            Clusters result;
            std::vector<uint32_t> index(clusters.size(), absent);
            for (uint32_t i = 0; i < clusters.size(); ++i)
            {
                const auto root = find(i);
                if (index[root] == absent) { index[root] = result.size(); result.emplace_back(); }
                auto &members = result[index[root]];
                members.insert(members.end(), clusters[i].begin(), clusters[i].end());
            }
            for (auto &members : result) std::sort(members.begin(), members.end());
            // Batch contractions are accepted only when the quotient remains a DAG.
            if (!orderClusters(result, edges, sizes.size())) return false;
            clusters = std::move(result);
            return true;
        }

        Clusters segment(const Clusters &clusters, const ClusterGraph &graph,
                         std::span<const uint32_t> sizes, std::size_t valueCount, uint32_t maxOps)
        {
            std::vector<std::vector<uint32_t>> sources(clusters.size()), targets(clusters.size());
            std::vector<uint32_t> sourceOfValue(valueCount + 1, absent);
            for (auto edge : graph.edges)
            {
                sources[edge.source].push_back(edge.value);
                targets[edge.target].push_back(edge.value);
                sourceOfValue[edge.value] = edge.source;
            }
            for (auto *table : {&sources, &targets})
                for (auto &row : *table)
                {
                    std::sort(row.begin(), row.end()); row.erase(std::unique(row.begin(), row.end()), row.end());
                }
            std::vector<uint64_t> prefix(clusters.size() + 1);
            for (std::size_t i = 0; i < clusters.size(); ++i) prefix[i + 1] = prefix[i] + clusterSize(clusters[i], sizes);
            const auto infinity = std::numeric_limits<uint64_t>::max();
            std::vector<uint64_t> cost(clusters.size() + 1, infinity);
            std::vector<uint32_t> previous(clusters.size() + 1), seen(valueCount + 1), counted(valueCount + 1);
            cost[0] = 0;
            // Legacy's uniform-weight objective: distinct incoming activation values + one per segment.
            for (uint32_t end = 1; end <= clusters.size(); ++end)
            {
                uint64_t incoming = 0;
                for (uint32_t begin = end; begin > 0;)
                {
                    --begin;
                    if (prefix[end] - prefix[begin] > maxOps)
                    {
                        if (begin + 1 == end) continue;
                        break;
                    }
                    for (auto value : targets[begin])
                    {
                        if (seen[value] == end) continue;
                        seen[value] = end;
                        if (sourceOfValue[value] < begin) { counted[value] = end; ++incoming; }
                    }
                    for (auto value : sources[begin])
                        if (counted[value] == end) { counted[value] = 0; --incoming; }
                    const auto candidate = cost[begin] + incoming + 1;
                    if (candidate <= cost[end]) { cost[end] = candidate; previous[end] = begin; }
                }
                if (cost[end] == infinity) { cost[end] = cost[end - 1] + 1; previous[end] = end - 1; }
            }
            Clusters result;
            for (uint32_t end = clusters.size(); end > 0;)
            {
                const auto begin = previous[end];
                std::vector<uint32_t> members;
                for (uint32_t i = begin; i < end; ++i) members.insert(members.end(), clusters[i].begin(), clusters[i].end());
                std::sort(members.begin(), members.end());
                result.push_back(std::move(members)); end = begin;
            }
            std::reverse(result.begin(), result.end());
            return result;
        }

        // Six-phase coarsen variant with the event-domain merge prohibition:
        // every union-find merge additionally combines the two clusters' event
        // domain state and rejects merges whose event-carrying result breaks
        // rule 1 (one common act set) or rule 2 (downstream closure stays in
        // that set). Rejected attempts are counted for diagnostics.
        bool coarsenGeneral(Clusters &clusters, std::span<const Edge> edges, std::span<const uint32_t> sizes,
                            uint32_t maxOps, unsigned mode, std::span<const EventDomainInfo> nodeInfos,
                            uint64_t &blocked)
        {
            ClusterGraph graph(clusters, edges, sizes.size());
            auto infos = clusterEventDomains(clusters, nodeInfos);
            std::vector<uint32_t> parent(clusters.size());
            std::iota(parent.begin(), parent.end(), 0);
            std::vector<uint64_t> weights;
            for (const auto &cluster : clusters) weights.push_back(clusterSize(cluster, sizes));
            const auto find = [&](uint32_t id) {
                while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
                return id;
            };
            bool changed = false;
            const auto merge = [&](uint32_t a, uint32_t b) {
                a = find(a); b = find(b);
                if (a == b || weights[a] + weights[b] > maxOps) return false;
                if (a > b) std::swap(a, b);
                auto combined = combineEventDomains(infos[a], infos[b]);
                if (!eventDomainAllowed(combined)) { ++blocked; return false; }
                parent[b] = a; weights[a] += weights[b]; infos[a] = std::move(combined); changed = true;
                return true;
            };
            if (mode == 2)
            {
                std::map<std::vector<uint32_t>, uint32_t> anchors;
                for (uint32_t i = 0; i < clusters.size(); ++i)
                {
                    if (graph.predecessors[i].empty()) continue;
                    auto [it, inserted] = anchors.emplace(graph.predecessors[i], i);
                    if (!inserted && !merge(it->second, i)) it->second = i;
                }
            }
            else
            {
                struct Candidate { uint32_t source, target, weight; };
                std::vector<Candidate> candidates;
                for (auto edge : graph.edges)
                {
                    if (mode == 0 ? graph.successors[edge.source].size() != 1 : graph.predecessors[edge.target].size() != 1)
                        continue;
                    if (!candidates.empty() && candidates.back().source == edge.source && candidates.back().target == edge.target)
                        ++candidates.back().weight;
                    else candidates.push_back({edge.source, edge.target, 1});
                }
                std::sort(candidates.begin(), candidates.end(), [](auto a, auto b) {
                    if (a.weight != b.weight) return a.weight > b.weight;
                    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
                });
                for (auto candidate : candidates) merge(candidate.source, candidate.target);
            }
            if (!changed) return false;
            Clusters result;
            std::vector<uint32_t> index(clusters.size(), absent);
            for (uint32_t i = 0; i < clusters.size(); ++i)
            {
                const auto root = find(i);
                if (index[root] == absent) { index[root] = result.size(); result.emplace_back(); }
                auto &members = result[index[root]];
                members.insert(members.end(), clusters[i].begin(), clusters[i].end());
            }
            for (auto &members : result) std::sort(members.begin(), members.end());
            // Batch contractions are accepted only when the quotient remains a DAG.
            if (!orderClusters(result, edges, sizes.size())) return false;
            clusters = std::move(result);
            return true;
        }

        // Six-phase DP segmentation with the event-domain prohibition: a
        // multi-cluster span whose combined domain state breaks the rules can
        // never become legal by growing further (uniformity is sticky and the
        // influence union only grows), so the span scan stops there. Single
        // clusters are exempt: node formation already fixed their contents.
        Clusters segmentGeneral(const Clusters &clusters, const ClusterGraph &graph,
                                std::span<const uint32_t> sizes, std::size_t valueCount, uint32_t maxOps,
                                std::span<const EventDomainInfo> infos, uint64_t &blocked)
        {
            std::vector<std::vector<uint32_t>> sources(clusters.size()), targets(clusters.size());
            std::vector<uint32_t> sourceOfValue(valueCount + 1, absent);
            for (auto edge : graph.edges)
            {
                sources[edge.source].push_back(edge.value);
                targets[edge.target].push_back(edge.value);
                sourceOfValue[edge.value] = edge.source;
            }
            for (auto *table : {&sources, &targets})
                for (auto &row : *table)
                {
                    std::sort(row.begin(), row.end()); row.erase(std::unique(row.begin(), row.end()), row.end());
                }
            std::vector<uint64_t> prefix(clusters.size() + 1);
            for (std::size_t i = 0; i < clusters.size(); ++i) prefix[i + 1] = prefix[i] + clusterSize(clusters[i], sizes);
            const auto infinity = std::numeric_limits<uint64_t>::max();
            std::vector<uint64_t> cost(clusters.size() + 1, infinity);
            std::vector<uint32_t> previous(clusters.size() + 1), seen(valueCount + 1), counted(valueCount + 1);
            cost[0] = 0;
            // Legacy's uniform-weight objective: distinct incoming activation values + one per segment.
            for (uint32_t end = 1; end <= clusters.size(); ++end)
            {
                uint64_t incoming = 0;
                bool spanFirst = true;
                EventDomainInfo span;
                for (uint32_t begin = end; begin > 0;)
                {
                    --begin;
                    if (spanFirst) { span = infos[begin]; spanFirst = false; }
                    else span = combineEventDomains(infos[begin], span);
                    if (begin + 1 != end && !eventDomainAllowed(span)) { ++blocked; break; }
                    if (prefix[end] - prefix[begin] > maxOps)
                    {
                        if (begin + 1 == end) continue;
                        break;
                    }
                    for (auto value : targets[begin])
                    {
                        if (seen[value] == end) continue;
                        seen[value] = end;
                        if (sourceOfValue[value] < begin) { counted[value] = end; ++incoming; }
                    }
                    for (auto value : sources[begin])
                        if (counted[value] == end) { counted[value] = 0; --incoming; }
                    const auto candidate = cost[begin] + incoming + 1;
                    if (candidate <= cost[end]) { cost[end] = candidate; previous[end] = begin; }
                }
                if (cost[end] == infinity) { cost[end] = cost[end - 1] + 1; previous[end] = end - 1; }
            }
            Clusters result;
            for (uint32_t end = clusters.size(); end > 0;)
            {
                const auto begin = previous[end];
                std::vector<uint32_t> members;
                for (uint32_t i = begin; i < end; ++i) members.insert(members.end(), clusters[i].begin(), clusters[i].end());
                std::sort(members.begin(), members.end());
                result.push_back(std::move(members)); end = begin;
            }
            std::reverse(result.begin(), result.end());
            return result;
        }

        void mergeSupernodes(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t maxOps,
                             diag::Diagnostics &diagnostics)
        {
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::Compute);
            const auto nodes = tree.partitions[phase.index - 1].children;
            const auto ops = partitionOps(tree, phase);
            ComputeGraph graph(model, ops);
            std::vector<uint32_t> owner(model.operations().size() + 1, absent), sizes;
            Clusters clusters(nodes.size());
            for (uint32_t i = 0; i < nodes.size(); ++i)
            {
                const auto &partition = tree.partitions[nodes[i].index - 1];
                sizes.push_back(partition.ops.size()); clusters[i].push_back(i);
                for (auto op : partition.ops) owner[op.index] = i;
            }
            const auto edges = nodeEdges(model, graph, owner);
            unsigned tail = 0, iterations = 0;
            while (!clusters.empty())
            {
                const auto before = clusters.size();
                for (unsigned mode = 0; mode < 3; ++mode) coarsen(clusters, edges, sizes, maxOps, mode);
                ++iterations;
                if (clusters.size() == before) break;
                tail = before >= 100000 && before - clusters.size() < 1024 ? tail + 1 : 0;
                if (tail == 3) break;
            }
            const auto coarsened = clusters.size();
            clusters = segment(clusters, ClusterGraph(clusters, edges, sizes.size()), sizes, model.values().size(), maxOps);
            tree.partitions[phase.index - 1].children.clear();
            for (const auto &cluster : clusters)
            {
                const auto supernode = addPartition(tree, phase, CpuPartitionKind::Supernode);
                for (auto node : cluster) attach(tree, supernode, nodes[node]);
            }
            const auto finalEdges = ClusterGraph(clusters, edges, sizes.size()).edges.size();
            diagnostics.info("coarsen_iterations=" + std::to_string(iterations) + " coarsened_clusters=" + std::to_string(coarsened) +
                             " compute_supernodes=" + std::to_string(clusters.size()) +
                             " boundary_value_targets=" + std::to_string(finalEdges), "cpu.st.merge-compute-supernodes");
        }

        // Six-phase variant (cpu.st.merge-general-supernodes): same
        // coarsen+DP frame as mergeSupernodes, plus the event-domain merge
        // prohibition from the P_general spec. Every resulting General
        // supernode records its event act union in attrs.eventActs (engaged,
        // empty array for event-free supernodes).
        void mergeGeneralSupernodes(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t maxOps,
                                    diag::Diagnostics &diagnostics)
        {
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::General);
            const auto nodes = tree.partitions[phase.index - 1].children;
            const auto ops = partitionOps(tree, phase);
            ComputeGraph graph(model, ops);
            std::vector<uint32_t> owner(model.operations().size() + 1, absent), sizes;
            Clusters clusters(nodes.size());
            for (uint32_t i = 0; i < nodes.size(); ++i)
            {
                const auto &partition = tree.partitions[nodes[i].index - 1];
                sizes.push_back(partition.ops.size()); clusters[i].push_back(i);
                for (auto op : partition.ops) owner[op.index] = i;
            }
            const auto edges = nodeEdges(model, graph, owner);
            const auto domainSets = computeCpuEventDomainSets(model);
            std::vector<EventDomainInfo> nodeInfos(nodes.size());
            for (uint32_t i = 0; i < nodes.size(); ++i)
                for (auto op : tree.partitions[nodes[i].index - 1].ops)
                {
                    EventDomainInfo info;
                    info.hasEvent = !domainSets.acts[op.index].empty();
                    info.acts = domainSets.acts[op.index];
                    info.influence = domainSets.influence[op.index];
                    nodeInfos[i] = combineEventDomains(nodeInfos[i], info);
                }
            uint64_t blocked = 0;
            unsigned tail = 0, iterations = 0;
            while (!clusters.empty())
            {
                const auto before = clusters.size();
                for (unsigned mode = 0; mode < 3; ++mode)
                    coarsenGeneral(clusters, edges, sizes, maxOps, mode, nodeInfos, blocked);
                ++iterations;
                if (clusters.size() == before) break;
                tail = before >= 100000 && before - clusters.size() < 1024 ? tail + 1 : 0;
                if (tail == 3) break;
            }
            const auto coarsened = clusters.size();
            clusters = segmentGeneral(clusters, ClusterGraph(clusters, edges, sizes.size()), sizes,
                                      model.values().size(), maxOps,
                                      clusterEventDomains(clusters, nodeInfos), blocked);
            const auto finalInfos = clusterEventDomains(clusters, nodeInfos);
            tree.partitions[phase.index - 1].children.clear();
            for (uint32_t i = 0; i < clusters.size(); ++i)
            {
                const auto supernode = addPartition(tree, phase, CpuPartitionKind::Supernode);
                tree.partitions[supernode.index - 1].attrs.eventActs = finalInfos[i].acts;
                for (auto node : clusters[i]) attach(tree, supernode, nodes[node]);
            }
            const auto finalEdges = ClusterGraph(clusters, edges, sizes.size()).edges.size();
            diagnostics.info("coarsen_iterations=" + std::to_string(iterations) + " coarsened_clusters=" + std::to_string(coarsened) +
                             " general_supernodes=" + std::to_string(clusters.size()) +
                             " boundary_value_targets=" + std::to_string(finalEdges) +
                             " event_domain_blocked=" + std::to_string(blocked), "cpu.st.merge-general-supernodes");
        }

        uint64_t estimatedLines(const GrhSimModel &model, OpId id)
        {
            const auto &op = model.operations()[id.index - 1];
            uint64_t lines = 4 + op.operands.count + op.results.count;
            for (auto value : model.results(op))
            {
                const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
                if (type.kind == TypeKind::Logic) lines += (uint64_t(type.width) + 63) / 64;
            }
            return lines;
        }

        void packWords(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t helperLines)
        {
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::Compute);
            const auto supernodes = std::move(tree.partitions[phase.index - 1].children);
            PartitionId word;
            for (uint32_t active = 0; active < supernodes.size(); ++active)
            {
                if (active % 8 == 0)
                {
                    word = addPartition(tree, phase, CpuPartitionKind::ActiveWord);
                    tree.partitions[word.index - 1].attrs.activeWord = active / 8;
                }
                const auto id = supernodes[active];
                attach(tree, word, id);
                auto &attrs = tree.partitions[id.index - 1].attrs;
                attrs.activeId = active;
                const auto ops = partitionOps(tree, id);
                uint64_t lines = 0;
                uint32_t begin = 0;
                for (uint32_t i = 0; i < ops.size(); ++i)
                {
                    const auto estimate = estimatedLines(model, ops[i]);
                    if (i > begin && lines + estimate > helperLines)
                    { attrs.helperChunks.push_back({begin, i - begin}); begin = i; lines = 0; }
                    lines += estimate;
                }
                if (!attrs.helperChunks.empty() || lines > helperLines)
                    attrs.helperChunks.push_back({begin, static_cast<uint32_t>(ops.size()) - begin});
            }
        }

        void packFunctions(const GrhSimModel &model, CpuBackendMapping &mapping,
                           uint32_t maxOps, uint32_t maxLines, uint32_t targetCount)
        {
            auto &tree = mapping.partitionTree;
            std::vector<PartitionId> parents{phasePartition(tree, CpuPhase::Compute)};
            const auto commit = phasePartition(tree, CpuPhase::Commit);
            const auto &domains = tree.partitions[commit.index - 1].children;
            parents.insert(parents.end(), domains.begin(), domains.end());
            uint64_t totalOps = model.operations().size(), totalLines = 0;
            for (const auto &op : model.operations()) totalLines += estimatedLines(model, op.id);
            const auto effectiveOps = targetCount ? std::max(uint64_t(maxOps), totalOps / targetCount) : maxOps;
            const auto effectiveLines = targetCount ? std::max(uint64_t(maxLines), totalLines / targetCount) : maxLines;
            for (auto parent : parents)
            {
                const auto children = std::move(tree.partitions[parent.index - 1].children);
                PartitionId function;
                uint64_t functionOps = 0, functionLines = 0;
                for (auto child : children)
                {
                    const auto ops = partitionOps(tree, child);
                    uint64_t lines = 0;
                    for (auto op : ops) lines += estimatedLines(model, op);
                    if (!function || functionOps + ops.size() > effectiveOps || functionLines + lines > effectiveLines)
                    { function = addPartition(tree, parent, CpuPartitionKind::EmitFunction); functionOps = 0; functionLines = 0; }
                    attach(tree, function, child);
                    functionOps += ops.size(); functionLines += lines;
                }
            }
        }

        // Six-phase variant (cpu.st.pack-general-functions): helper chunks on
        // General supernodes (no ActiveWord layer — the six-phase model gates
        // whole supernodes), then emit-function batching on the General
        // branch only. The flat Event/Mem/Output branches each collapse into
        // a single emit function holding the branch's ops (single task).
        void packGeneralFunctions(const GrhSimModel &model, CpuBackendMapping &mapping,
                                  uint32_t helperLines, uint32_t maxOps, uint32_t maxLines, uint32_t targetCount)
        {
            auto &tree = mapping.partitionTree;
            const auto general = phasePartition(tree, CpuPhase::General);
            const auto supernodes = tree.partitions[general.index - 1].children;
            uint64_t totalOps = 0, totalLines = 0;
            for (auto id : supernodes)
            {
                auto &attrs = tree.partitions[id.index - 1].attrs;
                const auto ops = partitionOps(tree, id);
                totalOps += ops.size();
                uint64_t lines = 0;
                uint32_t begin = 0;
                for (uint32_t i = 0; i < ops.size(); ++i)
                {
                    const auto estimate = estimatedLines(model, ops[i]);
                    totalLines += estimate;
                    if (i > begin && lines + estimate > helperLines)
                    { attrs.helperChunks.push_back({begin, i - begin}); begin = i; lines = 0; }
                    lines += estimate;
                }
                if (!attrs.helperChunks.empty() || lines > helperLines)
                    attrs.helperChunks.push_back({begin, static_cast<uint32_t>(ops.size()) - begin});
            }
            const auto effectiveOps = targetCount ? std::max(uint64_t(maxOps), totalOps / targetCount) : maxOps;
            const auto effectiveLines = targetCount ? std::max(uint64_t(maxLines), totalLines / targetCount) : maxLines;
            const auto children = std::move(tree.partitions[general.index - 1].children);
            PartitionId function;
            uint64_t functionOps = 0, functionLines = 0;
            for (auto child : children)
            {
                const auto ops = partitionOps(tree, child);
                uint64_t lines = 0;
                for (auto op : ops) lines += estimatedLines(model, op);
                if (!function || functionOps + ops.size() > effectiveOps || functionLines + lines > effectiveLines)
                { function = addPartition(tree, general, CpuPartitionKind::EmitFunction); functionOps = 0; functionLines = 0; }
                attach(tree, function, child);
                functionOps += ops.size(); functionLines += lines;
            }
            for (auto branchPhase : {CpuPhase::Event, CpuPhase::Mem, CpuPhase::Output})
            {
                const auto branch = phasePartition(tree, branchPhase);
                auto ops = std::move(tree.partitions[branch.index - 1].ops);
                tree.partitions[branch.index - 1].ops = {};
                const auto id = addPartition(tree, branch, CpuPartitionKind::EmitFunction);
                tree.partitions[id.index - 1].ops = std::move(ops);
            }
        }

        // Deterministic execution order for the flat Event/Output branches:
        // Kahn topological order over branch-internal value edges, smallest op
        // id first; Event edgeDet ops (data-less detectors) always trail the
        // event cone so the branch evaluates the cone before detecting edges.
        std::vector<OpId> orderFlatPhaseOps(const GrhSimModel &model, SimPhase phase, bool detectorsLast)
        {
            std::vector<OpId> producer(model.values().size() + 1);
            std::vector<bool> inPhase(model.operations().size() + 1, false);
            for (const auto &op : model.operations())
            {
                if (op.phase == phase) inPhase[op.id.index] = true;
                for (auto value : model.results(op)) producer[value.index] = op.id;
            }
            std::vector<uint32_t> indegree(model.operations().size() + 1, 0);
            std::vector<std::vector<OpId>> users(model.operations().size() + 1);
            std::set<uint32_t> ready;
            std::vector<uint32_t> detectors;
            uint32_t cone = 0;
            for (const auto &op : model.operations())
            {
                if (op.phase != phase) continue;
                if (detectorsLast && model.text(op.opType) == "core.event.edgeDet")
                {
                    detectors.push_back(op.id.index);
                    continue;
                }
                ++cone;
                for (auto value : model.operands(op))
                {
                    const auto source = producer[value.index];
                    if (!source.index || !inPhase[source.index]) continue;
                    users[source.index].push_back(op.id);
                    ++indegree[op.id.index];
                }
                if (indegree[op.id.index] == 0) ready.insert(op.id.index);
            }
            std::vector<OpId> result;
            while (!ready.empty())
            {
                const auto id = *ready.begin();
                ready.erase(ready.begin());
                result.push_back(OpId{id, 0});
                for (auto user : users[id])
                    if (--indegree[user.index] == 0) ready.insert(user.index);
            }
            if (result.size() != cone)
                throw std::runtime_error("six-phase flat branch contains a dependency cycle");
            std::sort(detectors.begin(), detectors.end());
            for (auto id : detectors) result.push_back(OpId{id, 0});
            return result;
        }

        class PartitionPass final : public Pass
        {
        public:
            PartitionPass(std::string name, CpuMappingStage inputStage, std::vector<uint32_t> options)
                : Pass(std::move(name), PassKind::BackendMapping), inputStage_(inputStage), options_(std::move(options)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != inputStage_)
                { diagnostics.error("CPU partition pass prerequisites are not satisfied", name()); return {false, false, {}}; }
                CpuBackendMapping mapping = *previous;
                switch (inputStage_)
                {
                case CpuMappingStage::EventDomains: buildNodes(model, mapping, options_[0], diagnostics); break;
                case CpuMappingStage::ComputeNodes: mergeSupernodes(model, mapping, options_[0], diagnostics); break;
                case CpuMappingStage::ComputeSupernodes: packWords(model, mapping, options_[0]); break;
                case CpuMappingStage::ActiveWords: packFunctions(model, mapping, options_[0], options_[1], options_[2]); break;
                default: throw std::logic_error("invalid CPU partition pass stage");
                }
                mapping.stage = static_cast<CpuMappingStage>(static_cast<unsigned>(inputStage_) + 1);
                diagnostics.info("partitions=" + std::to_string(mapping.partitionTree.partitions.size()), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            CpuMappingStage inputStage_;
            std::vector<uint32_t> options_;
        };

        // cpu.st.split-phases: completes the M2 phase attribution (None mem
        // writes -> Mem, None output.write -> Output, every other None op ->
        // General), then builds the four-branch root: flat Event (cone topo
        // order, edgeDets last), empty General shell, flat Mem in op-id order
        // (static priority seed), flat Output in topo order.
        class SplitPhasesPass final : public Pass
        {
        public:
            SplitPhasesPass() : Pass("cpu.st.split-phases", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                uint64_t attributedMem = 0, attributedOutput = 0, attributedGeneral = 0;
                for (const auto &op : model.operations())
                {
                    if (op.phase != SimPhase::None) continue;
                    const auto type = model.text(op.opType);
                    SimPhase phase = SimPhase::General;
                    if (isMemWriteOp(type)) { phase = SimPhase::Mem; ++attributedMem; }
                    else if (type == "core.output.write") { phase = SimPhase::Output; ++attributedOutput; }
                    else ++attributedGeneral;
                    model.setOperationPhase(op.id, phase);
                }
                uint64_t generalOps = 0;
                for (const auto &op : model.operations())
                    if (op.phase == SimPhase::General) ++generalOps;
                CpuBackendMapping mapping;
                auto &tree = mapping.partitionTree;
                tree.root = addPartition(tree, {}, CpuPartitionKind::Root);
                const std::array<CpuPhase, 4> order{CpuPhase::Event, CpuPhase::General,
                                                    CpuPhase::Mem, CpuPhase::Output};
                std::vector<PartitionId> branches;
                for (auto branchPhase : order)
                {
                    const auto id = addPartition(tree, tree.root, CpuPartitionKind::Phase);
                    tree.partitions[id.index - 1].attrs.phase = branchPhase;
                    branches.push_back(id);
                }
                auto &event = tree.partitions[branches[0].index - 1];
                event.ops = orderFlatPhaseOps(model, SimPhase::Event, true);
                auto &mem = tree.partitions[branches[2].index - 1];
                for (const auto &op : model.operations())
                    if (op.phase == SimPhase::Mem) mem.ops.push_back(op.id);
                auto &output = tree.partitions[branches[3].index - 1];
                output.ops = orderFlatPhaseOps(model, SimPhase::Output, false);
                mapping.stage = CpuMappingStage::SplitPhases;
                diagnostics.info("event_ops=" + std::to_string(event.ops.size()) +
                                 " general_ops=" + std::to_string(generalOps) +
                                 " mem_ops=" + std::to_string(mem.ops.size()) +
                                 " output_ops=" + std::to_string(output.ops.size()) +
                                 " attributed_mem=" + std::to_string(attributedMem) +
                                 " attributed_output=" + std::to_string(attributedOutput) +
                                 " attributed_general=" + std::to_string(attributedGeneral), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };

        // Six-phase counterpart of PartitionPass: GeneralNodes /
        // GeneralSupernodes / GeneralFunctions stage transitions on the
        // four-branch tree produced by cpu.st.split-phases.
        class PhasePartitionPass final : public Pass
        {
        public:
            PhasePartitionPass(std::string name, CpuMappingStage inputStage, std::vector<uint32_t> options)
                : Pass(std::move(name), PassKind::BackendMapping), inputStage_(inputStage), options_(std::move(options)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != inputStage_)
                { diagnostics.error("CPU partition pass prerequisites are not satisfied", name()); return {false, false, {}}; }
                CpuBackendMapping mapping = *previous;
                switch (inputStage_)
                {
                case CpuMappingStage::SplitPhases: buildGeneralNodes(model, mapping, options_[0], diagnostics); break;
                case CpuMappingStage::GeneralNodes: mergeGeneralSupernodes(model, mapping, options_[0], diagnostics); break;
                case CpuMappingStage::GeneralSupernodes:
                    packGeneralFunctions(model, mapping, options_[0], options_[1], options_[2], options_[3]); break;
                default: throw std::logic_error("invalid CPU partition pass stage");
                }
                mapping.stage = static_cast<CpuMappingStage>(static_cast<unsigned>(inputStage_) + 1);
                diagnostics.info("partitions=" + std::to_string(mapping.partitionTree.partitions.size()), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            CpuMappingStage inputStage_;
            std::vector<uint32_t> options_;
        };
    }

    CpuEventDomainSets computeCpuEventDomainSets(const GrhSimModel &model)
    {
        const auto opCount = model.operations().size();
        CpuEventDomainSets sets;
        sets.acts.resize(opCount + 1);
        sets.influence.resize(opCount + 1);
        // The influence graph spans the General-phase ops plus the Mem-phase
        // write ops; Event/Output ops are unreachable by construction (their
        // cones are self-contained) and the timeslot tasks' acts do not gate.
        // Edges are value fanout only (General -> Mem write-operand edges
        // included, mem writes being sinks). State write -> read edges are
        // deliberately excluded: state readers consume S directly and are
        // activated by P_publish's stateFanout, not by eventActiveFlag, so a
        // write's domain must not leak into its state readers' domains.
        std::vector<bool> inGraph(opCount + 1, false);
        for (const auto &op : model.operations())
        {
            if (op.phase != SimPhase::General && op.phase != SimPhase::Mem) continue;
            inGraph[op.id.index] = true;
            sets.acts[op.id.index] = readEventActs(model, op);
        }
        std::vector<OpId> producer(model.values().size() + 1);
        std::vector<uint32_t> useOffsets(model.values().size() + 2, 0);
        for (const auto &op : model.operations())
        {
            for (auto value : model.results(op)) producer[value.index] = op.id;
            for (auto value : model.operands(op)) ++useOffsets[value.index + 1];
        }
        std::partial_sum(useOffsets.begin(), useOffsets.end(), useOffsets.begin());
        std::vector<OpId> uses(useOffsets.back());
        auto cursor = useOffsets;
        for (const auto &op : model.operations())
            for (auto value : model.operands(op)) uses[cursor[value.index]++] = op.id;
        std::vector<std::vector<OpId>> successors(opCount + 1);
        const auto link = [&](OpId from, OpId to) {
            if (!from.index || !to.index || from == to) return;
            if (!inGraph[from.index] || !inGraph[to.index]) return;
            auto &row = successors[from.index];
            if (std::find(row.begin(), row.end(), to) == row.end()) row.push_back(to);
        };
        for (const auto &op : model.operations())
        {
            if (!inGraph[op.id.index]) continue;
            for (auto value : model.results(op))
                for (uint32_t i = useOffsets[value.index]; i < useOffsets[value.index + 1]; ++i)
                    link(op.id, uses[i]);
        }
        std::vector<std::vector<OpId>> predecessors(opCount + 1);
        for (uint32_t i = 1; i <= opCount; ++i)
            for (auto target : successors[i]) predecessors[target.index].push_back(OpId{i, 0});
        for (uint32_t i = 1; i <= opCount; ++i) sets.influence[i] = sets.acts[i];
        // Worklist least fixed point: influence = acts U union(influence[succ]).
        // Monotone and bounded by the finite act universe, so it converges.
        std::vector<bool> queued(opCount + 1, false);
        std::vector<OpId> worklist;
        for (const auto &op : model.operations())
            if (inGraph[op.id.index]) { queued[op.id.index] = true; worklist.push_back(op.id); }
        for (std::size_t head = 0; head < worklist.size();)
        {
            const auto id = worklist[head++];
            queued[id.index] = false;
            auto merged = sets.acts[id.index];
            for (auto target : successors[id.index]) unionInto(merged, sets.influence[target.index]);
            if (merged == sets.influence[id.index]) continue;
            sets.influence[id.index] = std::move(merged);
            for (auto source : predecessors[id.index])
                if (!queued[source.index]) { queued[source.index] = true; worklist.push_back(source); }
        }
        return sets;
    }

    void registerCpuPartitionPasses(PassRegistry &registry)
    {
        const auto add = [&](std::string name, CpuMappingStage stage,
                             std::vector<std::string> keys, std::vector<uint32_t> defaults,
                             bool sixPhase = false) {
            std::string error;
            const auto factory = [name, stage, keys, defaults, sixPhase](std::span<const std::string_view> args,
                                                                         std::string &error) -> std::unique_ptr<Pass> {
                auto options = defaults;
                std::vector<bool> seen(keys.size());
                if (args.size() % 2 != 0) { error = "CPU partition options require a value"; return {}; }
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    const auto it = std::find(keys.begin(), keys.end(), args[i]);
                    if (it == keys.end()) { error = "unknown CPU partition option: " + std::string(args[i]); return {}; }
                    const auto index = it - keys.begin();
                    if (seen[index]) { error = "duplicate CPU partition option"; return {}; }
                    seen[index] = true;
                    const auto text = args[i + 1];
                    const auto result = std::from_chars(text.data(), text.data() + text.size(), options[index]);
                    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                        (options[index] == 0 && keys[index] != "--target-batch-count"))
                    { error = "CPU partition limit must be a positive 32-bit integer"; return {}; }
                }
                if (sixPhase) return std::make_unique<PhasePartitionPass>(name, stage, std::move(options));
                return std::make_unique<PartitionPass>(name, stage, std::move(options));
            };
            if (!registry.registerPass(name, PassKind::BackendMapping, factory, error)) throw std::logic_error(error);
        };
        add("cpu.st.build-compute-nodes", CpuMappingStage::EventDomains, {"--max-op-in-compute-node"}, {128});
        add("cpu.st.merge-compute-supernodes", CpuMappingStage::ComputeNodes, {"--max-op-in-compute-supernode"}, {128});
        add("cpu.st.pack-active-words", CpuMappingStage::ComputeSupernodes, {"--helper-max-estimated-lines"}, {2048});
        add("cpu.st.pack-emit-functions", CpuMappingStage::ActiveWords,
            {"--batch-max-ops", "--batch-max-estimated-lines", "--target-batch-count"}, {2048, 8192, 64});
        add("cpu.st.build-general-nodes", CpuMappingStage::SplitPhases, {"--max-op-in-compute-node"}, {128}, true);
        add("cpu.st.merge-general-supernodes", CpuMappingStage::GeneralNodes, {"--max-op-in-compute-supernode"}, {128}, true);
        add("cpu.st.pack-general-functions", CpuMappingStage::GeneralSupernodes,
            {"--helper-max-estimated-lines", "--batch-max-ops", "--batch-max-estimated-lines", "--target-batch-count"},
            {2048, 2048, 8192, 64}, true);
        std::string error;
        if (!registry.registerPass("cpu.st.split-phases", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.split-phases does not accept arguments"; return {}; }
                return std::make_unique<SplitPhasesPass>();
            }, error)) throw std::logic_error(error);
    }
}
