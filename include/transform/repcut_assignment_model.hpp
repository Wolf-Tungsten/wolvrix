#ifndef WOLVRIX_TRANSFORM_REPCUT_ASSIGNMENT_MODEL_HPP
#define WOLVRIX_TRANSFORM_REPCUT_ASSIGNMENT_MODEL_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{
    inline constexpr uint32_t kRepcutNoAtom = std::numeric_limits<uint32_t>::max();

    // Optional structural metadata used by GrhSIM's initial assignment. The
    // metadata is orthogonal to the feature columns: old callers may leave
    // atomStateFlags empty and retain the old behavior.
    inline constexpr uint8_t kRepcutStateWriteRegister = 1u << 0;
    inline constexpr uint8_t kRepcutStateWriteMemory = 1u << 1;
    inline constexpr uint8_t kRepcutStateWriteLatch = 1u << 2;
    inline constexpr uint8_t kRepcutStateEffect = 1u << 3;
    inline constexpr uint8_t kRepcutStateOutput = 1u << 4;
    inline constexpr uint8_t kRepcutStateUnsupported = 1u << 5;
    // Experimental assignment accounting, independent of the production solver.
    // Feature columns are separate quantities, not interchangeable time units.
    struct RepcutWorkPiece
    {
        std::vector<uint32_t> atoms;
        std::vector<uint64_t> features;
    };

    struct RepcutDirectedValue
    {
        uint64_t key = 0;
        uint32_t writerAtom = 0;
        uint64_t width = 0;
        std::vector<uint32_t> readerAtoms;
        // For writer-free storage only. All reads of the same storage refer to
        // one owner group; each distinct user operation votes once per part.
        uint32_t inferredOwner = kRepcutNoAtom;
    };

    struct RepcutInferredOwner
    {
        std::vector<std::vector<uint32_t>> userAtoms;
    };

    struct RepcutStateDependency
    {
        uint32_t sourceAtom = kRepcutNoAtom;
        uint32_t sinkAtom = kRepcutNoAtom;
        bool operator==(const RepcutStateDependency &) const = default;
    };

    struct RepcutAssignmentModel
    {
        std::string adapter;
        uint32_t atomCount = 0;
        std::vector<std::string> featureNames;
        std::vector<RepcutWorkPiece> pieces;
        std::vector<RepcutDirectedValue> values;
        std::vector<RepcutInferredOwner> inferredOwners = {};
        // One optional bit mask per atom describing its sinks. Used together
        // with directed state/effect-result dependencies, never as a name filter.
        std::vector<uint8_t> atomStateFlags = {};
        // Exact value-level state-read to state-sink dependencies. When set,
        // the GrhSIM state classifier uses these edges instead of projecting
        // every direct value user to an ASC. The latter remains available for
        // ordinary communication/evaluator accounting.
        std::vector<RepcutStateDependency> stateDependencies = {};
        std::vector<uint32_t> stateCoreSources = {};
        bool hasExactStateDependencies = false;
    };

    // Static work of the current GrhSIM copy_changed helpers. A full block is
    // a 64-byte comparison group, NOT a physical cache line or an actual write.
    struct RepcutTransferFeatures
    {
        uint64_t calls = 0;
        uint64_t scalarCalls = 0;
        uint64_t wideCalls = 0;
        uint64_t storageBytes = 0;
        uint64_t full64ByteBlocks = 0;
        uint64_t tailCalls = 0;
        uint64_t tailBytes = 0;
        bool operator==(const RepcutTransferFeatures &) const = default;
    };

    RepcutTransferFeatures grhsimTransferFeaturesV1(uint64_t width);

    struct RepcutTransferModel
    {
        std::string name;
        std::function<RepcutTransferFeatures(uint64_t)> estimate;
    };

    struct RepcutTransferMember
    {
        uint64_t key = 0;
        uint64_t width = 0;
        uint64_t lowBit = 0;
        bool operator==(const RepcutTransferMember &) const = default;
    };

    struct RepcutTransferGroup
    {
        uint32_t ownerPart = 0;
        std::vector<uint32_t> readerParts;
        std::vector<RepcutTransferMember> members;
        uint64_t width = 0;
        RepcutTransferFeatures perReader;
        bool operator==(const RepcutTransferGroup &) const = default;
    };

    struct RepcutAssignmentPart
    {
        std::vector<uint64_t> features;
        uint64_t pieceCopies = 0;
        RepcutTransferFeatures sent;
        RepcutTransferFeatures received;
        uint32_t sendPeers = 0;
        uint32_t receivePeers = 0;
        bool operator==(const RepcutAssignmentPart &) const = default;
    };

    struct RepcutAssignmentReport
    {
        std::vector<RepcutAssignmentPart> parts;
        std::vector<uint64_t> uniqueFeatures;
        std::vector<uint64_t> totalFeatures;
        std::vector<RepcutTransferGroup> transfers;
        RepcutTransferFeatures totalTransfers;
        bool operator==(const RepcutAssignmentReport &) const = default;
    };

    // Direct reconstruction scope for two assignments of the SAME raw model,
    // using reports from that model's evaluator. Includes moved-atom endpoints
    // and every sender/reader of a changed bundle layout, even at equal bytes.
    // This is NOT the complete activity invalidation set: changed output arrival
    // masks must still propagate through downstream consumers to a fixed point.
    std::vector<uint32_t> repcutAssignmentDirectImpact(
        std::span<const uint32_t> beforeAssignment, const RepcutAssignmentReport &before,
        std::span<const uint32_t> afterAssignment, const RepcutAssignmentReport &after);

    // Owns a normalized model. Duplicate references are set membership, never
    // extra work; invalid IDs, duplicate value keys and arithmetic overflow fail.
    class RepcutAssignmentEvaluator
    {
    public:
        explicit RepcutAssignmentEvaluator(RepcutAssignmentModel model,
            RepcutTransferModel transfers = {"grhsim-copy-v1", grhsimTransferFeaturesV1});
        const RepcutAssignmentModel &model() const { return model_; }
        const std::string &transferModelName() const { return transferModel_.name; }
        RepcutAssignmentReport evaluate(std::span<const uint32_t> assignment,
                                        uint32_t partitionCount) const;

    private:
        friend class RepcutAssignmentState;
        void validateAssignment(std::span<const uint32_t>, uint32_t) const;
        void evaluateTransfers(std::span<const uint32_t>, RepcutAssignmentReport &) const;
        RepcutAssignmentModel model_;
        RepcutTransferModel transferModel_;
        std::vector<std::vector<uint32_t>> atomPieces_;
    };

    // The evaluator must outlive this state. Piece loads update incrementally
    // using first/last occupancy. Transfers currently rebuild in full: this is
    // the correctness reference for a future incremental communication planner,
    // not yet the inner loop of a large-scale search.
    class RepcutAssignmentState
    {
    public:
        RepcutAssignmentState(const RepcutAssignmentEvaluator &evaluator,
                              std::vector<uint32_t> assignment, uint32_t partitionCount);
        RepcutAssignmentState(RepcutAssignmentEvaluator &&, std::vector<uint32_t>, uint32_t) = delete;
        void moveAtom(uint32_t atom, uint32_t destination);
        const std::vector<uint32_t> &assignment() const { return assignment_; }
        const RepcutAssignmentReport &report() const { return report_; }

    private:
        const RepcutAssignmentEvaluator &evaluator_;
        std::vector<uint32_t> assignment_;
        RepcutAssignmentReport report_;
        std::vector<std::vector<std::pair<uint32_t, uint32_t>>> occupancy_;
    };
}

#endif
