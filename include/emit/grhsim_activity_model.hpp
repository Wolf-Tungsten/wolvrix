#ifndef WOLVRIX_EMIT_GRHSIM_ACTIVITY_MODEL_HPP
#define WOLVRIX_EMIT_GRHSIM_ACTIVITY_MODEL_HPP

#include "emit/grhsim_execution_plan.hpp"

#include <unordered_map>

namespace wolvrix::lib::emit
{
    // Symbolic scenarios chosen by the caller, NOT probabilities or measured
    // fire counts. Unknown is a separate uncertainty marker, not an extra step.
    using GrhSimActivityMask = uint64_t;
    inline constexpr GrhSimActivityMask kGrhSimUnknownActivity = UINT64_C(1) << 63;
    struct GrhSimActivityRange
    {
        uint32_t begin = 0;
        uint32_t end = 0; // exclusive, low bit first
        GrhSimActivityMask mask = 0;
        bool operator==(const GrhSimActivityRange &) const = default;
    };
    using GrhSimActivityRanges = std::vector<GrhSimActivityRange>;
    struct GrhSimEventActivity
    {
        GrhSimActivityMask posedge = 0;
        GrhSimActivityMask negedge = 0;
        GrhSimActivityMask anyedge = 0;
    };
    struct GrhSimActivitySeeds
    {
        // Missing primary inputs are unknown, not inactive. An explicit empty
        // range vector proves no recurring change for that input in this basis.
        std::unordered_map<grh::ValueId, GrhSimActivityRanges, grh::ValueIdHash> inputs;
        // Multiple unrelated clock domains retain independent scenario masks.
        // Only 1-bit assign/not aliases of these roots are resolved automatically.
        std::unordered_map<grh::ValueId, GrhSimEventActivity, grh::ValueIdHash> events;
    };
    enum class GrhSimUnseededEvents
    {
        OpaqueControl,
        ValueDependencies,
        // Conditional slice, NOT a complete may envelope: unseeded event
        // predicates are kept in plan.events but disabled in this slice.
        SeededRootsOnly
    };
    struct GrhSimActivityOptions
    {
        bool preserveBitRanges = true;
        // A derived edge cannot occur unless its event value changes. Propagate
        // that dependency together with state feedback instead of injecting an
        // unrelated unknown source. Edge polarity is conservative for non-aliases.
        GrhSimUnseededEvents unseededEvents = GrhSimUnseededEvents::ValueDependencies;
        bool honorProvedWriteGuards = true;
    };
    struct GrhSimActivityReport
    {
        // Indexed by ValueId.index / OperationId.index / active ID respectively.
        std::vector<GrhSimActivityRanges> values;
        std::vector<GrhSimActivityMask> operationMasks;
        // Separate event-qualified visits from possibly enabled/masked writes.
        // These are may sets, NOT event counts, store counts or probabilities.
        std::vector<GrhSimActivityMask> eventMasks;
        std::vector<GrhSimActivityMask> writeMasks;
        std::vector<GrhSimActivityMask> computeGroupMasks;
        std::map<std::string, GrhSimActivityMask> stateMasks;
        uint64_t unresolvedEvents = 0;
        uint64_t derivedEventReferences = 0;
        uint64_t derivedEventReferencesWithUnknown = 0;
        uint64_t residualEventReferences = 0;
        uint64_t provedDisabledWrites = 0;
        uint64_t worklistVisits = 0;
    };
    struct GrhSimWorkReport
    {
        // Inventory per planner traversal, not a number of traversals per step.
        std::map<std::string, uint64_t> staticFeatures;
        // Joint may masks retain uncertainty/phase overlap without multiplying
        // by an invented enable, toggle or event probability. Mask zero is kept.
        std::map<GrhSimActivityMask, std::map<std::string, uint64_t>> conditionalFeatures;
        uint64_t unmodeledCommitBodies = 0;
        uint64_t unmodeledComputeGroups = 0;
        uint64_t genericComputeOperations = 0;
    };

    struct GrhSimAttributedWorkReport
    {
        struct Site
        {
            enum class Kind { Compute, EventCheck, EnableCheck, EnabledBody, ChangedBody };
            Kind kind = Kind::Compute;
            uint32_t supernode = 0;
            GrhSimActivityMask mask = 0;
            // One shared body/check is one site, never divided among its ops.
            // IDs and features are valid only for this graph and schedule.
            std::vector<grh::OperationId> operations;
            std::map<std::string, uint64_t> features;
        };
        GrhSimWorkReport summary;
        std::vector<Site> sites;
        // Older/manually constructed plans may lack compute site provenance.
        // Preserve their work explicitly instead of inventing per-op weights.
        std::map<GrhSimActivityMask, std::map<std::string, uint64_t>> unattributedFeatures;
    };

    // A structural, phase/scenario-preserving view of the work report.  The
    // family names identify mutually exclusive backend shape buckets; values
    // inside a bucket retain their original feature names so an adapter can
    // choose the appropriate mechanism unit without inventing coefficients.
    // The activity mask is intentionally retained instead of being called a
    // clock phase: callers decide which scenario bits represent rising,
    // falling, or another workload phase.
    struct GrhSimBackendCostReport
    {
        using FeatureMap = std::map<std::string, uint64_t>;
        using FamilyMap = std::map<std::string, FeatureMap>;
        std::map<GrhSimActivityMask, FamilyMap> conditionalFeatures;
        std::map<std::string, uint64_t> staticFeatures;
        // Nonempty means a newly added feature has no explicit bucket yet. It
        // must never be silently charged as zero or folded into a catch-all.
        // Keep its mask too, so an adapter can diagnose the missing rule in
        // the same scenario context as the known families.
        std::map<GrhSimActivityMask, FeatureMap> unbucketedFeatures;
    };

    // Compose structural lowering features with a caller-selected activity
    // slice. This is a feature vector with explicit coverage, NOT a time model.
    GrhSimWorkReport summarizeGrhSimWork(const GrhSimExecutionPlan &plan,
                                       const GrhSimActivityReport &activity);

    // Exact provenance for the conditional summary; static dispatch/coverage
    // remains in summary.staticFeatures. Replan after changing the assignment.
    GrhSimAttributedWorkReport attributeGrhSimWork(const GrhSimExecutionPlan &plan,
                                                  const GrhSimActivityReport &activity);

    GrhSimBackendCostReport bucketizeGrhSimWork(const GrhSimWorkReport &work);

    // Mechanism units are named structural events/upper bounds, not weighted
    // time estimates.  Attribution is retained so consumers can audit that
    // every source feature contributes once to one named unit.
    struct GrhSimMechanismCostReport
    {
        using FeatureMap = std::map<std::string, uint64_t>;
        using UnitMap = std::map<std::string, uint64_t>;
        using AttributionMap = std::map<std::string, FeatureMap>;
        std::map<GrhSimActivityMask, UnitMap> conditionalUnits;
        std::map<GrhSimActivityMask, AttributionMap> conditionalAttribution;
        UnitMap staticUnits;
        AttributionMap staticAttribution;
        // A nonempty map means the mechanism vocabulary is incomplete.  Keep
        // the scenario mask and raw feature instead of assigning a fake zero.
        std::map<GrhSimActivityMask, FeatureMap> unmappedFeatures;
    };

    // Project bucketed structural features into named backend mechanism
    // units.  This preserves scenario masks and does not apply coefficients,
    // probabilities, or a sum that claims to predict walltime.
    GrhSimMechanismCostReport projectGrhSimMechanismUnits(
        const GrhSimBackendCostReport &backend);

    GrhSimActivityMask grhsimActivityInRange(const GrhSimActivityRanges &ranges,
                                            uint32_t begin, uint32_t end);
    // Preserves ranges through concat/static slices/aliases; conservatively
    // unions operand activity for other computations. State change is qualified
    // by the actual backend event operands. Enable/toggle probabilities and
    // repeated delta rounds are not predicted. Startup is excluded.
    GrhSimActivityReport analyzeGrhSimActivity(const grh::Graph &graph,
                                              const GrhSimExecutionPlan &plan,
                                              const GrhSimActivitySeeds &seeds,
                                              bool preserveBitRanges = true);
    // Explicit factored-guard analysis. The bool overload above preserves the
    // v1 opaque-event control for independent comparisons.
    GrhSimActivityReport analyzeGrhSimActivity(const grh::Graph &graph,
                                              const GrhSimExecutionPlan &plan,
                                              const GrhSimActivitySeeds &seeds,
                                              const GrhSimActivityOptions &options);
}

#endif
