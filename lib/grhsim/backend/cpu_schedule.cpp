#include "grhsim/backend/cpu.hpp"

#include "grhsim/backend/cpu_phase_common.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // ===== Six-phase static tables (C4/C5/C7 passes) =====

        // V2 (M2): fanout targets and the activation map cover NON-SINK
        // supernodes only (boundary 2) — sink supernodes fire on their
        // eventActStore signature (SinkEvent) or unconditionally every round
        // (SinkEscape), never on dataActiveFlag.
        bool isNonSinkSupernode(const CpuPartitionTree &tree, PartitionId id)
        {
            const auto &attrs = tree.partitions[id.index - 1].attrs;
            return attrs.kind == CpuPartitionKind::Supernode &&
                   attrs.supernodeCategory == CpuSupernodeCategory::NonSink;
        }

        // cpu.st.build-event-activation-map (V2-M2; the pre-v2
        // build-event-bitmaps produced sink-facing bitmaps over the M3
        // influence closure, both gone with eventActiveFlag): per event act,
        // the bitmap of non-sink General supernodes holding an op that
        // carries the act (bit i = supernode ordinal i), sourced from the
        // supernodes' eventActs annotations. P_event ORs the fired acts'
        // words into dataActiveFlag. Acts with no non-sink carrier get no
        // entry; sink supernodes never appear (see above).
        std::vector<CpuEventActivation> buildSixPhaseEventActivation(const CpuPartitionTree &tree)
        {
            const auto order = generalSupernodeOrder(tree);
            std::map<uint32_t, uint32_t> entryOfAct;
            std::vector<CpuEventActivation> entries;
            for (uint32_t i = 0; i < order.size(); ++i)
            {
                const auto &attrs = tree.partitions[order[i].index - 1].attrs;
                if (attrs.supernodeCategory != CpuSupernodeCategory::NonSink || !attrs.eventActs)
                    continue;
                for (const auto act : *attrs.eventActs)
                {
                    if (act < 0 || act > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                        throw std::runtime_error("event act index exceeds 32 bits");
                    auto [it, inserted] = entryOfAct.try_emplace(static_cast<uint32_t>(act),
                                                                 static_cast<uint32_t>(entries.size()));
                    if (inserted)
                    {
                        CpuEventActivation entry;
                        entry.act = static_cast<uint32_t>(act);
                        entry.supernodeWords.assign((order.size() + 63) / 64, 0);
                        entries.push_back(std::move(entry));
                    }
                    entries[it->second].supernodeWords[i / 64] |= uint64_t(1) << (i % 64);
                }
            }
            return entries;
        }

        // cpu.st.build-mem-write-plan (C5): Mem-phase write ops in op-id
        // order (per-mem priority = ascending op id, preserving source
        // order); readers are the target mem's General-phase memReads with
        // their owning supernode (staticRow engaged when the address is a
        // constant), plus the General-phase whole-array state.reads of the
        // target (no address operand: always dynamic readers, activated on
        // any write); eventFree marks writes without event_acts.
        // M5d-6: the write set is the Mem phase (mem-class states only —
        // General-phase regLatch-class writes commit through the NBA
        // regLatch next buffer inside their supernodes, not P_mem), and the
        // reader table follows the A7 store classification, not TypeKind.
        std::vector<CpuMemWritePlanEntry> buildSixPhaseMemWritePlan(const GrhSimModel &model,
                                                                    const CpuPartitionTree &tree)
        {
            const auto supernodeOf = generalSupernodeOf(model, tree);
            std::vector<OpId> producer(model.values().size() + 1);
            std::vector<std::vector<OpId>> readersByState(model.states().size() + 1);
            for (const auto &op : model.operations())
            {
                for (const auto value : model.results(op)) producer[value.index] = op.id;
                if (op.phase != SimPhase::General) continue;
                const auto opName = model.text(op.opType);
                // memReads carry an address (staticRow when constant);
                // whole-array state.reads of the target are always dynamic
                // readers (any write may change any row).
                if (opName != "core.state.memRead" && opName != "core.state.read") continue;
                const auto refs = model.objectRefs(op);
                if (refs.empty() || refs.front().kind != ObjectKind::State) continue;
                if (model.states()[refs.front().index - 1].storeClass != StateStoreClass::Mem) continue;
                readersByState[refs.front().index].push_back(op.id);
            }
            std::vector<uint32_t> perMem(model.states().size() + 1, 0);
            std::vector<CpuMemWritePlanEntry> plan;
            for (const auto &op : model.operations())
            {
                if (op.phase != SimPhase::Mem || !isCpuPhaseMemWriteOp(model.text(op.opType))) continue;
                const auto refs = model.objectRefs(op);
                if (refs.empty() || refs.front().kind != ObjectKind::State) continue;
                CpuMemWritePlanEntry entry;
                entry.writeOp = op.id;
                entry.priority = perMem[refs.front().index]++;
                entry.eventFree = readCpuPhaseEventActs(model, op).empty();
                for (const auto readerId : readersByState[refs.front().index])
                {
                    CpuMemReader memReader;
                    memReader.owner = supernodeOf[readerId.index];
                    const auto &reader = model.operations()[readerId.index - 1];
                    if (model.text(reader.opType) == "core.state.memRead")
                    {
                        const auto addr = model.operands(reader).front();
                        const auto source = producer[addr.index];
                        if (source && model.text(model.operations()[source.index - 1].opType) ==
                                          "core.compute.constant")
                        {
                            const Parameter *literal = findCpuPhaseParameter(
                                model, model.parameters(model.operations()[source.index - 1]), "constValue");
                            if (const auto *text = literal ? std::get_if<std::string>(&literal->value)
                                                           : nullptr)
                                memReader.staticRow = parseCpuConstLiteral(*text);
                        }
                    }
                    entry.readers.push_back(memReader);
                }
                std::sort(entry.readers.begin(), entry.readers.end(), [](const auto &a, const auto &b) {
                    return std::tie(a.owner.index, a.staticRow) < std::tie(b.owner.index, b.staticRow);
                });
                entry.readers.erase(std::unique(entry.readers.begin(), entry.readers.end()),
                                    entry.readers.end());
                plan.push_back(std::move(entry));
            }
            return plan;
        }

        struct SixPhaseFanouts
        {
            std::vector<CpuFanoutEntry<ValueId>> input;
            std::vector<CpuFanoutEntry<ValueId>> supernode;
            std::vector<CpuFanoutEntry<StateId>> state;
        };

        // cpu.st.build-phase-schedule fanout tables. inputFanout: General
        // input.read result -> owning supernode (event_only inputs produce no
        // row; P_input activation for those is covered by P_event).
        // supernodeFanout: boundary value -> consumer supernodes (Mem-phase
        // write consumers excluded: P_mem runs every round). stateFanout:
        // reg/latch state -> General reader supernodes (regLatch-class array
        // memReads included; mem-class states use the write plan). V2 (M2):
        // every table's targets are filtered to non-sink supernodes — sink
        // consumers are never data-activated (boundary 2).
        SixPhaseFanouts buildSixPhaseFanouts(const GrhSimModel &model, const CpuPartitionTree &tree)
        {
            const auto order = generalSupernodeOrder(tree);
            std::vector<uint32_t> ordinal(tree.partitions.size() + 1, ~0u);
            for (uint32_t i = 0; i < order.size(); ++i) ordinal[order[i].index] = i;
            const auto supernodeOf = generalSupernodeOf(model, tree);
            std::vector<OpId> producer(model.values().size() + 1);
            for (const auto &op : model.operations())
                for (const auto value : model.results(op)) producer[value.index] = op.id;
            const auto sortedUnique = [&](std::vector<PartitionId> &ids) {
                std::sort(ids.begin(), ids.end(),
                          [&](auto a, auto b) { return ordinal[a.index] < ordinal[b.index]; });
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            };
            SixPhaseFanouts fanouts;
            std::vector<std::vector<PartitionId>> inputTargets(model.values().size() + 1);
            for (const auto &op : model.operations())
            {
                if (op.phase != SimPhase::General || model.text(op.opType) != "core.input.read")
                    continue;
                const Parameter *mark = findCpuPhaseParameter(model, model.parameters(op), "event_only");
                if (const auto *flag = mark ? std::get_if<bool>(&mark->value) : nullptr)
                    if (*flag) continue;
                const auto owner = supernodeOf[op.id.index];
                if (!owner || !isNonSinkSupernode(tree, owner)) continue;
                for (const auto value : model.results(op))
                    inputTargets[value.index].push_back(owner);
            }
            for (uint32_t i = 1; i < inputTargets.size(); ++i)
            {
                if (inputTargets[i].empty()) continue;
                sortedUnique(inputTargets[i]);
                fanouts.input.push_back({ValueId{i, 0}, {std::move(inputTargets[i])}});
            }
            const auto boundary = sixPhaseBoundaryValues(model, tree, supernodeOf);
            std::vector<std::vector<PartitionId>> supernodeTargets(model.values().size() + 1);
            for (const auto &op : model.operations())
            {
                const auto consumer = supernodeOf[op.id.index];
                if (!consumer || !isNonSinkSupernode(tree, consumer)) continue;
                for (const auto operand : model.operands(op))
                {
                    if (!boundary[operand.index]) continue;
                    const auto source = producer[operand.index];
                    if (!source || supernodeOf[source.index] == consumer) continue;
                    supernodeTargets[operand.index].push_back(consumer);
                }
            }
            for (uint32_t i = 1; i < supernodeTargets.size(); ++i)
            {
                if (supernodeTargets[i].empty()) continue;
                sortedUnique(supernodeTargets[i]);
                fanouts.supernode.push_back({ValueId{i, 0}, {std::move(supernodeTargets[i])}});
            }
            std::vector<std::vector<PartitionId>> stateTargets(model.states().size() + 1);
            for (const auto &op : model.operations())
            {
                if (op.phase != SimPhase::General) continue;
                const auto opName = model.text(op.opType);
                // state.read covers scalar and whole-array readers. memRead
                // joins for regLatch-class arrays (M5d-6): their writers
                // commit through the NBA regLatch next buffer inside General
                // supernodes, so their readers activate through stateFanout
                // exactly like scalar reg/latch readers. mem-class array
                // readers stay on the P_mem write plan's reader tables.
                if (opName != "core.state.read" && opName != "core.state.memRead") continue;
                const auto refs = model.objectRefs(op);
                if (refs.empty() || refs.front().kind != ObjectKind::State) continue;
                const auto &state = model.states()[refs.front().index - 1];
                if (opName == "core.state.memRead" && state.storeClass != StateStoreClass::RegLatch)
                    continue;
                const auto owner = supernodeOf[op.id.index];
                if (!owner || !isNonSinkSupernode(tree, owner)) continue;
                stateTargets[refs.front().index].push_back(owner);
            }
            for (uint32_t i = 1; i < stateTargets.size(); ++i)
            {
                if (stateTargets[i].empty()) continue;
                sortedUnique(stateTargets[i]);
                fanouts.state.push_back({StateId{i, 0}, {std::move(stateTargets[i])}});
            }
            return fanouts;
        }

        // Event act -> timeslot flag map: the Output-phase timeslot tasks'
        // timeslotFlag x event_acts Cartesian expansion (task op-id order,
        // acts ascending inside one task).
        std::vector<CpuTimeslotTrigger> buildSixPhaseTimeslotTriggers(const GrhSimModel &model)
        {
            std::vector<CpuTimeslotTrigger> triggers;
            for (const auto &op : model.operations())
            {
                if (op.phase != SimPhase::Output || model.text(op.opType) != "core.system.task")
                    continue;
                const Parameter *flag = findCpuPhaseParameter(model, model.parameters(op), "timeslotFlag");
                if (!flag) continue;
                const auto *index = std::get_if<int64_t>(&flag->value);
                if (!index || *index < 0 || *index > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("timeslotFlag must be a non-negative int64");
                for (const auto act : readCpuPhaseEventActs(model, op))
                {
                    if (act < 0 || act > std::numeric_limits<uint32_t>::max())
                        throw std::runtime_error("event act index exceeds 32 bits");
                    triggers.push_back({static_cast<uint32_t>(act), static_cast<uint32_t>(*index)});
                }
            }
            return triggers;
        }

        // Single-core phase task sequence: P_event (every round) -> P_general
        // emit functions (data-gated, V2-M2: the eventActiveFlag half of the
        // pre-v2 dual gate is gone; sink supernodes self-gate on their
        // eventActStore signatures inside the scan. The General branch's
        // trailing EmitFunction leaves, each holding a supernode ordinal
        // interval) -> P_mem (every round), with P_output outside the round
        // loop (once per eval).
        std::vector<CpuNumaSchedule> buildSixPhaseTasks(const CpuPartitionTree &tree)
        {
            CpuNumaSchedule node;
            node.cores.push_back({});
            auto &tasks = node.cores.front().tasks;
            const auto addTask = [&](PartitionId partition, CpuExecution execution) {
                tasks.push_back({{static_cast<uint32_t>(tasks.size() + 1), 0}, partition, {}, execution});
            };
            const auto &root = tree.partitions[tree.root.index - 1];
            for (const auto branchId : root.children)
            {
                const auto &branch = tree.partitions[branchId.index - 1];
                if (branch.attrs.phase == CpuPhase::General)
                {
                    for (const auto child : branch.children)
                        if (tree.partitions[child.index - 1].attrs.kind == CpuPartitionKind::EmitFunction)
                            addTask(child, CpuExecution::DataGated);
                }
                else if (branch.attrs.phase == CpuPhase::Event || branch.attrs.phase == CpuPhase::Mem)
                    addTask(branch.children.front(), CpuExecution::AlwaysScanCommit);
                else if (branch.attrs.phase == CpuPhase::Output)
                    addTask(branch.children.front(), CpuExecution::EvalEnd);
            }
            return {node};
        }

        class BuildEventActivationMapPass final : public Pass
        {
        public:
            BuildEventActivationMapPass() : Pass("cpu.st.build-event-activation-map", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::LayoutNamedStores)
                {
                    diagnostics.error("requires cpu.st.layout-named-stores output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                CpuSchedulePlan schedule;
                schedule.eventActivation = buildSixPhaseEventActivation(mapping.partitionTree);
                uint64_t mapped = 0, supernodes = 0;
                for (const auto &partition : mapping.partitionTree.partitions)
                {
                    if (partition.attrs.kind != CpuPartitionKind::Supernode) continue;
                    ++supernodes;
                    if (partition.attrs.supernodeCategory == CpuSupernodeCategory::NonSink &&
                        partition.attrs.eventActs && !partition.attrs.eventActs->empty())
                        ++mapped;
                }
                diagnostics.info("event_acts=" + std::to_string(schedule.eventActivation->size()) +
                                 " supernodes=" + std::to_string(supernodes) +
                                 " mapped_supernodes=" + std::to_string(mapped), name());
                mapping.schedule = std::move(schedule);
                mapping.stage = CpuMappingStage::EventActivationMap;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };

        class BuildMemWritePlanPass final : public Pass
        {
        public:
            BuildMemWritePlanPass() : Pass("cpu.st.build-mem-write-plan", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::EventActivationMap || !previous->schedule)
                {
                    diagnostics.error("requires cpu.st.build-event-activation-map output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                auto plan = buildSixPhaseMemWritePlan(model, mapping.partitionTree);
                uint64_t readers = 0, eventFree = 0;
                for (const auto &entry : plan)
                {
                    readers += entry.readers.size();
                    if (entry.eventFree) ++eventFree;
                }
                diagnostics.info("mem_writes=" + std::to_string(plan.size()) +
                                 " mem_readers=" + std::to_string(readers) +
                                 " event_free_writes=" + std::to_string(eventFree), name());
                mapping.schedule->memWritePlan = std::move(plan);
                mapping.stage = CpuMappingStage::MemWritePlan;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };

        class BuildPhaseSchedulePass final : public Pass
        {
        public:
            BuildPhaseSchedulePass() : Pass("cpu.st.build-phase-schedule", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                // C7 (M5d-6): runs after pack-general-functions so the phase
                // task list can reference the emit-function intervals.
                if (!previous || previous->stage != CpuMappingStage::GeneralFunctions || !previous->schedule)
                {
                    diagnostics.error("requires cpu.st.pack-general-functions output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                auto &schedule = *mapping.schedule;
                const auto fanouts = buildSixPhaseFanouts(model, mapping.partitionTree);
                schedule.inputFanout = fanouts.input;
                schedule.computeSupernodeFanout = fanouts.supernode;
                schedule.commitStateFanout = fanouts.state;
                schedule.numaNodes = buildSixPhaseTasks(mapping.partitionTree);
                schedule.timeslotTriggers = buildSixPhaseTimeslotTriggers(model);
                diagnostics.info("tasks=" +
                                 std::to_string(schedule.numaNodes.front().cores.front().tasks.size()) +
                                 " input_sources=" + std::to_string(schedule.inputFanout.size()) +
                                 " supernode_sources=" + std::to_string(schedule.computeSupernodeFanout.size()) +
                                 " state_sources=" + std::to_string(schedule.commitStateFanout.size()) +
                                 " timeslot_triggers=" + std::to_string(schedule.timeslotTriggers->size()),
                                 name());
                mapping.stage = CpuMappingStage::PhaseSchedule;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };
    }

    bool verifyCpuSchedule(const GrhSimModel &model, const CpuBackendMapping &mapping, diag::Diagnostics &diagnostics)
    {
        // Six-phase only (M5d-6 removed the legacy rebuild path): stage-by-stage
        // payload checks recomputed from the model and partition tree, in rank
        // order (LayoutNamedStores < EventActivationMap < MemWritePlan <
        // GeneralFunctions < PhaseSchedule).
        const auto atLeast = [&](CpuMappingStage stage) {
            return cpuMappingStageAtLeast(mapping.stage, stage);
        };
        const auto error = [&](std::string message) {
            diagnostics.error(std::move(message), "cpu.schedule");
            return false;
        };
        if (mapping.stage == CpuMappingStage::LayoutNamedStores)
        {
            if (mapping.schedule)
                return error("six-phase schedule payload requires the event-activation-map stage");
            return true;
        }
        if (!mapping.schedule) return error("six-phase CPU mapping requires the schedule payload");
        const auto &schedule = *mapping.schedule;
        if (atLeast(CpuMappingStage::EventActivationMap))
        {
            // V2 (M2): the recompute-and-compare IS the coverage check — the
            // map must cover exactly the non-sink supernodes holding
            // event-carrying ops (a missed mapping would leave such an op
            // unexecuted on its edge round), and no sink supernode may
            // appear.
            if (!schedule.eventActivation) return error("six-phase schedule requires the event activation map");
            if (*schedule.eventActivation != buildSixPhaseEventActivation(mapping.partitionTree))
                return error("event activation map disagrees with the non-sink supernode act sets");
        }
        else if (schedule.eventActivation)
            return error("event activation map requires the event-activation-map stage");
        if (atLeast(CpuMappingStage::MemWritePlan))
        {
            if (!schedule.memWritePlan) return error("six-phase schedule requires the mem write plan");
            if (*schedule.memWritePlan != buildSixPhaseMemWritePlan(model, mapping.partitionTree))
                return error("mem write plan disagrees with the mem graph");
        }
        else if (schedule.memWritePlan)
            return error("mem write plan requires the mem-plan stage");
        if (atLeast(CpuMappingStage::PhaseSchedule))
        {
            const auto fanouts = buildSixPhaseFanouts(model, mapping.partitionTree);
            if (schedule.inputFanout != fanouts.input)
                return error("input fanout disagrees with the general input reads");
            if (schedule.computeSupernodeFanout != fanouts.supernode)
                return error("supernode fanout disagrees with the boundary value graph");
            if (schedule.commitStateFanout != fanouts.state)
                return error("state fanout does not cover every general state reader");
            if (schedule.numaNodes != buildSixPhaseTasks(mapping.partitionTree))
                return error("phase task sequence disagrees with the partition tree");
            if (!schedule.timeslotTriggers)
                return error("six-phase schedule requires the timeslot trigger map");
            if (*schedule.timeslotTriggers != buildSixPhaseTimeslotTriggers(model))
                return error("timeslot trigger map disagrees with the output tasks");
        }
        else if (schedule.timeslotTriggers || !schedule.inputFanout.empty() ||
                 !schedule.computeSupernodeFanout.empty() || !schedule.commitStateFanout.empty() ||
                 !schedule.numaNodes.empty())
            return error("phase schedule tables require the phase-schedule stage");
        return true;
    }

    void registerCpuSchedulePasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.build-event-activation-map", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.build-event-activation-map does not accept arguments"; return {}; }
                return std::make_unique<BuildEventActivationMapPass>();
            }, error)) throw std::logic_error(error);
        if (!registry.registerPass("cpu.st.build-mem-write-plan", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.build-mem-write-plan does not accept arguments"; return {}; }
                return std::make_unique<BuildMemWritePlanPass>();
            }, error)) throw std::logic_error(error);
        if (!registry.registerPass("cpu.st.build-phase-schedule", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.build-phase-schedule does not accept arguments"; return {}; }
                return std::make_unique<BuildPhaseSchedulePass>();
            }, error)) throw std::logic_error(error);
    }
}
