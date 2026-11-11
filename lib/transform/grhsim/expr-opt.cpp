#include "transform/grhsim/expr-opt.hpp"

#include "rewrite.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;

void requireGrhWhole(Graph& graph)
{
    // F22 runs on ordinary GRH, before grh-to-grhsim lowers state/event
    // operations.  A converted graph is already past this boundary.
    if (graph.metadata(kStage))
        throw std::invalid_argument("expr-opt requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations())
        if (attribute<std::vector<std::string>>(graph, op, kPartition))
            throw std::invalid_argument("expr-opt requires an unpartitioned GRH graph");
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = verifyGraph(graph); !problem.empty()) throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

struct ValuePairLess {
    bool operator()(const std::pair<ValueId, ValueId>& left,
                    const std::pair<ValueId, ValueId>& right) const noexcept {
        const auto less = [](ValueId a, ValueId b) {
            return a.index < b.index || (a.index == b.index && a.generation < b.generation);
        };
        return less(left.first, right.first) ||
            (left.first == right.first && less(left.second, right.second));
    }
};

using ValuePairSet = std::set<std::pair<ValueId, ValueId>, ValuePairLess>;

bool sameValue(const Graph& graph, ValueId left, ValueId right,
               ValuePairSet& visiting);

// Structural equality allows one mux arm to disappear.  Preserve the source
// identities attached to both equivalent arms before the discarded arm is
// reclaimed by DCE; otherwise a semantically meaningful FIRRTL node can be
// lost merely because it was the second copy of the same expression.
void mergeEquivalentSemantic(Graph& graph, ValueId left, ValueId right,
                             ValuePairSet& visiting)
{
    if (left == right || !left || !right) return;
    const auto pair = std::make_pair(left, right);
    if (!visiting.insert(pair).second) return;
    graph.mergeValueSemanticIds(left, right);
    const auto leftDef = graph.valueDef(left), rightDef = graph.valueDef(right);
    if (leftDef && rightDef && graph.opKind(leftDef) == graph.opKind(rightDef)) {
        graph.mergeOpSemanticIds(leftDef, rightDef);
        const auto leftArgs = graph.opOperands(leftDef), rightArgs = graph.opOperands(rightDef);
        if (leftArgs.size() == rightArgs.size())
            for (std::size_t i = 0; i < leftArgs.size(); ++i)
                mergeEquivalentSemantic(graph, leftArgs[i], rightArgs[i], visiting);
    }
    visiting.erase(pair);
}

bool sameOperation(const Graph& graph, OperationId left, OperationId right,
                   ValuePairSet& visiting)
{
    if (left == right) return true;
    if (!left || !right || graph.opKind(left) != graph.opKind(right)) return false;
    const auto leftResults = graph.opResults(left), rightResults = graph.opResults(right);
    if (leftResults.size() != rightResults.size()) return false;
    for (std::size_t i = 0; i < leftResults.size(); ++i)
        if (graph.valueLogicalType(leftResults[i]) != graph.valueLogicalType(rightResults[i])) return false;
    const auto leftArgs = graph.opOperands(left), rightArgs = graph.opOperands(right);
    if (leftArgs.size() != rightArgs.size()) return false;
    // Source locations, node hints, owner paths and origin lists are
    // provenance, not expression-tree content.
    std::vector<std::pair<std::string_view, std::string>> leftAttrs, rightAttrs;
    const auto collect = [](const Graph& graph, OperationId op,
                            std::vector<std::pair<std::string_view, std::string>>& out) {
        for (const auto attr : graph.opAttrRange(op))
            if (attr.key() != kPartition && attr.key() != kOriginModulesKey && attr.key() != "grhsim.nodeClone" &&
                !ingest::node::isAttribute(attr.key())) {
                std::ostringstream encoded;
                detail::encode(encoded, attr.value());
                out.emplace_back(attr.key(), encoded.str());
            }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    };
    collect(graph, left, leftAttrs); collect(graph, right, rightAttrs);
    if (leftAttrs != rightAttrs) return false;
    for (std::size_t i = 0; i < leftArgs.size(); ++i)
        if (!sameValue(graph, leftArgs[i], rightArgs[i], visiting)) return false;
    return true;
}

bool sameValue(const Graph& graph, ValueId left, ValueId right,
               ValuePairSet& visiting)
{
    if (left == right) return true;
    if (!left || !right || graph.valueLogicalType(left) != graph.valueLogicalType(right)) return false;
    const auto pair = std::make_pair(left, right);
    if (!visiting.insert(pair).second) return true;
    const bool equal = sameOperation(graph, graph.valueDef(left), graph.valueDef(right), visiting);
    visiting.erase(pair);
    return equal;
}

} // namespace

ExprOptPass::ExprOptPass(ExprOptOptions options)
    : Pass("expr-opt", "expr-opt", "GSIM-compatible local expression-tree cleanup"),
      options_(std::move(options))
{}

bool ExprOptPass::supportsDialect(std::string_view dialect) const noexcept
{
    return detail::supports(dialect);
}

PassResult ExprOptPass::run()
{
    PassResult result;
    try {
        auto& graph = [&]() -> Graph& {
            if (options_.top.empty()) {
                if (design().topGraphs().size() != 1) throw std::invalid_argument("Select exactly one top graph with -top");
                return *design().findGraph(design().topGraphs()[0]);
            }
            auto* selected = design().findGraph(options_.top);
            if (!selected) throw std::invalid_argument("Top graph does not exist: " + options_.top);
            return *selected;
        }();
        requireGrhWhole(graph);
        verifyWhole(graph);

        const auto beforeOperations = graph.operations().size();
        const auto beforeValues = graph.values().size();
        // An empty shared anchor set is intentional.  GSIM's first DCE does
        // not retain an otherwise-unused source declaration merely because it
        // has a name; ports and effectful roots are handled by Scope::exported.
        const std::unordered_set<uint32_t> noAnchors;
        const auto* anchors = keepDeclaredSymbols() ? nullptr : &noAnchors;
        detail::Scope scope(graph, {}, std::nullopt, anchors, nullptr, true);
        const auto operations = std::vector<OperationId>(graph.operations().begin(), graph.operations().end());
        std::size_t muxRewrites = 0;
        for (const auto op : operations) {
            if (!scope.contains(op) || graph.opKind(op) != OperationKind::kMux) continue;
            const auto args = graph.opOperands(op);
            if (args.size() != 3) continue;
            ValuePairSet visiting;
            if (sameValue(graph, args[1], args[2], visiting)) {
                ValuePairSet semanticVisiting;
                mergeEquivalentSemantic(graph, args[1], args[2], semanticVisiting);
                // Keep the rewritten assignment in the graph.  GSIM's
                // ExprOpt does not perform RemoveDeadNodes implicitly; the
                // explicit DCE stage outside this pass owns dead-row cleanup.
                if (scope.alias(op, args[1], false)) ++muxRewrites;
            }
        }
        result.changed = muxRewrites != 0 ||
            beforeOperations != graph.operations().size() ||
            beforeValues != graph.values().size();
        verifyWhole(graph);
        const auto summary = "mux-rewrites=" + std::to_string(muxRewrites) +
            " operations=" + std::to_string(beforeOperations) + "->" + std::to_string(graph.operations().size()) +
            " values=" + std::to_string(beforeValues) + "->" + std::to_string(graph.values().size()) +
            " semantic-pruned=0";
        info(summary);
        logInfo(summary);
    } catch (const std::exception& error) {
        this->error(error.what());
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
