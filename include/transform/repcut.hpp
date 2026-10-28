#ifndef WOLVRIX_TRANSFORM_REPCUT_HPP
#define WOLVRIX_TRANSFORM_REPCUT_HPP

#include "core/transform.hpp"
#include "transform/repcut_cost_model.hpp"

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
        RepcutCostModel costModel = RepcutCostModel::kLegacy;
        // Experimental communication emphasis for the ASC hyperedge objective.
        // The default one preserves the existing solver behavior.
        uint32_t communicationWeightScale = 1;
        // Experimental receiver-side byte-exposure tail refinement.
        // The exposure is tail-compressed and used only by closure refine;
        // zero keeps the existing solver behavior.
        uint32_t communicationReceiverWeightScale = 0;
        // Experimental wide-value reader hyperedges. A positive scale adds
        // one width-weighted edge for each wide value with multiple ASC
        // consumers; zero keeps the existing piece hypergraph unchanged.
        uint32_t communicationValueEdgeWeightScale = 0;
        bool keepIntermediateFiles = false;
        // Independent GrhSIM initial construction. The ordinary solver result
        // supplies structural budgets and a fallback, never core labels.
        std::string initialAssignment = "solver";
        double initialCoreImbalanceFactor = 0.05;
    };

    // Experimental C++ replay, selected only by partitioner="assignment-replay".
    // Put this value in the session under <path>.repcut.replay_assignment.
    // IDs refer to the ASC order of this exact input graph. Replay validates
    // every label and reuses normal reconstruction, without solving/refining.
    struct RepcutReplayAssignment
    {
        std::vector<uint32_t> atomPartitions;
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
