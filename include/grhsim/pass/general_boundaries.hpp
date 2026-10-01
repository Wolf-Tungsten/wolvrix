#ifndef WOLVRIX_GRHSIM_PASS_GENERAL_BOUNDARIES_HPP
#define WOLVRIX_GRHSIM_PASS_GENERAL_BOUNDARIES_HPP

#include "grhsim/ir/model.hpp"

#include <cstdint>
#include <vector>

namespace wolvrix::lib::grhsim
{

    // Predicted General-phase node partition and boundary value set (M5d-5,
    // B7 boundary-aware cloning, plan 归位决议 1): a static simulation of the
    // cone-absorption rules of cpu.st.build-general-nodes, computed without
    // building any CPU mapping. build-general-nodes is deterministic — an
    // absorbable op (core.compute.*, core.input.read, core.state.read,
    // core.state.memRead) joins its results' single agreed consumer node,
    // while shared values and commit-boundary consumers force a fresh node —
    // so the boundary set is predictable from the sealed semantic model.
    //
    // Op set (forward rule, matching the C1 rework in M5d-6): every
    // General-phase op, including General-tagged mem writes (writes on
    // regLatch-class states anchor nodes like regWrite/latchWrite sinks).
    // Mem-phase writes stay out of the set and their operands are predicted
    // boundaries (P_mem samples them). Event/Output cones are self-contained
    // and never extend the set.
    //
    // The legacy backend's build-general-nodes currently excludes mem write
    // op types (M5d-5 compat shim); on models without General-phase mem
    // writes the prediction coincides with its node formation exactly.
    struct GeneralBoundaryPrediction
    {
        static constexpr uint32_t kNoNode = ~0u;
        // Op id index -> predicted node, kNoNode for ops outside the set.
        std::vector<uint32_t> nodeOfOp;
        // Value index -> predicted boundary value (crosses nodes or sampled
        // by a Mem-phase write).
        std::vector<uint8_t> boundaryValue;
        uint32_t nodeCount = 0;
    };

    // maxOpsPerNode mirrors the build-general-nodes --max-op-in-compute-node
    // cap (default 128).
    GeneralBoundaryPrediction predictGeneralBoundaries(const GrhSimModel &model,
                                                       uint32_t maxOpsPerNode);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_PASS_GENERAL_BOUNDARIES_HPP
