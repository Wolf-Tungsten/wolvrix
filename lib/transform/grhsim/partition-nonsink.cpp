#include "transform/grhsim/partition-nonsink.hpp"

#include "partition-internal.hpp"

#include <numeric>
#include <tuple>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;
using Clusters = std::vector<std::vector<uint32_t>>;
constexpr uint32_t absent = UINT32_MAX;

uint64_t addCost(uint64_t left, uint64_t right) {
    if (right > UINT64_MAX - left) throw std::invalid_argument("Activity partition cost overflows uint64");
    return left + right;
}

struct Edge {
    uint32_t source, target;
    ValueId value;
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
            if (source != target) edges.push_back({source, target, edge.value});
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

bool coarsen(Clusters& clusters, std::span<const Edge> edges, const std::vector<uint64_t>& sizes,
             uint64_t limit, unsigned mode) {
    const Quotient graph(clusters, edges, sizes.size());
    std::vector<uint32_t> parents(clusters.size());
    std::iota(parents.begin(), parents.end(), 0);
    std::vector<uint64_t> weights(clusters.size());
    for (uint32_t i = 0; i < clusters.size(); ++i) for (const auto node : clusters[i]) weights[i] += sizes[node];
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
    if (!orderClusters(result, edges, sizes.size())) return false;
    clusters = std::move(result);
    return true;
}

// Same objective as the production activity cutter: distinct incoming values
// weighted by firing rate, plus a per-supernode penalty. Work caps control
// activation granularity; source lines and translation units are irrelevant.
Clusters segment(const Graph& graph, const Clusters& clusters, std::span<const Edge> edges,
                 const std::vector<uint64_t>& sizes, uint64_t limit, uint64_t penalty) {
    const Quotient quotient(clusters, edges, sizes.size());
    std::vector<std::vector<ValueId>> incoming(clusters.size()), outgoing(clusters.size());
    uint32_t lastValue = 0;
    for (const auto& edge : quotient.edges) lastValue = std::max(lastValue, edge.value.index);
    // This graph is immutable throughout segmentation. Local dense stamps
    // avoid allocating two hash sets for every candidate interval.
    struct BoundaryValue {
        uint64_t rate = 0;
        uint32_t source = absent, seen = 0, counted = 0;
    };
    std::vector<BoundaryValue> values(static_cast<std::size_t>(lastValue) + 1);
    for (const auto& edge : quotient.edges) {
        if (!edge.value) continue; // Ordering only; no activation payload.
        incoming[edge.target].push_back(edge.value);
        if (edge.source != absent) outgoing[edge.source].push_back(edge.value);
        values[edge.value.index].source = edge.source;
    }
    std::vector<uint64_t> prefix(clusters.size() + 1), cost(clusters.size() + 1, UINT64_MAX);
    std::vector<uint32_t> previous(clusters.size() + 1);
    for (uint32_t i = 0; i < clusters.size(); ++i) {
        prefix[i + 1] = prefix[i];
        for (const auto node : clusters[i]) prefix[i + 1] += sizes[node];
    }
    cost[0] = 0;
    for (uint32_t end = 1; end <= clusters.size(); ++end) {
        uint64_t boundary = 0;
        const auto rate = [&](ValueId value) {
            auto& cached = values[value.index].rate;
            if (cached) return cached;
            const auto op = graph.valueDef(value);
            const auto weight = op ? attribute<int64_t>(graph, op, "grhsim.activity").value_or(1) : 1;
            if (weight < 0) throw std::invalid_argument("Negative activity rate");
            return cached = uint64_t(std::max<int64_t>(1, weight));
        };
        for (uint32_t begin = end; begin > 0;) {
            --begin;
            if (prefix[end] - prefix[begin] > limit && begin + 1 != end) break;
            for (const auto value : incoming[begin]) {
                auto& entry = values[value.index];
                if (entry.seen == end) continue;
                entry.seen = end;
                if (entry.source == absent || entry.source < begin) {
                    entry.counted = end; boundary = addCost(boundary, rate(value));
                }
            }
            for (const auto value : outgoing[begin]) if (values[value.index].counted == end) {
                values[value.index].counted = 0; boundary -= rate(value);
            }
            const auto candidate = addCost(addCost(cost[begin], boundary), penalty);
            if (candidate <= cost[end]) { cost[end] = candidate; previous[end] = begin; }
            if (prefix[end] - prefix[begin] > limit) break; // An indivisible oversized node.
        }
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

std::string config(const PartitionNonsinkOptions& options) {
    return std::to_string(options.maxNodeWork) + "/" + std::to_string(options.maxSupernodeWork) + "/" +
        std::to_string(options.segmentPenalty) + "/" + std::to_string(options.semanticNodes) + "/" +
        std::to_string(options.cloneShared) + "/" + std::to_string(options.cloneMaxFanout) + "/" +
        std::to_string(options.maxClones) + "/" + std::to_string(options.cloneLossyCompare);
}

void cloneBoundaries(Graph& graph, const PartitionNonsinkOptions& options,
                     std::span<const OperationId> originals) {
    if (!options.cloneShared || options.maxClones == 0) return;
    const auto nonsink = [&](const Owner& path) { return path.size() >= 4 && path[0] == "general" && path[1] == "nonsink"; };
    std::unordered_set<ValueId, ValueIdHash> boundaries;
    for (const auto op : originals) for (const auto value : graph.opResults(op)) {
        for (const auto& user : graph.valueUsers(value))
            if (ownerView(graph, user.operation) != ownerView(graph, op)) boundaries.insert(value);
    }
    uint64_t clones = 0;
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
            if (clones >= options.maxClones) return;
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
            ++clones;
        }
    }
}
}

PartitionNonsinkPass::PartitionNonsinkPass(PartitionNonsinkOptions options)
    : Pass("grhsim-partition-nonsink", "grhsim-partition-nonsink", "Partition nonsink activity on GRH"), options_(std::move(options)) {}

bool PartitionNonsinkPass::supportsDialect(std::string_view dialect) const noexcept { return supports(dialect); }

PassResult PartitionNonsinkPass::run() {
    PassResult result;
    Graph* graph = nullptr;
    try {
        if (!options_.maxNodeWork || !options_.maxSupernodeWork || !options_.cloneMaxFanout)
            throw std::invalid_argument("Activity work/fanout limits must be positive");
        graph = &partitionGraph(design(), options_.top);
        const auto configuration = config(options_);
        if (stage(*graph) == "nonsink-partitioned") {
            if (graph->metadata("grhsim.nonsinkConfig") != std::optional<AttributeValue>(configuration))
                throw std::invalid_argument("Changed activity configuration requires the nonsink-sink-split checkpoint");
            return result;
        }
        if (stage(*graph) != "nonsink-sink-split") throw std::invalid_argument("Activity partitioning requires nonsink-sink-split stage");
        const Owner nonsinkOwner{"general", "nonsink"};
        const auto topo = topological(*graph, [&](OperationId op) { return ownerView(*graph, op) == nonsinkOwner; });
        std::unordered_map<OperationId, uint32_t, OperationIdHash> nodeOf;
        std::vector<uint64_t> sizes;
        std::unordered_set<uint32_t> anchored;
        for (const auto symbol : graph->declaredSymbols()) anchored.insert(symbol.value);
        for (const auto& group : graph->generateGroups()) for (const auto symbol : group.symbols) anchored.insert(symbol.value);
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            const auto op = *it;
            if (attribute<int64_t>(*graph, op, "grhsim.activity").value_or(0) < 0)
                throw std::invalid_argument("Negative activity rate");
            bool absorb = canCloneOperation(*graph, op) &&
                !(options_.semanticNodes && anchored.contains(graph->operationSymbol(op).value));
            uint32_t target = absent;
            for (const auto value : graph->opResults(op)) {
                if (options_.semanticNodes && anchored.contains(graph->valueSymbol(value).value)) absorb = false;
                for (const auto& use : graph->valueUsers(value)) {
                    const auto found = nodeOf.find(use.operation);
                    if (found == nodeOf.end()) { absorb = false; continue; }
                    if (target == absent) target = found->second;
                    else if (target != found->second) absorb = false;
                }
            }
            const auto weight = work(*graph, op);
            if (!absorb || target == absent || sizes[target] + weight > options_.maxNodeWork) {
                target = sizes.size(); sizes.push_back(0);
            }
            nodeOf[op] = target; sizes[target] += weight;
        }
        std::vector<std::vector<OperationId>> nodes(sizes.size());
        for (const auto op : topo) nodes[nodeOf.at(op)].push_back(op);
        std::vector<Edge> edges;
        for (const auto op : topo) for (const auto input : graph->opOperands(op)) {
            const auto found = nodeOf.find(graph->valueDef(input));
            if (found == nodeOf.end()) edges.push_back({absent, nodeOf.at(op), input});
            else if (found->second != nodeOf.at(op)) edges.push_back({found->second, nodeOf.at(op), input});
        }
        std::map<int64_t, OperationId> effects;
        for (const auto op : topo)
            if (const auto order = attribute<int64_t>(*graph, op, "grhsim.effectOrder")) effects.emplace(*order, op);
        OperationId previous;
        for (const auto& [order, op] : effects) {
            if (previous && nodeOf.at(previous) != nodeOf.at(op)) edges.push_back({nodeOf.at(previous), nodeOf.at(op), {}});
            previous = op;
        }
        Clusters clusters(nodes.size());
        for (uint32_t i = 0; i < nodes.size(); ++i) clusters[i] = {i};
        if (!orderClusters(clusters, edges, nodes.size())) throw std::invalid_argument("Node contraction introduced a cycle");
        while (!clusters.empty()) {
            const auto before = clusters.size();
            for (unsigned mode = 0; mode < 3; ++mode) coarsen(clusters, edges, sizes, options_.maxSupernodeWork, mode);
            if (clusters.size() == before) break;
        }
        clusters = segment(*graph, clusters, edges, sizes, options_.maxSupernodeWork, options_.segmentPenalty);
        result.changed = true;
        for (const auto& cluster : clusters) {
            const auto anchor = nodes[cluster.front()].front();
            const std::string key = "sn_" + std::string(graph->symbolText(graph->operationSymbol(anchor)));
            for (const auto node : cluster) {
                const std::string nodeKey = "node_" + std::string(graph->symbolText(graph->operationSymbol(nodes[node].front())));
                for (const auto op : nodes[node]) graph->setAttr(op, kPartition, Owner{"general", "nonsink", key, nodeKey});
            }
        }
        cloneBoundaries(*graph, options_, topo);
        graph->setMetadata("grhsim.nonsinkConfig", configuration);
        graph->setMetadata(kStage, std::string("nonsink-partitioned"));
        verifyPartitionResult(*graph);
        logInfo("activity_nodes=" + std::to_string(nodes.size()) + " supernodes=" + std::to_string(clusters.size()));
    } catch (const std::exception& exception) {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("nonsink-partitioned-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{}); result.failed = true;
    }
    return result;
}

}
