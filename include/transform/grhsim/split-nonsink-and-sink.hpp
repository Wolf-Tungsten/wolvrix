#ifndef WOLVRIX_TRANSFORM_GRHSIM_SPLIT_NONSINK_AND_SINK_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_SPLIT_NONSINK_AND_SINK_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct SplitNonsinkAndSinkOptions {
    std::string top;
    uint64_t memMinBytes = 64;
};

class SplitNonsinkAndSinkPass final : public Pass {
public:
    explicit SplitNonsinkAndSinkPass(SplitNonsinkAndSinkOptions options = {});
    bool supportsDialect(std::string_view dialect) const noexcept override;
    PassResult run() override;
private:
    SplitNonsinkAndSinkOptions options_;
};

}
#endif
