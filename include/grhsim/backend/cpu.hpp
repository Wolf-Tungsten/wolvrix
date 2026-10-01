#ifndef WOLVRIX_GRHSIM_BACKEND_CPU_HPP
#define WOLVRIX_GRHSIM_BACKEND_CPU_HPP

#include "core/diagnostics.hpp"
#include "grhsim/ir/model.hpp"

namespace wolvrix::lib::grhsim
{
    class PassRegistry;

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
}

#endif
