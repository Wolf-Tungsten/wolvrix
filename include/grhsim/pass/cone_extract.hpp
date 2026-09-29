#ifndef WOLVRIX_GRHSIM_PASS_CONE_EXTRACT_HPP
#define WOLVRIX_GRHSIM_PASS_CONE_EXTRACT_HPP

#include "grhsim/ir/model.hpp"

#include <span>
#include <vector>

namespace wolvrix::lib::grhsim
{

    // Copy+strip cone extraction shared by the six-phase lowering passes.
    // grhsim.lower-edge-detect (M2) uses it for the P_event cone;
    // grhsim.extract-output-cones and grhsim.migrate-timeslot-tasks (M2b)
    // reuse the same entry points for the P_output cones.
    struct ConeExtraction
    {
        std::vector<OpId> coneOps;  // original closure ops, ascending id
        std::vector<OpId> cloneOps; // clones in the same order as coneOps
        // Indexed by original ValueId.index; an invalid entry means the value
        // is not part of the extracted cone.
        std::vector<ValueId> oldToNewValues;
    };

    // Clones the full producer-side transitive closure of `sinks` (roots such as
    // core.input.read / core.state.read / core.state.memRead /
    // core.compute.constant included) and tags every clone with `phase`.
    // Object refs, parameters, name (plus a phase suffix) and origin are copied
    // unchanged. The caller rewires the sink consumers to the cloned values and
    // sweeps the original side.
    ConeExtraction extractCone(GrhSimModel &model, std::span<const ValueId> sinks,
                               SimPhase phase);

    // Fixed-point dead-cone sweep over candidate original ops: a candidate is
    // removed when none of its results has a remaining user and the op is a
    // pure cone member (core.compute.* or the read-only roots core.input.read /
    // core.state.read / core.state.memRead; writes, calls and other
    // side-effecting ops are never removed). Dual-use ops whose results still
    // feed live logic stay. Returns the ops to mask; the caller folds them into
    // its own model.compact call.
    std::vector<OpId> sweepDeadConeOps(const GrhSimModel &model,
                                       std::span<const OpId> candidateOps);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_PASS_CONE_EXTRACT_HPP
