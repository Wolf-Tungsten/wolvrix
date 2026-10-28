#include "transform/repcut_assignment_model.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace wolvrix::lib::transform
{
    namespace
    {
        void add(uint64_t &value, uint64_t amount)
        {
            if (amount > std::numeric_limits<uint64_t>::max() - value)
                throw std::overflow_error("repcut assignment feature overflow");
            value += amount;
        }

        void add(RepcutTransferFeatures &to, const RepcutTransferFeatures &from)
        {
            add(to.calls, from.calls);
            add(to.scalarCalls, from.scalarCalls);
            add(to.wideCalls, from.wideCalls);
            add(to.storageBytes, from.storageBytes);
            add(to.full64ByteBlocks, from.full64ByteBlocks);
            add(to.tailCalls, from.tailCalls);
            add(to.tailBytes, from.tailBytes);
        }

        template <class T> void unique(std::vector<T> &values)
        {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }

        std::vector<uint32_t> partsOf(std::span<const uint32_t> atoms,
                                      std::span<const uint32_t> assignment)
        {
            std::vector<uint32_t> result;
            result.reserve(atoms.size());
            for (const auto atom : atoms) result.push_back(assignment[atom]);
            unique(result);
            return result;
        }

        void totalWork(RepcutAssignmentReport &report)
        {
            report.totalFeatures.assign(report.uniqueFeatures.size(), 0);
            for (const auto &part : report.parts)
                for (std::size_t f = 0; f < part.features.size(); ++f)
                    add(report.totalFeatures[f], part.features[f]);
        }
    }

    RepcutTransferFeatures grhsimTransferFeaturesV1(uint64_t width)
    {
        if (width == 0) throw std::invalid_argument("repcut transfer width must be positive");
        RepcutTransferFeatures result;
        result.calls = 1;
        if (width <= 64)
        {
            result.scalarCalls = 1;
            result.storageBytes = width <= 8 ? 1 : width <= 16 ? 2 : width <= 32 ? 4 : 8;
        }
        else
        {
            const uint64_t words = width / 64 + (width % 64 != 0);
            result.wideCalls = 1;
            result.storageBytes = words * 8;
            result.full64ByteBlocks = words / 8;
            result.tailCalls = words % 8 != 0;
            result.tailBytes = (words % 8) * 8;
        }
        return result;
    }

    RepcutAssignmentEvaluator::RepcutAssignmentEvaluator(RepcutAssignmentModel model,
                                                         RepcutTransferModel transfers)
        : model_(std::move(model)), transferModel_(std::move(transfers))
    {
        if (transferModel_.name.empty() || !transferModel_.estimate)
            throw std::invalid_argument("repcut assignment requires a named transfer model");
        if (model_.adapter.empty() || model_.featureNames.empty())
            throw std::invalid_argument("repcut assignment requires an adapter and named feature columns");
        if (!model_.atomStateFlags.empty() && model_.atomStateFlags.size() != model_.atomCount)
            throw std::invalid_argument("repcut assignment state metadata must match atom count");
        std::set<std::string> names;
        for (const auto &name : model_.featureNames)
            if (name.empty() || !names.insert(name).second)
                throw std::invalid_argument("repcut assignment feature names must be unique and nonempty");
        if (model_.pieces.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("too many repcut pieces");
        atomPieces_.resize(model_.atomCount);
        for (std::size_t i = 0; i < model_.pieces.size(); ++i)
        {
            auto &piece = model_.pieces[i];
            if (piece.features.size() != model_.featureNames.size())
                throw std::invalid_argument("repcut assignment feature dimension mismatch");
            unique(piece.atoms);
            for (const auto atom : piece.atoms)
            {
                if (atom >= model_.atomCount) throw std::invalid_argument("invalid repcut piece atom");
                atomPieces_[atom].push_back(static_cast<uint32_t>(i));
            }
        }
        for (auto &owner : model_.inferredOwners)
            for (auto &user : owner.userAtoms)
            {
                unique(user);
                for (const auto atom : user)
                    if (atom >= model_.atomCount) throw std::invalid_argument("invalid inferred-owner atom");
            }
        std::sort(model_.values.begin(), model_.values.end(),
                  [](const auto &a, const auto &b) { return a.key < b.key; });
        for (std::size_t i = 0; i < model_.values.size(); ++i)
        {
            auto &value = model_.values[i];
            const bool hasWriter = value.writerAtom < model_.atomCount && value.inferredOwner == kRepcutNoAtom;
            const bool inferred = value.writerAtom == kRepcutNoAtom && value.inferredOwner < model_.inferredOwners.size();
            if ((!hasWriter && !inferred) || value.width == 0 ||
                value.width > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw std::invalid_argument("invalid repcut directed value owner or width");
            if (i && model_.values[i - 1].key == value.key)
                throw std::invalid_argument("duplicate repcut directed value key");
            unique(value.readerAtoms);
            for (const auto atom : value.readerAtoms)
                if (atom >= model_.atomCount) throw std::invalid_argument("invalid repcut reader atom");
        }
    }

    void RepcutAssignmentEvaluator::validateAssignment(std::span<const uint32_t> assignment,
                                                       uint32_t partitionCount) const
    {
        if (partitionCount == 0 || assignment.size() != model_.atomCount)
            throw std::invalid_argument("invalid repcut assignment size or partition count");
        for (const auto part : assignment)
            if (part >= partitionCount) throw std::invalid_argument("invalid repcut partition id");
    }

    void RepcutAssignmentEvaluator::evaluateTransfers(std::span<const uint32_t> assignment,
                                                      RepcutAssignmentReport &report) const
    {
        // This key matches Phase E's owner + exact destination signature.
        using Key = std::pair<uint32_t, std::vector<uint32_t>>;
        std::map<Key, RepcutTransferGroup> groups;
        std::vector<uint32_t> inferredParts;
        for (const auto &group : model_.inferredOwners)
        {
            std::vector<uint64_t> votes(report.parts.size());
            for (const auto &user : group.userAtoms)
                for (const auto part : partsOf(user, assignment)) add(votes[part], 1);
            // The first maximum implements Phase E's lowest-part-ID tie break.
            const auto best = std::max_element(votes.begin(), votes.end());
            inferredParts.push_back(*best == 0 ? kRepcutNoAtom : static_cast<uint32_t>(best - votes.begin()));
        }
        for (const auto &value : model_.values)
        {
            const uint32_t owner = value.writerAtom == kRepcutNoAtom
                ? inferredParts[value.inferredOwner] : assignment[value.writerAtom];
            auto readers = partsOf(value.readerAtoms, assignment);
            if (owner == kRepcutNoAtom)
            {
                if (!readers.empty()) throw std::invalid_argument("read-only storage has readers but no owner votes");
                continue;
            }
            readers.erase(std::remove(readers.begin(), readers.end(), owner), readers.end());
            if (readers.empty()) continue;
            auto [it, inserted] = groups.try_emplace({owner, readers});
            auto &group = it->second;
            if (inserted)
            {
                group.ownerPart = owner;
                group.readerParts = std::move(readers);
            }
            add(group.width, value.width);
            if (group.width > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                throw std::overflow_error("repcut boundary bundle exceeds signed width range");
            group.members.push_back({value.key, value.width, 0});
        }
        report.transfers.clear();
        report.totalTransfers = {};
        std::vector<std::set<uint32_t>> sendPeers(report.parts.size()), receivePeers(report.parts.size());
        for (auto &part : report.parts)
        {
            part.sent = {};
            part.received = {};
            part.sendPeers = part.receivePeers = 0;
        }
        for (auto &[key, group] : groups)
        {
            (void)key;
            uint64_t cursor = group.width;
            // Values were sorted by stable value key, as in Phase E.
            for (auto &member : group.members)
            {
                cursor -= member.width;
                member.lowBit = cursor;
            }
            group.perReader = transferModel_.estimate(group.width);
            for (const auto reader : group.readerParts)
            {
                add(report.parts[group.ownerPart].sent, group.perReader);
                add(report.parts[reader].received, group.perReader);
                add(report.totalTransfers, group.perReader);
                sendPeers[group.ownerPart].insert(reader);
                receivePeers[reader].insert(group.ownerPart);
            }
            report.transfers.push_back(std::move(group));
        }
        for (std::size_t part = 0; part < report.parts.size(); ++part)
        {
            report.parts[part].sendPeers = static_cast<uint32_t>(sendPeers[part].size());
            report.parts[part].receivePeers = static_cast<uint32_t>(receivePeers[part].size());
        }
    }

    RepcutAssignmentReport RepcutAssignmentEvaluator::evaluate(
        std::span<const uint32_t> assignment, uint32_t partitionCount) const
    {
        validateAssignment(assignment, partitionCount);
        RepcutAssignmentReport report;
        report.parts.resize(partitionCount);
        report.uniqueFeatures.resize(model_.featureNames.size());
        for (auto &part : report.parts) part.features.resize(model_.featureNames.size());
        for (const auto &piece : model_.pieces)
        {
            const auto parts = partsOf(piece.atoms, assignment);
            if (parts.empty()) continue;
            for (std::size_t f = 0; f < piece.features.size(); ++f)
                add(report.uniqueFeatures[f], piece.features[f]);
            for (const auto part : parts)
            {
                ++report.parts[part].pieceCopies;
                for (std::size_t f = 0; f < piece.features.size(); ++f)
                    add(report.parts[part].features[f], piece.features[f]);
            }
        }
        totalWork(report);
        evaluateTransfers(assignment, report);
        return report;
    }

    std::vector<uint32_t> repcutAssignmentDirectImpact(
        std::span<const uint32_t> beforeAssignment, const RepcutAssignmentReport &before,
        std::span<const uint32_t> afterAssignment, const RepcutAssignmentReport &after)
    {
        const auto partitions = before.parts.size();
        if (partitions == 0 || partitions > std::numeric_limits<uint32_t>::max() ||
            after.parts.size() != partitions || beforeAssignment.size() != afterAssignment.size())
            throw std::invalid_argument("incompatible repcut impact inputs");
        std::set<uint32_t> affected;
        for (std::size_t atom = 0; atom < beforeAssignment.size(); ++atom)
        {
            const auto source = beforeAssignment[atom], destination = afterAssignment[atom];
            if (source >= partitions || destination >= partitions)
                throw std::invalid_argument("invalid repcut impact assignment");
            if (source != destination) { affected.insert(source); affected.insert(destination); }
        }
        using Key = std::pair<uint32_t, std::vector<uint32_t>>;
        const auto index = [&](const RepcutAssignmentReport &report) {
            std::map<Key, const RepcutTransferGroup *> result;
            for (const auto &group : report.transfers)
            {
                if (group.ownerPart >= partitions || group.readerParts.empty() || group.width == 0 ||
                    !std::is_sorted(group.readerParts.begin(), group.readerParts.end()) ||
                    std::adjacent_find(group.readerParts.begin(), group.readerParts.end()) != group.readerParts.end())
                    throw std::invalid_argument("invalid repcut impact transfer group");
                for (const auto reader : group.readerParts)
                    if (reader >= partitions || reader == group.ownerPart)
                        throw std::invalid_argument("invalid repcut impact transfer reader");
                if (!result.emplace(Key{group.ownerPart, group.readerParts}, &group).second)
                    throw std::invalid_argument("duplicate repcut impact transfer group");
            }
            return result;
        };
        const auto oldGroups = index(before), newGroups = index(after);
        const auto mark = [&](const Key &key) {
            affected.insert(key.first);
            affected.insert(key.second.begin(), key.second.end());
        };
        for (const auto &[key, oldGroup] : oldGroups)
        {
            const auto next = newGroups.find(key);
            if (next == newGroups.end() || oldGroup->width != next->second->width ||
                oldGroup->members != next->second->members) mark(key);
        }
        for (const auto &[key, _] : newGroups)
            if (!oldGroups.contains(key)) mark(key);
        return {affected.begin(), affected.end()};
    }

    RepcutAssignmentState::RepcutAssignmentState(const RepcutAssignmentEvaluator &evaluator,
                                                 std::vector<uint32_t> assignment,
                                                 uint32_t partitionCount)
        : evaluator_(evaluator), assignment_(std::move(assignment)),
          report_(evaluator_.evaluate(assignment_, partitionCount))
    {
        occupancy_.resize(evaluator_.model_.pieces.size());
        for (std::size_t i = 0; i < occupancy_.size(); ++i)
        {
            std::map<uint32_t, uint32_t> counts;
            for (const auto atom : evaluator_.model_.pieces[i].atoms) ++counts[assignment_[atom]];
            occupancy_[i].assign(counts.begin(), counts.end());
        }
    }

    void RepcutAssignmentState::moveAtom(uint32_t atom, uint32_t destination)
    {
        if (atom >= assignment_.size() || destination >= report_.parts.size())
            throw std::invalid_argument("invalid repcut move");
        const auto source = assignment_[atom];
        if (source == destination) return;
        // Prepare all possibly-throwing work before changing the live state.
        auto trialAssignment = assignment_;
        trialAssignment[atom] = destination;
        RepcutAssignmentReport next;
        next.parts = report_.parts;
        next.uniqueFeatures = report_.uniqueFeatures;
        std::vector<std::pair<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>> nextCounts;
        for (const auto pieceId : evaluator_.atomPieces_[atom])
        {
            const auto &piece = evaluator_.model_.pieces[pieceId];
            auto counts = occupancy_[pieceId];
            auto from = std::find_if(counts.begin(), counts.end(),
                                     [=](const auto &v) { return v.first == source; });
            if (from == counts.end() || from->second == 0)
                throw std::logic_error("repcut piece occupancy invariant failed");
            if (--from->second == 0)
            {
                --next.parts[source].pieceCopies;
                for (std::size_t f = 0; f < piece.features.size(); ++f)
                    next.parts[source].features[f] -= piece.features[f];
                counts.erase(from);
            }
            auto to = std::find_if(counts.begin(), counts.end(),
                                   [=](const auto &v) { return v.first == destination; });
            if (to == counts.end())
            {
                ++next.parts[destination].pieceCopies;
                for (std::size_t f = 0; f < piece.features.size(); ++f)
                    add(next.parts[destination].features[f], piece.features[f]);
                counts.emplace_back(destination, 1);
            }
            else ++to->second;
            std::sort(counts.begin(), counts.end());
            nextCounts.emplace_back(pieceId, std::move(counts));
        }
        totalWork(next);
        evaluator_.evaluateTransfers(trialAssignment, next);
        for (auto &[piece, counts] : nextCounts) occupancy_[piece].swap(counts);
        assignment_.swap(trialAssignment);
        report_ = std::move(next);
    }
}
