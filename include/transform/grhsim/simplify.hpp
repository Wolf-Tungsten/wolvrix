#ifndef WOLVRIX_TRANSFORM_GRHSIM_SIMPLIFY_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_SIMPLIFY_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct SimplifyOptions {
    std::string top;
    uint32_t maxRounds = 8;
};

class SimplifyPass final : public transform::Pass {
public:
    explicit SimplifyPass(SimplifyOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;
private:
    SimplifyOptions options_;
};

} // namespace wolvrix::lib::transform::grhsim
#endif
