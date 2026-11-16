#include "transform/predicate_set.hpp"

#include "core/grh.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace wolvrix::lib::transform
{
    namespace
    {
        using namespace wolvrix::lib::grh;
        constexpr std::size_t maxLeaves = 512;
        constexpr std::size_t minConstants = 16;

        bool pure(const Operation &op)
        {
            if (const auto attr = op.attr("hasSideEffects"))
            {
                const auto *effect = std::get_if<bool>(&*attr);
                if (!effect || *effect) return false;
            }
            return true;
        }

        bool boolean(const Graph &graph, ValueId value)
        {
            return value.valid() && graph.valueWidth(value) == 1 &&
                   graph.valueType(value) == ValueType::Logic;
        }

        // Only flatten ORs and reductions of Boolean concatenations. Unknown
        // operators remain opaque leaves; no state/effect boundary is crossed.
        bool leaves(const Graph &graph, ValueId root, std::vector<ValueId> &result)
        {
            std::vector<std::pair<ValueId, bool>> pending{{root, false}};
            std::set<uint32_t> seen, active;
            std::size_t visits = 0;
            while (!pending.empty())
            {
                const auto [value, exiting] = pending.back(); pending.pop_back();
                if (exiting)
                {
                    active.erase(value.index); seen.insert(value.index); continue;
                }
                if (++visits > maxLeaves * 4 || !boolean(graph, value)) return false;
                if (seen.contains(value.index)) continue;
                if (!active.insert(value.index).second) return false;
                pending.emplace_back(value, true);
                const auto def = graph.valueDef(value);
                if (def.valid())
                {
                    const auto op = graph.getOperation(def);
                    const auto args = op.operands();
                    if (pure(op) && op.results().size() == 1)
                    {
                        if ((op.kind() == OperationKind::kOr || op.kind() == OperationKind::kLogicOr) &&
                            args.size() == 2 && boolean(graph, args[0]) && boolean(graph, args[1]))
                        {
                            pending.emplace_back(args[1], false); pending.emplace_back(args[0], false); continue;
                        }
                        if (op.kind() == OperationKind::kReduceOr && args.size() == 1)
                        {
                            const auto concatId = graph.valueDef(args[0]);
                            if (concatId.valid())
                            {
                                const auto concat = graph.getOperation(concatId);
                                const auto parts = concat.operands();
                                if (pure(concat) && concat.kind() == OperationKind::kConcat &&
                                    concat.results().size() == 1 && !parts.empty() && parts.size() <= maxLeaves &&
                                    graph.valueType(args[0]) == ValueType::Logic &&
                                    graph.valueWidth(args[0]) == static_cast<int32_t>(parts.size()) &&
                                    std::all_of(parts.begin(), parts.end(), [&](auto v) { return boolean(graph, v); }))
                                {
                                    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                                        pending.emplace_back(*it, false);
                                    continue;
                                }
                            }
                        }
                    }
                }
                result.push_back(value);
                if (result.size() > maxLeaves) return false;
            }
            return true;
        }

        struct Equality { ValueId source; uint64_t constant; };
        std::optional<Equality> equality(const Graph &graph, ValueId value)
        {
            const auto def = graph.valueDef(value);
            if (!def.valid()) return std::nullopt;
            const auto op = graph.getOperation(def);
            if (!pure(op) || op.kind() != OperationKind::kEq || op.operands().size() != 2 ||
                op.results().size() != 1 || !boolean(graph, value)) return std::nullopt;
            for (unsigned side = 0; side != 2; ++side)
            {
                const auto constant = op.operands()[side], source = op.operands()[1 - side];
                const auto constantDef = graph.valueDef(constant);
                if (!constantDef.valid()) continue;
                const auto c = graph.getOperation(constantDef);
                const auto width = graph.valueWidth(source);
                if (!pure(c) || c.kind() != OperationKind::kConstant || width < 2 || width > 64 ||
                    graph.getValue(source).isSigned() || graph.getValue(constant).isSigned() ||
                    graph.valueType(source) != ValueType::Logic || graph.valueType(constant) != ValueType::Logic ||
                    graph.valueWidth(constant) != width) continue;
                const auto attr = c.attr("constValue");
                const auto *literal = attr ? std::get_if<std::string>(&*attr) : nullptr;
                if (!literal) continue;
                try
                {
                    auto parsed = slang::SVInt::fromString(*literal);
                    if (parsed.hasUnknown()) continue;
                    parsed = parsed.resize(width); parsed.setSigned(false);
                    if (const auto number = parsed.as<uint64_t>()) return Equality{source, *number};
                }
                catch (const std::exception &) { continue; }
            }
            return std::nullopt;
        }

        struct Builder
        {
            Graph &graph;
            std::map<std::pair<int32_t, uint64_t>, ValueId> constants;
            std::map<std::tuple<OperationKind, uint32_t, uint32_t>, ValueId> expressions;

            ValueId constant(int32_t width, uint64_t bits)
            {
                const auto key = std::make_pair(width, bits);
                if (const auto it = constants.find(key); it != constants.end()) return it->second;
                const auto value = graph.createValue(width, false);
                const auto op = graph.createOperation(OperationKind::kConstant);
                graph.addResult(op, value);
                graph.setAttr(op, "constValue", std::to_string(width) + "'d" + std::to_string(bits));
                graph.setOpSrcLoc(op, makeTransformSrcLoc("predicate-set", "interval_constant"));
                constants.emplace(key, value); return value;
            }

            ValueId binary(OperationKind kind, ValueId a, ValueId b)
            {
                if ((kind == OperationKind::kLogicOr || kind == OperationKind::kLogicAnd) && a.index > b.index)
                    std::swap(a, b);
                const auto key = std::make_tuple(kind, a.index, b.index);
                if (const auto it = expressions.find(key); it != expressions.end()) return it->second;
                const auto value = graph.createValue(1, false);
                const auto op = graph.createOperation(kind);
                graph.addOperand(op, a); graph.addOperand(op, b); graph.addResult(op, value);
                graph.setOpSrcLoc(op, makeTransformSrcLoc("predicate-set", "interval_membership"));
                expressions.emplace(key, value); return value;
            }

            ValueId interval(ValueId source, uint64_t low, uint64_t high)
            {
                const auto width = graph.valueWidth(source);
                const auto maximum = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
                if (low == high) return binary(OperationKind::kEq, source, constant(width, low));
                if (low == 0 && high == maximum) return constant(1, 1);
                if (low == 0) return binary(OperationKind::kLe, source, constant(width, high));
                if (high == maximum) return binary(OperationKind::kGe, source, constant(width, low));
                return binary(OperationKind::kLogicAnd,
                    binary(OperationKind::kGe, source, constant(width, low)),
                    binary(OperationKind::kLe, source, constant(width, high)));
            }

            ValueId disjunction(std::vector<ValueId> terms)
            {
                while (terms.size() > 1)
                {
                    std::vector<ValueId> next;
                    for (std::size_t i = 0; i < terms.size(); i += 2)
                        next.push_back(i + 1 < terms.size() ? binary(OperationKind::kLogicOr, terms[i], terms[i + 1]) : terms[i]);
                    terms = std::move(next);
                }
                return terms.front();
            }
        };
    }

    PredicateSetPass::PredicateSetPass(PredicateSetOptions options)
        : Pass("predicate-set", "predicate-set", "Fuse same-source equality reductions into interval membership"),
          options_(options)
    {
    }

    PassResult PredicateSetPass::run()
    {
        PassResult result;
        if (!options_.twoState) return result;
        std::size_t roots = 0, families = 0, predicates = 0, intervals = 0;
        for (const auto &entry : design().graphs())
        {
            auto &graph = *entry.second;
            Builder builder{graph, {}, {}};
            const std::vector<OperationId> original(graph.operations().begin(), graph.operations().end());
            for (const auto id : original)
            {
                const auto op = graph.getOperation(id);
                if (!pure(op) || op.results().size() != 1 ||
                    (op.kind() != OperationKind::kReduceOr && op.kind() != OperationKind::kLogicOr &&
                     op.kind() != OperationKind::kOr)) continue;
                const auto root = op.results()[0];
                std::vector<ValueId> operands;
                if (!leaves(graph, root, operands)) continue;
                struct Family { ValueId source; std::set<uint64_t> constants; std::vector<ValueId> values; };
                std::map<uint32_t, Family> groups;
                std::vector<ValueId> remaining;
                for (const auto value : operands)
                {
                    if (const auto eq = equality(graph, value))
                    {
                        auto &family = groups[eq->source.index]; family.source = eq->source;
                        family.constants.insert(eq->constant); family.values.push_back(value);
                    }
                    else remaining.push_back(value);
                }
                bool changed = false;
                for (const auto &[key, family] : groups)
                {
                    std::vector<std::pair<uint64_t, uint64_t>> ranges;
                    for (const auto value : family.constants)
                    {
                        if (!ranges.empty() && ranges.back().second != UINT64_MAX && ranges.back().second + 1 == value)
                            ranges.back().second = value;
                        else ranges.emplace_back(value, value);
                    }
                    // Fixed structural budget: at least eight constants per
                    // interval on average. No workload/toggle/profile inputs.
                    if (family.constants.size() < minConstants || ranges.size() > family.constants.size() / 8)
                    {
                        remaining.insert(remaining.end(), family.values.begin(), family.values.end()); continue;
                    }
                    for (const auto &[low, high] : ranges) remaining.push_back(builder.interval(family.source, low, high));
                    changed = true; ++families; predicates += family.constants.size(); intervals += ranges.size();
                }
                if (!changed) continue;
                const auto replacement = builder.disjunction(std::move(remaining));
                // Keep the public result/name and all other predicate users.
                while (!graph.getOperation(id).operands().empty())
                    graph.eraseOperand(id, graph.getOperation(id).operands().size() - 1);
                graph.setOpKind(id, OperationKind::kAssign); graph.addOperand(id, replacement);
                ++roots; result.changed = true;
            }
        }
        logInfo("roots=" + std::to_string(roots) + " families=" + std::to_string(families) +
                " predicates=" + std::to_string(predicates) + " intervals=" + std::to_string(intervals));
        return result;
    }
}
