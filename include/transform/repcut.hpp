#ifndef WOLVRIX_TRANSFORM_REPCUT_HPP
#define WOLVRIX_TRANSFORM_REPCUT_HPP

#include "core/transform.hpp"

#include <cstddef>
#include <string>

namespace wolvrix::lib::transform
{

    enum class RepcutWeightMode
    {
        kBaseline,
        kClosureAware,
    };

    struct RepcutOptions
    {
        std::string path;
        std::size_t partitionCount = 2;
        double imbalanceFactor = 0.015;
        std::string workDir = ".";
        std::string partitioner = "mt-kahypar";
        std::string mtKaHyParPreset = "deterministic-quality";
        std::size_t mtKaHyParThreads = 0;
        RepcutWeightMode weightMode = RepcutWeightMode::kBaseline;
        bool keepIntermediateFiles = false;
    };

    class RepcutPass : public Pass
    {
    public:
        RepcutPass();
        explicit RepcutPass(RepcutOptions options);

        PassResult run() override;

    private:
        RepcutOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_REPCUT_HPP
