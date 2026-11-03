#pragma once

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    class RedundantElimPass : public Pass
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

        RedundantElimPass();
        PassResult run() override;

        const Stats& stats() const noexcept { return stats_; }

    private:
        Stats stats_;
    };

} // namespace wolvrix::lib::transform
