#ifndef WOLVRIX_GRHSIM_BACKEND_CPU_PHASE_EMIT_HPP
#define WOLVRIX_GRHSIM_BACKEND_CPU_PHASE_EMIT_HPP

#include "grhsim/pass/pass.hpp"

#include <filesystem>

namespace wolvrix::lib::grhsim
{
    // M5 six-phase CPU C++ emit: consumes a PhaseSchedule-stage cpu mapping and
    // generates the P_input/P_event/P_general/P_mem/P_publish/P_output model
    // described by the simulation-model refactor plan. Registered under the
    // public "cpu.st.emit-cpp" pass name.
    PassResult emitSixPhaseCpuCpp(const GrhSimModel &model, const std::filesystem::path &directory,
                                  wolvrix::lib::diag::Diagnostics &diagnostics);
    void registerCpuPhaseEmitPasses(PassRegistry &registry);
}

#endif
