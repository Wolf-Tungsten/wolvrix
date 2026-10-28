#include "transform/repcut_initial_assignment.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace wolvrix::lib::transform
{
    namespace
    {
        using Features = std::array<uint64_t, 4>;
        using BundleKey = std::pair<uint32_t, uint64_t>;

        void add(uint64_t &target, uint64_t value)
        {
            if (value > std::numeric_limits<uint64_t>::max() - target)
                throw std::overflow_error("initial assignment accounting overflow");
            target += value;
        }

        void subtract(uint64_t &target, uint64_t value)
        {
            if (value > target) throw std::logic_error("initial assignment accounting underflow");
            target -= value;
        }

        void validateFeatures(const RepcutAssignmentModel &model)
        {
            const std::vector<std::string> expected = {
                "grhsim_v1_closure_work", "grhsim_v1_recurring_work",
                "raw_operation_count", "state_write_count"};
            if (model.featureNames != expected)
                throw std::invalid_argument("GrhSIM initial assignment requires raw-structure-v2 features");
        }

        uint64_t scaledLimit(uint64_t value, uint64_t numerator)
        {
            const uint64_t whole = value / 1000;
            if (whole > std::numeric_limits<uint64_t>::max() / numerator)
                throw std::overflow_error("initial assignment budget overflow");
            uint64_t result = whole * numerator;
            add(result, (value % 1000) * numerator / 1000);
            return result;
        }

        uint64_t maximum(const std::vector<uint64_t> &values)
        {
            return *std::max_element(values.begin(), values.end());
        }

        struct Loads
        {
            std::vector<Features> work;
            std::vector<uint64_t> sendCalls, sendBytes, receiveCalls, receiveBytes;
            Features total{};
            uint64_t calls = 0, bytes = 0;
            explicit Loads(uint32_t k)
                : work(k), sendCalls(k), sendBytes(k), receiveCalls(k), receiveBytes(k) {}

            uint64_t recurringMax() const
            {
                uint64_t result = 0;
                for (const auto &part : work) result = std::max(result, part[1]);
                return result;
            }

            bool permitsWork(const RepcutInitialBudget &budget) const
            {
                return recurringMax() <= budget.recurringMax && total[1] <= budget.recurringSum &&
                       total[2] <= budget.operationSum;
            }

            bool permits(const RepcutInitialBudget &budget, uint64_t cap) const
            {
                return permitsWork(budget) && calls <= budget.calls && bytes <= budget.bytes &&
                       maximum(sendCalls) <= budget.sendCalls && maximum(sendBytes) <= budget.sendBytes &&
                       maximum(receiveCalls) <= budget.receiveCalls &&
                       maximum(receiveBytes) <= std::min(cap, budget.receiveBytes);
            }

            double communicationExcess(const RepcutInitialBudget &budget, uint64_t cap) const
            {
                double excess = 0.0;
                const std::array<std::pair<uint64_t, uint64_t>, 6> limits = {{
                    {calls, budget.calls}, {bytes, budget.bytes},
                    {maximum(sendCalls), budget.sendCalls}, {maximum(sendBytes), budget.sendBytes},
                    {maximum(receiveCalls), budget.receiveCalls},
                    {maximum(receiveBytes), std::min(cap, budget.receiveBytes)}}};
                for (const auto &[value, limit] : limits)
                {
                    const double ratio = limit ? static_cast<double>(value) / limit :
                        value ? std::numeric_limits<double>::infinity() : 1.0;
                    excess = std::max(excess, ratio - 1.0);
                }
                return excess;
            }
        };

        class InitialState
        {
            const RepcutAssignmentModel &model_;
            std::vector<std::vector<uint32_t>> atomPieces_, atomValues_, atomWriters_;
            std::vector<uint64_t> pieceMasks_, valueMasks_;
            std::vector<uint32_t> owners_;
            std::map<BundleKey, uint64_t> widths_;

            static void changeTransfer(Loads &loads, const BundleKey &key,
                                       const RepcutTransferFeatures &cost, bool remove)
            {
                auto update = remove ? subtract : add;
                auto mask = key.second;
                while (mask)
                {
                    const auto part = std::countr_zero(mask);
                    mask &= mask - 1;
                    update(loads.sendCalls.at(key.first), cost.calls);
                    update(loads.sendBytes.at(key.first), cost.storageBytes);
                    update(loads.receiveCalls[part], cost.calls);
                    update(loads.receiveBytes[part], cost.storageBytes);
                    update(loads.calls, cost.calls);
                    update(loads.bytes, cost.storageBytes);
                }
            }

        public:
            std::vector<uint32_t> assignment;
            Loads loads;
            InitialState(const RepcutAssignmentModel &model, std::vector<uint32_t> labels, uint32_t k)
                : model_(model), atomPieces_(model.atomCount), atomValues_(model.atomCount),
                  atomWriters_(model.atomCount),
                  pieceMasks_(model.pieces.size()), valueMasks_(model.values.size()),
                  owners_(model.values.size()), assignment(std::move(labels)), loads(k)
            {
                for (uint32_t i = 0; i < model.pieces.size(); ++i)
                {
                    const auto &piece = model.pieces[i];
                    uint64_t mask = 0;
                    for (const auto atom : piece.atoms)
                    {
                        if (assignment[atom] == kRepcutNoAtom) atomPieces_[atom].push_back(i);
                        else mask |= uint64_t{1} << assignment[atom];
                    }
                    pieceMasks_[i] = mask;
                    while (mask)
                    {
                        const auto part = std::countr_zero(mask);
                        mask &= mask - 1;
                        for (std::size_t f = 0; f < 4; ++f)
                        {
                            add(loads.work[part][f], piece.features[f]);
                            add(loads.total[f], piece.features[f]);
                        }
                    }
                }
                std::vector<uint32_t> inferred;
                for (const auto &owner : model.inferredOwners)
                {
                    std::vector<uint64_t> votes(k);
                    for (const auto &users : owner.userAtoms)
                    {
                        uint64_t mask = 0;
                        for (const auto atom : users)
                        {
                            if (assignment[atom] == kRepcutNoAtom)
                                throw std::invalid_argument("initial assignment deferred an owner voter");
                            mask |= uint64_t{1} << assignment[atom];
                        }
                        while (mask) { ++votes[std::countr_zero(mask)]; mask &= mask - 1; }
                    }
                    const auto best = std::max_element(votes.begin(), votes.end());
                    inferred.push_back(*best ? static_cast<uint32_t>(best - votes.begin()) : kRepcutNoAtom);
                }
                for (uint32_t i = 0; i < model.values.size(); ++i)
                {
                    const auto &value = model.values[i];
                    const bool inferredOwner = value.writerAtom == kRepcutNoAtom;
                    const auto owner = inferredOwner ? inferred.at(value.inferredOwner) :
                                       assignment[value.writerAtom];
                    if (owner == kRepcutNoAtom && inferredOwner && !value.readerAtoms.empty())
                        throw std::invalid_argument("initial assignment has an unassigned value owner");
                    owners_[i] = owner;
                    if (!inferredOwner && owner == kRepcutNoAtom)
                        atomWriters_[value.writerAtom].push_back(i);
                    uint64_t mask = 0;
                    for (const auto atom : value.readerAtoms)
                    {
                        if (assignment[atom] == kRepcutNoAtom) atomValues_[atom].push_back(i);
                        else if (owner == kRepcutNoAtom || assignment[atom] != owner)
                            mask |= uint64_t{1} << assignment[atom];
                    }
                    valueMasks_[i] = mask;
                    if (owner != kRepcutNoAtom && mask) add(widths_[{owner, mask}], value.width);
                }
                for (const auto &[key, width] : widths_)
                    changeTransfer(loads, key, grhsimTransferFeaturesV1(width), false);
            }

            struct Choice
            {
                uint32_t atom, destination;
                Loads next;
                std::map<BundleKey, std::pair<uint64_t, uint64_t>> changes; // removed / added bits
                std::vector<std::pair<uint32_t, uint64_t>> ownerChanges;
                std::vector<std::pair<uint32_t, uint64_t>> readerMaskChanges;
                Choice(uint32_t a, uint32_t p, const Loads &current) : atom(a), destination(p), next(current) {}
            };

            Choice evaluate(uint32_t atom, uint32_t destination) const
            {
                Choice choice(atom, destination, loads);
                const uint64_t bit = uint64_t{1} << destination;
                for (const auto i : atomPieces_[atom]) if (!(pieceMasks_[i] & bit))
                    for (std::size_t f = 0; f < 4; ++f)
                    {
                        add(choice.next.work[destination][f], model_.pieces[i].features[f]);
                        add(choice.next.total[f], model_.pieces[i].features[f]);
                    }
                for (const auto i : atomWriters_[atom])
                {
                    if (owners_[i] != kRepcutNoAtom)
                        throw std::logic_error("initial assignment writer was already assigned");
                    const auto newMask = valueMasks_[i] & ~bit;
                    if (newMask)
                        add(choice.changes[{destination, newMask}].second, model_.values[i].width);
                    choice.ownerChanges.emplace_back(i, newMask);
                }
                for (const auto i : atomValues_[atom])
                {
                    // A deferred writer and reader may be represented by one
                    // ASC. Assigning it gives the value its owner and its
                    // local read at once.
                    if (owners_[i] == kRepcutNoAtom &&
                        std::find(atomWriters_[atom].begin(), atomWriters_[atom].end(), i) != atomWriters_[atom].end())
                        continue;
                    if (destination == owners_[i] || (valueMasks_[i] & bit)) continue;
                    if (owners_[i] == kRepcutNoAtom)
                    {
                        choice.readerMaskChanges.emplace_back(i, valueMasks_[i] | bit);
                        continue;
                    }
                    const auto width = model_.values[i].width;
                    if (valueMasks_[i]) add(choice.changes[{owners_[i], valueMasks_[i]}].first, width);
                    add(choice.changes[{owners_[i], valueMasks_[i] | bit}].second, width);
                }
                for (const auto &[key, change] : choice.changes)
                {
                    const auto it = widths_.find(key);
                    const auto before = it == widths_.end() ? 0 : it->second;
                    auto after = before;
                    subtract(after, change.first);
                    add(after, change.second);
                    if (before) changeTransfer(choice.next, key, grhsimTransferFeaturesV1(before), true);
                    if (after) changeTransfer(choice.next, key, grhsimTransferFeaturesV1(after), false);
                }
                return choice;
            }

            void commit(const Choice &choice)
            {
                if (assignment[choice.atom] != kRepcutNoAtom)
                    throw std::logic_error("initial assignment places an atom twice");
                assignment[choice.atom] = choice.destination;
                loads = choice.next;
                const uint64_t bit = uint64_t{1} << choice.destination;
                for (const auto i : atomPieces_[choice.atom]) pieceMasks_[i] |= bit;
                for (const auto &[i, newMask] : choice.ownerChanges)
                {
                    owners_[i] = choice.destination;
                    valueMasks_[i] = newMask;
                }
                for (const auto &[i, newMask] : choice.readerMaskChanges) valueMasks_[i] = newMask;
                for (const auto i : atomValues_[choice.atom])
                    if (choice.destination != owners_[i] &&
                        std::find(atomWriters_[choice.atom].begin(), atomWriters_[choice.atom].end(), i) ==
                            atomWriters_[choice.atom].end())
                        valueMasks_[i] |= bit;
                for (const auto &[key, change] : choice.changes)
                {
                    auto &width = widths_[key];
                    subtract(width, change.first);
                    add(width, change.second);
                    if (!width) widths_.erase(key);
                }
            }

            void verify(const RepcutAssignmentReport &report) const
            {
                for (std::size_t p = 0; p < loads.work.size(); ++p)
                {
                    const auto &part = report.parts[p];
                    for (std::size_t f = 0; f < 4; ++f)
                        if (part.features[f] != loads.work[p][f])
                            throw std::logic_error("initial assignment work mismatch");
                    if (part.sent.calls != loads.sendCalls[p] || part.sent.storageBytes != loads.sendBytes[p] ||
                        part.received.calls != loads.receiveCalls[p] || part.received.storageBytes != loads.receiveBytes[p])
                        throw std::logic_error("initial assignment communication mismatch");
                }
                if (report.totalTransfers.calls != loads.calls || report.totalTransfers.storageBytes != loads.bytes ||
                    !std::equal(loads.total.begin(), loads.total.end(), report.totalFeatures.begin()))
                    throw std::logic_error("initial assignment totals mismatch");
            }
        };

        auto placementKey(const InitialState::Choice &choice, const RepcutInitialBudget &budget, uint64_t cap)
        {
            const auto &loads = choice.next;
            return std::tuple{loads.communicationExcess(budget, cap), loads.calls, loads.bytes, loads.total[1],
                              maximum(loads.receiveBytes), maximum(loads.sendCalls),
                              loads.work[choice.destination][1], choice.destination};
        }

        std::optional<InitialState> placeReaders(const InitialState &initial, const std::vector<uint32_t> &order,
                                                 const RepcutInitialBudget &budget, uint64_t cap,
                                                 std::string &failure)
        {
            auto state = initial;
            if (!state.loads.permitsWork(budget)) return std::nullopt;
            for (const auto atom : order)
            {
                std::optional<InitialState::Choice> best;
                for (uint32_t p = 0; p < state.loads.work.size(); ++p)
                {
                    auto choice = state.evaluate(atom, p);
                    if (!choice.next.permitsWork(budget)) continue;
                    if (!best || placementKey(choice, budget, cap) < placementKey(*best, budget, cap))
                        best = std::move(choice);
                }
                if (!best)
                {
                    failure = "no_work_feasible_destination:atom=" + std::to_string(atom);
                    return std::nullopt;
                }
                state.commit(*best);
            }
            // A later reader can merge bundle signatures and reduce calls or
            // rounded bytes. Only monotone work limits prune partial states.
            if (!state.loads.permits(budget, cap))
            {
                failure = "completed_assignment";
                const auto limit = [&](const char *name, uint64_t actual, uint64_t bound) {
                    if (actual > bound) failure += std::string(":") + name + "=" +
                        std::to_string(actual) + ">" + std::to_string(bound);
                };
                limit("calls", state.loads.calls, budget.calls);
                limit("bytes", state.loads.bytes, budget.bytes);
                limit("send_calls", maximum(state.loads.sendCalls), budget.sendCalls);
                limit("send_bytes", maximum(state.loads.sendBytes), budget.sendBytes);
                limit("receive_calls", maximum(state.loads.receiveCalls), budget.receiveCalls);
                limit("receive_bytes", maximum(state.loads.receiveBytes), std::min(cap,budget.receiveBytes));
                return std::nullopt;
            }
            return state;
        }
    }

    RepcutStateRegions classifyGrhsimStateRegions(const RepcutAssignmentModel &model)
    {
        RepcutStateRegions result;
        const auto n = model.atomCount;
        if (!model.atomStateFlags.empty() && model.atomStateFlags.size() != n)
            throw std::invalid_argument("GrhSIM state metadata must match atom count");
        std::vector<std::vector<uint32_t>> outgoing(n), incoming(n);
        if (model.hasExactStateDependencies)
        {
            for (const auto &dependency : model.stateDependencies)
            {
                if (dependency.sourceAtom >= n || dependency.sinkAtom >= n)
                    throw std::invalid_argument("invalid exact state dependency");
                outgoing[dependency.sourceAtom].push_back(dependency.sinkAtom);
            }
            for (const auto source : model.stateCoreSources)
                if (source >= n) throw std::invalid_argument("invalid exact state core source");
        }
        else
        {
            for (const auto &value : model.values)
            {
                if (value.writerAtom == kRepcutNoAtom) continue;
                if (value.writerAtom >= n) throw std::invalid_argument("invalid dependency writer");
                for (const auto reader : value.readerAtoms)
                {
                    if (reader >= n) throw std::invalid_argument("invalid dependency reader");
                    outgoing[value.writerAtom].push_back(reader);
                }
            }
        }
        for (uint32_t atom = 0; atom < n; ++atom)
        {
            auto &edges = outgoing[atom];
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            add(result.dependencyEdges, edges.size());
            for (const auto reader : edges) incoming[reader].push_back(atom);
        }
        // Iterative Kosaraju: large designs can contain very long state chains.
        std::vector<bool> seen(n);
        std::vector<uint32_t> order;
        std::vector<std::pair<uint32_t, std::size_t>> stack;
        order.reserve(n);
        for (uint32_t root = 0; root < n; ++root)
        {
            if (seen[root]) continue;
            seen[root] = true;
            stack.emplace_back(root, 0);
            while (!stack.empty())
            {
                auto &[atom, cursor] = stack.back();
                if (cursor == outgoing[atom].size())
                {
                    order.push_back(atom);
                    stack.pop_back();
                    continue;
                }
                const auto reader = outgoing[atom][cursor++];
                if (!seen[reader])
                {
                    seen[reader] = true;
                    stack.emplace_back(reader, 0);
                }
            }
        }
        result.component.assign(n, kRepcutNoAtom);
        std::vector<uint32_t> pending;
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            if (result.component[*it] != kRepcutNoAtom) continue;
            const auto component = static_cast<uint32_t>(result.componentSizes.size());
            result.componentSizes.push_back(0);
            result.component[*it] = component;
            pending.push_back(*it);
            while (!pending.empty())
            {
                const auto atom = pending.back();
                pending.pop_back();
                ++result.componentSizes[component];
                for (const auto writer : incoming[atom])
                    if (result.component[writer] == kRepcutNoAtom)
                    {
                        result.component[writer] = component;
                        pending.push_back(writer);
                    }
            }
        }
        result.cyclicComponents.resize(result.componentSizes.size());
        for (uint32_t atom = 0; atom < n; ++atom)
        {
            const auto component = result.component[atom];
            if (result.componentSizes[component] > 1 ||
                std::binary_search(outgoing[atom].begin(), outgoing[atom].end(), atom))
                result.cyclicComponents[component] = true;
        }
        result.reachesCore.assign(n, false);
        if (model.hasExactStateDependencies)
            for (const auto source : model.stateCoreSources)
                if (!result.reachesCore[source])
                {
                    result.reachesCore[source] = true;
                    pending.push_back(source);
                }
        for (uint32_t atom = 0; atom < n; ++atom)
        {
            const auto flags = model.atomStateFlags.empty() ? kRepcutStateUnsupported : model.atomStateFlags[atom];
            if (result.cyclicComponents[result.component[atom]] ||
                (flags & (kRepcutStateWriteMemory | kRepcutStateWriteLatch | kRepcutStateUnsupported)))
            {
                result.reachesCore[atom] = true;
                pending.push_back(atom);
            }
        }
        while (!pending.empty())
        {
            const auto atom = pending.back();
            pending.pop_back();
            for (const auto writer : incoming[atom])
                if (!result.reachesCore[writer])
                {
                    result.reachesCore[writer] = true;
                    pending.push_back(writer);
                }
        }
        result.terminalState.resize(n);
        for (uint32_t atom = 0; atom < n; ++atom)
            result.terminalState[atom] = !model.atomStateFlags.empty() &&
                model.atomStateFlags[atom] == kRepcutStateWriteRegister && !result.reachesCore[atom];
        return result;
    }

    RepcutInitialCore prepareGrhsimInitialCore(const RepcutAssignmentModel &model, bool includeStateTerminals)
    {
        validateFeatures(model);
        RepcutInitialCore result;
        if (includeStateTerminals) result.stateRegions = classifyGrhsimStateRegions(model);
        std::vector<bool> core(model.atomCount);
        std::vector<bool> ownerVoter(model.atomCount);
        std::vector<uint64_t> work(model.atomCount), inputBits(model.atomCount);
        for (const auto &value : model.values)
        {
            if (value.writerAtom != kRepcutNoAtom) core.at(value.writerAtom) = true;
            for (const auto atom : value.readerAtoms) add(inputBits.at(atom), value.width);
        }
        for (const auto &owner : model.inferredOwners)
            for (const auto &users : owner.userAtoms)
                for (const auto atom : users) core.at(atom) = ownerVoter.at(atom) = true;
        for (const auto &piece : model.pieces)
        {
            for (const auto atom : piece.atoms)
            {
                add(work.at(atom), piece.features.at(1));
                if (piece.features.at(3)) core.at(atom) = true;
            }
        }
        if (includeStateTerminals)
        {
            for (uint32_t atom = 0; atom < model.atomCount; ++atom)
                if (result.stateRegions.terminalState[atom] && !ownerVoter[atom]) core[atom] = false;
        }
        result.atomToCore.assign(model.atomCount, kRepcutNoAtom);
        for (uint32_t atom = 0; atom < model.atomCount; ++atom)
        {
            const auto words = inputBits[atom] / 64 + (inputBits[atom] % 64 != 0);
            const bool terminalState = includeStateTerminals && result.stateRegions.terminalState[atom];
            if (!core[atom] && ((words >= 128 && words > work[atom]) || terminalState))
            {
                result.terminalOrder.push_back(atom);
                if (terminalState)
                {
                    add(result.stateTerminalWorkSum, work[atom]);
                    result.stateTerminalWorkMax = std::max(result.stateTerminalWorkMax, work[atom]);
                }
            }
            else
            {
                result.atomToCore[atom] = static_cast<uint32_t>(result.coreToAtom.size());
                result.coreToAtom.push_back(atom);
            }
        }
        std::sort(result.terminalOrder.begin(), result.terminalOrder.end(), [&](auto a, auto b) {
            return std::tuple{inputBits[a], work[a], UINT32_MAX - a} >
                   std::tuple{inputBits[b], work[b], UINT32_MAX - b};
        });
        if (includeStateTerminals)
        {
            // Place downstream readers first so a deferred writer sees their
            // actual receiver labels. Keep the old size ordering among ready
            // nodes. No state chain is merged into a larger placement unit.
            std::vector<std::vector<uint32_t>> successors(model.atomCount), predecessors(model.atomCount);
            for (const auto &value : model.values)
            {
                if (value.writerAtom == kRepcutNoAtom || result.atomToCore[value.writerAtom] != kRepcutNoAtom)
                    continue;
                for (const auto reader : value.readerAtoms)
                    if (result.atomToCore[reader] == kRepcutNoAtom)
                        successors[value.writerAtom].push_back(reader);
            }
            std::vector<uint32_t> remaining(model.atomCount);
            std::priority_queue<std::tuple<uint64_t,uint64_t,uint32_t>> ready;
            for (const auto atom : result.terminalOrder)
            {
                auto &readers = successors[atom];
                std::sort(readers.begin(), readers.end());
                readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
                remaining[atom] = static_cast<uint32_t>(readers.size());
                for (const auto reader : readers) predecessors[reader].push_back(atom);
                if (readers.empty()) ready.emplace(inputBits[atom],work[atom],UINT32_MAX-atom);
            }
            std::vector<uint32_t> ordered;
            ordered.reserve(result.terminalOrder.size());
            while (!ready.empty())
            {
                const auto atom = UINT32_MAX-std::get<2>(ready.top());
                ready.pop();
                ordered.push_back(atom);
                for (const auto writer : predecessors[atom])
                    if (--remaining[writer] == 0) ready.emplace(inputBits[writer],work[writer],UINT32_MAX-writer);
            }
            if (ordered.size() != result.terminalOrder.size())
                throw std::logic_error("terminal state classification retained a dependency cycle");
            result.terminalOrder = std::move(ordered);
        }
        std::vector<uint64_t> fair(result.coreToAtom.size());
        for (std::size_t pid = 0; pid < model.pieces.size(); ++pid)
        {
            const auto &piece = model.pieces[pid];
            std::vector<uint32_t> atoms;
            for (const auto atom : piece.atoms)
                if (result.atomToCore[atom] != kRepcutNoAtom) atoms.push_back(result.atomToCore[atom]);
            if (atoms.empty()) continue;
            const auto q = piece.features[0] / atoms.size(), remainder = piece.features[0] % atoms.size();
            for (std::size_t i = 0; i < atoms.size(); ++i)
                add(fair[atoms[(pid + i) % atoms.size()]], q + (i < remainder));
        }
        for (const auto weight : fair)
        {
            if (weight > INT32_MAX) throw std::overflow_error("initial core weight exceeds solver range");
            result.nodeWeights.push_back(static_cast<uint32_t>(std::max<uint64_t>(1, weight)));
        }
        return result;
    }

    RepcutInitialBudget repcutInitialBudgetFromReference(const RepcutAssignmentReport &reference)
    {
        if (reference.parts.empty() || reference.totalFeatures.size() != 4)
            throw std::invalid_argument("initial assignment requires a nonempty raw-structure reference");
        RepcutInitialBudget budget;
        budget.recurringSum = scaledLimit(reference.totalFeatures[1], 1005);
        budget.operationSum = scaledLimit(reference.totalFeatures[2], 1010);
        budget.calls = scaledLimit(reference.totalTransfers.calls, 1005);
        budget.bytes = scaledLimit(reference.totalTransfers.storageBytes, 1005);
        for (const auto &part : reference.parts)
        {
            budget.recurringMax = std::max(budget.recurringMax, part.features.at(1));
            budget.sendCalls = std::max(budget.sendCalls, scaledLimit(part.sent.calls, 1010));
            budget.sendBytes = std::max(budget.sendBytes, scaledLimit(part.sent.storageBytes, 1010));
            budget.receiveCalls = std::max(budget.receiveCalls, scaledLimit(part.received.calls, 1010));
            budget.receiveBytes = std::max(budget.receiveBytes, scaledLimit(part.received.storageBytes, 1010));
        }
        return budget;
    }

    RepcutInitialAssignment completeGrhsimInitialAssignment(
        const RepcutAssignmentEvaluator &evaluator, const RepcutInitialCore &core,
        std::vector<uint32_t> partialAssignment, uint32_t partitionCount, const RepcutInitialBudget &budget)
    {
        validateFeatures(evaluator.model());
        if (evaluator.transferModelName() != "grhsim-copy-v1")
            throw std::invalid_argument("GrhSIM initial assignment requires grhsim-copy-v1 transfers");
        if (!partitionCount || partitionCount > 64 || partialAssignment.size() != evaluator.model().atomCount ||
            core.atomToCore.size() != partialAssignment.size())
            throw std::invalid_argument("invalid initial assignment shape or partition count");
        std::vector<bool> deferred(partialAssignment.size());
        for (const auto atom : core.terminalOrder)
        {
            if (deferred.at(atom)) throw std::invalid_argument("duplicate initial terminal atom");
            deferred[atom] = true;
        }
        for (std::size_t a = 0; a < partialAssignment.size(); ++a)
            if (deferred[a] ? partialAssignment[a] != kRepcutNoAtom : partialAssignment[a] >= partitionCount)
                throw std::invalid_argument("initial partial labels do not match terminal classification");
        RepcutInitialAssignment result;
        if (core.terminalOrder.empty()) { result.reason = "no_terminal_readers"; return result; }
        InitialState initial(evaluator.model(), std::move(partialAssignment), partitionCount);
        // Every deferred writer retains its complete work and transfer cost.
        // Classification does not loosen the reference-derived budgets.
        auto placementBudget = budget;
        uint64_t low = std::min(maximum(initial.loads.receiveBytes), placementBudget.receiveBytes);
        uint64_t high = placementBudget.receiveBytes;
        std::optional<InitialState> best;
        std::string failure;
        if (!initial.loads.permitsWork(placementBudget))
        {
            result.reason = "core_work_budget:max=" + std::to_string(initial.loads.recurringMax()) +
                ":limit=" + std::to_string(placementBudget.recurringMax) +
                ":total=" + std::to_string(initial.loads.total[1]) +
                ":total_limit=" + std::to_string(placementBudget.recurringSum) +
                ":ops=" + std::to_string(initial.loads.total[2]) +
                ":ops_limit=" + std::to_string(placementBudget.operationSum);
            return result;
        }
        for (uint32_t trial = 0; trial < 6; ++trial)
        {
            const auto cap = trial == 0 ? high : low + (high - low) / 2;
            auto state = placeReaders(initial, core.terminalOrder, placementBudget, cap, failure);
            ++result.attempts;
            if (state) { high = cap; best.emplace(std::move(*state)); }
            else
            {
                if (trial == 0 || cap == high) break;
                low = cap + 1;
            }
        }
        if (!best) { result.reason = "structural_budget:" + failure; return result; }
        result.report = evaluator.evaluate(best->assignment, partitionCount);
        best->verify(result.report);
        result.atomPartitions = std::move(best->assignment);
        result.receiverCap = high;
        result.accepted = true;
        result.reason = "accepted";
        return result;
    }
}
