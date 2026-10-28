#include "transform/repcut_initial_assignment.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace wolvrix::lib::transform;

namespace
{
    void require(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <class F> void rejects(F &&action)
    {
        bool rejected = false;
        try { action(); } catch (const std::exception &) { rejected = true; }
        require(rejected, "invalid request was accepted");
    }

    const std::vector<std::string> features = {
        "grhsim_v1_closure_work", "grhsim_v1_recurring_work", "raw_operation_count", "state_write_count"};
}

int main()
{
    try
    {
        RepcutAssignmentModel model{"fixture", 7, features,
            {{{0}, {100,100,1,1}}, {{1}, {100,100,1,1}}, {{2,3}, {10,10,1,0}},
             {{2}, {2,2,1,0}}, {{3}, {3,3,1,0}}, {{4}, {5000,5000,1,1}},
             {{5}, {1,1,1,0}}, {{6}, {1,1,1,0}}},
            {{1,0,8192,{2,3}}, {2,0,32,{0,2,3}}, {3,0,33,{3}}, {4,1,129,{2,3}},
             {5,6,8192,{6}}, {6,kRepcutNoAtom,16,{5,2},0}}, {{{{5}}}}};
        RepcutAssignmentEvaluator evaluator(std::move(model));
        const auto core = prepareGrhsimInitialCore(evaluator.model());
        require(core.terminalOrder.size() == 2, "terminal classification failed");
        require(core.atomToCore[2] == kRepcutNoAtom && core.atomToCore[3] == kRepcutNoAtom,
                "expected readers were not deferred");
        for (auto atom : {0u,1u,4u,5u,6u}) require(core.atomToCore[atom] != kRepcutNoAtom,
                "writer, state writer or owner voter was deferred");
        unsigned accepted = 0;
        for (uint32_t a = 0; a < 3; ++a) for (uint32_t b = 0; b < 3; ++b)
            for (uint32_t terminal = 0; terminal < 3; ++terminal)
            {
                const std::vector<uint32_t> reference = {a,b,terminal,terminal,2,a,b};
                const auto budget = repcutInitialBudgetFromReference(evaluator.evaluate(reference,3));
                auto partial = reference;
                partial[2] = partial[3] = kRepcutNoAtom;
                const auto result = completeGrhsimInitialAssignment(evaluator,core,partial,3,budget);
                const auto repeat = completeGrhsimInitialAssignment(evaluator,core,partial,3,budget);
                require(result.accepted == repeat.accepted && result.atomPartitions == repeat.atomPartitions,
                        "construction is not deterministic");
                if (!result.accepted) continue;
                ++accepted;
                require(result.report == evaluator.evaluate(result.atomPartitions,3), "oracle mismatch");
                for (auto atom : core.coreToAtom) require(result.atomPartitions[atom] == reference[atom],
                        "construction moved an assigned core atom");
            }
        require(accepted > 0, "no fixture placement succeeded");
        const std::vector<uint32_t> partial = {0,1,kRepcutNoAtom,kRepcutNoAtom,2,0,1};
        require(!completeGrhsimInitialAssignment(evaluator,core,partial,3,{}).accepted,
                "unsatisfied budget must leave the caller's fallback available");
        rejects([&] { completeGrhsimInitialAssignment(evaluator,core,partial,65,{}); });
        auto bad = partial; bad[0] = kRepcutNoAtom;
        rejects([&] { completeGrhsimInitialAssignment(evaluator,core,bad,3,{}); });
        auto empty = evaluator.model(); empty.featureNames[1] = "another-backend-unit";
        rejects([&] { prepareGrhsimInitialCore(empty); });

        RepcutAssignmentEvaluator highBit({"high-bit",2,features,
            {{{0},{100,100,1,1}},{{1},{1,1,1,0}}},{{1,0,8192,{1}}}});
        const auto highCore = prepareGrhsimInitialCore(highBit.model());
        auto highBudget = repcutInitialBudgetFromReference(highBit.evaluate(std::array<uint32_t,2>{63,0},64));
        ++highBudget.recurringMax;
        const auto high = completeGrhsimInitialAssignment(highBit,highCore,{63,kRepcutNoAtom},64,highBudget);
        require(high.accepted && high.atomPartitions[1] == 63 && high.report.totalTransfers.calls == 0,
                "partition 63/local-read accounting failed");
        RepcutAssignmentModel terminalWriter{"terminal-writer", 2, features,
            {{{0}, {10,10,1,1}}, {{1}, {5,5,1,0}}},
            {{7,0,64,{1}}}};
        terminalWriter.atomStateFlags = {kRepcutStateWriteRegister, kRepcutStateEffect};
        RepcutAssignmentEvaluator terminalEvaluator(std::move(terminalWriter));
        const auto terminalCore = prepareGrhsimInitialCore(terminalEvaluator.model(), true);
        require(terminalCore.terminalOrder.size() == 1 && terminalCore.terminalOrder.front() == 0,
                "terminal state writer was not deferred");
        const std::vector<uint32_t> terminalReference = {1, 0};
        const auto terminalBudget = repcutInitialBudgetFromReference(
            terminalEvaluator.evaluate(terminalReference, 2));
        const auto terminalResult = completeGrhsimInitialAssignment(
            terminalEvaluator, terminalCore, {kRepcutNoAtom, 0}, 2, terminalBudget);
        require(terminalResult.accepted && terminalResult.atomPartitions == terminalReference &&
                    terminalResult.report == terminalEvaluator.evaluate(terminalResult.atomPartitions, 2),
                "deferred state writer accounting failed");
        RepcutAssignmentModel regions{"state-regions",12,features,{},
            {{0,0,8,{1}}, {1,1,8,{2}}, {2,3,8,{4}}, {3,4,8,{3}}, {4,5,8,{3}},
             {5,6,8,{6}}, {6,7,8,{8}}, {7,9,8,{10}}, {8,10,8,{3}}}};
        regions.atomStateFlags.assign(12, kRepcutStateWriteRegister);
        regions.atomStateFlags[2] = regions.atomStateFlags[10] = kRepcutStateEffect;
        regions.atomStateFlags[8] = kRepcutStateWriteMemory;
        const auto classified = classifyGrhsimStateRegions(regions);
        require(classified.terminalState[0] && classified.terminalState[1] && classified.terminalState[11],
                "downstream register chain or dead terminal was excluded");
        for (const auto atom : {3u,4u,5u,6u,7u,8u,9u,10u})
            require(classified.reachesCore[atom] && !classified.terminalState[atom],
                    "feedback, self-hold, memory or effect return escaped the core");
        require(classified.component[3] == classified.component[4] &&
                classified.componentSizes[classified.component[3]] == 2,
                "mutual state dependency was not classified as an SCC");
        RepcutAssignmentModel exactProjection{"exact-state-projection", 2, features, {},
            {{0, 1, 8, {0}}}};
        exactProjection.atomStateFlags = {kRepcutStateWriteRegister, kRepcutStateWriteRegister};
        exactProjection.hasExactStateDependencies = true;
        const auto exactProjectionClassified = classifyGrhsimStateRegions(exactProjection);
        require(exactProjectionClassified.terminalState[0] &&
                    exactProjectionClassified.terminalState[1] &&
                    exactProjectionClassified.dependencyEdges == 0,
                "exact state graph did not replace projected value readers");
        auto unavailable = regions;
        unavailable.atomStateFlags.clear();
        const auto unknown = classifyGrhsimStateRegions(unavailable);
        require(std::none_of(unknown.terminalState.begin(), unknown.terminalState.end(), [](bool v) { return v; }),
                "missing metadata must not create state candidates");
        RepcutAssignmentModel chain{"state-chain",4,features,
            {{{0},{10,10,1,1}},{{1},{10,10,1,1}},{{2},{10,10,1,1}},{{3},{10,10,1,0}}},
            {{0,0,8192,{1}}, {1,1,64,{2}}, {2,2,128,{3}}}};
        chain.atomStateFlags = {kRepcutStateWriteRegister,kRepcutStateWriteRegister,
                                kRepcutStateWriteRegister,kRepcutStateEffect};
        RepcutAssignmentEvaluator chainEvaluator(chain);
        const auto chainCore = prepareGrhsimInitialCore(chainEvaluator.model(),true);
        require(chainCore.terminalOrder == std::vector<uint32_t>{2,1,0},
                "state chain was not ordered from its fixed endpoint upstream");
        const auto chainBudget = repcutInitialBudgetFromReference(chainEvaluator.evaluate(std::array<uint32_t,4>{0,0,0,0},2));
        const auto chainResult = completeGrhsimInitialAssignment(chainEvaluator,chainCore,
            {kRepcutNoAtom,kRepcutNoAtom,kRepcutNoAtom,0},2,chainBudget);
        require(chainResult.accepted && chainResult.atomPartitions == std::vector<uint32_t>{0,0,0,0} &&
                    chainResult.report == chainEvaluator.evaluate(chainResult.atomPartitions,2),
                "deferred state chain communication diverged from the full evaluator");
        auto overflow = evaluator.evaluate(std::array<uint32_t,7>{0,1,2,2,2,0,1},3);
        overflow.totalFeatures[1] = std::numeric_limits<uint64_t>::max();
        rejects([&] { repcutInitialBudgetFromReference(overflow); });
        std::cout << "PASS initial assignment: classification, shared pieces, inferred owners, bundles, "
                     "determinism, fallback, high partition bit and overflow\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
