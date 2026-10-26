#ifndef WOLVRIX_TRANSFORM_GRH_TO_GRHSIM_HPP
#define WOLVRIX_TRANSFORM_GRH_TO_GRHSIM_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct GrhToGrhsimOptions {
    std::string top;
    grh::LogicDomain logicDomain = grh::LogicDomain::TwoState;
};

class GrhToGrhsimPass final : public Pass {
public:
    explicit GrhToGrhsimPass(GrhToGrhsimOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;

private:
    GrhToGrhsimOptions options_;
};

} // namespace wolvrix::lib::transform::grhsim

#endif
