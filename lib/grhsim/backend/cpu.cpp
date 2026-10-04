#include "grhsim/backend/cpu.hpp"
#include "grhsim/backend/cpu_phase_common.hpp"
#include "grhsim/backend/cpu_phase_emit.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // Six-phase (M3/M4/M5d-6) structural verification for the
        // GeneralNodes..PhaseSchedule stages. The legacy two-phase pipeline
        // was removed in M5d-6; mappings older than SplitPhases are rejected
        // by verifyCpuMapping before reaching this verifier. Stage
        // comparisons use cpuMappingStageRank: the pipeline order is
        // GeneralNodes -> GeneralSupernodes -> LayoutNamedStores ->
        // EventActivationMap -> MemWritePlan -> GeneralFunctions ->
        // PhaseSchedule, which is not the enum's numeric order.
        bool verifyCpuPhases(const GrhSimModel &model, const CpuBackendMapping &cpu,
                             diag::Diagnostics &diagnostics)
        {
            const auto error = [&](std::string message) {
                diagnostics.error(std::move(message), "cpu.mapping");
                return false;
            };
            const auto atLeast = [&](CpuMappingStage stage) {
                return cpuMappingStageAtLeast(cpu.stage, stage);
            };
            if (!atLeast(CpuMappingStage::LayoutNamedStores) && (cpu.dataLayout || cpu.schedule || cpu.translationUnits))
                return error("six-phase CPU mapping must not carry data layout or schedule payloads");
            const auto &tree = cpu.partitionTree;
            const auto validPartition = [&](PartitionId id) {
                return id.generation == 0 && id.index > 0 && id.index <= tree.partitions.size();
            };
            if (!validPartition(tree.root)) return error("partition root is invalid");
            const auto &root = tree.partitions[tree.root.index - 1];
            const std::array<CpuPhase, 4> phaseOrder{CpuPhase::Event, CpuPhase::General,
                                                     CpuPhase::Mem, CpuPhase::Output};
            if (root.parent || root.attrs.kind != CpuPartitionKind::Root || !root.ops.empty() ||
                root.children.size() != phaseOrder.size())
                return error("six-phase CPU root must contain exactly four phase branches");
            for (std::size_t i = 0; i < phaseOrder.size(); ++i)
            {
                if (!validPartition(root.children[i])) return error("invalid phase branch");
                const auto &branch = tree.partitions[root.children[i].index - 1];
                if (branch.parent != tree.root || branch.attrs.kind != CpuPartitionKind::Phase ||
                    branch.attrs.phase != phaseOrder[i])
                    return error("six-phase branch order must be event, general, mem, output");
            }
            const auto memWrite = [](std::string_view type) {
                return type == "core.state.memWrite" || type == "core.state.memFill" ||
                       type == "core.state.memAssign" || type == "core.state.memWriteSeq";
            };
            std::vector<bool> visited(tree.partitions.size() + 1);
            std::vector<bool> covered(model.operations().size() + 1);
            std::vector<OpId> eventOps, generalOps, memOps, outputOps;
            uint32_t eventFunctions = 0, memFunctions = 0, outputFunctions = 0;
            struct Visit { PartitionId id; CpuPhase phase; };
            std::vector<Visit> stack{{tree.root, CpuPhase::None}};
            while (!stack.empty())
            {
                auto [id, phase] = stack.back();
                stack.pop_back();
                if (!validPartition(id) || visited[id.index]) return error("invalid, repeated or cyclic partition");
                visited[id.index] = true;
                const auto &partition = tree.partitions[id.index - 1];
                const auto &attrs = partition.attrs;
                if (partition.id != id) return error("partition IDs must be dense and match table order");
                if (attrs.kind == CpuPartitionKind::Phase)
                {
                    if (partition.parent != tree.root || phase != CpuPhase::None)
                        return error("phase partition is not a root child");
                    phase = attrs.phase;
                    if (phase == CpuPhase::General && !partition.ops.empty())
                        return error("general branch holds its ops in nodes, not on the phase");
                }
                else if (attrs.phase != CpuPhase::None)
                    return error("phase annotation must occur only on phase branches");
                if (attrs.kind == CpuPartitionKind::EventDomain || attrs.kind == CpuPartitionKind::ActiveWord ||
                    (attrs.kind == CpuPartitionKind::Root && id != tree.root))
                    return error("legacy partition kinds are not part of the six-phase mapping");
                const bool generalSupernode = phase == CpuPhase::General &&
                                              attrs.kind == CpuPartitionKind::Supernode;
                const bool generalFunction = phase == CpuPhase::General &&
                                             attrs.kind == CpuPartitionKind::EmitFunction;
                if (atLeast(CpuMappingStage::GeneralSupernodes))
                {
                    if (generalSupernode != attrs.eventActs.has_value())
                        return error("event act annotations must cover exactly the general supernodes");
                    if (generalSupernode != attrs.supernodeCategory.has_value())
                        return error("supernode categories must cover exactly the general supernodes");
                }
                else if (attrs.eventActs || attrs.supernodeCategory)
                    return error("supernode annotations require the general-supernode stage");
                // A1: an enable guard annotates only SinkEvent supernodes
                // (carved out by shared-enable subdivision) from the
                // general-supernode stage on; the per-op membership check
                // runs in the classification walk below.
                if (attrs.enableGuard &&
                    !(generalSupernode && atLeast(CpuMappingStage::GeneralSupernodes) &&
                      attrs.supernodeCategory &&
                      *attrs.supernodeCategory == CpuSupernodeCategory::SinkEvent))
                    return error("enable guards annotate only event sink supernodes");
                if (attrs.enableGuard &&
                    (*attrs.enableGuard <= 0 ||
                     static_cast<uint64_t>(*attrs.enableGuard) > model.values().size()))
                    return error("enable guard is not a valid value index");
                if (!attrs.helperChunks.empty() &&
                    !(atLeast(CpuMappingStage::GeneralFunctions) && generalSupernode))
                    return error("helper chunks annotate only packed general supernodes");
                // Resolution 2 (M5d-6): General emit functions are leaves that
                // only record their supernode ordinal interval.
                if (attrs.supernodeRange.has_value() !=
                    (generalFunction && atLeast(CpuMappingStage::GeneralFunctions)))
                    return error("supernode ranges annotate exactly the general emit functions");
                std::optional<CpuPartitionKind> childKind;
                switch (attrs.kind)
                {
                case CpuPartitionKind::Root:
                    childKind = CpuPartitionKind::Phase;
                    break;
                case CpuPartitionKind::Phase:
                    if (phase == CpuPhase::General)
                    {
                        // From GeneralFunctions on the branch holds its
                        // supernodes followed by the EmitFunction leaves; the
                        // ordered/tiling check runs after the walk.
                        if (atLeast(CpuMappingStage::GeneralFunctions)) childKind = std::nullopt;
                        else if (atLeast(CpuMappingStage::GeneralSupernodes)) childKind = CpuPartitionKind::Supernode;
                        else if (atLeast(CpuMappingStage::GeneralNodes)) childKind = CpuPartitionKind::Node;
                    }
                    else if (atLeast(CpuMappingStage::GeneralFunctions))
                        childKind = CpuPartitionKind::EmitFunction;
                    break;
                case CpuPartitionKind::Node:
                    if (phase != CpuPhase::General || !atLeast(CpuMappingStage::GeneralNodes) ||
                        partition.ops.empty() || !partition.children.empty())
                        return error("general node must be a nonempty leaf in the general branch");
                    {
                        const auto parentKind = tree.partitions[partition.parent.index - 1].attrs.kind;
                        const auto expected = cpu.stage == CpuMappingStage::GeneralNodes ?
                                              CpuPartitionKind::Phase : CpuPartitionKind::Supernode;
                        if (parentKind != expected)
                            return error("general node parenting disagrees with the mapping stage");
                    }
                    break;
                case CpuPartitionKind::Supernode:
                    // Supernodes stay direct General-branch children for their
                    // whole lifetime (M5d-6: ordinals decouple from function
                    // packing); they are never reparented under functions.
                    if (!generalSupernode || !atLeast(CpuMappingStage::GeneralSupernodes) ||
                        partition.children.empty())
                        return error("six-phase supernodes exist only in the general branch");
                    if (tree.partitions[partition.parent.index - 1].attrs.kind != CpuPartitionKind::Phase)
                        return error("general supernodes stay direct children of the phase branch");
                    {
                        uint64_t size = 0;
                        for (auto child : partition.children)
                        {
                            if (!validPartition(child)) return error("invalid general node child");
                            size += tree.partitions[child.index - 1].ops.size();
                        }
                        uint64_t end = 0;
                        for (auto chunk : attrs.helperChunks)
                        {
                            if (chunk.offset != end || chunk.count == 0) return error("invalid helper chunk range");
                            end += chunk.count;
                        }
                        if (!attrs.helperChunks.empty() && end != size)
                            return error("helper chunks do not cover supernode ops");
                    }
                    childKind = CpuPartitionKind::Node;
                    break;
                case CpuPartitionKind::EmitFunction:
                    if (!atLeast(CpuMappingStage::GeneralFunctions) ||
                        tree.partitions[partition.parent.index - 1].attrs.kind != CpuPartitionKind::Phase)
                        return error("emit functions require the function-packing stage under a phase branch");
                    if (phase == CpuPhase::General)
                    {
                        if (!partition.children.empty() || !partition.ops.empty())
                            return error("general emit function only records a supernode range");
                        if (!attrs.supernodeRange)
                            return error("general emit function requires a supernode range");
                    }
                    else
                    {
                        if (!partition.children.empty())
                            return error("flat emit function must be a leaf");
                        if (phase == CpuPhase::Event) ++eventFunctions;
                        else if (phase == CpuPhase::Mem) ++memFunctions;
                        else if (phase == CpuPhase::Output) ++outputFunctions;
                    }
                    break;
                default: return error("unknown partition kind");
                }
                if (!partition.children.empty() && !partition.ops.empty()) return error("non-leaf partition contains ops");
                if (childKind && !partition.ops.empty()) return error("structural partition must not contain ops");
                if (!childKind && !partition.children.empty() &&
                    !(attrs.kind == CpuPartitionKind::Phase && phase == CpuPhase::General &&
                      atLeast(CpuMappingStage::GeneralFunctions)))
                    return error("leaf partition must not contain children");
                for (auto it = partition.children.rbegin(); it != partition.children.rend(); ++it)
                {
                    const auto child = *it;
                    if (!validPartition(child) || tree.partitions[child.index - 1].parent != id)
                        return error("partition parent/child references disagree");
                    if (childKind && tree.partitions[child.index - 1].attrs.kind != *childKind)
                        return error("partition child kind disagrees with CPU mapping stage");
                    stack.push_back({child, phase});
                }
                for (auto opId : partition.ops)
                {
                    if (opId.generation != 0 || opId.index == 0 || opId.index > model.operations().size() ||
                        covered[opId.index])
                        return error("invalid or duplicate op ownership");
                    covered[opId.index] = true;
                    const auto &op = model.operations()[opId.index - 1];
                    if (phase == CpuPhase::None) return error("op outside any phase branch");
                    const auto required = phase == CpuPhase::Event ? SimPhase::Event :
                                          phase == CpuPhase::General ? SimPhase::General :
                                          phase == CpuPhase::Mem ? SimPhase::Mem : SimPhase::Output;
                    if (op.phase != required) return error("op belongs to the wrong phase branch");
                    if (phase == CpuPhase::Mem && !memWrite(model.text(op.opType)))
                        return error("mem branch holds only memory write ops");
                    (phase == CpuPhase::Event ? eventOps : phase == CpuPhase::General ? generalOps :
                     phase == CpuPhase::Mem ? memOps : outputOps).push_back(opId);
                }
            }
            if (std::count(visited.begin() + 1, visited.end(), false)) return error("unreachable partitions");
            for (const auto &op : model.operations())
                if (op.phase == SimPhase::None)
                    return error("six-phase mapping requires total phase attribution");
            if (cpu.stage == CpuMappingStage::SplitPhases)
            {
                // Compat for old checkpoints: the split-phases stage (no longer
                // produced since M5d-6) holds an empty General shell.
                for (std::size_t i = 1; i < covered.size(); ++i)
                    if (!covered[i] && model.operations()[i - 1].phase != SimPhase::General)
                        return error("flat branches do not cover every non-general op");
            }
            else if (std::count(covered.begin() + 1, covered.end(), false))
                return error("partition tree does not cover all ops");
            if (atLeast(CpuMappingStage::GeneralFunctions) &&
                (eventFunctions != 1 || memFunctions != 1 || outputFunctions != 1))
                return error("flat branches must each hold exactly one emit function");
            // General branch at the function stage: supernodes in ordinal
            // order, then EmitFunction leaves whose ranges tile [0, count).
            if (atLeast(CpuMappingStage::GeneralFunctions))
            {
                const auto &general = tree.partitions[root.children[1].index - 1];
                uint32_t supernodeCount = 0, rangeEnd = 0;
                bool seenFunction = false;
                for (const auto child : general.children)
                {
                    const auto &node = tree.partitions[child.index - 1];
                    if (node.attrs.kind == CpuPartitionKind::Supernode)
                    {
                        if (seenFunction)
                            return error("general supernodes must precede the emit function leaves");
                        ++supernodeCount;
                        continue;
                    }
                    if (node.attrs.kind != CpuPartitionKind::EmitFunction || !node.attrs.supernodeRange)
                        return error("general branch holds supernodes and emit function leaves only");
                    seenFunction = true;
                    if (node.attrs.supernodeRange->offset != rangeEnd || node.attrs.supernodeRange->count == 0)
                        return error("emit function supernode ranges must be contiguous and nonempty");
                    rangeEnd += node.attrs.supernodeRange->count;
                }
                if (rangeEnd != supernodeCount)
                    return error("emit function supernode ranges do not tile the general supernodes");
            }
            const auto checkOrder = [&](const std::vector<OpId> &ops, bool detectorsLast,
                                        const char *message) {
                std::vector<bool> defined(model.values().size() + 1);
                bool detectorSeen = false;
                for (auto opId : ops)
                {
                    const auto &op = model.operations()[opId.index - 1];
                    const bool detector = model.text(op.opType) == "core.event.edgeDet";
                    if (detectorsLast && detectorSeen && !detector)
                        return error("event edge detectors must trail the event cone");
                    detectorSeen = detectorSeen || detector;
                    for (auto value : model.operands(op))
                        if (!defined[value.index]) return error(message);
                    for (auto value : model.results(op)) defined[value.index] = true;
                }
                return true;
            };
            if (!checkOrder(eventOps, true, "event order uses a value before its definition")) return false;
            if (!checkOrder(outputOps, false, "output order uses a value before its definition")) return false;
            if (!checkOrder(generalOps, false, "general order uses a value before its definition")) return false;
            uint32_t lastMem = 0;
            for (auto opId : memOps)
            {
                if (opId.index <= lastMem) return error("mem write order must follow static op order");
                lastMem = opId.index;
            }
            if (atLeast(CpuMappingStage::GeneralSupernodes))
            {
                // V2-M1 classification invariants: every General supernode is
                // category-annotated; non-sink supernodes hold only
                // value-producing ops while sink supernodes hold only
                // no-result ops (SinkEvent: one common nonempty signature,
                // carried in attrs.eventActs; SinkEscape: no event acts at
                // all); non-sink supernodes precede sink supernodes in the
                // branch child order; and every sink operand is produced by
                // an earlier (non-sink) supernode.
                constexpr uint32_t absentOrdinal = std::numeric_limits<uint32_t>::max();
                const auto &general = tree.partitions[root.children[1].index - 1];
                std::vector<PartitionId> supernodeOrder;
                for (const auto child : general.children)
                    if (tree.partitions[child.index - 1].attrs.kind == CpuPartitionKind::Supernode)
                        supernodeOrder.push_back(child);
                std::vector<OpId> producer(model.values().size() + 1);
                for (const auto &op : model.operations())
                    for (auto value : model.results(op)) producer[value.index] = op.id;
                std::vector<uint32_t> ordinalOfOp(model.operations().size() + 1, absentOrdinal);
                bool seenSink = false;
                for (uint32_t ordinal = 0; ordinal < supernodeOrder.size(); ++ordinal)
                {
                    const auto &partition = tree.partitions[supernodeOrder[ordinal].index - 1];
                    const auto &attrs = partition.attrs;
                    const auto category = *attrs.supernodeCategory;
                    const bool sink = category != CpuSupernodeCategory::NonSink;
                    if (sink) seenSink = true;
                    else if (seenSink)
                        return error("non-sink supernode trails a sink supernode");
                    std::vector<int64_t> acts;
                    for (auto child : partition.children)
                        for (auto opId : tree.partitions[child.index - 1].ops)
                        {
                            const auto &op = model.operations()[opId.index - 1];
                            const bool opSink = model.results(op).empty();
                            if (sink && !opSink) return error("sink supernode holds a value-producing op");
                            if (!sink && opSink) return error("non-sink supernode holds a sink op");
                            const auto opActs = readCpuPhaseEventActs(model, op);
                            unionCpuPhaseActs(acts, opActs);
                            if (category == CpuSupernodeCategory::SinkEvent && opActs != *attrs.eventActs)
                                return error("sink supernode mixes event signatures");
                            if (category == CpuSupernodeCategory::SinkEscape && !opActs.empty())
                                return error("escape sink supernode holds an event-carrying op");
                            // A1: every op in a guard supernode must be
                            // individually guarded by the shared enable —
                            // the call-site gate is only sound when no op
                            // can fire without it.
                            if (attrs.enableGuard)
                            {
                                const auto guard = static_cast<uint32_t>(*attrs.enableGuard);
                                const auto operands = model.operands(op);
                                if (std::none_of(operands.begin(), operands.end(),
                                                 [guard](ValueId value) { return value.index == guard; }))
                                    return error("enable guard supernode holds an op without the guard operand");
                            }
                            ordinalOfOp[opId.index] = ordinal;
                        }
                    if (*attrs.eventActs != acts)
                        return error("general supernode event acts disagree with its ops");
                    if (category == CpuSupernodeCategory::SinkEvent && attrs.eventActs->empty())
                        return error("event sink supernode requires a nonempty signature");
                }
                for (uint32_t ordinal = 0; ordinal < supernodeOrder.size(); ++ordinal)
                {
                    const auto &partition = tree.partitions[supernodeOrder[ordinal].index - 1];
                    if (*partition.attrs.supernodeCategory == CpuSupernodeCategory::NonSink) continue;
                    for (auto child : partition.children)
                        for (auto opId : tree.partitions[child.index - 1].ops)
                            for (auto value : model.operands(model.operations()[opId.index - 1]))
                            {
                                const auto source = producer[value.index];
                                if (!source) continue;
                                const auto sourceOrdinal = ordinalOfOp[source.index];
                                if (sourceOrdinal == absentOrdinal || sourceOrdinal >= ordinal)
                                    return error("sink supernode operand is not produced by an earlier supernode");
                            }
                }
            }
            if (atLeast(CpuMappingStage::LayoutNamedStores) &&
                !(verifyCpuDataLayout(model, cpu, diagnostics) && verifyCpuSchedule(model, cpu, diagnostics)))
                return false;
            // M5d-7: the emit TU plan is engaged exactly at the terminal
            // TranslationUnits stage.
            if (cpu.stage == CpuMappingStage::TranslationUnits)
            {
                if (!cpu.translationUnits)
                    return error("translation-units stage requires the emit TU plan");
                return verifyCpuTranslationUnits(model, cpu, diagnostics);
            }
            if (cpu.translationUnits)
                return error("emit TU plan requires the translation-units stage");
            return true;
        }
    }

    bool verifyCpuMapping(const GrhSimModel &model, const BackendMapping &mapping,
                          diag::Diagnostics &diagnostics)
    {
        const auto error = [&](std::string message) {
            diagnostics.error(std::move(message), "cpu.mapping");
            return false;
        };
        if (model.text(mapping.backend) != "cpu" || model.text(mapping.schema) != "cpu.st.v1" || !mapping.cpu)
            return error("CPU mapping requires a typed cpu.st.v1 payload");
        const auto &cpu = *mapping.cpu;
        // The six-phase pipeline completes at PhaseSchedule and stays
        // complete through the TranslationUnits emit-planning stage (M5d-7);
        // the legacy two-phase pipeline (terminal Schedule) was removed in
        // M5d-6 and its checkpoints are no longer accepted.
        const bool terminal = cpu.stage == CpuMappingStage::PhaseSchedule ||
                              cpu.stage == CpuMappingStage::TranslationUnits;
        if (mapping.complete != terminal)
            return error("CPU mapping completion requires a terminal six-phase stage");
        if (cpu.stage > CpuMappingStage::TranslationUnits)
            return error("unknown CPU mapping stage");
        if (cpu.stage < CpuMappingStage::SplitPhases)
            return error("legacy two-phase CPU mappings are no longer supported (M5d-6)");
        return verifyCpuPhases(model, cpu, diagnostics);
    }

    void registerCpuPasses(PassRegistry &registry)
    {
        registerCpuPartitionPasses(registry);
        registerCpuLayoutPasses(registry);
        registerCpuSchedulePasses(registry);
        registerCpuEmitPlanPasses(registry);
        registerCpuPhaseEmitPasses(registry);
        registerCpuCloneSharedPasses(registry);
    }
}
