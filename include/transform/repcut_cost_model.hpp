#ifndef WOLVRIX_TRANSFORM_REPCUT_COST_MODEL_HPP
#define WOLVRIX_TRANSFORM_REPCUT_COST_MODEL_HPP

#include "core/grh.hpp"

#include <cstdint>
#include <string_view>

namespace wolvrix::lib::transform
{
    // Independent of ASC/shared-closure allocation (RepcutWeightMode).
    enum class RepcutCostModel
    {
        kLegacy,
        kVerilatorV1,
        // Static estimate after known SystemVerilog emitter lowering rules.
        // Experimental and intentionally not the default.
        kVerilatorLoweredV1,
        // GrhSIM's native scalar / uint64_t-word and staged-state representation.
        // Opt-in: does not change any Verilator model or ASC construction rule.
        kGrhsimV1,
    };

    struct RepcutOperationCost
    {
        uint32_t weight = 1;
        bool usedLegacyFallback = false;
    };

    std::string_view repcutCostModelName(RepcutCostModel model);

    // Relative operation work, not bytes, instructions, cycles or activation frequency.
    RepcutOperationCost estimateRepcutOperationCost(
        const grh::Graph &graph, const grh::Operation &op, RepcutCostModel model);
}

#endif
