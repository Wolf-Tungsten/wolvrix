#ifndef WOLVRIX_TRANSFORM_SPLIT_NODES_HPP
#define WOLVRIX_TRANSFORM_SPLIT_NODES_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform {

struct SplitNodesOptions {
    std::string top;
};

// Split independent static bit ranges in ordinary GRH expressions.  This is
// deliberately a standalone structural pass: liveness cleanup remains the
// caller's explicit dead-code-elim stage.
class SplitNodesPass final : public Pass {
public:
    explicit SplitNodesPass(SplitNodesOptions options = {});

    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    SplitNodesOptions options_;
};

} // namespace wolvrix::lib::transform

#endif
