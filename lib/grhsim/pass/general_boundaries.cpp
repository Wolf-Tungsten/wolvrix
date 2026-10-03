// predictGeneralBoundaries (M5d-5, B7 归位决议 1; V2-M1 更新): mirror of the
// cpu.st.build-general-nodes cone-absorption rules (formNodes in
// backend/cpu_partition.cpp) over the sealed model's General-phase op set,
// producing the predicted node ownership and boundary value set without
// building a CpuBackendMapping. Keep the two implementations aligned:
// ComputeGraph's use lists cover every model op (out-of-set consumers defeat
// absorption as commit boundaries), the ready stack is LIFO over the op-set
// scan order reversed, and node ids follow reverse-topo discovery order.
// V2-M1: the node-formation op set holds only NON-SINK General ops
// (value-producing); no-result sink ops never enter cone absorption — each
// anchors a singleton node appended in op-id order, so their operand values
// are predicted boundary values by construction.
// C1 (M5d-6) consumes the same helper so both passes share one rule.

#include "grhsim/pass/general_boundaries.hpp"

#include <algorithm>
#include <numeric>
#include <string_view>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        bool absorbableType(std::string_view type) noexcept
        {
            return type.starts_with("core.compute.") || type == "core.input.read" ||
                   type == "core.state.read" || type == "core.state.memRead";
        }

        bool isMemWriteOpType(std::string_view type) noexcept
        {
            return type == "core.state.memWrite" || type == "core.state.memFill" ||
                   type == "core.state.memAssign" || type == "core.state.memWriteSeq";
        }
    } // namespace

    GeneralBoundaryPrediction predictGeneralBoundaries(const GrhSimModel &model,
                                                       uint32_t maxOpsPerNode)
    {
        GeneralBoundaryPrediction prediction;
        const std::size_t opCount = model.operations().size();
        const std::size_t valueCount = model.values().size();
        constexpr uint32_t kNoNode = GeneralBoundaryPrediction::kNoNode;
        prediction.nodeOfOp.assign(opCount + 1, kNoNode);
        prediction.boundaryValue.assign(valueCount + 1, 0);

        // Op set: every non-sink General-phase op (value-producing). Sink ops
        // (no results: reg/latch writes, General-phase regLatch-class mem
        // writes, no-result calls) are collected separately and anchor
        // singleton nodes after the non-sink nodes, mirroring C1.
        std::vector<OpId> ops, sinks;
        std::vector<uint8_t> inSet(opCount + 1, 0);
        for (const auto &op : model.operations())
        {
            if (op.phase != SimPhase::General) continue;
            if (model.results(op).empty()) { sinks.push_back(op.id); continue; }
            ops.push_back(op.id);
            inSet[op.id.index] = 1;
        }
        if (ops.empty() && sinks.empty()) return prediction;

        // Producer/use tables over the whole model (use lists deliberately
        // include out-of-set consumers: they defeat absorption, mirroring
        // ComputeGraph's commit-boundary rule; sink consumers are out of the
        // set by construction, so sink operand cones never absorb into a
        // sink's node).
        std::vector<OpId> producer(valueCount + 1);
        std::vector<uint32_t> useOffsets(valueCount + 2, 0);
        for (const auto &op : model.operations())
        {
            for (auto value : model.results(op)) producer[value.index] = op.id;
            for (auto value : model.operands(op)) ++useOffsets[value.index + 1];
        }
        std::partial_sum(useOffsets.begin(), useOffsets.end(), useOffsets.begin());
        std::vector<OpId> uses(useOffsets.back());
        auto cursor = useOffsets;
        for (const auto &op : model.operations())
            for (auto value : model.operands(op)) uses[cursor[value.index]++] = op.id;

        // Kahn topo over in-set value edges (LIFO ready stack, scan order
        // reversed — identical to ComputeGraph). An operand produced outside
        // the set is a partition-interface root, not an error here.
        std::vector<uint32_t> indegree(opCount + 1, 0);
        std::vector<OpId> ready;
        std::vector<uint8_t> inTopo(opCount + 1, 0);
        for (auto id : ops)
        {
            for (auto value : model.operands(model.operations()[id.index - 1]))
            {
                const auto source = producer[value.index];
                if (source && inSet[source.index]) ++indegree[id.index];
            }
            if (indegree[id.index] == 0) ready.push_back(id);
        }
        std::reverse(ready.begin(), ready.end());
        std::vector<OpId> topo;
        topo.reserve(ops.size());
        while (!ready.empty())
        {
            const auto id = ready.back();
            ready.pop_back();
            topo.push_back(id);
            inTopo[id.index] = 1;
            for (auto value : model.results(model.operations()[id.index - 1]))
                for (uint32_t i = useOffsets[value.index]; i < useOffsets[value.index + 1]; ++i)
                    if (inSet[uses[i].index] && --indegree[uses[i].index] == 0)
                        ready.push_back(uses[i]);
        }
        // Combinational cycles cannot occur in a verified model; degrade
        // gracefully by anchoring any leftover op in its own node below.
        for (auto id : ops)
            if (!inTopo[id.index]) topo.push_back(id);

        // Reverse-topo cone absorption (mirror of formNodes): an absorbable
        // op joins the single agreed node of its results' in-set users.
        std::vector<uint32_t> owner(opCount + 1, kNoNode), sizes;
        for (auto it = topo.rbegin(); it != topo.rend(); ++it)
        {
            const auto &op = model.operations()[it->index - 1];
            bool absorb = absorbableType(model.text(op.opType));
            uint32_t target = kNoNode;
            for (auto value : model.results(op))
                for (uint32_t i = useOffsets[value.index]; i < useOffsets[value.index + 1]; ++i)
                {
                    const auto user = uses[i];
                    if (!inSet[user.index]) { absorb = false; continue; }
                    if (target == kNoNode) target = owner[user.index];
                    else if (target != owner[user.index]) absorb = false;
                }
            if (!absorb || target == kNoNode || sizes[target] >= maxOpsPerNode)
            {
                target = static_cast<uint32_t>(sizes.size());
                sizes.push_back(0);
            }
            owner[op.id.index] = target;
            ++sizes[target];
        }
        prediction.nodeCount = static_cast<uint32_t>(sizes.size());
        prediction.nodeOfOp = std::move(owner);
        // Sink singleton nodes trail the non-sink nodes in op-id order.
        for (auto id : sinks) prediction.nodeOfOp[id.index] = prediction.nodeCount++;

        // Predicted boundary values: a General-produced value is a boundary
        // when it crosses predicted nodes (sink singleton nodes included), is
        // sampled by a Mem-phase write, or leaves the partition any other way
        // (conservative — the seal forbids Event/Output consumers of General
        // values).
        for (const auto &op : model.operations())
        {
            const uint32_t consumerNode = prediction.nodeOfOp[op.id.index];
            const bool memConsumer =
                op.phase == SimPhase::Mem && isMemWriteOpType(model.text(op.opType));
            for (auto operand : model.operands(op))
            {
                const auto source = producer[operand.index];
                if (!source) continue;
                const uint32_t sourceNode = prediction.nodeOfOp[source.index];
                if (sourceNode == kNoNode) continue;
                if (memConsumer || consumerNode == kNoNode || consumerNode != sourceNode)
                    prediction.boundaryValue[operand.index] = 1;
            }
        }
        return prediction;
    }

} // namespace wolvrix::lib::grhsim
