#ifndef WOLVRIX_GRHSIM_PASS_SIMPLIFY_INTERNAL_HPP
#define WOLVRIX_GRHSIM_PASS_SIMPLIFY_INTERNAL_HPP

// Internal shared entry points for the unified grhsim.simplify pipeline
// (M5d-2). Each step function carries the body of the standalone pass of the
// same name, parameterized by a SimplifyScope; the standalone registered
// passes are thin wrappers running with scope = wholeGraph.

#include "core/diagnostics.hpp"
#include "grhsim/ir/model.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim
{
    // Scope restriction shared by the simplify sub-passes.
    // wholeGraph == true: every op is eligible for rewrite/removal.
    // wholeGraph == false (phase scope): only ops whose SimPhase equals
    // `phase` are eligible; every other op is an immutable root, so partition
    // interfaces, side-effect roots and cross-partition references survive by
    // construction. Values/states shared with out-of-scope ops are never
    // removed or narrowed (their bit demand is still computed as the union
    // over all phases, because the used-bits analysis always runs globally).
    struct SimplifyScope
    {
        SimPhase phase = SimPhase::None;
        bool wholeGraph = true;

        static SimplifyScope whole() noexcept { return {}; }
        static SimplifyScope restricted(SimPhase phase) noexcept { return {phase, false}; }
        bool inScope(SimPhase opPhase) const noexcept { return wholeGraph || opPhase == phase; }
    };

    struct SimplifyStepReport
    {
        bool changed = false;
        // (key, count) pairs in the exact order the standalone pass reports
        // them; the wrapper formats them into its diagnostics.info line.
        std::vector<std::pair<std::string_view, uint64_t>> counters;
    };

    SimplifyStepReport simplifyStepConstFold(GrhSimModel &model, diag::Diagnostics &diagnostics,
                                             SimplifyScope scope);
    SimplifyStepReport simplifyStepCanonicalizeCompute(GrhSimModel &model,
                                                       diag::Diagnostics &diagnostics,
                                                       SimplifyScope scope);
    SimplifyStepReport simplifyStepBitwisePredicates(GrhSimModel &model,
                                                     diag::Diagnostics &diagnostics,
                                                     SimplifyScope scope);
    SimplifyStepReport simplifyStepBitwiseMuxes(GrhSimModel &model, diag::Diagnostics &diagnostics,
                                                SimplifyScope scope);
    SimplifyStepReport simplifyStepMuxChainFold(GrhSimModel &model, diag::Diagnostics &diagnostics,
                                                SimplifyScope scope);
    SimplifyStepReport simplifyStepUsedBits(GrhSimModel &model, diag::Diagnostics &diagnostics,
                                            SimplifyScope scope);

    // Declaration-provenance maintenance helpers (the M5d-1 contract).
    //
    // redirect*: the target entity disappears but an equivalent survivor
    // exists (assign/CSE alias, equivalent-state merge). Slices are re-targeted
    // keeping their bit ranges; a Direct slice downgrades to `kind` (Alias for
    // value redirects, Merged for equivalent-state merges), slices already
    // Alias/Merged keep their kind. `redirect` is indexed by the old entity
    // index (slot 0 unused); an invalid entry means "no redirect".
    //
    // clamp*: the target entity is rebuilt narrower (used-bits). Slices are
    // re-targeted to the replacement and their covered range is clamped to
    // [targetOffset, newWidth); a slice entirely at or beyond newWidth is
    // dropped (those bits are provably unobservable). Kind is preserved.
    // Whole-object markers (width == 0) are retargeted unchanged.
    std::size_t redirectProvenanceValueSlices(GrhSimModel &model,
                                              std::span<const ValueId> redirect,
                                              DeclProvenanceKind kind);
    std::size_t redirectProvenanceStateSlices(GrhSimModel &model,
                                              std::span<const StateId> redirect,
                                              DeclProvenanceKind kind);
    std::size_t clampProvenanceValueSlices(GrhSimModel &model,
                                           std::span<const ValueId> replacement,
                                           std::span<const uint32_t> replacementWidth);
    std::size_t clampProvenanceStateSlices(GrhSimModel &model,
                                           std::span<const StateId> replacement,
                                           std::span<const uint32_t> replacementWidth);

    // merge*: the target entity is packed into another entity at a per-entity
    // bit base (reg-to-mem row -> table element, comb-pack lane -> packed
    // value, pack-bit-registers member -> packed word). Slices are re-targeted
    // and their targetOffset shifts by `targetBase[oldIndex]`; a Direct slice
    // downgrades to `kind` (Merged for all current callers). Whole-object
    // markers (width == 0) are dropped: after a merge the declaration is
    // realized by a bit slice of the target, never by the whole target.
    std::size_t mergeProvenanceValueSlices(GrhSimModel &model,
                                           std::span<const ValueId> target,
                                           std::span<const uint64_t> targetBase,
                                           DeclProvenanceKind kind);
    std::size_t mergeProvenanceStateSlices(GrhSimModel &model,
                                           std::span<const StateId> target,
                                           std::span<const uint64_t> targetBase,
                                           DeclProvenanceKind kind);

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_PASS_SIMPLIFY_INTERNAL_HPP
