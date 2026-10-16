#ifndef WOLVRIX_GRHSIM_DIALECT_COMPUTE_HPP
#define WOLVRIX_GRHSIM_DIALECT_COMPUTE_HPP

#include <string_view>

namespace wolvrix::lib::grhsim
{
    // Deterministic value computations, with no object accesses or calls.
    inline bool isPureComputeOp(std::string_view name)
    {
        return name.starts_with("core.compute.") || name == "cpu.compute.maskedOr";
    }
}

#endif
