#include "grhsim/pass/bitwise_predicates.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        class BitwisePredicatesPass final : public Pass
        {
        public:
            BitwisePredicatesPass() : Pass("grhsim.bitwise-predicates", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto bit = [&](ValueId value) {
                    const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
                    return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                           type.width == 1 && !type.isSigned;
                };
                uint32_t ands = 0, ors = 0;
                for (const auto &op : model.operations())
                {
                    const auto name = model.text(op.opType);
                    if (name != "core.compute.logicAnd" && name != "core.compute.logicOr") continue;
                    const auto inputs = model.operands(op), outputs = model.results(op);
                    if (inputs.size() != 2 || outputs.size() != 1 || !model.parameters(op).empty() ||
                        !model.objectRefs(op).empty() || !bit(outputs[0]) ||
                        !std::all_of(inputs.begin(), inputs.end(), bit)) continue;
                    // Operands are already evaluated SSA values with exactly the range {0,1}.
                    // A hardware predicate therefore needs no C++ short-circuit control flow.
                    // Preserve IDs and dependencies: this pass neither shares nor clones cones.
                    const bool isAnd = name == "core.compute.logicAnd";
                    const std::array operands{inputs[0], inputs[1]};
                    const std::array results{outputs[0]};
                    model.replaceOperation(op.id, isAnd ? "core.compute.and" : "core.compute.or", operands, results);
                    (isAnd ? ands : ors)++;
                }
                if (ands || ors)
                    model.compact(std::vector<uint8_t>(model.operations().size() + 1),
                                  std::vector<uint8_t>(model.states().size() + 1));
                diagnostics.info("bitwise_predicate_ands=" + std::to_string(ands) +
                                 " bitwise_predicate_ors=" + std::to_string(ors), name());
                return {true, ands != 0 || ors != 0, {}};
            }
        };
    }

    void registerBitwisePredicatesPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("grhsim.bitwise-predicates", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty())
                {
                    factoryError = "grhsim.bitwise-predicates does not accept arguments";
                    return {};
                }
                return std::make_unique<BitwisePredicatesPass>();
            }, error);
    }
}
