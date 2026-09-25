#include "grhsim/backend/cpu.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        struct StateWrite
        {
            OpId op;
            ValueId historyValue;
        };

        struct ScheduleGraph
        {
            std::vector<OpId> producer;
            std::vector<PartitionId> owner;
            std::vector<PartitionId> domain;
            std::vector<uint32_t> eventCounts;
            std::vector<std::vector<StateWrite>> writers;
            std::vector<bool> inputs;

            ScheduleGraph(const GrhSimModel &model, const CpuPartitionTree &tree)
                : producer(model.values().size() + 1), owner(model.operations().size() + 1),
                  domain(owner.size()), eventCounts(owner.size()), writers(model.states().size() + 1),
                  inputs(producer.size())
            {
                for (const auto &partition : tree.partitions)
                {
                    const auto unit = partition.attrs.kind == CpuPartitionKind::Node ? partition.parent : partition.id;
                    PartitionId gate;
                    for (auto parent = partition.parent; parent; parent = tree.partitions[parent.index - 1].parent)
                        if (tree.partitions[parent.index - 1].attrs.kind == CpuPartitionKind::EventDomain)
                        { gate = parent; break; }
                    for (auto op : partition.ops) { owner[op.index] = unit; domain[op.index] = gate; }
                }
                for (const auto &op : model.operations())
                {
                    const auto type = model.text(op.opType);
                    for (auto result : model.results(op))
                    { producer[result.index] = op.id; inputs[result.index] = type == "core.input.read"; }
                    for (const auto &parameter : model.parameters(op))
                    {
                        if (model.text(parameter.name) != "event_edges") continue;
                        const auto *edges = std::get_if<std::vector<std::string>>(&parameter.value);
                        if (!edges) throw std::runtime_error("CPU schedule event_edges must be strings");
                        for (const auto &edge : *edges)
                            if (edge != "posedge" && edge != "negedge")
                                throw std::runtime_error("CPU schedule unsupported event edge");
                        eventCounts[op.id.index] = static_cast<uint32_t>(edges->size());
                    }
                    const auto refs = model.objectRefs(op);
                    const auto count = eventCounts[op.id.index];
                    const bool commit = isCpuCommitOp(type);
                    const auto base = commit || type == "core.dpi.call" ? 1u : 0u;
                    if (count)
                    {
                        if ((!commit && type != "core.dpi.call" && type != "core.system.task") ||
                            refs.size() != count + base || model.operands(op).size() < count)
                            throw std::runtime_error("CPU schedule event/history arity mismatch");
                        const auto events = model.operands(op).last(count);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            const auto history = refs[base + i];
                            if (history.kind != ObjectKind::State)
                                throw std::runtime_error("CPU event history is not a state");
                            writers[history.index].push_back({op.id, events[i]});
                        }
                    }
                    if (commit) writers[refs.front().index].push_back({op.id, {}});
                }
            }
        };

        std::vector<bool> quiescenceStates(const GrhSimModel &model, const ScheduleGraph &graph)
        {
            std::vector<bool> states(model.states().size() + 1), ops(model.operations().size() + 1);
            std::vector<OpId> pendingOps;
            std::vector<StateId> pendingStates;
            const auto visitOp = [&](OpId op) {
                if (op && !ops[op.index]) { ops[op.index] = true; pendingOps.push_back(op); }
            };
            const auto visitState = [&](StateId state) {
                if (!states[state.index]) { states[state.index] = true; pendingStates.push_back(state); }
            };
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) == "core.output.write") visitOp(op.id);
                const auto count = graph.eventCounts[op.id.index];
                if (!count) continue;
                for (auto event : model.operands(op).last(count)) visitOp(graph.producer[event.index]);
                for (auto history : model.objectRefs(op).last(count))
                    visitState({history.index, history.generation});
            }
            // State dependencies traverse all writers, but a history's next value is only its event.
            while (!pendingOps.empty() || !pendingStates.empty())
            {
                while (!pendingOps.empty())
                {
                    const auto id = pendingOps.back(); pendingOps.pop_back();
                    const auto &op = model.operations()[id.index - 1];
                    for (auto operand : model.operands(op)) visitOp(graph.producer[operand.index]);
                    const auto type = model.text(op.opType);
                    if (type == "core.state.read" || type == "core.state.memRead")
                        for (auto ref : model.objectRefs(op)) visitState({ref.index, ref.generation});
                    const auto count = graph.eventCounts[id.index];
                    for (auto history : model.objectRefs(op).last(count))
                        visitState({history.index, history.generation});
                }
                while (!pendingStates.empty())
                {
                    const auto state = pendingStates.back(); pendingStates.pop_back();
                    for (const auto &writer : graph.writers[state.index])
                        if (writer.historyValue) visitOp(graph.producer[writer.historyValue.index]);
                        else visitOp(writer.op);
                }
            }
            return states;
        }

        struct FanoutEdge
        {
            uint32_t source = 0;
            PartitionId target;
            bool arm = false;
        };

        void sortActive(std::vector<PartitionId> &ids, const CpuPartitionTree &tree)
        {
            std::sort(ids.begin(), ids.end(), [&](auto a, auto b) {
                return tree.partitions[a.index - 1].attrs.activeId < tree.partitions[b.index - 1].attrs.activeId;
            });
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }

        template <typename SourceId>
        std::vector<CpuFanoutEntry<SourceId>> fanout(std::vector<FanoutEdge> edges, const CpuPartitionTree &tree)
        {
            std::sort(edges.begin(), edges.end(), [](const auto &a, const auto &b) {
                return std::tie(a.source, a.arm, a.target.index) < std::tie(b.source, b.arm, b.target.index);
            });
            std::vector<CpuFanoutEntry<SourceId>> rows;
            for (auto edge : edges)
            {
                if (rows.empty() || rows.back().source.index != edge.source)
                    rows.push_back({SourceId{edge.source, 0}, {}});
                if (!edge.target) continue;
                auto &targets = edge.arm ? rows.back().targets.arm : rows.back().targets.activate;
                if (targets.empty() || targets.back() != edge.target) targets.push_back(edge.target);
            }
            for (auto &row : rows) sortActive(row.targets.activate, tree);
            return rows;
        }

        CpuSchedulePlan buildSchedule(const GrhSimModel &model, const CpuBackendMapping &mapping)
        {
            CpuSchedulePlan schedule;
            schedule.numaNodes.push_back({0, {{0, {}}}});
            auto &tasks = schedule.numaNodes.front().cores.front().tasks;
            const auto &tree = mapping.partitionTree;
            const ScheduleGraph graph(model, tree);
            std::vector<bool> historyFallback(tree.partitions.size() + 1);
            // A stable shared history can still disagree with an event when another writer wins.
            for (const auto &writers : graph.writers)
            {
                if (writers.empty()) continue;
                const auto sample = writers.front().historyValue;
                if (sample && std::all_of(writers.begin(), writers.end(),
                    [&](const auto &writer) { return writer.historyValue == sample; })) continue;
                for (const auto &writer : writers)
                    if (writer.historyValue && graph.domain[writer.op.index])
                        historyFallback[graph.domain[writer.op.index].index] = true;
            }
            std::vector<PartitionId> stack{tree.root};
            while (!stack.empty())
            {
                const auto &partition = tree.partitions[stack.back().index - 1]; stack.pop_back();
                if (partition.attrs.kind == CpuPartitionKind::EmitFunction)
                {
                    const auto &parent = tree.partitions[partition.parent.index - 1];
                    const auto execution = parent.attrs.kind != CpuPartitionKind::EventDomain ? CpuExecution::ActivityDrivenCompute :
                                           parent.attrs.eventGate && !historyFallback[parent.id.index] ?
                                           CpuExecution::DomainGatedCommit : CpuExecution::AlwaysScanCommit;
                    tasks.push_back({{static_cast<uint32_t>(tasks.size() + 1), 0}, partition.id, {}, execution});
                }
                stack.insert(stack.end(), partition.children.rbegin(), partition.children.rend());
            }
            auto projection = quiescenceStates(model, graph);
            std::vector<FanoutEdge> inputEdges, computeEdges, stateEdges;
            for (const auto &state : model.states())
                if (projection[state.id.index]) stateEdges.push_back({state.id.index, {}, false});
            for (const auto &op : model.operations())
            {
                const auto owner = graph.owner[op.id.index];
                const auto type = model.text(op.opType);
                const bool compute = !graph.domain[op.id.index];
                for (auto operand : model.operands(op))
                {
                    const auto producer = mapping.dataLayout->values[operand.index - 1].owner;
                    if (graph.inputs[operand.index])
                    {
                        inputEdges.push_back({operand.index, producer, false});
                        if (compute) inputEdges.push_back({operand.index, owner, false});
                    }
                    if (compute && producer != owner) computeEdges.push_back({operand.index, owner, false});
                }
                if (type == "core.state.read" || type == "core.state.memRead")
                {
                    // Every state reader is armed through commit fanout, projected or not;
                    // only side-effecting system/DPI ops keep per-round seeding. Projection
                    // membership still decides the pending record's convergence flag.
                    for (auto ref : model.objectRefs(op))
                        stateEdges.push_back({ref.index, owner, false});
                }
                if (type == "core.system.function" || type == "core.system.task" || type == "core.dpi.call")
                    schedule.roundSeeds.push_back(owner);
                const auto count = graph.eventCounts[op.id.index];
                for (auto history : model.objectRefs(op).last(count))
                    if (compute) stateEdges.push_back({history.index, owner, false});
                    else stateEdges.push_back({history.index, graph.domain[op.id.index], true});
            }
            for (const auto &partition : tree.partitions)
            {
                if (!partition.attrs.eventGate) continue;
                for (auto event : partition.attrs.eventGate->events)
                {
                    // Any value transition arms a domain, including the opposite edge, to sample histories.
                    auto &edges = graph.inputs[event.value.index] ? inputEdges : computeEdges;
                    edges.push_back({event.value.index, partition.id, true});
                }
            }
            sortActive(schedule.roundSeeds, tree);
            schedule.inputFanout = fanout<ValueId>(std::move(inputEdges), tree);
            schedule.computeSupernodeFanout = fanout<ValueId>(std::move(computeEdges), tree);
            schedule.commitStateFanout = fanout<StateId>(std::move(stateEdges), tree);
            schedule.quiescenceProjection = std::move(projection);
            const auto addBytes = [](uint64_t a, uint64_t b) {
                if (b > std::numeric_limits<uint64_t>::max() - a)
                    throw std::runtime_error("CPU input shadow byte size overflow");
                return a + b;
            };
            for (const auto &row : schedule.inputFanout)
            {
                const auto typeId = mapping.dataLayout->values[row.source.index - 1].type;
                const auto &type = mapping.dataLayout->types[typeId.index - 1];
                const auto offset = addBytes(schedule.inputShadowBytes, type.alignment - 1) & ~uint64_t(type.alignment - 1);
                schedule.inputShadows.push_back({row.source, typeId, offset});
                schedule.inputShadowBytes = addBytes(offset, type.size);
            }
            schedule.inputShadowBytes = addBytes(schedule.inputShadowBytes, 7) & ~uint64_t(7);
            return schedule;
        }

        class BuildSchedulePass final : public Pass
        {
        public:
            BuildSchedulePass() : Pass("cpu.st.build-schedule", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::DataLayout || !previous->dataLayout)
                {
                    diagnostics.error("requires cpu.st.layout-data output", name());
                    return {false, false, {}};
                }
                auto schedule = buildSchedule(model, *previous);
                diagnostics.info("tasks=" + std::to_string(schedule.numaNodes.front().cores.front().tasks.size()) +
                                 " input_sources=" + std::to_string(schedule.inputFanout.size()) +
                                 " compute_sources=" + std::to_string(schedule.computeSupernodeFanout.size()) +
                                 " quiescence_states=" + std::to_string(std::count(schedule.quiescenceProjection.begin(), schedule.quiescenceProjection.end(), true)) +
                                 " commit_states=" + std::to_string(schedule.commitStateFanout.size()) +
                                 " round_seeds=" + std::to_string(schedule.roundSeeds.size()), name());
                auto mapping = *previous;
                mapping.schedule = std::move(schedule);
                mapping.stage = CpuMappingStage::Schedule;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }
        };
    }

    bool verifyCpuSchedule(const GrhSimModel &model, const CpuBackendMapping &mapping, diag::Diagnostics &diagnostics)
    {
        if (mapping.stage < CpuMappingStage::Schedule && !mapping.schedule) return true;
        if (mapping.stage != CpuMappingStage::Schedule || !mapping.schedule ||
            *mapping.schedule != buildSchedule(model, mapping))
        {
            diagnostics.error("CPU schedule differs from task order, dependency closure, fanout or shadows", "cpu.schedule");
            return false;
        }
        return true;
    }

    bool refreshCpuSchedule(GrhSimModel &model, diag::Diagnostics &diagnostics)
    {
        const auto *previous = model.cpuMapping();
        if (!previous || previous->stage < CpuMappingStage::DataLayout || !previous->dataLayout)
        {
            diagnostics.error("CPU schedule refresh requires a data-layout stage mapping", "cpu.schedule");
            return false;
        }
        auto mapping = *previous;
        mapping.schedule = buildSchedule(model, mapping);
        mapping.stage = CpuMappingStage::Schedule;
        model.setCpuMapping(std::move(mapping));
        return true;
    }

    void registerCpuSchedulePasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.build-schedule", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                if (!args.empty()) { error = "cpu.st.build-schedule does not accept arguments"; return {}; }
                return std::make_unique<BuildSchedulePass>();
            }, error)) throw std::logic_error(error);
    }
}
