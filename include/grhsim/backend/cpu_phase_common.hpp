#ifndef WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP
#define WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP

// Shared helpers for the six-phase layout/schedule passes and their
// verifiers (cpu_layout.cpp / cpu_schedule.cpp). Everything here is a pure
// function of the model plus the partition tree. M5d-6: the supernode
// ordinal is fixed by merge-general-supernodes (C2) as the General branch's
// child order; function packing (C6) only records intervals over it.

#include "grhsim/dialect/compute.hpp"
#include "grhsim/dialect/masked_or.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grhsim
{
    inline const Parameter *findCpuPhaseParameter(const GrhSimModel &model,
                                                  std::span<const Parameter> parameters,
                                                  std::string_view name)
    {
        for (const auto &parameter : parameters)
            if (model.text(parameter.name) == name) return &parameter;
        return nullptr;
    }

    // The op's event act set A(op): its event_acts cluster indices, sorted and
    // deduplicated; empty for event-free ops (mirrors the M3 partition helper).
    inline std::vector<int64_t> readCpuPhaseEventActs(const GrhSimModel &model, const SimOp &op)
    {
        const Parameter *parameter = findCpuPhaseParameter(model, model.parameters(op), "event_acts");
        if (!parameter) return {};
        const auto *indices = std::get_if<std::vector<int64_t>>(&parameter->value);
        if (!indices) throw std::runtime_error("event_acts must be an int64 array");
        auto acts = *indices;
        std::sort(acts.begin(), acts.end());
        acts.erase(std::unique(acts.begin(), acts.end()), acts.end());
        return acts;
    }

    inline void unionCpuPhaseActs(std::vector<int64_t> &target, const std::vector<int64_t> &source)
    {
        if (source.empty()) return;
        const auto middle = target.size();
        target.insert(target.end(), source.begin(), source.end());
        std::inplace_merge(target.begin(), target.begin() + middle, target.end());
        target.erase(std::unique(target.begin(), target.end()), target.end());
    }

    // The write op family that may be attributed to P_mem (stage-06 op
    // names; write covers both scalar whole-writes and the retired element
    // assign). Whether a write actually lands in the Mem phase follows the
    // target state's store class (class-aware routing: scalar/regLatch-class
    // writes go to General), so this helper only answers "can this op kind
    // write a mem-class state".
    inline bool isCpuPhaseMemWriteOp(std::string_view type) noexcept
    {
        return type == "core.state.write" || type == "core.state.writeMask" ||
               type == "core.state.writeArray" || type == "core.state.writeArrayMask" ||
               type == "core.state.fill" || type == "core.state.latchWrite";
    }

    struct CpuIndexedWriteLayout
    {
        bool valid = false;
        bool masked = false;
        uint32_t indexCount = 0;
    };

    // Resolve the post-edge-lowering operand geometry of an indexed state
    // write. `writeArray` is the canonical unmasked form, but old v3 models
    // used that name for a masked write; type geometry disambiguates the two
    // forms when arity alone is insufficient.
    inline CpuIndexedWriteLayout cpuIndexedWriteLayout(const GrhSimModel &model,
                                                       const SimOp &op,
                                                       const Type &target)
    {
        const std::string_view name = model.text(op.opType);
        const auto operands = model.operands(op);
        const bool forceMasked = name == "core.state.writeArrayMask";
        const auto validValue = [&](ValueId value, uint32_t width) {
            if (!value.valid() || value.index > model.values().size()) return false;
            const TypeId typeId = model.values()[value.index - 1].type;
            if (!typeId.valid() || typeId.index > model.types().size()) return false;
            const Type &type = model.types()[typeId.index - 1];
            // Packed-first: data and mask are consumed as flat bits. Their
            // shape is an optional structural view and does not affect the
            // state access layout.
            return type.kind == TypeKind::Logic && type.width == width;
        };
        const auto logicAnyShape = [&](ValueId value) {
            if (!value.valid() || value.index > model.values().size()) return false;
            const TypeId typeId = model.values()[value.index - 1].type;
            if (!typeId.valid() || typeId.index > model.types().size()) return false;
            const Type &type = model.types()[typeId.index - 1];
            return type.kind == TypeKind::Logic;
        };
        const auto elementWidth = typeElementWidth(target);
        if (target.kind != TypeKind::Logic || target.shape.empty() || !elementWidth)
            return {};
        const auto tryLayout = [&](bool masked) {
            CpuIndexedWriteLayout result{false, masked, 0};
            const std::size_t base = 2 + (masked ? 1 : 0);
            if (operands.size() < base + 1) return result;
            const std::size_t count = operands.size() - base;
            if (count == 0 || count > target.shape.size()) return result;
            for (std::size_t i = 0; i < count; ++i)
                if (!logicAnyShape(operands[1 + i])) return result;
            uint64_t suffix = 1;
            for (std::size_t i = count; i < target.shape.size(); ++i)
            {
                const uint64_t dim = static_cast<uint64_t>(target.shape[i]);
                if (!dim || suffix > UINT64_MAX / dim) return result;
                suffix *= dim;
            }
            const uint64_t dataWidth = uint64_t(*elementWidth) * suffix;
            if (dataWidth > UINT32_MAX) return result;
            if (!validValue(operands[1 + count], static_cast<uint32_t>(dataWidth)))
                return result;
            if (masked)
            {
                if (!logicAnyShape(operands[2 + count])) return result;
                const TypeId maskTypeId = model.values()[operands[2 + count].index - 1].type;
                const Type &maskType = model.types()[maskTypeId.index - 1];
                if (maskType.width != dataWidth) return result;
            }
            result.valid = true;
            result.indexCount = static_cast<uint32_t>(count);
            return result;
        };
        if (forceMasked) return tryLayout(true);
        const auto plain = tryLayout(false);
        if (plain.valid) return plain;
        return tryLayout(true);
    }

    // The supernode ordinal (M5d-6, resolution 2): the General branch's
    // Supernode children in tree order — fixed at C2 and untouched by C6's
    // function packing (EmitFunction leaves trail the supernodes and are
    // skipped here). Partition ids never reach the emitter; this 0..N-1
    // sequence indexes the ActiveFlags bit words (V3-M1: bit per supernode)
    // and the event bitmap words.
    inline std::vector<PartitionId> generalSupernodeOrder(const CpuPartitionTree &tree)
    {
        std::vector<PartitionId> order;
        const auto &root = tree.partitions[tree.root.index - 1];
        for (const auto branchId : root.children)
        {
            const auto &branch = tree.partitions[branchId.index - 1];
            if (branch.attrs.phase != CpuPhase::General) continue;
            for (const auto childId : branch.children)
                if (tree.partitions[childId.index - 1].attrs.kind == CpuPartitionKind::Supernode)
                    order.push_back(childId);
        }
        return order;
    }

    // Op -> owning General supernode (invalid for Event/Mem/Output ops).
    inline std::vector<PartitionId> generalSupernodeOf(const GrhSimModel &model, const CpuPartitionTree &tree)
    {
        std::vector<PartitionId> owner(model.operations().size() + 1);
        for (const auto supernode : generalSupernodeOrder(tree))
            for (const auto node : tree.partitions[supernode.index - 1].children)
                for (const auto op : tree.partitions[node.index - 1].ops)
                    owner[op.index] = supernode;
        return owner;
    }

    // Sorted unique (event,edge) cluster indices from the edgeDet act
    // parameters. edgeDetCount is the raw op count; the two agree unless the
    // model verifier's act-uniqueness rule is broken.
    inline std::vector<int64_t> eventClusterActs(const GrhSimModel &model, uint32_t &edgeDetCount)
    {
        std::vector<int64_t> acts;
        edgeDetCount = 0;
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) != "core.event.edgeDet") continue;
            ++edgeDetCount;
            const Parameter *act = findCpuPhaseParameter(model, model.parameters(op), "act");
            const auto *index = act ? std::get_if<int64_t>(&act->value) : nullptr;
            if (!index || *index < 0) throw std::runtime_error("edgeDet act must be a non-negative int64");
            acts.push_back(*index);
        }
        std::sort(acts.begin(), acts.end());
        acts.erase(std::unique(acts.begin(), acts.end()), acts.end());
        return acts;
    }

    enum class CpuBoundaryUse : uint8_t
    {
        NonSink,
        SmallSinkEnable,
        SmallSinkPayload,
        LargeSinkEnable,
        LargeSinkPayload
    };

    inline uint8_t cpuBoundaryUseBit(CpuBoundaryUse use) noexcept
    { return uint8_t{1} << static_cast<uint8_t>(use); }

    // A produced value can cross more than one boundary, so its uses are a
    // mask rather than an exclusive classification.
    inline std::vector<uint8_t> sixPhaseBoundaryUses(const GrhSimModel &model,
                                                      const CpuPartitionTree &tree,
                                                      std::span<const PartitionId> supernodeOf)
    {
        std::vector<OpId> producer(model.values().size() + 1);
        for (const auto &op : model.operations())
            for (const auto value : model.results(op)) producer[value.index] = op.id;
        std::vector<uint8_t> uses(model.values().size() + 1, 0);
        for (const auto &op : model.operations())
        {
            const bool largeSinkConsumer = op.phase == SimPhase::LargeSink;
            const auto consumer = supernodeOf[op.id.index];
            const auto operands = model.operands(op);
            for (std::size_t i = 0; i < operands.size(); ++i)
            {
                const auto operand = operands[i];
                const auto source = producer[operand.index];
                if (!source) continue;
                const auto sourceSupernode = supernodeOf[source.index];
                if (largeSinkConsumer)
                {
                    const auto use = i == 0 && isCpuPhaseMemWriteOp(model.text(op.opType))
                        ? CpuBoundaryUse::LargeSinkEnable : CpuBoundaryUse::LargeSinkPayload;
                    uses[operand.index] |= cpuBoundaryUseBit(use);
                }
                else if (consumer && consumer != sourceSupernode)
                {
                    const auto &attrs = tree.partitions[consumer.index - 1].attrs;
                    CpuBoundaryUse use = CpuBoundaryUse::NonSink;
                    if (attrs.supernodeCategory != CpuSupernodeCategory::NonSink)
                    {
                        const auto opType = model.text(op.opType);
                        const bool write = isCpuPhaseMemWriteOp(opType) ||
                                           opType == "core.state.latchWrite";
                        use = write && i == 0 ? CpuBoundaryUse::SmallSinkEnable
                                              : CpuBoundaryUse::SmallSinkPayload;
                    }
                    if (use != CpuBoundaryUse::NonSink || sourceSupernode)
                        uses[operand.index] |= cpuBoundaryUseBit(use);
                }
            }
        }
        return uses;
    }

    inline std::vector<bool> sixPhaseBoundaryValues(const GrhSimModel &model, const CpuPartitionTree &tree,
                                                    std::span<const PartitionId> supernodeOf)
    {
        const auto uses = sixPhaseBoundaryUses(model, tree, supernodeOf);
        std::vector<bool> boundary(uses.size(), false);
        for (std::size_t i = 0; i < uses.size(); ++i) boundary[i] = uses[i] != 0;
        return boundary;
    }

    inline std::vector<ValueId> cpuEnableBitmapValues(const GrhSimModel &model,
                                                      const CpuNamedStore &enableStore,
                                                      SimPhase phase)
    {
        std::vector<bool> inStore(model.values().size() + 1, false);
        std::vector<bool> seen(model.values().size() + 1, false);
        std::vector<OpId> producer(model.values().size() + 1);
        for (const auto &field : enableStore.fields)
            if (field.value) inStore[field.value.index] = true;
        for (const auto &op : model.operations())
            for (const auto value : model.results(op)) producer[value.index] = op.id;
        std::vector<ValueId> values;
        for (const auto &op : model.operations())
        {
            if (op.phase != phase) continue;
            const auto name = model.text(op.opType);
            if (!isCpuPhaseMemWriteOp(name) &&
                !(phase == SimPhase::General && name == "core.state.latchWrite")) continue;
            const auto operands = model.operands(op);
            if (operands.empty()) continue;
            const auto enable = operands.front();
            if (seen[enable.index]) continue;
            const auto &type = model.types()[model.values()[enable.index - 1].type.index - 1];
            if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                type.width != 1 || !type.shape.empty()) continue;
            const auto source = producer[enable.index];
            if (source && model.text(model.operations()[source.index - 1].opType) ==
                              "core.compute.constant") continue;
            if (!inStore[enable.index])
            {
                if (!source) continue;
                const auto &sourceOp = model.operations()[source.index - 1];
                if (model.text(sourceOp.opType) != "core.input.read") continue;
                const auto refs = model.objectRefs(sourceOp);
                if (refs.size() != 1 || refs[0].kind != ObjectKind::Input) continue;
            }
            seen[enable.index] = true;
            values.push_back(enable);
        }
        return values;
    }

    inline uint32_t estimatedCpuOpWork(const GrhSimModel &model, const SimOp &op)
    {
        if (model.text(op.opType) == "cpu.compute.maskedOr")
        {
            return static_cast<uint32_t>(std::min<uint64_t>(2 * maskedOrTermCount(model, op) - 1,
                                                          std::numeric_limits<uint32_t>::max()));
        }
        return 1;
    }

    // Emitted-line estimate for one op, shared by the C6 function packing and
    // the C8 TU planning heuristics (M5d-7). A heuristic, not a contract: it
    // only steers chunk sizes, never correctness.
    inline uint64_t estimatedCpuOpLines(const GrhSimModel &model, OpId id)
    {
        const auto &op = model.operations()[id.index - 1];
        uint64_t lines = 4 + op.operands.count + op.results.count;
        if (model.text(op.opType) == "cpu.compute.maskedOr")
            lines += 12 + (maskedOrViews(model, op) ? 2 * maskedOrTermCount(model, op) : 0);
        for (auto value : model.results(op))
        {
            const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
            if (type.kind == TypeKind::Logic)
            {
                if (type.width > 64)
                {
                    const uint64_t words = (uint64_t(type.width) + 63) / 64;
                    // Moderate wide ops emit as one native _BitInt expression.
                    // Ultra-wide ops (multi-KiBit values from whole-array
                    // reads) cost clang superlinearly per op AND per sibling
                    // op count in the same function (measured on XS aggregate:
                    // 32KiBit op chains run ~25ms/op alone, ~95ms/op at 256
                    // siblings — -O3 legalization); charge quadratically so
                    // the chunk packer splits those bodies.
                    lines += words <= 16 ? 2 : words * words / 128;
                }
            }
        }
        return lines;
    }

    // Flat op list of an Event/Output phase branch (its single EmitFunction
    // leaf's ops, M5d-6 tree shape). Empty when the branch holds no leaf.
    inline std::vector<OpId> cpuFlatBranchOps(const CpuPartitionTree &tree, CpuPhase phase)
    {
        const auto &root = tree.partitions[tree.root.index - 1];
        for (const auto branchId : root.children)
        {
            const auto &branch = tree.partitions[branchId.index - 1];
            if (branch.attrs.phase != phase || branch.children.empty()) continue;
            return tree.partitions[branch.children.front().index - 1].ops;
        }
        return {};
    }

    // V3-M2: deterministic C6 helperChunks -> SupernodePart ranges for a split
    // supernode. C8 uses it to size and number the SupernodePart chunks; the
    // emitter re-runs it with the same unit cap to map a chunk's part index
    // back to its helper range — both sides must stay bit-identical. Greedy:
    // a part closes when the next helper would push it past the cap (a lone
    // helper may exceed the cap). The returned ranges tile
    // attrs.helperChunks in order; each carries its estimated lines.
    inline std::vector<std::pair<Range, uint64_t>> cpuSupernodePartRanges(const GrhSimModel &model,
                                                                          const CpuPartitionTree &tree,
                                                                          PartitionId supernode,
                                                                          uint64_t cap)
    {
        const auto &partition = tree.partitions[supernode.index - 1];
        std::vector<OpId> ops;
        for (const auto node : partition.children)
            for (const auto op : tree.partitions[node.index - 1].ops) ops.push_back(op);
        std::vector<uint64_t> helperLines;
        helperLines.reserve(partition.attrs.helperChunks.size());
        for (const auto range : partition.attrs.helperChunks)
        {
            uint64_t lines = 2;
            for (uint32_t i = 0; i < range.count; ++i) lines += estimatedCpuOpLines(model, ops[range.offset + i]);
            helperLines.push_back(lines);
        }
        std::vector<std::pair<Range, uint64_t>> parts;
        uint32_t begin = 0;
        uint64_t lines = 0;
        for (uint32_t i = 0; i < helperLines.size(); ++i)
        {
            if (lines > 0 && lines + helperLines[i] > cap)
            {
                parts.push_back({Range{begin, i - begin}, lines});
                begin = i;
                lines = 0;
            }
            lines += helperLines[i];
        }
        if (begin < helperLines.size())
            parts.push_back({Range{begin, static_cast<uint32_t>(helperLines.size()) - begin}, lines});
        return parts;
    }

    // The init stream emitted by init() and its cpu_init_<k> chunks (M5d-7):
    // flattened init steps, then one constant-boundary preload per boundary
    // field whose producer is a constant, then one prevEvent init per edgeDet
    // op, then the final smallSinkStoreNext sync line.
    struct CpuInitStreamTotals
    {
        uint64_t steps = 0;
        uint64_t boundaryPreloads = 0;
        uint64_t prevEvents = 0;
        uint64_t total() const { return steps + boundaryPreloads + prevEvents + 1; }
    };

    inline CpuInitStreamTotals cpuInitStreamTotals(const GrhSimModel &model,
                                                  std::span<const CpuNamedStore> stores)
    {
        CpuInitStreamTotals totals;
        std::vector<OpId> producer(model.values().size() + 1);
        for (const auto &op : model.operations())
        {
            for (const auto value : model.results(op)) producer[value.index] = op.id;
            if (model.text(op.opType) == "core.event.edgeDet") ++totals.prevEvents;
        }
        for (const auto &record : model.initRecords()) totals.steps += model.steps(record).size();
        for (const auto &store : stores)
            for (const auto &field : store.fields)
            {
                if (!field.value || store.kind == CpuNamedStoreKind::PrevEvent) continue;
                const auto source = producer[field.value.index];
                if (source && model.text(model.operations()[source.index - 1].opType) == "core.compute.constant")
                    ++totals.boundaryPreloads;
            }
        return totals;
    }

    // The canonical dump item list (dumpState and its cpu_dump_<k> chunks):
    // input ports, output ports, then the named-store fields in store order.
    inline uint64_t cpuDumpItemCount(const GrhSimModel &model, std::span<const CpuNamedStore> stores)
    {
        uint64_t count = model.inputs().size() + model.outputs().size();
        for (const auto &store : stores) count += store.fields.size();
        return count;
    }

    // Parses a core.compute.constant constValue literal ("4'h5", "1'b1",
    // "16'h0000", plain decimal). Returns nullopt for x/z digits or overflow.
    inline std::optional<uint64_t> parseCpuConstLiteral(std::string_view text)
    {
        std::string digits;
        digits.reserve(text.size());
        for (const char c : text)
            if (c != '_') digits.push_back(c);
        unsigned base = 10;
        const auto tick = digits.find('\'');
        if (tick != std::string::npos)
        {
            if (tick + 1 >= digits.size()) return std::nullopt;
            const char marker = digits[tick + 1];
            if (marker == 'b' || marker == 'B') base = 2;
            else if (marker == 'o' || marker == 'O') base = 8;
            else if (marker == 'd' || marker == 'D') base = 10;
            else if (marker == 'h' || marker == 'H') base = 16;
            else return std::nullopt;
            digits = digits.substr(tick + 2);
        }
        if (digits.empty()) return std::nullopt;
        uint64_t value = 0;
        for (const char c : digits)
        {
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
            else return std::nullopt;
            if (digit >= base) return std::nullopt;
            if (value > (~uint64_t{0} - digit) / base) return std::nullopt;
            value = value * base + digit;
        }
        return value;
    }
}

#endif
