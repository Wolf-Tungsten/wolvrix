#ifndef WOLVRIX_TRANSFORM_REPCUT_INITIAL_ASSIGNMENT_HPP
#define WOLVRIX_TRANSFORM_REPCUT_INITIAL_ASSIGNMENT_HPP

#include "transform/repcut_assignment_model.hpp"

namespace wolvrix::lib::transform
{
    struct RepcutStateRegions
    {
        std::vector<uint32_t> component;
        std::vector<uint32_t> componentSizes;
        std::vector<bool> cyclicComponents;
        std::vector<bool> reachesCore;
        std::vector<bool> terminalState;
        uint64_t dependencyEdges = 0;
    };

    // Collapse directed state/effect-result dependencies into SCCs. Self edges
    // (including self-hold) remain conservative feedback. Register ASCs are
    // terminal only if no downstream path reaches feedback, memory or latch
    // state, or an unsupported sink. This does not merge or split any ASC.
    RepcutStateRegions classifyGrhsimStateRegions(const RepcutAssignmentModel &model);

    // GrhSIM-specific initial construction over the existing semantic ASCs.
    // The raw model has closure, recurring, operation and state-write features.
    // These are independent structural quantities, not a predicted runtime.
    struct RepcutInitialCore
    {
        std::vector<uint32_t> atomToCore;
        std::vector<uint32_t> coreToAtom;
        std::vector<uint32_t> nodeWeights;
        std::vector<uint32_t> terminalOrder;
        uint64_t stateTerminalWorkSum = 0;
        uint64_t stateTerminalWorkMax = 0;
        RepcutStateRegions stateRegions;
    };

    struct RepcutInitialBudget
    {
        uint64_t recurringMax = 0;
        uint64_t recurringSum = 0;
        uint64_t operationSum = 0;
        uint64_t calls = 0;
        uint64_t bytes = 0;
        uint64_t sendCalls = 0;
        uint64_t sendBytes = 0;
        uint64_t receiveCalls = 0;
        uint64_t receiveBytes = 0;
    };

    struct RepcutInitialAssignment
    {
        bool accepted = false;
        std::string reason;
        std::vector<uint32_t> atomPartitions;
        RepcutAssignmentReport report;
        uint64_t receiverCap = 0;
        uint32_t attempts = 0;
    };

    // Writers, state-writing ASCs and inferred-owner voters stay in the core.
    // Only terminal readers with >=128 input words and more input words than
    // standalone recurring work are deferred. With includeStateTerminals,
    // acyclic downstream register regions are also eligible. No
    // symbol names are inspected.
    RepcutInitialCore prepareGrhsimInitialCore(const RepcutAssignmentModel &model,
                                               bool includeStateTerminals = false);

    // Reference labels are used ONLY to derive aggregate limits and by the
    // caller as a fallback. The core is solved independently from raw topology.
    RepcutInitialBudget repcutInitialBudgetFromReference(const RepcutAssignmentReport &reference);

    // partialAssignment contains kRepcutNoAtom exactly at terminalOrder atoms.
    // Each terminal receives one initial label. No already assigned ASC moves.
    // K is limited to 64; a caller must retain its reference on rejection.
    RepcutInitialAssignment completeGrhsimInitialAssignment(
        const RepcutAssignmentEvaluator &evaluator,
        const RepcutInitialCore &core,
        std::vector<uint32_t> partialAssignment,
        uint32_t partitionCount,
        const RepcutInitialBudget &budget);
}

#endif
