#include "transform/grhsim/partition-nonsink.hpp"

#include "partition-internal.hpp"

#include <chrono>
#include <fstream>
#include <numeric>
#include <set>
#include <tuple>

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
        for (auto& row : predecessors) std::sort(row.begin(), row.end());
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

bool coarsen(Clusters& clusters, std::span<const Edge> edges, std::size_t nodeCount,
             uint64_t limit, unsigned mode) {
    const Quotient graph(clusters, edges, nodeCount);
    std::vector<uint32_t> parents(clusters.size());
    std::iota(parents.begin(), parents.end(), 0);
    std::vector<uint64_t> weights(clusters.size());
    for (uint32_t i = 0; i < clusters.size(); ++i) weights[i] = clusters[i].size();
    const auto find = [&](uint32_t node) {
        while (parents[node] != node) { parents[node] = parents[parents[node]]; node = parents[node]; }
        return node;
    };
    bool changed = false;
    const auto merge = [&](uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a == b || weights[a] + weights[b] > limit) return false;
        if (a > b) std::swap(a, b);
        parents[b] = a; weights[a] += weights[b]; changed = true;
        return true;
    };
    if (mode == 2) {
        std::map<std::vector<uint32_t>, uint32_t> anchors;
        for (uint32_t i = 0; i < clusters.size(); ++i) if (!graph.predecessors[i].empty()) {
            const auto [it, inserted] = anchors.emplace(graph.predecessors[i], i);
            if (!inserted && !merge(it->second, i)) it->second = i;
        }
    } else {
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> links;
        for (const auto& edge : graph.edges) if (edge.source != absent)
            if (mode == 0 ? graph.successors[edge.source].size() == 1 : graph.predecessors[edge.target].size() == 1)
                ++links[{edge.source, edge.target}];
        std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> candidates;
        for (const auto& [link, weight] : links) candidates.emplace_back(weight, link);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        for (const auto& [weight, link] : candidates) merge(link.first, link.second);
    }
    if (!changed) return false;
    Clusters result;
    std::map<uint32_t, uint32_t> indices;
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        const auto [it, inserted] = indices.emplace(find(i), result.size());
        if (inserted) result.emplace_back();
        result[it->second].insert(result[it->second].end(), clusters[i].begin(), clusters[i].end());
    }
    if (!orderClusters(result, edges, nodeCount)) return false;
    clusters = std::move(result);
    return true;
}

// The upper partitioner sees only Node DAG edges and unit node counts.
Clusters segment(const Clusters& clusters, std::span<const Edge> edges, std::size_t nodeCount,
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
    for (uint32_t i = 0; i < clusters.size(); ++i) prefix[i + 1] = prefix[i] + clusters[i].size();
    cost[0] = 0;
    for (uint32_t end = 1; end <= clusters.size(); ++end) {
        uint64_t boundary = 0;
        for (uint32_t begin = end; begin > 0;) {
            --begin;
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
    std::vector<Edge> edges;
    uint64_t coveredOps = 0, coveredWork = 0, multiMember = 0, families = 0;
    uint64_t hintedNodes = 0, sharedNodes = 0, mffcNodes = 0, boundaryNodes = 0;
    uint64_t conflictGroups = 0, conflictOps = 0, boundaryHints = 0;
};

// Normalize source associations before constructing MFFCs. Identical sets of
// source families share one owner; overlapping sets are separate shared nodes.
// Only cyclic quotient components lose their grouping and fall back to MFFC.
NodeAnalysis buildNodes(const Graph& graph, std::span<const OperationId> topo,
                        const PartitionNonsinkOptions& options) {
    NodeAnalysis result;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> index;
    for (uint32_t i = 0; i < topo.size(); ++i) index.emplace(topo[i], i);
    std::vector<Edge> edges;
    std::vector<std::vector<std::string>> members(topo.size());
    std::set<std::string> families;
    std::unordered_set<uint32_t> anchors;
    if (options.semanticNodes) {
        for (const auto symbol : graph.declaredSymbols()) anchors.insert(symbol.value);
        for (const auto& group : graph.generateGroups()) for (const auto symbol : group.symbols) anchors.insert(symbol.value);
    }
    std::vector<bool> boundary(topo.size());
    for (uint32_t i = 0; i < topo.size(); ++i) {
        const auto op = topo[i];
        if (attribute<int64_t>(graph, op, "grhsim.activity").value_or(0) < 0)
            throw std::invalid_argument("Negative activity rate");
        boundary[i] = operationEffect(graph, op) != OpEffect::Pure ||
            attribute<int64_t>(graph, op, "grhsim.effectOrder").has_value();
        const auto attributes = ingest::node::read(graph, op);
        if (!attributes.members.empty()) {
            ++result.coveredOps; result.coveredWork += work(graph, op);
            result.multiMember += attributes.members.size() > 1;
        }
        for (const auto& encoded : attributes.members) {
            auto identity = ingest::node::parseKey(encoded);
            identity.fieldPath.clear();
            members[i].push_back(ingest::node::key(identity));
            families.insert(members[i].back());
        }
        std::sort(members[i].begin(), members[i].end());
        members[i].erase(std::unique(members[i].begin(), members[i].end()), members[i].end());
        if (boundary[i] && !members[i].empty()) ++result.boundaryHints;
        if (options.firrtlNodes == "off" || boundary[i]) members[i].clear();
        for (const auto input : graph.opOperands(op)) {
            const auto producer = graph.valueDef(input);
            const auto found = index.find(producer);
            const auto rate = producer ? attribute<int64_t>(graph, producer, "grhsim.activity").value_or(1) : 1;
            if (rate < 0) throw std::invalid_argument("Negative activity rate");
            edges.push_back({found == index.end() ? absent : found->second, i, input,
                uint64_t(std::max<int64_t>(1, rate)), valueBytes(graph, input)});
        }
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
    std::map<std::vector<std::string>, uint32_t> associated;
    for (uint32_t i = 0; i < topo.size(); ++i) {
        uint32_t group = candidates.size();
        if (!members[i].empty()) {
            const auto [it, inserted] = associated.emplace(members[i], group);
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
    associated.clear();
    std::vector<uint32_t> nodeOf(topo.size(), absent);
    std::vector<bool> hinted;
    for (uint32_t i = 0; i < topo.size(); ++i) if (!members[i].empty()) {
        const auto [it, inserted] = associated.emplace(members[i], result.nodes.size());
        if (inserted) {
            result.nodes.emplace_back(); hinted.push_back(true); ++result.hintedNodes;
            result.sharedNodes += members[i].size() > 1;
        }
        nodeOf[i] = it->second;
    }
    for (std::size_t i = topo.size(); i-- > 0;) if (nodeOf[i] == absent) {
        const auto op = topo[i];
        bool absorb = !boundary[i] && !anchors.contains(graph.operationSymbol(op).value);
        uint32_t target = absent;
        for (const auto value : graph.opResults(op)) {
            if (graph.valueIsOutput(value) || graph.valueIsInout(value) || anchors.contains(graph.valueSymbol(value).value)) absorb = false;
            for (const auto& use : graph.valueUsers(value)) {
                const auto found = index.find(use.operation);
                if (found == index.end()) { absorb = false; continue; }
                const auto destination = nodeOf[found->second];
                if (destination == absent || hinted[destination]) { absorb = false; continue; }
                if (target == absent) target = destination;
                else if (target != destination) absorb = false;
            }
        }
        // State reads/effects remain boundaries on both sides of a cone.
        if (target != absent && !hinted[target] && boundary[index.at(result.nodes[target].front())]) absorb = false;
        if (!absorb || target == absent) {
            target = result.nodes.size(); result.nodes.push_back({op}); hinted.push_back(false);
            if (boundary[i]) ++result.boundaryNodes; else ++result.mffcNodes;
        }
        nodeOf[i] = target;
    }
    for (auto& node : result.nodes) node.clear();
    for (uint32_t i = 0; i < topo.size(); ++i) result.nodes[nodeOf[i]].push_back(topo[i]);
    for (auto edge : edges) {
        edge.source = edge.source == absent ? absent : nodeOf[edge.source];
        edge.target = nodeOf[edge.target];
        if (edge.source != edge.target) result.edges.push_back(edge);
    }
    return result;
}

std::string config(const PartitionNonsinkOptions& options) {
    return "v3/" + std::to_string(options.maxSupernodeNodes) + "/" + std::to_string(options.segmentPenalty) + "/" +
        std::to_string(options.semanticNodes) + "/" + std::to_string(options.cloneShared) + "/" +
        std::to_string(options.cloneMaxFanout) + "/" + std::to_string(options.maxClones) + "/" +
        std::to_string(options.cloneLossyCompare) + "/" + options.firrtlNodes + "/" + options.costModel + "/hint-mffc-v1";
}

struct CloneStats { uint64_t count = 0, work = 0, costRejected = 0; };

CloneStats cloneBoundaries(Graph& graph, const PartitionNonsinkOptions& options,
                     std::span<const OperationId> originals) {
    CloneStats stats;
    if (!options.cloneShared || options.maxClones == 0) return stats;
    const auto nonsink = [&](const Owner& path) { return path.size() >= 4 && path[0] == "general" && path[1] == "nonsink"; };
    std::unordered_set<ValueId, ValueIdHash> boundaries;
    for (const auto op : originals) for (const auto value : graph.opResults(op)) {
        for (const auto& user : graph.valueUsers(value))
            if (ownerView(graph, user.operation) != ownerView(graph, op)) boundaries.insert(value);
    }
    for (const auto op : originals) {
        if (!canCloneOperation(graph, op) || graph.opResults(op).size() != 1) continue;
        const auto originalOwner = owner(graph, op);
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
            if (path != originalOwner && nonsink(path)) destinations[path].push_back(user);
        }
        if (destinations.empty() || destinations.size() > options.cloneMaxFanout) continue;
        // One bounded round over the original candidates, with no recursive
        // cloning of a newly-created operation.
        for (const auto& [path, users] : destinations) {
            if (stats.count >= options.maxClones) return stats;
            auto execution = path; execution.resize(3);
            std::set<uint32_t> constants;
            uint64_t extra = work(graph, op);
            for (const auto input : args) {
                const auto constant = graph.valueDef(input);
                if (constant && graph.opKind(constant) == OperationKind::kConstant && ownerView(graph, constant) != path &&
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
                if (constant && graph.opKind(constant) == OperationKind::kConstant && ownerView(graph, constant) != path) {
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
        const auto topo = topological(*graph, [&](OperationId op) { return ownerView(*graph, op) == nonsinkOwner; });
        const auto analysisStarted = std::chrono::steady_clock::now();
        const auto analysis = buildNodes(*graph, topo, options_);
        const auto& nodes = analysis.nodes;
        const auto& edges = analysis.edges;
        const bool useHints = analysis.hintedNodes != 0;
        Clusters clusters(nodes.size());
        for (uint32_t i = 0; i < nodes.size(); ++i) clusters[i] = {i};
        if (!orderClusters(clusters, edges, nodes.size())) throw std::invalid_argument("Node contraction introduced a cycle");
        while (!clusters.empty() && options_.costModel == "legacy") {
            const auto before = clusters.size();
            for (unsigned mode = 0; mode < 3; ++mode)
                coarsen(clusters, edges, nodes.size(), options_.maxSupernodeNodes, mode);
            if (clusters.size() == before) break;
        }
        clusters = segment(clusters, edges, nodes.size(), options_.maxSupernodeNodes,
            options_.segmentPenalty, options_.costModel == "bytes");
        result.changed = true;
        for (const auto& cluster : clusters) {
            const auto anchor = nodes[cluster.front()].front();
            const std::string key = "sn_" + std::string(graph->symbolText(graph->operationSymbol(anchor)));
            for (const auto node : cluster) {
                const std::string nodeKey = "node_" + std::string(graph->symbolText(graph->operationSymbol(nodes[node].front())));
                for (const auto op : nodes[node]) graph->setAttr(op, kPartition, Owner{"general", "nonsink", key, nodeKey});
            }
        }
        const auto cloneStats = cloneBoundaries(*graph, options_, topo);
        uint64_t splitHints = 0, leafCount = 0, boundaryBytes = 0, comparedBytes = 0, multiSourceGroups = 0;
        std::map<std::string, std::set<Owner>> destinations;
        std::map<Owner, std::set<std::string>> groupSources;
        std::set<Owner> leaves;
        std::map<Owner, uint64_t> leafWork, executionWork;
        uint64_t totalWork = 0, nonsinkOps = 0;
        std::unordered_set<ValueId, ValueIdHash> persistent;
        std::set<std::pair<uint32_t, Owner>> comparisons;
        for (const auto op : graph->operations()) {
            const auto path = owner(*graph, op);
            if (path.size() != 4 || path[0] != "general" || path[1] != "nonsink") continue;
            leaves.insert(path);
            Owner execution(path.begin(), path.begin() + 3);
            const auto weight = work(*graph, op); ++nonsinkOps; totalWork += weight;
            leafWork[path] += weight; executionWork[execution] += weight;
            const auto attributes = ingest::node::read(*graph, op);
            for (const auto& key : attributes.members) { destinations[key].insert(execution); groupSources[execution].insert(key); }
            if (attributes.members.empty()) groupSources[execution].insert("unannotated");
        }
        // Census every real consumer, including sink, event and output phases.
        // The first three nonsink components identify the execution owner.
        for (const auto op : graph->operations()) {
            auto execution = owner(*graph, op);
            if (execution.size() == 4 && execution[0] == "general" && execution[1] == "nonsink") execution.resize(3);
            for (const auto value : graph->opOperands(op)) {
                const auto producer = graph->valueDef(value);
                auto from = producer ? owner(*graph, producer) : Owner{};
                if (from.size() == 4 && from[0] == "general" && from[1] == "nonsink") from.resize(3);
                if (from != execution && comparisons.emplace(value.index, execution).second) comparedBytes += valueBytes(*graph, value);
                if (producer && from.size() == 3 && from[0] == "general" && from[1] == "nonsink" &&
                    from != execution && persistent.insert(value).second) boundaryBytes += valueBytes(*graph, value);
            }
        }
        leafCount = leaves.size();
        for (const auto& [key, groups] : destinations) splitHints += groups.size() > 1;
        for (const auto& [key, sources] : groupSources) multiSourceGroups += sources.size() > 1;
        uint64_t maxLeaf = 0, maxExecution = 0;
        for (const auto& [key, weight] : leafWork) maxLeaf = std::max(maxLeaf, weight);
        for (const auto& [key, weight] : executionWork) maxExecution = std::max(maxExecution, weight);
        const std::string resolved = useHints ? "prefer" : "off";
        uint64_t maxNodes = 0;
        for (const auto& cluster : clusters) maxNodes = std::max<uint64_t>(maxNodes, cluster.size());
        std::vector<std::string> stats{"hint-mffc-v1", "requested=" + options_.firrtlNodes,
            "resolved=" + resolved, "cost=" + options_.costModel + "-node-v1",
            "covered-ops=" + std::to_string(analysis.coveredOps), "covered-work=" + std::to_string(analysis.coveredWork),
            "multi-member-ops=" + std::to_string(analysis.multiMember), "hint-families=" + std::to_string(analysis.families),
            "firrtl-nodes=" + std::to_string(analysis.hintedNodes), "shared-nodes=" + std::to_string(analysis.sharedNodes),
            "mffc-nodes=" + std::to_string(analysis.mffcNodes), "boundary-nodes=" + std::to_string(analysis.boundaryNodes),
            "hint-conflict-groups=" + std::to_string(analysis.conflictGroups), "hint-conflict-ops=" + std::to_string(analysis.conflictOps),
            "hint-boundary-ops=" + std::to_string(analysis.boundaryHints), "split-hints=" + std::to_string(splitHints),
            "mixed-groups=" + std::to_string(multiSourceGroups), "nodes=" + std::to_string(leafCount),
            "supernodes=" + std::to_string(clusters.size()), "supernode-max-nodes=" + std::to_string(maxNodes),
            "persistent-bytes=" + std::to_string(boundaryBytes), "consumer-bytes=" + std::to_string(comparedBytes),
            "clone-ops=" + std::to_string(cloneStats.count), "clone-extra-work=" + std::to_string(cloneStats.work),
            "clone-skip-cost=" + std::to_string(cloneStats.costRejected),
            "nonsink-ops=" + std::to_string(nonsinkOps), "total-work=" + std::to_string(totalWork),
            "node-max-work=" + std::to_string(maxLeaf), "supernode-max-work=" + std::to_string(maxExecution)};
        for (const auto& cluster : clusters) {
            const auto path = owner(*graph, nodes[cluster.front()].front());
            stats.push_back("supernode-nodes=" + path[2] + ":" + std::to_string(cluster.size()));
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
        verifyPartitionResult(*graph);
        logInfo("activity_nodes=" + std::to_string(nodes.size()) + " supernodes=" + std::to_string(clusters.size()) +
                " hint-analysis-us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - analysisStarted).count()));
    } catch (const std::exception& exception) {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("nonsink-partitioned-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{}); result.failed = true;
    }
    return result;
}

}
