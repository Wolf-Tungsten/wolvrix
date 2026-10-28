#ifndef WOLVRIX_TRANSFORM_GRHSIM_ASSIGNMENT_PROPOSAL_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_ASSIGNMENT_PROPOSAL_HPP

#include "transform/repcut_assignment_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wolvrix::lib::transform
{
    inline constexpr std::size_t kGrhSimProposalPhaseCount = 2;
    inline constexpr std::size_t kGrhSimProposalAxisCount = 3;

    // The feature vector supplied to RepcutAssignmentModel uses these six
    // named columns.  They are structural units, not time coefficients.
    struct GrhSimPhaseFeatureLayout
    {
        std::array<std::size_t, kGrhSimProposalPhaseCount * kGrhSimProposalAxisCount> indices{};
    };

    GrhSimPhaseFeatureLayout makeGrhSimPhaseFeatureLayout(
        const RepcutAssignmentModel &model);

    // Replace/augment the piece feature vector with backend-supplied phase
    // units. The evaluator still performs set-union accounting; this function
    // does not infer phase work from an already partitioned assignment.
    RepcutAssignmentModel withGrhSimPhaseFeatures(
        const RepcutAssignmentModel &base,
        std::span<const std::array<uint64_t, kGrhSimProposalPhaseCount * kGrhSimProposalAxisCount>>
            pieceFeatures);

    struct GrhSimPhaseAxisCost
    {
        uint64_t computeCore = 0;
        uint64_t commitPublish = 0;
        uint64_t wideHelper = 0;
        bool operator==(const GrhSimPhaseAxisCost &) const = default;
    };

    struct GrhSimProposalScore
    {
        // Per-phase maxima over partitions.  The two phases are kept
        // separate; they must not be added as if they were simultaneous.
        std::array<GrhSimPhaseAxisCost, kGrhSimProposalPhaseCount> maxWork{};
        RepcutTransferFeatures maxSent;
        RepcutTransferFeatures maxReceived;
        uint32_t maxSendPeers = 0;
        uint32_t maxReceivePeers = 0;
        RepcutTransferFeatures totalTransfers;
        bool operator==(const GrhSimProposalScore &) const = default;
    };

    GrhSimProposalScore scoreGrhSimAssignment(
        const RepcutAssignmentReport &report,
        const GrhSimPhaseFeatureLayout &layout);

    // Pareto dominance uses only named structural axes and independent
    // communication maxima.  No scalar coefficient or fitted time is used.
    bool grhsimProposalDominates(const GrhSimProposalScore &left,
                                 const GrhSimProposalScore &right);

    // Deterministic tie-break used only after Pareto filtering.  Its order is
    // documented by the implementation and is deliberately lexicographic,
    // not a weighted sum.
    bool grhsimProposalLexicographicallyLess(const GrhSimProposalScore &left,
                                             const GrhSimProposalScore &right);

    struct GrhSimMoveProposal
    {
        uint32_t atom = 0;
        uint32_t source = 0;
        uint32_t destination = 0;
        GrhSimProposalScore score;
    };

    struct GrhSimProposalResult
    {
        GrhSimProposalScore baseline;
        // Candidates not dominated by the baseline or another candidate,
        // ordered by (atom, destination).  This is a proposal set, not a
        // claim that one trade-off is universally best.
        std::vector<GrhSimMoveProposal> paretoMoves;
        std::optional<GrhSimMoveProposal> selectedMove;
    };

    // Enumerate one-atom moves for an explicit candidate atom set.  Requiring
    // the set prevents accidentally launching an O(ASC * K) search on a full
    // XiangShan assignment before an incremental inner loop exists.
    GrhSimProposalResult proposeGrhSimMoves(
        const RepcutAssignmentEvaluator &evaluator,
        std::span<const uint32_t> assignment,
        uint32_t partitionCount,
        std::span<const uint32_t> candidateAtoms,
        const GrhSimPhaseFeatureLayout &layout);
}

#endif
