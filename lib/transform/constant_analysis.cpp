#include "transform/constant_analysis.hpp"

#include "core/op_descriptor.hpp"
#include "logic_eval.hpp"
#include "transform/grhsim/ops.hpp"
#include "grhsim/rewrite.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wolvrix::lib::transform {
namespace {

using namespace grh;

void requireWholeGrh(Graph& graph)
{
    if (graph.metadata(grhsim::kStage))
        throw std::invalid_argument("constant-analysis requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations()) {
        if (grhsim::attribute<std::vector<std::string>>(graph, op, grhsim::kPartition))
            throw std::invalid_argument("constant-analysis requires an unpartitioned GRH graph");
    }
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = grhsim::verifyGraph(graph); !problem.empty())
        throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

bool isFoldable(OperationKind kind)
{
    switch (kind) {
    case OperationKind::kConstant:
    case OperationKind::kAdd: case OperationKind::kSub: case OperationKind::kMul:
    case OperationKind::kDiv: case OperationKind::kMod:
    case OperationKind::kEq: case OperationKind::kNe: case OperationKind::kCaseEq:
    case OperationKind::kCaseNe: case OperationKind::kWildcardEq: case OperationKind::kWildcardNe:
    case OperationKind::kLt: case OperationKind::kLe: case OperationKind::kGt: case OperationKind::kGe:
    case OperationKind::kAnd: case OperationKind::kOr: case OperationKind::kXor:
    case OperationKind::kXnor: case OperationKind::kNot:
    case OperationKind::kLogicAnd: case OperationKind::kLogicOr: case OperationKind::kLogicNot:
    case OperationKind::kReduceAnd: case OperationKind::kReduceOr: case OperationKind::kReduceXor:
    case OperationKind::kReduceNor: case OperationKind::kReduceNand: case OperationKind::kReduceXnor:
    case OperationKind::kShl: case OperationKind::kLShr: case OperationKind::kAShr:
    case OperationKind::kMux: case OperationKind::kAssign: case OperationKind::kConcat:
    case OperationKind::kArrayCreate: case OperationKind::kReplicate:
    case OperationKind::kSliceStatic: case OperationKind::kSliceDynamic: case OperationKind::kSliceArray:
        return true;
    default:
        return false;
    }
}

bool isPureSystemFunction(const Graph& graph, OperationId op)
{
    return graph.opKind(op) == OperationKind::kSystemFunction &&
        grhsim::systemFunctionEffect(graph, op) == "pure";
}

bool makeConstant(Graph& graph, OperationId op, slang::SVInt bits)
{
    const auto results = graph.opResults(op);
    if (results.size() != 1 || graph.valueType(results[0]) != ValueType::Logic) return false;
    const auto result = results[0];
    // The folded producer keeps its operation and result identity, but its
    // constant operands can be reclaimed by the explicit DCE stage that
    // follows this pass.  Carry their source identities to the surviving
    // producer before dropping the operand edges so SemanticTree provenance
    // remains attached to the folded expression.
    // Semantic-ID merging mutates graph storage; snapshot operands before
    // editing so no borrowed operand span survives that mutation.
    const std::vector<ValueId> operands(graph.opOperands(op).begin(), graph.opOperands(op).end());
    for (const auto operand : operands) {
        graph.mergeValueSemanticIds(result, operand);
        if (const auto producer = graph.valueDef(operand)) graph.mergeOpSemanticIds(op, producer);
    }
    bits.setSigned(graph.valueSigned(result));
    bits = bits.resize(graph.valueWidth(result));
    if (graph.valueDomain(result) == LogicDomain::TwoState) bits.flattenUnknowns();
    grhsim::detail::rewrite(graph, op, OperationKind::kConstant, {});
    graph.setAttr(op, "constValue", bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth()));
    return true;
}

std::vector<OperationId> topologicalOrder(const Graph& graph)
{
    const auto operations = graph.operations();
    uint32_t lastIndex = 0;
    for (const auto op : operations) lastIndex = std::max(lastIndex, op.index);
    std::vector<std::size_t> pendingInputs(static_cast<std::size_t>(lastIndex) + 1);
    std::vector<OperationId> order;
    order.reserve(operations.size());
    for (const auto op : operations) {
        auto& count = pendingInputs[op.index];
        // Count operand uses, not unique producers: valueUsers enumerates
        // exactly those uses, including repeated operands and distinct
        // results from the same producer, during the matching decrement.
        for (const auto arg : graph.opOperands(op))
            if (graph.valueDef(arg)) ++count;
        if (count == 0) order.push_back(op);
    }
    // The result also serves as the FIFO. A node reaches zero exactly once,
    // so no per-operation seen set or per-edge allocation is necessary.
    for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
        const auto op = order[cursor];
        for (const auto result : graph.opResults(op))
            for (const auto user : graph.valueUsers(result)) {
                auto& count = pendingInputs[user.operation.index];
                if (--count == 0) order.push_back(user.operation);
            }
    }
    if (order.size() != operations.size())
        throw std::invalid_argument("constant-analysis requires an acyclic expanded graph");
    return order;
}

std::size_t analyze(Graph& graph)
{
    std::size_t folded = 0;
    const auto order = topologicalOrder(graph);
    for (const auto op : order) {
        const auto kind = graph.opKind(op);
        const bool pureSystemFunction = isPureSystemFunction(graph, op);
        if (!op || kind == OperationKind::kConstant || (!isFoldable(kind) && !pureSystemFunction) ||
            (operationEffect(graph, op) != OpEffect::Pure && !pureSystemFunction) || graph.opResults(op).size() != 1)
            continue;
        const auto result = graph.opResults(op)[0];
        if (graph.valueType(result) != ValueType::Logic) continue;
        std::vector<slang::SVInt> operands;
        operands.reserve(graph.opOperands(op).size());
        bool allConstant = true;
        for (const auto arg : graph.opOperands(op)) {
            const auto bits = grhsim::detail::constantBits(graph, arg);
            if (!bits) { allConstant = false; break; }
            operands.push_back(*bits);
        }
        if (!allConstant) continue;
        if ((kind == OperationKind::kDiv || kind == OperationKind::kMod) &&
            operands.size() == 2 && grhsim::detail::isZero(operands[1])) continue;
        auto bits = detail::evaluateLogicOperation(graph, op, operands, kind);
        if (!bits) continue;
        if (!makeConstant(graph, op, *bits)) continue;
        ++folded;
    }
    return folded;
}

} // namespace

ConstantAnalysisPass::ConstantAnalysisPass(ConstantAnalysisOptions options)
    : Pass("constant-analysis", "constant-analysis", "GSIM-compatible constant propagation on ordinary GRH"),
      options_(std::move(options))
{}

bool ConstantAnalysisPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh";
}

PassResult ConstantAnalysisPass::run()
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
        const auto folded = analyze(graph);
        result.changed = folded != 0;
        verifyWhole(graph);
        const auto summary = "folded=" + std::to_string(folded) +
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
