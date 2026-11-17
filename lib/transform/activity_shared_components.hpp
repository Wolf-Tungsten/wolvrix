#ifndef WOLVRIX_ACTIVITY_SHARED_COMPONENTS_HPP
#define WOLVRIX_ACTIVITY_SHARED_COMPONENTS_HPP

#include "activity_shared_refinement.hpp"
#include <numeric>
#include <set>

namespace wolvrix::lib::transform::detail
{
    struct SharedComponentNode
    {
        bool work = false;
        bool activationKnown = true;
        // External values use their exact ValueId token. Register reads use a
        // separate token namespace, assigned by the caller to state symbols.
        std::vector<std::pair<uint32_t, uint64_t>> inputs;
        std::vector<uint64_t> states;
    };

    struct SharedComponents
    {
        std::vector<std::vector<uint32_t>> classes;
        std::size_t workComponents = 0;
        std::size_t equalActivationJoins = 0;
        std::size_t noContraction = 0;
    };

    // Whole nodes only. Read/constant edges do not establish computational
    // connectivity. A read-only node follows a component only when all of its
    // outgoing edges are already owned by that one component.
    inline SharedComponents connectedSharedClasses(
        const std::vector<std::vector<uint32_t>> &nodeDag,
        const std::vector<std::vector<uint32_t>> &workDag,
        const std::vector<uint32_t> &oldGroup,
        const std::vector<std::vector<uint32_t>> &terminalClasses,
        const std::vector<SharedComponentNode> &nodes)
    {
        const auto count = nodes.size();
        if (nodeDag.size() != count || workDag.size() != count || oldGroup.size() != count)
            throw std::logic_error("shared components inconsistent node inventory");
        std::vector<uint32_t> parent(count), category(count, noSharedGroup);
        std::iota(parent.begin(), parent.end(), 0u);
        std::vector<bool> work(count), equivalent(count, false);
        for (uint32_t n = 0; n < count; ++n) work[n] = nodes[n].work;
        for (uint32_t c = 0; c < terminalClasses.size(); ++c)
            for (const auto n : terminalClasses[c])
            {
                if (n >= count || category[n] != noSharedGroup)
                    throw std::logic_error("shared components invalid terminal class");
                category[n] = c;
            }
        const auto root = [&](uint32_t n) {
            while (parent[n] != n) { parent[n] = parent[parent[n]]; n = parent[n]; }
            return n;
        };
        const auto join = [&](uint32_t a, uint32_t b) {
            a = root(a); b = root(b);
            if (a == b) return;
            if (a > b) std::swap(a, b);
            parent[b] = a; work[a] = work[a] || work[b];
            equivalent[a] = equivalent[a] || equivalent[b];
        };
        for (uint32_t n = 0; n < count; ++n)
            if (category[n] != noSharedGroup)
                for (const auto next : workDag[n])
                    if (category.at(next) == category[n]) join(n, next);
        const auto attachReads = [&]() {
            for (uint32_t n = 0; n < count; ++n)
            {
                if (category[n] == noSharedGroup || work[root(n)] || nodeDag[n].empty()) continue;
                uint32_t destination = noSharedGroup;
                bool exclusive = true;
                for (const auto next : nodeDag[n])
                {
                    const auto r = root(next);
                    if (category[next] != category[n] || !work[r] ||
                        (destination != noSharedGroup && destination != r)) { exclusive = false; break; }
                    destination = r;
                }
                if (exclusive) join(n, destination);
            }
        };
        attachReads();
        std::map<uint32_t, std::vector<uint32_t>> components;
        for (const auto &c : terminalClasses)
            for (const auto n : c) if (work[root(n)]) components[root(n)].push_back(n);
        SharedComponents result;
        result.workComponents = components.size();
        std::map<std::pair<uint32_t, std::vector<uint64_t>>, uint32_t> frontiers;
        for (const auto &[r, members] : components)
        {
            std::vector<uint64_t> activation;
            bool known = true;
            for (const auto n : members)
            {
                known = known && nodes[n].activationKnown;
                activation.insert(activation.end(), nodes[n].states.begin(), nodes[n].states.end());
                for (const auto &[producer, value] : nodes[n].inputs)
                    if (producer == noSharedGroup || root(producer) != root(r)) activation.push_back(value);
            }
            if (!known || activation.empty()) continue;
            std::sort(activation.begin(), activation.end());
            activation.erase(std::unique(activation.begin(), activation.end()), activation.end());
            const auto [it, inserted] = frontiers.emplace(std::make_pair(category[r], std::move(activation)), r);
            if (!inserted)
            {
                join(r, it->second); equivalent[root(r)] = true;
                ++result.equalActivationJoins;
            }
        }
        attachReads();
        components.clear();
        for (const auto &c : terminalClasses)
            for (const auto n : c) if (work[root(n)]) components[root(n)].push_back(n);
        // A useful computational contraction must cross original owners, not
        // merely extract an already-local tree or attach its source registers.
        std::set<uint32_t> crossing;
        for (uint32_t n = 0; n < count; ++n)
            if (category[n] != noSharedGroup)
                for (const auto next : workDag[n])
                    if (root(n) == root(next) && oldGroup[n] != oldGroup[next]) crossing.insert(root(n));
        for (auto &[r, members] : components)
        {
            std::set<uint32_t> owners;
            for (const auto n : members) if (nodes[n].work) owners.insert(oldGroup[n]);
            if (owners.size() < 2 || (!crossing.contains(r) && !equivalent[r]))
            { ++result.noContraction; continue; }
            std::sort(members.begin(), members.end());
            result.classes.push_back(std::move(members));
        }
        // Keep endpoint order and stable minimum node order within an endpoint.
        std::sort(result.classes.begin(), result.classes.end(), [&](const auto &a, const auto &b) {
            return std::make_pair(category[a.front()], a.front()) < std::make_pair(category[b.front()], b.front());
        });
        return result;
    }
}
#endif
