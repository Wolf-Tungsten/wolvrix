#ifndef WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP
#define WOLVRIX_LIB_GRHSIM_BACKEND_CPU_PHASE_COMMON_HPP

// Shared helpers for the M4 six-phase layout/schedule passes and their
// verifiers (cpu_layout.cpp / cpu_schedule.cpp). Everything here is a pure
// function of the model plus the GeneralFunctions-stage partition tree.

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

    // The M4 supernode ordinal: the General branch flattened in tree order
    // (emit-function child order, then each function's supernode child order).
    // Partition ids never reach the emitter; this 0..N-1 sequence indexes the
    // ActiveFlags byte arrays and the event bitmap words.
    inline std::vector<PartitionId> generalSupernodeOrder(const CpuPartitionTree &tree)
    {
        std::vector<PartitionId> order;
        const auto &root = tree.partitions[tree.root.index - 1];
        for (const auto branchId : root.children)
        {
            const auto &branch = tree.partitions[branchId.index - 1];
            if (branch.attrs.phase != CpuPhase::General) continue;
            for (const auto functionId : branch.children)
                for (const auto supernodeId : tree.partitions[functionId.index - 1].children)
                    order.push_back(supernodeId);
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
    // supernode boundary or by a Mem-phase write op. Event/Output cones are
    // self-contained (they read stores), so they never extend the set.
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
