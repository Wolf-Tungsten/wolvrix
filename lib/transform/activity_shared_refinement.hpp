#ifndef WOLVRIX_ACTIVITY_SHARED_REFINEMENT_HPP
#define WOLVRIX_ACTIVITY_SHARED_REFINEMENT_HPP

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform::detail
{
    inline constexpr uint32_t noSharedGroup = std::numeric_limits<uint32_t>::max();
    struct SharedPartition
    {
        uint32_t originalGroup = noSharedGroup;
        uint32_t sharedClass = noSharedGroup;
        std::vector<uint32_t> nodes;
    };
    struct SharedRefinement
    {
        std::vector<SharedPartition> partitions;
        std::vector<bool> accepted;
        std::size_t rounds = 0;
        std::size_t splitGroups = 0;
        std::size_t rejected = 0;
    };

    // Iterative Kosaraju: only vertices in a nontrivial SCC need refinement.
    // A blocked Kahn frontier would also contain acyclic descendants of cycles.
    inline std::vector<bool> cyclicSharedPartitions(
        const std::vector<std::vector<uint32_t>> &nodeDag,
        const std::vector<SharedPartition> &partitions)
    {
        std::vector<uint32_t> owner(nodeDag.size(), noSharedGroup);
        for (uint32_t group = 0; group < partitions.size(); ++group)
            for (const auto node : partitions[group].nodes)
            {
                if (node >= owner.size() || owner[node] != noSharedGroup)
                    throw std::logic_error("shared refinement invalid node partition");
                owner[node] = group;
            }
        std::vector<std::vector<uint32_t>> dag(partitions.size()), reverse(partitions.size());
        for (uint32_t node = 0; node < nodeDag.size(); ++node)
        {
            if (owner[node] == noSharedGroup) continue;
            for (const auto target : nodeDag[node])
            {
                if (target >= owner.size()) throw std::logic_error("shared refinement invalid edge");
                if (owner[target] != noSharedGroup && owner[target] != owner[node])
                    dag[owner[node]].push_back(owner[target]);
            }
        }
        for (uint32_t group = 0; group < dag.size(); ++group)
        {
            auto &edges = dag[group];
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            for (const auto target : edges) reverse[target].push_back(group);
        }
        std::vector<bool> seen(dag.size(), false), cyclic(dag.size(), false);
        std::vector<uint32_t> finished;
        std::vector<std::pair<uint32_t, std::size_t>> stack;
        for (uint32_t root = 0; root < dag.size(); ++root)
        {
            if (seen[root]) continue;
            seen[root] = true;
            stack.emplace_back(root, 0);
            while (!stack.empty())
            {
                auto &[node, next] = stack.back();
                if (next < dag[node].size())
                {
                    const auto child = dag[node][next++];
                    if (!seen[child]) { seen[child] = true; stack.emplace_back(child, 0); }
                }
                else { finished.push_back(node); stack.pop_back(); }
            }
        }
        std::fill(seen.begin(), seen.end(), false);
        std::vector<uint32_t> work, component;
        for (auto it = finished.rbegin(); it != finished.rend(); ++it)
        {
            if (seen[*it]) continue;
            work.push_back(*it); seen[*it] = true; component.clear();
            while (!work.empty())
            {
                const auto node = work.back(); work.pop_back(); component.push_back(node);
                for (const auto child : reverse[node])
                    if (!seen[child]) { seen[child] = true; work.push_back(child); }
            }
            if (component.size() > 1)
                for (const auto node : component) cyclic[node] = true;
        }
        return cyclic;
    }

    inline SharedRefinement refineSharedPartitions(
        const std::vector<std::vector<uint32_t>> &nodeDag,
        const std::vector<std::vector<uint32_t>> &originalGroups,
        const std::vector<std::vector<uint32_t>> &terminals,
        const std::vector<std::vector<uint32_t>> &sharedClasses)
    {
        SharedRefinement result;
        result.accepted.assign(sharedClasses.size(), true);
        for (;;)
        {
            result.partitions.clear();
            std::vector<bool> moved(nodeDag.size(), false), split(originalGroups.size(), false);
            for (std::size_t i = 0; i < sharedClasses.size(); ++i)
                if (result.accepted[i]) for (const auto node : sharedClasses[i]) moved.at(node) = true;
            for (uint32_t group = 0; group < originalGroups.size(); ++group)
            {
                SharedPartition p{group, noSharedGroup, {}};
                for (const auto node : originalGroups[group]) if (!moved.at(node)) p.nodes.push_back(node);
                if (!p.nodes.empty()) result.partitions.push_back(std::move(p));
            }
            for (uint32_t i = 0; i < sharedClasses.size(); ++i)
                if (result.accepted[i]) result.partitions.push_back({noSharedGroup, i, sharedClasses[i]});
            for (;;)
            {
                const auto cyclic = cyclicSharedPartitions(nodeDag, result.partitions);
                if (std::none_of(cyclic.begin(), cyclic.end(), [](bool value) { return value; }))
                {
                    result.splitGroups = std::count(split.begin(), split.end(), true);
                    return result;
                }
                bool changed = false;
                std::vector<SharedPartition> refined;
                for (std::size_t i = 0; i < result.partitions.size(); ++i)
                {
                    const auto &p = result.partitions[i];
                    std::map<std::vector<uint32_t>, std::vector<uint32_t>> fragments;
                    if (cyclic[i] && p.originalGroup != noSharedGroup)
                        for (const auto node : p.nodes) fragments[terminals.at(node)].push_back(node);
                    if (fragments.size() > 1)
                    {
                        changed = true; split[p.originalGroup] = true;
                        for (auto &[key, nodes] : fragments)
                            refined.push_back({p.originalGroup, noSharedGroup, std::move(nodes)});
                    }
                    else refined.push_back(p);
                }
                if (changed)
                {
                    ++result.rounds; result.partitions = std::move(refined); continue;
                }
                // A protected/homogeneous remainder cannot be split further.
                // Withdraw only shared classes in cyclic components and retry
                // from the original groups; unrelated accepted classes survive.
                bool rejected = false;
                for (std::size_t i = 0; i < result.partitions.size(); ++i)
                    if (cyclic[i] && result.partitions[i].sharedClass != noSharedGroup)
                    {
                        result.accepted[result.partitions[i].sharedClass] = false;
                        ++result.rejected; rejected = true;
                    }
                if (!rejected) throw std::logic_error("shared refinement found cyclic original groups");
                break;
            }
        }
    }
}
#endif
