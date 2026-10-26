#ifndef WOLVRIX_TRANSFORM_GRHSIM_PARTITION_SINK_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_PARTITION_SINK_HPP

#include "transform/grhsim/sink-schedule.hpp"

namespace wolvrix::lib::transform::grhsim {

struct PartitionSinkOptions {
    std::string top;
    uint32_t enableGuardMinSize = 8; // Zero disables hoisting shared enables.
};

class PartitionSinkPass final : public Pass {
public:
    explicit PartitionSinkPass(PartitionSinkOptions options = {});
    bool supportsDialect(std::string_view dialect) const noexcept override;
    PassResult run() override;
private:
    PartitionSinkOptions options_;
};

}
#endif
