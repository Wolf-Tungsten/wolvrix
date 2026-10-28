#include "transform/grhsim_assignment_proposal.hpp"

#include <iostream>
#include <stdexcept>

using namespace wolvrix::lib::transform;

namespace
{
    void check(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <class Exception = std::invalid_argument, class F> void rejects(F action)
    {
        try { action(); }
        catch (const Exception &) { return; }
        throw std::runtime_error("invalid proposal input was accepted");
    }

    RepcutAssignmentModel model()
    {
        return {
            "grhsim-proposal-fixture-v1", 5,
            {"base.work"},
            {{{0}, {10}}, {{1}, {9}}, {{2}, {7}}, {{3}, {1}}, {{4}, {0}}},
            {{101, 0, 130, {1, 2}}, {102, 1, 65, {2, 3}}}};
    }

    void scoreAndDominance()
    {
        const auto base = model();
        std::vector<std::array<uint64_t, 6>> phase(base.pieces.size());
        for (std::size_t i = 0; i < phase.size(); ++i)
            phase[i] = {i + 1, 2 * i + 1, 3 * i + 1, 4 * i + 1, 5 * i + 1, 6 * i + 1};
        const auto m = withGrhSimPhaseFeatures(base, phase);
        check(m.pieces[0].features.size() == base.pieces[0].features.size() + 6,
              "phase adapter did not append piece features");
        check(m.adapter.ends_with("+grhsim-phase-v1"), "phase adapter identity missing");
        const auto layout = makeGrhSimPhaseFeatureLayout(m);
        RepcutAssignmentEvaluator evaluator(m);
        const auto report = evaluator.evaluate(std::vector<uint32_t>{0, 0, 1, 2, 3}, 4);
        const auto score = scoreGrhSimAssignment(report, layout);
        check(score.maxWork[0] == GrhSimPhaseAxisCost{5, 9, 13}, "rising maxima mismatch");
        check(score.maxWork[1] == GrhSimPhaseAxisCost{17, 21, 25}, "falling maxima mismatch");
        check(score.maxSent.storageBytes == 56 && score.maxReceived.storageBytes == 40,
              "proposal score lost transfer maxima");

        auto lower = score, higher = score, tradeoff = score;
        lower.maxWork[0].computeCore--;
        higher.maxWork[0].computeCore++;
        tradeoff.maxWork[0].computeCore--;
        tradeoff.maxWork[0].commitPublish++;
        check(grhsimProposalDominates(lower, score) && !grhsimProposalDominates(score, lower),
              "proposal dominance failed for a strict lower vector");
        check(!grhsimProposalDominates(higher, score) && !grhsimProposalDominates(tradeoff, score),
              "proposal dominance accepted an inferior/trade-off vector");
        check(grhsimProposalLexicographicallyLess(lower, score) &&
              !grhsimProposalLexicographicallyLess(score, lower),
              "proposal lexicographic order failed");
    }

    void proposals()
    {
        const auto base = model();
        std::vector<std::array<uint64_t, 6>> phase(base.pieces.size());
        for (std::size_t i = 0; i < phase.size(); ++i)
            phase[i] = {i + 1, 2 * i + 1, 3 * i + 1, 4 * i + 1, 5 * i + 1, 6 * i + 1};
        const auto m = withGrhSimPhaseFeatures(base, phase);
        const auto layout = makeGrhSimPhaseFeatureLayout(m);
        RepcutAssignmentEvaluator evaluator(m);
        const std::vector<uint32_t> assignment{0, 0, 1, 2, 3};
        const std::vector<uint32_t> atoms{0, 1, 2, 3};
        const auto result = proposeGrhSimMoves(evaluator, assignment, 4, atoms, layout);
        check(!result.paretoMoves.empty(), "proposal search found no Pareto move");
        check(result.selectedMove.has_value(), "proposal search found no strict selected move");
        for (const auto &move : result.paretoMoves)
        {
            check(!grhsimProposalDominates(result.baseline, move.score),
                  "Pareto proposal is dominated by baseline");
            check(move.source == assignment[move.atom] && move.destination < 4 &&
                  move.destination != move.source, "proposal has invalid move metadata");
        }
        const auto selected = *result.selectedMove;
        check(grhsimProposalLexicographicallyLess(selected.score, result.baseline),
              "selected proposal is not lexicographically improving");
        const auto repeated = proposeGrhSimMoves(evaluator, assignment, 4, atoms, layout);
        check(repeated.paretoMoves.size() == result.paretoMoves.size() &&
              repeated.selectedMove->atom == selected.atom &&
              repeated.selectedMove->destination == selected.destination,
              "proposal selection is not deterministic");
        const std::vector<uint32_t> duplicateAtoms{0, 0};
        const std::vector<uint32_t> invalidAtoms{5};
        rejects([&] { proposeGrhSimMoves(evaluator, assignment, 4, duplicateAtoms, layout); });
        rejects([&] { proposeGrhSimMoves(evaluator, assignment, 4, invalidAtoms, layout); });
        rejects([&] { proposeGrhSimMoves(evaluator, assignment, 0, atoms, layout); });
    }

    void invalidLayout()
    {
        auto m = model();
        m.featureNames.pop_back();
        rejects([&] { makeGrhSimPhaseFeatureLayout(m); });
    }
}

int main()
{
    try
    {
        scoreAndDominance(); proposals(); invalidLayout();
        std::cout << "PASS: GrhSIM phase axes, Pareto dominance, deterministic proposals\n";
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
