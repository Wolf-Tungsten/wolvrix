#include "transform/pattern_detect.hpp"

#include "core/op_descriptor.hpp"
#include "grhsim/rewrite.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace wolvrix::lib::transform {
namespace {

using namespace grh;

void requireWholeGrh(Graph& graph)
{
    if (graph.metadata(grhsim::kStage))
        throw std::invalid_argument("pattern-detect requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations())
        if (grhsim::attribute<std::vector<std::string>>(graph, op, grhsim::kPartition))
            throw std::invalid_argument("pattern-detect requires an unpartitioned GRH graph");
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = grhsim::verifyGraph(graph); !problem.empty())
        throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

bool scalarLogic(const Graph& graph, ValueId value)
{
    const auto type = graph.valueLogicalType(value);
    return type.kind == ValueType::Logic && type.shape.empty() && type.width > 0;
}

std::optional<slang::SVInt> constantValue(const Graph& graph, ValueId value)
{
    return grhsim::detail::constantBits(graph, value);
}

ValueId makeConstant(Graph& graph, OperationId source, ValueId prototype, slang::SVInt bits)
{
    const auto type = graph.valueLogicalType(prototype);
    bits.setSigned(type.isSigned);
    bits = bits.resize(static_cast<slang::bitwidth_t>(type.width));
    if (type.domain == LogicDomain::TwoState) bits.flattenUnknowns();
    const auto op = graph.createOperation(OperationKind::kConstant);
    const auto value = graph.createValue(type);
    graph.addResult(op, value);
    graph.setAttr(op, "constValue",
                  bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth()));
    graph.mergeOpSemanticIds(op, source);
    graph.mergeValueSemanticIds(value, prototype);
    if (const auto location = graph.opSrcLoc(source)) graph.setOpSrcLoc(op, *location);
    if (const auto location = graph.valueSrcLoc(prototype)) graph.setValueSrcLoc(value, *location);
    return value;
}

// Rewrite (concat(lhs, rhs) == constant) into two narrower equality tests.
// The source operation remains in place, so callers can explicitly DCE the
// now-unused concat and generated intermediates at the next pipeline boundary.
bool splitConcatEquality(Graph& graph, OperationId op)
{
    if (graph.opKind(op) != OperationKind::kEq || operationEffect(graph, op) != OpEffect::Pure ||
        graph.opOperands(op).size() != 2 || graph.opResults(op).size() != 1)
        return false;
    const auto result = graph.opResults(op)[0];
    if (!scalarLogic(graph, result) || graph.valueWidth(result) != 1) return false;
    const auto lhs = graph.opOperands(op)[0], rhs = graph.opOperands(op)[1];
    const auto constant = constantValue(graph, rhs);
    if (!constant || constant->hasUnknown() || !scalarLogic(graph, lhs)) return false;
    const auto concat = graph.valueDef(lhs);
    if (!concat || graph.opKind(concat) != OperationKind::kConcat ||
        graph.opOperands(concat).size() != 2 || graph.opResults(concat).size() != 1)
        return false;
    const auto high = graph.opOperands(concat)[0], low = graph.opOperands(concat)[1];
    if (!scalarLogic(graph, high) || !scalarLogic(graph, low)) return false;
    const auto highWidth = graph.valueWidth(high), lowWidth = graph.valueWidth(low);
    if (highWidth <= 0 || lowWidth <= 0 || graph.valueWidth(lhs) != highWidth + lowWidth ||
        graph.valueWidth(rhs) != highWidth + lowWidth)
        return false;

    const auto highBits = constant->slice(static_cast<slang::bitwidth_t>(highWidth + lowWidth - 1),
                                          static_cast<slang::bitwidth_t>(lowWidth));
    const auto lowBits = constant->slice(static_cast<slang::bitwidth_t>(lowWidth - 1), 0);
    const auto highConstant = makeConstant(graph, op, high, highBits);
    const auto lowConstant = makeConstant(graph, op, low, lowBits);
    const auto eqType = graph.valueLogicalType(result);
    const auto highEq = grhsim::detail::makeOperation(graph, op, OperationKind::kEq, eqType,
                                                       std::array{high, highConstant}, "pattern-detect");
    const auto lowEq = grhsim::detail::makeOperation(graph, op, OperationKind::kEq, eqType,
                                                      std::array{low, lowConstant}, "pattern-detect");

    // The concat and original equality are represented by the surviving AND;
    // transfer their source identities before dropping the concat operand edge.
    grhsim::detail::mergeOrigins(graph, op, {concat});
    graph.mergeValueSemanticIds(result, lhs);
    graph.mergeValueSemanticIds(result, highEq);
    graph.mergeValueSemanticIds(result, lowEq);
    grhsim::detail::rewrite(graph, op, OperationKind::kAnd, std::array{highEq, lowEq});
    return true;
}

std::size_t analyze(Graph& graph)
{
    std::size_t changed = 0;
    const std::vector<OperationId> operations(graph.operations().begin(), graph.operations().end());
    for (const auto op : operations)
        if (op && splitConcatEquality(graph, op)) ++changed;
    return changed;
}

} // namespace

PatternDetectPass::PatternDetectPass(PatternDetectOptions options)
    : Pass("pattern-detect", "pattern-detect", "GSIM-compatible local expression pattern detection"),
      options_(std::move(options))
{}

bool PatternDetectPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh";
}

PassResult PatternDetectPass::run()
{
    PassResult result;
    try {
        Graph& graph = [&]() -> Graph& {
            if (options_.top.empty()) {
                if (design().topGraphs().size() != 1)
                    throw std::invalid_argument("Select exactly one top graph with -top");
                return *design().findGraph(design().topGraphs()[0]);
            }
            auto* selected = design().findGraph(options_.top);
            if (!selected) throw std::invalid_argument("Top graph does not exist: " + options_.top);
            return *selected;
        }();
        requireWholeGrh(graph);
        verifyWhole(graph);
        const auto patterns = analyze(graph);
        result.changed = patterns != 0;
        verifyWhole(graph);
        const auto summary = "concat-equality-patterns=" + std::to_string(patterns) +
            " operations=" + std::to_string(graph.operations().size());
        info(summary);
        logInfo(summary);
    } catch (const std::exception& error) {
        this->error(error.what());
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform
