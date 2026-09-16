#include "grhsim/pass/bitwise_muxes.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        class BitwiseMuxesPass final : public Pass {
        public:
            BitwiseMuxesPass() : Pass("grhsim.bitwise-muxes", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                uint32_t converted = 0;
                for (const auto &op : model.operations()) {
                    if (model.text(op.opType) != "core.compute.mux") continue;
                    const auto operands = model.operands(op), results = model.results(op);
                    if (operands.size() != 3 || results.size() != 1 ||
                        !model.objectRefs(op).empty() || !model.parameters(op).empty()) continue;
                    const auto typeId = model.values()[results[0].index - 1].type;
                    const auto &type = model.types()[typeId.index - 1];
                    if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                        type.width != 1 || type.isSigned ||
                        !std::all_of(operands.begin(), operands.end(), [&](ValueId value) {
                            return model.values()[value.index - 1].type == typeId;
                        })) continue;
                    // For one bit, a nonzero predicate is also the per-bit selection mask.
                    // Producers and all dependency edges stay eager and unchanged.
                    const std::array inputs{operands[0], operands[1], operands[2]};
                    const std::array outputs{results[0]};
                    model.replaceOperation(op.id, "core.compute.bitSelect", inputs, outputs);
                    ++converted;
                }
                if (converted) {
                    model.compact(std::vector<uint8_t>(model.operations().size() + 1),
                                  std::vector<uint8_t>(model.states().size() + 1));
                }
                diagnostics.info("bitwise_muxes=" + std::to_string(converted), name());
                return {true, converted != 0, {}};
            }
        };
    }

    void registerBitwiseMuxesPass(PassRegistry &registry) {
        std::string error;
        registry.registerPass("grhsim.bitwise-muxes", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) {
                    factoryError = "grhsim.bitwise-muxes does not accept arguments";
                    return {};
                }
                return std::make_unique<BitwiseMuxesPass>();
            }, error);
    }
}
