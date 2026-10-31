#include "transform/grhsim/node-cse.hpp"

#include "rewrite.hpp"
#include "node-cse-internal.hpp"
#include "transform/verify.hpp"

#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
namespace node = ingest::node;
constexpr std::string_view policy = "gsim-soft-v2";

// Hashes only index buckets. Comparing the complete local signature (whose
// children are already exact interned terms) is the equivalence proof.
struct Candidate {
    node::NodeRecord record;
    std::unordered_set<OperationId, OperationIdHash> members, region;
    std::set<std::size_t> dependencies;
    std::set<std::string> successors;
    std::unordered_map<ValueId, std::size_t, ValueIdHash> expressions;
    std::string reason;
    ValueId root;
    std::size_t term = 0;
    bool input = false;
};

struct Analysis {
    Graph& graph;
    detail::NodeTerms terms;
    std::vector<Candidate> nodes;
    std::unordered_map<ValueId, std::vector<std::size_t>, ValueIdHash> roots;
    std::unordered_map<ValueId, std::vector<std::string>, ValueIdHash> outputs;
    std::vector<std::size_t> order;
    std::map<std::string, std::size_t> rejected;

    explicit Analysis(Graph& graph) : graph(graph) {
        for (const auto& port : graph.outputPorts()) outputs[port.value].push_back(port.name);
        const auto inspection = node::inspect(graph);
        if (!inspection.errors.empty()) throw std::invalid_argument(inspection.errors.front());
        for (const auto& record : inspection.nodes) {
            const auto i = nodes.size();
            Candidate candidate; candidate.record = record;
            candidate.members.insert(record.members.begin(), record.members.end());
            if (record.roots.size() == 1) {
                candidate.root = record.roots.front().value; roots[candidate.root].push_back(i);
            } else candidate.reason = record.roots.empty() ? "dead" : "multiple-roots";
            nodes.push_back(std::move(candidate));
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) collect(i);
        std::vector<std::size_t> pending(nodes.size());
        std::vector<std::vector<std::size_t>> users(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            pending[i] = nodes[i].dependencies.size();
            for (const auto dependency : nodes[i].dependencies) users[dependency].push_back(i);
            if (!pending[i]) order.push_back(i);
        }
        for (std::size_t j = 0; j < order.size(); ++j)
            for (const auto user : users[order[j]]) if (!--pending[user]) order.push_back(user);
        // Optional, overlapping associations need not form a node DAG. Reject
        // those candidates, never the executable op graph or its simplification.
        if (order.size() != nodes.size())
            for (std::size_t i = 0; i < nodes.size(); ++i) if (pending[i]) {
                nodes[i].reason = "overlapping-dependencies"; order.push_back(i);
            }
        for (const auto i : order) build(i);
        successors();
    }

    std::optional<std::size_t> otherRoot(ValueId value, std::size_t current) const {
        const auto found = roots.find(value);
        if (found == roots.end()) return {};
        for (const auto i : found->second) if (i != current) return i;
        return {};
    }
    bool contains(const Candidate& candidate, OperationId op) const {
        return op && candidate.members.contains(op);
    }
    void collect(std::size_t index) {
        auto& candidate = nodes[index];
        if (!candidate.root) return;
        const auto type = graph.valueLogicalType(candidate.root);
        if (type.kind != ValueType::Logic || !type.shape.empty()) candidate.reason = "array-or-nonlogic";
        std::unordered_set<ValueId, ValueIdHash> seen;
        std::vector<ValueId> work{candidate.root};
        while (!work.empty()) {
            const auto value = work.back(); work.pop_back();
            if (!seen.insert(value).second) continue;
            if (value != candidate.root) if (const auto dependency = otherRoot(value, index)) {
                candidate.dependencies.insert(*dependency); candidate.input = true; continue;
            }
            const auto op = graph.valueDef(value);
            if (!op || (!contains(candidate, op) && graph.opKind(op) != OperationKind::kConstant)) {
                if (value == candidate.root) candidate.reason = "incomplete-region";
                candidate.input = true; continue;
            }
            candidate.region.insert(op);
            if (graph.opResults(op).size() != 1) candidate.reason = "multiple-results";
            if (operationEffect(graph, op) != OpEffect::Pure || !canCseOperation(graph, op))
                candidate.reason = "state-or-effect";
            if (!detail::owner(graph, op).empty()) candidate.reason = "execution-owner";
            const auto valueType = graph.valueLogicalType(value);
            if (valueType.kind != ValueType::Logic || !valueType.shape.empty()) candidate.reason = "array-or-nonlogic";
            for (const auto argument : graph.opOperands(op)) work.push_back(argument);
        }
        // Only complete expressions can be shared. A private internal value
        // escaping the result boundary is not an independently shareable node.
        for (const auto op : candidate.region) {
            if (graph.opKind(op) == OperationKind::kConstant) continue;
            for (const auto value : graph.opResults(op)) {
                if (value == candidate.root) continue;
                if (candidate.reason.empty() && (graph.valueIsOutput(value) || graph.valueIsInout(value))) candidate.reason = "incomplete-region";
                for (const auto& use : graph.valueUsers(value))
                    if (candidate.reason.empty() && !candidate.region.contains(use.operation)) candidate.reason = "incomplete-region";
            }
        }
        if (!candidate.input && candidate.reason.empty()) candidate.reason = "constant";
    }

    std::size_t boundary(ValueId value) {
        std::ostringstream out; out << "boundary:" << value.index << ':' << value.generation << ';';
        detail::encode(out, graph.valueLogicalType(value)); return terms.intern(out.str());
    }
    void build(std::size_t index) {
        auto& candidate = nodes[index];
        if (!candidate.root) { ++rejected[candidate.reason]; return; }
        if (!candidate.reason.empty()) {
            candidate.term = boundary(candidate.root); ++rejected[candidate.reason]; return;
        }
        struct Frame { ValueId value; bool expanded; };
        std::vector<Frame> work{{candidate.root, false}};
        std::unordered_set<ValueId, ValueIdHash> active;
        while (!work.empty()) {
            const auto frame = work.back(); work.pop_back(); const auto value = frame.value;
            if (candidate.expressions.contains(value)) continue;
            if (value != candidate.root) if (const auto dependency = otherRoot(value, index)) {
                candidate.expressions[value] = nodes[*dependency].term; continue;
            }
            const auto op = graph.valueDef(value);
            if (!op || !candidate.region.contains(op) || operationEffect(graph, op) != OpEffect::Pure ||
                graph.opResults(op).size() != 1) {
                candidate.expressions[value] = boundary(value); continue;
            }
            if (!frame.expanded) {
                if (!active.insert(value).second) throw std::invalid_argument("Cyclic node expression");
                work.push_back({value, true});
                const auto arguments = graph.opOperands(op);
                for (auto at = arguments.rbegin(); at != arguments.rend(); ++at)
                    if (!candidate.expressions.contains(*at)) work.push_back({*at, false});
                continue;
            }
            active.erase(value);
            const auto arguments = graph.opOperands(op);
            if (graph.opKind(op) == OperationKind::kAssign && arguments.size() == 1 && graph.sameValueType(value, arguments[0])) {
                candidate.expressions[value] = candidate.expressions.at(arguments[0]); continue;
            }
            std::ostringstream out; out << "op:" << static_cast<unsigned>(graph.opKind(op)) << ';';
            detail::encode(out, graph.valueLogicalType(value));
            for (const auto argument : arguments) {
                detail::encode(out, graph.valueLogicalType(argument)); out << candidate.expressions.at(argument) << ';';
            }
            std::vector<std::pair<std::string, AttributeValue>> attributes;
            for (const auto& entry : graph.opAttrs(op))
                if (!node::isAttribute(entry.key) && entry.key != kPartition && entry.key != kOriginModulesKey && entry.key != "grhsim.nodeClone")
                    attributes.emplace_back(entry.key, entry.value);
            std::sort(attributes.begin(), attributes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [key, attribute] : attributes) { out << key.size() << ':' << key; detail::encode(out, attribute); }
            candidate.expressions[value] = terms.intern(out.str());
        }
        candidate.term = candidate.reason.empty() ? candidate.expressions.at(candidate.root) : boundary(candidate.root);
        if (!candidate.reason.empty()) ++rejected[candidate.reason];
    }

    void successors() {
        for (auto& candidate : nodes) {
            if (!candidate.root) continue;
            std::unordered_set<ValueId, ValueIdHash> seen;
            std::vector<ValueId> work{candidate.root};
            while (!work.empty()) {
                const auto value = work.back(); work.pop_back();
                if (!seen.insert(value).second) continue;
                if (const auto found = outputs.find(value); found != outputs.end())
                    for (const auto& name : found->second) candidate.successors.insert("output:" + name);
                for (const auto& use : graph.valueUsers(value)) {
                    const auto attrs = node::read(graph, use.operation);
                    bool nodeUse = false;
                    for (const auto& member : attrs.members) if (member != candidate.record.key) {
                        candidate.successors.insert("node:" + member); nodeUse = true;
                    }
                    if (nodeUse) continue;
                    if (operationEffect(graph, use.operation) == OpEffect::Pure && attrs.members.empty()) {
                        for (const auto result : graph.opResults(use.operation)) work.push_back(result);
                    } else if (!candidate.region.contains(use.operation))
                        candidate.successors.insert("terminal:" + std::to_string(use.operation.index));
                }
            }
        }
    }
};

void merge(Analysis& analysis, Candidate& target, Candidate& source) {
    auto& graph = analysis.graph;
    std::map<std::pair<std::size_t, OperationKind>, OperationId> representatives;
    // Choose deterministic local representatives without rescanning the graph
    // once per merged source node.
    std::vector<OperationId> ordered(target.region.begin(), target.region.end());
    std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a.index < b.index; });
    for (const auto op : ordered) {
        const auto result = graph.opResults(op);
        if (result.size() == 1 && target.expressions.contains(result[0]))
            representatives.try_emplace(std::make_pair(target.expressions.at(result[0]), graph.opKind(op)), op);
    }
    const auto targetRoot = graph.valueDef(target.root), sourceRoot = graph.valueDef(source.root);
    // The source root can be an identity carrier after local simplification.
    // Its root identity still belongs to the representative's complete result.
    node::mergeEquivalentOperations(graph, targetRoot, sourceRoot, std::array<int64_t, 1>{0});
    for (const auto op : source.region) {
        if (op == sourceRoot) continue;
        const auto values = graph.opResults(op);
        if (values.size() != 1 || !source.expressions.contains(values[0])) continue;
        const auto found = representatives.find({source.expressions.at(values[0]), graph.opKind(op)});
        if (found == representatives.end()) continue;
        node::mergeEquivalentOperations(graph, found->second, op, std::array<int64_t, 1>{0});
    }
    graph.replaceAllUses(source.root, target.root);
    if (const auto found = analysis.outputs.find(source.root); found != analysis.outputs.end()) {
        const auto names = found->second;
        for (const auto& name : names) graph.bindOutputPort(name, target.root);
        auto& namesAtTarget = analysis.outputs[target.root];
        namesAtTarget.insert(namesAtTarget.end(), names.begin(), names.end());
        analysis.outputs.erase(source.root);
    }
    // Keep a named declaration as a view instead of erasing an observation.
    bool anchored = false;
    for (const auto symbol : graph.declaredSymbols()) anchored |= symbol == graph.valueSymbol(source.root) || symbol == graph.operationSymbol(sourceRoot);
    for (const auto& group : graph.generateGroups()) for (const auto symbol : group.symbols)
        anchored |= symbol == graph.valueSymbol(source.root) || symbol == graph.operationSymbol(sourceRoot);
    if (anchored) {
        detail::rewrite(graph, sourceRoot, OperationKind::kAssign, std::array{target.root});
        node::write(graph, sourceRoot, {});
    }
}
} // namespace

void recordNodeRetention(grh::Graph& graph) {
    node::recordRetention(graph);
}

NodeCsePass::NodeCsePass(NodeCseOptions options)
    : Pass("grhsim-node-cse", "grhsim-node-cse", "GSIM whole-node equivalence and thresholded sharing"), options_(std::move(options)) {}
bool NodeCsePass::supportsDialect(std::string_view dialect) const noexcept { return detail::supports(dialect); }

PassResult NodeCsePass::run() {
    PassResult result;
    try {
        if (options_.mode != "gsim" && options_.mode != "off") throw std::invalid_argument("firrtl-node-cse must be off or gsim");
        auto& graph = detail::selectGraph(design(), options_.top);
        node::requirePolicy(graph);
        const std::string config = "v2/" + options_.mode + "/5/256/1";
        if (const auto stored = graph.metadata("firrtl.nodeCseConfig")) {
            if (*stored != AttributeValue(config)) throw std::invalid_argument("FIRRTL node CSE checkpoint configuration differs");
            if (graph.metadata("firrtl.nodeOptimization") != std::optional<AttributeValue>(std::string(policy)))
                throw std::invalid_argument("FIRRTL node CSE checkpoint revision differs");
            node::requireValid(graph); recordNodeRetention(graph); return result;
        }
        if (graph.metadata(kStage) != std::optional<AttributeValue>(std::string("converted")))
            throw std::invalid_argument("Whole-node CSE requires an unpartitioned converted checkpoint");
        std::ofstream report;
        if (!options_.report.empty()) {
            report.open(options_.report);
            if (!report) throw std::invalid_argument("Cannot write node CSE report");
        }
        PassManager verifier; verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) return {false, true, {}};
        Analysis analysis(graph);
        std::map<std::size_t, std::vector<std::size_t>> groups;
        for (const auto i : analysis.order) if (analysis.nodes[i].reason.empty()) groups[analysis.nodes[i].term].push_back(i);
        std::size_t equivalents = 0, merged = 0, retained = 0, byCount = 0, byWidth = 0, byFanout = 0;
        for (const auto& [term, group] : groups) {
            if (group.size() < 2) continue;
            equivalents += group.size();
            const bool count = group.size() >= 5;
            const bool width = graph.valueWidth(analysis.nodes[group.front()].root) > 256;
            const bool fanout = std::any_of(group.begin(), group.end(), [&](auto i) { return analysis.nodes[i].successors.size() > 1; });
            if (options_.mode == "off" || (!count && !width && !fanout)) { retained += group.size(); continue; }
            byCount += count; byWidth += width; byFanout += fanout;
            auto& target = analysis.nodes[group.front()];
            for (std::size_t j = 1; j < group.size(); ++j) {
                auto& source = analysis.nodes[group[j]];
                // Ordinary CSE may already have shared these source results.
                if (source.root == target.root) continue;
                merge(analysis, target, source); ++merged;
            }
        }
        if (merged) { detail::Scope scope(graph, {}); scope.dce(); result.changed = true; }
        std::vector<std::string> stats{"v1", "nodes=" + std::to_string(analysis.nodes.size()), "equivalent=" + std::to_string(equivalents),
            "merged=" + std::to_string(merged), "retained=" + std::to_string(retained), "count-groups=" + std::to_string(byCount),
            "width-groups=" + std::to_string(byWidth), "fanout-groups=" + std::to_string(byFanout)};
        for (const auto& [reason, count] : analysis.rejected) stats.push_back("skip-" + reason + "=" + std::to_string(count));
        node::requireValid(graph);
        graph.setMetadata("firrtl.nodeOptimization", std::string(policy)); graph.setMetadata("firrtl.nodeCseConfig", config);
        graph.setMetadata("firrtl.nodeCseStats", stats); recordNodeRetention(graph);
        result.changed = true;
        if (report.is_open()) {
            for (const auto& line : stats) report << line << '\n';
            report << node::inspect(graph).report(graph);
            if (!report) throw std::invalid_argument("Cannot write node CSE report");
        }
        if (!verifier.run(design(), diags()).success) result.failed = true;
        logInfo("equivalent=" + std::to_string(equivalents) + " merged=" + std::to_string(merged) + " retained=" + std::to_string(retained));
    } catch (const std::exception& failure) { error(failure.what()); result.failed = true; }
    return result;
}
} // namespace wolvrix::lib::transform::grhsim
