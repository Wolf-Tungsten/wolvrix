#ifndef WOLVRIX_GRHSIM_BACKEND_CPU_EMIT_HPP
#define WOLVRIX_GRHSIM_BACKEND_CPU_EMIT_HPP

#include "grhsim/pass/pass.hpp"

#include <filesystem>

namespace wolvrix::lib::grhsim
{
    PassResult emitCpuCpp(const GrhSimModel &model, const std::filesystem::path &directory,
                          wolvrix::lib::diag::Diagnostics &diagnostics, bool dynamicStats = false, bool commitCompactWalk = false,
                          bool commitMemWalk = false);
    void registerCpuEmitPasses(PassRegistry &registry);
}

#endif
