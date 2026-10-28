#ifndef WOLVRIX_EMIT_GRHSIM_PARTITION_WORK_HPP
#define WOLVRIX_EMIT_GRHSIM_PARTITION_WORK_HPP

#include "emit/grhsim_activity_model.hpp"

#include <optional>
#include <span>

namespace wolvrix::lib::emit
{
    struct GrhSimInputBinding
    {
        enum class Kind { Unknown, Primary, DeferredState, EarlyEffect };
        grh::ValueId input;
        Kind kind = Kind::Unknown;
        std::optional<uint32_t> writerPartition;
    };

    // Exact work for one rebuilt/scheduled partition and a caller-selected
    // activity scenario. References into work.sites are views, not extra costs.
    // Recompute after assignment/interface/schedule changes; no frozen per-ASC
    // scalar or inferred runtime probabilities are introduced here.
    struct GrhSimPartitionWork
    {
        struct SourceRead
        {
            enum class Kind { Constant, LocalState, LocalMemory };
            Kind kind = Kind::Constant;
            std::size_t site = 0;
            grh::OperationId operation;
        };
        struct InputConsumer
        {
            GrhSimInputBinding binding;
            int32_t width = 0;
            GrhSimActivityRanges arrivals;
            GrhSimExecutionPlan::ValueActivation activation;
            // Direct IR uses and activation targets are distinct quantities,
            // neither is a count of machine loads or bitmap writes.
            std::vector<grh::OperationId> directUsers;
        };
        struct WriteBody
        {
            GrhSimExecutionPlan::CommitWriteBody::Kind kind;
            uint32_t supernode = 0;
            std::vector<grh::OperationId> operations;
            std::optional<std::size_t> enabledSite;
            std::optional<std::size_t> changedSite;
        };
        uint32_t partition = 0;
        grh::GraphId graph;
        GrhSimAttributedWorkReport work;
        std::vector<SourceRead> sources;
        std::vector<InputConsumer> inputs;
        std::vector<WriteBody> bodies;
    };

    GrhSimPartitionWork summarizeGrhSimPartitionWork(
        uint32_t partition, const grh::Graph &graph, const GrhSimExecutionPlan &plan,
        const GrhSimActivityReport &activity,
        std::span<const GrhSimInputBinding> bindings);
}

#endif
