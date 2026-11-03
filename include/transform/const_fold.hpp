#ifndef WOLVRIX_TRANSFORM_CONST_FOLD_HPP
#define WOLVRIX_TRANSFORM_CONST_FOLD_HPP

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

    struct ConstantFoldOptions
    {
        enum class XFoldMode
        {
            Strict,
            Known,
            Propagate
        };
        enum class Semantics
        {
            FourState,
            TwoState
        };
        XFoldMode xFold = XFoldMode::Known;
        Semantics semantics = Semantics::FourState;
    };

    // Forward declarations for internal types
    struct ConstantValue;
    struct ConstantKey;
    struct ConstantKeyHash;

    class ConstantFoldPass : public Pass
    {
    public:
        // Per-run counters used by simplify diagnostics.  They intentionally
        // describe scheduling work as well as rewrites so a fixed-point round
        // that makes no progress can be distinguished from one that merely
        // propagated a long dependency chain.
        struct Stats
        {
            std::size_t graphs = 0;
            std::size_t changedGraphs = 0;
            std::size_t constantsVisited = 0;
            std::size_t worklistEnqueued = 0;
            std::size_t worklistPopped = 0;
            std::size_t foldedOps = 0;
            std::size_t dedupedConstants = 0;
            std::size_t sliceCandidates = 0;
            std::size_t simplifiedSlices = 0;
            std::size_t comparisonCandidates = 0;
            std::size_t unsignedComparisons = 0;
            std::size_t deadConstantCandidates = 0;
            std::size_t deadConstants = 0;
            std::size_t opsErased = 0;
        };

        ConstantFoldPass();
        explicit ConstantFoldPass(ConstantFoldOptions options);

        PassResult run() override;

        const Stats& stats() const noexcept { return stats_; }

        // Slice/compare rewrites run after folding and may expose new folds.
        bool needsRevisit() const noexcept { return needsRevisit_; }

    private:
        using ConstantStore = std::unordered_map<wolvrix::lib::grh::ValueId, ConstantValue, wolvrix::lib::grh::ValueIdHash>;
        using ConstantPool = std::unordered_map<ConstantKey, wolvrix::lib::grh::ValueId, ConstantKeyHash>;

        // Per-graph folding context
        struct GraphFoldContext
        {
            wolvrix::lib::grh::Graph &graph;
            ConstantStore &constants;  // Shared across graphs (values can reference constants from other graphs)
            std::unique_ptr<ConstantPool> pool;  // Per-graph constant pool for deduplication
            bool &failed;
            bool protectDeclaredSymbols = false;
            std::size_t dedupedConstants = 0;
            std::size_t foldedOpsCount = 0;
            std::size_t simplifiedSlices = 0;
            std::size_t deadConstantsRemoved = 0;
            std::size_t unsignedCmpSimplified = 0;
            std::size_t opsErased = 0;
            std::size_t constantsVisited = 0;
            std::size_t worklistEnqueued = 0;
            std::size_t worklistPopped = 0;
            std::size_t sliceCandidates = 0;
            std::size_t comparisonCandidates = 0;
            std::size_t deadConstantCandidates = 0;
        };

        // Process a single graph for constant folding
        bool processSingleGraph(GraphFoldContext &ctx);

        // Individual optimization passes
        bool collectConstants(GraphFoldContext &ctx);
        bool iterativeFolding(GraphFoldContext &ctx);
        bool simplifySlices(GraphFoldContext &ctx);
        bool eliminateDeadConstants(GraphFoldContext &ctx);
        bool simplifyUnsignedComparisons(GraphFoldContext &ctx);

        ConstantFoldOptions options_;
        bool needsRevisit_ = false;
        Stats stats_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_CONST_FOLD_HPP
