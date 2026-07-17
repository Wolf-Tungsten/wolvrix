#ifndef WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP
#define WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{
    enum class ActivityOpClass : uint8_t
    {
        Source,
        Sink,
        Compute,
        Declaration,
        Unsupported,
    };

    enum class ActivityScheduleSupernodeKind : uint8_t
    {
        Compute = 0,
        Commit = 1,
    };

    struct ActivityScheduleOptions
    {
        std::string path;
        std::size_t maxOpInComputeSupernode = 128;
        std::size_t maxOpInComputeNode = 8192;
        std::size_t maxOpInCommitSupernode = 4096;
        std::size_t localSharedComputeMaxFanout = 2;
        std::size_t localSharedComputeMaxWidth = 64;
        std::size_t localSharedComputeMaxClones = 4096;
        std::size_t localSharedComputeMaxClonedOpPpm = 5000;
        std::size_t localSharedComputeCommonOwnerMaxClones = 4096;
        std::size_t localSharedComputeCommonOwnerMaxClonedOpPpm = 5000;
        std::size_t splitOversizeComputeNodeMaxOps = 0;
        std::size_t dpSegmentPenaltyPpm = 1000000;
        std::size_t finalFaninPullbackMaxNodeOps = 8;
        std::size_t finalFaninPullbackMaxValueWidth = 64;
        std::size_t finalFaninPullbackMinGain = 3;
        std::size_t finalFaninPullbackMaxMoves = 4096;
        std::size_t finalFaninPullbackMaxMovedOpPpm = 5000;
        std::size_t finalTerminalPushforwardMaxNodeOps = 8;
        std::size_t finalTerminalPushforwardMaxInputs = 16;
        std::size_t finalTerminalPushforwardMaxOutputs = 16;
        std::size_t finalTerminalPushforwardMaxValueWidth = 64;
        std::size_t finalTerminalPushforwardMinBaeGain = 1;
        std::size_t finalTerminalPushforwardMinBoundaryValueGain = 1;
        std::size_t finalTerminalPushforwardMaxMoves = 128;
        std::size_t finalTerminalPushforwardMaxMovedOpPpm = 200;
        std::size_t finalSiblingFusionMinGain = 4;
        std::size_t finalSiblingFusionMaxPairs = 256;
        std::size_t finalSiblingFusionMaxFusedOpPpm = 5000;
        std::size_t postDpRefineMaxRounds = 1;
        std::size_t postDpRefineMaxMoves = 4096;
        std::size_t postDpRefineMaxMovedOpPpm = 10000;
        std::size_t postDpRefineMaxRegressionPpm = 10000;
        std::size_t kahnLevelPackMaxMoves = 4096;
        std::size_t kahnLevelPackMaxMovedOpPpm = 10000;
        std::size_t kahnLevelPackMaxRegressionPpm = 10000;
        bool enableCoarsen = true;
        bool enableChainMerge = true;
        bool enableLocalSharedCompute = false;
        bool commitGuardEventBuckets = true;
        bool splitOversizeComputeNodes = false;
        bool declaredValueComputeNodeBoundary = false;
        std::string localSharedComputeCommonOwnerPolicy = "off";
        std::string finalFaninPullbackPolicy = "off";
        std::string finalTerminalPushforwardPolicy = "off";
        std::string finalSiblingFusionPolicy = "off";
        std::string postDpRefinePolicy = "off";
        std::string kahnLevelPackPolicy = "off";
        std::string finalTopoPolicy = "level-id";
        std::string exportComputeDagPath;
    };

    struct ActivityScheduleSymbolIdHash
    {
        std::size_t operator()(wolvrix::lib::grh::SymbolId id) const noexcept
        {
            return static_cast<std::size_t>(id.value);
        }
    };

    using ActivityScheduleSupernodeToOps = std::vector<std::vector<wolvrix::lib::grh::OperationId>>;
    using ActivityScheduleOpToSupernode = std::vector<uint32_t>;
    using ActivityScheduleCommitLocalityGroupByOp = std::vector<uint32_t>;
    using ActivityScheduleCommitLocalityGroupOrder = std::vector<uint32_t>;
    using ActivityScheduleDag = std::vector<std::vector<uint32_t>>;
    using ActivityScheduleValueFanout = std::vector<std::vector<uint32_t>>;
    using ActivityScheduleTopoOrder = std::vector<uint32_t>;
    using ActivityScheduleStateReadSupernodes = std::unordered_map<std::string, std::vector<uint32_t>>;
    using ActivityScheduleSupernodeKinds = std::vector<ActivityScheduleSupernodeKind>;
    using ActivityScheduleComputeNodesBySupernode = std::vector<std::vector<uint32_t>>;

    struct ActivityScheduleSummaryStats
    {
        using KindCountMap = std::map<std::string, std::size_t>;

        std::size_t supernodes = 0;
        std::size_t computeSupernodes = 0;
        std::size_t commitSupernodes = 0;
        std::size_t dagEdges = 0;
        std::size_t boundaryValues = 0;
        std::size_t boundaryActivationEdges = 0;
        std::size_t computeComputeValuePairs = 0;
        std::size_t computeCommitValuePairs = 0;
        std::size_t stateReadActivationEdges = 0;
        std::size_t memoryReadActivationEdges = 0;
        std::size_t constantActivationEdges = 0;
        std::size_t otherComputeActivationEdges = 0;
        std::size_t otherComputeSingleTargetValues = 0;
        std::size_t otherComputeMultiTargetValues = 0;
        std::size_t otherComputeSingleTargetActivationEdges = 0;
        std::size_t otherComputeMultiTargetActivationEdges = 0;
        std::size_t otherComputeUniqueSupernodePairs = 0;
        std::size_t otherComputeDuplicateActivationEdges = 0;
        std::size_t computeNodes = 0;
        std::size_t computeNodeOpsTotal = 0;
        std::size_t computeNodeCycleSplitIters = 0;
        std::size_t initialComputeSupernodes = 0;
        std::size_t initialComputeSupernodeOpsTotal = 0;
        std::size_t initialComputeSupernodeDagEdges = 0;
        std::size_t initialBoundaryValues = 0;
        std::size_t initialBoundaryActivationEdges = 0;
        std::size_t initialComputeComputeValuePairs = 0;
        std::size_t initialComputeCommitValuePairs = 0;
        std::size_t sourceClonesInComputeNodes = 0;
        std::size_t localSharedComputeClonesInComputeNodes = 0;
        std::size_t directSourceInputsToCommitSupernodes = 0;
        std::size_t commonExprComputeNodes = 0;
        std::size_t computeNodeBoundaryInputsTotal = 0;
        std::size_t computeNodeBoundaryInputNoDef = 0;
        std::size_t computeNodeBoundaryInputDefOutOfRange = 0;
        std::size_t computeNodeBoundaryInputDeclared = 0;
        std::size_t computeNodeBoundaryDeclaredValues = 0;
        std::size_t computeNodeBoundaryDeclaredEdges = 0;
        std::size_t computeNodeDeclaredCutViolationsFixed = 0;
        std::size_t computeNodeDeclaredCutViolationsFatal = 0;
        std::size_t computeNodeBoundaryInputSourceSpill = 0;
        std::size_t computeNodeBoundaryInputUnsupported = 0;
        std::size_t computeNodeBoundaryInputExistingOwner = 0;
        std::size_t computeNodeBoundaryInputExistingCommonOwner = 0;
        std::size_t computeNodeBoundaryInputShared = 0;
        std::size_t computeNodeBoundaryInputCapacity = 0;
        std::size_t computeNodeBoundaryValues = 0;
        std::size_t commitInputRootValues = 0;
        std::size_t commitSinkOps = 0;
        std::size_t commitEventKeyRuns = 0;
        std::size_t commitEventKeys = 0;
        std::size_t topoEdges = 0;
        std::size_t graphOps = 0;
        std::size_t graphValues = 0;
        KindCountMap activationEdgesBySourceKind;
        KindCountMap activationSourceValuesBySourceKind;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByKind;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByWidthBucket;
        KindCountMap computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
    };

    inline constexpr uint32_t kInvalidActivitySupernodeId = std::numeric_limits<uint32_t>::max();

    class ActivitySchedulePass : public Pass
    {
    public:
        ActivitySchedulePass();
        explicit ActivitySchedulePass(ActivityScheduleOptions options);

        PassResult run() override;

    private:
        ActivityScheduleOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_ACTIVITY_SCHEDULE_HPP
