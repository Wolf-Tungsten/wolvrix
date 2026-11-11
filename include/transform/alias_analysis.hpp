#ifndef WOLVRIX_TRANSFORM_ALIAS_ANALYSIS_HPP
#define WOLVRIX_TRANSFORM_ALIAS_ANALYSIS_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform {

struct AliasAnalysisOptions {
    std::string top;
};

// Propagate identity views in an ordinary GRH graph.  This pass only rewires
// uses of type-identical scalar assign/full-slice views; it leaves the alias
// operations and values in place for the caller's explicit DCE boundary.
class AliasAnalysisPass final : public Pass {
public:
    explicit AliasAnalysisPass(AliasAnalysisOptions options = {});

    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    AliasAnalysisOptions options_;
};

} // namespace wolvrix::lib::transform

#endif
