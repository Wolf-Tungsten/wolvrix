#include "transform/grhsim/partition-nonsink.hpp"

#include "partition-internal.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;
using Clusters = std::vector<std::vector<uint32_t>>;
constexpr uint32_t absent = UINT32_MAX;

uint64_t valueBytes(const Graph& graph, ValueId value) {
    if (graph.valueType(value) != ValueType::Logic) return 8;
    const auto width = uint64_t(graph.valueWidth(value));
    return std::max<uint64_t>(1, (width + 7) / 8) *
        (graph.valueDomain(value) == LogicDomain::FourState ? 2 : 1);
}

uint64_t multiplyCost(uint64_t left, uint64_t right) {
    if (right && left > UINT64_MAX / right) throw std::invalid_argument("Activity partition cost overflows uint64");
    return left * right;
}

uint64_t addCost(uint64_t left, uint64_t right) {
    if (right > UINT64_MAX - left) throw std::invalid_argument("Activity partition cost overflows uint64");
    return left + right;
}

struct Edge {
    uint32_t source, target;
    ValueId value;
    uint64_t rate = 1, bytes = 0;
};

uint64_t distinctPairs(std::vector<uint64_t>& pairs) {
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs.size();
}

uint64_t pairKey(uint32_t source, uint32_t target) {
    return (uint64_t(source) << 32) | target;
}

uint64_t work(const Graph& graph, OperationId op) {
    uint64_t words = 1;
    for (const auto value : graph.opResults(op))
        if (graph.valueType(value) == ValueType::Logic) words = std::max(words, (uint64_t(graph.valueWidth(value)) + 63) / 64);
    const auto kind = graph.opKind(op);
    if (kind == OperationKind::kMul || kind == OperationKind::kDiv || kind == OperationKind::kMod) words *= 4;
    if (kind == kDpiCall || (kind == kSystemFunction && operationEffect(graph, op) != OpEffect::Pure)) words *= 8;
    return words;
}

struct Quotient {
    std::vector<Edge> edges;
    std::vector<std::vector<uint32_t>> predecessors, successors;

    Quotient(const Clusters& clusters, std::span<const Edge> input, std::size_t nodes)
        : predecessors(clusters.size()), successors(clusters.size()) {
        std::vector<uint32_t> owner(nodes);
        for (uint32_t i = 0; i < clusters.size(); ++i) for (const auto node : clusters[i]) owner[node] = i;
        edges.reserve(input.size());
        for (const auto& edge : input) {
            const auto source = edge.source == absent ? absent : owner[edge.source], target = owner[edge.target];
            if (source != target) edges.push_back({source, target, edge.value, edge.rate, edge.bytes});
        }
        const auto key = [](const Edge& edge) { return std::tuple{edge.source, edge.target, edge.value.index}; };
        std::sort(edges.begin(), edges.end(), [&](const auto& left, const auto& right) { return key(left) < key(right); });
        edges.erase(std::unique(edges.begin(), edges.end(), [&](const auto& left, const auto& right) {
            return key(left) == key(right);
        }), edges.end());
        uint32_t previousSource = absent, previousTarget = absent;
        for (const auto& edge : edges) if (edge.source != absent) {
            if (edge.source != previousSource || edge.target != previousTarget) {
                successors[edge.source].push_back(edge.target);
                predecessors[edge.target].push_back(edge.source);
                previousSource = edge.source; previousTarget = edge.target;
            }
        }
        // Edges visit each (source, target) pair once in source order, so
        // every predecessor row is already strictly increasing.
    }

    std::vector<uint32_t> order() const {
        std::vector<std::size_t> degree(predecessors.size());
        std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
        for (uint32_t i = 0; i < degree.size(); ++i) {
            degree[i] = predecessors[i].size();
            if (degree[i] == 0) ready.push(i);
        }
        std::vector<uint32_t> result;
        while (!ready.empty()) {
            const auto node = ready.top(); ready.pop(); result.push_back(node);
            for (const auto target : successors[node]) if (--degree[target] == 0) ready.push(target);
        }
        return result;
    }
};

bool orderClusters(Clusters& clusters, std::span<const Edge> edges, std::size_t nodes) {
    const auto order = Quotient(clusters, edges, nodes).order();
    if (order.size() != clusters.size()) return false;
    Clusters sorted;
    for (const auto cluster : order) sorted.push_back(std::move(clusters[cluster]));
    clusters = std::move(sorted);
    return true;
}

struct PredecessorHash {
    std::size_t operator()(std::span<const uint32_t> values) const noexcept {
        std::size_t hash = values.size();
        for (const auto value : values)
            hash ^= std::hash<uint32_t>{}(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct PredecessorEqual {
    bool operator()(std::span<const uint32_t> left, std::span<const uint32_t> right) const noexcept {
        return std::equal(left.begin(), left.end(), right.begin(), right.end());
    }
};

bool coarsen(Clusters& clusters, std::span<const Edge> edges, std::size_t nodeCount,
             std::span<const uint8_t> clockDomains, std::span<const uint32_t> capacities,
             uint64_t limit, unsigned mode,
             bool phaseTiming, std::unique_ptr<Quotient>& quotient) {
    auto previousPhase = phaseTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::array<uint64_t, 4> phaseMilliseconds{};
    const auto phase = [&](std::size_t index) {
        if (!phaseTiming) return;
        const auto now = std::chrono::steady_clock::now();
        phaseMilliseconds[index] = std::chrono::duration_cast<std::chrono::milliseconds>(now - previousPhase).count();
        previousPhase = now;
    };
    const auto before = clusters.size();
    const bool quotientCacheHit = bool(quotient);
    std::size_t candidateCount = 0, quotientEdges = 0;
    const auto record = [&](bool changed, bool accepted) {
        if (!phaseTiming) return;
        std::fprintf(stderr, "[grhsim-coarsen-phase] mode=%u clusters_before=%zu clusters_after=%zu quotient_edges=%zu candidates=%zu quotient_cache_hit=%u quotient_ms=%llu candidates_ms=%llu merge_ms=%llu order_ms=%llu changed=%u accepted=%u\n",
            mode, before, clusters.size(), quotientEdges, candidateCount,
            unsigned(quotientCacheHit),
            static_cast<unsigned long long>(phaseMilliseconds[0]), static_cast<unsigned long long>(phaseMilliseconds[1]),
            static_cast<unsigned long long>(phaseMilliseconds[2]), static_cast<unsigned long long>(phaseMilliseconds[3]),
            unsigned(changed), unsigned(accepted));
    };
    if (!quotient) quotient = std::make_unique<Quotient>(clusters, edges, nodeCount);
    const auto& graph = *quotient;
    quotientEdges = graph.edges.size();
    phase(0);
    std::vector<uint32_t> parents(clusters.size());
    std::iota(parents.begin(), parents.end(), 0);
    std::vector<uint64_t> weights(clusters.size());
    std::vector<uint8_t> domains(clusters.size());
    std::array<std::size_t, 2> domainCounts{};
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        for (const auto node : clusters[i]) weights[i] += capacities[node];
        domains[i] = clockDomains[clusters[i].front()];
        ++domainCounts[domains[i]];
    }
    const auto find = [&](uint32_t node) {
        while (parents[node] != node) { parents[node] = parents[parents[node]]; node = parents[node]; }
        return node;
    };
    bool changed = false;
    const auto merge = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a == b || domains[a] != domains[b] || weights[a] + weights[b] > limit) return false;
        if (a > b) std::swap(a, b);
        parents[b] = a; weights[a] += weights[b]; changed = true;
        return true;
    };
    std::vector<uint32_t> predecessorGroups;
    std::vector<uint32_t> anchors;
    std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> candidates;
    if (mode == 2) {
        // These spans borrow immutable rows of graph, which outlives both
        // grouping and merging. Hash-table iteration never determines order.
        std::array<std::unordered_map<std::span<const uint32_t>, uint32_t, PredecessorHash, PredecessorEqual>, 2> groups;
        for (std::size_t domain = 0; domain < groups.size(); ++domain) groups[domain].reserve(domainCounts[domain]);
        predecessorGroups.resize(clusters.size(), absent);
        for (uint32_t i = 0; i < clusters.size(); ++i) if (!graph.predecessors[i].empty()) {
            const auto [it, inserted] = groups[domains[i]].emplace(std::span<const uint32_t>(graph.predecessors[i]), anchors.size());
            if (inserted) anchors.push_back(i);
            else ++candidateCount;
            predecessorGroups[i] = it->second;
        }
    } else {
        for (std::size_t begin = 0; begin < graph.edges.size();) {
            const auto& first = graph.edges[begin];
            auto end = begin + 1;
            while (end < graph.edges.size() && graph.edges[end].source == first.source && graph.edges[end].target == first.target) ++end;
            if (first.source != absent &&
                (mode == 0 ? graph.successors[first.source].size() == 1 : graph.predecessors[first.target].size() == 1))
                candidates.emplace_back(end - begin, std::pair{first.source, first.target});
            begin = end;
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        candidateCount = candidates.size();
    }
    phase(1);
    if (mode == 2) {
        for (uint32_t i = 0; i < clusters.size(); ++i) if (predecessorGroups[i] != absent) {
            auto& anchor = anchors[predecessorGroups[i]];
            // The first member creates its anchor without a merge attempt.
            // Later failures replace it exactly as in the ordered-map route.
            if (anchor != i && !merge(anchor, i)) anchor = i;
        }
    } else {
        for (const auto& [weight, link] : candidates) merge(link.first, link.second);
    }
    if (!changed) {
        phase(2);
        record(false, false);
        return false;
    }
    Clusters result;
    std::vector<uint32_t> indices(clusters.size(), absent);
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        auto& index = indices[find(i)];
        if (index == absent) { index = result.size(); result.emplace_back(); }
        result[index].insert(result[index].end(), clusters[i].begin(), clusters[i].end());
    }
    phase(2);
    if (!orderClusters(result, edges, nodeCount)) {
        phase(3);
        record(true, false);
        return false;
    }
    phase(3);
    clusters = std::move(result);
    // Only accepted contractions change cluster identities. Failed cycle
    // checks leave the original clusters, and their quotient, intact.
    quotient.reset();
    record(true, true);
    return true;
}

// The upper partitioner sees only Node DAG edges and whole-node capacities.
Clusters segment(const Clusters& clusters, std::span<const Edge> edges, std::size_t nodeCount,
                 std::span<const uint8_t> clockDomains, std::span<const uint32_t> capacities,
                 uint64_t limit, uint64_t penalty, bool bytes) {
    const Quotient quotient(clusters, edges, nodeCount);
    std::vector<std::vector<ValueId>> incoming(clusters.size()), outgoing(clusters.size());
    uint32_t lastValue = 0;
    for (const auto& edge : quotient.edges) lastValue = std::max(lastValue, edge.value.index);
    struct BoundaryValue {
        uint64_t cost = 0;
        uint32_t source = absent, seen = 0, counted = 0;
    };
    std::vector<BoundaryValue> values(static_cast<std::size_t>(lastValue) + 1);
    for (const auto& edge : quotient.edges) {
        if (!edge.value) continue;
        incoming[edge.target].push_back(edge.value);
        if (edge.source != absent) outgoing[edge.source].push_back(edge.value);
        values[edge.value.index].source = edge.source;
        values[edge.value.index].cost = multiplyCost(edge.rate, bytes ? edge.bytes : 1);
    }
    std::vector<uint64_t> prefix(clusters.size() + 1), cost(clusters.size() + 1, UINT64_MAX);
    std::vector<uint32_t> previous(clusters.size() + 1);
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        prefix[i + 1] = prefix[i];
        for (const auto node : clusters[i]) prefix[i + 1] = addCost(prefix[i + 1], capacities[node]);
    }
    cost[0] = 0;
    for (uint32_t end = 1; end <= clusters.size(); ++end) {
        uint64_t boundary = 0;
        for (uint32_t begin = end; begin > 0;) {
            --begin;
            if (clockDomains[clusters[begin].front()] != clockDomains[clusters[end - 1].front()]) break;
            if (prefix[end] - prefix[begin] > limit) break;
            for (const auto value : incoming[begin]) {
                auto& entry = values[value.index];
                if (entry.seen == end) continue;
                entry.seen = end;
                if (entry.source == absent || entry.source < begin) {
                    entry.counted = end;
                    boundary = addCost(boundary, entry.cost);
                }
            }
            for (const auto value : outgoing[begin]) if (values[value.index].counted == end) {
                values[value.index].counted = 0;
                boundary -= values[value.index].cost;
            }
            const auto candidate = addCost(addCost(cost[begin], boundary), penalty);
            if (candidate <= cost[end]) { cost[end] = candidate; previous[end] = begin; }
        }
        if (cost[end] == UINT64_MAX) throw std::logic_error("Node cluster exceeds supernode capacity");
    }
    Clusters result;
    for (uint32_t end = clusters.size(); end > 0;) {
        const auto begin = previous[end];
        std::vector<uint32_t> members;
        for (uint32_t i = begin; i < end; ++i) members.insert(members.end(), clusters[i].begin(), clusters[i].end());
        result.push_back(std::move(members)); end = begin;
    }
    std::reverse(result.begin(), result.end());
    return result;
}


struct NodeAnalysis {
    std::vector<std::vector<OperationId>> nodes;
    std::vector<uint8_t> clockDomains;
    std::vector<uint32_t> capacities;
    std::vector<Edge> edges;
    uint64_t coveredOps = 0, coveredWork = 0, multiMember = 0, families = 0;
    uint64_t hintedNodes = 0, sharedNodes = 0, mffcNodes = 0, boundaryNodes = 0;
    uint64_t conflictGroups = 0, conflictOps = 0, boundaryHints = 0;
    uint64_t semanticCoveredOps = 0, legacyCoveredOps = 0;
    uint64_t clockRoots = 0, clockOps = 0;
    uint64_t taskNodes = 0, taskArgs = 0, weightedTaskNodes = 0, clampedTaskNodes = 0;
    uint64_t totalCapacity = 0, maxTaskArgs = 0;
};

// Normalize source associations before constructing MFFCs. Identical sets of
// source families share one owner; overlapping sets are separate shared nodes.
// Only cyclic quotient components lose their grouping and fall back to MFFC.
NodeAnalysis buildNodes(const Graph& graph, std::span<const OperationId> topo,
                        const PartitionNonsinkOptions& options) {
    NodeAnalysis result;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> index;
    for (uint32_t i = 0; i < topo.size(); ++i) index.emplace(topo[i], i);
    std::unordered_set<ValueId, ValueIdHash> clockRoots;
    for (const auto op : graph.operations()) if (graph.opKind(op) == kEdgeDetect) {
        for (const auto value : graph.opOperands(op)) clockRoots.insert(value);
        for (const auto value : graph.opResults(op)) clockRoots.insert(value);
    }
    result.clockRoots = clockRoots.size();
    // Follow real operands only. State writer relationships and ordered-effect
    // arcs constrain scheduling, but do not make an independent read a clock.
    std::vector<uint8_t> clockDomains(topo.size());
    std::vector<Edge> edges;
    std::vector<std::vector<uint64_t>> members(topo.size());
    std::set<uint64_t> families;
    ingest::node::NodeKeyInterner legacyFamilies;
    const auto* semanticTree = graph.semanticTree();
    std::unordered_set<uint32_t> anchors;
    std::vector<bool> semanticAnchors(topo.size());
    if (options.semanticNodes) {
        for (const auto symbol : graph.declaredSymbols()) anchors.insert(symbol.value);
        for (const auto& group : graph.generateGroups()) for (const auto symbol : group.symbols) anchors.insert(symbol.value);
        if (semanticTree) {
            const auto leaf = [&](SemanticId id) { return semanticTree->contains(id); };
            for (uint32_t i = 0; i < topo.size(); ++i) {
                // Value anchors mark Node boundaries; op membership marks the
                // implementation set and does not itself create a boundary.
                for (const auto value : graph.opResults(topo[i])) {
                    if (std::any_of(graph.valueSemanticIds(value).begin(),
                                    graph.valueSemanticIds(value).end(), leaf)) {
                        semanticAnchors[i] = true;
                        break;
                    }
                }
            }
        }
    }
    std::vector<bool> boundary(topo.size());
    for (uint32_t i = 0; i < topo.size(); ++i) {
        const auto op = topo[i];
        for (const auto value : graph.opResults(op)) clockDomains[i] |= clockRoots.contains(value);
        if (attribute<int64_t>(graph, op, "grhsim.activity").value_or(0) < 0)
            throw std::invalid_argument("Negative activity rate");
        boundary[i] = operationEffect(graph, op) != OpEffect::Pure ||
            attribute<int64_t>(graph, op, "grhsim.effectOrder").has_value();
        if (semanticTree) {
            const auto addSemantic = [&](std::span<const SemanticId> ids) {
                for (const auto id : ids) {
                    if (!semanticTree->contains(id)) throw std::invalid_argument("partition has invalid SemanticTree Node ID");
                    members[i].push_back(id);
                }
            };
            addSemantic(graph.opSemanticIds(op));
            if (members[i].empty())
                for (const auto value : graph.opResults(op)) addSemantic(graph.valueSemanticIds(value));
            if (!members[i].empty()) ++result.semanticCoveredOps;
        }
        // Legacy schema-v2 annotations remain readable for old checkpoints;
        // SemanticTree associations take precedence for current FIRRTL input.
        if (members[i].empty()) {
            const auto attributes = ingest::node::read(graph, op);
            for (const auto& encoded : attributes.members) {
                auto identity = ingest::node::parseKey(encoded);
                identity.fieldPath.clear();
                members[i].push_back((uint64_t{1} << 32) | legacyFamilies.intern(ingest::node::key(identity)));
            }
            if (!members[i].empty()) ++result.legacyCoveredOps;
        }
        std::sort(members[i].begin(), members[i].end());
        members[i].erase(std::unique(members[i].begin(), members[i].end()), members[i].end());
        if (!members[i].empty()) {
            ++result.coveredOps; result.coveredWork += work(graph, op);
            result.multiMember += members[i].size() > 1;
            families.insert(members[i].begin(), members[i].end());
        }
        if (boundary[i] && !members[i].empty()) ++result.boundaryHints;
        if (options.firrtlNodes == "off" || boundary[i]) members[i].clear();
        for (const auto input : graph.opOperands(op)) {
            const auto producer = graph.valueDef(input);
            const auto found = index.find(producer);
            clockDomains[i] |= clockRoots.contains(input) || (found != index.end() && clockDomains[found->second]);
            const auto rate = producer ? attribute<int64_t>(graph, producer, "grhsim.activity").value_or(1) : 1;
            if (rate < 0) throw std::invalid_argument("Negative activity rate");
            edges.push_back({found == index.end() ? absent : found->second, i, input,
                uint64_t(std::max<int64_t>(1, rate)), valueBytes(graph, input)});
        }
        result.clockOps += clockDomains[i];
    }
    result.families = families.size();
    std::map<int64_t, uint32_t> effects;
    for (uint32_t i = 0; i < topo.size(); ++i)
        if (const auto order = attribute<int64_t>(graph, topo[i], "grhsim.effectOrder")) effects.emplace(*order, i);
    uint32_t previous = absent;
    for (const auto& [order, op] : effects) {
        if (previous != absent) edges.push_back({previous, op, {}});
        previous = op;
    }
    Clusters candidates;
    std::array<std::map<std::vector<uint64_t>, uint32_t>, 2> associated;
    for (uint32_t i = 0; i < topo.size(); ++i) {
        uint32_t group = candidates.size();
        if (!members[i].empty()) {
            const auto [it, inserted] = associated[clockDomains[i]].emplace(members[i], group);
            group = it->second;
        }
        if (group == candidates.size()) candidates.emplace_back();
        candidates[group].push_back(i);
    }
    const Quotient quotient(candidates, edges, topo.size());
    if (quotient.order().size() != candidates.size()) {
        // Iterative Kosaraju avoids recursion on large source graphs.
        std::vector<uint8_t> seen(candidates.size());
        std::vector<uint32_t> finish;
        for (uint32_t root = 0; root < candidates.size(); ++root) if (!seen[root]) {
            std::vector<std::pair<uint32_t, std::size_t>> stack{{root, 0}};
            seen[root] = 1;
            while (!stack.empty()) {
                auto& [current, cursor] = stack.back();
                if (cursor == quotient.successors[current].size()) { finish.push_back(current); stack.pop_back(); continue; }
                const auto next = quotient.successors[current][cursor++];
                if (!seen[next]) { seen[next] = 1; stack.emplace_back(next, 0); }
            }
        }
        std::fill(seen.begin(), seen.end(), 0);
        for (auto it = finish.rbegin(); it != finish.rend(); ++it) if (!seen[*it]) {
            std::vector<uint32_t> component, stack{*it}; seen[*it] = 1;
            while (!stack.empty()) {
                const auto current = stack.back(); stack.pop_back(); component.push_back(current);
                for (const auto next : quotient.predecessors[current]) if (!seen[next]) { seen[next] = 1; stack.push_back(next); }
            }
            if (component.size() <= 1) continue;
            for (const auto group : component) if (candidates[group].size() > 1) {
                ++result.conflictGroups;
                for (const auto op : candidates[group]) { members[op].clear(); ++result.conflictOps; }
            }
        }
    }
    for (auto& groups : associated) groups.clear();
    std::vector<uint32_t> nodeOf(topo.size(), absent);
    std::vector<bool> hinted;
    for (uint32_t i = 0; i < topo.size(); ++i) if (!members[i].empty()) {
        const auto [it, inserted] = associated[clockDomains[i]].emplace(members[i], result.nodes.size());
        if (inserted) {
            result.nodes.emplace_back(); hinted.push_back(true); ++result.hintedNodes;
            result.clockDomains.push_back(clockDomains[i]);
            result.sharedNodes += members[i].size() > 1;
        }
        nodeOf[i] = it->second;
    }
    for (std::size_t i = topo.size(); i-- > 0;) if (nodeOf[i] == absent) {
        const auto op = topo[i];
        bool absorb = !boundary[i] && !semanticAnchors[i] && !anchors.contains(graph.operationSymbol(op).value);
        uint32_t target = absent;
        for (const auto value : graph.opResults(op)) {
            if (graph.valueIsOutput(value) || graph.valueIsInout(value) || anchors.contains(graph.valueSymbol(value).value)) absorb = false;
            for (const auto& use : graph.valueUsers(value)) {
                const auto found = index.find(use.operation);
                if (found == index.end()) { absorb = false; continue; }
                const auto destination = nodeOf[found->second];
                if (destination == absent || hinted[destination]) { absorb = false; continue; }
                if (clockDomains[i] != result.clockDomains[destination]) absorb = false;
                if (target == absent) target = destination;
                else if (target != destination) absorb = false;
            }
        }
        // State reads/effects remain boundaries on both sides of a cone.
        if (target != absent && !hinted[target] && boundary[index.at(result.nodes[target].front())]) absorb = false;
        if (!absorb || target == absent) {
            target = result.nodes.size(); result.nodes.push_back({op}); hinted.push_back(false);
            result.clockDomains.push_back(clockDomains[i]);
            if (boundary[i]) ++result.boundaryNodes; else ++result.mffcNodes;
        }
        nodeOf[i] = target;
    }
    for (auto& node : result.nodes) node.clear();
    for (uint32_t i = 0; i < topo.size(); ++i) result.nodes[nodeOf[i]].push_back(topo[i]);
    // A task argument expands to a TaskArg initializer in generated C++.
    // Charge this expansion without splitting or changing an activity node.
    result.capacities.reserve(result.nodes.size());
    for (const auto& node : result.nodes) {
        uint64_t taskArgs = 0;
        bool hasTask = false;
        for (const auto op : node) if (graph.opKind(op) == kSystemTask) {
            const auto operands = graph.opOperands(op);
            if (operands.size() < 2) throw std::invalid_argument("System task requires fire and enable operands");
            taskArgs = addCost(taskArgs, operands.size() - 2);
            if (graph.opAttrRange(op).find(kLiteralFormat)) taskArgs = addCost(taskArgs, 1);
            hasTask = true;
        }
        const auto capacity = std::max<uint64_t>(1, taskArgs / 16 + (taskArgs % 16 != 0));
        const auto bounded = std::min<uint64_t>(capacity, options.maxSupernodeNodes);
        result.capacities.push_back(static_cast<uint32_t>(bounded));
        result.taskNodes += hasTask;
        result.taskArgs = addCost(result.taskArgs, taskArgs);
        result.weightedTaskNodes += bounded > 1;
        result.clampedTaskNodes += capacity > bounded;
        result.totalCapacity = addCost(result.totalCapacity, bounded);
        result.maxTaskArgs = std::max(result.maxTaskArgs, taskArgs);
    }
    for (auto edge : edges) {
        edge.source = edge.source == absent ? absent : nodeOf[edge.source];
        edge.target = nodeOf[edge.target];
        if (edge.source != edge.target) result.edges.push_back(edge);
    }
    return result;
}

std::string config(const PartitionNonsinkOptions& options) {
    return "v6/" + std::to_string(options.maxSupernodeNodes) + "/" + std::to_string(options.segmentPenalty) + "/" +
        std::to_string(options.semanticNodes) + "/" + std::to_string(options.cloneShared) + "/" +
        std::to_string(options.cloneMaxFanout) + "/" + std::to_string(options.maxClones) + "/" +
        std::to_string(options.cloneLossyCompare) + "/" + options.firrtlNodes + "/" + options.costModel + "/semantic-tree-mffc-clock-task-v1";
}

struct CloneStats { uint64_t count = 0, work = 0, costRejected = 0, clockRejected = 0; };

CloneStats cloneBoundaries(Graph& graph, const PartitionNonsinkOptions& options,
                     std::span<const OperationId> originals, const std::map<Owner, uint8_t>& clockOwners) {
    CloneStats stats;
    if (!options.cloneShared || options.maxClones == 0) return stats;
    const auto nonsink = [&](StringListView path) { return path.size() >= 4 && path[0] == "general" && path[1] == "nonsink"; };
    std::unordered_set<ValueId, ValueIdHash> boundaries;
    for (const auto op : originals) for (const auto value : graph.opResults(op)) {
        for (const auto& user : graph.valueUsers(value))
            if (!sameOwner(ownerView(graph, user.operation), ownerView(graph, op))) boundaries.insert(value);
    }
    for (const auto op : originals) {
        if (!canCloneOperation(graph, op) || graph.opResults(op).size() != 1) continue;
        const auto originalOwner = owner(graph, op);
        auto originalExecution = originalOwner; originalExecution.resize(3);
        const auto result = graph.opResults(op)[0];
        const auto type = graph.valueLogicalType(result);
        if (type.kind != ValueType::Logic || type.domain != LogicDomain::TwoState || !type.shape.empty() || type.width > 64) continue;
        const std::vector<ValueId> args(graph.opOperands(op).begin(), graph.opOperands(op).end());
        const auto kind = graph.opKind(op);
        ValueId varying;
        if ((kind == OperationKind::kNot || kind == OperationKind::kLogicNot || kind == OperationKind::kAssign) && args.size() == 1 && graph.sameValueType(result, args[0])) varying = args[0];
        const auto scalar = [&](ValueId input) {
            const auto inputType = graph.valueLogicalType(input);
            return inputType.kind == ValueType::Logic && inputType.domain == LogicDomain::TwoState &&
                inputType.shape.empty() && inputType.width <= 64;
        };
        const bool bijection = (kind == OperationKind::kXor || kind == OperationKind::kAdd || kind == OperationKind::kSub) &&
            args.size() == 2 && graph.sameValueType(result, args[0]) && graph.sameValueType(result, args[1]);
        const bool comparison = options.cloneLossyCompare && (kind == OperationKind::kEq || kind == OperationKind::kNe) &&
            args.size() == 2 && scalar(args[0]) && scalar(args[1]);
        if (bijection || comparison) {
            const bool left = constantBits(graph, args[0]).has_value(), right = constantBits(graph, args[1]).has_value();
            if (left != right) varying = args[left ? 1 : 0];
        }
        if (!varying) continue;
        const auto producer = graph.valueDef(varying);
        if (producer && !boundaries.contains(varying) && !isRegisterReadPortKind(graph.opKind(producer)) && graph.opKind(producer) != OperationKind::kLatchReadPort) continue;
        std::map<Owner, std::vector<ValueUser>> destinations;
        for (const auto& user : graph.valueUsers(result)) {
            const auto& path = ownerView(graph, user.operation);
            if (!sameOwner(path, originalOwner) && nonsink(path)) destinations[copyOwner(path)].push_back(user);
        }
        if (destinations.empty() || destinations.size() > options.cloneMaxFanout) continue;
        // One bounded round over the original candidates, with no recursive
        // cloning of a newly-created operation.
        for (const auto& [path, users] : destinations) {
            if (stats.count >= options.maxClones) return stats;
            auto execution = path; execution.resize(3);
            // Do not reintroduce data-only work into an owner fired by a clock.
            // Constants copied below are immutable and remain neutral.
            if (clockOwners.at(originalExecution) != clockOwners.at(execution)) {
                ++stats.costRejected; ++stats.clockRejected; continue;
            }
            std::set<uint32_t> constants;
            uint64_t extra = work(graph, op);
            for (const auto input : args) {
                const auto constant = graph.valueDef(input);
                if (constant && graph.opKind(constant) == OperationKind::kConstant && !sameOwner(ownerView(graph, constant), path) &&
                    constants.insert(constant.index).second) extra += work(graph, constant);
            }
            // The input must already reach this execution owner. Then copying a
            // cheap transform removes one payload without adding an activation
            // dependency. Record copied work only as a diagnostic.
            bool available = false;
            if (producer) {
                auto from = owner(graph, producer); if (from.size() > 3) from.resize(3);
                available |= from == execution;
            }
            for (const auto& user : graph.valueUsers(varying)) {
                auto destination = owner(graph, user.operation); if (destination.size() > 3) destination.resize(3);
                available |= destination == execution;
            }
            auto from = originalOwner; from.resize(3);
            if (from == execution || !available) {
                ++stats.costRejected; continue;
            }
            std::vector<ValueId> inputs(args.begin(), args.end());
            for (auto& input : inputs) {
                const auto constant = graph.valueDef(input);
                if (constant && graph.opKind(constant) == OperationKind::kConstant && !sameOwner(ownerView(graph, constant), path)) {
                    const auto copy = cloneOperation(graph, constant, {}, path);
                    input = graph.opResults(copy)[0];
                }
            }
            const auto clone = cloneOperation(graph, op, inputs, path);
            const auto copied = graph.opResults(clone)[0];
            for (const auto& use : users) graph.replaceOperand(use.operation, use.operandIndex, copied);
            ++stats.count; stats.work += extra;
        }
    }
    return stats;
}
}

PartitionNonsinkPass::PartitionNonsinkPass(PartitionNonsinkOptions options)
    : Pass("grhsim-partition-nonsink", "grhsim-partition-nonsink", "Partition nonsink activity on GRH"), options_(std::move(options)) {}

bool PartitionNonsinkPass::supportsDialect(std::string_view dialect) const noexcept { return supports(dialect); }

PassResult PartitionNonsinkPass::run() {
    PassResult result;
    Graph* graph = nullptr;
    const bool phaseTiming = std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr;
    const auto timingStart = std::chrono::steady_clock::now();
    auto previousPhase = timingStart;
    const auto phase = [&](const char* name) {
        if (!phaseTiming) return;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previousPhase).count();
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(now - timingStart).count();
        uint64_t rssKiB = 0, peakRssKiB = 0;
        std::ifstream status("/proc/self/status");
        for (std::string line; std::getline(status, line);) {
            if (line.starts_with("VmRSS:")) rssKiB = std::strtoull(line.c_str() + 6, nullptr, 10);
            else if (line.starts_with("VmHWM:")) peakRssKiB = std::strtoull(line.c_str() + 6, nullptr, 10);
        }
        std::fprintf(stderr, "[grhsim-partition-nonsink-phase] phase=%s elapsed_ms=%lld total_ms=%lld rss_kib=%llu peak_rss_kib=%llu operations=%zu values=%zu\n",
            name, static_cast<long long>(elapsed), static_cast<long long>(total),
            static_cast<unsigned long long>(rssKiB), static_cast<unsigned long long>(peakRssKiB),
            graph ? graph->operations().size() : 0, graph ? graph->values().size() : 0);
        previousPhase = now;
    };
    try {
        if (!options_.maxSupernodeNodes || !options_.cloneMaxFanout)
            throw std::invalid_argument("Node capacity/fanout limits must be positive");
        graph = &partitionGraph(design(), options_.top);
        ingest::node::requirePolicy(*graph);
        if (options_.firrtlNodes != "off" && options_.firrtlNodes != "prefer" && options_.firrtlNodes != "auto")
            throw std::invalid_argument("firrtl-nodes must be off, prefer or auto");
        if (options_.costModel != "legacy" && options_.costModel != "bytes")
            throw std::invalid_argument("cost-model must be legacy or bytes");
        const auto configuration = config(options_);
        if (stage(*graph) == "nonsink-partitioned") {
            if (graph->metadata("grhsim.nonsinkConfig") != std::optional<AttributeValue>(configuration))
                throw std::invalid_argument("Changed activity configuration requires the nonsink-sink-split checkpoint");
            if (!options_.report.empty()) {
                std::ofstream report(options_.report);
                const auto stats = graph->metadata("grhsim.nonsinkHintStats");
                if (!report || !stats) throw std::invalid_argument("Cannot re-emit nonsink hint report");
                for (const auto& line : std::get<std::vector<std::string>>(*stats)) report << line << '\n';
                if (!report) throw std::invalid_argument("Cannot re-emit nonsink hint report");
            }
            return result;
        }
        if (stage(*graph) != "nonsink-sink-split") throw std::invalid_argument("Activity partitioning requires nonsink-sink-split stage");
        const Owner nonsinkOwner{"general", "nonsink"};
        phase("contracts-checked");
        const auto topo = topological(*graph, [&](OperationId op) { return sameOwner(ownerView(*graph, op), nonsinkOwner); });
        phase("nonsink-topology-built");
        const auto analysisStarted = std::chrono::steady_clock::now();
        const auto analysis = buildNodes(*graph, topo, options_);
        phase("activity-nodes-built");
        const auto& nodes = analysis.nodes;
        const auto& edges = analysis.edges;
        const bool useHints = analysis.hintedNodes != 0;
        Clusters clusters(nodes.size());
        for (uint32_t i = 0; i < nodes.size(); ++i) clusters[i] = {i};
        if (!orderClusters(clusters, edges, nodes.size())) throw std::invalid_argument("Node contraction introduced a cycle");
        phase("initial-clusters-ordered");
        std::array<uint64_t, 3> coarsenAccepted{};
        {
            // Reuse the quotient across modes and rounds only while clusters
            // stay unchanged; release it before segment builds its own.
            std::unique_ptr<Quotient> quotient;
            while (!clusters.empty() && options_.costModel == "legacy") {
                const auto before = clusters.size();
                for (unsigned mode = 0; mode < 3; ++mode)
                    coarsenAccepted[mode] += coarsen(clusters, edges, nodes.size(), analysis.clockDomains, analysis.capacities,
                        options_.maxSupernodeNodes, mode, phaseTiming, quotient);
                if (clusters.size() == before) break;
            }
        }
        phase("clusters-coarsened");
        clusters = segment(clusters, edges, nodes.size(), analysis.clockDomains, analysis.capacities, options_.maxSupernodeNodes,
            options_.segmentPenalty, options_.costModel == "bytes");
        phase("clusters-segmented");
        result.changed = true;
        std::map<Owner, uint8_t> clockOwners;
        uint64_t clockSupernodes = 0;
        for (const auto& cluster : clusters) {
            const auto anchor = nodes[cluster.front()].front();
            const std::string key = "sn_" + std::string(graph->symbolText(graph->operationSymbol(anchor)));
            const auto domain = analysis.clockDomains[cluster.front()];
            clockOwners.emplace(Owner{"general", "nonsink", key}, domain);
            clockSupernodes += domain;
            for (const auto node : cluster) {
                const std::string nodeKey = "node_" + std::string(graph->symbolText(graph->operationSymbol(nodes[node].front())));
                for (const auto op : nodes[node]) graph->setAttr(op, kPartition, Owner{"general", "nonsink", key, nodeKey});
            }
        }
        phase("partition-owners-written");
        const auto cloneStats = cloneBoundaries(*graph, options_, topo, clockOwners);
        phase("boundary-clones-materialized");
        uint64_t splitHints = 0, leafCount = 0, boundaryBytes = 0, comparedBytes = 0, multiSourceGroups = 0;
        std::map<std::string, std::set<Owner>> destinations;
        std::map<Owner, std::set<std::string>> groupSources;
        std::set<Owner> leaves;
        std::map<Owner, uint64_t> leafWork, executionWork;
        std::map<Owner, uint32_t> nodeIds, executionIds;
        uint32_t lastOperation = 0;
        for (const auto op : graph->operations()) lastOperation = std::max(lastOperation, op.index);
        std::vector<uint32_t> nodeOfOp(static_cast<std::size_t>(lastOperation) + 1, absent),
                              executionOfOp(nodeOfOp.size(), absent);
        uint64_t totalWork = 0, nonsinkOps = 0;
        std::unordered_set<ValueId, ValueIdHash> persistent;
        std::set<std::pair<uint32_t, Owner>> comparisons;
        for (const auto op : graph->operations()) {
            const auto path = owner(*graph, op);
            if (path.size() != 4 || path[0] != "general" || path[1] != "nonsink") continue;
            leaves.insert(path);
            Owner execution(path.begin(), path.begin() + 3);
            nodeOfOp[op.index] = nodeIds.emplace(path, nodeIds.size()).first->second;
            executionOfOp[op.index] = executionIds.emplace(execution, executionIds.size()).first->second;
            const auto weight = work(*graph, op); ++nonsinkOps; totalWork += weight;
            leafWork[path] += weight; executionWork[execution] += weight;
            const auto attributes = ingest::node::read(*graph, op);
            for (const auto& key : attributes.members) { destinations[key].insert(execution); groupSources[execution].insert(key); }
            if (attributes.members.empty()) groupSources[execution].insert("unannotated");
        }
        phase("report-owner-census-done");
        // Census every real consumer, including sink, event and output phases.
        // The first three nonsink components identify the execution owner.
        std::vector<uint64_t> nodeEdges, supernodeEdges, valueTargets, externalValueTargets;
        std::map<int64_t, OperationId> orderedEffects;
        for (const auto op : graph->operations()) {
            auto execution = owner(*graph, op);
            if (execution.size() == 4 && execution[0] == "general" && execution[1] == "nonsink") execution.resize(3);
            for (const auto value : graph->opOperands(op)) {
                const auto producer = graph->valueDef(value);
                const auto targetNode = nodeOfOp[op.index], targetSupernode = executionOfOp[op.index];
                const auto sourceNode = producer ? nodeOfOp[producer.index] : absent;
                const auto sourceSupernode = producer ? executionOfOp[producer.index] : absent;
                if (targetNode != absent) {
                    if (sourceNode != absent && sourceNode != targetNode)
                        nodeEdges.push_back(pairKey(sourceNode, targetNode));
                    if (sourceSupernode != targetSupernode) {
                        if (sourceSupernode == absent) externalValueTargets.push_back(pairKey(value.index, targetSupernode));
                        else {
                            supernodeEdges.push_back(pairKey(sourceSupernode, targetSupernode));
                            valueTargets.push_back(pairKey(value.index, targetSupernode));
                        }
                    }
                }
                auto from = producer ? owner(*graph, producer) : Owner{};
                if (from.size() == 4 && from[0] == "general" && from[1] == "nonsink") from.resize(3);
                if (from != execution && comparisons.emplace(value.index, execution).second) comparedBytes += valueBytes(*graph, value);
                if (producer && from.size() == 3 && from[0] == "general" && from[1] == "nonsink" &&
                    from != execution && persistent.insert(value).second) boundaryBytes += valueBytes(*graph, value);
            }
            if (const auto order = attribute<int64_t>(*graph, op, "grhsim.effectOrder")) orderedEffects.emplace(*order, op);
        }
        phase("report-edge-census-done");
        const auto nodeDataEdges = distinctPairs(nodeEdges), supernodeDataEdges = distinctPairs(supernodeEdges);
        const auto internalTargets = distinctPairs(valueTargets), externalTargets = distinctPairs(externalValueTargets);
        OperationId previousEffect;
        for (const auto& [order, op] : orderedEffects) {
            if (previousEffect) {
                const auto from = nodeOfOp[previousEffect.index], to = nodeOfOp[op.index];
                if (from != absent && to != absent && from != to) nodeEdges.push_back(pairKey(from, to));
                const auto source = executionOfOp[previousEffect.index], target = executionOfOp[op.index];
                if (source != absent && target != absent && source != target) supernodeEdges.push_back(pairKey(source, target));
            }
            previousEffect = op;
        }
        const auto nodeDependencyEdges = distinctPairs(nodeEdges), supernodeDependencyEdges = distinctPairs(supernodeEdges);
        leafCount = leaves.size();
        for (const auto& [key, groups] : destinations) splitHints += groups.size() > 1;
        for (const auto& [key, sources] : groupSources) multiSourceGroups += sources.size() > 1;
        uint64_t maxLeaf = 0, maxExecution = 0;
        for (const auto& [key, weight] : leafWork) maxLeaf = std::max(maxLeaf, weight);
        for (const auto& [key, weight] : executionWork) maxExecution = std::max(maxExecution, weight);
        const std::string resolved = useHints ? "prefer" : "off";
        uint64_t maxNodes = 0, maxCapacity = 0;
        for (const auto& cluster : clusters) {
            maxNodes = std::max<uint64_t>(maxNodes, cluster.size());
            uint64_t capacity = 0;
            for (const auto node : cluster) capacity += analysis.capacities[node];
            maxCapacity = std::max(maxCapacity, capacity);
        }
        std::vector<std::string> stats{"semantic-tree-mffc-clock-task-v1", "requested=" + options_.firrtlNodes,
            "resolved=" + resolved, "cost=" + options_.costModel + "-node-v1",
            "covered-ops=" + std::to_string(analysis.coveredOps), "covered-work=" + std::to_string(analysis.coveredWork),
            "semantic-covered-ops=" + std::to_string(analysis.semanticCoveredOps),
            "legacy-covered-ops=" + std::to_string(analysis.legacyCoveredOps),
            "multi-member-ops=" + std::to_string(analysis.multiMember), "hint-families=" + std::to_string(analysis.families),
            "clock-roots=" + std::to_string(analysis.clockRoots), "clock-dependent-ops=" + std::to_string(analysis.clockOps),
            "clock-dependent-supernodes=" + std::to_string(clockSupernodes),
            "task-nodes=" + std::to_string(analysis.taskNodes), "task-args=" + std::to_string(analysis.taskArgs),
            "task-weighted-nodes=" + std::to_string(analysis.weightedTaskNodes),
            "task-clamped-nodes=" + std::to_string(analysis.clampedTaskNodes),
            "node-max-task-args=" + std::to_string(analysis.maxTaskArgs),
            "node-capacity-units=" + std::to_string(analysis.totalCapacity),
            "firrtl-nodes=" + std::to_string(analysis.hintedNodes), "shared-nodes=" + std::to_string(analysis.sharedNodes),
            "mffc-nodes=" + std::to_string(analysis.mffcNodes), "boundary-nodes=" + std::to_string(analysis.boundaryNodes),
            "hint-conflict-groups=" + std::to_string(analysis.conflictGroups), "hint-conflict-ops=" + std::to_string(analysis.conflictOps),
            "hint-boundary-ops=" + std::to_string(analysis.boundaryHints), "split-hints=" + std::to_string(splitHints),
            "mixed-groups=" + std::to_string(multiSourceGroups), "nodes=" + std::to_string(leafCount),
            "supernodes=" + std::to_string(clusters.size()), "supernode-max-nodes=" + std::to_string(maxNodes),
            "supernode-max-capacity=" + std::to_string(maxCapacity),
            "coarsen-mode0-accepted=" + std::to_string(coarsenAccepted[0]),
            "coarsen-mode1-accepted=" + std::to_string(coarsenAccepted[1]),
            "coarsen-mode2-accepted=" + std::to_string(coarsenAccepted[2]),
            "node-data-edges=" + std::to_string(nodeDataEdges), "node-dependency-edges=" + std::to_string(nodeDependencyEdges),
            "supernode-data-edges=" + std::to_string(supernodeDataEdges),
            "supernode-dependency-edges=" + std::to_string(supernodeDependencyEdges),
            "nonsink-internal-boundary-value-targets=" + std::to_string(internalTargets),
            "nonsink-external-boundary-value-targets=" + std::to_string(externalTargets),
            "nonsink-boundary-value-targets=" + std::to_string(internalTargets + externalTargets),
            "persistent-bytes=" + std::to_string(boundaryBytes), "consumer-bytes=" + std::to_string(comparedBytes),
            "clone-ops=" + std::to_string(cloneStats.count), "clone-extra-work=" + std::to_string(cloneStats.work),
            "clone-skip-cost=" + std::to_string(cloneStats.costRejected),
            "clone-skip-clock=" + std::to_string(cloneStats.clockRejected),
            "nonsink-ops=" + std::to_string(nonsinkOps), "total-work=" + std::to_string(totalWork),
            "node-max-work=" + std::to_string(maxLeaf), "supernode-max-work=" + std::to_string(maxExecution)};
        for (const auto& cluster : clusters) {
            const auto path = owner(*graph, nodes[cluster.front()].front());
            stats.push_back("supernode-nodes=" + path[2] + ":" + std::to_string(cluster.size()));
            uint64_t capacity = 0;
            for (const auto node : cluster) capacity += analysis.capacities[node];
            stats.push_back("supernode-capacity=" + path[2] + ":" + std::to_string(capacity));
        }
        graph->setMetadata("grhsim.nonsinkHintStats", stats);
        if (!options_.report.empty()) {
            std::ofstream report(options_.report);
            if (!report) throw std::invalid_argument("Cannot write nonsink hint report");
            for (const auto& line : stats) report << line << '\n';
            if (!report) throw std::invalid_argument("Cannot write nonsink hint report");
        }
        graph->setMetadata("grhsim.nonsinkConfig", configuration);
        graph->setMetadata(kStage, std::string("nonsink-partitioned"));
        phase("report-published");
        verifyPartitionResult(*graph);
        phase("partition-verified");
        logInfo("activity_nodes=" + std::to_string(nodes.size()) + " supernodes=" + std::to_string(clusters.size()) +
                " hint-analysis-us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - analysisStarted).count()));
    } catch (const std::exception& exception) {
        phase("failed");
        if (graph && result.changed) graph->setMetadata(kStage, std::string("nonsink-partitioned-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{}); result.failed = true;
    }
    return result;
}

}
