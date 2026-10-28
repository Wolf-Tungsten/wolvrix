#ifndef WOLVRIX_TRANSFORM_REPCUT_GRHSIM_SCENARIOS_HPP
#define WOLVRIX_TRANSFORM_REPCUT_GRHSIM_SCENARIOS_HPP

#include "core/grh.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{
    using RepcutScenarioMask = uint64_t;
    // Uncertainty marker, not an additional execution scenario. A value with
    // this bit and no known scenario bits is not proved inactive in those cases.
    inline constexpr RepcutScenarioMask kRepcutUnknownScenario = UINT64_C(1) << 63;

    struct RepcutGrhsimEventScenarios
    {
        RepcutScenarioMask posedge = 0;
        RepcutScenarioMask negedge = 0;
        RepcutScenarioMask anyedge = 0;
    };

    // Caller-declared scenarios, not measured activation probabilities. Missing
    // primary inputs and unresolved event roots remain explicitly unknown.
    struct RepcutGrhsimScenarios
    {
        std::unordered_map<grh::ValueId, RepcutScenarioMask, grh::ValueIdHash> inputs;
        std::unordered_map<grh::ValueId, RepcutGrhsimEventScenarios, grh::ValueIdHash> events;
        // For each event-scenario bit, scenarios in which a committed state can
        // activate subsequent compute. A missing (zero) entry means unknown;
        // no alternating-clock rule is assumed by this adapter.
        std::array<RepcutScenarioMask, 63> afterCommit{};
    };

    struct RepcutGrhsimScenarioReport
    {
        // Raw GRH IDs; no partition, activity schedule or emitted C++ required.
        // Coarse whole-value may sets: inlining/group coactivation is unmodeled.
        std::vector<RepcutScenarioMask> values;
        std::vector<RepcutScenarioMask> operations;
        std::vector<RepcutScenarioMask> events;
        uint64_t unresolvedEventReferences = 0;
        uint64_t worklistVisits = 0;
    };

    RepcutGrhsimScenarioReport analyzeRepcutGrhsimScenarios(
        const grh::Graph &graph, const RepcutGrhsimScenarios &scenarios);
}

#endif
