#ifndef WOLVRIX_TRANSFORM_GRHSIM_PARTITION_NONSINK_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_PARTITION_NONSINK_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct PartitionNonsinkOptions {
    std::string top;
    uint32_t maxNodeWork = 128;
    uint32_t maxSupernodeWork = 128;
    uint32_t segmentPenalty = 1;
    bool semanticNodes = false;
    bool cloneShared = true;
    uint32_t cloneMaxFanout = 8;
    uint32_t maxClones = 250000;
    bool cloneLossyCompare = true;
};

class PartitionNonsinkPass final : public Pass {
public:
    explicit PartitionNonsinkPass(PartitionNonsinkOptions options = {});
    bool supportsDialect(std::string_view dialect) const noexcept override;
    PassResult run() override;
private:
    PartitionNonsinkOptions options_;
};

}
#endif
