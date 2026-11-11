#ifndef WOLVRIX_TRANSFORM_GRHSIM_SINK_SCHEDULE_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_SINK_SCHEDULE_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

using PartitionKey = std::vector<std::string>;

struct SinkGroup {
    PartitionKey partitionKey;
    grh::ValueId fire; // Empty for level-sensitive writers.
    std::vector<grh::ValueId> guards;
    friend bool operator==(const SinkGroup&, const SinkGroup&) = default;
};

struct SinkRun {
    PartitionKey partitionKey;
    std::vector<grh::OperationId> orderedWriters;
    // Sorted unique direct dependencies in SinkSchedule's topological DAG.
    // These are roots of the complete prerequisite closure, not the closure.
    std::vector<uint32_t> directNonsinkRoots;
    friend bool operator==(const SinkRun&, const SinkRun&) = default;
};

struct SinkSchedule {
    DesignStamp stamp;
    uint64_t partitionConfigHash = 0;
    bool complete = false;
    // Each logical leaf key is stored once. The CSR row for ordinal i holds
    // its immediate prerequisites, each with a smaller topological ordinal.
    std::vector<PartitionKey> nonsinkKeys;
    std::vector<std::size_t> nonsinkPrerequisiteOffsets;
    std::vector<uint32_t> nonsinkPrerequisites;
    std::vector<SinkGroup> sinkGroups;
    std::vector<SinkRun> generalRuns;
    std::vector<SinkRun> largeCommitRuns;
};

// Expand one run on demand, including transitive and effect-order prerequisites.
// Results retain the schedule's complete topological leaf order.
std::vector<uint32_t> requiredNonsinkOrdinals(const SinkSchedule& schedule, const SinkRun& run);
std::vector<PartitionKey> requiredNonsinkKeys(const SinkSchedule& schedule, const SinkRun& run);
std::string sinkScheduleKey(const grh::Graph& graph);
const SinkSchedule& requireSinkSchedule(const SessionStore* session, const grh::Graph& graph);
std::string verifySinkSchedule(const grh::Graph& graph, const SinkSchedule& schedule);

}
#endif
