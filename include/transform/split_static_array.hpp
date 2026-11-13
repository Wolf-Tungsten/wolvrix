#ifndef WOLVRIX_TRANSFORM_SPLIT_STATIC_ARRAY_HPP
#define WOLVRIX_TRANSFORM_SPLIT_STATIC_ARRAY_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    struct SplitStaticArrayOptions
    {
        // Empty disables diagnostics. Multi-graph runs require {graph} in path.
        std::string traceNodeEvolution;
        std::string traceInputSha256;
        bool traceSourceEndpoints = false;
    };

    class SplitStaticArrayPass : public Pass
    {
    public:
        struct Stats
        {
            std::size_t graphs = 0;
            std::size_t changedGraphs = 0;
            std::size_t slicesVisited = 0;
            std::size_t slicesRewritten = 0;
            std::size_t valuesWithReadsSplit = 0;
            std::size_t opsCreated = 0;
            std::size_t dynamicSlicesSkipped = 0;
            std::size_t unsupportedSlicesSkipped = 0;
            std::size_t staticSlicesVisited = 0;
            std::size_t staticSlicesRewritten = 0;
            std::size_t stateArraysVisited = 0;
            std::size_t stateArraysSplit = 0;
            std::size_t stateArraysDynamicSkipped = 0;
            std::size_t stateArraysUnsupportedSkipped = 0;
            std::size_t stateArraysDeclaredSkipped = 0;
            std::size_t stateMembersCreated = 0;
            std::size_t stateReadsRewritten = 0;
            std::size_t stateWritesRewritten = 0;
            std::size_t stateWholeReadsRebuilt = 0;
            std::size_t stateWholeWritesSplit = 0;
            std::size_t stateSymbolsRenamed = 0;
        };

        SplitStaticArrayPass();
        explicit SplitStaticArrayPass(SplitStaticArrayOptions options);

        PassResult run() override;
        bool preservesDialects() const noexcept override { return true; }
        std::unique_ptr<Pass> cloneGraphKernel() const override
        {
            // A traced run writes graph files in the coordinator, preventing
            // collisions when selecting more than one graph.
            return options_.traceNodeEvolution.empty() ?
                std::make_unique<SplitStaticArrayPass>(options_) : nullptr;
        }

        const Stats &stats() const noexcept { return stats_; }

    private:
        SplitStaticArrayOptions options_;
        Stats stats_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_SPLIT_STATIC_ARRAY_HPP
