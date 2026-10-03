#include "grhsim/backend/cpu.hpp"

#include "grhsim/backend/cpu_phase_common.hpp"

#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <string>
#include <vector>

// M5d-7 (C8): cpu.st.plan-translation-units plans the multi-TU emit. It
// consumes the terminal layout/schedule (PhaseSchedule stage) and records a
// CpuTranslationUnitPlan: an ordered chunk stream (Core, Init, Event,
// GeneralScan, Supernode, Mem, Output, Dump) bin-packed into size-bounded
// translation units. cpu.st.emit-cpp then emits one .cpp per unit plus the
// shared header; chunk functions and their spill frames are derived from the
// recorded ranges (supernode helper chunks come from C6). Verification
// replans with the recorded caps and compares, mirroring verifyCpuSchedule.

namespace wolvrix::lib::grhsim
{
    namespace
    {
        struct PlanContext
        {
            const GrhSimModel &model;
            const CpuPartitionTree &tree;
            const CpuDataLayout &layout;
            const CpuSchedulePlan &schedule;
            const CpuNamedStore *boundary = nullptr;
            const CpuNamedStore *prevEvent = nullptr;
            std::vector<PartitionId> supernodeOrder; // ordinal -> partition
            std::vector<Range> scanIntervals;        // C6 EmitFunction intervals tiling [0, N)
        };

        uint64_t estimateInitStepLines(const GrhSimModel &model, StateId state, const InitStep &step)
        {
            const auto intParam = [&](std::string_view name) -> const int64_t * {
                const Parameter *p = findCpuPhaseParameter(model, model.parameters(step), name);
                return p ? std::get_if<int64_t>(&p->value) : nullptr;
            };
            const auto &target = model.types()[model.states()[state.index - 1].type.index - 1];
            const bool array = target.kind == TypeKind::Array;
            const auto kind = model.text(step.kind);
            if (kind == "core.init.const")
            {
                if (!array) return 3;
                const Parameter *p = findCpuPhaseParameter(model, model.parameters(step), "value");
                const auto *values = p ? std::get_if<std::vector<std::string>>(&p->value) : nullptr;
                return 4 + (values ? values->size() : target.count);
            }
            if (kind == "core.init.random") return 4;
            if (kind == "core.init.fill") return 6;
            if (kind == "core.init.readmem")
            {
                const auto *start = intParam("start");
                const auto *count = intParam("count");
                const uint64_t first = start && *start > 0 ? uint64_t(*start) : 0;
                const uint64_t size = count && *count > 0 ? uint64_t(*count) : target.count - first;
                return 6 + size;
            }
            return 4;
        }

        // Supernode scan-line estimate per ordinal: the event-gated variant
        // emits one more line than the data-only one.
        uint64_t scanLines(const PlanContext &ctx, uint32_t ordinal)
        {
            const auto &attrs = ctx.tree.partitions[ctx.supernodeOrder[ordinal].index - 1].attrs;
            return attrs.eventActs && !attrs.eventActs->empty() ? 4 : 3;
        }

        uint64_t estimateSupernodeLines(const GrhSimModel &model, const CpuPartitionTree &tree,
                                        PartitionId supernode)
        {
            const auto &partition = tree.partitions[supernode.index - 1];
            uint64_t lines = 4 + 2 * partition.attrs.helperChunks.size();
            for (const auto node : partition.children)
                for (const auto op : tree.partitions[node.index - 1].ops)
                    lines += estimatedCpuOpLines(model, op);
            return lines;
        }

        // Greedy chunking of a per-item line stream: a chunk closes when the
        // next item would push it past the cap, so a chunk exceeds the cap
        // only when a single indivisible item does.
        void packStream(std::vector<CpuEmitChunk> &out, CpuEmitChunkKind kind,
                        const std::vector<uint64_t> &itemLines, uint64_t cap)
        {
            uint32_t begin = 0;
            uint64_t lines = 0;
            for (uint32_t i = 0; i < itemLines.size(); ++i)
            {
                if (i > begin && lines + itemLines[i] > cap)
                {
                    out.push_back(CpuEmitChunk{kind, begin, i - begin, lines});
                    begin = i;
                    lines = 0;
                }
                lines += itemLines[i];
            }
            if (begin < itemLines.size())
                out.push_back(CpuEmitChunk{kind, begin, static_cast<uint32_t>(itemLines.size()) - begin, lines});
        }

        std::vector<CpuEmitChunk> buildChunks(const PlanContext &ctx, uint64_t chunkCap, uint64_t unitCap)
        {
            const auto &model = ctx.model;
            std::vector<CpuEmitChunk> chunks;

            // Core: the fixed small phase drivers and the system-task driver.
            uint64_t coreLines = 96 + 4 * model.inputs().size();
            if (ctx.schedule.timeslotTriggers) coreLines += 2 * ctx.schedule.timeslotTriggers->size();
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.system.task") { coreLines += 48; break; }
            chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::Core, 0, 0, coreLines});

            // Init stream: steps, then the constant-boundary preloads and
            // prevEvent inits (1 line each), then the regLatchStoreNext sync.
            if (ctx.boundary)
            {
                std::vector<uint64_t> items;
                for (const auto &record : model.initRecords())
                    for (const auto &step : model.steps(record))
                        items.push_back(estimateInitStepLines(model, record.state, step));
                const auto totals = cpuInitStreamTotals(model, *ctx.boundary);
                items.insert(items.end(), totals.boundaryPreloads + totals.prevEvents + 1, 1);
                packStream(chunks, CpuEmitChunkKind::Init, items, chunkCap);
            }

            // Event cone (the Event branch's flat op list).
            {
                const auto ops = cpuFlatBranchOps(ctx.tree, CpuPhase::Event);
                std::vector<uint64_t> items;
                items.reserve(ops.size());
                for (const auto op : ops)
                {
                    uint64_t lines = estimatedCpuOpLines(model, op);
                    if (model.text(model.operations()[op.index - 1].opType) == "core.event.edgeDet") lines += 8;
                    items.push_back(lines);
                }
                packStream(chunks, CpuEmitChunkKind::Event, items, chunkCap);
            }

            // General scan: ordinal ranges aligned to the C6 EmitFunction
            // intervals; an interval that alone exceeds the cap splits at
            // ordinal granularity.
            {
                uint32_t begin = 0;
                uint64_t lines = 0;
                uint32_t cursor = 0;
                for (const auto &interval : ctx.scanIntervals)
                {
                    uint64_t intervalLines = 0;
                    for (uint32_t i = 0; i < interval.count; ++i)
                        intervalLines += scanLines(ctx, interval.offset + i);
                    if (lines > 0 && lines + intervalLines > chunkCap)
                    {
                        chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::GeneralScan, begin, cursor - begin, lines});
                        begin = cursor;
                        lines = 0;
                    }
                    if (intervalLines > chunkCap)
                    {
                        // Oversized interval: cap-sized ordinal subranges.
                        uint32_t subBegin = interval.offset;
                        uint64_t subLines = 0;
                        for (uint32_t i = 0; i < interval.count; ++i)
                        {
                            const uint64_t item = scanLines(ctx, interval.offset + i);
                            if (i > 0 && subLines + item > chunkCap && subLines > 0)
                            {
                                chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::GeneralScan, subBegin,
                                                              interval.offset + i - subBegin, subLines});
                                subBegin = interval.offset + i;
                                subLines = 0;
                            }
                            subLines += item;
                        }
                        if (subBegin < interval.offset + interval.count)
                            chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::GeneralScan, subBegin,
                                                          interval.offset + interval.count - subBegin, subLines});
                        cursor = interval.offset + interval.count;
                        begin = cursor;
                        lines = 0;
                        continue;
                    }
                    lines += intervalLines;
                    cursor = interval.offset + interval.count;
                }
                if (lines > 0)
                    chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::GeneralScan, begin, cursor - begin, lines});
            }

            // Supernodes: one chunk per ordinal. A supernode whose estimate
            // exceeds the unit cap (and has C6 helperChunks to split along)
            // emits a small wrapper chunk plus SupernodePart chunks, so the
            // unit packer can spread its helper functions across TUs
            // (V3-M2); everything else keeps the whole-supernode chunk.
            for (uint32_t ordinal = 0; ordinal < ctx.supernodeOrder.size(); ++ordinal)
            {
                const auto supernode = ctx.supernodeOrder[ordinal];
                const auto &attrs = ctx.tree.partitions[supernode.index - 1].attrs;
                const auto estimate = estimateSupernodeLines(model, ctx.tree, supernode);
                if (estimate > unitCap && attrs.helperChunks.size() >= 2)
                {
                    chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::Supernode, ordinal, 1,
                                                  4 + 2 * attrs.helperChunks.size()});
                    uint32_t part = 0;
                    for (const auto &[range, lines] : cpuSupernodePartRanges(model, ctx.tree, supernode, unitCap))
                        chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::SupernodePart, ordinal, part++, lines});
                }
                else
                {
                    chunks.push_back(CpuEmitChunk{CpuEmitChunkKind::Supernode, ordinal, 1, estimate});
                }
            }

            // Mem write plan entries (each entry emits its write plus one
            // reader-activation line per reader).
            if (ctx.schedule.memWritePlan)
            {
                std::vector<uint64_t> items;
                items.reserve(ctx.schedule.memWritePlan->size());
                for (const auto &entry : *ctx.schedule.memWritePlan)
                    items.push_back(estimatedCpuOpLines(model, entry.writeOp) + 4 + entry.readers.size());
                packStream(chunks, CpuEmitChunkKind::Mem, items, chunkCap);
            }

            // Output cone.
            {
                const auto ops = cpuFlatBranchOps(ctx.tree, CpuPhase::Output);
                std::vector<uint64_t> items;
                items.reserve(ops.size());
                for (const auto op : ops) items.push_back(estimatedCpuOpLines(model, op) + 2);
                packStream(chunks, CpuEmitChunkKind::Output, items, chunkCap);
            }

            // dumpState items.
            {
                std::vector<uint64_t> items(cpuDumpItemCount(model, *ctx.layout.namedStores), 2);
                packStream(chunks, CpuEmitChunkKind::Dump, items, chunkCap);
            }
            return chunks;
        }

        CpuTranslationUnitPlan buildCpuTranslationUnitPlan(const GrhSimModel &model,
                                                           const CpuBackendMapping &mapping,
                                                           uint64_t chunkCap, uint64_t unitCap)
        {
            PlanContext ctx{model, mapping.partitionTree, *mapping.dataLayout, *mapping.schedule};
            for (const auto &store : *ctx.layout.namedStores)
            {
                if (store.kind == CpuNamedStoreKind::Boundary) ctx.boundary = &store;
                if (store.kind == CpuNamedStoreKind::PrevEvent) ctx.prevEvent = &store;
            }
            if (!ctx.boundary || !ctx.prevEvent)
                throw std::runtime_error("CPU TU planning requires the named-store layout");
            ctx.supernodeOrder = generalSupernodeOrder(ctx.tree);
            const auto &root = ctx.tree.partitions[ctx.tree.root.index - 1];
            for (const auto branchId : root.children)
            {
                const auto &branch = ctx.tree.partitions[branchId.index - 1];
                if (branch.attrs.phase != CpuPhase::General) continue;
                for (const auto child : branch.children)
                {
                    const auto &partition = ctx.tree.partitions[child.index - 1];
                    if (partition.attrs.kind == CpuPartitionKind::EmitFunction && partition.attrs.supernodeRange)
                        ctx.scanIntervals.push_back(*partition.attrs.supernodeRange);
                }
            }

            CpuTranslationUnitPlan plan;
            plan.chunkMaxEstimatedLines = chunkCap;
            plan.unitMaxEstimatedLines = unitCap;
            const auto chunks = buildChunks(ctx, chunkCap, unitCap);
            uint64_t unitLines = 0;
            for (const auto &chunk : chunks)
            {
                if (unitLines > 0 && unitLines + chunk.estimatedLines > unitCap)
                {
                    plan.units.back().estimatedLines = unitLines;
                    unitLines = 0;
                }
                if (unitLines == 0)
                    plan.units.push_back(CpuTranslationUnit{"tu" + std::to_string(plan.units.size()), {}, 0});
                plan.units.back().chunks.push_back(chunk);
                unitLines += chunk.estimatedLines;
            }
            if (!plan.units.empty()) plan.units.back().estimatedLines = unitLines;
            if (plan.units.empty()) plan.units.push_back(CpuTranslationUnit{"tu0", {}, 0});
            return plan;
        }

        class PlanTranslationUnitsPass final : public Pass
        {
        public:
            PlanTranslationUnitsPass(uint64_t chunkCap, uint64_t unitCap)
                : Pass("cpu.st.plan-translation-units", PassKind::BackendMapping),
                  chunkCap_(chunkCap), unitCap_(unitCap) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                // Accepts the PhaseSchedule output; re-running on a planned
                // (TranslationUnits) mapping discards and replans, the same
                // rebuild semantics as C1 build-general-nodes.
                if (!previous || !previous->dataLayout || !previous->schedule ||
                    (previous->stage != CpuMappingStage::PhaseSchedule &&
                     previous->stage != CpuMappingStage::TranslationUnits))
                {
                    diagnostics.error("requires cpu.st.build-phase-schedule output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                auto plan = buildCpuTranslationUnitPlan(model, mapping, chunkCap_, unitCap_);
                uint64_t chunkCount = 0, maxUnit = 0, maxChunk = 0;
                for (const auto &unit : plan.units)
                {
                    chunkCount += unit.chunks.size();
                    maxUnit = std::max(maxUnit, unit.estimatedLines);
                    for (const auto &chunk : unit.chunks) maxChunk = std::max(maxChunk, chunk.estimatedLines);
                }
                diagnostics.info("units=" + std::to_string(plan.units.size()) +
                                 " chunks=" + std::to_string(chunkCount) +
                                 " max_unit_estimated_lines=" + std::to_string(maxUnit) +
                                 " max_chunk_estimated_lines=" + std::to_string(maxChunk), name());
                mapping.translationUnits = std::move(plan);
                mapping.stage = CpuMappingStage::TranslationUnits;
                model.setCpuMapping(std::move(mapping));
                return {true, true, {}};
            }

        private:
            uint64_t chunkCap_, unitCap_;
        };
    }

    bool verifyCpuTranslationUnits(const GrhSimModel &model, const CpuBackendMapping &mapping,
                                   diag::Diagnostics &diagnostics)
    {
        const auto error = [&](std::string message) {
            diagnostics.error(std::move(message), "cpu.emit_plan");
            return false;
        };
        if (mapping.stage != CpuMappingStage::TranslationUnits || !mapping.translationUnits)
            return error("emit TU plan requires the translation-units stage");
        const auto &plan = *mapping.translationUnits;
        if (plan.chunkMaxEstimatedLines == 0 || plan.unitMaxEstimatedLines == 0)
            return error("emit TU plan caps must be nonzero");
        // Replan with the recorded caps and compare (the unit names are part
        // of the contract: emit writes <prefix>_<name>.cpp per unit).
        const auto expected = buildCpuTranslationUnitPlan(model, mapping, plan.chunkMaxEstimatedLines,
                                                          plan.unitMaxEstimatedLines);
        if (plan.units != expected.units)
            return error("emit TU plan disagrees with the replanned translation-unit assignment");
        return true;
    }

    void registerCpuEmitPlanPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.plan-translation-units", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &error) -> std::unique_ptr<Pass> {
                constexpr std::string_view usage =
                    "expected [--chunk-max-estimated-lines <n>] [--unit-max-estimated-lines <n>]";
                if (args.size() % 2 != 0) { error = std::string(usage); return {}; }
                uint64_t chunkCap = 2048, unitCap = 32768;
                std::vector<bool> seen(2);
                const std::array<std::string_view, 2> keys{"--chunk-max-estimated-lines", "--unit-max-estimated-lines"};
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    const auto it = std::find(keys.begin(), keys.end(), args[i]);
                    if (it == keys.end()) { error = std::string(usage); return {}; }
                    const auto index = static_cast<std::size_t>(it - keys.begin());
                    if (seen[index]) { error = std::string(usage); return {}; }
                    seen[index] = true;
                    uint64_t value = 0;
                    const auto text = args[i + 1];
                    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
                    { error = "CPU TU plan caps must be positive integers"; return {}; }
                    (index == 0 ? chunkCap : unitCap) = value;
                }
                return std::make_unique<PlanTranslationUnitsPass>(chunkCap, unitCap);
            }, error)) throw std::logic_error(error);
    }
}
