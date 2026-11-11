#include "transform/alias_analysis.hpp"

#include "core/op_descriptor.hpp"
#include "ingest/node_attributes.hpp"
#include "transform/grhsim/ops.hpp"
#include "grhsim/rewrite.hpp"
#include "transform/verify.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform {
namespace {

using namespace grh;

void requireWholeGrh(Graph& graph)
{
    if (graph.metadata(grhsim::kStage))
        throw std::invalid_argument("alias-analysis requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations()) {
        if (grhsim::attribute<std::vector<std::string>>(graph, op, grhsim::kPartition))
            throw std::invalid_argument("alias-analysis requires an unpartitioned GRH graph");
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

bool scalarLogic(const Graph& graph, ValueId value)
{
    const auto type = graph.valueLogicalType(value);
    return type.kind == ValueType::Logic && type.shape.empty() && type.width > 0;
}

bool fullStaticSlice(const Graph& graph, OperationId op)
{
    if (graph.opKind(op) != OperationKind::kSliceStatic || graph.opOperands(op).size() != 1 ||
        graph.opResults(op).size() != 1)
        return false;
    const auto start = grhsim::attribute<int64_t>(graph, op, "sliceStart");
    const auto end = grhsim::attribute<int64_t>(graph, op, "sliceEnd");
    const auto source = graph.opOperands(op)[0];
    const auto result = graph.opResults(op)[0];
    return start && end && *start == 0 && *end + 1 == graph.valueWidth(source) &&
        graph.valueWidth(result) == graph.valueWidth(source);
}

bool identityView(const Graph& graph, OperationId op)
{
    if (!op || graph.opResults(op).size() != 1 || graph.opOperands(op).size() != 1) return false;
    const auto kind = graph.opKind(op);
    if (kind != OperationKind::kAssign && !fullStaticSlice(graph, op)) return false;
    const auto input = graph.opOperands(op)[0], result = graph.opResults(op)[0];
    return input != result && scalarLogic(graph, input) && scalarLogic(graph, result) &&
        graph.sameValueType(input, result) && operationEffect(graph, op) == OpEffect::Pure;
}

std::size_t analyze(Graph& graph)
{
    std::size_t aliases = 0;
    // Rewriting use lists does not erase rows, so a stable snapshot is enough
    // and permits alias chains to collapse in operation order.
    const std::vector<OperationId> operations(graph.operations().begin(), graph.operations().end());
    for (const auto op : operations) {
        if (!identityView(graph, op)) continue;
        const auto input = graph.opOperands(op)[0];
        const auto result = graph.opResults(op)[0];
        // Keep FIRRTL node roots and operation/value provenance attached to
        // the surviving source.  The identity operation stays live in the
        // graph; the next explicit DCE may reclaim it if no root needs it.
        // An input/port value has no defining operation that can carry node
        // roots. Leave roots on the alias operation in that case so the
        // explicit DCE records them as eliminated; redirecting them would
        // silently discard provenance. For an operation-backed source,
        // transfer roots to the surviving producer.
        const auto producer = graph.valueDef(input);
        if (producer) ingest::node::redirectRoots(graph, result, input);
        if (producer) grhsim::detail::mergeOrigins(graph, producer, {op});
        graph.replaceAllUses(result, input);
        ++aliases;
    }
    return aliases;
}

} // namespace

AliasAnalysisPass::AliasAnalysisPass(AliasAnalysisOptions options)
    : Pass("alias-analysis", "alias-analysis", "GSIM-compatible identity alias propagation on ordinary GRH"),
      options_(std::move(options))
{}

bool AliasAnalysisPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh";
}

PassResult AliasAnalysisPass::run()
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
        const auto aliases = analyze(graph);
        result.changed = aliases != 0;
        verifyWhole(graph);
        const auto summary = "aliases=" + std::to_string(aliases) +
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
