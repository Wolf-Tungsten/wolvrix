#include "transform/grhsim_assignment_proposal.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wolvrix::lib::transform
{
    namespace
    {
        constexpr std::array<std::string_view, kGrhSimProposalPhaseCount * kGrhSimProposalAxisCount>
            kFeatureNames{
                "rising.compute_core", "rising.commit_publish", "rising.wide_helper",
                "falling.compute_core", "falling.commit_publish", "falling.wide_helper"};

        void maxTransfer(RepcutTransferFeatures &target, const RepcutTransferFeatures &value)
        {
            target.calls = std::max(target.calls, value.calls);
            target.scalarCalls = std::max(target.scalarCalls, value.scalarCalls);
            target.wideCalls = std::max(target.wideCalls, value.wideCalls);
            target.storageBytes = std::max(target.storageBytes, value.storageBytes);
            target.full64ByteBlocks = std::max(target.full64ByteBlocks, value.full64ByteBlocks);
            target.tailCalls = std::max(target.tailCalls, value.tailCalls);
            target.tailBytes = std::max(target.tailBytes, value.tailBytes);
        }

        std::array<uint64_t, 29> scoreKey(const GrhSimProposalScore &score)
        {
            std::array<uint64_t, 29> key{};
            std::size_t at = 0;
            for (const auto &phase : score.maxWork)
            {
                key[at++] = phase.computeCore;
                key[at++] = phase.commitPublish;
                key[at++] = phase.wideHelper;
            }
            const auto appendTransfer = [&](const RepcutTransferFeatures &value) {
                key[at++] = value.calls;
                key[at++] = value.tailCalls;
                key[at++] = value.storageBytes;
                key[at++] = value.tailBytes;
                key[at++] = value.full64ByteBlocks;
                key[at++] = value.scalarCalls;
                key[at++] = value.wideCalls;
            };
            appendTransfer(score.maxSent);
            appendTransfer(score.maxReceived);
            key[at++] = score.maxSendPeers;
            key[at++] = score.maxReceivePeers;
            appendTransfer(score.totalTransfers);
            if (at != key.size()) throw std::logic_error("invalid GrhSIM proposal score key");
            return key;
        }
    }

    GrhSimPhaseFeatureLayout makeGrhSimPhaseFeatureLayout(
        const RepcutAssignmentModel &model)
    {
        GrhSimPhaseFeatureLayout layout;
        for (std::size_t i = 0; i < kFeatureNames.size(); ++i)
        {
            const auto it = std::find(model.featureNames.begin(), model.featureNames.end(),
                                      kFeatureNames[i]);
            if (it == model.featureNames.end())
                throw std::invalid_argument("missing GrhSIM phase feature: " + std::string(kFeatureNames[i]));
            layout.indices[i] = static_cast<std::size_t>(it - model.featureNames.begin());
        }
        return layout;
    }

    RepcutAssignmentModel withGrhSimPhaseFeatures(
        const RepcutAssignmentModel &base,
        std::span<const std::array<uint64_t, kGrhSimProposalPhaseCount * kGrhSimProposalAxisCount>>
            pieceFeatures)
    {
        if (pieceFeatures.size() != base.pieces.size())
            throw std::invalid_argument("GrhSIM phase feature count does not match pieces");
        auto result = base;
        static constexpr std::array<std::string_view, kGrhSimProposalPhaseCount * kGrhSimProposalAxisCount>
            names{
                "rising.compute_core", "rising.commit_publish", "rising.wide_helper",
                "falling.compute_core", "falling.commit_publish", "falling.wide_helper"};
        for (const auto name : names)
        {
            if (std::find(result.featureNames.begin(), result.featureNames.end(), name) != result.featureNames.end())
                throw std::invalid_argument("duplicate GrhSIM phase feature: " + std::string(name));
            result.featureNames.emplace_back(name);
        }
        for (std::size_t i = 0; i < result.pieces.size(); ++i)
            result.pieces[i].features.insert(result.pieces[i].features.end(),
                                             pieceFeatures[i].begin(), pieceFeatures[i].end());
        result.adapter += "+grhsim-phase-v1";
        return result;
    }

    GrhSimProposalScore scoreGrhSimAssignment(
        const RepcutAssignmentReport &report,
        const GrhSimPhaseFeatureLayout &layout)
    {
        if (report.parts.empty()) throw std::invalid_argument("GrhSIM proposal requires partitions");
        GrhSimProposalScore score;
        for (const auto &part : report.parts)
        {
            if (part.features.size() <= *std::max_element(layout.indices.begin(), layout.indices.end()))
                throw std::invalid_argument("GrhSIM phase feature index is outside assignment report");
            const auto update = [&](std::size_t phase, GrhSimPhaseAxisCost &maximum) {
                const auto &features = part.features;
                maximum.computeCore = std::max(maximum.computeCore, features[layout.indices[phase * 3]]);
                maximum.commitPublish = std::max(maximum.commitPublish, features[layout.indices[phase * 3 + 1]]);
                maximum.wideHelper = std::max(maximum.wideHelper, features[layout.indices[phase * 3 + 2]]);
            };
            update(0, score.maxWork[0]);
            update(1, score.maxWork[1]);
            maxTransfer(score.maxSent, part.sent);
            maxTransfer(score.maxReceived, part.received);
            score.maxSendPeers = std::max(score.maxSendPeers, part.sendPeers);
            score.maxReceivePeers = std::max(score.maxReceivePeers, part.receivePeers);
        }
        score.totalTransfers = report.totalTransfers;
        return score;
    }

    bool grhsimProposalDominates(const GrhSimProposalScore &left,
                                 const GrhSimProposalScore &right)
    {
        const auto lhs = scoreKey(left), rhs = scoreKey(right);
        bool strict = false;
        for (std::size_t i = 0; i < lhs.size(); ++i)
        {
            if (lhs[i] > rhs[i]) return false;
            strict |= lhs[i] < rhs[i];
        }
        return strict;
    }

    bool grhsimProposalLexicographicallyLess(const GrhSimProposalScore &left,
                                             const GrhSimProposalScore &right)
    {
        return scoreKey(left) < scoreKey(right);
    }

    GrhSimProposalResult proposeGrhSimMoves(
        const RepcutAssignmentEvaluator &evaluator,
        std::span<const uint32_t> assignment,
        uint32_t partitionCount,
        std::span<const uint32_t> candidateAtoms,
        const GrhSimPhaseFeatureLayout &layout)
    {
        const auto baselineReport = evaluator.evaluate(assignment, partitionCount);
        GrhSimProposalResult result;
        result.baseline = scoreGrhSimAssignment(baselineReport, layout);
        std::vector<bool> seen(assignment.size(), false);
        std::vector<GrhSimMoveProposal> candidates;
        for (const auto atom : candidateAtoms)
        {
            if (atom >= assignment.size()) throw std::invalid_argument("invalid GrhSIM proposal atom");
            if (seen[atom]) throw std::invalid_argument("duplicate GrhSIM proposal atom");
            seen[atom] = true;
            const auto source = assignment[atom];
            for (uint32_t destination = 0; destination < partitionCount; ++destination)
            {
                if (destination == source) continue;
                auto trial = std::vector<uint32_t>(assignment.begin(), assignment.end());
                trial[atom] = destination;
                const auto report = evaluator.evaluate(trial, partitionCount);
                candidates.push_back({atom, source, destination,
                                      scoreGrhSimAssignment(report, layout)});
            }
        }

        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const auto &candidate = candidates[i];
            if (grhsimProposalDominates(result.baseline, candidate.score)) continue;
            bool dominated = false;
            for (std::size_t j = 0; j < candidates.size(); ++j)
                if (i != j && grhsimProposalDominates(candidates[j].score, candidate.score))
                {
                    dominated = true;
                    break;
                }
            if (!dominated) result.paretoMoves.push_back(candidate);
        }

        for (const auto &candidate : result.paretoMoves)
        {
            if (!grhsimProposalLexicographicallyLess(candidate.score, result.baseline)) continue;
            if (!result.selectedMove ||
                grhsimProposalLexicographicallyLess(candidate.score, result.selectedMove->score) ||
                (candidate.score == result.selectedMove->score &&
                 std::pair{candidate.atom, candidate.destination} <
                     std::pair{result.selectedMove->atom, result.selectedMove->destination}))
                result.selectedMove = candidate;
        }
        return result;
    }
}
