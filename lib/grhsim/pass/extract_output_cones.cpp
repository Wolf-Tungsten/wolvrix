#include "grhsim/pass/extract_output_cones.hpp"
#include "grhsim/pass/cone_extract.hpp"
#include "grhsim/ir/model.hpp"

#include <array>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        // grhsim.extract-output-cones: clones the producer-side cone of every
        // core.output.write operand into P_output, rewires the output.write to
        // the cloned value and tags it Output. The original cone is swept by
        // the shared dead-cone DCE (dual-use ops stay). An output.write whose
        // operand is already produced by an Output-phase op is treated as
        // extracted, which keeps the pass idempotent.
        class ExtractOutputConesPass final : public Pass
        {
        public:
            ExtractOutputConesPass()
                : Pass("grhsim.extract-output-cones", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                std::vector<OpId> producers(model.values().size() + 1);
                for (const auto &op : model.operations())
                    for (const auto result : model.results(op))
                        producers[result.index] = op.id;

                std::vector<OpId> targets;
                std::vector<ValueId> sinks;
                for (const auto &op : model.operations())
                {
                    if (model.text(op.opType) != "core.output.write") continue;
                    const auto operands = model.operands(op);
                    // Malformed shapes are reported by the verifier.
                    if (operands.size() != 1) continue;
                    const ValueId source = operands.front();
                    OpId producer;
                    if (source.generation == 0 && source.valid() &&
                        source.index < producers.size())
                        producer = producers[source.index];
                    if (producer &&
                        model.operations()[producer.index - 1].phase == SimPhase::Output)
                        continue; // already extracted
                    targets.push_back(op.id);
                    sinks.push_back(source);
                }
                if (targets.empty())
                {
                    diagnostics.info("cloned_ops=0 removed_ops=0", name());
                    return {true, false, {}};
                }

                auto extraction = extractCone(model, sinks, SimPhase::Output);
                for (const auto id : targets)
                {
                    const auto &op = model.operations()[id.index - 1];
                    const ValueId source = model.operands(op).front();
                    const ValueId clone = extraction.oldToNewValues[source.index];
                    const std::array<ValueId, 1> newOperands{clone.valid() ? clone : source};
                    const std::vector<ObjectRef> refs(model.objectRefs(op).begin(),
                                                      model.objectRefs(op).end());
                    const std::vector<Parameter> params(model.parameters(op).begin(),
                                                        model.parameters(op).end());
                    model.replaceOperation(id, "core.output.write", newOperands, {}, refs,
                                           params);
                    model.setOperationPhase(id, SimPhase::Output);
                }

                const auto deadOps = sweepDeadConeOps(model, extraction.coneOps);
                std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
                for (const auto id : deadOps) removeOps[id.index] = 1;
                model.compact(removeOps,
                              std::vector<uint8_t>(model.states().size() + 1, 0));

                diagnostics.info("cloned_ops=" + std::to_string(extraction.cloneOps.size()) +
                                     " removed_ops=" + std::to_string(deadOps.size()),
                                 name());
                return {true, true, {}};
            }
        };
    } // namespace

    void registerExtractOutputConesPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.extract-output-cones", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.extract-output-cones does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<ExtractOutputConesPass>());
            },
            error);
    }

} // namespace wolvrix::lib::grhsim
