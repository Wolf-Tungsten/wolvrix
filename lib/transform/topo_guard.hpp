#pragma once

// Shared topological cycle-guard infrastructure for transform passes
// (stage-07 compile-time fix). Passes that rewrite `replaceUsers(r, o)`-style
// edges historically guarded each rewrite with a fresh cone DFS
// (dependsTransitivelyOn) — O(cone) with fat Value/Operation materialization
// per node, quadratic across a sweep. The guard here computes one
// topological value ranking per sweep: valueRank strictly increases along
// every def-use edge, so valueRank[o] <= valueRank[r] proves o cannot
// transitively depend on r and the DFS can be skipped. The sweep's rewrites
// preserve the ranking because they only add edges from a non-downstream
// replacement to the replaced value's users; values created mid-sweep or
// belonging to residual comb cycles carry no rank and fall back to the
// precise (lean, stamp-based) DFS.

#include "core/grh.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wolvrix::lib::transform::topo_guard
{

    struct ConeDfsScratch
    {
        std::vector<uint64_t> stamp;
        std::vector<uint32_t> generation;
        uint64_t nextStamp = 0;
        std::vector<grh::ValueId> stack;
        // Diagnostics: newly visited nodes across all queries on this scratch.
        uint64_t nodesExpanded = 0;
    };

    // Lean cone-dependency DFS: Graph id/span accessors only (no Value/
    // Operation materialization), stamp-marked visited array instead of a
    // fresh unordered_set per query.
    inline bool dependsTransitivelyOn(const grh::Graph &graph,
                                      grh::ValueId valueId,
                                      grh::ValueId targetId,
                                      ConeDfsScratch &scratch)
    {
        if (!valueId.valid() || !targetId.valid())
        {
            return false;
        }
        if (scratch.stamp.size() <= valueId.index || scratch.stamp.size() <= targetId.index)
        {
            scratch.stamp.resize(std::max(valueId.index, targetId.index) + 1, 0);
            scratch.generation.resize(scratch.stamp.size(), 0);
        }
        const uint64_t stamp = ++scratch.nextStamp;
        scratch.stack.clear();
        scratch.stack.push_back(valueId);
        auto markVisited = [&](grh::ValueId id) -> bool {
            if (id.index >= scratch.stamp.size())
            {
                scratch.stamp.resize(id.index + 1, 0);
                scratch.generation.resize(id.index + 1, 0);
            }
            if (scratch.generation[id.index] != id.generation)
            {
                scratch.generation[id.index] = id.generation;
                scratch.stamp[id.index] = 0;
            }
            if (scratch.stamp[id.index] == stamp)
            {
                return false;
            }
            scratch.stamp[id.index] = stamp;
            return true;
        };
        while (!scratch.stack.empty())
        {
            const grh::ValueId current = scratch.stack.back();
            scratch.stack.pop_back();
            if (!current.valid())
            {
                continue;
            }
            if (current == targetId)
            {
                return true;
            }
            if (!markVisited(current))
            {
                continue;
            }
            ++scratch.nodesExpanded;
            const grh::OperationId defOpId = graph.valueDef(current);
            if (!defOpId.valid())
            {
                continue;
            }
            for (const auto operandId : graph.opOperands(defOpId))
            {
                if (operandId.valid())
                {
                    scratch.stack.push_back(operandId);
                }
            }
        }
        return false;
    }

    struct TopoValueRanks
    {
        std::vector<int64_t> rank;        // by ValueId.index; -1 = unranked
        std::vector<uint32_t> generation; // generation stamp per index
        std::size_t unrankedValues = 0;
        std::size_t totalValues = 0;
        // Guard counters (diagnostics): fast-path clears vs DFS fallbacks.
        mutable std::size_t fastSafe = 0;
        mutable std::size_t dfsFallback = 0;
        // Fallback breakdown: unranked/stale id side vs genuine rank inversion.
        mutable std::size_t fallbackUnranked = 0;
        mutable std::size_t fallbackOrdered = 0;

        // Rank of a value, or -1 for invalid/stale-generation/unranked ids.
        int64_t rankOf(grh::ValueId value) const
        {
            if (!value.valid() || value.index >= rank.size()
                || generation[value.index] != value.generation)
            {
                return -1;
            }
            return rank[value.index];
        }

        bool definitelyNotDependent(grh::ValueId operand, grh::ValueId target) const
        {
            if (!operand.valid() || !target.valid())
            {
                return true;
            }
            const int64_t operandRank = rankOf(operand);
            const int64_t targetRank = rankOf(target);
            if (operandRank < 0 || targetRank < 0)
            {
                ++dfsFallback;
                ++fallbackUnranked;
                return false;
            }
            const bool safe = operandRank <= targetRank;
            if (safe)
            {
                ++fastSafe;
            }
            else
            {
                ++dfsFallback;
                ++fallbackOrdered;
            }
            return safe;
        }
    };

    // Whether `from` may be aliased to `to` under Graph::replaceAllUses'
    // shape contract: yes when shapes match, when `from` is flat (no shaped
    // consumers exist), or when `to` is flat (the core backfills `from`'s
    // shape). Two different non-empty shapes may never alias — the rewrite
    // must keep a conversion op between the two views instead.
    inline bool shapesAliasable(const grh::Graph &graph, grh::ValueId from, grh::ValueId to)
    {
        if (!from.valid() || !to.valid())
        {
            return true;
        }
        if (graph.valueWidth(from) != graph.valueWidth(to) ||
            graph.valueType(from) != graph.valueType(to) ||
            graph.valueDomain(from) != graph.valueDomain(to)) return false;
        const std::span<const int64_t> fromShape = graph.valueShape(from);
        if (fromShape.empty())
        {
            return true;
        }
        const std::span<const int64_t> toShape = graph.valueShape(to);
        if (toShape.empty())
        {
            return true;
        }
        return std::equal(fromShape.begin(), fromShape.end(), toShape.begin(), toShape.end());
    }

    inline TopoValueRanks computeTopoValueRanks(const grh::Graph &graph)
    {
        TopoValueRanks result;
        const auto ops = graph.operations();
        const auto values = graph.values();
        std::size_t maxOpIndex = 0;
        std::size_t maxValueIndex = 0;
        for (const auto op : ops)
        {
            maxOpIndex = std::max<std::size_t>(maxOpIndex, op.index);
        }
        for (const auto value : values)
        {
            maxValueIndex = std::max<std::size_t>(maxValueIndex, value.index);
        }

        // Kahn over ops (longest-path layering). Edge multiplicities are
        // counted per operand slot on both the indegree and the CSR
        // successor sides, so no dedup is needed. Self edges are skipped.
        const std::size_t opSlots = maxOpIndex + 1;
        std::vector<uint32_t> outdegree(opSlots, 0);
        std::vector<uint32_t> indegree(opSlots, 0);
        for (const auto op : ops)
        {
            for (const auto operand : graph.opOperands(op))
            {
                const grh::OperationId def = graph.valueDef(operand);
                if (def.valid() && def != op)
                {
                    ++outdegree[def.index];
                    ++indegree[op.index];
                }
            }
        }
        std::vector<uint64_t> offsets(opSlots + 1, 0);
        for (std::size_t i = 0; i < opSlots; ++i)
        {
            offsets[i + 1] = offsets[i] + outdegree[i];
        }
        std::vector<grh::OperationId> successors(offsets.back());
        std::vector<uint64_t> cursor(offsets.begin(), offsets.end() - 1);
        for (const auto op : ops)
        {
            for (const auto operand : graph.opOperands(op))
            {
                const grh::OperationId def = graph.valueDef(operand);
                if (def.valid() && def != op)
                {
                    successors[cursor[def.index]++] = op;
                }
            }
        }

        std::vector<int64_t> opRank(opSlots, -1);
        std::vector<grh::OperationId> ready;
        ready.reserve(ops.size());
        for (const auto op : ops)
        {
            if (indegree[op.index] == 0)
            {
                opRank[op.index] = 0;
                ready.push_back(op);
            }
        }
        for (std::size_t head = 0; head < ready.size(); ++head)
        {
            const grh::OperationId current = ready[head];
            for (std::size_t i = offsets[current.index]; i < offsets[current.index + 1]; ++i)
            {
                const grh::OperationId next = successors[i];
                opRank[next.index] = std::max(opRank[next.index], opRank[current.index] + 1);
                if (--indegree[next.index] == 0)
                {
                    ready.push_back(next);
                }
            }
        }

        result.rank.assign(maxValueIndex + 1, -1);
        result.generation.assign(maxValueIndex + 1, 0);
        for (const auto value : values)
        {
            result.generation[value.index] = value.generation;
            const grh::OperationId def = graph.valueDef(value);
            if (!def.valid())
            {
                result.rank[value.index] = 0;
                continue;
            }
            if (def.index < opRank.size() && opRank[def.index] >= 0)
            {
                result.rank[value.index] = opRank[def.index] + 1;
                continue;
            }
            ++result.unrankedValues;
        }
        result.totalValues = values.size();
        return result;
    }

} // namespace wolvrix::lib::transform::topo_guard
