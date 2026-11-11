#ifndef WOLVRIX_TRANSFORM_GRHSIM_PARTITION_INTERNAL_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_PARTITION_INTERNAL_HPP

#include "rewrite.hpp"
#include "transform/verify.hpp"

#include <functional>
#include <map>
#include <queue>

namespace wolvrix::lib::transform::grhsim::detail {

inline constexpr std::string_view kStatePolicy = "grhsim.statePolicy";
inline constexpr std::string_view kOutputEnd = "grhsim.outputEnd";
inline constexpr std::string_view kTimeslot = "grhsim.timeslot";
inline constexpr std::string_view kSampleBoundary = "grhsim.sampleBoundary";

inline bool declaration(const Graph& graph, OperationId op) {
    return operationEffect(graph, op) == OpEffect::Declaration;
}

inline std::string stage(const Graph& graph) {
    return std::get<std::string>(graph.metadata(kStage).value());
}

inline Graph& partitionGraph(Design& design, std::string_view top) {
    Graph& graph = selectGraph(design, top);
    PassManager verifier;
    verifier.addPass(std::make_unique<GrhVerifyPass>());
    PassDiagnostics diagnostics;
    if (!verifier.run(design, diagnostics).success) {
        std::string message = "Invalid input graph";
        for (const auto& diagnostic : diagnostics.messages()) message += "; " + diagnostic.message;
        throw std::invalid_argument(message);
    }
    return graph;
}

inline void verifyPartitionResult(const Graph& graph) {
    if (const auto problem = verifyGraph(graph); !problem.empty()) throw std::logic_error(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty()) throw std::logic_error(problem);
}

// IDs are local tie breakers only; persisted keys are independent text attrs.
inline std::vector<OperationId> topological(const Graph& graph,
                                           const std::function<bool(OperationId)>& include,
                                           const std::function<uint64_t(OperationId)>& priority = {}) {
    const auto operations = graph.operations();
    uint32_t last = 0;
    for (const auto op : operations) last = std::max(last, op.index);
    // Local read-only tables: no IDs survive a mutation or freeze. Payload
    // edges are already represented by the graph's borrowed use-lists.
    std::vector<uint8_t> included(static_cast<std::size_t>(last) + 1);
    std::vector<std::size_t> degree(included.size());
    std::vector<OperationId> effectNext(included.size());
    std::vector<uint64_t> priorities(priority ? included.size() : 0);
    std::size_t count = 0;
    for (const auto op : operations) if (include(op)) {
        included[op.index] = 1; ++count;
        if (priority) priorities[op.index] = priority(op);
    }
    for (const auto op : operations) if (included[op.index])
        for (const auto value : graph.opOperands(op)) {
            const auto producer = graph.valueDef(value);
            if (producer && included.at(producer.index)) ++degree[op.index];
        }
    std::map<int64_t, OperationId> effects;
    for (const auto op : operations) if (included[op.index])
        if (const auto order = attribute<int64_t>(graph, op, "grhsim.effectOrder")) {
            if (*order < 0 || !effects.emplace(*order, op).second)
                throw std::invalid_argument("Invalid or duplicate effect order");
        }
    OperationId previous;
    for (const auto& [order, op] : effects) {
        if (previous) { ++degree[op.index]; effectNext[previous.index] = op; }
        previous = op;
    }
    const auto compare = [&](OperationId left, OperationId right) {
        if (priority && priorities[left.index] != priorities[right.index]) return priorities[left.index] > priorities[right.index];
        return left.index > right.index;
    };
    std::priority_queue<OperationId, std::vector<OperationId>, decltype(compare)> ready(compare);
    for (const auto op : operations) if (included[op.index] && degree[op.index] == 0) ready.push(op);
    std::vector<OperationId> result;
    result.reserve(count);
    while (!ready.empty()) {
        const auto op = ready.top(); ready.pop(); result.push_back(op);
        for (const auto value : graph.opResults(op))
            for (const auto& user : graph.valueUsers(value))
                if (included.at(user.operation.index) && --degree[user.operation.index] == 0) ready.push(user.operation);
        if (const auto next = effectNext[op.index]; next && --degree[next.index] == 0) ready.push(next);
    }
    if (result.size() != count) throw std::invalid_argument("Combinational cycle in simulation graph");
    return result;
}

inline OperationId cloneOperation(Graph& graph, OperationId source,
                                  std::span<const ValueId> operands, const Owner& destination) {
    const Operation old = graph.getOperation(source);
    const auto op = graph.createOperation(old.kind());
    for (const auto& attr : old.attrs()) graph.setAttr(op, attr.key, attr.value);
    graph.mergeOpSemanticIds(op, source);
    graph.setAttr(op, kPartition, destination);
    if (ingest::node::annotated(graph, source)) graph.setAttr(op, "grhsim.nodeClone", true);
    if (old.srcLoc()) graph.setOpSrcLoc(op, *old.srcLoc());
    for (const auto value : operands) graph.addOperand(op, value);
    for (const auto value : old.results()) {
        const auto result = graph.createValue(graph.valueLogicalType(value));
        graph.addValueSemanticIds(result, graph.valueSemanticIds(value));
        if (const auto location = graph.getValue(value).srcLoc()) graph.setValueSrcLoc(result, *location);
        graph.addResult(op, result);
    }
    return op;
}

inline bool outputEnd(const Graph& graph, OperationId op) {
    return attribute<bool>(graph, op, kOutputEnd).value_or(false);
}

inline std::vector<OperationId> orderedWriters(const Graph& graph, bool includeOutput = false) {
    std::vector<OperationId> result;
    for (const auto op : graph.operations())
        if (isWriter(graph.opKind(op)) && (includeOutput || !outputEnd(graph, op))) result.push_back(op);
    const auto original = result;
    std::map<std::string, std::vector<std::size_t>> byState;
    for (std::size_t i = 0; i < result.size(); ++i)
        byState[std::string(graph.getOperation(writerTarget(graph, result[i])).symbolText())].push_back(i);
    for (const auto& [target, positions] : byState) {
        auto ordered = positions;
        std::sort(ordered.begin(), ordered.end(), [&](const std::size_t lhs, const std::size_t rhs) {
            return attribute<int64_t>(graph, original[lhs], "writerOrder").value() <
                attribute<int64_t>(graph, original[rhs], "writerOrder").value();
        });
        for (std::size_t i = 0; i < positions.size(); ++i)
            result[positions[i]] = original[ordered[i]];
    }
    return result;
}

inline uint64_t configurationHash(std::string_view text) {
    uint64_t value = 14695981039346656037ull;
    for (const unsigned char character : text) { value ^= character; value *= 1099511628211ull; }
    return value;
}

}
#endif
