#ifndef WOLVRIX_TRANSFORM_PATTERN_DETECT_HPP
#define WOLVRIX_TRANSFORM_PATTERN_DETECT_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform {

struct PatternDetectOptions {
    std::string top;
};

// Detect safe, local expression patterns in an unpartitioned ordinary GRH.
// Liveness cleanup remains an explicit caller-owned DCE boundary.
class PatternDetectPass final : public Pass {
public:
    explicit PatternDetectPass(PatternDetectOptions options = {});

    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    PatternDetectOptions options_;
};

} // namespace wolvrix::lib::transform

#endif
