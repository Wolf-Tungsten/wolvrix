// grhsim.split-phases (M5d-5, pipeline stage B5): the semantic-layer phase
// attribution, split out of the legacy cpu.st.split-phases mapping pass.
// Runs after B1-B4 (classify-event-inputs, lower-edge-detect,
// extract-output-cones, migrate-timeslot-tasks) have tagged the Event and
// Output partitions; this pass completes the attribution so every op carries
// its compute partition:
//
//   - core.event.edgeDet -> Event (defensive; B2 already tags them);
//   - core.output.write -> Output (defensive; B3 already tags them);
//   - state write ops (core.state.write/writeMask/writeArray/writeArrayMask/fill): the P_mem write
//     duty follows the TARGET STATE's store class (A7
//     grhsim.select-state-stores) — mem-class arrays commit in P_mem, while
//     writes on regLatch-class states (including small arrays) join General
//     and take the next-buffer NBA path. Stage-06: core.state.write also
//     covers scalar targets (the retired regWrite), which classify RegLatch
//     and thus take the General path. Without a classification the legacy
//     shape-based attribution applies (shaped target = array = Mem, scalar
//     write = General, matching verifyPhaseAttribution), so pre-A7 flows are
//     unchanged;
//   - everything else -> General.
//
// The pass only writes SimOp::phase; it adds/removes no ops or values.
// verifyPhaseAttribution enforces the same class-aware table, and the B8
// seal (grhsim.verify --seal semantic) certifies totality at the end of the
// partition stage. The legacy CPU backend still schedules all mem write op
// types in P_mem (its largeSinkStore layout implements the same NBA contract);
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
        bool isStateWriteOpType(std::string_view type) noexcept
        {
            return type == "core.state.write" || type == "core.state.writeMask" ||
                   type == "core.state.writeArray" || type == "core.state.writeArrayMask" ||
                   type == "core.state.fill" || type == "core.state.latchWrite";
        }

        // Whether the write commits in P_mem. On a classified model the target
        // state's store class decides (mem-class arrays commit in place in
        // P_mem; regLatch-class states — every scalar, including the targets
        // the retired regWrite served — take the General next-buffer path).
        // Without a classification the legacy shape-based fallback applies:
        // shaped targets are arrays and commit in P_mem, scalar writes go to
        // General. Malformed refs (reported by the structural verifier
        // separately) mirror verifyPhaseAttribution's fallback: General for a
        // scalar-capable write, Mem for the array-only writeArray/fill.
        bool writeCommitsInMem(const GrhSimModel &model, const SimOp &op, std::string_view type)
        {
            std::span<const ObjectRef> refs;
            try
            {
                refs = model.objectRefs(op);
            }
            catch (const std::exception &)
            {
                refs = {};
            }
            if (refs.empty() || refs[0].kind != ObjectKind::State ||
                refs[0].index == 0 || refs[0].index > model.states().size())
                return type != "core.state.write" && type != "core.state.writeMask";
            const auto &state = model.states()[refs[0].index - 1];
            if (model.hasStateStoreClassification())
                return state.storeClass != StateStoreClass::SmallSink;
            if (state.type.generation != 0 || !state.type.valid() ||
                state.type.index > model.types().size())
                return true;
            return !model.types()[state.type.index - 1].shape.empty();
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
                    else if (isStateWriteOpType(type))
                    {
                        if (!writeCommitsInMem(model, op, type))
                        {
                            phase = SimPhase::General;
                            // memWritesRegLatch keeps its original meaning:
                            // array (shaped-target) writes taking the regLatch
                            // path. Scalar writes count as plain General.
                            bool shaped = false;
                            try
                            {
                                const auto refs = model.objectRefs(op);
                                shaped = !refs.empty() && refs[0].kind == ObjectKind::State &&
                                         refs[0].index != 0 && refs[0].index <= model.states().size() &&
                                         model.states()[refs[0].index - 1].type.index != 0 &&
                                         model.states()[refs[0].index - 1].type.index <=
                                             model.types().size() &&
                                         !model.types()[model.states()[refs[0].index - 1].type.index - 1]
                                              .shape.empty();
                            }
                            catch (const std::exception &)
                            {
                                shaped = type != "core.state.write" && type != "core.state.writeMask";
                            }
                            if (shaped) ++memWritesRegLatch; else ++attributedGeneral;
                        }
                        else
                        {
                            phase = SimPhase::LargeSink;
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
