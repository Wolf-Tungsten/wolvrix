// grhsim.split-phases (M5d-5, pipeline stage B5): the semantic-layer phase
// attribution, split out of the legacy cpu.st.split-phases mapping pass.
// Runs after B1-B4 (classify-event-inputs, lower-edge-detect,
// extract-output-cones, migrate-timeslot-tasks) have tagged the Event and
// Output partitions; this pass completes the attribution so every op carries
// its compute partition:
//
//   - core.event.edgeDet -> Event (defensive; B2 already tags them);
//   - core.output.write -> Output (defensive; B3 already tags them);
//   - mem write ops (memWrite/memFill/memAssign/memWriteSeq): the P_mem
//     write duty follows the TARGET STATE's store class (A7
//     grhsim.select-state-stores) — mem-class arrays commit in P_mem, while
//     writes on regLatch-class states (including small arrays) join General
//     and take the next-buffer NBA path. Without a classification the
//     legacy all-Mem attribution applies, so pre-A7 flows are unchanged;
//   - everything else -> General.
//
// The pass only writes SimOp::phase; it adds/removes no ops or values.
// verifyPhaseAttribution enforces the same class-aware table, and the B8
// seal (grhsim.verify --seal semantic) certifies totality at the end of the
// partition stage. The legacy CPU backend still schedules all mem write op
// types in P_mem (its memStore layout implements the same NBA contract);
// consuming the class-aware attribution for layout/emit is M5d-6.

#include "grhsim/pass/split_phases.hpp"

#include "grhsim/ir/model.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        bool isMemWriteOpType(std::string_view type) noexcept
        {
            return type == "core.state.memWrite" || type == "core.state.memFill" ||
                   type == "core.state.memAssign" || type == "core.state.memWriteSeq";
        }

        // Resolves the mem write's target state; returns StateStoreClass::None
        // when the model carries no classification or the ref is malformed
        // (the structural verifier reports the latter separately).
        StateStoreClass memTargetClass(const GrhSimModel &model, const SimOp &op)
        {
            if (!model.hasStateStoreClassification()) return StateStoreClass::None;
            std::span<const ObjectRef> refs;
            try
            {
                refs = model.objectRefs(op);
            }
            catch (const std::exception &)
            {
                return StateStoreClass::None;
            }
            if (refs.empty() || refs[0].kind != ObjectKind::State ||
                refs[0].index == 0 || refs[0].index > model.states().size())
                return StateStoreClass::None;
            return model.states()[refs[0].index - 1].storeClass;
        }

        class SplitPhasesPass final : public Pass
        {
        public:
            SplitPhasesPass() : Pass("grhsim.split-phases", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                uint64_t attributedEvent = 0, attributedGeneral = 0, attributedMem = 0,
                         attributedOutput = 0, memWritesRegLatch = 0, kept = 0;
                for (const auto &op : model.operations())
                {
                    if (op.phase != SimPhase::None)
                    {
                        ++kept;
                        continue;
                    }
                    if (!model.strings().valid(op.opType)) continue;
                    const std::string_view type = model.text(op.opType);
                    SimPhase phase = SimPhase::General;
                    if (type == "core.event.edgeDet")
                    {
                        phase = SimPhase::Event;
                        ++attributedEvent;
                    }
                    else if (type == "core.output.write")
                    {
                        phase = SimPhase::Output;
                        ++attributedOutput;
                    }
                    else if (isMemWriteOpType(type))
                    {
                        if (memTargetClass(model, op) == StateStoreClass::RegLatch)
                        {
                            phase = SimPhase::General;
                            ++memWritesRegLatch;
                        }
                        else
                        {
                            phase = SimPhase::Mem;
                            ++attributedMem;
                        }
                    }
                    else ++attributedGeneral;
                    model.setOperationPhase(op.id, phase);
                }
                const uint64_t attributed =
                    attributedEvent + attributedGeneral + attributedMem + attributedOutput;
                diagnostics.info(
                    "attributed=" + std::to_string(attributed) +
                        " phase_event=" + std::to_string(attributedEvent) +
                        " phase_general=" + std::to_string(attributedGeneral) +
                        " phase_mem=" + std::to_string(attributedMem) +
                        " phase_output=" + std::to_string(attributedOutput) +
                        " mem_writes_reglatch=" + std::to_string(memWritesRegLatch) +
                        " already_attributed=" + std::to_string(kept),
                    name());
                return {true, attributed != 0, {}};
            }
        };
    } // namespace

    void registerSplitPhasesPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.split-phases", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.split-phases does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<SplitPhasesPass>());
            },
            error);
    }
} // namespace wolvrix::lib::grhsim
