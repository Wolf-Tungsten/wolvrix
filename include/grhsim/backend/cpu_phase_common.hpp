#ifndef WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP
#define WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP

// Shared helpers for the six-phase layout/schedule passes and their
// verifiers (cpu_layout.cpp / cpu_schedule.cpp). Everything here is a pure
// function of the model plus the partition tree. M5d-6: the supernode
// ordinal is fixed by merge-general-supernodes (C2) as the General branch's
// child order; function packing (C6) only records intervals over it.

#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <cstdint>
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

    inline bool isCpuPhaseMemWriteOp(std::string_view type) noexcept
    {
        return type == "core.state.memWrite" || type == "core.state.memFill" ||
               type == "core.state.memAssign" || type == "core.state.memWriteSeq";
    }

    // The supernode ordinal (M5d-6, resolution 2): the General branch's
    // Supernode children in tree order — fixed at C2 and untouched by C6's
    // function packing (EmitFunction leaves trail the supernodes and are
    // skipped here). Partition ids never reach the emitter; this 0..N-1
    // sequence indexes the ActiveFlags byte arrays and the event bitmap
    // words.
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

    // Boundary-store value set: General-produced values consumed across a
    // supernode boundary or sampled by a Mem-phase write op. Event/Output
    // cones are self-contained (they read stores), so they never extend the
    // set. M5d-6: General-phase regLatch-class mem writes live inside
    // General supernodes, so their operands follow the normal
    // cross-supernode rule; only Mem-phase (mem-class) writes sample
    // unconditionally.
    inline std::vector<bool> sixPhaseBoundaryValues(const GrhSimModel &model, const CpuPartitionTree &tree,
                                                    std::span<const PartitionId> supernodeOf)
    {
        std::vector<OpId> producer(model.values().size() + 1);
        for (const auto &op : model.operations())
            for (const auto value : model.results(op)) producer[value.index] = op.id;
        std::vector<bool> boundary(model.values().size() + 1, false);
        for (const auto &op : model.operations())
        {
            const bool memConsumer = op.phase == SimPhase::Mem;
            const auto consumer = supernodeOf[op.id.index];
            for (const auto operand : model.operands(op))
            {
                const auto source = producer[operand.index];
                if (!source) continue;
                const auto sourceSupernode = supernodeOf[source.index];
                if (memConsumer)
                {
                    if (sourceSupernode) boundary[operand.index] = true;
                }
                else if (consumer && sourceSupernode && consumer != sourceSupernode)
                    boundary[operand.index] = true;
            }
        }
        return boundary;
    }

    // Emitted-line estimate for one op, shared by the C6 function packing and
    // the C8 TU planning heuristics (M5d-7). A heuristic, not a contract: it
    // only steers chunk sizes, never correctness.
    inline uint64_t estimatedCpuOpLines(const GrhSimModel &model, OpId id)
    {
        const auto &op = model.operations()[id.index - 1];
        uint64_t lines = 4 + op.operands.count + op.results.count;
        for (auto value : model.results(op))
        {
            const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
            if (type.kind == TypeKind::Logic) lines += (uint64_t(type.width) + 63) / 64;
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

    // The init stream emitted by init() and its cpu_init_<k> chunks (M5d-7):
    // flattened init steps, then one constant-boundary preload per boundary
    // field whose producer is a constant, then one prevEvent init per edgeDet
    // op, then the final regLatchStoreNext sync line.
    struct CpuInitStreamTotals
    {
        uint64_t steps = 0;
        uint64_t boundaryPreloads = 0;
        uint64_t prevEvents = 0;
        uint64_t total() const { return steps + boundaryPreloads + prevEvents + 1; }
    };

    inline CpuInitStreamTotals cpuInitStreamTotals(const GrhSimModel &model, const CpuNamedStore &boundaryStore)
    {
        CpuInitStreamTotals totals;
        std::vector<OpId> producer(model.values().size() + 1);
        for (const auto &op : model.operations())
        {
            for (const auto value : model.results(op)) producer[value.index] = op.id;
            if (model.text(op.opType) == "core.event.edgeDet") ++totals.prevEvents;
        }
        for (const auto &record : model.initRecords()) totals.steps += model.steps(record).size();
        for (const auto &field : boundaryStore.fields)
        {
            if (!field.value) continue;
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
