#ifndef WOLVRIX_TRANSFORM_GRHSIM_USED_BITS_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_USED_BITS_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct UsedBitsOptions {
    std::string top;
};

// Run the GSIM-style backward used-bit analysis as a standalone pass.  The
// default intentionally does not treat declared symbols as liveness roots;
// ports, effects and state accesses remain observable roots.
class UsedBitsPass final : public transform::Pass {
public:
    explicit UsedBitsPass(UsedBitsOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    UsedBitsOptions options_;
};

} // namespace wolvrix::lib::transform::grhsim

#endif
