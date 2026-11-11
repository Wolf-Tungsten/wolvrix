#ifndef WOLVRIX_TRANSFORM_GRHSIM_EXPR_OPT_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_EXPR_OPT_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct ExprOptOptions {
    std::string top;
};

// The narrow expression-tree cleanup that follows the first GSIM DCE.  The
// pass deliberately has its own identity so that its cost and graph delta can
// be measured independently from the broader fixed-point simplifier.
class ExprOptPass final : public transform::Pass {
public:
    explicit ExprOptPass(ExprOptOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    ExprOptOptions options_;
};

} // namespace wolvrix::lib::transform::grhsim

#endif
