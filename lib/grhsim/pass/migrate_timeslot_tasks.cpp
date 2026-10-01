#include "grhsim/pass/migrate_timeslot_tasks.hpp"
#include "grhsim/pass/cone_extract.hpp"
#include "grhsim/ir/model.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        const Parameter *findParameter(const GrhSimModel &model,
                                       std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const auto &parameter : parameters)
                if (model.text(parameter.name) == name) return &parameter;
            return nullptr;
        }

        uint64_t widthMask(uint32_t width) noexcept
        {
            return width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
        }

        // Literal rendering matches the init.const constValue style ("1'h0",
        // "8'h00"): explicit width, lowercase hex, zero-padded to full nibbles.
        std::string hexLiteral(uint32_t width, uint64_t bits)
        {
            static const char digits[] = "0123456789abcdef";
            const uint32_t count = (width + 3) / 4;
            std::string text = std::to_string(width) + "'h";
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t shift = (count - 1 - i) * 4;
                const uint8_t nibble =
                    shift < 64 ? static_cast<uint8_t>((bits >> shift) & 0xf) : 0;
                text.push_back(digits[nibble]);
            }
            return text;
        }

        // All-ones literal of the given width; past 64 bits the hex digits are
        // emitted directly instead of going through a uint64_t mask.
        std::string maskLiteral(uint32_t width)
        {
            if (width <= 64) return hexLiteral(width, widthMask(width));
            return std::to_string(width) + "'h" + std::string((width + 3) / 4, 'f');
        }

        bool isTimeslotTaskName(std::string_view name) noexcept
        {
            return name == "strobe" || name == "monitor";
        }

        // grhsim.migrate-timeslot-tasks: migrates the time-slot system tasks
        // ($strobe/$monitor, name parameter in {"strobe", "monitor"}) into
        // P_output. Tasks carrying event_acts keep the parameter (M4 maps act
        // bits to timeslot flags) and gain an int64 timeslotFlag index
        // (0..K-1 in ascending op id); their whole [callCond, args...]
        // operand cone is cloned into P_output. Event-free tasks get one
        // __tslot_prev_<opId>_<i> history state per operand slot, an
        // Output-phase ne/or reduction against the current-value clones
        // feeding a rewritten logicAnd guard, and an Output-phase latchWrite
        // per prev state (enable=1, mask=all ones). See the pass document for
        // the read-old/write-new execution-semantics contract.
        class MigrateTimeslotTasksPass final : public Pass
        {
        public:
            MigrateTimeslotTasksPass()
                : Pass("grhsim.migrate-timeslot-tasks", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                struct Candidate
                {
                    OpId id;
                    bool event;
                };
                std::vector<OpId> producers(model.values().size() + 1);
                for (const auto &op : model.operations())
                    for (const auto result : model.results(op))
                        producers[result.index] = op.id;
                const auto producerPhase = [&](ValueId value) {
                    if (value.generation != 0 || !value.valid() ||
                        value.index >= producers.size())
                        return SimPhase::None;
                    const auto producer = producers[value.index];
                    if (!producer) return SimPhase::None;
                    return model.operations()[producer.index - 1].phase;
                };
                const auto typeOf = [&](ValueId value) -> const Type & {
                    return model.types()[model.values()[value.index - 1].type.index - 1];
                };

                std::vector<Candidate> candidates;
                std::vector<ValueId> sinks;
                for (const auto &op : model.operations())
                {
                    if (model.text(op.opType) != "core.system.task") continue;
                    const auto parameters = model.parameters(op);
                    const Parameter *taskName = findParameter(model, parameters, "name");
                    const auto *nameText =
                        taskName ? std::get_if<std::string>(&taskName->value) : nullptr;
                    if (!nameText || !isTimeslotTaskName(*nameText)) continue;
                    if (findParameter(model, parameters, "timeslotFlag"))
                        continue; // already migrated
                    // The event_edges form is grhsim.lower-edge-detect input;
                    // leave it for that pass.
                    if (findParameter(model, parameters, "event_edges")) continue;
                    const auto operands = model.operands(op);
                    if (operands.empty()) continue; // malformed: verifier reports it
                    const bool event = findParameter(model, parameters, "event_acts") != nullptr;
                    if (!event)
                    {
                        // Free tasks carry no marker parameter; an all-Output
                        // operand set means the task was migrated already.
                        bool allOutput = true;
                        bool allLogic = true;
                        for (const auto operand : operands)
                        {
                            if (producerPhase(operand) != SimPhase::Output) allOutput = false;
                            if (typeOf(operand).kind != TypeKind::Logic) allLogic = false;
                        }
                        if (allOutput) continue;
                        // ne/latchWrite only make sense for logic values;
                        // tasks with non-logic arguments stay put.
                        if (!allLogic) continue;
                    }
                    candidates.push_back(Candidate{op.id, event});
                    for (const auto operand : operands) sinks.push_back(operand);
                }
                if (candidates.empty())
                {
                    diagnostics.info("migrated_event_tasks=0 migrated_free_tasks=0 "
                                     "timeslot_flags=0",
                                     name());
                    return {true, false, {}};
                }

                auto extraction = extractCone(model, sinks, SimPhase::Output);
                const auto cloneOf = [&](ValueId value) { 
                    return extraction.oldToNewValues[value.index];
                };

                const auto bit = model.logicType(1, false, LogicDomain::TwoState);
                ValueId enable;
                std::map<uint32_t, ValueId> onesByWidth;
                const auto constant = [&](TypeId type, std::string literal) {
                    const auto value = model.addValue(type);
                    const std::array params{Parameter{model.intern("constValue"),
                                                      std::move(literal)}};
                    model.setOperationPhase(
                        model.addOperation("core.compute.constant", {}, std::array{value}, {},
                                           params),
                        SimPhase::Output);
                    return value;
                };
                const auto enableOne = [&] {
                    if (!enable.valid()) enable = constant(bit, "1'h1");
                    return enable;
                };
                const auto onesMask = [&](uint32_t width, TypeId type) {
                    const auto [it, inserted] = onesByWidth.emplace(width, ValueId{});
                    if (inserted) it->second = constant(type, maskLiteral(width));
                    return it->second;
                };

                uint32_t eventTasks = 0;
                uint32_t freeTasks = 0;
                int64_t nextFlag = 0;
                for (const auto &candidate : candidates)
                {
                    // Copy the views up front: the loop body appends ops and
                    // values, which may reallocate the model pools.
                    const auto &task = model.operations()[candidate.id.index - 1];
                    const std::vector<ValueId> operands(model.operands(task).begin(),
                                                        model.operands(task).end());
                    const std::vector<ObjectRef> refs(model.objectRefs(task).begin(),
                                                      model.objectRefs(task).end());
                    std::vector<Parameter> params(model.parameters(task).begin(),
                                                  model.parameters(task).end());
                    std::vector<ValueId> clones;
                    clones.reserve(operands.size());
                    bool missingClone = false;
                    for (const auto operand : operands)
                    {
                        const auto clone = cloneOf(operand);
                        if (!clone.valid()) missingClone = true;
                        clones.push_back(clone);
                    }
                    if (missingClone)
                    {
                        diagnostics.error("timeslot task operand has no producer to clone",
                                          "operations[" +
                                              std::to_string(candidate.id.index - 1) + "]");
                        return {false, false, {}};
                    }
                    std::vector<ValueId> newOperands;
                    if (candidate.event)
                    {
                        newOperands = std::move(clones);
                        params.push_back(
                            Parameter{model.intern("timeslotFlag"), nextFlag++});
                        ++eventTasks;
                    }
                    else
                    {
                        // One __tslot_prev state per operand slot; the changed
                        // reduction compares every clone against its prev.
                        const std::string base =
                            "__tslot_prev_" + std::to_string(candidate.id.index) + "_";
                        std::vector<ValueId> changedBits;
                        changedBits.reserve(operands.size());
                        std::vector<StateId> prevStates;
                        prevStates.reserve(operands.size());
                        for (std::size_t i = 0; i < operands.size(); ++i)
                        {
                            const auto &type = typeOf(operands[i]);
                            const auto typeId = model.values()[operands[i].index - 1].type;
                            const auto prev = model.addState(base + std::to_string(i), typeId);
                            // M5d-4 incremental classification: time-slot
                            // monitoring states are small latch histories and
                            // commit with the regLatch publish boundary; the
                            // Output-phase latchWrite timing is unchanged.
                            if (model.hasStateStoreClassification())
                                model.setStateStoreClass(prev, StateStoreClass::RegLatch);
                            const std::array initParams{Parameter{
                                model.intern("value"), hexLiteral(type.width, 0)}};
                            const std::array steps{
                                InitStep{model.intern("core.init.const"), {0, 1}}};
                            model.addInit(prev, steps, initParams);
                            prevStates.push_back(prev);
                            const auto prevValue = model.addValue(typeId);
                            model.setOperationPhase(
                                model.addOperation("core.state.read", {},
                                                   std::array{prevValue},
                                                   std::array{ObjectRef::state(prev)}),
                                SimPhase::Output);
                            const auto neValue = model.addValue(bit);
                            model.setOperationPhase(
                                model.addOperation("core.compute.ne",
                                                   std::array{clones[i], prevValue},
                                                   std::array{neValue}),
                                SimPhase::Output);
                            changedBits.push_back(neValue);
                        }
                        ValueId changed = changedBits.front();
                        for (std::size_t i = 1; i < changedBits.size(); ++i)
                        {
                            const auto orValue = model.addValue(bit);
                            model.setOperationPhase(
                                model.addOperation("core.compute.or",
                                                   std::array{changed, changedBits[i]},
                                                   std::array{orValue}),
                                SimPhase::Output);
                            changed = orValue;
                        }
                        const auto guard = model.addValue(bit);
                        model.setOperationPhase(
                            model.addOperation("core.compute.logicAnd",
                                               std::array{clones.front(), changed},
                                               std::array{guard}),
                            SimPhase::Output);
                        newOperands.push_back(guard);
                        for (std::size_t i = 1; i < clones.size(); ++i)
                            newOperands.push_back(clones[i]);
                        for (std::size_t i = 0; i < operands.size(); ++i)
                        {
                            const auto &type = typeOf(operands[i]);
                            const auto typeId = model.values()[operands[i].index - 1].type;
                            model.setOperationPhase(
                                model.addOperation(
                                    "core.state.latchWrite",
                                    std::array{enableOne(), clones[i],
                                               onesMask(type.width, typeId)},
                                    {}, std::array{ObjectRef::state(prevStates[i])}),
                                SimPhase::Output);
                        }
                        ++freeTasks;
                    }
                    model.replaceOperation(candidate.id, "core.system.task", newOperands, {},
                                           refs, params);
                    model.setOperationPhase(candidate.id, SimPhase::Output);
                }

                const auto deadOps = sweepDeadConeOps(model, extraction.coneOps);
                std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
                for (const auto id : deadOps) removeOps[id.index] = 1;
                model.compact(removeOps,
                              std::vector<uint8_t>(model.states().size() + 1, 0));

                diagnostics.info("migrated_event_tasks=" + std::to_string(eventTasks) +
                                     " migrated_free_tasks=" + std::to_string(freeTasks) +
                                     " timeslot_flags=" + std::to_string(nextFlag),
                                 name());
                return {true, true, {}};
            }
        };
    } // namespace

    void registerMigrateTimeslotTasksPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.migrate-timeslot-tasks", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.migrate-timeslot-tasks does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<MigrateTimeslotTasksPass>());
            },
            error);
    }

} // namespace wolvrix::lib::grhsim
