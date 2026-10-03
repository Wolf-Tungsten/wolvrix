#ifndef WOLVRIX_GRHSIM_BACKEND_CPU_HPP
#define WOLVRIX_GRHSIM_BACKEND_CPU_HPP

#include "core/diagnostics.hpp"
#include "grhsim/ir/model.hpp"

namespace wolvrix::lib::grhsim
{
    class PassRegistry;

    bool verifyCpuMapping(const GrhSimModel &model, const BackendMapping &mapping,
                          wolvrix::lib::diag::Diagnostics &diagnostics);
    void registerCpuPasses(PassRegistry &registry);
    void registerCpuPartitionPasses(PassRegistry &registry);
    void registerCpuLayoutPasses(PassRegistry &registry);
    void registerCpuSchedulePasses(PassRegistry &registry);
    void registerCpuEmitPlanPasses(PassRegistry &registry);
    bool verifyCpuSchedule(const GrhSimModel &model, const CpuBackendMapping &mapping,
                           wolvrix::lib::diag::Diagnostics &diagnostics);
    bool verifyCpuDataLayout(const GrhSimModel &model, const CpuBackendMapping &mapping,
                             wolvrix::lib::diag::Diagnostics &diagnostics);
    // M5d-7: replans the TU assignment with the recorded caps and compares
    // (same recompute-and-compare pattern as verifyCpuSchedule).
    bool verifyCpuTranslationUnits(const GrhSimModel &model, const CpuBackendMapping &mapping,
                                   wolvrix::lib::diag::Diagnostics &diagnostics);
}

#endif
