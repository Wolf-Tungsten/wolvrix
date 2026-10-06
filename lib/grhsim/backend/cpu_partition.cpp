#include "grhsim/backend/cpu.hpp"
#include "grhsim/backend/cpu_phase_common.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
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
            uint64_t anchoredValues = 0;
        };

        // Reverse-topological cone absorption. Legacy mode stops at shared
        // values, commit boundaries and the maxOps node size cap. Semantic
        // mode (S1, plan 20261005-170704): every value targeted by a
        // DeclProvenance Value slice (a source-level declared signal) anchors
        // its own node — the producer op is never absorbed downstream — and
        // the maxOps cap is dropped, so node shape follows declaration
        // boundaries instead of dataflow shape; the optional semanticMaxOps
        // cap (0 = none) can still bound cone growth for compile-time
        // control. Unanchored ops keep the legacy single-consumer cone
        // absorption: values outside provenance coverage fall back to the old
        // rule. No source cloning either way: every op remains owned by
        // exactly one mapping leaf.
        NodeStats formNodes(const GrhSimModel &model, CpuPartitionTree &tree, PartitionId phase,
                            std::vector<OpId> ops, uint32_t maxOps, bool semantic, uint32_t semanticMaxOps)
        {
            ComputeGraph graph(model, ops);
            std::vector<bool> anchored;
            uint64_t anchoredValues = 0;
            if (semantic)
            {
                anchored.assign(model.values().size() + 1, false);
                for (const auto &record : model.declProvenances())
                    for (const auto &slice : record.slices)
                    {
                        if (slice.target != DeclProvenanceTarget::Value) continue;
                        if (slice.targetIndex == 0 || slice.targetIndex >= anchored.size()) continue;
                        if (!anchored[slice.targetIndex]) { anchored[slice.targetIndex] = true; ++anchoredValues; }
                    }
            }
            std::vector<uint32_t> owner(model.operations().size() + 1, absent), sizes;
            for (auto it = graph.topo.rbegin(); it != graph.topo.rend(); ++it)
            {
                const auto &op = model.operations()[it->index - 1];
                const auto type = model.text(op.opType);
                bool absorb = type.starts_with("core.compute.") || type == "core.input.read" ||
                              type == "core.state.read" || type == "core.state.memRead";
                if (semantic)
                    for (auto value : model.results(op))
                        if (anchored[value.index]) { absorb = false; break; }
                uint32_t target = absent;
                for (auto value : model.results(op))
                    for (uint32_t i = graph.useOffsets[value.index]; i < graph.useOffsets[value.index + 1]; ++i)
                    {
                        const auto user = graph.uses[i];
                        if (!graph.compute[user.index]) { absorb = false; continue; }
                        if (target == absent) target = owner[user.index];
                        else if (target != owner[user.index]) absorb = false;
                    }
                if (!absorb || target == absent || (!semantic && sizes[target] >= maxOps) ||
                    (semantic && semanticMaxOps && sizes[target] >= semanticMaxOps))
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
            return {sizes.size(), edges.size(), anchoredValues};
        }

        uint64_t clusterSize(const std::vector<uint32_t> &cluster, std::span<const uint32_t> sizes)
        {
            uint64_t result = 0;
            for (auto node : cluster) result += sizes[node];
            return result;
        }

        // Six-phase coarsen over the non-sink node graph (V2-M1: sink ops are
        // clustered by event signature outside this framework, and the
        // merge-time event-domain prohibition is gone — a non-sink supernode
        // fires data-driven, with per-op eventActStore guards inside its
        // body, so mixed act sets are legal here).
        bool coarsenGeneral(Clusters &clusters, std::span<const Edge> edges, std::span<const uint32_t> sizes,
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

        // Six-phase DP segmentation over the non-sink clusters (V2-M1: no
        // event-domain span check — see coarsenGeneral). valueRate, when
        // nonempty, replaces the uniform per-value weight with measured
        // firing rates (profile-guided boundary cost; rate 0 falls back to
        // uniform weight 1 for unprofiled values).
        Clusters segmentGeneral(const Clusters &clusters, const ClusterGraph &graph,
                                std::span<const uint32_t> sizes, std::size_t valueCount, uint32_t maxOps,
                                uint32_t segmentPenalty, std::span<const uint64_t> valueRate)
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
            // Legacy's uniform-weight objective: distinct incoming activation
            // values + segmentPenalty per segment (default 1; larger values
            // bias toward fewer, larger segments). With a firing profile the
            // per-value term becomes the measured producer firing rate, so
            // hot boundaries price out of segment splits.
            const auto weightOf = [&](uint32_t value) -> uint64_t {
                if (valueRate.empty()) return 1;
                const auto rate = value < valueRate.size() ? valueRate[value] : 0;
                return rate ? rate : 1;
            };
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
                        if (sourceOfValue[value] < begin) { counted[value] = end; incoming += weightOf(value); }
                    }
                    for (auto value : sources[begin])
                        if (counted[value] == end) { counted[value] = 0; incoming -= weightOf(value); }
                    const auto candidate = cost[begin] + incoming + segmentPenalty;
                    if (candidate <= cost[end]) { cost[end] = candidate; previous[end] = begin; }
                }
                if (cost[end] == infinity) { cost[end] = cost[end - 1] + segmentPenalty; previous[end] = end - 1; }
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

        // Optional firing-rate profile for the DP segmentation (E3 boundary
        // exploration): env WOLVRIX_GRHSIM_FIRE_PROFILE points to a TSV of
        // "opIndex<TAB>fireCount" rows (op indices are the model's 1-based
        // OpId indexes, e.g. produced from an instrumented emu's per-supernode
        // firing dump). Empty/absent file keeps the uniform objective.
        std::vector<uint64_t> loadFireProfile(const GrhSimModel &model, std::size_t opCount,
                                              diag::Diagnostics &diagnostics)
        {
            const char *path = std::getenv("WOLVRIX_GRHSIM_FIRE_PROFILE");
            if (!path || !*path) return {};
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error(std::string("cannot open firing profile: ") + path);
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            std::vector<uint64_t> fire(opCount + 1, 0);
            const char *cursor = text.data(), *end = cursor + text.size();
            uint64_t rows = 0;
            while (cursor < end)
            {
                uint32_t op = 0;
                auto first = std::from_chars(cursor, end, op);
                if (first.ec != std::errc{}) break;
                cursor = first.ptr;
                if (cursor < end && (*cursor == '\t' || *cursor == ' ')) ++cursor;
                uint64_t count = 0;
                auto second = std::from_chars(cursor, end, count);
                if (second.ec != std::errc{}) break;
                cursor = second.ptr;
                while (cursor < end && *cursor != '\n') ++cursor;
                if (cursor < end) ++cursor;
                if (op >= 1 && op <= opCount) { fire[op] = count; ++rows; }
            }
            diagnostics.info("fire_profile_rows=" + std::to_string(rows), "cpu.st.merge-general-supernodes");
            return fire;
        }

        // cpu.st.merge-general-supernodes (C2, V2-M1): the General branch's
        // nodes split into non-sink nodes (value-producing ops) and sink
        // nodes (no-result ops, one op each from C1). Non-sink nodes go
        // through the coarsen+DP frame with no event-domain prohibition — a
        // non-sink supernode fires data-driven and its event-carrying ops
        // self-guard on eventActStore inside the body. Sink nodes never
        // enter the frame: they cluster by canonical event signature (the
        // op's sorted event_acts set), one supernode per signature — the
        // empty signature forms the escape class (SinkEscape, fires every
        // round), nonempty signatures form SinkEvent supernodes. A1: a
        // nonempty signature cluster is further subdivided by shared write
        // enable — singleton regWrite nodes with a non-constant en group by
        // en value id, and a group reaching --sink-enable-guard-min-size
        // (default 8, 0 disables) becomes its own SinkEvent supernode with
        // attrs.enableGuard set (the call site ANDs one enable read into
        // the signature gate); the rest keeps the plain per-signature
        // supernode. Every
        // supernode records attrs.eventActs (the act union; for sinks that IS
        // the signature) and attrs.supernodeCategory. Child order: non-sink
        // supernodes first (frame/topological order), sink supernodes after
        // (lexicographic signature order) — sink operands are produced by
        // non-sink ops only, so every sink boundary producer has a smaller
        // ordinal (a verifyCpuMapping invariant). The child order IS the
        // final supernode ordinal space consumed by the layout, event
        // bitmaps, schedule and emitter (M5d-6, resolution 2).
        void mergeGeneralSupernodes(const GrhSimModel &model, CpuBackendMapping &mapping, uint32_t maxOps,
                                    uint32_t guardMinSize, uint32_t segmentPenalty, uint32_t coarsenMaxOps,
                                    diag::Diagnostics &diagnostics)
        {
            // S2 (plan 20261005-170704): coarsen merge weight cap. 0 follows
            // maxOps (legacy); a large value effectively lifts the cap so
            // chain/sibling absorption is limited by structure, not size
            // (the DP window below still caps segment size; oversized
            // clusters become singleton segments).
            const uint32_t coarsenCap = coarsenMaxOps ? coarsenMaxOps : maxOps;
            auto &tree = mapping.partitionTree;
            const auto phase = phasePartition(tree, CpuPhase::General);
            const auto nodes = tree.partitions[phase.index - 1].children;
            std::vector<uint32_t> nonSinkNodes, sinkNodes;
            for (uint32_t i = 0; i < nodes.size(); ++i)
            {
                const auto &partition = tree.partitions[nodes[i].index - 1];
                bool sink = true;
                for (auto op : partition.ops)
                    if (!model.results(model.operations()[op.index - 1]).empty()) { sink = false; break; }
                if (!sink)
                    for (auto op : partition.ops)
                        if (model.results(model.operations()[op.index - 1]).empty())
                            throw std::runtime_error("general node mixes sink and non-sink ops");
                (sink ? sinkNodes : nonSinkNodes).push_back(i);
            }
            std::vector<OpId> computeOps;
            for (const auto index : nonSinkNodes)
            {
                const auto &ops = tree.partitions[nodes[index].index - 1].ops;
                computeOps.insert(computeOps.end(), ops.begin(), ops.end());
            }
            ComputeGraph graph(model, computeOps);
            std::vector<uint32_t> owner(model.operations().size() + 1, absent), sizes;
            Clusters clusters(nonSinkNodes.size());
            for (uint32_t i = 0; i < nonSinkNodes.size(); ++i)
            {
                sizes.push_back(tree.partitions[nodes[nonSinkNodes[i]].index - 1].ops.size());
                clusters[i].push_back(i);
                for (auto op : tree.partitions[nodes[nonSinkNodes[i]].index - 1].ops) owner[op.index] = i;
            }
            const auto edges = nodeEdges(model, graph, owner);
            unsigned tail = 0, iterations = 0;
            while (!clusters.empty())
            {
                const auto before = clusters.size();
                for (unsigned mode = 0; mode < 3; ++mode)
                    coarsenGeneral(clusters, edges, sizes, coarsenCap, mode);
                ++iterations;
                if (clusters.size() == before) break;
                tail = before >= 100000 && before - clusters.size() < 1024 ? tail + 1 : 0;
                if (tail == 3) break;
            }
            const auto coarsened = clusters.size();
            const auto opFire = loadFireProfile(model, model.operations().size(), diagnostics);
            std::vector<uint64_t> valueRate;
            if (!opFire.empty())
            {
                valueRate.assign(model.values().size() + 1, 0);
                for (uint32_t value = 1; value < valueRate.size(); ++value)
                    valueRate[value] = opFire[graph.producer[value].index];
            }
            clusters = segmentGeneral(clusters, ClusterGraph(clusters, edges, sizes.size()), sizes,
                                      model.values().size(), maxOps, segmentPenalty, valueRate);
            tree.partitions[phase.index - 1].children.clear();
            uint64_t nonSinkSupernodes = 0;
            for (const auto &cluster : clusters)
            {
                const auto supernode = addPartition(tree, phase, CpuPartitionKind::Supernode);
                auto &attrs = tree.partitions[supernode.index - 1].attrs;
                std::vector<int64_t> acts;
                for (auto node : cluster)
                    for (auto op : tree.partitions[nodes[nonSinkNodes[node]].index - 1].ops)
                        unionInto(acts, readEventActs(model, model.operations()[op.index - 1]));
                attrs.eventActs = std::move(acts);
                attrs.supernodeCategory = CpuSupernodeCategory::NonSink;
                for (auto node : cluster) attach(tree, supernode, nodes[nonSinkNodes[node]]);
                ++nonSinkSupernodes;
            }
            // Sink clustering: one supernode per canonical event signature.
            // A sink node must carry a single signature across its ops (C1
            // forms singleton sink nodes, so this is structural).
            std::map<std::vector<int64_t>, std::vector<uint32_t>> signatureGroups;
            uint64_t sinkOps = 0;
            for (const auto index : sinkNodes)
            {
                const auto &ops = tree.partitions[nodes[index].index - 1].ops;
                std::vector<int64_t> signature;
                bool firstOp = true;
                for (auto op : ops)
                {
                    auto acts = readEventActs(model, model.operations()[op.index - 1]);
                    if (firstOp) { signature = std::move(acts); firstOp = false; continue; }
                    if (acts != signature)
                        throw std::runtime_error("sink node carries mixed event signatures");
                }
                signatureGroups[signature].push_back(index);
                sinkOps += ops.size();
            }
            uint64_t escapeSupernodes = 0, eventSupernodes = 0, guardSupernodes = 0, guardOps = 0;
            for (const auto &[signature, members] : signatureGroups)
            {
                // A1: inside a nonempty signature, subdivide by shared write
                // enable. A singleton regWrite node whose en (operands[0])
                // producer is not a constant joins the group of its en value
                // id; a group of at least guardMinSize nodes becomes its own
                // SinkEvent supernode carrying attrs.enableGuard (the call
                // site ANDs one read of the enable into the signature gate).
                // Everything else — non-regWrite sinks, constant enables,
                // small groups — keeps the previous behavior and lands in
                // the signature's ordinary supernode. The escape class
                // (empty signature) is never subdivided. guardMinSize == 0
                // disables the subdivision entirely.
                std::map<uint32_t, std::vector<uint32_t>> enableGroups;
                std::vector<uint32_t> ordinary;
                if (!signature.empty() && guardMinSize > 0)
                {
                    for (const auto index : members)
                    {
                        const auto &ops = tree.partitions[nodes[index].index - 1].ops;
                        if (ops.size() != 1) { ordinary.push_back(index); continue; }
                        const auto &op = model.operations()[ops.front().index - 1];
                        const auto operands = model.operands(op);
                        if (model.text(op.opType) != "core.state.regWrite" || operands.size() != 3)
                        { ordinary.push_back(index); continue; }
                        const auto producer = graph.producer[operands[0].index];
                        if (!producer ||
                            model.text(model.operations()[producer.index - 1].opType) == "core.compute.constant")
                        { ordinary.push_back(index); continue; }
                        enableGroups[operands[0].index].push_back(index);
                    }
                }
                else
                    ordinary = members;
                for (const auto &[enable, group] : enableGroups)
                {
                    if (group.size() < guardMinSize)
                    {
                        ordinary.insert(ordinary.end(), group.begin(), group.end());
                        continue;
                    }
                    const auto supernode = addPartition(tree, phase, CpuPartitionKind::Supernode);
                    auto &attrs = tree.partitions[supernode.index - 1].attrs;
                    attrs.eventActs = signature;
                    attrs.supernodeCategory = CpuSupernodeCategory::SinkEvent;
                    attrs.enableGuard = static_cast<int64_t>(enable);
                    for (auto node : group) attach(tree, supernode, nodes[node]);
                    ++eventSupernodes; ++guardSupernodes; guardOps += group.size();
                }
                if (ordinary.empty()) continue;
                const auto supernode = addPartition(tree, phase, CpuPartitionKind::Supernode);
                auto &attrs = tree.partitions[supernode.index - 1].attrs;
                attrs.eventActs = signature;
                attrs.supernodeCategory = signature.empty() ? CpuSupernodeCategory::SinkEscape
                                                            : CpuSupernodeCategory::SinkEvent;
                for (auto node : ordinary) attach(tree, supernode, nodes[node]);
                if (signature.empty()) ++escapeSupernodes; else ++eventSupernodes;
            }
            const auto finalEdges = ClusterGraph(clusters, edges, sizes.size()).edges.size();
            diagnostics.info("coarsen_iterations=" + std::to_string(iterations) + " coarsen_cap=" + std::to_string(coarsenCap) +
                             " coarsened_clusters=" + std::to_string(coarsened) +
                             " nonsink_supernodes=" + std::to_string(nonSinkSupernodes) +
                             " sink_supernodes=" + std::to_string(signatureGroups.size()) +
                             " sink_escape_supernodes=" + std::to_string(escapeSupernodes) +
                             " sink_event_supernodes=" + std::to_string(eventSupernodes) +
                             " sink_guard_supernodes=" + std::to_string(guardSupernodes) +
                             " sink_guard_ops=" + std::to_string(guardOps) +
                             " sink_ops=" + std::to_string(sinkOps) +
                             " boundary_value_targets=" + std::to_string(finalEdges), "cpu.st.merge-general-supernodes");
        }

        // cpu.st.pack-general-functions (C6, M5d-6 position: after
        // build-mem-write-plan, before build-phase-schedule): helper chunks
        // on General supernodes (the six-phase model gates whole
        // supernodes), then emit-function batching. Resolution 2: the
        // supernodes stay direct General-branch children in C2 ordinal
        // order; each EmitFunction is a leaf appended after them that only
        // records the contiguous supernode ordinal interval it holds
        // (attrs.supernodeRange). The flat Event/Mem/Output branches each
        // collapse into a single emit function holding the branch's ops
        // (single task).
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
                    const auto estimate = estimatedCpuOpLines(model, ops[i]);
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
            PartitionId function;
            uint64_t functionOps = 0, functionLines = 0;
            uint32_t rangeBegin = 0;
            const auto closeFunction = [&](uint32_t ordinal) {
                if (function)
                    tree.partitions[function.index - 1].attrs.supernodeRange = Range{rangeBegin, ordinal - rangeBegin};
            };
            for (uint32_t ordinal = 0; ordinal < supernodes.size(); ++ordinal)
            {
                const auto ops = partitionOps(tree, supernodes[ordinal]);
                uint64_t lines = 0;
                for (auto op : ops) lines += estimatedCpuOpLines(model, op);
                if (!function || functionOps + ops.size() > effectiveOps || functionLines + lines > effectiveLines)
                {
                    closeFunction(ordinal);
                    function = addPartition(tree, general, CpuPartitionKind::EmitFunction);
                    rangeBegin = ordinal;
                    functionOps = 0;
                    functionLines = 0;
                }
                functionOps += ops.size();
                functionLines += lines;
            }
            closeFunction(static_cast<uint32_t>(supernodes.size()));
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

        // cpu.st.build-general-nodes (C1, V2-M1): initializes the one final
        // CPU mapping from the sealed semantic model. The semantic pipeline
        // (B5 grhsim.split-phases + B8 seal) has completed the class-aware
        // phase attribution, so this pass only consumes SimPhase: it builds
        // the four-branch root (flat Event in cone topo order with edgeDets
        // last, flat Mem in op-id order — the static priority seed, flat
        // Output in topo order), then forms General nodes over the NON-SINK
        // General-phase ops only. Sink ops (no results: reg/latch writes,
        // General-phase system.task/dpi.call, General-phase regLatch-class
        // mem writes) never enter node formation — each anchors a singleton
        // node appended in op-id order after the non-sink nodes, and C2
        // clusters them into sink supernodes by event signature. Their
        // operand cones stay in the non-sink framework (their values become
        // boundary values sampled by the sink supernodes). Any previous
        // mapping is discarded: this is the pipeline's single mapping
        // initialization point.
        class BuildGeneralNodesPass final : public Pass
        {
        public:
            explicit BuildGeneralNodesPass(uint32_t maxOps, bool semantic, uint32_t semanticMaxOps)
                : Pass("cpu.st.build-general-nodes", PassKind::BackendMapping), maxOps_(maxOps),
                  semantic_(semantic), semanticMaxOps_(semanticMaxOps) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                for (const auto &op : model.operations())
                    if (op.phase == SimPhase::None)
                    {
                        diagnostics.error("cpu.st.build-general-nodes requires total phase attribution "
                                          "(run grhsim.split-phases first)", name());
                        return {false, false, {}};
                    }
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
                std::vector<OpId> generalOps, sinkOps;
                for (const auto &op : model.operations())
                    if (op.phase == SimPhase::General)
                        (model.results(op).empty() ? sinkOps : generalOps).push_back(op.id);
                const auto stats = formNodes(model, tree, branches[1], std::move(generalOps), maxOps_, semantic_, semanticMaxOps_);
                uint64_t sinkNodes = 0;
                for (const auto opId : sinkOps)
                {
                    const auto id = addPartition(tree, branches[1], CpuPartitionKind::Node);
                    tree.partitions[id.index - 1].ops = {opId};
                    ++sinkNodes;
                }
                mapping.stage = CpuMappingStage::GeneralNodes;
                // formNodes grows the partition table, so the branch reads go
                // through fresh id lookups (no dangling references).
                diagnostics.info("event_ops=" + std::to_string(tree.partitions[branches[0].index - 1].ops.size()) +
                                 " general_nodes=" + std::to_string(stats.nodes) +
                                 " anchored_values=" + std::to_string(stats.anchoredValues) +
                                 " semantic_nodes=" + (semantic_ ? "1" : "0") +
                                 " sink_nodes=" + std::to_string(sinkNodes) +
                                 " mem_ops=" + std::to_string(tree.partitions[branches[2].index - 1].ops.size()) +
                                 " output_ops=" + std::to_string(tree.partitions[branches[3].index - 1].ops.size()) +
                                 " boundary_value_targets=" + std::to_string(stats.boundaryEdges), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            uint32_t maxOps_;
            bool semantic_;
            uint32_t semanticMaxOps_;
        };

        // C2 (cpu.st.merge-general-supernodes, GeneralNodes ->
        // GeneralSupernodes) and C6 (cpu.st.pack-general-functions,
        // MemWritePlan -> GeneralFunctions): stage transitions on the
        // four-branch tree produced by C1. The output stage is explicit —
        // the M5d-6 pipeline order is not the enum's numeric order.
        class PhasePartitionPass final : public Pass
        {
        public:
            PhasePartitionPass(std::string name, CpuMappingStage inputStage, CpuMappingStage outputStage,
                               std::vector<uint32_t> options)
                : Pass(std::move(name), PassKind::BackendMapping), inputStage_(inputStage),
                  outputStage_(outputStage), options_(std::move(options)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != inputStage_)
                { diagnostics.error("CPU partition pass prerequisites are not satisfied", name()); return {false, false, {}}; }
                CpuBackendMapping mapping = *previous;
                switch (inputStage_)
                {
                case CpuMappingStage::GeneralNodes: mergeGeneralSupernodes(model, mapping, options_[0], options_[1], options_[2], options_[3], diagnostics); break;
                case CpuMappingStage::MemWritePlan:
                    packGeneralFunctions(model, mapping, options_[0], options_[1], options_[2], options_[3]); break;
                default: throw std::logic_error("invalid CPU partition pass stage");
                }
                mapping.stage = outputStage_;
                diagnostics.info("partitions=" + std::to_string(mapping.partitionTree.partitions.size()), name());
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            CpuMappingStage inputStage_;
            CpuMappingStage outputStage_;
            std::vector<uint32_t> options_;
        };
    }

    void registerCpuPartitionPasses(PassRegistry &registry)
    {
        // The M5d-6 C-segment partition passes: C1 build-general-nodes
        // (initializes the mapping), C2 merge-general-supernodes, C6
        // pack-general-functions. The legacy two-phase passes and the
        // cpu.st.split-phases initializer were removed in M5d-6.
        const auto add = [&](std::string name, std::optional<CpuMappingStage> inputStage,
                             CpuMappingStage outputStage,
                             std::vector<std::string> keys, std::vector<uint32_t> defaults) {
            std::string error;
            const auto factory = [name, inputStage, outputStage, keys, defaults](std::span<const std::string_view> args,
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
                    // Zero disables the sink enable-guard subdivision; every
                    // other limit must be positive (batch count excepted: it
                    // means "auto"; semantic-nodes is a 0/1 flag; semantic-node-max-op
                    // and coarsen-max-op zero mean uncapped/follow the supernode cap).
                    const bool zeroAllowed = keys[index] == "--target-batch-count" ||
                                             keys[index] == "--sink-enable-guard-min-size" ||
                                             keys[index] == "--segment-penalty" ||
                                             keys[index] == "--semantic-nodes" ||
                                             keys[index] == "--semantic-node-max-op" ||
                                             keys[index] == "--coarsen-max-op";
                    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                        (options[index] == 0 && !zeroAllowed))
                    { error = "CPU partition limit must be a positive 32-bit integer"; return {}; }
                }
                if (!inputStage) return std::unique_ptr<Pass>(std::make_unique<BuildGeneralNodesPass>(options[0],
                                                                                                      options[1] != 0,
                                                                                                      options[2]));
                return std::unique_ptr<Pass>(std::make_unique<PhasePartitionPass>(name, *inputStage, outputStage,
                                                                                  std::move(options)));
            };
            if (!registry.registerPass(name, PassKind::BackendMapping, factory, error)) throw std::logic_error(error);
        };
        add("cpu.st.build-general-nodes", std::nullopt, CpuMappingStage::GeneralNodes,
            {"--max-op-in-compute-node", "--semantic-nodes", "--semantic-node-max-op"}, {128, 0, 0});
        add("cpu.st.merge-general-supernodes", CpuMappingStage::GeneralNodes, CpuMappingStage::GeneralSupernodes,
            {"--max-op-in-compute-supernode", "--sink-enable-guard-min-size", "--segment-penalty", "--coarsen-max-op"},
            {128, 8, 1, 0});
        add("cpu.st.pack-general-functions", CpuMappingStage::MemWritePlan, CpuMappingStage::GeneralFunctions,
            {"--helper-max-estimated-lines", "--batch-max-ops", "--batch-max-estimated-lines", "--target-batch-count"},
            {2048, 2048, 8192, 64});
    }
}
