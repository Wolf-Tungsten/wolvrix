#ifndef WOLVRIX_GRHSIM_PASS_GENERAL_BOUNDARIES_HPP
#define WOLVRIX_GRHSIM_PASS_GENERAL_BOUNDARIES_HPP

#include "grhsim/ir/model.hpp"

#include <cstdint>
#include <vector>

namespace wolvrix::lib::grhsim
{

    // Predicted General-phase node partition and boundary value set (M5d-5,
    // B7 boundary-aware cloning, plan 归位决议 1; V2-M1 更新): a static
    // simulation of the cone-absorption rules of cpu.st.build-general-nodes,
    // computed without building any CPU mapping. build-general-nodes is
    // deterministic — an absorbable op (core.compute.*, core.input.read,
    // core.state.read, core.state.memRead) joins its results' single agreed
    // consumer node, while shared values and commit-boundary consumers force
    // a fresh node — so the boundary set is predictable from the sealed
    // semantic model.
    //
    // Op set (V2-M1 rule): every NON-SINK General-phase op (value-producing).
    // No-result sink ops (reg/latch writes, General-phase regLatch-class mem
    // writes, no-result calls) never enter cone absorption — each anchors a
    // singleton node trailing the non-sink nodes, so sink operand values are
    // predicted boundaries. Mem-phase writes stay out of the set and their
    // operands are predicted boundaries (P_mem samples them). Event/Output
    // cones are self-contained and never extend the set.
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
