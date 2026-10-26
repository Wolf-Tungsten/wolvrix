#ifndef WOLVRIX_TRANSFORM_GRHSIM_SPLIT_EVENT_AND_OUTPUT_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_SPLIT_EVENT_AND_OUTPUT_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct SplitEventAndOutputOptions {
    std::string top;
};

class SplitEventAndOutputPass final : public Pass {
public:
    explicit SplitEventAndOutputPass(SplitEventAndOutputOptions options = {});
    bool supportsDialect(std::string_view dialect) const noexcept override;
    PassResult run() override;
private:
    SplitEventAndOutputOptions options_;
};

}
#endif
