#ifndef WOLVRIX_TRANSFORM_CONSTANT_ANALYSIS_HPP
#define WOLVRIX_TRANSFORM_CONSTANT_ANALYSIS_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform {

struct ConstantAnalysisOptions {
    std::string top;
};

// Propagate values that are provably constant through pure ordinary-GRH
// expressions.  This pass deliberately keeps graph liveness and declaration
// retention at the caller's boundary; it never runs DCE.
class ConstantAnalysisPass final : public Pass {
public:
    explicit ConstantAnalysisPass(ConstantAnalysisOptions options = {});

    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    ConstantAnalysisOptions options_;
};

} // namespace wolvrix::lib::transform

#endif
