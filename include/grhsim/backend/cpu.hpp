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
}

#endif
