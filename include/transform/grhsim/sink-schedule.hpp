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
    std::vector<PartitionKey> requiredNonsinkKeys;
    friend bool operator==(const SinkRun&, const SinkRun&) = default;
};

struct SinkSchedule {
    DesignStamp stamp;
    uint64_t partitionConfigHash = 0;
    bool complete = false;
    std::vector<SinkGroup> sinkGroups;
    std::vector<SinkRun> generalRuns;
    std::vector<SinkRun> largeCommitRuns;
};

std::string sinkScheduleKey(const grh::Graph& graph);
const SinkSchedule& requireSinkSchedule(const SessionStore* session, const grh::Graph& graph);
std::string verifySinkSchedule(const grh::Graph& graph, const SinkSchedule& schedule);

}
#endif
