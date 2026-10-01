#include "grhsim/pass/const_fold.hpp"

#include "grhsim/ir/model.hpp"
#include "simplify_internal.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // Full constant operation folding over two-state logic compute ops.
        // The rewrite is in place: an op whose operands are all constants is
        // rewritten into a core.compute.constant carrying the evaluated
        // literal, keeping the result value ID, so declaration provenance and
        // dependency edges survive untouched. Dead constant operands left
        // behind are reclaimed by used-bits dead-cone elimination.
        //
        // Four-state values are never folded (X/Z semantics), and div/mod by a
        // zero constant divisor is left alone: the unfolded runtime behavior
        // (C++ division) must not be replaced by a folded value.

        bool twoStateLogic(const GrhSimModel &model, TypeId type)
        {
            const auto &resolved = model.types()[type.index - 1];
            return resolved.kind == TypeKind::Logic && resolved.domain == LogicDomain::TwoState &&
                   resolved.width > 0;
        }

        std::optional<slang::SVInt> constantOf(const GrhSimModel &model, const SimOp &op)
        {
            const auto results = model.results(op);
            if (model.text(op.opType) != "core.compute.constant" || results.size() != 1 ||
                !model.operands(op).empty() || !model.objectRefs(op).empty() ||
                model.parameters(op).size() != 1)
                return {};
            const auto &value = model.values()[results[0].index - 1];
            if (!twoStateLogic(model, value.type)) return {};
            const auto &type = model.types()[value.type.index - 1];
            for (const auto &parameter : model.parameters(op))
            {
                if (model.text(parameter.name) != "constValue" && model.text(parameter.name) != "value")
                    continue;
                std::string literal;
                if (const auto *text = std::get_if<std::string>(&parameter.value)) literal = *text;
                else if (const auto *integer = std::get_if<int64_t>(&parameter.value))
                    literal = std::to_string(*integer);
                else if (const auto *boolean = std::get_if<bool>(&parameter.value))
                    literal = *boolean ? "1" : "0";
                else return {};
                try
                {
                    auto bits = slang::SVInt::fromString(literal);
                    bits.setSigned(type.isSigned);
                    bits = bits.resize(type.width);
                    bits.flattenUnknowns();
                    bits.setSigned(type.isSigned);
                    return bits;
                }
                catch (const std::exception &) { return {}; }
            }
            return {};
        }

        std::optional<int64_t> intParameter(const GrhSimModel &model, std::span<const Parameter> parameters,
                                            std::string_view name)
        {
            for (const auto &parameter : parameters)
                if (model.text(parameter.name) == name)
                    if (const auto *integer = std::get_if<int64_t>(&parameter.value)) return *integer;
            return {};
        }

        std::optional<slang::SVInt> evaluate(const GrhSimModel &model, const SimOp &op,
                                             const std::vector<slang::SVInt> &operands)
        {
            const std::string_view name = model.text(op.opType);
            if (!name.starts_with("core.compute.") || name == "core.compute.constant" ||
                name == "core.compute.expr")
                return {};
            const auto kind = name.substr(std::string_view("core.compute.").size());
            const auto params = model.parameters(op);
            slang::SVInt folded;
            if (kind == "assign")
            {
                if (operands.size() != 1 || !params.empty()) return {};
                folded = operands[0];
            }
            else if (kind == "not")
            {
                if (operands.size() != 1 || !params.empty()) return {};
                folded = ~operands[0];
            }
            else if (kind == "logicNot")
            {
                if (operands.size() != 1 || !params.empty()) return {};
                folded = slang::SVInt(!operands[0]);
            }
            else if (kind == "reduceAnd" || kind == "reduceOr" || kind == "reduceXor" ||
                     kind == "reduceNor" || kind == "reduceNand" || kind == "reduceXnor")
            {
                if (operands.size() != 1 || !params.empty()) return {};
                if (kind == "reduceAnd") folded = slang::SVInt(operands[0].reductionAnd());
                else if (kind == "reduceOr") folded = slang::SVInt(operands[0].reductionOr());
                else if (kind == "reduceXor") folded = slang::SVInt(operands[0].reductionXor());
                else if (kind == "reduceNor") folded = slang::SVInt(!operands[0].reductionOr());
                else if (kind == "reduceNand") folded = slang::SVInt(!operands[0].reductionAnd());
                else folded = slang::SVInt(!operands[0].reductionXor());
            }
            else if (kind == "add" || kind == "sub" || kind == "mul" || kind == "and" ||
                     kind == "or" || kind == "xor" || kind == "xnor" || kind == "div" ||
                     kind == "mod" || kind == "eq" || kind == "ne" || kind == "caseEq" ||
                     kind == "caseNe" || kind == "wildcardEq" || kind == "wildcardNe" ||
                     kind == "lt" || kind == "le" || kind == "gt" || kind == "ge" ||
                     kind == "logicAnd" || kind == "logicOr" || kind == "shl" ||
                     kind == "lshr" || kind == "ashr")
            {
                if (operands.size() != 2 || !params.empty()) return {};
                const slang::SVInt &lhs = operands[0];
                const slang::SVInt &rhs = operands[1];
                if (kind == "add") folded = lhs + rhs;
                else if (kind == "sub") folded = lhs - rhs;
                else if (kind == "mul") folded = lhs * rhs;
                else if (kind == "and") folded = lhs & rhs;
                else if (kind == "or") folded = lhs | rhs;
                else if (kind == "xor") folded = lhs ^ rhs;
                else if (kind == "xnor") folded = ~(lhs ^ rhs);
                else if (kind == "div")
                {
                    if (rhs.getActiveBits() == 0) return {}; // keep the runtime (UB) behavior unchanged
                    folded = lhs / rhs;
                }
                else if (kind == "mod")
                {
                    if (rhs.getActiveBits() == 0) return {};
                    folded = lhs % rhs;
                }
                else if (kind == "eq" || kind == "caseEq" || kind == "wildcardEq")
                    folded = slang::SVInt(lhs == rhs);
                else if (kind == "ne" || kind == "caseNe" || kind == "wildcardNe")
                    folded = slang::SVInt(lhs != rhs);
                else if (kind == "lt") folded = slang::SVInt(lhs < rhs);
                else if (kind == "le") folded = slang::SVInt(lhs <= rhs);
                else if (kind == "gt") folded = slang::SVInt(lhs > rhs);
                else if (kind == "ge") folded = slang::SVInt(lhs >= rhs);
                else if (kind == "logicAnd") folded = slang::SVInt(lhs && rhs);
                else if (kind == "logicOr") folded = slang::SVInt(lhs || rhs);
                else if (kind == "shl") folded = lhs.shl(rhs);
                else if (kind == "lshr") folded = lhs.lshr(rhs);
                else folded = lhs.ashr(rhs);
            }
            else if (kind == "mux" || kind == "bitSelect")
            {
                if (operands.size() != 3 || !params.empty()) return {};
                folded = slang::SVInt::conditional(operands[0], operands[1], operands[2]);
            }
            else if (kind == "prioritySelect")
            {
                if (operands.size() < 3 || operands.size() % 2 != 1 || !params.empty()) return {};
                const std::size_t count = (operands.size() - 1) / 2;
                folded = operands.back();
                for (std::size_t i = 0; i < count; ++i)
                    if (operands[i].getActiveBits() != 0)
                    {
                        folded = operands[count + i];
                        break;
                    }
            }
            else if (kind == "concat")
            {
                if (operands.empty() || !params.empty()) return {};
                folded = slang::SVInt::concat(operands);
            }
            else if (kind == "replicate")
            {
                if (operands.size() != 1) return {};
                const auto rep = intParameter(model, params, "rep");
                if (!rep || *rep <= 0 || params.size() != 1) return {};
                folded = operands[0].replicate(slang::SVInt(static_cast<uint64_t>(*rep)));
            }
            else if (kind == "sliceStatic")
            {
                if (operands.size() != 1) return {};
                const auto start = intParameter(model, params, "sliceStart");
                const auto end = intParameter(model, params, "sliceEnd");
                if (!start || !end || *start < 0 || *end < *start) return {};
                folded = operands[0]
                             .lshr(static_cast<slang::bitwidth_t>(*start))
                             .trunc(static_cast<slang::bitwidth_t>(*end - *start + 1));
            }
            else if (kind == "sliceDynamic" || kind == "sliceArray")
            {
                if (operands.size() != 2) return {};
                const auto width = intParameter(model, params, "sliceWidth");
                if (!width || *width <= 0) return {};
                folded = operands[0].lshr(operands[1]).trunc(static_cast<slang::bitwidth_t>(*width));
            }
            else return {};
            if (folded.hasUnknown()) return {};
            return folded;
        }

        std::string formatLiteral(const slang::SVInt &value)
        {
            return value.toString(slang::LiteralBase::Hex, true, value.getBitWidth());
        }
    } // namespace

    SimplifyStepReport simplifyStepConstFold(GrhSimModel &model, diag::Diagnostics &diagnostics,
                                             SimplifyScope scope)
    {
        (void)diagnostics;
        SimplifyStepReport report;
        const auto &ops = model.operations();
        std::vector<std::optional<slang::SVInt>> constants(model.values().size() + 1);
        std::vector<uint32_t> producer(model.values().size() + 1, 0);
        std::vector<std::vector<uint32_t>> users(model.values().size() + 1);
        for (const auto &op : ops)
            for (auto result : model.results(op)) producer[result.index] = op.id.index;
        for (const auto &op : ops)
            for (auto operand : model.operands(op)) users[operand.index].push_back(op.id.index);

        // Constants are readable from any scope: precompute them for every
        // constant op, then fold only in-scope ops.
        for (const auto &op : ops)
        {
            if (model.text(op.opType) != "core.compute.constant") continue;
            const auto results = model.results(op);
            if (results.size() == 1) constants[results[0].index] = constantOf(model, op);
        }
        std::deque<uint32_t> queue;
        std::vector<uint8_t> queued(ops.size() + 1, 0), folded(ops.size() + 1, 0);
        for (const auto &op : ops)
        {
            if (!scope.inScope(op.phase)) continue;
            queue.push_back(op.id.index);
            queued[op.id.index] = 1;
        }
        uint64_t foldedOps = 0;
        while (!queue.empty())
        {
            const uint32_t opIndex = queue.front();
            queue.pop_front();
            queued[opIndex] = 0;
            if (folded[opIndex]) continue;
            const auto &op = ops[opIndex - 1];
            const auto results = model.results(op);
            if (results.size() != 1 || !twoStateLogic(model, model.values()[results[0].index - 1].type))
                continue;
            if (model.text(op.opType) == "core.compute.constant") continue;
            const auto operandIds = model.operands(op);
            if (operandIds.empty()) continue;
            std::vector<slang::SVInt> operands;
            operands.reserve(operandIds.size());
            bool allConstant = true;
            for (auto operand : operandIds)
            {
                const auto &constant = constants[operand.index];
                if (!constant) { allConstant = false; break; }
                operands.push_back(*constant);
            }
            if (!allConstant) continue;
            const ValueId result = results[0];
            const auto &resultType = model.types()[model.values()[result.index - 1].type.index - 1];
            auto value = evaluate(model, op, operands);
            if (!value) continue;
            // Normalize to the result value's width and signedness (the GRH
            // normalizeToValue idiom: sign first, resize, sign again).
            value->setSigned(resultType.isSigned);
            *value = value->resize(resultType.width);
            value->setSigned(resultType.isSigned);
            value->flattenUnknowns();
            folded[opIndex] = 1;
            const std::array<ValueId, 0> noOperands{};
            const std::array out{result};
            const std::array params{Parameter{model.intern("constValue"), formatLiteral(*value)}};
            model.replaceOperation(op.id, "core.compute.constant", noOperands, out, {}, params);
            constants[result.index] = *value;
            ++foldedOps;
            for (const uint32_t user : users[result.index])
                if (!queued[user] && !folded[user] && scope.inScope(ops[user - 1].phase))
                {
                    queued[user] = 1;
                    queue.push_back(user);
                }
        }
        report.changed = foldedOps != 0;
        report.counters.emplace_back("const_fold_ops", foldedOps);
        return report;
    }

    namespace
    {
        class ConstFoldPass final : public Pass
        {
        public:
            ConstFoldPass() : Pass("grhsim.const-fold", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto report = simplifyStepConstFold(model, diagnostics, SimplifyScope::whole());
                diagnostics.info("const_fold_ops=" + std::to_string(report.counters[0].second), name());
                return {true, report.changed, {}};
            }
        };
    } // namespace

    void registerConstFoldPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.const-fold", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.const-fold does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<ConstFoldPass>());
            }, error);
    }

} // namespace wolvrix::lib::grhsim
