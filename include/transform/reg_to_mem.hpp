#ifndef WOLVRIX_TRANSFORM_REG_TO_MEM_HPP
#define WOLVRIX_TRANSFORM_REG_TO_MEM_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{
    enum class RegToMemProfitabilityPolicy
    {
        None,
        VerilatorV1,
    };

    struct RegToMemOptions
    {
        bool enableIntent = true;
        bool enableTrueMerge = true;
        bool enableOrderedWrites = false;
        bool enableDecodedWriteStorage = true;
        std::size_t minElementCount = 4;
        RegToMemProfitabilityPolicy profitabilityFilter = RegToMemProfitabilityPolicy::None;
    };

    class RegToMemPass : public Pass
    {
    public:
        RegToMemPass();
        explicit RegToMemPass(RegToMemOptions options);

        PassResult run() override;

    private:
        RegToMemOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_REG_TO_MEM_HPP
