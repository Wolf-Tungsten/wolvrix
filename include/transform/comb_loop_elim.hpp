#ifndef WOLVRIX_TRANSFORM_COMB_LOOP_ELIM_HPP
#define WOLVRIX_TRANSFORM_COMB_LOOP_ELIM_HPP

#include "core/transform.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    struct CombLoopReport
    {
        std::string graphName;
        std::vector<wolvrix::lib::grh::ValueId> loopValues;
        std::vector<wolvrix::lib::grh::OperationId> loopOps;
        std::optional<wolvrix::lib::grh::SrcLoc> sourceLocation;
        std::string status;
        std::string diagnosticKind;
        std::string description;
    };

    struct CombLoopElimOptions
    {
        // Maximum number of values to analyze per graph (0 means no limit).
        std::size_t maxAnalysisNodes = 0;

        // Maximum number of analysis threads (0 means use hardware concurrency).
        std::size_t numThreads = 0;

        // Whether to split values for detected false loops.
        bool fixFalseLoops = true;

        // Maximum iterations when fixing false loops.
        std::size_t maxFixIterations = 100;

        // Treat true loops as pass failures.
        bool failOnTrueLoop = false;

        // Enable array-aware loop analysis and fragmentation (kSliceArray
        // leaves the "uncertain" black hole). When false, kSliceArray keeps
        // the legacy behavior (analysis gives up on the loop).
        bool enableArrayFragmentation = true;

        // Maximum number of fragments produced per value while splitting a
        // loop (0 means no limit). Loops exceeding the cap are left intact
        // and reported as unresolved.
        std::size_t maxFragmentsPerValue = 4096;

        // Attempt to fix loops that contain a dynamic-index kSliceArray
        // (whole-interval conservative analysis + full-view re-merge). When
        // false, such loops are reported as undecidable without rewriting.
        bool fixDynamicArrayLoops = true;

        // Optional session key used to store loop reports for later inspection.
        std::string outputKey;
    };

    class CombLoopElimPass : public Pass
    {
    public:
        CombLoopElimPass();
        explicit CombLoopElimPass(CombLoopElimOptions options);

        PassResult run() override;

    private:
        CombLoopElimOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_COMB_LOOP_ELIM_HPP
