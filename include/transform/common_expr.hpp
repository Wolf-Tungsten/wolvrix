#ifndef WOLVRIX_TRANSFORM_COMMON_EXPR_HPP
#define WOLVRIX_TRANSFORM_COMMON_EXPR_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform {

struct CommonExprOptions {
    std::string top;
};

// Share repeated pure scalar expression trees in an ordinary GRH graph.  The
// pass rewires proven duplicate producers but keeps their rows; general
// liveness cleanup stays with the caller's explicit dead-code-elim pass.
class CommonExprPass final : public Pass {
public:
    explicit CommonExprPass(CommonExprOptions options = {});

    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    CommonExprOptions options_;
};

} // namespace wolvrix::lib::transform

#endif
