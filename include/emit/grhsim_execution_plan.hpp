#ifndef WOLVRIX_EMIT_GRHSIM_EXECUTION_PLAN_HPP
#define WOLVRIX_EMIT_GRHSIM_EXECUTION_PLAN_HPP

#include "core/grh.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::lib::emit
{
    // A read-only snapshot of the actual pre-emission dispatch/activation plan.
    // IDs refer to the supplied graph and activity-schedule session. Rebuild the
    // plan after repartitioning, transforming or rescheduling that graph.
    // Counts and line estimates describe structure, never time or probabilities.
    struct GrhSimExecutionPlan
    {
        enum class Phase { Compute, Commit };
        enum class Predicate { Unknown, False, True };
        struct Group
        {
            uint32_t supernode = 0;
            std::size_t activeId = 0;
            Phase phase = Phase::Compute;
            std::size_t estimatedLines = 0;
            std::vector<grh::OperationId> operations;
        };
        struct Word
        {
            std::size_t activeWord = 0;
            std::vector<uint32_t> groups;
            // Half-open ranges into groups, exactly as used by the emitter.
            std::vector<std::pair<std::size_t, std::size_t>> helperChunks;
        };
        struct Batch
        {
            std::size_t index = 0;
            Phase phase = Phase::Compute;
            std::size_t estimatedLines = 0;
            std::vector<Word> words;
        };
        struct ValueActivation
        {
            struct BitMask
            {
                uint32_t target = 0;
                std::size_t word = 0;
                uint64_t mask = 0;
            };
            grh::ValueId value;
            // Active IDs, not supernode IDs. Commit groups are not in this map.
            std::vector<uint32_t> targets;
            bool inputChangeFlag = false;
            // An absent target means whole-value activation. Present entries
            // are the actual input-bit-activation planner's 64-bit masks.
            std::vector<BitMask> bitMasks;
        };
        struct StateActivation
        {
            std::string symbol;
            std::vector<uint32_t> targets;
            bool rowSelective = false;
        };
        struct Event
        {
            grh::OperationId operation;
            std::vector<grh::ValueId> values;
            std::vector<std::string> edges;
        };
        struct Write
        {
            grh::OperationId operation;
            std::string state;
            bool shadowed = false;
            bool memory = false;
            grh::ValueId enable;
            grh::ValueId mask;
            uint32_t dataWidth = 0;
            // Literal proofs from the same backend constant reader used for
            // lowering. Unknown includes dynamic predicates and X/Z literals.
            Predicate enableProof = Predicate::Unknown;
            Predicate maskNonzeroProof = Predicate::Unknown;
        };
        struct CommitEventCheck
        {
            std::size_t batch = 0;
            std::size_t activeWord = 0;
            // Exact backend predicate identity, not an optimizer name match.
            std::string expression;
            uint8_t activeMask = 0;
            std::vector<uint32_t> groups;
        };
        struct CommitEnableGroup
        {
            std::string expression;
            grh::ValueId representativeEnable;
            std::vector<grh::OperationId> operations;
        };
        struct CommitWriteBody
        {
            enum class Kind { Single, Lockstep, SameReader, OrderedMemoryAffine };
            Kind kind = Kind::Single;
            std::vector<grh::OperationId> operations;
            // Structural source-level work, NOT instructions or time. The first
            // map is conditional on event+enable; the second additionally on a
            // change. *_upper entries include short-circuit/row-selective work.
            // A same-reader body stores all its members when ANY member changes.
            std::map<std::string, uint64_t> enabledWork;
            std::map<std::string, uint64_t> changedWork;
            // Nonempty means these maps are incomplete, never a zero-cost body.
            std::string unmodeledWork;
        };
        struct CommitWriteRun
        {
            uint32_t supernode = 0;
            std::size_t begin = 0;
            std::size_t end = 0;
            std::string eventExpression;
            bool eventHandledByDispatch = false;
            // Logical predicate groups. Ordered-memory lowering can put
            // several such tests in one loop, so this is not native branches.
            std::vector<CommitEnableGroup> enables;
            std::vector<CommitWriteBody> bodies;
        };
        struct ComputeLowering
        {
            struct Site
            {
                // Attribution within this exact schedule, not independently
                // movable weights. Fused regions retain all consumed ops;
                // the surrounding supernode remains their lowering context.
                std::vector<grh::OperationId> operations;
                std::map<std::string, uint64_t> features;
            };
            uint32_t supernode = 0;
            // Special lowering membership and ordinary operations are disjoint.
            // Ordinary operations remain an explicit unpriced fallback: their
            // result-width descriptors are NOT a lowered instruction estimate.
            std::vector<grh::OperationId> loweredOperations;
            std::vector<grh::OperationId> genericOperations;
            std::map<std::string, uint64_t> features;
            std::string unmodeledWork;
            std::vector<Site> sites;
        };

        grh::GraphId graph;
        std::string graphName;
        std::size_t activeFlagBits = 0;
        std::vector<Group> groups;
        std::vector<Batch> batches;
        std::vector<ValueActivation> inputs;
        std::vector<ValueActivation> boundaries;
        std::vector<StateActivation> states;
        std::vector<Event> events;
        std::vector<Write> writes;
        std::vector<CommitEventCheck> commitEventChecks;
        std::vector<CommitWriteRun> commitWriteRuns;
        std::vector<ComputeLowering> computeLowering;
        // Explicit coverage boundary for the read-only late guard snapshot.
        std::vector<grh::OperationId> unmodeledCommitWrites;
        bool eventQualifiedContinuation = false;
        std::map<std::string, uint64_t> storageFeatures;
        // These variants can bypass ordinary per-group dispatch. The snapshot
        // records them explicitly; a consumer must not assume default dispatch.
        bool inputFullpass = false;
        bool posedgeFullpass = false;
        bool fullActiveWordConsume = false;
        bool pureEventWordBypass = false;
        bool inputBitActivation = false;
        bool producerDirtyWords = false;
        bool sharedDeferredActivation = false;
        bool activationOverlays = false;
    };
}

#endif
