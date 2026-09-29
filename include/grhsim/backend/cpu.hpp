#ifndef WOLVRIX_GRHSIM_BACKEND_CPU_HPP
#define WOLVRIX_GRHSIM_BACKEND_CPU_HPP

#include "core/diagnostics.hpp"
#include "grhsim/ir/model.hpp"

namespace wolvrix::lib::grhsim
{
    class PassRegistry;

    bool isCpuCommitOp(std::string_view opType) noexcept;
    bool verifyCpuMapping(const GrhSimModel &model, const BackendMapping &mapping,
                          wolvrix::lib::diag::Diagnostics &diagnostics);
    // M3 six-phase partition support: per-op event domain sets over the
    // General-phase ops plus the Mem-phase write ops. Both tables are indexed
    // by op index (slot 0 unused). acts[i] is the op's own event_acts cluster
    // indices (sorted, deduplicated; empty for event-free ops and for ops
    // outside the General/Mem influence graph). influence[i] is the union of
    // the acts of every event-carrying op reachable from op i through value
    // fanout (including the General -> Mem write-operand sink edges), its own
    // acts included (the E(S) closure of the M3 spec, per op). State
    // write -> read edges are excluded: state readers are activated by
    // P_publish's stateFanout, not eventActiveFlag.
    struct CpuEventDomainSets
    {
        std::vector<std::vector<int64_t>> acts;
        std::vector<std::vector<int64_t>> influence;
    };
    CpuEventDomainSets computeCpuEventDomainSets(const GrhSimModel &model);
    void registerCpuPasses(PassRegistry &registry);
    void registerCpuPartitionPasses(PassRegistry &registry);
    void registerCpuLayoutPasses(PassRegistry &registry);
    void registerCpuSchedulePasses(PassRegistry &registry);
    bool verifyCpuSchedule(const GrhSimModel &model, const CpuBackendMapping &mapping,
                           wolvrix::lib::diag::Diagnostics &diagnostics);
    bool verifyCpuDataLayout(const GrhSimModel &model, const CpuBackendMapping &mapping,
                             wolvrix::lib::diag::Diagnostics &diagnostics);
    // Recompute the canonical data layout over the mapping's partition tree and
    // install it into the cpu mapping; stage, schedule and partition tree stay
    // untouched. Required after mapping-preserving passes that rewrite operand
    // structure (e.g. grhsim.fuse-expr-chains), since helper read caches and
    // boundary densification are functions of op operands.
    bool refreshCpuDataLayout(GrhSimModel &model,
                              wolvrix::lib::diag::Diagnostics &diagnostics);
    // Recompute the canonical schedule over the mapping's partition tree and
    // data layout, and install it into the cpu mapping; stage and partition tree
    // stay untouched. Required after mapping-preserving passes that move ops
    // between partitions (e.g. grhsim.migrate-boundary-ops), since fanout,
    // shadows and task structure are functions of partition membership.
    bool refreshCpuSchedule(GrhSimModel &model,
                            wolvrix::lib::diag::Diagnostics &diagnostics);

    // NO00015 edge-completion de-monitoring selection: statically eligible
    // values priced with a dynamic per-value change profile (write/change
    // counts indexed by value id, sized values()+1; pricing only, safety is
    // static). Returns the sorted removal list plus pricing aggregates
    // (integer-scaled by 4) for diagnostics. Applying the list adds the
    // missing operand->consumer activation edges and removes the rows.
    struct DemonitorEdgeCompletionSelection
    {
        std::vector<ValueId> removed;
        uint64_t eligible = 0;
        uint64_t profitable = 0;
        uint64_t cascadeTrimmed = 0;
        uint64_t addedEdges = 0;
        int64_t saveX4 = 0;
        int64_t widenX4 = 0;
    };
    DemonitorEdgeCompletionSelection computeDemonitorEdgeCompletionSelection(
        const GrhSimModel &model, const CpuBackendMapping &mapping,
        const std::vector<uint64_t> &writeCounts, const std::vector<uint64_t> &changeCounts);
}

#endif
