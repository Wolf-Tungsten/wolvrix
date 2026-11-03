#ifndef WOLVRIX_TRANSFORM_DEAD_CODE_ELIM_HPP
#define WOLVRIX_TRANSFORM_DEAD_CODE_ELIM_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    struct DeadCodeElimOptions
    {
        std::string outputKey;
        std::size_t sampleLimit = 16;
    };

    class DeadCodeElimPass : public Pass
    {
    public:
        struct Stats
        {
            std::size_t graphs = 0;
            std::size_t changedGraphs = 0;
            std::size_t operationsVisited = 0;
            std::size_t opsRemoved = 0;
            std::size_t valuesRemoved = 0;
        };

        DeadCodeElimPass();
        explicit DeadCodeElimPass(DeadCodeElimOptions options);

        PassResult run() override;

        const Stats& stats() const noexcept { return stats_; }

    private:
        DeadCodeElimOptions options_;
        Stats stats_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_DEAD_CODE_ELIM_HPP
