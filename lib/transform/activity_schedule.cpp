#include "transform/activity_schedule.hpp"

#include "core/toposort.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                                 std::string_view key)
        {
            const auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::string escapeJsonString(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char ch : text)
            {
                switch (ch)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(ch); break;
                }
            }
            return out;
        }

        template <typename T>
        std::optional<T> getAttrValue(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            const auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        using ValueCanonicalMap =
            std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>;

        wolvrix::lib::grh::ValueId canonicalActivityValue(wolvrix::lib::grh::ValueId value,
                                                          const ValueCanonicalMap *canonicalValues)
        {
            if (canonicalValues == nullptr)
            {
                return value;
            }
            const auto it = canonicalValues->find(value);
            if (it == canonicalValues->end())
            {
                return value;
            }
            return it->second;
        }

        bool isSinkPartitionOp(const wolvrix::lib::grh::Operation &op)
        {
            if (!op.results().empty())
            {
                return false;
            }
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                return true;
            default:
                return false;
            }
        }

        std::string describeOp(const wolvrix::lib::grh::Graph &graph,
                               wolvrix::lib::grh::OperationId opId)
        {
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            if (!op.symbolText().empty())
            {
                return std::string(op.symbolText());
            }
            std::ostringstream oss;
            oss << wolvrix::lib::grh::toString(op.kind()) << "#" << opId.index;
            return oss.str();
        }

        std::string describeValue(const wolvrix::lib::grh::Graph &graph,
                                  wolvrix::lib::grh::ValueId value)
        {
            if (!value.valid())
            {
                return "<invalid>";
            }
            const wolvrix::lib::grh::Value valueInfo = graph.getValue(value);
            std::ostringstream oss;
            if (!valueInfo.symbolText().empty())
            {
                oss << valueInfo.symbolText();
            }
            else
            {
                oss << "value#" << value.index;
            }
            oss << "(id=" << value.index << ",width=" << valueInfo.width() << ")";
            return oss.str();
        }

        std::vector<std::string> splitPath(std::string_view path)
        {
            std::vector<std::string> out;
            std::string current;
            for (const char ch : path)
            {
                if (ch == '.')
                {
                    if (!current.empty())
                    {
                        out.push_back(current);
                        current.clear();
                    }
                    continue;
                }
                current.push_back(ch);
            }
            if (!current.empty())
            {
                out.push_back(current);
            }
            return out;
        }

        wolvrix::lib::grh::OperationId findUniqueInstance(const wolvrix::lib::grh::Graph &graph,
                                                          std::string_view instanceName)
        {
            wolvrix::lib::grh::OperationId found = wolvrix::lib::grh::OperationId::invalid();
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    continue;
                }
                const auto name = getAttrString(op, "instanceName");
                if (!name || *name != instanceName)
                {
                    continue;
                }
                if (found.valid())
                {
                    return wolvrix::lib::grh::OperationId::invalid();
                }
                found = opId;
            }
            return found;
        }

        std::optional<std::string> resolveTargetGraphName(wolvrix::lib::grh::Design &design,
                                                          std::string_view path,
                                                          std::string &error)
        {
            const std::vector<std::string> segments = splitPath(path);
            if (segments.empty())
            {
                error = "activity-schedule path must not be empty";
                return std::nullopt;
            }
            if (segments.size() == 1)
            {
                if (design.findGraph(segments.front()) == nullptr)
                {
                    error = "activity-schedule graph not found: " + segments.front();
                    return std::nullopt;
                }
                return segments.front();
            }

            auto *current = design.findGraph(segments.front());
            if (current == nullptr)
            {
                error = "activity-schedule root graph not found: " + segments.front();
                return std::nullopt;
            }
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                const auto instOp = findUniqueInstance(*current, segments[i]);
                if (!instOp.valid())
                {
                    error = "activity-schedule instance not found or not unique: " + segments[i];
                    return std::nullopt;
                }
                const auto op = current->getOperation(instOp);
                const auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    error = "activity-schedule instance missing moduleName: " + segments[i];
                    return std::nullopt;
                }
                current = design.findGraph(*moduleName);
                if (current == nullptr)
                {
                    error = "activity-schedule target module graph not found: " + *moduleName;
                    return std::nullopt;
                }
            }
            return current->symbol();
        }

        bool isHierLikeOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kXMRRead:
            case wolvrix::lib::grh::OperationKind::kXMRWrite:
                return true;
            default:
                return false;
            }
        }

        bool isStorageDeclOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kMemory:
            case wolvrix::lib::grh::OperationKind::kLatch:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
                return true;
            default:
                return false;
            }
        }

        bool isPartitionableOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            return !isStorageDeclOpKind(kind) && !isHierLikeOpKind(kind);
        }

        std::optional<std::string> stateSymbolForReadOp(const wolvrix::lib::grh::Operation &op)
        {
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                return getAttrString(op, "regSymbol");
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return getAttrString(op, "latchSymbol");
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return getAttrString(op, "memSymbol");
            default:
                return std::nullopt;
            }
        }

        struct ActivityScheduleBuild
        {
            ActivityScheduleSupernodeToOps supernodeToOps;
            ActivityScheduleOpToSupernode opToSupernode;
            std::vector<std::vector<uint32_t>> dag;
            ActivityScheduleValueFanout valueFanout;
            std::vector<wolvrix::lib::grh::OperationKind> valueSourceKind;
            std::vector<uint32_t> valueSourceSupernode;
            ActivityScheduleTopoOrder topoOrder;
            ActivityScheduleStateReadSupernodes stateReadSupernodes;
            ActivityScheduleSupernodeKinds supernodeKinds;
            ActivityScheduleComputeNodesBySupernode computeNodesBySupernode;
        };

        bool isRegToMemIntentSlice(const wolvrix::lib::grh::Operation &op);

        std::optional<wolvrix::lib::grh::ValueId>
        regToMemIntentSliceIndexValue(const wolvrix::lib::grh::Graph &graph,
                                      const wolvrix::lib::grh::Operation &op);

        std::vector<std::string>
        regToMemIntentSliceStorageReadSymbols(const wolvrix::lib::grh::Graph &graph,
                                              const wolvrix::lib::grh::Operation &op);

        std::string encodeActivityScheduleSummaryStatsJson(const ActivityScheduleSummaryStats &stats)
        {
            const auto emitCountMap = [](std::ostringstream &out,
                                         std::string_view key,
                                         const ActivityScheduleSummaryStats::KindCountMap &counts)
            {
                out << ",\"" << key << "\":{";
                bool first = true;
                for (const auto &[name, count] : counts)
                {
                    if (!first)
                    {
                        out << ",";
                    }
                    first = false;
                    out << "\"" << name << "\":" << count;
                }
                out << "}";
            };
            std::ostringstream out;
            out << std::setprecision(17);
            out << "{";
            out << "\"supernodes\":" << stats.supernodes;
            out << ",\"compute_supernodes\":" << stats.computeSupernodes;
            out << ",\"commit_supernodes\":" << stats.commitSupernodes;
            out << ",\"dag_edges\":" << stats.dagEdges;
            out << ",\"boundary_values\":" << stats.boundaryValues;
            out << ",\"boundary_activation_edges\":" << stats.boundaryActivationEdges;
            out << ",\"compute_compute_value_pairs\":" << stats.computeComputeValuePairs;
            out << ",\"compute_commit_value_pairs\":" << stats.computeCommitValuePairs;
            out << ",\"state_read_activation_edges\":" << stats.stateReadActivationEdges;
            out << ",\"memory_read_activation_edges\":" << stats.memoryReadActivationEdges;
            out << ",\"constant_activation_edges\":" << stats.constantActivationEdges;
            out << ",\"other_compute_activation_edges\":" << stats.otherComputeActivationEdges;
            out << ",\"other_compute_single_target_values\":" << stats.otherComputeSingleTargetValues;
            out << ",\"other_compute_multi_target_values\":" << stats.otherComputeMultiTargetValues;
            out << ",\"other_compute_single_target_activation_edges\":"
                << stats.otherComputeSingleTargetActivationEdges;
            out << ",\"other_compute_multi_target_activation_edges\":"
                << stats.otherComputeMultiTargetActivationEdges;
            out << ",\"other_compute_unique_supernode_pairs\":" << stats.otherComputeUniqueSupernodePairs;
            out << ",\"other_compute_duplicate_activation_edges\":" << stats.otherComputeDuplicateActivationEdges;
            out << ",\"compute_nodes\":" << stats.computeNodes;
            out << ",\"compute_node_ops_total\":" << stats.computeNodeOpsTotal;
            out << ",\"compute_node_cycle_split_iters\":" << stats.computeNodeCycleSplitIters;
            out << ",\"initial_compute_supernodes\":" << stats.initialComputeSupernodes;
            out << ",\"initial_compute_supernode_ops_total\":"
                << stats.initialComputeSupernodeOpsTotal;
            out << ",\"initial_compute_supernode_dag_edges\":"
                << stats.initialComputeSupernodeDagEdges;
            out << ",\"initial_boundary_values\":" << stats.initialBoundaryValues;
            out << ",\"initial_boundary_activation_edges\":"
                << stats.initialBoundaryActivationEdges;
            out << ",\"initial_compute_compute_value_pairs\":"
                << stats.initialComputeComputeValuePairs;
            out << ",\"initial_compute_commit_value_pairs\":"
                << stats.initialComputeCommitValuePairs;
            out << ",\"source_clones_in_compute_nodes\":" << stats.sourceClonesInComputeNodes;
            out << ",\"local_shared_compute_clones_in_compute_nodes\":"
                << stats.localSharedComputeClonesInComputeNodes;
            out << ",\"direct_source_inputs_to_commit_supernodes\":"
                << stats.directSourceInputsToCommitSupernodes;
            out << ",\"common_expr_compute_nodes\":" << stats.commonExprComputeNodes;
            out << ",\"compute_node_boundary_inputs_total\":" << stats.computeNodeBoundaryInputsTotal;
            out << ",\"compute_node_boundary_input_no_def\":" << stats.computeNodeBoundaryInputNoDef;
            out << ",\"compute_node_boundary_input_def_out_of_range\":"
                << stats.computeNodeBoundaryInputDefOutOfRange;
            out << ",\"compute_node_boundary_input_declared\":" << stats.computeNodeBoundaryInputDeclared;
            out << ",\"compute_node_boundary_declared_values\":" << stats.computeNodeBoundaryDeclaredValues;
            out << ",\"compute_node_boundary_declared_edges\":" << stats.computeNodeBoundaryDeclaredEdges;
            out << ",\"compute_node_declared_cut_violations_fixed\":"
                << stats.computeNodeDeclaredCutViolationsFixed;
            out << ",\"compute_node_declared_cut_violations_fatal\":"
                << stats.computeNodeDeclaredCutViolationsFatal;
            out << ",\"compute_node_boundary_input_source_spill\":"
                << stats.computeNodeBoundaryInputSourceSpill;
            out << ",\"compute_node_boundary_input_unsupported\":"
                << stats.computeNodeBoundaryInputUnsupported;
            out << ",\"compute_node_boundary_input_existing_owner\":"
                << stats.computeNodeBoundaryInputExistingOwner;
            out << ",\"compute_node_boundary_input_existing_common_owner\":"
                << stats.computeNodeBoundaryInputExistingCommonOwner;
            out << ",\"compute_node_boundary_input_shared\":" << stats.computeNodeBoundaryInputShared;
            out << ",\"compute_node_boundary_input_capacity\":" << stats.computeNodeBoundaryInputCapacity;
            out << ",\"compute_node_boundary_values\":" << stats.computeNodeBoundaryValues;
            out << ",\"commit_input_root_values\":" << stats.commitInputRootValues;
            out << ",\"commit_sink_ops\":" << stats.commitSinkOps;
            out << ",\"commit_event_key_runs\":" << stats.commitEventKeyRuns;
            out << ",\"commit_event_keys\":" << stats.commitEventKeys;
            out << ",\"topo_edges\":" << stats.topoEdges;
            out << ",\"graph_ops\":" << stats.graphOps;
            out << ",\"graph_values\":" << stats.graphValues;
            emitCountMap(out, "activation_edges_by_source_kind", stats.activationEdgesBySourceKind);
            emitCountMap(out, "activation_source_values_by_source_kind", stats.activationSourceValuesBySourceKind);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_kind",
                         stats.computeNodeBoundaryExistingCommonOwnerByKind);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_width_bucket",
                         stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket);
            emitCountMap(out,
                         "compute_node_boundary_existing_common_owner_by_fanout_bucket",
                         stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket);
            out << "}";
            return out.str();
        }

        template <typename RewriteBuildT, typename OpDataT>
        ActivityScheduleSummaryStats buildActivityScheduleSummaryStats(const ActivityScheduleBuild &build,
                                                                       const RewriteBuildT &rewrite,
                                                                       const OpDataT &opData,
                                                                       const wolvrix::lib::grh::Graph &graph)
        {
            ActivityScheduleSummaryStats stats;
            std::unordered_set<uint64_t> otherComputeUniquePairs;
            stats.supernodes = build.supernodeToOps.size();
            stats.computeNodes = rewrite.stats.computeNodes;
            stats.computeNodeOpsTotal = rewrite.stats.computeNodeOpsTotal;
            stats.computeNodeCycleSplitIters = rewrite.stats.computeNodeCycleSplitIters;
            stats.initialComputeSupernodes = rewrite.stats.initialComputeSupernodes;
            stats.initialComputeSupernodeOpsTotal =
                rewrite.stats.initialComputeSupernodeOpsTotal;
            stats.initialComputeSupernodeDagEdges =
                rewrite.stats.initialComputeSupernodeDagEdges;
            stats.initialBoundaryValues = rewrite.stats.initialBoundaryValues;
            stats.initialBoundaryActivationEdges =
                rewrite.stats.initialBoundaryActivationEdges;
            stats.initialComputeComputeValuePairs =
                rewrite.stats.initialComputeComputeValuePairs;
            stats.initialComputeCommitValuePairs =
                rewrite.stats.initialComputeCommitValuePairs;
            stats.sourceClonesInComputeNodes = rewrite.stats.sourceClonesInComputeNodes;
            stats.localSharedComputeClonesInComputeNodes =
                rewrite.stats.localSharedComputeClonesInComputeNodes;
            stats.directSourceInputsToCommitSupernodes = rewrite.stats.directSourceInputsToCommitSupernodes;
            stats.commonExprComputeNodes = rewrite.stats.commonExprComputeNodes;
            stats.computeNodeBoundaryInputsTotal = rewrite.stats.computeNodeBoundaryInputsTotal;
            stats.computeNodeBoundaryInputNoDef = rewrite.stats.computeNodeBoundaryInputNoDef;
            stats.computeNodeBoundaryInputDefOutOfRange = rewrite.stats.computeNodeBoundaryInputDefOutOfRange;
            stats.computeNodeBoundaryInputDeclared = rewrite.stats.computeNodeBoundaryInputDeclared;
            stats.computeNodeBoundaryDeclaredValues = rewrite.stats.computeNodeBoundaryDeclaredValues;
            stats.computeNodeBoundaryDeclaredEdges = rewrite.stats.computeNodeBoundaryDeclaredEdges;
            stats.computeNodeDeclaredCutViolationsFixed = rewrite.stats.computeNodeDeclaredCutViolationsFixed;
            stats.computeNodeDeclaredCutViolationsFatal = rewrite.stats.computeNodeDeclaredCutViolationsFatal;
            stats.computeNodeBoundaryInputSourceSpill = rewrite.stats.computeNodeBoundaryInputSourceSpill;
            stats.computeNodeBoundaryInputUnsupported = rewrite.stats.computeNodeBoundaryInputUnsupported;
            stats.computeNodeBoundaryInputExistingOwner = rewrite.stats.computeNodeBoundaryInputExistingOwner;
            stats.computeNodeBoundaryInputExistingCommonOwner = rewrite.stats.computeNodeBoundaryInputExistingCommonOwner;
            stats.computeNodeBoundaryInputShared = rewrite.stats.computeNodeBoundaryInputShared;
            stats.computeNodeBoundaryInputCapacity = rewrite.stats.computeNodeBoundaryInputCapacity;
            stats.computeNodeBoundaryValues = rewrite.stats.computeNodeBoundaryValues;
            stats.commitInputRootValues = rewrite.stats.commitInputRootValues;
            stats.commitSinkOps = rewrite.stats.commitSinkOps;
            stats.commitEventKeyRuns = rewrite.stats.commitEventKeyRuns;
            stats.commitEventKeys = rewrite.stats.commitEventKeys;
            stats.computeNodeBoundaryExistingCommonOwnerByKind =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByKind;
            stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket;
            stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket =
                rewrite.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
            stats.topoEdges = opData.topoEdges.size();
            stats.graphOps = graph.operations().size();
            stats.graphValues = graph.values().size();
            for (const auto kind : build.supernodeKinds)
            {
                if (kind == ActivityScheduleSupernodeKind::Compute)
                {
                    ++stats.computeSupernodes;
                }
                else if (kind == ActivityScheduleSupernodeKind::Commit)
                {
                    ++stats.commitSupernodes;
                }
            }
            for (const auto &succs : build.dag)
            {
                stats.dagEdges += succs.size();
            }
            for (std::size_t valueIndex = 0; valueIndex < build.valueFanout.size(); ++valueIndex)
            {
                const auto &fanout = build.valueFanout[valueIndex];
                if (fanout.empty())
                {
                    continue;
                }
                ++stats.boundaryValues;
                stats.boundaryActivationEdges += fanout.size();
                const std::string sourceKindName =
                    valueIndex + 1 < build.valueSourceKind.size()
                        ? std::string(wolvrix::lib::grh::toString(build.valueSourceKind[valueIndex + 1]))
                        : std::string("unknown");
                stats.activationEdgesBySourceKind[sourceKindName] += fanout.size();
                stats.activationSourceValuesBySourceKind[sourceKindName] += 1;
                if (valueIndex + 1 < build.valueSourceKind.size())
                {
                    switch (build.valueSourceKind[valueIndex + 1])
                    {
                    case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                    case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                        stats.stateReadActivationEdges += fanout.size();
                        break;
                    case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                        stats.memoryReadActivationEdges += fanout.size();
                        break;
                    case wolvrix::lib::grh::OperationKind::kConstant:
                        stats.constantActivationEdges += fanout.size();
                        break;
                    default:
                        stats.otherComputeActivationEdges += fanout.size();
                        if (fanout.size() <= 1)
                        {
                            ++stats.otherComputeSingleTargetValues;
                            stats.otherComputeSingleTargetActivationEdges += fanout.size();
                        }
                        else
                        {
                            ++stats.otherComputeMultiTargetValues;
                            stats.otherComputeMultiTargetActivationEdges += fanout.size();
                        }
                        break;
                    }
                }
                else
                {
                    stats.otherComputeActivationEdges += fanout.size();
                    if (fanout.size() <= 1)
                    {
                        ++stats.otherComputeSingleTargetValues;
                        stats.otherComputeSingleTargetActivationEdges += fanout.size();
                    }
                    else
                    {
                        ++stats.otherComputeMultiTargetValues;
                        stats.otherComputeMultiTargetActivationEdges += fanout.size();
                    }
                }
                const bool isOtherCompute =
                    valueIndex + 1 >= build.valueSourceKind.size() ||
                    (build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kRegisterReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kLatchReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kMemoryReadPort &&
                     build.valueSourceKind[valueIndex + 1] != wolvrix::lib::grh::OperationKind::kConstant);
                const uint32_t sourceSupernode =
                    valueIndex + 1 < build.valueSourceSupernode.size() ? build.valueSourceSupernode[valueIndex + 1]
                                                                       : kInvalidActivitySupernodeId;
                for (const auto targetSupernode : fanout)
                {
                    if (targetSupernode >= build.supernodeKinds.size())
                    {
                        continue;
                    }
                    if (isOtherCompute && sourceSupernode != kInvalidActivitySupernodeId)
                    {
                        const uint64_t packed =
                            (static_cast<uint64_t>(sourceSupernode) << 32) | targetSupernode;
                        otherComputeUniquePairs.insert(packed);
                    }
                    if (build.supernodeKinds[targetSupernode] == ActivityScheduleSupernodeKind::Compute)
                    {
                        ++stats.computeComputeValuePairs;
                    }
                    else if (build.supernodeKinds[targetSupernode] == ActivityScheduleSupernodeKind::Commit)
                    {
                        ++stats.computeCommitValuePairs;
                    }
                }
            }
            stats.otherComputeUniqueSupernodePairs = otherComputeUniquePairs.size();
            if (stats.otherComputeActivationEdges >= stats.otherComputeUniqueSupernodePairs)
            {
                stats.otherComputeDuplicateActivationEdges =
                    stats.otherComputeActivationEdges - stats.otherComputeUniqueSupernodePairs;
            }
            return stats;
        }
        struct ActivityOpData
        {
            std::vector<wolvrix::lib::grh::OperationId> topoOps;
            std::vector<wolvrix::lib::grh::SymbolId> topoSymbols;
            std::vector<wolvrix::lib::grh::OperationKind> topoKinds;
            std::vector<uint8_t> topoSinkOnly;
            std::vector<std::pair<uint32_t, uint32_t>> topoEdges;
            std::vector<uint32_t> topoPosByOpIndex;
            std::size_t maxOpIndex = 0;
        };

        bool topoLessOp(const ActivityOpData &opData,
                        wolvrix::lib::grh::OperationId lhs,
                        wolvrix::lib::grh::OperationId rhs);

        struct SinkPartition
        {
            std::vector<std::vector<uint32_t>> clusters;
        };

        struct ComputeNodeMaterializePerfStats
        {
            struct CoarsenIteration
            {
                std::size_t iteration = 0;
                std::size_t clusters = 0;
                std::size_t clusterDelta = 0;
                bool changed = false;
                bool out1Changed = false;
                bool in1Changed = false;
                bool siblingsChanged = false;
                bool tailStopped = false;
                std::uint64_t elapsedMs = 0;
            };

            struct PostDpSwapProbe
            {
                bool evaluated = false;
                std::size_t capacityBlockedSeeds = 0;
                std::size_t enumeratedRhs = 0;
                std::size_t rejectedLocked = 0;
                std::size_t rejectedEqualLoad = 0;
                std::size_t rejectedTopo = 0;
                std::size_t rejectedDagSupport = 0;
                std::size_t rejectedDagEdgeCount = 0;
                std::size_t rejectedDagSupportKey = 0;
                std::size_t rejectedNonpositiveBae = 0;
                std::size_t rejectedScanLimit = 0;
                std::size_t rawEligible = 0;
                std::size_t eligibleBaeGain = 0;
                std::map<std::size_t, std::size_t> eligibleByClusterOps;
                std::size_t selected = 0;
                std::size_t selectedMovedClusters = 0;
                std::size_t selectedMovedOps = 0;
                std::size_t rejectedConflict = 0;
                std::size_t rejectedBudget = 0;
                std::size_t projectedBaeGain = 0;
                std::size_t actualBaeGain = 0;
                std::size_t computeBaeCandidate = 0;
                std::size_t boundaryValuesBefore = 0;
                std::size_t boundaryValuesCandidate = 0;
                std::size_t dagEdgesCandidate = 0;
                bool segmentCountValid = false;
                bool segmentOpsValid = false;
                bool segmentShapeValid = false;
                bool dagSupportValid = false;
                bool topoValid = false;
                bool projectedActualValid = false;
                bool valid = false;
            };

            std::uint64_t initClustersMs = 0;
            std::uint64_t topoBeforeCoarsenMs = 0;
            std::uint64_t coarsenMs = 0;
            std::uint64_t topoAfterCoarsenMs = 0;
            std::uint64_t buildClusterViewMs = 0;
            std::uint64_t dpSegmentMs = 0;
            std::uint64_t kahnLevelPackMs = 0;
            std::uint64_t postDpRefineMs = 0;
            std::uint64_t flattenSegmentsMs = 0;
            std::uint64_t buildFinalSupernodesMs = 0;
            std::uint64_t buildFinalDagMs = 0;
            std::uint64_t buildStateReadSetsMs = 0;
            std::uint64_t finalTopoMs = 0;
            std::size_t clustersBeforeCoarsen = 0;
            std::size_t clustersAfterCoarsen = 0;
            std::size_t coarsenIterations = 0;
            std::size_t coarsenOut1Merges = 0;
            std::size_t coarsenIn1Merges = 0;
            std::size_t coarsenSiblingMerges = 0;
            std::size_t segments = 0;
            std::size_t computeSupernodes = 0;
            std::size_t splitOversizeComputeNodes = 0;
            std::size_t splitOversizeComputeNodeSupernodes = 0;
            std::size_t kahnLevelPackLevels = 0;
            std::size_t kahnLevelPackLockedClusters = 0;
            std::size_t kahnLevelPackSharedValues = 0;
            std::size_t kahnLevelPackAffinityPairs = 0;
            std::size_t kahnLevelPackCandidates = 0;
            std::size_t kahnLevelPackSwaps = 0;
            std::size_t kahnLevelPackMovedClusters = 0;
            std::size_t kahnLevelPackMovedOps = 0;
            std::size_t kahnLevelPackRejectedBudget = 0;
            std::size_t kahnLevelPackRejectedLocked = 0;
            std::size_t kahnLevelPackRejectedTopo = 0;
            std::size_t kahnLevelPackRejectedSegmentCount = 0;
            std::size_t kahnLevelPackRejectedSegmentShape = 0;
            std::size_t kahnLevelPackRejectedPolicy = 0;
            std::size_t kahnLevelPackBaselineSegments = 0;
            std::size_t kahnLevelPackCandidateSegments = 0;
            std::size_t kahnLevelPackBeforeComputeBae = 0;
            std::size_t kahnLevelPackCandidateComputeBae = 0;
            std::size_t kahnLevelPackAfterComputeBae = 0;
            std::size_t kahnLevelPackBeforeDagEdges = 0;
            std::size_t kahnLevelPackCandidateDagEdges = 0;
            std::size_t kahnLevelPackAfterDagEdges = 0;
            bool kahnLevelPackEvaluated = false;
            bool kahnLevelPackCandidateBuilt = false;
            bool kahnLevelPackAdopted = false;
            bool kahnLevelPackSkippedOversize = false;
            bool kahnLevelPackSkippedSplitSensitive = false;
            bool kahnLevelPackFinalTopoValid = false;
            std::size_t postDpRefineRounds = 0;
            std::size_t postDpRefineCandidates = 0;
            std::size_t postDpRefineMoves = 0;
            std::size_t postDpRefineSwaps = 0;
            std::size_t postDpRefineMovedOps = 0;
            std::size_t postDpRefineRejectedCapacity = 0;
            std::size_t postDpRefineRejectedTopo = 0;
            std::size_t postDpRefineRejectedPolicy = 0;
            std::size_t postDpRefineLockedClusters = 0;
            std::size_t postDpRefineBeforeComputeBae = 0;
            std::size_t postDpRefineAfterComputeBae = 0;
            std::size_t postDpRefineBeforeDagEdges = 0;
            std::size_t postDpRefineAfterDagEdges = 0;
            bool postDpRefineEvaluated = false;
            PostDpSwapProbe postDpSwapProbe;
            bool coarsenTailStopped = false;
            std::size_t coarsenTailIterations = 0;
            std::vector<CoarsenIteration> coarsenIterationStats;
        };

        std::vector<uint32_t> findCyclePath(const std::vector<std::vector<uint32_t>> &dag)
        {
            std::vector<uint8_t> color(dag.size(), 0U);
            std::vector<uint32_t> stack;
            std::vector<uint32_t> stackIndex(dag.size(), kInvalidActivitySupernodeId);
            std::vector<uint32_t> cycle;

            const auto dfs = [&](auto &&self, uint32_t node) -> bool
            {
                color[node] = 1U;
                stackIndex[node] = static_cast<uint32_t>(stack.size());
                stack.push_back(node);
                for (const auto succ : dag[node])
                {
                    if (succ >= dag.size())
                    {
                        continue;
                    }
                    if (color[succ] == 0U)
                    {
                        if (self(self, succ))
                        {
                            return true;
                        }
                        continue;
                    }
                    if (color[succ] == 1U)
                    {
                        const uint32_t begin = stackIndex[succ];
                        cycle.assign(stack.begin() + begin, stack.end());
                        cycle.push_back(succ);
                        return true;
                    }
                }
                stackIndex[node] = kInvalidActivitySupernodeId;
                stack.pop_back();
                color[node] = 2U;
                return false;
            };

            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                if (color[node] == 0U && dfs(dfs, node))
                {
                    break;
                }
            }
            return cycle;
        }


        constexpr std::size_t kComputeNodeCoarsenTailLargeClusterThreshold = 100000;
        constexpr std::size_t kComputeNodeCoarsenTailMaxClusterDeltaExclusive = 1024;
        constexpr std::size_t kComputeNodeCoarsenTailMaxConsecutiveIters = 3;

        std::uint64_t elapsedMs(const std::chrono::steady_clock::time_point &start) noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }

        struct DisjointSet
        {
            explicit DisjointSet(std::size_t count)
                : parent(count), size(count, 1)
            {
                std::iota(parent.begin(), parent.end(), 0);
            }

            uint32_t find(uint32_t node)
            {
                uint32_t root = node;
                while (parent[root] != root)
                {
                    root = parent[root];
                }
                while (parent[node] != node)
                {
                    const uint32_t next = parent[node];
                    parent[node] = root;
                    node = next;
                }
                return root;
            }

            bool unite(uint32_t lhs, uint32_t rhs)
            {
                lhs = find(lhs);
                rhs = find(rhs);
                if (lhs == rhs)
                {
                    return false;
                }
                if (size[lhs] < size[rhs])
                {
                    std::swap(lhs, rhs);
                }
                parent[rhs] = lhs;
                size[lhs] += size[rhs];
                return true;
            }

            std::vector<uint32_t> parent;
            std::vector<uint32_t> size;
        };

        ActivityOpData buildActivityOpData(const wolvrix::lib::grh::Graph &graph,
                                           std::string &error)
        {
            ActivityOpData data;

            for (const auto opId : graph.operations())
            {
                data.maxOpIndex = std::max<std::size_t>(data.maxOpIndex, opId.index);
            }

            std::vector<wolvrix::lib::grh::OperationId> eligibleOps;
            eligibleOps.reserve(graph.operations().size());
            for (const auto opId : graph.operations())
            {
                if (isPartitionableOpKind(graph.opKind(opId)))
                {
                    eligibleOps.push_back(opId);
                }
            }

            data.topoPosByOpIndex.assign(data.maxOpIndex + 1, kInvalidActivitySupernodeId);
            if (eligibleOps.empty())
            {
                return data;
            }

            std::vector<uint8_t> eligibleByOpIndex(data.maxOpIndex + 1, 0);
            for (const auto opId : eligibleOps)
            {
                eligibleByOpIndex[opId.index] = 1;
            }

            wolvrix::lib::toposort::TopoDag<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> topoDag;
            topoDag.reserveNodes(eligibleOps.size());
            for (const auto opId : eligibleOps)
            {
                topoDag.addNode(opId);
            }

            std::vector<std::pair<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationId>> opEdges;
            opEdges.reserve(eligibleOps.size() * 2);
            for (const auto opId : eligibleOps)
            {
                for (const auto operand : graph.opOperands(opId))
                {
                    const auto defOp = graph.valueDef(operand);
                    if (!defOp.valid())
                    {
                        continue;
                    }
                    if (defOp.index >= eligibleByOpIndex.size() || eligibleByOpIndex[defOp.index] == 0)
                    {
                        continue;
                    }
                    topoDag.addEdge(defOp, opId);
                    opEdges.emplace_back(defOp, opId);
                }
            }

            try
            {
                const auto layers = topoDag.toposort();
                for (const auto &layer : layers)
                {
                    data.topoOps.insert(data.topoOps.end(), layer.begin(), layer.end());
                }
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule topo failed: ") + ex.what();
                return data;
            }

            if (data.topoOps.size() != eligibleOps.size())
            {
                error = "activity-schedule topo failed: combinational dependency cycle detected";
                data.topoOps.clear();
                return data;
            }

            data.topoSymbols.reserve(data.topoOps.size());
            data.topoKinds.reserve(data.topoOps.size());
            data.topoSinkOnly.reserve(data.topoOps.size());
            for (std::size_t pos = 0; pos < data.topoOps.size(); ++pos)
            {
                const auto opId = data.topoOps[pos];
                data.topoPosByOpIndex[opId.index] = static_cast<uint32_t>(pos);
                data.topoSymbols.push_back(graph.operationSymbol(opId));
                data.topoKinds.push_back(graph.opKind(opId));
                data.topoSinkOnly.push_back(isSinkPartitionOp(graph.getOperation(opId)) ? 1U : 0U);
            }

            data.topoEdges.reserve(opEdges.size());
            for (const auto &[srcOp, dstOp] : opEdges)
            {
                const uint32_t srcPos = data.topoPosByOpIndex[srcOp.index];
                const uint32_t dstPos = data.topoPosByOpIndex[dstOp.index];
                if (srcPos == kInvalidActivitySupernodeId || dstPos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                data.topoEdges.emplace_back(srcPos, dstPos);
            }
            std::sort(data.topoEdges.begin(), data.topoEdges.end());
            data.topoEdges.erase(std::unique(data.topoEdges.begin(), data.topoEdges.end()),
                                 data.topoEdges.end());

            return data;
        }

        std::string normalizedSinkEventKey(const wolvrix::lib::grh::Graph &graph,
                                           const wolvrix::lib::grh::Operation &op,
                                           const ValueCanonicalMap *canonicalValues)
        {
            const auto edges =
                getAttrValue<std::vector<std::string>>(op, "eventEdge").value_or(std::vector<std::string>{});
            if (edges.empty())
            {
                return "none";
            }

            const auto operands = op.operands();
            std::size_t eventStart = operands.size();
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                eventStart = 3;
                break;
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                eventStart = 4;
                break;
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
                eventStart = operands.size() >= edges.size() ? operands.size() - edges.size() : operands.size();
                break;
            default:
                return "opaque";
            }

            const std::size_t safeStart = std::min(eventStart, operands.size());
            const std::size_t eventCount = std::min(edges.size(), operands.size() - safeStart);
            if (eventCount == 0)
            {
                return "none";
            }

            std::vector<std::string> parts;
            parts.reserve(eventCount);
            for (std::size_t i = 0; i < eventCount; ++i)
            {
                std::string edge = edges[i];
                if (edge.empty())
                {
                    edge = "any";
                }
                const auto canonicalValue = canonicalActivityValue(operands[safeStart + i], canonicalValues);
                parts.push_back(edge + ":" + std::to_string(canonicalValue.index));
            }
            std::sort(parts.begin(), parts.end());
            parts.erase(std::unique(parts.begin(), parts.end()), parts.end());

            std::ostringstream key;
            key << "ev";
            for (const auto &part : parts)
            {
                key << "|" << part;
            }
            return key.str();
        }

        struct MemoryWritePriority
        {
            std::string group;
            int64_t priority = 0;
        };

        std::optional<MemoryWritePriority> memoryWritePriority(
            const wolvrix::lib::grh::Operation &op)
        {
            const auto group = getAttrString(op, wolvrix::lib::grh::kMemoryWritePriorityGroupAttr);
            const auto priority = getAttrValue<int64_t>(op, wolvrix::lib::grh::kMemoryWritePriorityAttr);
            if (!group || !priority)
            {
                return std::nullopt;
            }
            return MemoryWritePriority{.group = *group, .priority = *priority};
        }

        bool validateMemoryWritePriorityGroups(const wolvrix::lib::grh::Graph &graph,
                                               std::string &error)
        {
            struct GroupInfo
            {
                std::string memSymbol;
                std::string eventKey;
                std::set<int64_t> priorities;
            };
            std::unordered_map<std::string, GroupInfo> groups;
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                const auto group = getAttrString(op, wolvrix::lib::grh::kMemoryWritePriorityGroupAttr);
                const auto priority = getAttrValue<int64_t>(op, wolvrix::lib::grh::kMemoryWritePriorityAttr);
                if (!group && !priority)
                {
                    continue;
                }
                if (op.kind() != wolvrix::lib::grh::OperationKind::kMemoryWritePort ||
                    !group || group->empty() || !priority || *priority < 0)
                {
                    error = "activity-schedule invalid ordered memory write attrs: " + describeOp(graph, opId);
                    return false;
                }
                const auto memSymbol = getAttrString(op, "memSymbol");
                if (!memSymbol || memSymbol->empty())
                {
                    error = "activity-schedule ordered memory write missing memSymbol: " + describeOp(graph, opId);
                    return false;
                }
                const std::string eventKey = normalizedSinkEventKey(graph, op, nullptr);
                auto [it, inserted] = groups.try_emplace(*group);
                if (inserted)
                {
                    it->second.memSymbol = *memSymbol;
                    it->second.eventKey = eventKey;
                }
                else if (it->second.memSymbol != *memSymbol || it->second.eventKey != eventKey)
                {
                    error = "activity-schedule ordered memory write group crosses memory or event: " + *group;
                    return false;
                }
                if (!it->second.priorities.insert(*priority).second)
                {
                    error = "activity-schedule duplicate ordered memory write priority: " + *group;
                    return false;
                }
            }
            for (const auto &[group, info] : groups)
            {
                int64_t expected = 0;
                for (const int64_t priority : info.priorities)
                {
                    if (priority != expected++)
                    {
                        error = "activity-schedule non-contiguous ordered memory write priorities: " + group;
                        return false;
                    }
                }
            }
            return true;
        }

        void orderMemoryWritePriorityGroups(const wolvrix::lib::grh::Graph &graph,
                                            const ActivityOpData &opData,
                                            std::vector<uint32_t> &positions)
        {
            struct Entry
            {
                std::size_t slot = 0;
                uint32_t topoPosition = 0;
                int64_t priority = 0;
            };
            std::unordered_map<std::string, std::vector<Entry>> entriesByGroup;
            for (std::size_t slot = 0; slot < positions.size(); ++slot)
            {
                const uint32_t topoPosition = positions[slot];
                const auto op = graph.getOperation(opData.topoOps[topoPosition]);
                const auto ordered = memoryWritePriority(op);
                if (!ordered)
                {
                    continue;
                }
                entriesByGroup[ordered->group].push_back(Entry{
                    .slot = slot,
                    .topoPosition = topoPosition,
                    .priority = ordered->priority,
                });
            }
            for (auto &[group, entries] : entriesByGroup)
            {
                (void)group;
                std::vector<std::size_t> slots;
                slots.reserve(entries.size());
                for (const Entry &entry : entries)
                {
                    slots.push_back(entry.slot);
                }
                std::sort(entries.begin(), entries.end(), [](const Entry &lhs, const Entry &rhs) {
                    if (lhs.priority != rhs.priority)
                    {
                        return lhs.priority > rhs.priority;
                    }
                    return lhs.topoPosition < rhs.topoPosition;
                });
                for (std::size_t index = 0; index < entries.size(); ++index)
                {
                    positions[slots[index]] = entries[index].topoPosition;
                }
            }
        }

        std::vector<std::vector<uint32_t>> buildAtomicSinkUnits(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityOpData &opData,
            const std::vector<uint32_t> &positions)
        {
            std::unordered_map<std::string, std::vector<uint32_t>> orderedGroups;
            for (const uint32_t topoPosition : positions)
            {
                const auto op = graph.getOperation(opData.topoOps[topoPosition]);
                if (const auto ordered = memoryWritePriority(op))
                {
                    orderedGroups[ordered->group].push_back(topoPosition);
                }
            }
            for (auto &[group, groupPositions] : orderedGroups)
            {
                (void)group;
                orderMemoryWritePriorityGroups(graph, opData, groupPositions);
            }

            std::vector<std::vector<uint32_t>> units;
            units.reserve(positions.size());
            std::unordered_set<std::string> emittedGroups;
            for (const uint32_t topoPosition : positions)
            {
                const auto op = graph.getOperation(opData.topoOps[topoPosition]);
                const auto ordered = memoryWritePriority(op);
                if (!ordered)
                {
                    units.push_back(std::vector<uint32_t>{topoPosition});
                    continue;
                }
                if (!emittedGroups.insert(ordered->group).second)
                {
                    continue;
                }
                units.push_back(orderedGroups.at(ordered->group));
            }
            return units;
        }

        std::optional<wolvrix::lib::grh::ValueId> sinkUpdateCondValue(const wolvrix::lib::grh::Operation &op)
        {
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                break;
            default:
                return std::nullopt;
            }

            const auto operands = op.operands();
            if (operands.empty())
            {
                return std::nullopt;
            }
            return operands[0];
        }

        std::string normalizedSinkEventGuardKey(const wolvrix::lib::grh::Graph &graph,
                                                const wolvrix::lib::grh::Operation &op,
                                                wolvrix::lib::grh::OperationId opId,
                                                const ValueCanonicalMap *canonicalValues)
        {
            std::ostringstream key;
            key << normalizedSinkEventKey(graph, op, canonicalValues);
            if (const auto ordered = memoryWritePriority(op))
            {
                key << "|ordered:" << ordered->group.size() << ':' << ordered->group;
                return key.str();
            }
            const auto updateCond = sinkUpdateCondValue(op);
            if (!updateCond)
            {
                key << "|guard:opaque:" << static_cast<int>(op.kind());
                if (!op.symbolText().empty())
                {
                    key << ":" << op.symbolText();
                }
                key << ":" << opId.index;
                return key.str();
            }

            const auto canonicalCond = canonicalActivityValue(*updateCond, canonicalValues);
            key << "|guard:" << canonicalCond.index;
            return key.str();
        }

        SinkPartition buildEventClusteredSinkPartition(const wolvrix::lib::grh::Graph &graph,
                                                       const ActivityOpData &opData,
                                                       const std::vector<uint32_t> &topoPositions,
                                                       std::size_t maxSize,
                                                       const ValueCanonicalMap *canonicalValues,
                                                       bool groupByGuard)
        {
            SinkPartition partition;
            if (topoPositions.empty())
            {
                return partition;
            }

            const std::size_t chunkSize = maxSize == 0 ? topoPositions.size() : maxSize;

            if (groupByGuard)
            {
                constexpr std::size_t kMaxGuardEventMergeOps = 4096;
                const std::size_t baselineMergeLimit =
                    maxSize == 0 ? kMaxGuardEventMergeOps : std::min(maxSize, kMaxGuardEventMergeOps);

                struct EventGuardBuckets
                {
                    std::vector<std::string> guardOrder;
                    std::unordered_map<std::string, std::vector<uint32_t>> positionsByGuard;
                };

                std::vector<std::string> eventOrder;
                std::unordered_map<std::string, EventGuardBuckets> bucketsByEvent;
                eventOrder.reserve(topoPositions.size());
                bucketsByEvent.reserve(topoPositions.size());
                for (const uint32_t topoPos : topoPositions)
                {
                    const auto opId = opData.topoOps[topoPos];
                    const auto op = graph.getOperation(opId);
                    const std::string eventKey = normalizedSinkEventKey(graph, op, canonicalValues);
                    const std::string guardKey = normalizedSinkEventGuardKey(graph, op, opId, canonicalValues);

                    auto [eventIt, eventInserted] = bucketsByEvent.try_emplace(eventKey);
                    if (eventInserted)
                    {
                        eventOrder.push_back(eventKey);
                        eventIt->second.positionsByGuard.reserve(4);
                    }

                    auto &eventBuckets = eventIt->second;
                    auto [guardIt, guardInserted] = eventBuckets.positionsByGuard.try_emplace(guardKey);
                    if (guardInserted)
                    {
                        eventBuckets.guardOrder.push_back(guardKey);
                    }
                    guardIt->second.push_back(topoPos);
                }

                partition.clusters.reserve(eventOrder.size());
                for (const auto &eventKey : eventOrder)
                {
                    const auto eventIt = bucketsByEvent.find(eventKey);
                    if (eventIt == bucketsByEvent.end())
                    {
                        continue;
                    }
                    const auto &eventBuckets = eventIt->second;
                    std::vector<std::vector<uint32_t>> baselineClusters;
                    std::vector<uint32_t> positions;
                    auto flushPositions = [&]()
                    {
                        if (positions.empty())
                        {
                            return;
                        }
                        baselineClusters.push_back(std::move(positions));
                        positions = {};
                    };
                    for (const auto &guardKey : eventBuckets.guardOrder)
                    {
                        const auto guardIt = eventBuckets.positionsByGuard.find(guardKey);
                        if (guardIt == eventBuckets.positionsByGuard.end())
                        {
                            continue;
                        }
                        std::vector<uint32_t> guardPositions = guardIt->second;
                        if (guardPositions.empty())
                        {
                            continue;
                        }
                        orderMemoryWritePriorityGroups(graph, opData, guardPositions);
                        if (guardPositions.size() > baselineMergeLimit)
                        {
                            flushPositions();
                            baselineClusters.emplace_back(guardPositions.begin(), guardPositions.end());
                            continue;
                        }
                        if (!positions.empty() &&
                            positions.size() + guardPositions.size() > baselineMergeLimit)
                        {
                            flushPositions();
                        }
                        positions.insert(positions.end(), guardPositions.begin(), guardPositions.end());
                    }
                    flushPositions();

                    if (maxSize == 0 || maxSize <= kMaxGuardEventMergeOps)
                    {
                        for (auto &cluster : baselineClusters)
                        {
                            partition.clusters.push_back(std::move(cluster));
                        }
                        continue;
                    }

                    std::vector<uint32_t> mergedPositions;
                    auto flushMergedPositions = [&]()
                    {
                        if (mergedPositions.empty())
                        {
                            return;
                        }
                        partition.clusters.push_back(std::move(mergedPositions));
                        mergedPositions = {};
                    };
                    for (auto &cluster : baselineClusters)
                    {
                        if (cluster.size() > maxSize)
                        {
                            flushMergedPositions();
                            partition.clusters.push_back(std::move(cluster));
                            continue;
                        }
                        if (!mergedPositions.empty() &&
                            mergedPositions.size() + cluster.size() > maxSize)
                        {
                            flushMergedPositions();
                        }
                        mergedPositions.insert(mergedPositions.end(), cluster.begin(), cluster.end());
                    }
                    flushMergedPositions();
                }

                return partition;
            }

            const std::size_t clusterReserve = (topoPositions.size() + chunkSize - 1) / chunkSize;
            partition.clusters.reserve(clusterReserve);

            std::vector<std::string> keyOrder;
            std::unordered_map<std::string, std::vector<uint32_t>> positionsByKey;
            keyOrder.reserve(topoPositions.size());
            positionsByKey.reserve(topoPositions.size());
            for (const uint32_t topoPos : topoPositions)
            {
                const auto opId = opData.topoOps[topoPos];
                const auto op = graph.getOperation(opId);
                const std::string key = normalizedSinkEventKey(graph, op, canonicalValues);
                auto [it, inserted] = positionsByKey.try_emplace(key);
                if (inserted)
                {
                    keyOrder.push_back(key);
                }
                it->second.push_back(topoPos);
            }

            for (const auto &key : keyOrder)
            {
                const auto it = positionsByKey.find(key);
                if (it == positionsByKey.end())
                {
                    continue;
                }
                std::vector<uint32_t> cluster;
                auto flushCluster = [&]()
                {
                    if (cluster.empty())
                    {
                        return;
                    }
                    partition.clusters.push_back(std::move(cluster));
                    cluster = {};
                };
                for (auto &unit : buildAtomicSinkUnits(graph, opData, it->second))
                {
                    if (unit.size() > chunkSize)
                    {
                        flushCluster();
                        partition.clusters.push_back(std::move(unit));
                        continue;
                    }
                    if (!cluster.empty() && cluster.size() + unit.size() > chunkSize)
                    {
                        flushCluster();
                    }
                    cluster.insert(cluster.end(), unit.begin(), unit.end());
                }
                flushCluster();
            }

            return partition;
        }

        struct ComputeNodeRewriteStats
        {
            using KindCountMap = ActivityScheduleSummaryStats::KindCountMap;

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
            KindCountMap computeNodeBoundaryExistingCommonOwnerByKind;
            KindCountMap computeNodeBoundaryExistingCommonOwnerByWidthBucket;
            KindCountMap computeNodeBoundaryExistingCommonOwnerByFanoutBucket;
        };

        struct ComputeNode
        {
            std::vector<wolvrix::lib::grh::OperationId> ops;
            std::vector<wolvrix::lib::grh::ValueId> boundaryInputs;
            bool commonExpr = false;
            bool indivisible = false;
            std::string intentGroup;
        };

        struct CommitNode
        {
            std::vector<wolvrix::lib::grh::OperationId> ops;
            std::vector<wolvrix::lib::grh::ValueId> inputValues;
        };

        struct ComputeRewriteBuild
        {
            std::vector<ComputeNode> computeNodes;
            std::vector<CommitNode> commitNodes;
            std::vector<std::vector<uint32_t>> computeDag;
            std::vector<uint32_t> computeTopoOrder;
            std::vector<uint32_t> computeNodeOfOp;
            ValueCanonicalMap canonicalValues;
            bool declaredValueComputeNodeBoundary = false;
            ComputeNodeRewriteStats stats;
        };

        const char *activitySupernodeKindName(ActivityScheduleSupernodeKind kind) noexcept
        {
            switch (kind)
            {
            case ActivityScheduleSupernodeKind::Compute:
                return "compute";
            case ActivityScheduleSupernodeKind::Commit:
                return "commit";
            }
            return "unknown";
        }

        void appendFinalSupernodeSummary(std::ostringstream &oss,
                                         const wolvrix::lib::grh::Graph &graph,
                                         const ComputeRewriteBuild &rewrite,
                                         const ActivityScheduleBuild &build,
                                         const std::vector<std::vector<uint32_t>> &computeNodesBySupernode,
                                         uint32_t supernodeId)
        {
            oss << "supernode=" << supernodeId;
            if (supernodeId < build.supernodeKinds.size())
            {
                oss << "(" << activitySupernodeKindName(build.supernodeKinds[supernodeId]) << ")";
            }
            if (supernodeId < computeNodesBySupernode.size() && !computeNodesBySupernode[supernodeId].empty())
            {
                oss << " computeNodes=[";
                const auto &nodes = computeNodesBySupernode[supernodeId];
                const std::size_t limit = std::min<std::size_t>(nodes.size(), 8);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    const uint32_t computeNodeId = nodes[i];
                    oss << computeNodeId;
                    if (computeNodeId < rewrite.computeNodes.size())
                    {
                        oss << ":ops=" << rewrite.computeNodes[computeNodeId].ops.size();
                if (rewrite.computeNodes[computeNodeId].commonExpr)
                {
                    oss << ":common";
                }
                if (rewrite.computeNodes[computeNodeId].indivisible)
                {
                    oss << ":indivisible";
                }
            }
                }
                if (nodes.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
            }
            if (supernodeId < build.supernodeToOps.size())
            {
                const auto &ops = build.supernodeToOps[supernodeId];
                oss << " ops=[";
                const std::size_t limit = std::min<std::size_t>(ops.size(), 6);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    oss << describeOp(graph, ops[i]);
                }
                if (ops.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
            }
        }

        void appendFinalEdgeReasons(std::ostringstream &oss,
                                    const wolvrix::lib::grh::Graph &graph,
                                    const ComputeRewriteBuild &rewrite,
                                    const ActivityScheduleBuild &build,
                                    const std::vector<uint32_t> &supernodeOfOp,
                                    uint32_t from,
                                    uint32_t to)
        {
            oss << " edge " << from << " -> " << to << " via";
            if (to >= build.supernodeToOps.size())
            {
                oss << " <invalid-target>";
                return;
            }
            std::size_t printed = 0;
            for (const auto toOpId : build.supernodeToOps[to])
            {
                const auto operands = graph.opOperands(toOpId);
                for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
                {
                    const auto operand = operands[operandIndex];
                    const auto defOp = graph.valueDef(operand);
                    if (!defOp.valid() || defOp.index >= supernodeOfOp.size() ||
                        supernodeOfOp[defOp.index] != from)
                    {
                        continue;
                    }
                    if (printed == 0)
                    {
                        oss << " ";
                    }
                    else
                    {
                        oss << "; ";
                    }
                    oss << describeValue(graph, operand)
                        << " def=" << describeOp(graph, defOp);
                    if (defOp.index < rewrite.computeNodeOfOp.size() &&
                        rewrite.computeNodeOfOp[defOp.index] != kInvalidActivitySupernodeId)
                    {
                        oss << "(computeNode=" << rewrite.computeNodeOfOp[defOp.index] << ")";
                    }
                    oss << " use=" << describeOp(graph, toOpId)
                        << "(operand=" << operandIndex;
                    if (toOpId.index < rewrite.computeNodeOfOp.size() &&
                        rewrite.computeNodeOfOp[toOpId.index] != kInvalidActivitySupernodeId)
                    {
                        oss << ",computeNode=" << rewrite.computeNodeOfOp[toOpId.index];
                    }
                    oss << ")";
                    ++printed;
                    if (printed >= 6)
                    {
                        oss << "; ...";
                        return;
                    }
                }
            }
            if (printed == 0)
            {
                oss << " <no matching operand found>";
            }
        }

        std::string describeFinalScheduleCycle(const wolvrix::lib::grh::Graph &graph,
                                               const ComputeRewriteBuild &rewrite,
                                               const ActivityScheduleBuild &build,
                                               const std::vector<std::vector<uint32_t>> &computeNodesBySupernode,
                                               const std::vector<uint32_t> &supernodeOfOp)
        {
            const std::vector<uint32_t> cycle = findCyclePath(build.dag);
            if (cycle.empty())
            {
                return "cycle=<unavailable>";
            }

            std::ostringstream oss;
            oss << "cycle_path=";
            const std::size_t nodeLimit = std::min<std::size_t>(cycle.size(), 16);
            for (std::size_t i = 0; i < nodeLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " -> ";
                }
                oss << cycle[i];
            }
            if (cycle.size() > nodeLimit)
            {
                oss << " -> ...";
            }
            oss << " cycle_nodes={";
            const std::size_t summaryLimit = std::min<std::size_t>(cycle.size(), 12);
            for (std::size_t i = 0; i < summaryLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " | ";
                }
                appendFinalSupernodeSummary(oss, graph, rewrite, build, computeNodesBySupernode, cycle[i]);
            }
            if (cycle.size() > summaryLimit)
            {
                oss << " | ...";
            }
            oss << "} cycle_edges={";
            const std::size_t edgeLimit = std::min<std::size_t>(cycle.size() - 1, 12);
            for (std::size_t i = 0; i < edgeLimit; ++i)
            {
                if (i != 0)
                {
                    oss << " | ";
                }
                appendFinalEdgeReasons(oss, graph, rewrite, build, supernodeOfOp, cycle[i], cycle[i + 1]);
            }
            if (cycle.size() - 1 > edgeLimit)
            {
                oss << " | ...";
            }
            oss << "}";
            return oss.str();
        }

        ActivityOpClass classifyActivityOp(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return ActivityOpClass::Source;
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                return ActivityOpClass::Sink;
            default:
                break;
            }
            if (isStorageDeclOpKind(kind) || isHierLikeOpKind(kind))
            {
                return ActivityOpClass::Declaration;
            }
            return ActivityOpClass::Compute;
        }

        std::vector<ActivityOpClass> buildOpClasses(const wolvrix::lib::grh::Graph &graph,
                                                    std::size_t maxOpIndex)
        {
            std::vector<ActivityOpClass> out(maxOpIndex + 1, ActivityOpClass::Unsupported);
            for (const auto opId : graph.operations())
            {
                if (opId.index < out.size())
                {
                    out[opId.index] = classifyActivityOp(graph.opKind(opId));
                }
            }
            return out;
        }

        bool vectorContainsValue(const std::vector<wolvrix::lib::grh::ValueId> &values,
                                 wolvrix::lib::grh::ValueId value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        std::vector<wolvrix::lib::grh::OperationId>
        uniqueOpsPreservingOrder(const std::vector<wolvrix::lib::grh::OperationId> &ops)
        {
            std::vector<wolvrix::lib::grh::OperationId> out;
            out.reserve(ops.size());
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                seen;
            seen.reserve(ops.size());
            for (const auto opId : ops)
            {
                if (seen.insert(opId).second)
                {
                    out.push_back(opId);
                }
            }
            return out;
        }

        bool isDeclaredValue(const wolvrix::lib::grh::Graph &graph,
                             wolvrix::lib::grh::ValueId value) noexcept
        {
            if (!value.valid())
            {
                return false;
            }
            const auto symbol = graph.valueSymbol(value);
            return symbol.valid() && graph.isDeclaredSymbol(symbol);
        }

        bool isDeclaredCutValue(const wolvrix::lib::grh::Graph &graph,
                                const ValueCanonicalMap &canonicalValues,
                                wolvrix::lib::grh::ValueId value) noexcept
        {
            if (isDeclaredValue(graph, value))
            {
                return true;
            }
            const auto canonical = canonicalActivityValue(value, &canonicalValues);
            return canonical != value && isDeclaredValue(graph, canonical);
        }

        wolvrix::lib::grh::OperationId cloneSingleResultSourceOp(wolvrix::lib::grh::Graph &graph,
                                                                 wolvrix::lib::grh::OperationId sourceOpId,
                                                                 wolvrix::lib::grh::ValueId sourceValue,
                                                                 wolvrix::lib::grh::ValueId &cloneValue,
                                                                 std::string &error)
        {
            try
            {
                const auto sourceOp = graph.getOperation(sourceOpId);
                const auto sourceInfo = graph.getValue(sourceValue);
                const auto cloneOp = graph.createOperation(sourceOp.kind(), graph.makeInternalOpSym());
                if (sourceOp.srcLoc())
                {
                    graph.setOpSrcLoc(cloneOp, *sourceOp.srcLoc());
                }
                for (const auto &attr : sourceOp.attrs())
                {
                    graph.setAttr(cloneOp, attr.key, attr.value);
                }
                for (const auto operand : sourceOp.operands())
                {
                    graph.addOperand(cloneOp, operand);
                }
                cloneValue = graph.createValue(graph.makeInternalValSym(),
                                               sourceInfo.width(),
                                               sourceInfo.isSigned(),
                                               sourceInfo.type());
                if (sourceInfo.srcLoc())
                {
                    graph.setValueSrcLoc(cloneValue, *sourceInfo.srcLoc());
                }
                graph.addResult(cloneOp, cloneValue);
                return cloneOp;
            }
            catch (const std::exception &ex)
            {
                error = "activity-schedule source clone failed source=" +
                        describeOp(graph, sourceOpId) + ": " + ex.what();
                return wolvrix::lib::grh::OperationId::invalid();
            }
        }

        bool cloneSourceUsesForCompute(wolvrix::lib::grh::Graph &graph,
                                       std::vector<ActivityOpClass> &opClasses,
                                       ComputeNodeRewriteStats &stats,
                                       ValueCanonicalMap &canonicalValues,
                                       bool &graphChanged,
                                       std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;
            using wolvrix::lib::grh::ValueUser;

            struct Rewrite
            {
                OperationId sourceOp;
                ValueId sourceValue;
                OperationId userOp;
                uint32_t operandIndex = 0;
            };

            std::vector<Rewrite> rewrites;
            std::unordered_set<OperationId, OperationIdHash> originalSourceOps;
            for (const auto opId : graph.operations())
            {
                if (opId.index < opClasses.size() && opClasses[opId.index] == ActivityOpClass::Source)
                {
                    originalSourceOps.insert(opId);
                }
            }

            for (const auto sourceOp : originalSourceOps)
            {
                const auto results = graph.opResults(sourceOp);
                if (results.size() != 1)
                {
                    continue;
                }
                const ValueId sourceValue = results.front();
                const auto sourceValueInfo = graph.getValue(sourceValue);
                const std::vector<ValueUser> users(sourceValueInfo.users().begin(),
                                                   sourceValueInfo.users().end());
                for (const auto &user : users)
                {
                    if (!user.operation.valid() || user.operation.index >= opClasses.size())
                    {
                        continue;
                    }
                    if (opClasses[user.operation.index] != ActivityOpClass::Compute)
                    {
                        continue;
                    }
                    rewrites.push_back(Rewrite{sourceOp, sourceValue, user.operation, user.operandIndex});
                }
            }

            for (const auto &rewrite : rewrites)
            {
                ValueId cloneValue;
                const auto cloneOp =
                    cloneSingleResultSourceOp(graph, rewrite.sourceOp, rewrite.sourceValue, cloneValue, error);
                if (!cloneOp.valid())
                {
                    return false;
                }
                if (cloneOp.index >= opClasses.size())
                {
                    opClasses.resize(cloneOp.index + 1, ActivityOpClass::Unsupported);
                }
                opClasses[cloneOp.index] = ActivityOpClass::Source;
                canonicalValues[cloneValue] = rewrite.sourceValue;
                try
                {
                    graph.replaceOperand(rewrite.userOp, rewrite.operandIndex, cloneValue);
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule source clone replaceOperand failed user=" +
                            describeOp(graph, rewrite.userOp) + ": " + ex.what();
                    return false;
                }
                ++stats.sourceClonesInComputeNodes;
                graphChanged = true;
            }
            return true;
        }

        bool isLocalSharedComputeOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
            case wolvrix::lib::grh::OperationKind::kXMRRead:
            case wolvrix::lib::grh::OperationKind::kXMRWrite:
                return false;
            default:
                return true;
            }
        }

        bool opHasSideEffects(const wolvrix::lib::grh::Operation &op)
        {
            return getAttrValue<bool>(op, "hasSideEffects").value_or(false);
        }

        std::optional<uint64_t> parseSimpleConstUInt64(std::string_view literal)
        {
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (compact.empty() || compact.front() == '-')
            {
                return std::nullopt;
            }
            if (compact.front() == '+')
            {
                compact.erase(compact.begin());
            }
            if (compact.empty())
            {
                return std::nullopt;
            }

            int base = 10;
            std::string digits;
            const std::size_t tick = compact.find('\'');
            if (tick == std::string::npos)
            {
                digits = compact;
            }
            else
            {
                std::size_t pos = tick + 1;
                if (pos < compact.size() && compact[pos] == 's')
                {
                    ++pos;
                }
                if (pos >= compact.size())
                {
                    return std::nullopt;
                }
                switch (compact[pos++])
                {
                case 'b': base = 2; break;
                case 'o': base = 8; break;
                case 'd': base = 10; break;
                case 'h': base = 16; break;
                default: return std::nullopt;
                }
                digits = compact.substr(pos);
            }
            if (digits.empty())
            {
                return std::nullopt;
            }

            uint64_t value = 0;
            for (char ch : digits)
            {
                int digit = -1;
                if (ch >= '0' && ch <= '9')
                {
                    digit = ch - '0';
                }
                else if (ch >= 'a' && ch <= 'f')
                {
                    digit = 10 + (ch - 'a');
                }
                else
                {
                    return std::nullopt;
                }
                if (digit < 0 || digit >= base)
                {
                    return std::nullopt;
                }
                if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(digit)) /
                                static_cast<uint64_t>(base))
                {
                    return std::nullopt;
                }
                value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
            }
            return value;
        }

        bool isRegToMemIntentSlice(const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;
            const auto group = getAttrString(op, "regToMem.intent.group");
            return (op.kind() == OperationKind::kSliceArray || op.kind() == OperationKind::kSliceDynamic) &&
                   getAttrString(op, "regToMem.intent.role").value_or(std::string()) == "slice" &&
                   getAttrString(op, "regToMem.intent.mode").value_or(std::string()) == "array-index" &&
                   group && !group->empty();
        }

        std::optional<wolvrix::lib::grh::ValueId>
        regToMemIntentSliceIndexValue(const wolvrix::lib::grh::Graph &graph,
                                      const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;
            if (!isRegToMemIntentSlice(op) || op.operands().size() != 2)
            {
                return std::nullopt;
            }
            if (op.kind() == OperationKind::kSliceArray)
            {
                return op.operands()[1];
            }

            const auto elementWidth = getAttrValue<int64_t>(op, "regToMem.intent.elementWidth");
            if (!elementWidth || *elementWidth <= 0)
            {
                return std::nullopt;
            }
            if (*elementWidth == 1)
            {
                return op.operands()[1];
            }
            const auto startDefId = graph.valueDef(op.operands()[1]);
            if (!startDefId.valid())
            {
                return std::nullopt;
            }
            const auto startDef = graph.getOperation(startDefId);
            if (startDef.kind() != OperationKind::kMul || startDef.operands().size() != 2)
            {
                return std::nullopt;
            }

            const auto constMatchesWidth = [&](wolvrix::lib::grh::ValueId value) {
                const auto defId = graph.valueDef(value);
                if (!defId.valid())
                {
                    return false;
                }
                const auto def = graph.getOperation(defId);
                if (def.kind() != OperationKind::kConstant)
                {
                    return false;
                }
                const auto literal = getAttrString(def, "constValue");
                if (!literal)
                {
                    return false;
                }
                const auto parsedValue = parseSimpleConstUInt64(*literal);
                return parsedValue && *parsedValue == static_cast<uint64_t>(*elementWidth);
            };

            const auto operands = startDef.operands();
            if (constMatchesWidth(operands[0]))
            {
                return operands[1];
            }
            if (constMatchesWidth(operands[1]))
            {
                return operands[0];
            }
            return std::nullopt;
        }

        std::vector<std::string>
        regToMemIntentSliceStorageReadSymbols(const wolvrix::lib::grh::Graph &graph,
                                              const wolvrix::lib::grh::Operation &op)
        {
            using wolvrix::lib::grh::OperationKind;

            std::vector<std::string> symbols;
            if (!isRegToMemIntentSlice(op) || op.operands().size() != 2)
            {
                return symbols;
            }
            const auto group = getAttrString(op, "regToMem.intent.group");
            const auto elementCount = getAttrValue<int64_t>(op, "regToMem.intent.elementCount");
            if (!group || group->empty() || !elementCount || *elementCount <= 0)
            {
                return symbols;
            }

            const auto concatOpId = graph.valueDef(op.operands().front());
            if (!concatOpId.valid())
            {
                return symbols;
            }
            const auto concatOp = graph.getOperation(concatOpId);
            if (concatOp.kind() != OperationKind::kConcat ||
                getAttrString(concatOp, "regToMem.intent.group").value_or(std::string()) != *group ||
                getAttrString(concatOp, "regToMem.intent.role").value_or(std::string()) != "concat" ||
                getAttrString(concatOp, "regToMem.intent.mode").value_or(std::string()) != "array-index")
            {
                return symbols;
            }

            const auto localRegSymbols =
                getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
            const auto storageRegSymbols =
                getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.storageRegSymbols");
            const auto storageElementCount =
                getAttrValue<int64_t>(concatOp, "regToMem.intent.storageElementCount").value_or(*elementCount);
            const auto storageRowOffset =
                getAttrValue<int64_t>(concatOp, "regToMem.intent.storageRowOffset").value_or(0);
            if (!localRegSymbols ||
                localRegSymbols->size() != static_cast<std::size_t>(*elementCount) ||
                storageElementCount < *elementCount ||
                storageRowOffset < 0 ||
                storageRowOffset > storageElementCount ||
                storageElementCount - storageRowOffset < *elementCount)
            {
                return symbols;
            }

            const std::vector<std::string> &storageSymbols =
                storageRegSymbols ? *storageRegSymbols : *localRegSymbols;
            if (storageSymbols.size() != static_cast<std::size_t>(storageElementCount))
            {
                return symbols;
            }

            symbols.reserve(static_cast<std::size_t>(*elementCount));
            for (int64_t row = 0; row < *elementCount; ++row)
            {
                const auto storageRow = static_cast<std::size_t>(storageRowOffset + row);
                if (storageRow >= storageSymbols.size() || storageSymbols[storageRow].empty())
                {
                    symbols.clear();
                    return symbols;
                }
                symbols.push_back(storageSymbols[storageRow]);
            }
            return symbols;
        }

        std::size_t semanticConsumerCount(const wolvrix::lib::grh::Graph &graph,
                                          wolvrix::lib::grh::ValueId value,
                                          const std::vector<ActivityOpClass> &opClasses,
                                          uint32_t currentNode,
                                          const std::vector<uint32_t> &nodeOfOp)
        {
            std::unordered_set<uint64_t> consumers;
            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                if (!user.operation.valid() || user.operation.index >= opClasses.size())
                {
                    continue;
                }
                const ActivityOpClass opClass = opClasses[user.operation.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    continue;
                }
                if (user.operation.index < nodeOfOp.size() && nodeOfOp[user.operation.index] == currentNode)
                {
                    consumers.insert((static_cast<uint64_t>(currentNode) << 32) |
                                     static_cast<uint64_t>(user.operation.index));
                    continue;
                }
                consumers.insert(static_cast<uint64_t>(user.operation.index));
            }
            return consumers.size();
        }

        bool isEarliestSemanticConsumer(const wolvrix::lib::grh::Graph &graph,
                                        wolvrix::lib::grh::ValueId value,
                                        wolvrix::lib::grh::OperationId candidate,
                                        const std::vector<ActivityOpClass> &opClasses,
                                        const ActivityOpData &opData)
        {
            if (!candidate.valid() || candidate.index >= opData.topoPosByOpIndex.size())
            {
                return false;
            }
            const uint32_t candidatePos = opData.topoPosByOpIndex[candidate.index];
            if (candidatePos == kInvalidActivitySupernodeId)
            {
                return false;
            }
            uint32_t bestPos = kInvalidActivitySupernodeId;
            wolvrix::lib::grh::OperationId bestOp = wolvrix::lib::grh::OperationId::invalid();
            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                if (!user.operation.valid() || user.operation.index >= opClasses.size() ||
                    user.operation.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const ActivityOpClass opClass = opClasses[user.operation.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    continue;
                }
                const uint32_t userPos = opData.topoPosByOpIndex[user.operation.index];
                if (userPos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                if (bestOp == wolvrix::lib::grh::OperationId::invalid() ||
                    userPos < bestPos ||
                    (userPos == bestPos && user.operation.index < bestOp.index))
                {
                    bestPos = userPos;
                    bestOp = user.operation;
                }
            }
            return bestOp == candidate;
        }

        bool otherSemanticConsumerCanReachNode(const wolvrix::lib::grh::Graph &graph,
                                               wolvrix::lib::grh::ValueId value,
                                               wolvrix::lib::grh::OperationId currentConsumer,
                                               const std::vector<wolvrix::lib::grh::OperationId> &nodeOps,
                                               const std::vector<ActivityOpClass> &opClasses,
                                               const ActivityOpData &opData)
        {
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                targets;
            targets.reserve(nodeOps.size());
            uint32_t maxTargetPos = 0;
            bool haveTarget = false;
            for (const auto opId : nodeOps)
            {
                if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                if (pos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                targets.insert(opId);
                maxTargetPos = haveTarget ? std::max(maxTargetPos, pos) : pos;
                haveTarget = true;
            }
            if (!haveTarget)
            {
                return false;
            }

            constexpr std::size_t kMaxReachabilityVisits = 4096;
            const auto canFollow = [&](wolvrix::lib::grh::OperationId opId)
            {
                if (!opId.valid() || opId.index >= opClasses.size() ||
                    opId.index >= opData.topoPosByOpIndex.size())
                {
                    return false;
                }
                const ActivityOpClass opClass = opClasses[opId.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Sink)
                {
                    return false;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                return pos != kInvalidActivitySupernodeId && pos <= maxTargetPos;
            };

            const auto valueInfo = graph.getValue(value);
            for (const auto &user : valueInfo.users())
            {
                const auto start = user.operation;
                if (start == currentConsumer || targets.find(start) != targets.end() ||
                    !canFollow(start))
                {
                    continue;
                }

                std::vector<wolvrix::lib::grh::OperationId> stack;
                std::unordered_set<wolvrix::lib::grh::OperationId,
                                   wolvrix::lib::grh::OperationIdHash>
                    seen;
                stack.push_back(start);
                seen.insert(start);
                std::size_t visits = 0;
                while (!stack.empty())
                {
                    const auto opId = stack.back();
                    stack.pop_back();
                    if (++visits > kMaxReachabilityVisits)
                    {
                        return true;
                    }
                    for (const auto result : graph.opResults(opId))
                    {
                        const auto resultInfo = graph.getValue(result);
                        for (const auto &nextUser : resultInfo.users())
                        {
                            const auto next = nextUser.operation;
                            if (targets.find(next) != targets.end())
                            {
                                return true;
                            }
                            if (!canFollow(next))
                            {
                                continue;
                            }
                            if (seen.insert(next).second)
                            {
                                stack.push_back(next);
                            }
                        }
                    }
                }
            }
            return false;
        }

        std::string widthBucket(std::size_t width)
        {
            if (width <= 1)
            {
                return "1";
            }
            if (width <= 8)
            {
                return "2-8";
            }
            if (width <= 32)
            {
                return "9-32";
            }
            if (width <= 64)
            {
                return "33-64";
            }
            if (width <= 256)
            {
                return "65-256";
            }
            return ">256";
        }

        std::string fanoutBucket(std::size_t fanout)
        {
            if (fanout <= 1)
            {
                return "1";
            }
            if (fanout <= 4)
            {
                return "2-4";
            }
            if (fanout <= 16)
            {
                return "5-16";
            }
            if (fanout <= 64)
            {
                return "17-64";
            }
            return ">64";
        }

        class ComputeNodeBuilder
        {
        public:
            ComputeNodeBuilder(wolvrix::lib::grh::Graph &graph,
                               const ActivityScheduleOptions &options,
                               const ActivityOpData &opData,
                               std::vector<ActivityOpClass> &opClasses,
                               ComputeRewriteBuild &build,
                               std::string &error)
                : graph_(graph),
                  options_(options),
                  opData_(opData),
                  opClasses_(opClasses),
                  build_(build),
                  error_(error)
            {
            }

            uint32_t ensureSourceOwnerNode(wolvrix::lib::grh::OperationId opId)
            {
                if (!opId.valid())
                {
                    return kInvalidActivitySupernodeId;
                }
                ensureOpCapacity(opId);
                uint32_t &owner = build_.computeNodeOfOp[opId.index];
                if (owner != kInvalidActivitySupernodeId)
                {
                    return owner;
                }
                owner = newNode(false);
                build_.computeNodes[owner].ops.push_back(opId);
                processOperandsBounded(owner, opId);
                return owner;
            }

            uint32_t ensureComputeNodeForOp(wolvrix::lib::grh::OperationId opId, bool commonExpr)
            {
                if (!opId.valid())
                {
                    return kInvalidActivitySupernodeId;
                }
                ensureOpCapacity(opId);
                uint32_t &owner = build_.computeNodeOfOp[opId.index];
                if (owner != kInvalidActivitySupernodeId)
                {
                    return owner;
                }
                owner = newNode(commonExpr);
                absorbOp(owner, opId);
                return owner;
            }

            std::optional<uint32_t> createIntentGroupNode(std::string group,
                                                          std::vector<wolvrix::lib::grh::OperationId> ops)
            {
                ops = uniqueOpsPreservingOrder(ops);
                if (group.empty() || ops.empty())
                {
                    return std::nullopt;
                }
                for (const auto opId : ops)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    ensureOpCapacity(opId);
                    const uint32_t existingOwner = build_.computeNodeOfOp[opId.index];
                    if (existingOwner != kInvalidActivitySupernodeId)
                    {
                        if (existingOwner < build_.computeNodes.size() &&
                            build_.computeNodes[existingOwner].intentGroup == group)
                        {
                            continue;
                        }
                        error_ = "activity-schedule reg-to-mem intent group overlaps existing compute node group=" +
                                 group + " op=" + describeOp(graph_, opId);
                        return std::nullopt;
                    }
                }

                const uint32_t nodeId = newNode(false);
                auto &node = build_.computeNodes[nodeId];
                node.indivisible = true;
                node.intentGroup = std::move(group);
                std::sort(ops.begin(), ops.end(), [&](const auto lhs, const auto rhs) {
                    return topoLessOp(opData_, lhs, rhs);
                });
                ops = uniqueOpsPreservingOrder(ops);
                for (const auto opId : ops)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    ensureOpCapacity(opId);
                    node.ops.push_back(opId);
                    build_.computeNodeOfOp[opId.index] = nodeId;
                }
                return nodeId;
            }

            bool processIntentGroupNode(uint32_t nodeId)
            {
                if (nodeId >= build_.computeNodes.size())
                {
                    return true;
                }
                const auto ops = build_.computeNodes[nodeId].ops;
                for (const auto opId : ops)
                {
                    processOperandsBounded(nodeId, opId);
                    if (!error_.empty())
                    {
                        return false;
                    }
                }
                return true;
            }

        private:
            void ensureOpCapacity(wolvrix::lib::grh::OperationId opId)
            {
                if (opId.index >= build_.computeNodeOfOp.size())
                {
                    build_.computeNodeOfOp.resize(opId.index + 1, kInvalidActivitySupernodeId);
                }
                if (opId.index >= opClasses_.size())
                {
                    opClasses_.resize(opId.index + 1, ActivityOpClass::Unsupported);
                }
            }

            uint32_t newNode(bool commonExpr)
            {
                const uint32_t nodeId = static_cast<uint32_t>(build_.computeNodes.size());
                ComputeNode node;
                node.commonExpr = commonExpr;
                build_.computeNodes.push_back(std::move(node));
                if (commonExpr)
                {
                    ++build_.stats.commonExprComputeNodes;
                }
                return nodeId;
            }

            bool canAddRawOp(uint32_t nodeId) const
            {
                const std::size_t maxOps =
                    options_.maxOpInComputeNode == 0 ? std::numeric_limits<std::size_t>::max()
                                                     : options_.maxOpInComputeNode;
                return nodeId < build_.computeNodes.size() && build_.computeNodes[nodeId].ops.size() < maxOps;
            }

            void addBoundary(uint32_t nodeId, wolvrix::lib::grh::ValueId value)
            {
                if (nodeId >= build_.computeNodes.size() || !value.valid())
                {
                    return;
                }
                auto &inputs = build_.computeNodes[nodeId].boundaryInputs;
                if (!vectorContainsValue(inputs, value))
                {
                    inputs.push_back(value);
                }
            }

            void noteExistingCommonOwner(wolvrix::lib::grh::OperationId defOp,
                                         wolvrix::lib::grh::ValueId value)
            {
                ++build_.stats.computeNodeBoundaryInputExistingCommonOwner;
                build_.stats.computeNodeBoundaryExistingCommonOwnerByKind[
                    std::string(wolvrix::lib::grh::toString(graph_.opKind(defOp)))]++;
                const auto valueInfo = graph_.getValue(value);
                build_.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket[
                    widthBucket(valueInfo.width())]++;
                build_.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket[
                    fanoutBucket(valueInfo.users().size())]++;
            }

            void absorbOp(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid() || !error_.empty())
                {
                    return;
                }
                auto &node = build_.computeNodes[nodeId];
                if (std::find(node.ops.begin(), node.ops.end(), opId) == node.ops.end())
                {
                    node.ops.push_back(opId);
                    ensureOpCapacity(opId);
                    build_.computeNodeOfOp[opId.index] = nodeId;
                }
                processOperandsBounded(nodeId, opId);
            }

            bool absorbSourceOp(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid())
                {
                    return false;
                }
                ensureOpCapacity(opId);
                const uint32_t existingOwner = build_.computeNodeOfOp[opId.index];
                if (existingOwner != kInvalidActivitySupernodeId)
                {
                    return existingOwner == nodeId;
                }
                auto &node = build_.computeNodes[nodeId];
                if (std::find(node.ops.begin(), node.ops.end(), opId) != node.ops.end())
                {
                    build_.computeNodeOfOp[opId.index] = nodeId;
                    return true;
                }
                node.ops.push_back(opId);
                build_.computeNodeOfOp[opId.index] = nodeId;
                processOperandsBounded(nodeId, opId);
                return true;
            }

            void processOperandsBounded(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                if (nodeId >= build_.computeNodes.size() || !opId.valid() || !error_.empty())
                {
                    return;
                }
                if (operandProcessDepth_ >= kMaxOperandProcessRecursion)
                {
                    pendingOperandProcesses_.push_back({nodeId, opId});
                    return;
                }
                ++operandProcessDepth_;
                processOperands(nodeId, opId);
                --operandProcessDepth_;
                if (operandProcessDepth_ == 0 && !drainingPendingOperands_)
                {
                    drainPendingOperandProcesses();
                }
            }

            void drainPendingOperandProcesses()
            {
                if (drainingPendingOperands_)
                {
                    return;
                }
                drainingPendingOperands_ = true;
                while (!pendingOperandProcesses_.empty() && error_.empty())
                {
                    const auto [nodeId, opId] = pendingOperandProcesses_.back();
                    pendingOperandProcesses_.pop_back();
                    processOperandsBounded(nodeId, opId);
                }
                drainingPendingOperands_ = false;
            }

            void processOperands(uint32_t nodeId, wolvrix::lib::grh::OperationId opId)
            {
                    const bool nodeIndivisible =
                        nodeId < build_.computeNodes.size() && build_.computeNodes[nodeId].indivisible;
                    const bool nodeIsIntentGroup =
                        nodeId < build_.computeNodes.size() && !build_.computeNodes[nodeId].intentGroup.empty();
                    const auto originalOperands = graph_.opOperands(opId);
                    std::vector<wolvrix::lib::grh::ValueId> operands(originalOperands.begin(), originalOperands.end());
                    if (nodeIsIntentGroup)
                    {
                        const auto op = graph_.getOperation(opId);
                        if (isRegToMemIntentSlice(op))
                        {
                            operands.clear();
                            if (const auto indexValue = regToMemIntentSliceIndexValue(graph_, op))
                            {
                                operands.push_back(*indexValue);
                            }
                        }
                    }
                    const std::size_t operandCount = operands.size();
                    for (std::size_t operandIndex = 0; operandIndex < operandCount; ++operandIndex)
                    {
                    if (operandIndex >= operands.size())
                    {
                        return;
                    }
                    const auto operand = operands[operandIndex];
                    const bool declaredCut =
                        options_.declaredValueComputeNodeBoundary &&
                        isDeclaredCutValue(graph_, build_.canonicalValues, operand);
                    const auto defOp = graph_.valueDef(operand);
                    if (!defOp.valid())
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        if (declaredCut)
                        {
                            ++build_.stats.computeNodeBoundaryInputDeclared;
                        }
                        else
                        {
                            ++build_.stats.computeNodeBoundaryInputNoDef;
                        }
                        continue;
                    }
                    if (defOp.index >= opClasses_.size())
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputDefOutOfRange;
                        continue;
                    }
                    const ActivityOpClass defClass = opClasses_[defOp.index];
                    if (defClass == ActivityOpClass::Source)
                    {
                        if (declaredCut)
                        {
                            ensureSourceOwnerNode(defOp);
                            if (!error_.empty())
                            {
                                return;
                            }
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputDeclared;
                            continue;
                        }
                        ensureOpCapacity(defOp);
                        const uint32_t existingOwner = build_.computeNodeOfOp[defOp.index];
                        if (existingOwner == nodeId)
                        {
                            continue;
                        }
                        if (nodeIsIntentGroup)
                        {
                            ensureSourceOwnerNode(defOp);
                            if (!error_.empty())
                            {
                                return;
                            }
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputSourceSpill;
                            continue;
                        }
                        if (nodeIndivisible)
                        {
                            ensureSourceOwnerNode(defOp);
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputSourceSpill;
                            continue;
                        }
                        if (canAddRawOp(nodeId) && absorbSourceOp(nodeId, defOp))
                        {
                            continue;
                        }
                        ensureSourceOwnerNode(defOp);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputSourceSpill;
                        continue;
                    }
                    if (defClass == ActivityOpClass::Sink)
                    {
                        error_ = "activity-schedule compute-supernode builder encountered sink predecessor source=" +
                                 describeOp(graph_, defOp) + " user=" + describeOp(graph_, opId);
                        return;
                    }
                    if (defClass != ActivityOpClass::Compute)
                    {
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputUnsupported;
                        continue;
                    }
                    if (declaredCut)
                    {
                        const bool common =
                            semanticConsumerCount(graph_,
                                                  operand,
                                                  opClasses_,
                                                  kInvalidActivitySupernodeId,
                                                  build_.computeNodeOfOp) > 1;
                        ensureComputeNodeForOp(defOp, common);
                        if (!error_.empty())
                        {
                            return;
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputDeclared;
                        continue;
                    }
                    if (!isLocalSharedComputeOpKind(graph_.opKind(defOp)))
                    {
                        if (opHasSideEffects(graph_.getOperation(defOp)))
                        {
                            ensureComputeNodeForOp(defOp, true);
                            if (!error_.empty())
                            {
                                return;
                            }
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputUnsupported;
                        continue;
                    }
                    ensureOpCapacity(defOp);
                    const uint32_t existingOwner = build_.computeNodeOfOp[defOp.index];
                    if (existingOwner != kInvalidActivitySupernodeId)
                    {
                        if (existingOwner != nodeId)
                        {
                            addBoundary(nodeId, operand);
                            ++build_.stats.computeNodeBoundaryInputsTotal;
                            ++build_.stats.computeNodeBoundaryInputExistingOwner;
                            if (existingOwner < build_.computeNodes.size() &&
                                build_.computeNodes[existingOwner].commonExpr)
                            {
                                noteExistingCommonOwner(defOp, operand);
                            }
                        }
                        continue;
                    }
                    if (nodeIsIntentGroup)
                    {
                        const bool common =
                            semanticConsumerCount(graph_,
                                                  operand,
                                                  opClasses_,
                                                  kInvalidActivitySupernodeId,
                                                  build_.computeNodeOfOp) > 1;
                        ensureComputeNodeForOp(defOp, common);
                        if (!error_.empty())
                        {
                            return;
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputExistingOwner;
                        continue;
                    }
                    if (nodeIndivisible)
                    {
                        const bool common =
                            semanticConsumerCount(graph_,
                                                  operand,
                                                  opClasses_,
                                                  kInvalidActivitySupernodeId,
                                                  build_.computeNodeOfOp) > 1;
                        ensureComputeNodeForOp(defOp, common);
                        if (!error_.empty())
                        {
                            return;
                        }
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputExistingOwner;
                        continue;
                    }

                    const std::size_t consumers =
                        semanticConsumerCount(graph_, operand, opClasses_, nodeId, build_.computeNodeOfOp);
                    const bool shared = consumers > 1;
                    if (shared)
                    {
                        if (canAddRawOp(nodeId) &&
                            nodeId < build_.computeNodes.size() &&
                            !build_.computeNodes[nodeId].commonExpr &&
                            isEarliestSemanticConsumer(graph_, operand, opId, opClasses_, opData_) &&
                            !otherSemanticConsumerCanReachNode(graph_,
                                                               operand,
                                                               opId,
                                                               build_.computeNodes[nodeId].ops,
                                                               opClasses_,
                                                               opData_))
                        {
                            absorbOp(nodeId, defOp);
                            if (!error_.empty())
                            {
                                return;
                            }
                            continue;
                        }
                        ensureComputeNodeForOp(defOp, true);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputShared;
                        continue;
                    }
                    if (!canAddRawOp(nodeId))
                    {
                        ensureComputeNodeForOp(defOp, false);
                        addBoundary(nodeId, operand);
                        ++build_.stats.computeNodeBoundaryInputsTotal;
                        ++build_.stats.computeNodeBoundaryInputCapacity;
                        continue;
                    }
                    absorbOp(nodeId, defOp);
                    if (!error_.empty())
                    {
                        return;
                    }
                }
            }

            wolvrix::lib::grh::Graph &graph_;
            const ActivityScheduleOptions &options_;
            const ActivityOpData &opData_;
            std::vector<ActivityOpClass> &opClasses_;
            ComputeRewriteBuild &build_;
            std::string &error_;
            static constexpr std::size_t kMaxOperandProcessRecursion = 256;
            std::size_t operandProcessDepth_ = 0;
            bool drainingPendingOperands_ = false;
            std::vector<std::pair<uint32_t, wolvrix::lib::grh::OperationId>> pendingOperandProcesses_;
        };

        struct RegToMemIntentComputeGroup
        {
            std::string group;
            std::vector<wolvrix::lib::grh::OperationId> ops;
        };

        std::vector<RegToMemIntentComputeGroup>
        collectRegToMemIntentComputeGroups(const wolvrix::lib::grh::Graph &graph,
                                           const std::vector<ActivityOpClass> &opClasses)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationKind;
            using wolvrix::lib::grh::OperationIdHash;

            struct GroupBuild
            {
                std::string group;
                std::vector<OperationId> ops;
                std::unordered_set<OperationId, OperationIdHash> seen;
                std::optional<int64_t> elementWidth;
                std::optional<int64_t> elementCount;
                bool valid = true;
            };

            std::vector<GroupBuild> builders;
            std::unordered_map<std::string, std::size_t> indexByGroup;

            const auto addOp = [&](GroupBuild &builder, OperationId opId)
            {
                if (!opId.valid() || opId.index >= opClasses.size())
                {
                    return;
                }
                const ActivityOpClass opClass = opClasses[opId.index];
                if (opClass != ActivityOpClass::Compute && opClass != ActivityOpClass::Source)
                {
                    return;
                }
                if (builder.seen.insert(opId).second)
                {
                    builder.ops.push_back(opId);
                }
            };

            const auto validateCommonAttrs = [](const wolvrix::lib::grh::Operation &op,
                                                std::string_view group,
                                                std::string_view role,
                                                GroupBuild &builder) {
                if (getAttrString(op, "regToMem.intent.group").value_or(std::string()) != group ||
                    getAttrString(op, "regToMem.intent.role").value_or(std::string()) != role ||
                    getAttrString(op, "regToMem.intent.mode").value_or(std::string()) != "array-index")
                {
                    return false;
                }
                const auto elementWidth = getAttrValue<int64_t>(op, "regToMem.intent.elementWidth");
                const auto elementCount = getAttrValue<int64_t>(op, "regToMem.intent.elementCount");
                if (!elementWidth || !elementCount || *elementWidth <= 0 || *elementCount <= 0)
                {
                    return false;
                }
                if (builder.elementWidth && *builder.elementWidth != *elementWidth)
                {
                    return false;
                }
                if (builder.elementCount && *builder.elementCount != *elementCount)
                {
                    return false;
                }
                builder.elementWidth = *elementWidth;
                builder.elementCount = *elementCount;
                return true;
            };

            for (const auto sliceOpId : graph.operations())
            {
                const auto sliceOp = graph.getOperation(sliceOpId);
                if (sliceOp.kind() != OperationKind::kSliceArray &&
                    sliceOp.kind() != OperationKind::kSliceDynamic)
                {
                    continue;
                }
                const auto group = getAttrString(sliceOp, "regToMem.intent.group");
                const auto role = getAttrString(sliceOp, "regToMem.intent.role");
                const auto mode = getAttrString(sliceOp, "regToMem.intent.mode");
                if (!group || group->empty() || role.value_or(std::string()) != "slice" ||
                    mode.value_or(std::string()) != "array-index")
                {
                    continue;
                }
                auto [it, inserted] = indexByGroup.emplace(*group, builders.size());
                if (inserted)
                {
                    GroupBuild build;
                    build.group = *group;
                    builders.push_back(std::move(build));
                }
                GroupBuild &build = builders[it->second];
                if (!build.valid)
                {
                    continue;
                }
                if (!validateCommonAttrs(sliceOp, *group, "slice", build))
                {
                    build.valid = false;
                    continue;
                }
                const auto sliceWidth = getAttrValue<int64_t>(sliceOp, "sliceWidth");
                if (!sliceWidth || *sliceWidth != *build.elementWidth ||
                    sliceOp.results().empty() ||
                    graph.valueWidth(sliceOp.results().front()) != *build.elementWidth ||
                    !regToMemIntentSliceIndexValue(graph, sliceOp))
                {
                    build.valid = false;
                    continue;
                }
                addOp(build, sliceOpId);
                const auto sliceOperands = sliceOp.operands();
                if (sliceOperands.size() != 2)
                {
                    build.valid = false;
                    continue;
                }
                const OperationId concatOpId = graph.valueDef(sliceOperands.front());
                if (!concatOpId.valid())
                {
                    build.valid = false;
                    continue;
                }
                const auto concatOp = graph.getOperation(concatOpId);
                if (concatOp.kind() != OperationKind::kConcat ||
                    concatOp.results().size() != 1 ||
                    !validateCommonAttrs(concatOp, *group, "concat", build))
                {
                    build.valid = false;
                    continue;
                }
                const auto regSymbols = getAttrValue<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
                const auto operandRows = getAttrValue<std::vector<int64_t>>(concatOp, "regToMem.intent.operandRows");
                const auto storageGroup =
                    getAttrString(concatOp, "regToMem.intent.storageGroup").value_or(*group);
                const auto storageRowOffset =
                    getAttrValue<int64_t>(concatOp, "regToMem.intent.storageRowOffset").value_or(0);
                const auto storageElementCount =
                    getAttrValue<int64_t>(concatOp, "regToMem.intent.storageElementCount")
                        .value_or(*build.elementCount);
                const auto concatOperands = concatOp.operands();
                if (!regSymbols || !operandRows ||
                    regSymbols->size() != static_cast<std::size_t>(*build.elementCount) ||
                    operandRows->size() != concatOperands.size() ||
                    concatOperands.size() != static_cast<std::size_t>(*build.elementCount) ||
                    storageGroup.empty() ||
                    storageElementCount < *build.elementCount ||
                    storageRowOffset < 0 ||
                    storageRowOffset > storageElementCount ||
                    storageElementCount - storageRowOffset < *build.elementCount)
                {
                    build.valid = false;
                    continue;
                }
                addOp(build, concatOpId);
                for (std::size_t operandIndex = 0; operandIndex < concatOperands.size(); ++operandIndex)
                {
                    const auto operand = concatOperands[operandIndex];
                    const OperationId readOpId = graph.valueDef(operand);
                    if (!readOpId.valid())
                    {
                        build.valid = false;
                        break;
                    }
                    const auto readOp = graph.getOperation(readOpId);
                    const int64_t row = (*operandRows)[operandIndex];
                    const auto readGroup = getAttrString(readOp, "regToMem.intent.group");
                    const auto readRow = getAttrValue<int64_t>(readOp, "regToMem.intent.row");
                    const auto readStorageGroup = getAttrString(readOp, "regToMem.intent.storageGroup");
                    const auto readStorageRow = getAttrValue<int64_t>(readOp, "regToMem.intent.storageRow");
                    const bool readLocalMatch = readGroup && *readGroup == *group && readRow && *readRow == row;
                    const bool readStorageMatch = readStorageGroup && *readStorageGroup == storageGroup &&
                                                  readStorageRow &&
                                                  *readStorageRow == row + storageRowOffset;
                    const auto readRegSymbol = getAttrString(readOp, "regSymbol");
                    if (readOp.kind() != OperationKind::kRegisterReadPort ||
                        getAttrString(readOp, "regToMem.intent.role").value_or(std::string()) != "read" ||
                        getAttrString(readOp, "regToMem.intent.mode").value_or(std::string()) != "array-index" ||
                        (!readLocalMatch && !readStorageMatch) ||
                        row < 0 || row >= *build.elementCount ||
                        !readRegSymbol ||
                        (*regSymbols)[static_cast<std::size_t>(row)] != *readRegSymbol ||
                        graph.valueWidth(operand) != *build.elementWidth)
                    {
                        build.valid = false;
                        break;
                    }
                    if (readLocalMatch)
                    {
                        addOp(build, readOpId);
                    }
                }
            }

            std::vector<RegToMemIntentComputeGroup> out;
            out.reserve(builders.size());
            for (auto &builder : builders)
            {
                if (!builder.valid || builder.ops.size() < 3)
                {
                    continue;
                }
                RegToMemIntentComputeGroup group;
                group.group = std::move(builder.group);
                group.ops = std::move(builder.ops);
                out.push_back(std::move(group));
            }
            return out;
        }

        struct ClusterValueEdges
        {
            struct ValueFanout
            {
                uint32_t sourceCluster = kInvalidActivitySupernodeId;
                std::vector<uint32_t> targetClusters;
            };

            std::unordered_map<uint64_t, std::size_t> weights;
            std::vector<std::vector<std::pair<uint32_t, std::size_t>>> outgoing;
            std::vector<ValueFanout> valueFanouts;
            std::vector<wolvrix::lib::grh::ValueId> fanoutValues;
            std::vector<std::vector<uint32_t>> sourceValuesByCluster;
            std::vector<std::vector<uint32_t>> targetValuesByCluster;
            std::vector<std::vector<uint32_t>> commitSuccsByCluster;
        };

        uint64_t packClusterPair(uint32_t from, uint32_t to) noexcept
        {
            return (static_cast<uint64_t>(from) << 32) | static_cast<uint64_t>(to);
        }

        std::size_t clusterEdgeWeight(const ClusterValueEdges &edges, uint32_t from, uint32_t to)
        {
            const auto it = edges.weights.find(packClusterPair(from, to));
            return it == edges.weights.end() ? 0 : it->second;
        }

        std::vector<uint32_t> computeNodeOpSizes(const ComputeRewriteBuild &rewrite)
        {
            std::vector<uint32_t> out;
            out.reserve(rewrite.computeNodes.size());
            for (const auto &node : rewrite.computeNodes)
            {
                out.push_back(static_cast<uint32_t>(node.ops.size()));
            }
            return out;
        }

        std::size_t clusterOpSize(const std::vector<uint32_t> &members,
                                  const std::vector<uint32_t> &nodeOpSizes)
        {
            std::size_t total = 0;
            for (const uint32_t node : members)
            {
                if (node < nodeOpSizes.size())
                {
                    total += nodeOpSizes[node];
                }
            }
            return total;
        }

        std::vector<uint32_t> topoOrderForDag(const std::vector<std::vector<uint32_t>> &dag,
                                              const std::vector<std::size_t> *layerOrderKeys = nullptr)
        {
            wolvrix::lib::toposort::TopoDag<uint32_t> topoDag;
            topoDag.reserveNodes(dag.size());
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                topoDag.addNode(node);
            }
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                for (const auto succ : dag[node])
                {
                    topoDag.addEdge(node, succ);
                }
            }
            std::vector<uint32_t> out;
            const auto layers = topoDag.toposort();
            for (const auto &layer : layers)
            {
                std::vector<uint32_t> ordered(layer.begin(), layer.end());
                std::sort(ordered.begin(),
                          ordered.end(),
                          [&](uint32_t lhs, uint32_t rhs)
                          {
                              if (layerOrderKeys != nullptr &&
                                  lhs < layerOrderKeys->size() &&
                                  rhs < layerOrderKeys->size())
                              {
                                  const std::size_t lhsKey = (*layerOrderKeys)[lhs];
                                  const std::size_t rhsKey = (*layerOrderKeys)[rhs];
                                  if (lhsKey != rhsKey)
                                  {
                                      return lhsKey < rhsKey;
                                  }
                              }
                              return lhs < rhs;
                          });
                out.insert(out.end(), ordered.begin(), ordered.end());
            }
            return out;
        }

        std::vector<std::size_t> minOpIndexBySupernode(const ActivityScheduleBuild &build)
        {
            std::vector<std::size_t> keys(build.supernodeToOps.size(),
                                          std::numeric_limits<std::size_t>::max());
            for (std::size_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    keys[supernodeId] =
                        std::min(keys[supernodeId], static_cast<std::size_t>(opId.index));
                }
            }
            return keys;
        }

        std::vector<uint32_t> topoOrderForDagReadyStack(
            const std::vector<std::vector<uint32_t>> &dag,
            const std::vector<std::size_t> &orderKeys)
        {
            if (orderKeys.size() != dag.size())
            {
                throw std::runtime_error("toposort failed: ready-stack key count mismatch");
            }
            auto lessByKey = [&](uint32_t lhs, uint32_t rhs)
            {
                if (orderKeys[lhs] != orderKeys[rhs])
                {
                    return orderKeys[lhs] < orderKeys[rhs];
                }
                return lhs < rhs;
            };

            std::vector<uint32_t> indegree(dag.size(), 0);
            for (const auto &succs : dag)
            {
                for (const uint32_t succ : succs)
                {
                    if (succ >= indegree.size())
                    {
                        throw std::runtime_error("toposort failed: ready-stack successor out of range");
                    }
                    ++indegree[succ];
                }
            }

            std::vector<uint32_t> readyStack;
            readyStack.reserve(dag.size());
            for (uint32_t node = 0; node < indegree.size(); ++node)
            {
                if (indegree[node] == 0)
                {
                    readyStack.push_back(node);
                }
            }
            std::sort(readyStack.begin(), readyStack.end(), lessByKey);

            std::vector<uint32_t> out;
            out.reserve(dag.size());
            while (!readyStack.empty())
            {
                const uint32_t node = readyStack.back();
                readyStack.pop_back();
                out.push_back(node);

                std::vector<uint32_t> orderedSuccs = dag[node];
                std::sort(orderedSuccs.begin(), orderedSuccs.end(), lessByKey);
                for (const uint32_t succ : orderedSuccs)
                {
                    if (indegree[succ] == 0)
                    {
                        throw std::runtime_error("toposort failed: duplicate ready-stack edge");
                    }
                    --indegree[succ];
                    if (indegree[succ] == 0)
                    {
                        readyStack.push_back(succ);
                    }
                }
            }
            if (out.size() != dag.size())
            {
                throw std::runtime_error("toposort failed: graph contains cycle");
            }
            return out;
        }

        std::vector<uint32_t> topoOrderForDagValueLocal(const std::vector<std::vector<uint32_t>> &dag,
                                                        const ClusterValueEdges *valueEdges)
        {
            std::vector<uint32_t> indegree(dag.size(), 0);
            for (uint32_t node = 0; node < dag.size(); ++node)
            {
                for (const auto succ : dag[node])
                {
                    if (succ < indegree.size())
                    {
                        ++indegree[succ];
                    }
                }
            }

            std::set<uint32_t> ready;
            for (uint32_t node = 0; node < indegree.size(); ++node)
            {
                if (indegree[node] == 0)
                {
                    ready.insert(node);
                }
            }

            std::vector<uint32_t> out;
            out.reserve(dag.size());
            uint32_t previous = kInvalidActivitySupernodeId;
            while (!ready.empty())
            {
                uint32_t node = *ready.begin();
                if (valueEdges != nullptr && previous != kInvalidActivitySupernodeId &&
                    previous < valueEdges->outgoing.size())
                {
                    std::size_t bestWeight = 0;
                    for (const auto &[candidate, weight] : valueEdges->outgoing[previous])
                    {
                        if (ready.find(candidate) == ready.end())
                        {
                            continue;
                        }
                        if (weight > bestWeight || (weight == bestWeight && candidate < node))
                        {
                            node = candidate;
                            bestWeight = weight;
                        }
                    }
                }

                ready.erase(node);
                out.push_back(node);
                previous = node;

                if (node >= dag.size())
                {
                    continue;
                }
                for (const auto succ : dag[node])
                {
                    if (succ >= indegree.size() || indegree[succ] == 0)
                    {
                        continue;
                    }
                    --indegree[succ];
                    if (indegree[succ] == 0)
                    {
                        ready.insert(succ);
                    }
                }
            }
            if (out.size() != dag.size())
            {
                throw std::runtime_error("toposort failed: graph contains cycle");
            }
            return out;
        }

        void buildComputeDag(ComputeRewriteBuild &build,
                             const std::vector<uint32_t> &nodeOfOpByIndex,
                             const wolvrix::lib::grh::Graph &graph)
        {
            build.computeDag.assign(build.computeNodes.size(), {});
            build.stats.computeNodeBoundaryValues = 0;
            build.stats.computeNodeBoundaryDeclaredValues = 0;
            build.stats.computeNodeBoundaryDeclaredEdges = 0;
            std::unordered_set<uint64_t> seen;
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> seenDeclaredValues;
            std::unordered_set<uint64_t> seenDeclaredEdges;
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                for (const auto boundary : build.computeNodes[nodeId].boundaryInputs)
                {
                    const bool declaredBoundary =
                        build.declaredValueComputeNodeBoundary &&
                        isDeclaredCutValue(graph, build.canonicalValues, boundary);
                    if (declaredBoundary && seenDeclaredValues.insert(boundary).second)
                    {
                        ++build.stats.computeNodeBoundaryDeclaredValues;
                    }
                    const auto defOp = graph.valueDef(boundary);
                    if (!defOp.valid() || defOp.index >= nodeOfOpByIndex.size())
                    {
                        continue;
                    }
                    const uint32_t pred = nodeOfOpByIndex[defOp.index];
                    if (pred == kInvalidActivitySupernodeId || pred == nodeId)
                    {
                        continue;
                    }
                    const uint64_t packed = (static_cast<uint64_t>(pred) << 32) | nodeId;
                    if (seen.insert(packed).second)
                    {
                        build.computeDag[pred].push_back(nodeId);
                        ++build.stats.computeNodeBoundaryValues;
                    }
                    if (declaredBoundary && seenDeclaredEdges.insert(packed).second)
                    {
                        ++build.stats.computeNodeBoundaryDeclaredEdges;
                    }
                }
            }
            for (auto &succs : build.computeDag)
            {
                std::sort(succs.begin(), succs.end());
            }
        }

        uint32_t topoPosForOp(const ActivityOpData &opData, wolvrix::lib::grh::OperationId opId)
        {
            if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
            {
                return kInvalidActivitySupernodeId;
            }
            return opData.topoPosByOpIndex[opId.index];
        }

        bool topoLessOp(const ActivityOpData &opData,
                        wolvrix::lib::grh::OperationId lhs,
                        wolvrix::lib::grh::OperationId rhs)
        {
            const uint32_t lhsPos = topoPosForOp(opData, lhs);
            const uint32_t rhsPos = topoPosForOp(opData, rhs);
            if (lhsPos != rhsPos)
            {
                if (lhsPos == kInvalidActivitySupernodeId)
                {
                    return false;
                }
                if (rhsPos == kInvalidActivitySupernodeId)
                {
                    return true;
                }
                return lhsPos < rhsPos;
            }
            return lhs.index < rhs.index;
        }

        void recomputeComputeNodeOwnersAndBoundaries(ComputeRewriteBuild &build,
                                                     const wolvrix::lib::grh::Graph &graph)
        {
            std::size_t mapSize = build.computeNodeOfOp.size();
            for (const auto &node : build.computeNodes)
            {
                for (const auto opId : node.ops)
                {
                    mapSize = std::max<std::size_t>(mapSize, opId.index + 1);
                }
            }
            build.computeNodeOfOp.assign(mapSize, kInvalidActivitySupernodeId);
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                auto &node = build.computeNodes[nodeId];
                node.boundaryInputs.clear();
                for (const auto opId : node.ops)
                {
                    if (opId.index >= build.computeNodeOfOp.size())
                    {
                        build.computeNodeOfOp.resize(opId.index + 1, kInvalidActivitySupernodeId);
                    }
                    build.computeNodeOfOp[opId.index] = nodeId;
                }
            }
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                auto &node = build.computeNodes[nodeId];
                for (const auto opId : node.ops)
                {
                    for (const auto operand : graph.opOperands(opId))
                    {
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid() || defOp.index >= build.computeNodeOfOp.size() ||
                            build.computeNodeOfOp[defOp.index] != nodeId)
                        {
                            if (!vectorContainsValue(node.boundaryInputs, operand))
                            {
                                node.boundaryInputs.push_back(operand);
                            }
                        }
                    }
                }
            }
        }

        bool computeNodeHasDeclaredCutViolation(const ComputeRewriteBuild &build,
                                                const wolvrix::lib::grh::Graph &graph,
                                                uint32_t nodeId)
        {
            if (!build.declaredValueComputeNodeBoundary || nodeId >= build.computeNodes.size())
            {
                return false;
            }
            for (const auto opId : build.computeNodes[nodeId].ops)
            {
                for (const auto operand : graph.opOperands(opId))
                {
                    if (!isDeclaredCutValue(graph, build.canonicalValues, operand))
                    {
                        continue;
                    }
                    const auto defOp = graph.valueDef(operand);
                    if (!defOp.valid() || defOp.index >= build.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    if (build.computeNodeOfOp[defOp.index] == nodeId)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        bool hasDeclaredCutViolation(const ComputeRewriteBuild &build,
                                     const wolvrix::lib::grh::Graph &graph)
        {
            if (!build.declaredValueComputeNodeBoundary)
            {
                return false;
            }
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                if (computeNodeHasDeclaredCutViolation(build, graph, nodeId))
                {
                    return true;
                }
            }
            return false;
        }

        bool splitDeclaredCutComputeNodes(ComputeRewriteBuild &build,
                                          const wolvrix::lib::grh::Graph &graph,
                                          const ActivityOpData &opData)
        {
            if (!build.declaredValueComputeNodeBoundary)
            {
                return true;
            }

            bool changed = false;
            std::vector<ComputeNode> nextNodes;
            nextNodes.reserve(build.computeNodes.size());

            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                const auto &node = build.computeNodes[nodeId];
                if (!computeNodeHasDeclaredCutViolation(build, graph, nodeId))
                {
                    nextNodes.push_back(node);
                    continue;
                }

                std::vector<wolvrix::lib::grh::OperationId> orderedOps = node.ops;
                std::sort(orderedOps.begin(), orderedOps.end(), [&](const auto lhs, const auto rhs) {
                    return topoLessOp(opData, lhs, rhs);
                });

                std::vector<wolvrix::lib::grh::OperationId> chunkOps;
                std::unordered_set<uint32_t> chunkOpIndices;
                auto flushChunk = [&]() {
                    if (chunkOps.empty())
                    {
                        return;
                    }
                    ComputeNode chunk;
                    chunk.ops = std::move(chunkOps);
                    chunk.commonExpr = node.commonExpr;
                    nextNodes.push_back(std::move(chunk));
                    chunkOps = {};
                    chunkOpIndices.clear();
                };

                for (const auto opId : orderedOps)
                {
                    bool cutBeforeOp = false;
                    for (const auto operand : graph.opOperands(opId))
                    {
                        if (!isDeclaredCutValue(graph, build.canonicalValues, operand))
                        {
                            continue;
                        }
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid())
                        {
                            continue;
                        }
                        if (chunkOpIndices.find(defOp.index) != chunkOpIndices.end())
                        {
                            cutBeforeOp = true;
                            break;
                        }
                    }
                    if (cutBeforeOp)
                    {
                        flushChunk();
                    }
                    chunkOps.push_back(opId);
                    chunkOpIndices.insert(opId.index);
                }
                flushChunk();
                ++build.stats.computeNodeDeclaredCutViolationsFixed;
                changed = true;
            }

            if (changed)
            {
                build.computeNodes = std::move(nextNodes);
                recomputeComputeNodeOwnersAndBoundaries(build, graph);
            }
            if (hasDeclaredCutViolation(build, graph))
            {
                ++build.stats.computeNodeDeclaredCutViolationsFatal;
                return false;
            }
            return true;
        }

        bool splitCycleComputeNodesToSingletons(ComputeRewriteBuild &build,
                                                const wolvrix::lib::grh::Graph &graph,
                                                const ActivityOpData &opData,
                                                const std::vector<uint32_t> &cycle)
        {
            if (cycle.empty())
            {
                return false;
            }
            std::vector<uint8_t> splitNode(build.computeNodes.size(), 0U);
            bool anySplit = false;
            std::size_t extraNodes = 0;
            for (const uint32_t nodeId : cycle)
            {
                if (nodeId >= build.computeNodes.size())
                {
                    continue;
                }
                if (build.computeNodes[nodeId].ops.size() <= 1)
                {
                    continue;
                }
                if (splitNode[nodeId] == 0U)
                {
                    splitNode[nodeId] = 1U;
                    anySplit = true;
                    extraNodes += build.computeNodes[nodeId].ops.size() - 1;
                }
            }
            if (!anySplit)
            {
                return false;
            }

            std::vector<ComputeNode> nextNodes;
            nextNodes.reserve(build.computeNodes.size() + extraNodes);
            for (uint32_t nodeId = 0; nodeId < build.computeNodes.size(); ++nodeId)
            {
                const auto &node = build.computeNodes[nodeId];
                if (splitNode[nodeId] == 0U)
                {
                    ComputeNode copy;
                    copy.ops = node.ops;
                    copy.commonExpr = node.commonExpr;
                    nextNodes.push_back(std::move(copy));
                    continue;
                }
                std::vector<wolvrix::lib::grh::OperationId> orderedOps = node.ops;
                std::sort(orderedOps.begin(), orderedOps.end(), [&](const auto lhs, const auto rhs) {
                    return topoLessOp(opData, lhs, rhs);
                });
                for (const auto opId : orderedOps)
                {
                    ComputeNode split;
                    split.ops.push_back(opId);
                    split.commonExpr = node.commonExpr;
                    nextNodes.push_back(std::move(split));
                }
            }
            build.computeNodes = std::move(nextNodes);
            recomputeComputeNodeOwnersAndBoundaries(build, graph);
            return true;
        }

        void appendComputeNodeSummary(std::ostringstream &oss,
                                      const wolvrix::lib::grh::Graph &graph,
                                      const ComputeRewriteBuild &build,
                                      const ActivityOpData &opData,
                                      uint32_t nodeId)
        {
            oss << "node=" << nodeId;
            if (nodeId >= build.computeNodes.size())
            {
                return;
            }
            const auto &node = build.computeNodes[nodeId];
            oss << ":ops=" << node.ops.size()
                << ":boundaries=" << node.boundaryInputs.size();
            if (node.commonExpr)
            {
                oss << ":common";
            }
            uint32_t minTopo = kInvalidActivitySupernodeId;
            uint32_t maxTopo = 0;
            for (const auto opId : node.ops)
            {
                if (!opId.valid() || opId.index >= opData.topoPosByOpIndex.size())
                {
                    continue;
                }
                const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                if (pos == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                minTopo = std::min(minTopo, pos);
                maxTopo = std::max(maxTopo, pos);
            }
            if (minTopo != kInvalidActivitySupernodeId)
            {
                oss << ":topo=[" << minTopo << "," << maxTopo << "]";
            }
            oss << "[";
            const std::size_t opLimit = std::min<std::size_t>(node.ops.size(), 6);
            for (std::size_t opIndex = 0; opIndex < opLimit; ++opIndex)
            {
                if (opIndex != 0)
                {
                    oss << ",";
                }
                const auto opId = node.ops[opIndex];
                oss << describeOp(graph, opId);
                if (opId.valid() && opId.index < opData.topoPosByOpIndex.size())
                {
                    const uint32_t pos = opData.topoPosByOpIndex[opId.index];
                    if (pos != kInvalidActivitySupernodeId)
                    {
                        oss << "@topo" << pos;
                    }
                }
            }
            if (node.ops.size() > opLimit)
            {
                oss << ",...";
            }
            oss << "]";
        }

        void appendComputeNodeEdgeReasons(std::ostringstream &oss,
                                          const wolvrix::lib::grh::Graph &graph,
                                          const ComputeRewriteBuild &build,
                                          const ActivityOpData &opData,
                                          uint32_t from,
                                          uint32_t to)
        {
            oss << " edge " << from << " -> " << to << " via";
            if (to >= build.computeNodes.size())
            {
                oss << " <invalid-target>";
                return;
            }
            std::size_t printed = 0;
            for (const auto boundary : build.computeNodes[to].boundaryInputs)
            {
                const auto defOp = graph.valueDef(boundary);
                if (!defOp.valid() || defOp.index >= build.computeNodeOfOp.size() ||
                    build.computeNodeOfOp[defOp.index] != from)
                {
                    continue;
                }
                for (const auto useOp : build.computeNodes[to].ops)
                {
                    const auto operands = graph.opOperands(useOp);
                    for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
                    {
                        if (operands[operandIndex] != boundary)
                        {
                            continue;
                        }
                        if (printed == 0)
                        {
                            oss << " ";
                        }
                        else
                        {
                            oss << "; ";
                        }
                        oss << describeValue(graph, boundary)
                            << " def=" << describeOp(graph, defOp);
                        if (defOp.index < opData.topoPosByOpIndex.size())
                        {
                            const uint32_t pos = opData.topoPosByOpIndex[defOp.index];
                            if (pos != kInvalidActivitySupernodeId)
                            {
                                oss << "@topo" << pos;
                            }
                        }
                        oss << " use=" << describeOp(graph, useOp)
                            << "(operand=" << operandIndex;
                        if (useOp.index < opData.topoPosByOpIndex.size())
                        {
                            const uint32_t pos = opData.topoPosByOpIndex[useOp.index];
                            if (pos != kInvalidActivitySupernodeId)
                            {
                                oss << ",topo=" << pos;
                            }
                        }
                        oss << ")";
                        ++printed;
                        if (printed >= 6)
                        {
                            oss << "; ...";
                            return;
                        }
                    }
                }
            }
            if (printed == 0)
            {
                oss << " <no matching boundary found>";
            }
        }

        struct NodeClusterView
        {
            std::vector<std::vector<uint32_t>> members;
            std::vector<std::vector<uint32_t>> preds;
            std::vector<std::vector<uint32_t>> succs;
            std::vector<uint32_t> clusterOfNode;
        };

        NodeClusterView buildNodeClusterView(const std::vector<std::vector<uint32_t>> &clusters,
                                             const std::vector<std::vector<uint32_t>> &nodeDag,
                                             std::size_t nodeCount)
        {
            NodeClusterView view;
            view.members = clusters;
            view.preds.resize(clusters.size());
            view.succs.resize(clusters.size());
            view.clusterOfNode.assign(nodeCount, kInvalidActivitySupernodeId);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                auto &members = view.members[clusterId];
                std::sort(members.begin(), members.end());
                members.erase(std::unique(members.begin(), members.end()), members.end());
                for (const auto node : members)
                {
                    if (node < view.clusterOfNode.size())
                    {
                        view.clusterOfNode[node] = clusterId;
                    }
                }
            }
            for (uint32_t node = 0; node < nodeDag.size(); ++node)
            {
                const uint32_t from = node < view.clusterOfNode.size() ? view.clusterOfNode[node]
                                                                       : kInvalidActivitySupernodeId;
                if (from == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                for (const auto succNode : nodeDag[node])
                {
                    const uint32_t to = succNode < view.clusterOfNode.size() ? view.clusterOfNode[succNode]
                                                                             : kInvalidActivitySupernodeId;
                    if (to == kInvalidActivitySupernodeId || to == from)
                    {
                        continue;
                    }
                    view.succs[from].push_back(to);
                    view.preds[to].push_back(from);
                }
            }
            for (auto &preds : view.preds)
            {
                std::sort(preds.begin(), preds.end());
                preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
            }
            for (auto &succs : view.succs)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            return view;
        }

        ClusterValueEdges buildClusterValueEdges(const NodeClusterView &view,
                                                 const ComputeRewriteBuild &rewrite,
                                                 const wolvrix::lib::grh::Graph &graph)
        {
            ClusterValueEdges out;
            out.outgoing.resize(view.members.size());
            out.sourceValuesByCluster.resize(view.members.size());
            out.targetValuesByCluster.resize(view.members.size());
            out.commitSuccsByCluster.resize(view.members.size());
            std::unordered_map<wolvrix::lib::grh::ValueId,
                               uint32_t,
                               wolvrix::lib::grh::ValueIdHash>
                valueToFanout;
            std::unordered_set<wolvrix::lib::grh::ValueId,
                               wolvrix::lib::grh::ValueIdHash>
                externalActivatedValues;
            for (const auto &node : rewrite.computeNodes)
            {
                for (const auto opId : node.ops)
                {
                    const auto op = graph.getOperation(opId);
                    if (!isRegToMemIntentSlice(op))
                    {
                        continue;
                    }
                    const auto indexValue = regToMemIntentSliceIndexValue(graph, op);
                    if (indexValue && !graph.valueDef(*indexValue).valid())
                    {
                        externalActivatedValues.insert(*indexValue);
                    }
                }
            }
            for (uint32_t toCluster = 0; toCluster < view.members.size(); ++toCluster)
            {
                for (const auto nodeId : view.members[toCluster])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    for (const auto boundary : rewrite.computeNodes[nodeId].boundaryInputs)
                    {
                        const auto defOp = graph.valueDef(boundary);
                        uint32_t fromCluster = kInvalidActivitySupernodeId;
                        if (!defOp.valid())
                        {
                            if (!externalActivatedValues.contains(boundary))
                            {
                                continue;
                            }
                        }
                        else if (defOp.index >= rewrite.computeNodeOfOp.size())
                        {
                            continue;
                        }
                        else
                        {
                            const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                            if (predNode == kInvalidActivitySupernodeId ||
                                predNode >= view.clusterOfNode.size())
                            {
                                continue;
                            }
                            fromCluster = view.clusterOfNode[predNode];
                        }
                        if (fromCluster == toCluster)
                        {
                            continue;
                        }
                        auto [it, inserted] =
                            valueToFanout.emplace(boundary, static_cast<uint32_t>(out.valueFanouts.size()));
                        if (inserted)
                        {
                            ClusterValueEdges::ValueFanout fanout;
                            fanout.sourceCluster = fromCluster;
                            out.valueFanouts.push_back(std::move(fanout));
                            out.fanoutValues.push_back(boundary);
                            if (fromCluster != kInvalidActivitySupernodeId &&
                                fromCluster < out.sourceValuesByCluster.size())
                            {
                                out.sourceValuesByCluster[fromCluster].push_back(it->second);
                            }
                        }
                        auto &targets = out.valueFanouts[it->second].targetClusters;
                        if (std::find(targets.begin(), targets.end(), toCluster) == targets.end())
                        {
                            targets.push_back(toCluster);
                            if (fromCluster != kInvalidActivitySupernodeId)
                            {
                                ++out.weights[packClusterPair(fromCluster, toCluster)];
                            }
                            if (toCluster < out.targetValuesByCluster.size())
                            {
                                out.targetValuesByCluster[toCluster].push_back(it->second);
                            }
                        }
                    }
                }
            }
            const uint32_t commitBase = static_cast<uint32_t>(view.members.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t fromCluster = view.clusterOfNode[predNode];
                    if (fromCluster == kInvalidActivitySupernodeId ||
                        fromCluster >= out.commitSuccsByCluster.size())
                    {
                        continue;
                    }
                    out.commitSuccsByCluster[fromCluster].push_back(commitBase + commitId);
                }
            }
            for (auto &fanout : out.valueFanouts)
            {
                std::sort(fanout.targetClusters.begin(), fanout.targetClusters.end());
            }
            for (auto &values : out.sourceValuesByCluster)
            {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }
            for (auto &values : out.targetValuesByCluster)
            {
                std::sort(values.begin(), values.end());
                values.erase(std::unique(values.begin(), values.end()), values.end());
            }
            for (auto &succs : out.commitSuccsByCluster)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            for (const auto &[packed, weight] : out.weights)
            {
                const uint32_t from = static_cast<uint32_t>(packed >> 32);
                const uint32_t to = static_cast<uint32_t>(packed & 0xffffffffu);
                if (from < out.outgoing.size())
                {
                    out.outgoing[from].push_back({to, weight});
                }
            }
            for (auto &edges : out.outgoing)
            {
                std::sort(edges.begin(),
                          edges.end(),
                          [](const auto &lhs, const auto &rhs)
                          {
                              if (lhs.second != rhs.second)
                              {
                                  return lhs.second > rhs.second;
                              }
                              return lhs.first < rhs.first;
                          });
            }
            return out;
        }

        void recordInitialComputeSupernodeStats(const NodeClusterView &view,
                                                const ClusterValueEdges &valueEdges,
                                                ComputeRewriteBuild &rewrite,
                                                const wolvrix::lib::grh::Graph &graph,
                                                const std::vector<uint32_t> &nodeOpSizes)
        {
            auto &stats = rewrite.stats;
            stats.initialComputeSupernodes = view.members.size();
            stats.initialComputeSupernodeOpsTotal = 0;
            stats.initialComputeSupernodeDagEdges = 0;
            stats.initialBoundaryValues = 0;
            stats.initialBoundaryActivationEdges = 0;
            stats.initialComputeComputeValuePairs = 0;
            stats.initialComputeCommitValuePairs = 0;
            for (const auto &members : view.members)
            {
                stats.initialComputeSupernodeOpsTotal += clusterOpSize(members, nodeOpSizes);
            }

            std::unordered_set<uint64_t> dagEdges;
            dagEdges.reserve(valueEdges.weights.size() + rewrite.commitNodes.size());
            std::unordered_set<std::size_t> boundaryValues;
            boundaryValues.reserve(valueEdges.valueFanouts.size() + rewrite.commitNodes.size());

            auto noteComputeFanout = [&](wolvrix::lib::grh::ValueId value, uint32_t from, uint32_t to) {
                if (!value.valid() || from == kInvalidActivitySupernodeId ||
                    to == kInvalidActivitySupernodeId || from == to)
                {
                    return;
                }
                boundaryValues.insert(value.index);
                dagEdges.insert(packClusterPair(from, to));
                ++stats.initialBoundaryActivationEdges;
                ++stats.initialComputeComputeValuePairs;
            };

            for (std::size_t valueFanoutId = 0; valueFanoutId < valueEdges.valueFanouts.size(); ++valueFanoutId)
            {
                if (valueFanoutId >= valueEdges.fanoutValues.size())
                {
                    continue;
                }
                const auto value = valueEdges.fanoutValues[valueFanoutId];
                const auto &fanout = valueEdges.valueFanouts[valueFanoutId];
                for (const uint32_t to : fanout.targetClusters)
                {
                    noteComputeFanout(value, fanout.sourceCluster, to);
                }
            }

            const uint32_t commitBase = static_cast<uint32_t>(view.members.size());
            for (uint32_t commitId = 0; commitId < rewrite.commitNodes.size(); ++commitId)
            {
                const auto &commit = rewrite.commitNodes[commitId];
                for (const auto input : commit.inputValues)
                {
                    const auto defOp = graph.valueDef(input);
                    if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                    if (predNode == kInvalidActivitySupernodeId ||
                        predNode >= view.clusterOfNode.size())
                    {
                        continue;
                    }
                    const uint32_t from = view.clusterOfNode[predNode];
                    if (from == kInvalidActivitySupernodeId)
                    {
                        continue;
                    }
                    boundaryValues.insert(input.index);
                    dagEdges.insert(packClusterPair(from, commitBase + commitId));
                    ++stats.initialBoundaryActivationEdges;
                    ++stats.initialComputeCommitValuePairs;
                }
            }

            stats.initialComputeSupernodeDagEdges = dagEdges.size();
            stats.initialBoundaryValues = boundaryValues.size();
        }

        std::vector<std::vector<uint32_t>> canonicalizeNodeClusters(std::vector<std::vector<uint32_t>> clusters,
                                                                    const std::vector<uint32_t> &nodeTopoPos)
        {
            for (auto &members : clusters)
            {
                std::sort(members.begin(),
                          members.end(),
                          [&](uint32_t lhs, uint32_t rhs)
                          {
                              const uint32_t lhsPos = lhs < nodeTopoPos.size() ? nodeTopoPos[lhs]
                                                                               : kInvalidActivitySupernodeId;
                              const uint32_t rhsPos = rhs < nodeTopoPos.size() ? nodeTopoPos[rhs]
                                                                               : kInvalidActivitySupernodeId;
                              if (lhsPos != rhsPos)
                              {
                                  return lhsPos < rhsPos;
                              }
                              return lhs < rhs;
                          });
            }
            std::sort(clusters.begin(),
                      clusters.end(),
                      [&](const auto &lhs, const auto &rhs)
                      {
                          const uint32_t lhsHead =
                              lhs.empty() || lhs.front() >= nodeTopoPos.size() ? kInvalidActivitySupernodeId
                                                                               : nodeTopoPos[lhs.front()];
                          const uint32_t rhsHead =
                              rhs.empty() || rhs.front() >= nodeTopoPos.size() ? kInvalidActivitySupernodeId
                                                                               : nodeTopoPos[rhs.front()];
                          if (lhsHead != rhsHead)
                          {
                              return lhsHead < rhsHead;
                          }
                          return lhs < rhs;
                      });
            return clusters;
        }

        std::string formatTopCounts(const ActivityScheduleSummaryStats::KindCountMap &counts,
                                    std::size_t limit)
        {
            std::vector<std::pair<std::string, std::size_t>> ordered(counts.begin(), counts.end());
            std::sort(ordered.begin(),
                      ordered.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.second != rhs.second)
                          {
                              return lhs.second > rhs.second;
                          }
                          return lhs.first < rhs.first;
                      });
            std::ostringstream oss;
            for (std::size_t i = 0; i < ordered.size() && i < limit; ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << ordered[i].first << ":" << ordered[i].second;
            }
            return oss.str();
        }

        std::size_t percentileOfSorted(const std::vector<std::size_t> &values, std::size_t pct)
        {
            if (values.empty())
            {
                return 0;
            }
            const std::size_t index =
                std::min(values.size() - 1, (values.size() - 1) * pct / static_cast<std::size_t>(100));
            return values[index];
        }

        std::string summarizeCoarsenClusterShape(const NodeClusterView &view,
                                                 const ComputeRewriteBuild &rewrite,
                                                 const wolvrix::lib::grh::Graph &graph,
                                                 const std::vector<uint32_t> &nodeOpSizes)
        {
            std::size_t isolated = 0;
            std::size_t sources = 0;
            std::size_t sinks = 0;
            std::size_t linear = 0;
            std::size_t forks = 0;
            std::size_t joins = 0;
            std::size_t maxPred = 0;
            std::size_t maxSucc = 0;
            std::vector<std::size_t> opSizes;
            opSizes.reserve(view.members.size());
            ActivityScheduleSummaryStats::KindCountMap boundaryDefKinds;
            ActivityScheduleSummaryStats::KindCountMap boundaryUseKinds;

            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const std::size_t predCount = clusterId < view.preds.size() ? view.preds[clusterId].size() : 0;
                const std::size_t succCount = clusterId < view.succs.size() ? view.succs[clusterId].size() : 0;
                maxPred = std::max(maxPred, predCount);
                maxSucc = std::max(maxSucc, succCount);
                if (predCount == 0 && succCount == 0)
                {
                    ++isolated;
                }
                if (predCount == 0 && succCount != 0)
                {
                    ++sources;
                }
                if (succCount == 0 && predCount != 0)
                {
                    ++sinks;
                }
                if (predCount == 1 && succCount == 1)
                {
                    ++linear;
                }
                if (succCount > 1)
                {
                    ++forks;
                }
                if (predCount > 1)
                {
                    ++joins;
                }
                opSizes.push_back(clusterOpSize(view.members[clusterId], nodeOpSizes));

                for (const auto nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    const auto &computeNode = rewrite.computeNodes[nodeId];
                    for (const auto boundary : computeNode.boundaryInputs)
                    {
                        const auto defOp = graph.valueDef(boundary);
                        if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                        if (predNode == kInvalidActivitySupernodeId ||
                            predNode >= view.clusterOfNode.size() ||
                            view.clusterOfNode[predNode] == clusterId)
                        {
                            continue;
                        }
                        ++boundaryDefKinds[std::string(wolvrix::lib::grh::toString(graph.opKind(defOp)))];
                        if (!computeNode.ops.empty())
                        {
                            ++boundaryUseKinds[
                                std::string(wolvrix::lib::grh::toString(graph.opKind(computeNode.ops.front())))];
                        }
                    }
                }
            }

            std::sort(opSizes.begin(), opSizes.end());
            const std::size_t totalOps =
                std::accumulate(opSizes.begin(), opSizes.end(), static_cast<std::size_t>(0));
            const std::size_t meanOps = opSizes.empty() ? 0 : totalOps / opSizes.size();

            std::ostringstream oss;
            oss << "clusters=" << view.members.size()
                << " isolated=" << isolated
                << " sources=" << sources
                << " sinks=" << sinks
                << " linear=" << linear
                << " forks=" << forks
                << " joins=" << joins
                << " max_pred=" << maxPred
                << " max_succ=" << maxSucc
                << " op_size_min=" << (opSizes.empty() ? 0 : opSizes.front())
                << " op_size_mean=" << meanOps
                << " op_size_p50=" << percentileOfSorted(opSizes, 50)
                << " op_size_p90=" << percentileOfSorted(opSizes, 90)
                << " op_size_max=" << (opSizes.empty() ? 0 : opSizes.back())
                << " boundary_def_kinds=" << formatTopCounts(boundaryDefKinds, 12)
                << " boundary_use_kinds=" << formatTopCounts(boundaryUseKinds, 12);
            return oss.str();
        }

        bool orderNodeClustersTopologically(std::vector<std::vector<uint32_t>> &clusters,
                                            const std::vector<std::vector<uint32_t>> &nodeDag,
                                            std::size_t nodeCount,
                                            const ComputeRewriteBuild *rewrite,
                                            const wolvrix::lib::grh::Graph *graph)
        {
            if (clusters.empty())
            {
                return true;
            }
            const NodeClusterView view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            std::vector<uint32_t> order;
            try
            {
                (void)rewrite;
                (void)graph;
                order = topoOrderForDagValueLocal(view.succs, nullptr);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (order.size() != view.members.size())
            {
                return false;
            }
            std::vector<std::vector<uint32_t>> out;
            out.reserve(order.size());
            for (const auto clusterId : order)
            {
                if (clusterId >= view.members.size())
                {
                    return false;
                }
                out.push_back(view.members[clusterId]);
            }
            clusters = std::move(out);
            return true;
        }

        std::uint64_t nodeSiblingPredHash(const std::vector<uint32_t> &preds)
        {
            std::uint64_t hash = static_cast<std::uint64_t>(preds.size()) * 0x9e3779b185ebca87ull;
            for (uint32_t pred : preds)
            {
                hash ^= static_cast<std::uint64_t>(pred) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            }
            return hash;
        }

        bool tryMergeNodeSiblings(std::vector<std::vector<uint32_t>> &clusters,
                                  const std::vector<std::vector<uint32_t>> &nodeDag,
                                  std::size_t nodeCount,
                                  const std::vector<uint32_t> &nodeTopoPos,
                                  const std::vector<uint32_t> &nodeOpSizes,
                                  std::size_t maxOps,
                                  const ComputeRewriteBuild &rewrite,
                                  const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            std::unordered_map<std::uint64_t, std::vector<std::vector<uint32_t>>> buckets;
            buckets.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (view.preds[clusterId].empty())
                {
                    continue;
                }
                auto &bucket = buckets[nodeSiblingPredHash(view.preds[clusterId])];
                auto groupIt = std::find_if(bucket.begin(),
                                            bucket.end(),
                                            [&](const auto &group)
                                            {
                                                return !group.empty() &&
                                                       view.preds[group.front()] == view.preds[clusterId];
                                            });
                if (groupIt == bucket.end())
                {
                    bucket.push_back(std::vector<uint32_t>{clusterId});
                }
                else
                {
                    groupIt->push_back(clusterId);
                }
            }

            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                sizes[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
            }

            bool changed = false;
            for (auto &[_, bucket] : buckets)
            {
                for (auto &siblings : bucket)
                {
                    if (siblings.size() < 2)
                    {
                        continue;
                    }
                    std::sort(siblings.begin(), siblings.end());
                    uint32_t anchor = dsu.find(siblings.front());
                    for (std::size_t index = 1; index < siblings.size(); ++index)
                    {
                        uint32_t lhs = dsu.find(anchor);
                        uint32_t rhs = dsu.find(siblings[index]);
                        if (lhs == rhs)
                        {
                            continue;
                        }
                        if (sizes[lhs] + sizes[rhs] > maxOps)
                        {
                            anchor = rhs;
                            continue;
                        }
                        if (dsu.unite(lhs, rhs))
                        {
                            const uint32_t root = dsu.find(lhs);
                            sizes[root] = sizes[lhs] + sizes[rhs];
                            anchor = root;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed)
            {
                return false;
            }

            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            out.reserve(view.members.size());
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                const uint32_t root = dsu.find(clusterId);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(),
                                       view.members[clusterId].begin(),
                                       view.members[clusterId].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        bool tryMergeNodeOut1(std::vector<std::vector<uint32_t>> &clusters,
                              const std::vector<std::vector<uint32_t>> &nodeDag,
                              std::size_t nodeCount,
                              const std::vector<uint32_t> &nodeTopoPos,
                              const std::vector<uint32_t> &nodeOpSizes,
                              std::size_t maxOps,
                              const ComputeRewriteBuild &rewrite,
                              const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            const auto valueEdges = buildClusterValueEdges(view, rewrite, graph);
            struct Candidate
            {
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t weight = 0;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(view.members.size());
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                if (view.succs[id].size() != 1)
                {
                    continue;
                }
                const uint32_t succ = view.succs[id].front();
                const std::size_t weight = clusterEdgeWeight(valueEdges, id, succ);
                if (weight == 0)
                {
                    continue;
                }
                candidates.push_back(Candidate{id, succ, weight});
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.weight != rhs.weight)
                          {
                              return lhs.weight > rhs.weight;
                          }
                          if (lhs.from != rhs.from)
                          {
                              return lhs.from < rhs.from;
                          }
                          return lhs.to < rhs.to;
                      });
            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                sizes[id] = clusterOpSize(view.members[id], nodeOpSizes);
            }
            bool changed = false;
            for (const auto &candidate : candidates)
            {
                uint32_t lhs = dsu.find(candidate.from);
                uint32_t rhs = dsu.find(candidate.to);
                if (lhs == rhs || sizes[lhs] + sizes[rhs] > maxOps)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    sizes[root] = sizes[lhs] + sizes[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                const uint32_t root = dsu.find(id);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(), view.members[id].begin(), view.members[id].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        bool tryMergeNodeIn1(std::vector<std::vector<uint32_t>> &clusters,
                             const std::vector<std::vector<uint32_t>> &nodeDag,
                             std::size_t nodeCount,
                             const std::vector<uint32_t> &nodeTopoPos,
                             const std::vector<uint32_t> &nodeOpSizes,
                             std::size_t maxOps,
                             const ComputeRewriteBuild &rewrite,
                             const wolvrix::lib::grh::Graph &graph)
        {
            const auto view = buildNodeClusterView(clusters, nodeDag, nodeCount);
            const auto valueEdges = buildClusterValueEdges(view, rewrite, graph);
            struct Candidate
            {
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t weight = 0;
            };
            std::vector<Candidate> candidates;
            candidates.reserve(view.members.size());
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                if (view.preds[id].size() != 1)
                {
                    continue;
                }
                const uint32_t pred = view.preds[id].front();
                const std::size_t weight = clusterEdgeWeight(valueEdges, pred, id);
                if (weight == 0)
                {
                    continue;
                }
                candidates.push_back(Candidate{pred, id, weight});
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          if (lhs.weight != rhs.weight)
                          {
                              return lhs.weight > rhs.weight;
                          }
                          if (lhs.from != rhs.from)
                          {
                              return lhs.from < rhs.from;
                          }
                          return lhs.to < rhs.to;
                      });
            DisjointSet dsu(view.members.size());
            std::vector<std::size_t> sizes(view.members.size(), 0);
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                sizes[id] = clusterOpSize(view.members[id], nodeOpSizes);
            }
            bool changed = false;
            for (const auto &candidate : candidates)
            {
                uint32_t lhs = dsu.find(candidate.to);
                uint32_t rhs = dsu.find(candidate.from);
                if (lhs == rhs || sizes[lhs] + sizes[rhs] > maxOps)
                {
                    continue;
                }
                if (dsu.unite(lhs, rhs))
                {
                    const uint32_t root = dsu.find(lhs);
                    sizes[root] = sizes[lhs] + sizes[rhs];
                    changed = true;
                }
            }
            if (!changed)
            {
                return false;
            }
            std::unordered_map<uint32_t, uint32_t> rootToCluster;
            std::vector<std::vector<uint32_t>> out;
            for (uint32_t id = 0; id < view.members.size(); ++id)
            {
                const uint32_t root = dsu.find(id);
                auto [it, inserted] = rootToCluster.emplace(root, static_cast<uint32_t>(out.size()));
                if (inserted)
                {
                    out.push_back({});
                }
                out[it->second].insert(out[it->second].end(), view.members[id].begin(), view.members[id].end());
            }
            out = canonicalizeNodeClusters(std::move(out), nodeTopoPos);
            if (!orderNodeClustersTopologically(out, nodeDag, nodeCount, &rewrite, &graph))
            {
                return false;
            }
            clusters = std::move(out);
            return true;
        }

        std::vector<std::vector<uint32_t>> buildComputeSupernodeSegments(const NodeClusterView &view,
                                                                         const ClusterValueEdges &valueEdges,
                                                                         const std::vector<uint32_t> &nodeOpSizes,
                                                                         std::size_t maxNodes,
                                                                         const std::vector<double> *valueWeights,
                                                                         double segmentPenalty)
        {
            const std::size_t count = view.members.size();
            if (count == 0)
            {
                return {};
            }

            std::vector<std::size_t> prefixSize(count + 1, 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                prefixSize[i + 1] = prefixSize[i] + clusterOpSize(view.members[i], nodeOpSizes);
            }

            auto segmentSize = [&](std::size_t begin, std::size_t end) {
                return prefixSize[end] - prefixSize[begin];
            };

            constexpr double kInf = std::numeric_limits<double>::infinity();
            std::vector<double> dp(count + 1, kInf);
            std::vector<std::size_t> prev(count + 1, 0);
            std::vector<uint32_t> targetSeen(valueEdges.valueFanouts.size(), 0);
            std::vector<uint32_t> countedIncoming(valueEdges.valueFanouts.size(), 0);
            const auto fanoutWeight = [&](uint32_t valueId) -> double {
                if (valueWeights != nullptr && valueId < valueWeights->size())
                {
                    return std::max(0.0, (*valueWeights)[valueId]);
                }
                return 1.0;
            };
            dp[0] = 0;
            for (std::size_t end = 1; end <= count; ++end)
            {
                const uint32_t stamp = static_cast<uint32_t>(end);
                double incomingActivationCost = 0.0;
                for (std::size_t begin = end; begin > 0; --begin)
                {
                    const std::size_t start = begin - 1;
                    const std::size_t size = segmentSize(start, end);
                    if (size > maxNodes && start + 1 < end)
                    {
                        break;
                    }
                    if (size > maxNodes && start + 1 == end)
                    {
                        continue;
                    }
                    if (start < valueEdges.targetValuesByCluster.size())
                    {
                        for (const auto valueId : valueEdges.targetValuesByCluster[start])
                        {
                            if (valueId >= valueEdges.valueFanouts.size() || targetSeen[valueId] == stamp)
                            {
                                continue;
                            }
                            targetSeen[valueId] = stamp;
                            if (valueEdges.valueFanouts[valueId].sourceCluster < start)
                            {
                                countedIncoming[valueId] = stamp;
                                incomingActivationCost += fanoutWeight(valueId);
                            }
                        }
                    }
                    if (start < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const auto valueId : valueEdges.sourceValuesByCluster[start])
                        {
                            if (valueId < countedIncoming.size() && countedIncoming[valueId] == stamp)
                            {
                                countedIncoming[valueId] = 0;
                                incomingActivationCost -= fanoutWeight(valueId);
                            }
                        }
                    }
                    if (dp[start] == kInf)
                    {
                        continue;
                    }
                    const double candidate = dp[start] + incomingActivationCost + segmentPenalty;
                    if (candidate + 1e-12 < dp[end] ||
                        (std::fabs(candidate - dp[end]) <= 1e-12 && (end - start) > (end - prev[end])))
                    {
                        dp[end] = candidate;
                        prev[end] = start;
                    }
                }
                if (dp[end] == kInf)
                {
                    dp[end] = dp[end - 1] + segmentPenalty;
                    prev[end] = end - 1;
                }
            }

            std::vector<std::pair<std::size_t, std::size_t>> ranges;
            for (std::size_t end = count; end > 0;)
            {
                const std::size_t begin = prev[end];
                ranges.emplace_back(begin, end);
                end = begin;
            }
            std::reverse(ranges.begin(), ranges.end());

            std::vector<std::vector<uint32_t>> segments;
            segments.reserve(ranges.size());
            for (const auto &[begin, end] : ranges)
            {
                std::vector<uint32_t> segment;
                segment.reserve(end - begin);
                for (std::size_t cluster = begin; cluster < end; ++cluster)
                {
                    segment.push_back(static_cast<uint32_t>(cluster));
                }
                segments.push_back(std::move(segment));
            }
            return segments;
        }

        struct PostDpPartitionMetrics
        {
            std::size_t computeBae = 0;
            std::size_t dagEdges = 0;
        };

        struct PostDpDagState
        {
            std::unordered_map<uint64_t, uint32_t> refs;
            std::size_t edges = 0;
        };

        std::vector<uint32_t> postDpSegmentOwners(
            const NodeClusterView &view,
            const std::vector<std::vector<uint32_t>> &segments)
        {
            std::vector<uint32_t> owners(view.members.size(), kInvalidActivitySupernodeId);
            for (uint32_t segmentId = 0; segmentId < segments.size(); ++segmentId)
            {
                for (const uint32_t clusterId : segments[segmentId])
                {
                    if (clusterId < owners.size())
                    {
                        owners[clusterId] = segmentId;
                    }
                }
            }
            return owners;
        }

        std::size_t recountPostDpComputeBae(const ClusterValueEdges &valueEdges,
                                            const std::vector<uint32_t> &ownerByCluster,
                                            std::size_t segmentCount)
        {
            std::vector<uint32_t> segmentSeen(segmentCount, 0);
            uint32_t stamp = 0;
            std::size_t total = 0;
            for (const auto &fanout : valueEdges.valueFanouts)
            {
                ++stamp;
                if (stamp == 0)
                {
                    std::fill(segmentSeen.begin(), segmentSeen.end(), 0);
                    stamp = 1;
                }
                const uint32_t sourceSegment =
                    fanout.sourceCluster < ownerByCluster.size()
                        ? ownerByCluster[fanout.sourceCluster]
                        : kInvalidActivitySupernodeId;
                for (const uint32_t targetCluster : fanout.targetClusters)
                {
                    const uint32_t targetSegment =
                        targetCluster < ownerByCluster.size()
                            ? ownerByCluster[targetCluster]
                            : kInvalidActivitySupernodeId;
                    if (targetSegment == kInvalidActivitySupernodeId ||
                        targetSegment >= segmentSeen.size() ||
                        (sourceSegment != kInvalidActivitySupernodeId &&
                         targetSegment == sourceSegment) ||
                        segmentSeen[targetSegment] == stamp)
                    {
                        continue;
                    }
                    segmentSeen[targetSegment] = stamp;
                    ++total;
                }
            }
            return total;
        }

        std::size_t recountPostDpComputeBoundaryValues(
            const ClusterValueEdges &valueEdges,
            const std::vector<uint32_t> &ownerByCluster)
        {
            std::size_t total = 0;
            for (const auto &fanout : valueEdges.valueFanouts)
            {
                const uint32_t sourceSegment =
                    fanout.sourceCluster < ownerByCluster.size()
                        ? ownerByCluster[fanout.sourceCluster]
                        : kInvalidActivitySupernodeId;
                if (sourceSegment == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                const bool boundary = std::any_of(
                    fanout.targetClusters.begin(),
                    fanout.targetClusters.end(),
                    [&](uint32_t targetCluster)
                    {
                        return targetCluster < ownerByCluster.size() &&
                               ownerByCluster[targetCluster] != kInvalidActivitySupernodeId &&
                               ownerByCluster[targetCluster] != sourceSegment;
                    });
                total += boundary ? 1U : 0U;
            }
            return total;
        }

        PostDpDagState buildPostDpDagState(const NodeClusterView &view,
                                           const ClusterValueEdges &valueEdges,
                                           const std::vector<uint32_t> &ownerByCluster,
                                           std::size_t segmentCount)
        {
            PostDpDagState state;
            state.refs.reserve(valueEdges.weights.size() + view.members.size());
            const auto notePair = [&](uint32_t from, uint32_t to)
            {
                if (from == kInvalidActivitySupernodeId ||
                    to == kInvalidActivitySupernodeId ||
                    from == to)
                {
                    return;
                }
                auto [it, inserted] = state.refs.try_emplace(packClusterPair(from, to), 0);
                ++it->second;
                if (inserted)
                {
                    ++state.edges;
                }
            };
            for (uint32_t fromCluster = 0; fromCluster < view.succs.size(); ++fromCluster)
            {
                if (fromCluster >= ownerByCluster.size())
                {
                    continue;
                }
                for (const uint32_t toCluster : view.succs[fromCluster])
                {
                    if (toCluster < ownerByCluster.size())
                    {
                        notePair(ownerByCluster[fromCluster], ownerByCluster[toCluster]);
                    }
                }
            }
            const uint32_t clusterCount = static_cast<uint32_t>(view.members.size());
            for (uint32_t fromCluster = 0;
                 fromCluster < valueEdges.commitSuccsByCluster.size();
                 ++fromCluster)
            {
                if (fromCluster >= ownerByCluster.size())
                {
                    continue;
                }
                for (const uint32_t rawCommit : valueEdges.commitSuccsByCluster[fromCluster])
                {
                    if (rawCommit < clusterCount)
                    {
                        continue;
                    }
                    const uint32_t commitEndpoint =
                        static_cast<uint32_t>(segmentCount) + (rawCommit - clusterCount);
                    notePair(ownerByCluster[fromCluster], commitEndpoint);
                }
            }
            return state;
        }

        bool equalPostDpDagStates(const PostDpDagState &lhs, const PostDpDagState &rhs)
        {
            if (lhs.edges != rhs.edges || lhs.refs.size() != rhs.refs.size())
            {
                return false;
            }
            for (const auto &[pair, count] : lhs.refs)
            {
                const auto it = rhs.refs.find(pair);
                if (it == rhs.refs.end() || it->second != count)
                {
                    return false;
                }
            }
            return true;
        }

        bool equalPostDpDagSupport(const PostDpDagState &lhs,
                                   const PostDpDagState &rhs)
        {
            if (lhs.edges != rhs.edges || lhs.refs.size() != rhs.refs.size())
            {
                return false;
            }
            return std::all_of(
                lhs.refs.begin(),
                lhs.refs.end(),
                [&](const auto &entry)
                {
                    return rhs.refs.contains(entry.first);
                });
        }

        PostDpPartitionMetrics recountPostDpPartitionMetrics(
            const NodeClusterView &view,
            const ClusterValueEdges &valueEdges,
            const std::vector<std::vector<uint32_t>> &segments)
        {
            const std::vector<uint32_t> owners = postDpSegmentOwners(view, segments);
            return {
                .computeBae = recountPostDpComputeBae(valueEdges, owners, segments.size()),
                .dagEdges = buildPostDpDagState(view, valueEdges, owners, segments.size()).edges,
            };
        }

        bool partitionPolicyAccepts(std::string_view policy,
                                    const PostDpPartitionMetrics &baseline,
                                    const PostDpPartitionMetrics &current,
                                    const PostDpPartitionMetrics &candidate,
                                    std::size_t maxRegressionPpm)
        {
            const std::int64_t localBaeGain =
                static_cast<std::int64_t>(current.computeBae) -
                static_cast<std::int64_t>(candidate.computeBae);
            const std::int64_t localDagGain =
                static_cast<std::int64_t>(current.dagEdges) -
                static_cast<std::int64_t>(candidate.dagEdges);
            const std::int64_t globalBaeGain =
                static_cast<std::int64_t>(baseline.computeBae) -
                static_cast<std::int64_t>(candidate.computeBae);
            const std::int64_t globalDagGain =
                static_cast<std::int64_t>(baseline.dagEdges) -
                static_cast<std::int64_t>(candidate.dagEdges);
            const std::size_t baeRegressionBudget = static_cast<std::size_t>(
                (static_cast<unsigned __int128>(baseline.computeBae) * maxRegressionPpm) /
                1000000U);
            const std::size_t dagRegressionBudget = static_cast<std::size_t>(
                (static_cast<unsigned __int128>(baseline.dagEdges) * maxRegressionPpm) /
                1000000U);

            if (policy == "strict")
            {
                return localBaeGain >= 0 && localDagGain >= 0 &&
                       (localBaeGain > 0 || localDagGain > 0);
            }
            if (policy == "bae-budget")
            {
                return localBaeGain > 0 &&
                       candidate.dagEdges <= baseline.dagEdges + dagRegressionBudget;
            }
            if (policy == "balanced")
            {
                const __int128 localGain =
                    static_cast<__int128>(localBaeGain) *
                        std::max<std::size_t>(baseline.dagEdges, 1) +
                    static_cast<__int128>(localDagGain) *
                        std::max<std::size_t>(baseline.computeBae, 1);
                const __int128 globalGain =
                    static_cast<__int128>(globalBaeGain) *
                        std::max<std::size_t>(baseline.dagEdges, 1) +
                    static_cast<__int128>(globalDagGain) *
                        std::max<std::size_t>(baseline.computeBae, 1);
                return localGain > 0 && globalGain > 0 &&
                       candidate.computeBae <= baseline.computeBae + baeRegressionBudget &&
                       candidate.dagEdges <= baseline.dagEdges + dagRegressionBudget;
            }
            return false;
        }

        bool validateComputeSupernodeSegments(
            const NodeClusterView &view,
            const std::vector<std::vector<uint32_t>> &segments,
            const std::vector<uint32_t> &nodeOpSizes,
            std::size_t maxOps)
        {
            std::vector<uint8_t> seen(view.members.size(), 0U);
            for (const auto &segment : segments)
            {
                if (segment.empty())
                {
                    return false;
                }
                std::size_t segmentOps = 0;
                for (const uint32_t clusterId : segment)
                {
                    if (clusterId >= view.members.size() || seen[clusterId] != 0)
                    {
                        return false;
                    }
                    seen[clusterId] = 1U;
                    segmentOps += clusterOpSize(view.members[clusterId], nodeOpSizes);
                }
                if (segmentOps > maxOps)
                {
                    return false;
                }
            }
            return std::find(seen.begin(), seen.end(), uint8_t{0}) == seen.end();
        }

        bool validateClusterOrderPermutationAndTopo(
            const NodeClusterView &view,
            const std::vector<uint32_t> &order,
            std::vector<uint32_t> *positionOut = nullptr)
        {
            if (order.size() != view.members.size())
            {
                return false;
            }
            std::vector<uint32_t> position(order.size(), kInvalidActivitySupernodeId);
            for (uint32_t pos = 0; pos < order.size(); ++pos)
            {
                const uint32_t clusterId = order[pos];
                if (clusterId >= position.size() ||
                    position[clusterId] != kInvalidActivitySupernodeId)
                {
                    return false;
                }
                position[clusterId] = pos;
            }
            for (uint32_t from = 0; from < view.succs.size(); ++from)
            {
                for (const uint32_t to : view.succs[from])
                {
                    if (to >= position.size() || position[from] >= position[to])
                    {
                        return false;
                    }
                }
            }
            if (positionOut != nullptr)
            {
                *positionOut = std::move(position);
            }
            return true;
        }

        bool packKahnLevelClusters(
            std::vector<std::vector<uint32_t>> &clusters,
            NodeClusterView &view,
            ClusterValueEdges &valueEdges,
            std::vector<std::vector<uint32_t>> &segments,
            const std::vector<uint32_t> &nodeOpSizes,
            const ComputeRewriteBuild &rewrite,
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            std::size_t maxOps,
            std::size_t splitNodeMaxOps,
            ComputeNodeMaterializePerfStats &perf,
            std::string &error)
        {
            if (options.kahnLevelPackPolicy == "off")
            {
                return true;
            }

            perf.kahnLevelPackEvaluated = true;
            perf.kahnLevelPackBaselineSegments = segments.size();
            const PostDpPartitionMetrics baselineMetrics =
                recountPostDpPartitionMetrics(view, valueEdges, segments);
            perf.kahnLevelPackBeforeComputeBae = baselineMetrics.computeBae;
            perf.kahnLevelPackAfterComputeBae = baselineMetrics.computeBae;
            perf.kahnLevelPackBeforeDagEdges = baselineMetrics.dagEdges;
            perf.kahnLevelPackAfterDagEdges = baselineMetrics.dagEdges;
            perf.kahnLevelPackFinalTopoValid = true;

            const std::size_t clusterCount = view.members.size();
            if (clusterCount < 3 || segments.empty())
            {
                return true;
            }

            std::vector<std::size_t> clusterOps(clusterCount, 0);
            std::vector<uint8_t> locked(clusterCount, 0U);
            std::size_t totalOps = 0;
            for (uint32_t clusterId = 0; clusterId < clusterCount; ++clusterId)
            {
                clusterOps[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
                totalOps += clusterOps[clusterId];
                if (clusterOps[clusterId] > maxOps)
                {
                    perf.kahnLevelPackSkippedOversize = true;
                    return true;
                }
                for (const uint32_t nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        error = "activity-schedule Kahn-level pack found an invalid compute node";
                        return false;
                    }
                    const auto &node = rewrite.computeNodes[nodeId];
                    if (node.indivisible || !node.intentGroup.empty())
                    {
                        locked[clusterId] = 1U;
                    }
                    if (options.splitOversizeComputeNodes && splitNodeMaxOps != 0 &&
                        node.ops.size() > splitNodeMaxOps)
                    {
                        perf.kahnLevelPackSkippedSplitSensitive = true;
                        return true;
                    }
                }
            }
            perf.kahnLevelPackLockedClusters =
                std::count(locked.begin(), locked.end(), uint8_t{1});

            std::vector<uint32_t> indegree(clusterCount, 0);
            for (uint32_t from = 0; from < clusterCount; ++from)
            {
                for (const uint32_t to : view.succs[from])
                {
                    if (to >= clusterCount)
                    {
                        error = "activity-schedule Kahn-level pack found an invalid quotient edge";
                        return false;
                    }
                    ++indegree[to];
                }
            }
            std::set<uint32_t> ready;
            for (uint32_t clusterId = 0; clusterId < clusterCount; ++clusterId)
            {
                if (indegree[clusterId] == 0)
                {
                    ready.insert(clusterId);
                }
            }
            std::vector<uint32_t> level(clusterCount, 0);
            std::size_t visited = 0;
            while (!ready.empty())
            {
                const uint32_t clusterId = *ready.begin();
                ready.erase(ready.begin());
                ++visited;
                for (const uint32_t succ : view.succs[clusterId])
                {
                    level[succ] = std::max(level[succ], level[clusterId] + 1);
                    if (--indegree[succ] == 0)
                    {
                        ready.insert(succ);
                    }
                }
            }
            if (visited != clusterCount)
            {
                error = "activity-schedule Kahn-level pack found a cyclic quotient graph";
                return false;
            }
            const uint32_t levelCount =
                *std::max_element(level.begin(), level.end()) + 1;
            perf.kahnLevelPackLevels = levelCount;
            std::vector<std::vector<uint32_t>> levelSlots(levelCount);
            std::vector<uint32_t> slotRankByPosition(clusterCount, 0);
            for (uint32_t pos = 0; pos < clusterCount; ++pos)
            {
                const uint32_t clusterLevel = level[pos];
                slotRankByPosition[pos] =
                    static_cast<uint32_t>(levelSlots[clusterLevel].size());
                levelSlots[clusterLevel].push_back(pos);
            }

            std::size_t affinityReserve = 0;
            for (const auto &targetValues : valueEdges.targetValuesByCluster)
            {
                affinityReserve += targetValues.size();
            }
            std::unordered_map<uint64_t, std::size_t> affinityByPair;
            affinityByPair.reserve(affinityReserve + 1);
            std::vector<uint32_t> previousTargetByLevel(levelCount, 0);
            std::vector<uint32_t> previousTargetStamp(levelCount, 0);
            uint32_t fanoutStamp = 0;
            for (const auto &fanout : valueEdges.valueFanouts)
            {
                ++fanoutStamp;
                if (fanoutStamp == 0)
                {
                    std::fill(previousTargetStamp.begin(), previousTargetStamp.end(), 0);
                    fanoutStamp = 1;
                }
                bool sharedAtLevel = false;
                for (const uint32_t target : fanout.targetClusters)
                {
                    if (target >= clusterCount)
                    {
                        continue;
                    }
                    const uint32_t targetLevel = level[target];
                    if (previousTargetStamp[targetLevel] == fanoutStamp)
                    {
                        const uint32_t lhs = std::min(previousTargetByLevel[targetLevel], target);
                        const uint32_t rhs = std::max(previousTargetByLevel[targetLevel], target);
                        ++affinityByPair[packClusterPair(lhs, rhs)];
                        sharedAtLevel = true;
                    }
                    previousTargetByLevel[targetLevel] = target;
                    previousTargetStamp[targetLevel] = fanoutStamp;
                }
                if (sharedAtLevel)
                {
                    ++perf.kahnLevelPackSharedValues;
                }
            }
            perf.kahnLevelPackAffinityPairs = affinityByPair.size();

            struct Peer
            {
                uint32_t cluster = 0;
                std::size_t affinity = 0;
            };
            constexpr std::size_t kTopPeers = 32;
            std::vector<std::vector<Peer>> peers(clusterCount);
            for (const auto &[packed, affinity] : affinityByPair)
            {
                const uint32_t lhs = static_cast<uint32_t>(packed >> 32);
                const uint32_t rhs = static_cast<uint32_t>(packed);
                if (lhs >= clusterCount || rhs >= clusterCount || level[lhs] != level[rhs])
                {
                    continue;
                }
                peers[lhs].push_back({rhs, affinity});
                peers[rhs].push_back({lhs, affinity});
            }
            for (auto &clusterPeers : peers)
            {
                std::sort(clusterPeers.begin(),
                          clusterPeers.end(),
                          [](const Peer &lhs, const Peer &rhs)
                          {
                              if (lhs.affinity != rhs.affinity)
                              {
                                  return lhs.affinity > rhs.affinity;
                              }
                              return lhs.cluster < rhs.cluster;
                          });
                if (clusterPeers.size() > kTopPeers)
                {
                    clusterPeers.resize(kTopPeers);
                }
            }

            struct Candidate
            {
                uint32_t cluster = 0;
                uint32_t peer = 0;
                std::size_t affinity = 0;
                uint32_t distance = 0;
                bool packsBaselineSegment = false;
            };
            constexpr std::size_t kCandidatesPerCluster = 4;
            const std::vector<uint32_t> baselineOwners =
                postDpSegmentOwners(view, segments);
            std::vector<Candidate> candidates;
            for (uint32_t clusterId = 0; clusterId < clusterCount; ++clusterId)
            {
                const uint32_t clusterRank = slotRankByPosition[clusterId];
                std::size_t added = 0;
                for (const Peer &peer : peers[clusterId])
                {
                    const uint32_t peerRank = slotRankByPosition[peer.cluster];
                    const uint32_t distance = clusterRank > peerRank
                                                  ? clusterRank - peerRank
                                                  : peerRank - clusterRank;
                    if (distance > 1)
                    {
                        const uint32_t destinationRank =
                            clusterRank < peerRank ? peerRank - 1 : peerRank + 1;
                        const auto &slots = levelSlots[level[clusterId]];
                        const uint32_t destinationPosition = slots[destinationRank];
                        const bool packsBaselineSegment =
                            destinationPosition < baselineOwners.size() &&
                            peer.cluster < baselineOwners.size() &&
                            baselineOwners[destinationPosition] == baselineOwners[peer.cluster];
                        candidates.push_back({clusterId,
                                              peer.cluster,
                                              peer.affinity,
                                              distance,
                                              packsBaselineSegment});
                        if (++added >= kCandidatesPerCluster)
                        {
                            break;
                        }
                    }
                }
            }
            std::sort(candidates.begin(),
                      candidates.end(),
                      [](const Candidate &lhs, const Candidate &rhs)
                      {
                          if (lhs.affinity != rhs.affinity)
                          {
                              return lhs.affinity > rhs.affinity;
                          }
                          if (lhs.packsBaselineSegment != rhs.packsBaselineSegment)
                          {
                              return lhs.packsBaselineSegment > rhs.packsBaselineSegment;
                          }
                          if (lhs.distance != rhs.distance)
                          {
                              return lhs.distance > rhs.distance;
                          }
                          if (lhs.cluster != rhs.cluster)
                          {
                              return lhs.cluster < rhs.cluster;
                          }
                          return lhs.peer < rhs.peer;
                      });
            perf.kahnLevelPackCandidates = candidates.size();
            if (candidates.empty())
            {
                return true;
            }

            const std::size_t movedOpBudget = static_cast<std::size_t>(
                (static_cast<unsigned __int128>(totalOps) *
                 options.kahnLevelPackMaxMovedOpPpm) /
                1000000U);
            std::vector<uint32_t> order(clusterCount, 0);
            std::iota(order.begin(), order.end(), 0U);
            std::vector<uint32_t> position = order;
            std::vector<uint8_t> moved(clusterCount, 0U);
            std::size_t movedClusters = 0;
            std::size_t movedOps = 0;

            const auto proposedPosition = [&](uint32_t clusterId,
                                              uint32_t lhs,
                                              uint32_t rhs)
            {
                if (clusterId == lhs)
                {
                    return position[rhs];
                }
                if (clusterId == rhs)
                {
                    return position[lhs];
                }
                return position[clusterId];
            };
            const auto swapPreservesTopo = [&](uint32_t lhs, uint32_t rhs)
            {
                const auto clusterPreservesTopo = [&](uint32_t clusterId)
                {
                    const uint32_t clusterPosition = proposedPosition(clusterId, lhs, rhs);
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        if (pred >= clusterCount ||
                            proposedPosition(pred, lhs, rhs) >= clusterPosition)
                        {
                            return false;
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        if (succ >= clusterCount ||
                            clusterPosition >= proposedPosition(succ, lhs, rhs))
                        {
                            return false;
                        }
                    }
                    return true;
                };
                return clusterPreservesTopo(lhs) && clusterPreservesTopo(rhs);
            };

            for (const Candidate &candidate : candidates)
            {
                if (movedClusters + 2 > options.kahnLevelPackMaxMoves)
                {
                    ++perf.kahnLevelPackRejectedBudget;
                    break;
                }
                if (candidate.cluster >= clusterCount || candidate.peer >= clusterCount ||
                    moved[candidate.cluster] != 0)
                {
                    continue;
                }
                const uint32_t clusterPosition = position[candidate.cluster];
                const uint32_t peerPosition = position[candidate.peer];
                if (level[candidate.cluster] != level[candidate.peer] ||
                    level[candidate.cluster] != level[order[clusterPosition]] ||
                    level[candidate.peer] != level[order[peerPosition]])
                {
                    error = "activity-schedule Kahn-level pack lost a level slot";
                    return false;
                }
                const uint32_t clusterRank = slotRankByPosition[clusterPosition];
                const uint32_t peerRank = slotRankByPosition[peerPosition];
                const uint32_t distance = clusterRank > peerRank
                                              ? clusterRank - peerRank
                                              : peerRank - clusterRank;
                if (distance <= 1)
                {
                    continue;
                }
                const uint32_t destinationRank =
                    clusterRank < peerRank ? peerRank - 1 : peerRank + 1;
                const auto &slots = levelSlots[level[candidate.cluster]];
                if (destinationRank >= slots.size())
                {
                    continue;
                }
                const uint32_t destinationPosition = slots[destinationRank];
                const uint32_t rhs = order[destinationPosition];
                if (rhs == candidate.cluster || rhs == candidate.peer || moved[rhs] != 0)
                {
                    continue;
                }
                if (locked[candidate.cluster] != 0 || locked[rhs] != 0)
                {
                    ++perf.kahnLevelPackRejectedLocked;
                    continue;
                }
                const std::size_t swapOps = clusterOps[candidate.cluster] + clusterOps[rhs];
                if (movedOps + swapOps > movedOpBudget)
                {
                    ++perf.kahnLevelPackRejectedBudget;
                    continue;
                }
                if (!swapPreservesTopo(candidate.cluster, rhs))
                {
                    ++perf.kahnLevelPackRejectedTopo;
                    continue;
                }
                std::swap(order[clusterPosition], order[destinationPosition]);
                position[candidate.cluster] = destinationPosition;
                position[rhs] = clusterPosition;
                moved[candidate.cluster] = 1U;
                moved[rhs] = 1U;
                movedClusters += 2;
                movedOps += swapOps;
                ++perf.kahnLevelPackSwaps;
            }

            if (perf.kahnLevelPackSwaps == 0)
            {
                return true;
            }
            std::vector<uint32_t> exactPosition;
            if (!validateClusterOrderPermutationAndTopo(view, order, &exactPosition))
            {
                ++perf.kahnLevelPackRejectedTopo;
                return true;
            }
            std::size_t exactMovedClusters = 0;
            std::size_t exactMovedOps = 0;
            for (uint32_t clusterId = 0; clusterId < clusterCount; ++clusterId)
            {
                if (exactPosition[clusterId] != clusterId)
                {
                    ++exactMovedClusters;
                    exactMovedOps += clusterOps[clusterId];
                }
            }
            if (exactMovedClusters != movedClusters || exactMovedOps != movedOps ||
                exactMovedClusters > options.kahnLevelPackMaxMoves ||
                exactMovedOps > movedOpBudget)
            {
                error = "activity-schedule Kahn-level pack moved-cluster budget mismatch";
                return false;
            }
            perf.kahnLevelPackMovedClusters = exactMovedClusters;
            perf.kahnLevelPackMovedOps = exactMovedOps;

            std::vector<std::vector<uint32_t>> candidateClusters;
            candidateClusters.reserve(clusterCount);
            for (const uint32_t clusterId : order)
            {
                candidateClusters.push_back(view.members[clusterId]);
            }
            NodeClusterView candidateView =
                buildNodeClusterView(candidateClusters, rewrite.computeDag, rewrite.computeNodes.size());
            ClusterValueEdges candidateValueEdges =
                buildClusterValueEdges(candidateView, rewrite, graph);
            std::vector<std::vector<uint32_t>> candidateSegments =
                buildComputeSupernodeSegments(candidateView,
                                              candidateValueEdges,
                                              nodeOpSizes,
                                              maxOps,
                                              nullptr,
                                              static_cast<double>(options.dpSegmentPenaltyPpm) /
                                                  1000000.0);
            perf.kahnLevelPackCandidateBuilt = true;
            perf.kahnLevelPackCandidateSegments = candidateSegments.size();
            if (candidateSegments.size() > segments.size())
            {
                ++perf.kahnLevelPackRejectedSegmentCount;
                return true;
            }
            if (!validateComputeSupernodeSegments(candidateView,
                                                  candidateSegments,
                                                  nodeOpSizes,
                                                  maxOps))
            {
                ++perf.kahnLevelPackRejectedSegmentShape;
                return true;
            }
            const PostDpPartitionMetrics candidateMetrics =
                recountPostDpPartitionMetrics(candidateView,
                                              candidateValueEdges,
                                              candidateSegments);
            perf.kahnLevelPackCandidateComputeBae = candidateMetrics.computeBae;
            perf.kahnLevelPackCandidateDagEdges = candidateMetrics.dagEdges;
            if (!partitionPolicyAccepts(options.kahnLevelPackPolicy,
                                        baselineMetrics,
                                        baselineMetrics,
                                        candidateMetrics,
                                        options.kahnLevelPackMaxRegressionPpm))
            {
                ++perf.kahnLevelPackRejectedPolicy;
                return true;
            }

            clusters = std::move(candidateClusters);
            view = std::move(candidateView);
            valueEdges = std::move(candidateValueEdges);
            segments = std::move(candidateSegments);
            perf.kahnLevelPackAfterComputeBae = candidateMetrics.computeBae;
            perf.kahnLevelPackAfterDagEdges = candidateMetrics.dagEdges;
            perf.kahnLevelPackAdopted = true;
            return true;
        }

        bool refinePostDpSegments(const NodeClusterView &view,
                                  const ClusterValueEdges &valueEdges,
                                  const std::vector<uint32_t> &nodeOpSizes,
                                  const ComputeRewriteBuild &rewrite,
                                  std::vector<std::vector<uint32_t>> &segments,
                                  const ActivityScheduleOptions &options,
                                  std::size_t maxOps,
                                  std::size_t splitNodeMaxOps,
                                  ComputeNodeMaterializePerfStats &perf,
                                  std::string &error)
        {
            if (options.postDpRefinePolicy == "off" || segments.empty())
            {
                return true;
            }

            std::vector<uint32_t> ownerByCluster = postDpSegmentOwners(view, segments);
            if (std::find(ownerByCluster.begin(),
                          ownerByCluster.end(),
                          kInvalidActivitySupernodeId) != ownerByCluster.end())
            {
                error = "activity-schedule post-DP refine found an unowned cluster";
                return false;
            }

            std::vector<std::size_t> clusterOps(view.members.size(), 0);
            std::vector<std::size_t> segmentOps(segments.size(), 0);
            std::size_t totalOps = 0;
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                clusterOps[clusterId] = clusterOpSize(view.members[clusterId], nodeOpSizes);
                totalOps += clusterOps[clusterId];
                segmentOps[ownerByCluster[clusterId]] += clusterOps[clusterId];
            }

            std::vector<uint8_t> locked(view.members.size(), 0);
            std::vector<uint8_t> splitSensitive(view.members.size(), 0);
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                bool lock = clusterOps[clusterId] > maxOps;
                for (const uint32_t nodeId : view.members[clusterId])
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        lock = true;
                        break;
                    }
                    const auto &node = rewrite.computeNodes[nodeId];
                    if (node.indivisible || !node.intentGroup.empty())
                    {
                        lock = true;
                    }
                    if (options.splitOversizeComputeNodes && splitNodeMaxOps != 0 &&
                        node.ops.size() > splitNodeMaxOps)
                    {
                        lock = true;
                        splitSensitive[clusterId] = 1U;
                    }
                }
                locked[clusterId] = lock ? 1U : 0U;
            }
            std::vector<uint8_t> splitSensitiveSegments(segments.size(), 0);
            for (uint32_t clusterId = 0; clusterId < splitSensitive.size(); ++clusterId)
            {
                if (splitSensitive[clusterId] != 0 &&
                    ownerByCluster[clusterId] < splitSensitiveSegments.size())
                {
                    splitSensitiveSegments[ownerByCluster[clusterId]] = 1U;
                }
            }
            for (uint32_t clusterId = 0; clusterId < locked.size(); ++clusterId)
            {
                if (ownerByCluster[clusterId] < splitSensitiveSegments.size() &&
                    splitSensitiveSegments[ownerByCluster[clusterId]] != 0)
                {
                    locked[clusterId] = 1U;
                }
            }
            for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
            {
                if (clusterOps[clusterId] <= maxOps && splitSensitive[clusterId] == 0)
                {
                    continue;
                }
                for (const uint32_t pred : view.preds[clusterId])
                {
                    if (pred < locked.size())
                    {
                        locked[pred] = 1U;
                    }
                }
                for (const uint32_t succ : view.succs[clusterId])
                {
                    if (succ < locked.size())
                    {
                        locked[succ] = 1U;
                    }
                }
            }
            perf.postDpRefineLockedClusters =
                std::count(locked.begin(), locked.end(), uint8_t{1});

            const std::size_t movedOpBudget = static_cast<std::size_t>(
                (static_cast<unsigned __int128>(totalOps) *
                 options.postDpRefineMaxMovedOpPpm) /
                1000000U);
            std::vector<uint8_t> moved(view.members.size(), 0);
            std::size_t movedClusters = 0;

            const PostDpPartitionMetrics baselineMetrics =
                recountPostDpPartitionMetrics(view, valueEdges, segments);
            std::size_t currentBae = baselineMetrics.computeBae;
            PostDpDagState dagState =
                buildPostDpDagState(view, valueEdges, ownerByCluster, segments.size());
            const std::size_t baselineBae = baselineMetrics.computeBae;
            const std::size_t baselineDag = baselineMetrics.dagEdges;
            if (dagState.edges != baselineDag)
            {
                error = "activity-schedule post-DP refine baseline recount mismatch";
                return false;
            }
            perf.postDpRefineBeforeComputeBae = baselineBae;
            perf.postDpRefineBeforeDagEdges = baselineDag;
            perf.postDpRefineEvaluated = true;

            std::vector<uint32_t> valueSeen(valueEdges.valueFanouts.size(), 0);
            uint32_t valueStamp = 0;
            const auto nextStamp = [](std::vector<uint32_t> &seen, uint32_t &stamp)
            {
                ++stamp;
                if (stamp == 0)
                {
                    std::fill(seen.begin(), seen.end(), 0);
                    stamp = 1;
                }
                return stamp;
            };

            std::unordered_map<uint64_t, uint32_t> valueTargetSegmentRefs;
            valueTargetSegmentRefs.reserve(valueEdges.weights.size() * 2 + 1);
            std::vector<std::size_t> valueTargetDistinct(valueEdges.valueFanouts.size(), 0);
            for (uint32_t valueId = 0; valueId < valueEdges.valueFanouts.size(); ++valueId)
            {
                for (const uint32_t targetCluster : valueEdges.valueFanouts[valueId].targetClusters)
                {
                    if (targetCluster >= ownerByCluster.size())
                    {
                        continue;
                    }
                    const uint32_t targetSegment = ownerByCluster[targetCluster];
                    auto [it, inserted] = valueTargetSegmentRefs.try_emplace(
                        packClusterPair(valueId, targetSegment),
                        0);
                    ++it->second;
                    if (inserted)
                    {
                        ++valueTargetDistinct[valueId];
                    }
                }
            }

            const auto valueTargetSegmentCount = [&](uint32_t valueId, uint32_t segmentId)
            {
                if (segmentId == kInvalidActivitySupernodeId)
                {
                    return uint32_t{0};
                }
                const auto it = valueTargetSegmentRefs.find(packClusterPair(valueId, segmentId));
                return it == valueTargetSegmentRefs.end() ? uint32_t{0} : it->second;
            };

            const auto applyTargetClusterMove = [&](uint32_t clusterId,
                                                    uint32_t from,
                                                    uint32_t to)
            {
                if (from == to || clusterId >= valueEdges.targetValuesByCluster.size())
                {
                    return;
                }
                for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                {
                    const uint64_t fromKey = packClusterPair(valueId, from);
                    const auto fromIt = valueTargetSegmentRefs.find(fromKey);
                    if (fromIt != valueTargetSegmentRefs.end())
                    {
                        if (--fromIt->second == 0)
                        {
                            valueTargetSegmentRefs.erase(fromIt);
                            --valueTargetDistinct[valueId];
                        }
                    }
                    const uint64_t toKey = packClusterPair(valueId, to);
                    auto [toIt, inserted] = valueTargetSegmentRefs.try_emplace(toKey, 0);
                    ++toIt->second;
                    if (inserted)
                    {
                        ++valueTargetDistinct[valueId];
                    }
                }
            };

            const uint32_t clusterCount = static_cast<uint32_t>(view.members.size());
            std::unordered_map<uint64_t, uint32_t> commitSegmentRefs;
            std::size_t commitCount = 0;
            for (uint32_t clusterId = 0;
                 clusterId < valueEdges.commitSuccsByCluster.size();
                 ++clusterId)
            {
                for (const uint32_t rawCommit : valueEdges.commitSuccsByCluster[clusterId])
                {
                    if (rawCommit < clusterCount)
                    {
                        continue;
                    }
                    const uint32_t commitId = rawCommit - clusterCount;
                    commitCount = std::max(commitCount,
                                           static_cast<std::size_t>(commitId) + 1);
                    ++commitSegmentRefs[packClusterPair(commitId,
                                                        ownerByCluster[clusterId])];
                }
            }
            const auto applyCommitClusterMove = [&](uint32_t clusterId,
                                                    uint32_t from,
                                                    uint32_t to)
            {
                if (from == to || clusterId >= valueEdges.commitSuccsByCluster.size())
                {
                    return;
                }
                for (const uint32_t rawCommit : valueEdges.commitSuccsByCluster[clusterId])
                {
                    if (rawCommit < clusterCount)
                    {
                        continue;
                    }
                    const uint32_t commitId = rawCommit - clusterCount;
                    const uint64_t fromKey = packClusterPair(commitId, from);
                    const auto fromIt = commitSegmentRefs.find(fromKey);
                    if (fromIt != commitSegmentRefs.end() && --fromIt->second == 0)
                    {
                        commitSegmentRefs.erase(fromIt);
                    }
                    ++commitSegmentRefs[packClusterPair(commitId, to)];
                }
            };

            struct ProposalEval
            {
                std::int64_t baeGain = 0;
                std::int64_t dagGain = 0;
                std::vector<std::pair<uint64_t, int32_t>> dagRefDeltas;
            };
            std::vector<uint64_t> logicalEdgeScratch;
            std::vector<std::pair<uint64_t, int32_t>> pairDeltaScratch;

            const auto ownerAfter = [&](uint32_t clusterId,
                                        uint32_t lhs,
                                        uint32_t lhsTo,
                                        uint32_t rhs,
                                        uint32_t rhsTo)
            {
                if (clusterId == lhs)
                {
                    return lhsTo;
                }
                if (clusterId == rhs)
                {
                    return rhsTo;
                }
                return clusterId < ownerByCluster.size()
                           ? ownerByCluster[clusterId]
                           : kInvalidActivitySupernodeId;
            };

            const auto valueTargetCountAfter = [&](uint32_t valueId,
                                                   uint32_t lhs,
                                                   uint32_t lhsTo,
                                                   uint32_t rhs,
                                                   uint32_t rhsTo)
            {
                if (valueId >= valueEdges.valueFanouts.size())
                {
                    return std::size_t{0};
                }
                const auto &fanout = valueEdges.valueFanouts[valueId];
                std::pair<uint32_t, int32_t> deltas[4];
                std::size_t deltaCount = 0;
                const auto addDelta = [&](uint32_t segmentId, int32_t delta)
                {
                    if (segmentId == kInvalidActivitySupernodeId)
                    {
                        return;
                    }
                    for (std::size_t i = 0; i < deltaCount; ++i)
                    {
                        if (deltas[i].first == segmentId)
                        {
                            deltas[i].second += delta;
                            return;
                        }
                    }
                    deltas[deltaCount++] = {segmentId, delta};
                };
                const auto noteTargetMove = [&](uint32_t clusterId, uint32_t destination)
                {
                    if (clusterId == kInvalidActivitySupernodeId ||
                        clusterId >= ownerByCluster.size() ||
                        !std::binary_search(fanout.targetClusters.begin(),
                                            fanout.targetClusters.end(),
                                            clusterId))
                    {
                        return;
                    }
                    addDelta(ownerByCluster[clusterId], -1);
                    addDelta(destination, 1);
                };
                noteTargetMove(lhs, lhsTo);
                noteTargetMove(rhs, rhsTo);

                std::int64_t distinct =
                    static_cast<std::int64_t>(valueTargetDistinct[valueId]);
                for (std::size_t i = 0; i < deltaCount; ++i)
                {
                    const std::int64_t before =
                        valueTargetSegmentCount(valueId, deltas[i].first);
                    const std::int64_t after = before + deltas[i].second;
                    distinct += static_cast<std::int64_t>(after > 0) -
                                static_cast<std::int64_t>(before > 0);
                }
                const uint32_t sourceSegment =
                    ownerAfter(fanout.sourceCluster, lhs, lhsTo, rhs, rhsTo);
                if (sourceSegment != kInvalidActivitySupernodeId)
                {
                    std::int64_t sourceTargets =
                        valueTargetSegmentCount(valueId, sourceSegment);
                    for (std::size_t i = 0; i < deltaCount; ++i)
                    {
                        if (deltas[i].first == sourceSegment)
                        {
                            sourceTargets += deltas[i].second;
                            break;
                        }
                    }
                    if (sourceTargets > 0)
                    {
                        --distinct;
                    }
                }
                return distinct < 0 ? std::size_t{0} : static_cast<std::size_t>(distinct);
            };

            const auto evaluateProposal = [&](uint32_t lhs,
                                              uint32_t lhsTo,
                                              uint32_t rhs,
                                              uint32_t rhsTo,
                                              bool keepDagDeltas)
            {
                ProposalEval eval;
                const uint32_t valueVisitStamp = nextStamp(valueSeen, valueStamp);
                const auto visitValue = [&](uint32_t valueId)
                {
                    if (valueId >= valueSeen.size() ||
                        valueSeen[valueId] == valueVisitStamp)
                    {
                        return;
                    }
                    valueSeen[valueId] = valueVisitStamp;
                    const std::size_t before = valueTargetCountAfter(
                        valueId,
                        kInvalidActivitySupernodeId,
                        kInvalidActivitySupernodeId,
                        kInvalidActivitySupernodeId,
                        kInvalidActivitySupernodeId);
                    const std::size_t after =
                        valueTargetCountAfter(valueId, lhs, lhsTo, rhs, rhsTo);
                    eval.baeGain += static_cast<std::int64_t>(before) -
                                    static_cast<std::int64_t>(after);
                };
                const auto visitClusterValues = [&](uint32_t clusterId)
                {
                    if (clusterId < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                        {
                            visitValue(valueId);
                        }
                    }
                    if (clusterId < valueEdges.targetValuesByCluster.size())
                    {
                        for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                        {
                            visitValue(valueId);
                        }
                    }
                };
                visitClusterValues(lhs);
                if (rhs != kInvalidActivitySupernodeId)
                {
                    visitClusterValues(rhs);
                }

                logicalEdgeScratch.clear();
                const auto visitClusterEdges = [&](uint32_t clusterId)
                {
                    if (clusterId >= view.members.size())
                    {
                        return;
                    }
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        logicalEdgeScratch.push_back(packClusterPair(pred, clusterId));
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        logicalEdgeScratch.push_back(packClusterPair(clusterId, succ));
                    }
                    if (clusterId < valueEdges.commitSuccsByCluster.size())
                    {
                        for (const uint32_t rawCommit : valueEdges.commitSuccsByCluster[clusterId])
                        {
                            logicalEdgeScratch.push_back(packClusterPair(clusterId, rawCommit));
                        }
                    }
                };
                visitClusterEdges(lhs);
                if (rhs != kInvalidActivitySupernodeId)
                {
                    visitClusterEdges(rhs);
                }
                std::sort(logicalEdgeScratch.begin(), logicalEdgeScratch.end());
                logicalEdgeScratch.erase(
                    std::unique(logicalEdgeScratch.begin(), logicalEdgeScratch.end()),
                    logicalEdgeScratch.end());

                pairDeltaScratch.clear();
                const uint32_t clusterCount = static_cast<uint32_t>(view.members.size());
                const auto quotientPair = [&](uint32_t fromCluster,
                                              uint32_t logicalTo,
                                              bool after) -> std::optional<uint64_t>
                {
                    const uint32_t from =
                        after ? ownerAfter(fromCluster, lhs, lhsTo, rhs, rhsTo)
                              : ownerByCluster[fromCluster];
                    uint32_t to = kInvalidActivitySupernodeId;
                    if (logicalTo < clusterCount)
                    {
                        to = after ? ownerAfter(logicalTo, lhs, lhsTo, rhs, rhsTo)
                                   : ownerByCluster[logicalTo];
                    }
                    else
                    {
                        to = static_cast<uint32_t>(segments.size()) +
                             (logicalTo - clusterCount);
                    }
                    if (from == kInvalidActivitySupernodeId ||
                        to == kInvalidActivitySupernodeId ||
                        from == to)
                    {
                        return std::nullopt;
                    }
                    return packClusterPair(from, to);
                };
                for (const uint64_t logical : logicalEdgeScratch)
                {
                    const uint32_t fromCluster = static_cast<uint32_t>(logical >> 32);
                    const uint32_t logicalTo = static_cast<uint32_t>(logical);
                    const auto before = quotientPair(fromCluster, logicalTo, false);
                    const auto after = quotientPair(fromCluster, logicalTo, true);
                    if (before == after)
                    {
                        continue;
                    }
                    if (before)
                    {
                        pairDeltaScratch.push_back({*before, -1});
                    }
                    if (after)
                    {
                        pairDeltaScratch.push_back({*after, 1});
                    }
                }
                std::sort(pairDeltaScratch.begin(),
                          pairDeltaScratch.end(),
                          [](const auto &lhsDelta, const auto &rhsDelta)
                          {
                              return lhsDelta.first < rhsDelta.first;
                          });
                std::int64_t afterEdges = static_cast<std::int64_t>(dagState.edges);
                for (std::size_t begin = 0; begin < pairDeltaScratch.size();)
                {
                    const uint64_t pair = pairDeltaScratch[begin].first;
                    int32_t delta = 0;
                    std::size_t end = begin;
                    while (end < pairDeltaScratch.size() &&
                           pairDeltaScratch[end].first == pair)
                    {
                        delta += pairDeltaScratch[end].second;
                        ++end;
                    }
                    if (delta == 0)
                    {
                        begin = end;
                        continue;
                    }
                    const auto it = dagState.refs.find(pair);
                    const std::int64_t beforeCount =
                        it == dagState.refs.end() ? 0 : it->second;
                    const std::int64_t afterCount = beforeCount + delta;
                    if (afterCount < 0)
                    {
                        eval.dagRefDeltas.clear();
                        eval.dagGain = std::numeric_limits<std::int64_t>::min();
                        return eval;
                    }
                    afterEdges += static_cast<std::int64_t>(afterCount > 0) -
                                  static_cast<std::int64_t>(beforeCount > 0);
                    if (keepDagDeltas)
                    {
                        eval.dagRefDeltas.push_back({pair, delta});
                    }
                    begin = end;
                }
                eval.dagGain = static_cast<std::int64_t>(dagState.edges) - afterEdges;
                return eval;
            };

            const auto proposalPreservesTopo = [&](uint32_t lhs,
                                                   uint32_t lhsTo,
                                                   uint32_t rhs,
                                                   uint32_t rhsTo)
            {
                const auto clusterOk = [&](uint32_t clusterId)
                {
                    if (clusterId >= view.members.size())
                    {
                        return false;
                    }
                    const uint32_t segment =
                        ownerAfter(clusterId, lhs, lhsTo, rhs, rhsTo);
                    if (segment == kInvalidActivitySupernodeId)
                    {
                        return false;
                    }
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        const uint32_t predSegment =
                            ownerAfter(pred, lhs, lhsTo, rhs, rhsTo);
                        if (predSegment == kInvalidActivitySupernodeId ||
                            (predSegment != segment && predSegment >= segment))
                        {
                            return false;
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        const uint32_t succSegment =
                            ownerAfter(succ, lhs, lhsTo, rhs, rhsTo);
                        if (succSegment == kInvalidActivitySupernodeId ||
                            (succSegment != segment && segment >= succSegment))
                        {
                            return false;
                        }
                    }
                    return true;
                };
                return clusterOk(lhs) &&
                       (rhs == kInvalidActivitySupernodeId || clusterOk(rhs));
            };

            const auto policyAccepts = [&](const ProposalEval &eval)
            {
                if (eval.dagGain == std::numeric_limits<std::int64_t>::min())
                {
                    return false;
                }
                const std::int64_t nextBae =
                    static_cast<std::int64_t>(currentBae) - eval.baeGain;
                const std::int64_t nextDag =
                    static_cast<std::int64_t>(dagState.edges) - eval.dagGain;
                if (nextBae < 0 || nextDag < 0)
                {
                    return false;
                }
                return partitionPolicyAccepts(
                    options.postDpRefinePolicy,
                    baselineMetrics,
                    PostDpPartitionMetrics{
                        .computeBae = currentBae,
                        .dagEdges = dagState.edges,
                    },
                    PostDpPartitionMetrics{
                        .computeBae = static_cast<std::size_t>(nextBae),
                        .dagEdges = static_cast<std::size_t>(nextDag),
                    },
                    options.postDpRefineMaxRegressionPpm);
            };

            const auto proposalScore = [&](const ProposalEval &eval)
            {
                return static_cast<long double>(eval.baeGain) /
                           std::max<std::size_t>(baselineBae, 1) +
                       static_cast<long double>(eval.dagGain) /
                           std::max<std::size_t>(baselineDag, 1);
            };

            const auto applyDagDeltas = [&](const ProposalEval &eval)
            {
                for (const auto &[pair, delta] : eval.dagRefDeltas)
                {
                    const auto it = dagState.refs.find(pair);
                    const uint32_t before =
                        it == dagState.refs.end() ? 0U : it->second;
                    const std::int64_t after = static_cast<std::int64_t>(before) + delta;
                    if (before == 0 && after > 0)
                    {
                        ++dagState.edges;
                    }
                    else if (before > 0 && after == 0)
                    {
                        --dagState.edges;
                    }
                    if (after == 0)
                    {
                        if (it != dagState.refs.end())
                        {
                            dagState.refs.erase(it);
                        }
                    }
                    else
                    {
                        dagState.refs[pair] = static_cast<uint32_t>(after);
                    }
                }
            };

            struct MoveCandidate
            {
                uint32_t cluster = 0;
                uint32_t from = 0;
                uint32_t to = 0;
                std::int64_t baeGain = 0;
                std::int64_t dagGain = 0;
                long double score = 0.0;
            };
            struct BlockedMove
            {
                uint32_t cluster = 0;
                uint32_t from = 0;
                uint32_t to = 0;
                std::size_t affinity = 0;
            };
            struct SwapCandidate
            {
                uint32_t lhs = 0;
                uint32_t rhs = 0;
                uint32_t lhsFrom = 0;
                uint32_t lhsTo = 0;
                std::int64_t baeGain = 0;
                std::int64_t dagGain = 0;
                long double score = 0.0;
            };

            constexpr std::size_t kTopDestinations = 16;
            constexpr std::size_t kTopPeerDestinations = 4;
            constexpr std::size_t kMaxBlockedMoves = 32768;
            constexpr std::size_t kMaxSwapCandidates = 65536;
            constexpr std::size_t kMaxSwapPairScans = 4 * 1024 * 1024;
            struct TopPeerDestinations
            {
                uint32_t segments[kTopPeerDestinations]{};
                uint32_t counts[kTopPeerDestinations]{};
                std::size_t size = 0;
            };
            const auto addTopPeer = [&](TopPeerDestinations &top,
                                        uint32_t segment,
                                        uint32_t count)
            {
                std::size_t position = 0;
                while (position < top.size &&
                       (top.counts[position] > count ||
                        (top.counts[position] == count &&
                         top.segments[position] < segment)))
                {
                    ++position;
                }
                if (position >= kTopPeerDestinations)
                {
                    return;
                }
                const std::size_t nextSize =
                    std::min<std::size_t>(top.size + 1, kTopPeerDestinations);
                for (std::size_t i = nextSize; i > position + 1; --i)
                {
                    top.segments[i - 1] = top.segments[i - 2];
                    top.counts[i - 1] = top.counts[i - 2];
                }
                top.segments[position] = segment;
                top.counts[position] = count;
                top.size = nextSize;
            };

            if (options.postDpRefinePolicy == "swap-probe")
            {
                auto &probe = perf.postDpSwapProbe;
                probe.evaluated = true;
                probe.boundaryValuesBefore =
                    recountPostDpComputeBoundaryValues(valueEdges, ownerByCluster);

                std::vector<TopPeerDestinations> valuePeers(
                    valueEdges.valueFanouts.size());
                for (const auto &[packed, count] : valueTargetSegmentRefs)
                {
                    const uint32_t valueId = static_cast<uint32_t>(packed >> 32);
                    const uint32_t segmentId = static_cast<uint32_t>(packed);
                    if (valueId < valuePeers.size())
                    {
                        addTopPeer(valuePeers[valueId], segmentId, count);
                    }
                }
                std::vector<TopPeerDestinations> commitPeers(commitCount);
                for (const auto &[packed, count] : commitSegmentRefs)
                {
                    const uint32_t commitId = static_cast<uint32_t>(packed >> 32);
                    const uint32_t segmentId = static_cast<uint32_t>(packed);
                    if (commitId < commitPeers.size())
                    {
                        addTopPeer(commitPeers[commitId], segmentId, count);
                    }
                }

                const auto affinityDestinations = [&](uint32_t clusterId)
                {
                    std::unordered_map<uint32_t, std::size_t> affinity;
                    if (clusterId >= ownerByCluster.size())
                    {
                        return std::vector<std::pair<uint32_t, std::size_t>>{};
                    }
                    const uint32_t from = ownerByCluster[clusterId];
                    const auto addDestination = [&](uint32_t destination,
                                                    std::size_t amount)
                    {
                        if (destination != kInvalidActivitySupernodeId &&
                            destination < segments.size() && destination != from &&
                            splitSensitiveSegments[destination] == 0)
                        {
                            affinity[destination] += amount;
                        }
                    };
                    const auto addValuePeers = [&](uint32_t valueId)
                    {
                        if (valueId >= valuePeers.size())
                        {
                            return;
                        }
                        const auto &peers = valuePeers[valueId];
                        for (std::size_t i = 0; i < peers.size; ++i)
                        {
                            addDestination(peers.segments[i], peers.counts[i]);
                        }
                    };
                    if (clusterId < valueEdges.sourceValuesByCluster.size())
                    {
                        for (const uint32_t valueId :
                             valueEdges.sourceValuesByCluster[clusterId])
                        {
                            addValuePeers(valueId);
                        }
                    }
                    if (clusterId < valueEdges.targetValuesByCluster.size())
                    {
                        for (const uint32_t valueId :
                             valueEdges.targetValuesByCluster[clusterId])
                        {
                            if (valueId >= valueEdges.valueFanouts.size())
                            {
                                continue;
                            }
                            const uint32_t sourceCluster =
                                valueEdges.valueFanouts[valueId].sourceCluster;
                            if (sourceCluster < ownerByCluster.size())
                            {
                                addDestination(ownerByCluster[sourceCluster], 1);
                            }
                            addValuePeers(valueId);
                        }
                    }
                    if (clusterId < valueEdges.commitSuccsByCluster.size())
                    {
                        for (const uint32_t rawCommit :
                             valueEdges.commitSuccsByCluster[clusterId])
                        {
                            if (rawCommit < clusterCount)
                            {
                                continue;
                            }
                            const uint32_t commitId = rawCommit - clusterCount;
                            if (commitId >= commitPeers.size())
                            {
                                continue;
                            }
                            const auto &peers = commitPeers[commitId];
                            for (std::size_t i = 0; i < peers.size; ++i)
                            {
                                addDestination(peers.segments[i], peers.counts[i]);
                            }
                        }
                    }
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        if (pred < ownerByCluster.size())
                        {
                            addDestination(ownerByCluster[pred], 1);
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        if (succ < ownerByCluster.size())
                        {
                            addDestination(ownerByCluster[succ], 1);
                        }
                    }
                    std::vector<std::pair<uint32_t, std::size_t>> destinations(
                        affinity.begin(), affinity.end());
                    std::sort(destinations.begin(),
                              destinations.end(),
                              [](const auto &lhs, const auto &rhs)
                              {
                                  if (lhs.second != rhs.second)
                                  {
                                      return lhs.second > rhs.second;
                                  }
                                  return lhs.first < rhs.first;
                              });
                    if (destinations.size() > kTopDestinations)
                    {
                        destinations.resize(kTopDestinations);
                    }
                    return destinations;
                };

                const auto blockedMoveBetter = [](const BlockedMove &lhs,
                                                  const BlockedMove &rhs)
                {
                    if (lhs.affinity != rhs.affinity)
                    {
                        return lhs.affinity > rhs.affinity;
                    }
                    if (lhs.cluster != rhs.cluster)
                    {
                        return lhs.cluster < rhs.cluster;
                    }
                    if (lhs.to != rhs.to)
                    {
                        return lhs.to < rhs.to;
                    }
                    return lhs.from < rhs.from;
                };
                std::set<BlockedMove, decltype(blockedMoveBetter)> blockedMoveTop(
                    blockedMoveBetter);
                if (options.postDpRefineMaxRounds != 0)
                {
                    for (uint32_t lhs = 0; lhs < view.members.size(); ++lhs)
                    {
                        if (locked[lhs] != 0)
                        {
                            continue;
                        }
                        const uint32_t from = ownerByCluster[lhs];
                        if (from >= segments.size())
                        {
                            continue;
                        }
                        for (const auto &[to, affinity] : affinityDestinations(lhs))
                        {
                            if (segments[from].size() > 1 &&
                                segmentOps[to] + clusterOps[lhs] <= maxOps)
                            {
                                continue;
                            }
                            ++probe.capacityBlockedSeeds;
                            const BlockedMove candidate{lhs, from, to, affinity};
                            if (blockedMoveTop.size() < kMaxBlockedMoves)
                            {
                                blockedMoveTop.insert(candidate);
                                continue;
                            }
                            ++probe.rejectedScanLimit;
                            const auto worst = std::prev(blockedMoveTop.end());
                            if (blockedMoveBetter(candidate, *worst))
                            {
                                blockedMoveTop.erase(worst);
                                blockedMoveTop.insert(candidate);
                            }
                        }
                    }
                }
                const std::vector<BlockedMove> blockedMoves(blockedMoveTop.begin(),
                                                            blockedMoveTop.end());

                struct SwapProbeCandidate
                {
                    uint32_t lhs = 0;
                    uint32_t rhs = 0;
                    uint32_t lhsFrom = 0;
                    uint32_t lhsTo = 0;
                    std::size_t clusterOps = 0;
                    std::size_t affinity = 0;
                    std::size_t baeGain = 0;
                };
                const auto proposalPreservesDagSupport = [&](const ProposalEval &eval)
                {
                    if (eval.dagGain == std::numeric_limits<std::int64_t>::min())
                    {
                        return false;
                    }
                    for (const auto &[pair, delta] : eval.dagRefDeltas)
                    {
                        const auto it = dagState.refs.find(pair);
                        const std::int64_t before =
                            it == dagState.refs.end() ? 0 : it->second;
                        const std::int64_t after = before + delta;
                        if (after < 0 || (before > 0) != (after > 0))
                        {
                            return false;
                        }
                    }
                    return true;
                };

                const auto swapCandidateBetter = [](const SwapProbeCandidate &lhs,
                                                    const SwapProbeCandidate &rhs)
                {
                    if (lhs.baeGain != rhs.baeGain)
                    {
                        return lhs.baeGain > rhs.baeGain;
                    }
                    if (lhs.affinity != rhs.affinity)
                    {
                        return lhs.affinity > rhs.affinity;
                    }
                    if (lhs.clusterOps != rhs.clusterOps)
                    {
                        return lhs.clusterOps < rhs.clusterOps;
                    }
                    if (lhs.lhs != rhs.lhs)
                    {
                        return lhs.lhs < rhs.lhs;
                    }
                    return lhs.rhs < rhs.rhs;
                };
                std::set<SwapProbeCandidate, decltype(swapCandidateBetter)> eligibleTop(
                    swapCandidateBetter);
                std::unordered_set<uint64_t> seenClusterPairs;
                seenClusterPairs.reserve(kMaxSwapCandidates * 2);
                std::size_t totalRhsPairs = 0;
                for (const auto &blocked : blockedMoves)
                {
                    if (blocked.to >= segments.size())
                    {
                        continue;
                    }
                    const std::size_t rhsCount = segments[blocked.to].size();
                    if (rhsCount > std::numeric_limits<std::size_t>::max() - totalRhsPairs)
                    {
                        totalRhsPairs = std::numeric_limits<std::size_t>::max();
                        break;
                    }
                    totalRhsPairs += rhsCount;
                }
                bool pairScanLimitReached = false;
                for (const auto &blocked : blockedMoves)
                {
                    if (blocked.cluster >= ownerByCluster.size() ||
                        blocked.to >= segments.size())
                    {
                        continue;
                    }
                    for (const uint32_t rhs : segments[blocked.to])
                    {
                        if (probe.enumeratedRhs >= kMaxSwapPairScans)
                        {
                            pairScanLimitReached = true;
                            break;
                        }
                        ++probe.enumeratedRhs;
                        if (rhs >= ownerByCluster.size() || rhs == blocked.cluster ||
                            locked[rhs] != 0 || splitSensitive[rhs] != 0)
                        {
                            ++probe.rejectedLocked;
                            continue;
                        }
                        if (clusterOps[blocked.cluster] != clusterOps[rhs])
                        {
                            ++probe.rejectedEqualLoad;
                            continue;
                        }
                        if (!proposalPreservesTopo(blocked.cluster,
                                                   blocked.to,
                                                   rhs,
                                                   blocked.from))
                        {
                            ++probe.rejectedTopo;
                            continue;
                        }
                        ProposalEval eval = evaluateProposal(blocked.cluster,
                                                             blocked.to,
                                                             rhs,
                                                             blocked.from,
                                                             true);
                        if (!proposalPreservesDagSupport(eval))
                        {
                            ++probe.rejectedDagSupport;
                            if (eval.dagGain == 0)
                            {
                                ++probe.rejectedDagSupportKey;
                            }
                            else
                            {
                                ++probe.rejectedDagEdgeCount;
                            }
                            continue;
                        }
                        if (eval.baeGain <= 0)
                        {
                            ++probe.rejectedNonpositiveBae;
                            continue;
                        }
                        const auto [pairLhs, pairRhs] =
                            std::minmax(blocked.cluster, rhs);
                        if (!seenClusterPairs.insert(packClusterPair(pairLhs, pairRhs)).second)
                        {
                            continue;
                        }
                        const SwapProbeCandidate candidate{
                            blocked.cluster,
                            rhs,
                            blocked.from,
                            blocked.to,
                            clusterOps[blocked.cluster],
                            blocked.affinity,
                            static_cast<std::size_t>(eval.baeGain),
                        };
                        if (eligibleTop.size() < kMaxSwapCandidates)
                        {
                            eligibleTop.insert(candidate);
                            continue;
                        }
                        ++probe.rejectedScanLimit;
                        const auto worst = std::prev(eligibleTop.end());
                        if (swapCandidateBetter(candidate, *worst))
                        {
                            eligibleTop.erase(worst);
                            eligibleTop.insert(candidate);
                        }
                    }
                    if (pairScanLimitReached)
                    {
                        break;
                    }
                }
                if (totalRhsPairs > probe.enumeratedRhs)
                {
                    const std::size_t skippedRhs = totalRhsPairs - probe.enumeratedRhs;
                    if (skippedRhs > std::numeric_limits<std::size_t>::max() -
                                         probe.rejectedScanLimit)
                    {
                        probe.rejectedScanLimit =
                            std::numeric_limits<std::size_t>::max();
                    }
                    else
                    {
                        probe.rejectedScanLimit += skippedRhs;
                    }
                }
                const std::vector<SwapProbeCandidate> eligible(eligibleTop.begin(),
                                                               eligibleTop.end());
                probe.rawEligible = eligible.size();
                for (const auto &candidate : eligible)
                {
                    probe.eligibleBaeGain += candidate.baeGain;
                    ++probe.eligibleByClusterOps[candidate.clusterOps];
                }
                perf.postDpRefineCandidates = probe.rawEligible;

                std::vector<uint8_t> selectedClusters(view.members.size(), 0U);
                std::vector<uint8_t> selectedSegments(segments.size(), 0U);
                std::vector<SwapProbeCandidate> selected;
                for (const auto &candidate : eligible)
                {
                    if (selectedClusters[candidate.lhs] != 0 ||
                        selectedClusters[candidate.rhs] != 0 ||
                        selectedSegments[candidate.lhsFrom] != 0 ||
                        selectedSegments[candidate.lhsTo] != 0)
                    {
                        ++probe.rejectedConflict;
                        continue;
                    }
                    const std::size_t movedOps = candidate.clusterOps * 2;
                    if (probe.selectedMovedClusters + 2 >
                            options.postDpRefineMaxMoves ||
                        probe.selectedMovedOps + movedOps > movedOpBudget)
                    {
                        ++probe.rejectedBudget;
                        continue;
                    }
                    selected.push_back(candidate);
                    selectedClusters[candidate.lhs] = 1U;
                    selectedClusters[candidate.rhs] = 1U;
                    selectedSegments[candidate.lhsFrom] = 1U;
                    selectedSegments[candidate.lhsTo] = 1U;
                    probe.selectedMovedClusters += 2;
                    probe.selectedMovedOps += movedOps;
                    probe.projectedBaeGain += candidate.baeGain;
                }
                probe.selected = selected.size();

                std::vector<std::vector<uint32_t>> candidateSegments = segments;
                for (const auto &candidate : selected)
                {
                    auto &fromMembers = candidateSegments[candidate.lhsFrom];
                    auto &toMembers = candidateSegments[candidate.lhsTo];
                    const auto lhsIt =
                        std::find(fromMembers.begin(), fromMembers.end(), candidate.lhs);
                    const auto rhsIt =
                        std::find(toMembers.begin(), toMembers.end(), candidate.rhs);
                    if (lhsIt == fromMembers.end() || rhsIt == toMembers.end())
                    {
                        continue;
                    }
                    *lhsIt = candidate.rhs;
                    *rhsIt = candidate.lhs;
                    std::sort(fromMembers.begin(), fromMembers.end());
                    std::sort(toMembers.begin(), toMembers.end());
                }

                const std::vector<uint32_t> candidateOwners =
                    postDpSegmentOwners(view, candidateSegments);
                probe.computeBaeCandidate = recountPostDpComputeBae(
                    valueEdges, candidateOwners, candidateSegments.size());
                probe.boundaryValuesCandidate =
                    recountPostDpComputeBoundaryValues(valueEdges, candidateOwners);
                const PostDpDagState candidateDag = buildPostDpDagState(
                    view, valueEdges, candidateOwners, candidateSegments.size());
                probe.dagEdgesCandidate = candidateDag.edges;
                probe.segmentCountValid = candidateSegments.size() == segments.size();
                std::vector<std::size_t> candidateSegmentOps(candidateSegments.size(), 0);
                for (uint32_t segmentId = 0; segmentId < candidateSegments.size(); ++segmentId)
                {
                    for (const uint32_t clusterId : candidateSegments[segmentId])
                    {
                        if (clusterId < clusterOps.size())
                        {
                            candidateSegmentOps[segmentId] += clusterOps[clusterId];
                        }
                    }
                }
                probe.segmentOpsValid = candidateSegmentOps == segmentOps;
                probe.segmentShapeValid = validateComputeSupernodeSegments(
                    view, candidateSegments, nodeOpSizes, maxOps);
                probe.dagSupportValid = equalPostDpDagSupport(dagState, candidateDag);
                probe.topoValid =
                    std::find(candidateOwners.begin(),
                              candidateOwners.end(),
                              kInvalidActivitySupernodeId) == candidateOwners.end();
                if (probe.topoValid)
                {
                    for (uint32_t fromCluster = 0;
                         fromCluster < view.succs.size() && probe.topoValid;
                         ++fromCluster)
                    {
                        for (const uint32_t toCluster : view.succs[fromCluster])
                        {
                            if (fromCluster >= candidateOwners.size() ||
                                toCluster >= candidateOwners.size())
                            {
                                probe.topoValid = false;
                                break;
                            }
                            const uint32_t from = candidateOwners[fromCluster];
                            const uint32_t to = candidateOwners[toCluster];
                            if (from != to && from >= to)
                            {
                                probe.topoValid = false;
                                break;
                            }
                        }
                    }
                }
                if (probe.computeBaeCandidate <= baselineBae)
                {
                    probe.actualBaeGain = baselineBae - probe.computeBaeCandidate;
                }
                probe.projectedActualValid =
                    probe.actualBaeGain == probe.projectedBaeGain;
                probe.valid = probe.segmentCountValid && probe.segmentOpsValid &&
                              probe.segmentShapeValid && probe.dagSupportValid &&
                              probe.topoValid && probe.projectedActualValid;

                perf.postDpRefineRounds =
                    options.postDpRefineMaxRounds == 0 ? 0 : 1;
                perf.postDpRefineAfterComputeBae = baselineBae;
                perf.postDpRefineAfterDagEdges = baselineDag;
                return true;
            }

            for (std::size_t round = 0;
                 round < options.postDpRefineMaxRounds &&
                 movedClusters < options.postDpRefineMaxMoves &&
                 perf.postDpRefineMovedOps < movedOpBudget;
                 ++round)
            {
                std::vector<TopPeerDestinations> valuePeers(
                    valueEdges.valueFanouts.size());
                for (const auto &[packed, count] : valueTargetSegmentRefs)
                {
                    const uint32_t valueId = static_cast<uint32_t>(packed >> 32);
                    const uint32_t segmentId = static_cast<uint32_t>(packed);
                    if (valueId < valuePeers.size())
                    {
                        addTopPeer(valuePeers[valueId], segmentId, count);
                    }
                }
                std::vector<TopPeerDestinations> commitPeers(commitCount);
                for (const auto &[packed, count] : commitSegmentRefs)
                {
                    const uint32_t commitId = static_cast<uint32_t>(packed >> 32);
                    const uint32_t segmentId = static_cast<uint32_t>(packed);
                    if (commitId < commitPeers.size())
                    {
                        addTopPeer(commitPeers[commitId], segmentId, count);
                    }
                }
                std::vector<MoveCandidate> candidates;
                std::vector<BlockedMove> blockedMoves;
                for (uint32_t clusterId = 0; clusterId < view.members.size(); ++clusterId)
                {
                    if (locked[clusterId] != 0 || moved[clusterId] != 0)
                    {
                        continue;
                    }
                    const uint32_t from = ownerByCluster[clusterId];
                    if (from >= segments.size())
                    {
                        continue;
                    }
                    std::optional<MoveCandidate> bestCandidate;
                    std::optional<BlockedMove> bestBlockedMove;
                    std::unordered_map<uint32_t, std::size_t> affinity;
                    const auto addDestination = [&](uint32_t destination, std::size_t amount)
                    {
                        if (destination != kInvalidActivitySupernodeId &&
                            destination < segments.size() && destination != from &&
                            splitSensitiveSegments[destination] == 0)
                        {
                            affinity[destination] += amount;
                        }
                    };
                    const auto addValuePeerDestinations = [&](uint32_t valueId)
                    {
                        if (valueId >= valuePeers.size())
                        {
                            return;
                        }
                        const auto &peers = valuePeers[valueId];
                        for (std::size_t i = 0; i < peers.size; ++i)
                        {
                            addDestination(peers.segments[i], peers.counts[i]);
                        }
                    };
                    for (const uint32_t valueId : valueEdges.sourceValuesByCluster[clusterId])
                    {
                        addValuePeerDestinations(valueId);
                    }
                    for (const uint32_t valueId : valueEdges.targetValuesByCluster[clusterId])
                    {
                        if (valueId >= valueEdges.valueFanouts.size())
                        {
                            continue;
                        }
                        const uint32_t sourceCluster =
                            valueEdges.valueFanouts[valueId].sourceCluster;
                        if (sourceCluster < ownerByCluster.size())
                        {
                            addDestination(ownerByCluster[sourceCluster], 1);
                        }
                        addValuePeerDestinations(valueId);
                    }
                    if (clusterId < valueEdges.commitSuccsByCluster.size())
                    {
                        for (const uint32_t rawCommit :
                             valueEdges.commitSuccsByCluster[clusterId])
                        {
                            if (rawCommit < clusterCount)
                            {
                                continue;
                            }
                            const uint32_t commitId = rawCommit - clusterCount;
                            if (commitId >= commitPeers.size())
                            {
                                continue;
                            }
                            const auto &peers = commitPeers[commitId];
                            for (std::size_t i = 0; i < peers.size; ++i)
                            {
                                addDestination(peers.segments[i], peers.counts[i]);
                            }
                        }
                    }
                    for (const uint32_t pred : view.preds[clusterId])
                    {
                        if (pred < ownerByCluster.size())
                        {
                            addDestination(ownerByCluster[pred], 1);
                        }
                    }
                    for (const uint32_t succ : view.succs[clusterId])
                    {
                        if (succ < ownerByCluster.size())
                        {
                            addDestination(ownerByCluster[succ], 1);
                        }
                    }
                    std::vector<std::pair<uint32_t, std::size_t>> destinations(
                        affinity.begin(), affinity.end());
                    std::sort(destinations.begin(),
                              destinations.end(),
                              [](const auto &lhs, const auto &rhs)
                              {
                                  if (lhs.second != rhs.second)
                                  {
                                      return lhs.second > rhs.second;
                                  }
                                  return lhs.first < rhs.first;
                              });
                    if (destinations.size() > kTopDestinations)
                    {
                        destinations.resize(kTopDestinations);
                    }
                    for (const auto &[to, score] : destinations)
                    {
                        if (!proposalPreservesTopo(clusterId,
                                                   to,
                                                   kInvalidActivitySupernodeId,
                                                   kInvalidActivitySupernodeId))
                        {
                            ++perf.postDpRefineRejectedTopo;
                            continue;
                        }
                        ProposalEval eval = evaluateProposal(
                            clusterId,
                            to,
                            kInvalidActivitySupernodeId,
                            kInvalidActivitySupernodeId,
                            false);
                        if (!policyAccepts(eval))
                        {
                            ++perf.postDpRefineRejectedPolicy;
                            continue;
                        }
                        if (segments[from].size() <= 1 ||
                            segmentOps[to] + clusterOps[clusterId] > maxOps)
                        {
                            ++perf.postDpRefineRejectedCapacity;
                            BlockedMove blocked{clusterId, from, to, score};
                            if (!bestBlockedMove ||
                                blocked.affinity > bestBlockedMove->affinity ||
                                (blocked.affinity == bestBlockedMove->affinity &&
                                 blocked.to < bestBlockedMove->to))
                            {
                                bestBlockedMove = blocked;
                            }
                            continue;
                        }
                        const long double scoreValue = proposalScore(eval);
                        MoveCandidate candidate{clusterId,
                                                from,
                                                to,
                                                eval.baeGain,
                                                eval.dagGain,
                                                scoreValue};
                        if (!bestCandidate ||
                            candidate.score > bestCandidate->score ||
                            (candidate.score == bestCandidate->score &&
                             (candidate.baeGain > bestCandidate->baeGain ||
                              (candidate.baeGain == bestCandidate->baeGain &&
                               (candidate.dagGain > bestCandidate->dagGain ||
                                (candidate.dagGain == bestCandidate->dagGain &&
                                 candidate.to < bestCandidate->to))))))
                        {
                            bestCandidate = candidate;
                        }
                    }
                    if (bestCandidate)
                    {
                        candidates.push_back(*bestCandidate);
                    }
                    if (bestBlockedMove)
                    {
                        blockedMoves.push_back(*bestBlockedMove);
                    }
                }
                perf.postDpRefineCandidates += candidates.size();
                std::sort(candidates.begin(),
                          candidates.end(),
                          [](const MoveCandidate &lhs, const MoveCandidate &rhs)
                          {
                              if (lhs.score != rhs.score)
                              {
                                  return lhs.score > rhs.score;
                              }
                              if (lhs.baeGain != rhs.baeGain)
                              {
                                  return lhs.baeGain > rhs.baeGain;
                              }
                              if (lhs.dagGain != rhs.dagGain)
                              {
                                  return lhs.dagGain > rhs.dagGain;
                              }
                              if (lhs.cluster != rhs.cluster)
                              {
                                  return lhs.cluster < rhs.cluster;
                              }
                              return lhs.to < rhs.to;
                          });
                std::sort(blockedMoves.begin(),
                          blockedMoves.end(),
                          [](const BlockedMove &lhs, const BlockedMove &rhs)
                          {
                              if (lhs.affinity != rhs.affinity)
                              {
                                  return lhs.affinity > rhs.affinity;
                              }
                              if (lhs.cluster != rhs.cluster)
                              {
                                  return lhs.cluster < rhs.cluster;
                              }
                              return lhs.to < rhs.to;
                          });
                if (blockedMoves.size() > kMaxBlockedMoves)
                {
                    blockedMoves.resize(kMaxBlockedMoves);
                }

                std::size_t acceptedThisRound = 0;
                for (const auto &candidate : candidates)
                {
                    const uint32_t clusterId = candidate.cluster;
                    if (movedClusters >= options.postDpRefineMaxMoves ||
                        moved[clusterId] != 0 ||
                        ownerByCluster[clusterId] != candidate.from ||
                        segmentOps[candidate.to] + clusterOps[clusterId] > maxOps ||
                        segments[candidate.from].size() <= 1 ||
                        perf.postDpRefineMovedOps + clusterOps[clusterId] > movedOpBudget)
                    {
                        continue;
                    }
                    if (!proposalPreservesTopo(clusterId,
                                               candidate.to,
                                               kInvalidActivitySupernodeId,
                                               kInvalidActivitySupernodeId))
                    {
                        ++perf.postDpRefineRejectedTopo;
                        continue;
                    }
                    ProposalEval eval = evaluateProposal(
                        clusterId,
                        candidate.to,
                        kInvalidActivitySupernodeId,
                        kInvalidActivitySupernodeId,
                        true);
                    if (!policyAccepts(eval))
                    {
                        ++perf.postDpRefineRejectedPolicy;
                        continue;
                    }
                    auto &fromMembers = segments[candidate.from];
                    const auto it = std::find(fromMembers.begin(), fromMembers.end(), clusterId);
                    if (it == fromMembers.end())
                    {
                        continue;
                    }
                    fromMembers.erase(it);
                    segments[candidate.to].push_back(clusterId);
                    std::sort(segments[candidate.to].begin(), segments[candidate.to].end());
                    applyTargetClusterMove(clusterId, candidate.from, candidate.to);
                    applyCommitClusterMove(clusterId, candidate.from, candidate.to);
                    ownerByCluster[clusterId] = candidate.to;
                    segmentOps[candidate.from] -= clusterOps[clusterId];
                    segmentOps[candidate.to] += clusterOps[clusterId];
                    applyDagDeltas(eval);
                    currentBae = static_cast<std::size_t>(
                        static_cast<std::int64_t>(currentBae) - eval.baeGain);
                    moved[clusterId] = 1U;
                    ++movedClusters;
                    ++perf.postDpRefineMoves;
                    perf.postDpRefineMovedOps += clusterOps[clusterId];
                    ++acceptedThisRound;
                }

                std::vector<SwapCandidate> swapCandidates;
                for (const auto &blocked : blockedMoves)
                {
                    const uint32_t lhs = blocked.cluster;
                    if (lhs >= ownerByCluster.size() || moved[lhs] != 0 ||
                        ownerByCluster[lhs] != blocked.from ||
                        blocked.to >= segments.size())
                    {
                        continue;
                    }
                    const std::size_t overflow =
                        segmentOps[blocked.to] + clusterOps[lhs] > maxOps
                            ? segmentOps[blocked.to] + clusterOps[lhs] - maxOps
                            : 0;
                    const std::size_t lhsFromAfter =
                        segmentOps[blocked.from] - clusterOps[lhs];
                    for (const uint32_t rhs : segments[blocked.to])
                    {
                        if (rhs >= ownerByCluster.size() || moved[rhs] != 0 ||
                            locked[rhs] != 0 || rhs == lhs ||
                            clusterOps[rhs] < overflow ||
                            lhsFromAfter + clusterOps[rhs] > maxOps)
                        {
                            continue;
                        }
                        if (!proposalPreservesTopo(lhs,
                                                   blocked.to,
                                                   rhs,
                                                   blocked.from))
                        {
                            ++perf.postDpRefineRejectedTopo;
                            continue;
                        }
                        ProposalEval eval = evaluateProposal(
                            lhs,
                            blocked.to,
                            rhs,
                            blocked.from,
                            false);
                        if (!policyAccepts(eval))
                        {
                            ++perf.postDpRefineRejectedPolicy;
                            continue;
                        }
                        const long double scoreValue = proposalScore(eval);
                        swapCandidates.push_back({lhs,
                                                  rhs,
                                                  blocked.from,
                                                  blocked.to,
                                                  eval.baeGain,
                                                  eval.dagGain,
                                                  scoreValue});
                        if (swapCandidates.size() >= kMaxSwapCandidates)
                        {
                            break;
                        }
                    }
                    if (swapCandidates.size() >= kMaxSwapCandidates)
                    {
                        break;
                    }
                }
                perf.postDpRefineCandidates += swapCandidates.size();
                std::sort(swapCandidates.begin(),
                          swapCandidates.end(),
                          [](const SwapCandidate &lhs, const SwapCandidate &rhs)
                          {
                              if (lhs.score != rhs.score)
                              {
                                  return lhs.score > rhs.score;
                              }
                              if (lhs.baeGain != rhs.baeGain)
                              {
                                  return lhs.baeGain > rhs.baeGain;
                              }
                              if (lhs.dagGain != rhs.dagGain)
                              {
                                  return lhs.dagGain > rhs.dagGain;
                              }
                              if (lhs.lhs != rhs.lhs)
                              {
                                  return lhs.lhs < rhs.lhs;
                              }
                              return lhs.rhs < rhs.rhs;
                          });
                for (const auto &candidate : swapCandidates)
                {
                    if (movedClusters + 2 > options.postDpRefineMaxMoves ||
                        moved[candidate.lhs] != 0 || moved[candidate.rhs] != 0 ||
                        ownerByCluster[candidate.lhs] != candidate.lhsFrom ||
                        ownerByCluster[candidate.rhs] != candidate.lhsTo ||
                        perf.postDpRefineMovedOps + clusterOps[candidate.lhs] +
                                clusterOps[candidate.rhs] >
                            movedOpBudget)
                    {
                        continue;
                    }
                    const std::size_t nextFromOps =
                        segmentOps[candidate.lhsFrom] - clusterOps[candidate.lhs] +
                        clusterOps[candidate.rhs];
                    const std::size_t nextToOps =
                        segmentOps[candidate.lhsTo] - clusterOps[candidate.rhs] +
                        clusterOps[candidate.lhs];
                    if (nextFromOps > maxOps || nextToOps > maxOps)
                    {
                        ++perf.postDpRefineRejectedCapacity;
                        continue;
                    }
                    if (!proposalPreservesTopo(candidate.lhs,
                                               candidate.lhsTo,
                                               candidate.rhs,
                                               candidate.lhsFrom))
                    {
                        ++perf.postDpRefineRejectedTopo;
                        continue;
                    }
                    ProposalEval eval = evaluateProposal(candidate.lhs,
                                                         candidate.lhsTo,
                                                         candidate.rhs,
                                                         candidate.lhsFrom,
                                                         true);
                    if (!policyAccepts(eval))
                    {
                        ++perf.postDpRefineRejectedPolicy;
                        continue;
                    }
                    auto &fromMembers = segments[candidate.lhsFrom];
                    auto &toMembers = segments[candidate.lhsTo];
                    const auto lhsIt =
                        std::find(fromMembers.begin(), fromMembers.end(), candidate.lhs);
                    const auto rhsIt =
                        std::find(toMembers.begin(), toMembers.end(), candidate.rhs);
                    if (lhsIt == fromMembers.end() || rhsIt == toMembers.end())
                    {
                        continue;
                    }
                    *lhsIt = candidate.rhs;
                    *rhsIt = candidate.lhs;
                    std::sort(fromMembers.begin(), fromMembers.end());
                    std::sort(toMembers.begin(), toMembers.end());
                    applyTargetClusterMove(candidate.lhs,
                                           candidate.lhsFrom,
                                           candidate.lhsTo);
                    applyTargetClusterMove(candidate.rhs,
                                           candidate.lhsTo,
                                           candidate.lhsFrom);
                    applyCommitClusterMove(candidate.lhs,
                                           candidate.lhsFrom,
                                           candidate.lhsTo);
                    applyCommitClusterMove(candidate.rhs,
                                           candidate.lhsTo,
                                           candidate.lhsFrom);
                    ownerByCluster[candidate.lhs] = candidate.lhsTo;
                    ownerByCluster[candidate.rhs] = candidate.lhsFrom;
                    segmentOps[candidate.lhsFrom] = nextFromOps;
                    segmentOps[candidate.lhsTo] = nextToOps;
                    applyDagDeltas(eval);
                    currentBae = static_cast<std::size_t>(
                        static_cast<std::int64_t>(currentBae) - eval.baeGain);
                    moved[candidate.lhs] = 1U;
                    moved[candidate.rhs] = 1U;
                    movedClusters += 2;
                    ++perf.postDpRefineSwaps;
                    perf.postDpRefineMovedOps +=
                        clusterOps[candidate.lhs] + clusterOps[candidate.rhs];
                    ++acceptedThisRound;
                }

                if (acceptedThisRound == 0)
                {
                    break;
                }
                ++perf.postDpRefineRounds;
                const std::size_t recountedBae =
                    recountPostDpComputeBae(valueEdges, ownerByCluster, segments.size());
                const PostDpDagState recountedDag =
                    buildPostDpDagState(view, valueEdges, ownerByCluster, segments.size());
                if (recountedBae != currentBae ||
                    !equalPostDpDagStates(recountedDag, dagState))
                {
                    error = "activity-schedule post-DP refine exact-delta recount mismatch";
                    return false;
                }
            }

            perf.postDpRefineAfterComputeBae = currentBae;
            perf.postDpRefineAfterDagEdges = dagState.edges;
            return true;
        }

        std::vector<std::vector<uint32_t>> flattenNodeSegments(const NodeClusterView &view,
                                                               const std::vector<std::vector<uint32_t>> &segments,
                                                               const std::vector<uint32_t> &nodeTopoPos)
        {
            std::vector<std::vector<uint32_t>> out;
            out.reserve(segments.size());
            for (const auto &segment : segments)
            {
                std::vector<uint32_t> nodes;
                for (const auto clusterId : segment)
                {
                    if (clusterId < view.members.size())
                    {
                        nodes.insert(nodes.end(), view.members[clusterId].begin(), view.members[clusterId].end());
                    }
                }
                out.push_back(std::move(nodes));
            }
            for (auto &nodes : out)
            {
                std::sort(nodes.begin(),
                          nodes.end(),
                          [&](uint32_t lhs, uint32_t rhs)
                          {
                              const uint32_t lhsPos = lhs < nodeTopoPos.size() ? nodeTopoPos[lhs]
                                                                               : kInvalidActivitySupernodeId;
                              const uint32_t rhsPos = rhs < nodeTopoPos.size() ? nodeTopoPos[rhs]
                                                                               : kInvalidActivitySupernodeId;
                              if (lhsPos != rhsPos)
                              {
                                  return lhsPos < rhsPos;
                              }
                              return lhs < rhs;
                          });
            }
            return out;
        }

        bool topoSortLocalOps(const wolvrix::lib::grh::Graph &graph,
                              const std::vector<wolvrix::lib::grh::OperationId> &ops,
                              std::vector<wolvrix::lib::grh::OperationId> &out,
                              std::string &error)
        {
            out.clear();
            const std::vector<wolvrix::lib::grh::OperationId> uniqueOps =
                uniqueOpsPreservingOrder(ops);
            if (uniqueOps.size() < 2)
            {
                out = uniqueOps;
                return true;
            }

            std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> local;
            local.reserve(uniqueOps.size());
            for (const auto opId : uniqueOps)
            {
                local.insert(opId);
            }

            wolvrix::lib::toposort::TopoDag<wolvrix::lib::grh::OperationId,
                                            wolvrix::lib::grh::OperationIdHash>
                dag;
            dag.reserveNodes(uniqueOps.size());
            for (const auto opId : uniqueOps)
            {
                dag.addNode(opId);
            }
            for (const auto opId : uniqueOps)
            {
                for (const auto operand : graph.opOperands(opId))
                {
                    const auto defOp = graph.valueDef(operand);
                    if (defOp.valid() && local.find(defOp) != local.end() && defOp != opId)
                    {
                        dag.addEdge(defOp, opId);
                    }
                }
            }

            try
            {
                const auto layers = dag.toposort();
                out.reserve(uniqueOps.size());
                for (const auto &layer : layers)
                {
                    std::vector<wolvrix::lib::grh::OperationId> ordered(layer.begin(), layer.end());
                    std::sort(ordered.begin(),
                              ordered.end(),
                              [](const auto lhs, const auto rhs)
                              {
                                  return lhs.index < rhs.index;
                    });
                    out.insert(out.end(), ordered.begin(), ordered.end());
                }
                if (out.size() == uniqueOps.size())
                {
                    return true;
                }
                error = "activity-schedule local op topo failed: missing ops";
                return false;
            }
            catch (const std::exception &ex)
            {
                std::ostringstream oss;
                oss << "activity-schedule local op topo failed: " << ex.what()
                    << " ops=" << uniqueOps.size();
                const std::size_t limit = std::min<std::size_t>(uniqueOps.size(), 12);
                oss << " sample=[";
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        oss << ",";
                    }
                    oss << describeOp(graph, uniqueOps[i]);
                }
                if (uniqueOps.size() > limit)
                {
                    oss << ",...";
                }
                oss << "]";
                error = oss.str();
                return false;
            }
        }

        bool buildComputeNodeRewrite(wolvrix::lib::grh::Graph &graph,
                                     const ActivityScheduleOptions &options,
                                     const ActivityOpData &opData,
                                     std::vector<ActivityOpClass> &opClasses,
                                     const ValueCanonicalMap &canonicalValues,
                                     ComputeRewriteBuild &out,
                                     std::string &error,
                                     const ComputeRewriteBuild *fixedCommitPartition = nullptr)
        {
            out = ComputeRewriteBuild{};
            out.canonicalValues = canonicalValues;
            out.declaredValueComputeNodeBoundary = options.declaredValueComputeNodeBoundary;
            out.computeNodeOfOp.assign(opClasses.size(), kInvalidActivitySupernodeId);
            ComputeNodeBuilder builder(graph, options, opData, opClasses, out, error);

            if (!validateMemoryWritePriorityGroups(graph, error))
            {
                return false;
            }

            std::vector<uint32_t> intentNodeIds;
            for (auto &intentGroup : collectRegToMemIntentComputeGroups(graph, opClasses))
            {
                auto nodeId = builder.createIntentGroupNode(std::move(intentGroup.group), std::move(intentGroup.ops));
                if (!error.empty())
                {
                    return false;
                }
                if (nodeId)
                {
                    intentNodeIds.push_back(*nodeId);
                }
            }
            for (uint32_t nodeId : intentNodeIds)
            {
                if (!builder.processIntentGroupNode(nodeId))
                {
                    return false;
                }
            }

            const std::size_t maxCommitOps = options.maxOpInCommitSupernode;
            std::vector<uint32_t> sinkTopoPositions;
            sinkTopoPositions.reserve(opData.topoOps.size());
            for (uint32_t topoPos = 0; topoPos < opData.topoOps.size(); ++topoPos)
            {
                const auto opId = opData.topoOps[topoPos];
                if (opId.index < opClasses.size() && opClasses[opId.index] == ActivityOpClass::Sink)
                {
                    sinkTopoPositions.push_back(topoPos);
                }
            }
            SinkPartition sinkPartition =
                buildEventClusteredSinkPartition(graph,
                                                 opData,
                                                 sinkTopoPositions,
                                                 maxCommitOps,
                                                 &canonicalValues,
                                                 options.commitGuardEventBuckets);
            if (fixedCommitPartition != nullptr)
            {
                using wolvrix::lib::grh::OperationId;
                using wolvrix::lib::grh::OperationIdHash;

                std::unordered_set<OperationId, OperationIdHash> candidateSinkOps;
                candidateSinkOps.reserve(sinkTopoPositions.size());
                for (const uint32_t topoPos : sinkTopoPositions)
                {
                    candidateSinkOps.insert(opData.topoOps[topoPos]);
                }
                std::unordered_set<OperationId, OperationIdHash> copiedSinkOps;
                copiedSinkOps.reserve(candidateSinkOps.size());
                for (std::size_t nodeIndex = 0;
                     nodeIndex < fixedCommitPartition->commitNodes.size();
                     ++nodeIndex)
                {
                    const auto &copied = fixedCommitPartition->commitNodes[nodeIndex];
                    std::vector<wolvrix::lib::grh::ValueId> rebuiltInputs;
                    for (const OperationId opId : copied.ops)
                    {
                        if (!opId.valid() || opId.graph != graph.id() ||
                            opId.index >= opClasses.size() ||
                            opClasses[opId.index] != ActivityOpClass::Sink ||
                            candidateSinkOps.find(opId) == candidateSinkOps.end())
                        {
                            error = "activity-schedule fixed commit partition contains invalid or non-sink op node=" +
                                    std::to_string(nodeIndex) + " op=" +
                                    std::to_string(opId.index);
                            return false;
                        }
                        if (!copiedSinkOps.insert(opId).second)
                        {
                            error = "activity-schedule fixed commit partition contains duplicate sink op=" +
                                    std::to_string(opId.index);
                            return false;
                        }
                        for (const auto operand : graph.opOperands(opId))
                        {
                            if (!vectorContainsValue(rebuiltInputs, operand))
                            {
                                rebuiltInputs.push_back(operand);
                            }
                        }
                    }
                    if (rebuiltInputs != copied.inputValues)
                    {
                        error = "activity-schedule fixed commit partition input values changed node=" +
                                std::to_string(nodeIndex) + " copied=" +
                                std::to_string(copied.inputValues.size()) + " rebuilt=" +
                                std::to_string(rebuiltInputs.size());
                        return false;
                    }
                }
                if (copiedSinkOps != candidateSinkOps)
                {
                    error = "activity-schedule fixed commit partition does not exactly cover candidate sinks copied=" +
                            std::to_string(copiedSinkOps.size()) + " candidate=" +
                            std::to_string(candidateSinkOps.size());
                    return false;
                }

                const auto normalizedFixedPartition = [&]()
                {
                    std::vector<std::vector<uint32_t>> partition;
                    partition.reserve(fixedCommitPartition->commitNodes.size());
                    for (const auto &node : fixedCommitPartition->commitNodes)
                    {
                        std::vector<uint32_t> ids;
                        ids.reserve(node.ops.size());
                        for (const OperationId opId : node.ops)
                        {
                            ids.push_back(opId.index);
                        }
                        std::sort(ids.begin(), ids.end());
                        partition.push_back(std::move(ids));
                    }
                    std::sort(partition.begin(), partition.end());
                    return partition;
                }();
                const auto normalizedCandidatePartition = [&]()
                {
                    std::vector<std::vector<uint32_t>> partition;
                    partition.reserve(sinkPartition.clusters.size());
                    for (const auto &cluster : sinkPartition.clusters)
                    {
                        std::vector<uint32_t> ids;
                        ids.reserve(cluster.size());
                        for (const uint32_t topoPos : cluster)
                        {
                            ids.push_back(opData.topoOps[topoPos].index);
                        }
                        std::sort(ids.begin(), ids.end());
                        partition.push_back(std::move(ids));
                    }
                    std::sort(partition.begin(), partition.end());
                    return partition;
                }();
                if (normalizedFixedPartition != normalizedCandidatePartition)
                {
                    error = "activity-schedule fixed commit partition violates candidate cap/event grouping";
                    return false;
                }

                out.commitNodes = fixedCommitPartition->commitNodes;
                out.stats.commitSinkOps = fixedCommitPartition->stats.commitSinkOps;
                out.stats.commitEventKeyRuns = fixedCommitPartition->stats.commitEventKeyRuns;
                out.stats.commitEventKeys = fixedCommitPartition->stats.commitEventKeys;
                out.stats.commitInputRootValues =
                    fixedCommitPartition->stats.commitInputRootValues;
            }
            else
            {
                out.stats.commitSinkOps = sinkTopoPositions.size();
                out.stats.commitEventKeyRuns = sinkPartition.clusters.size();
                {
                    std::unordered_set<std::string> uniqueEventKeys;
                    uniqueEventKeys.reserve(sinkTopoPositions.size());
                    for (const auto topoPos : sinkTopoPositions)
                    {
                        const auto op = graph.getOperation(opData.topoOps[topoPos]);
                        uniqueEventKeys.insert(normalizedSinkEventKey(graph, op, &canonicalValues));
                    }
                    out.stats.commitEventKeys = uniqueEventKeys.size();
                }
                for (const auto &cluster : sinkPartition.clusters)
                {
                    CommitNode commit;
                    for (const auto topoPos : cluster)
                    {
                        const auto sinkOp = opData.topoOps[topoPos];
                        commit.ops.push_back(sinkOp);
                        for (const auto operand : graph.opOperands(sinkOp))
                        {
                            if (!vectorContainsValue(commit.inputValues, operand))
                            {
                                commit.inputValues.push_back(operand);
                                ++out.stats.commitInputRootValues;
                            }
                        }
                    }
                    out.commitNodes.push_back(std::move(commit));
                }
            }

            auto ensureRootValue = [&](wolvrix::lib::grh::ValueId value, bool commitRoot) {
                if (!value.valid() || value.graph != graph.id())
                {
                    if (commitRoot)
                    {
                        error = "activity-schedule commit root value ownership mismatch";
                    }
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    return;
                }
                if (defOp.index >= opClasses.size())
                {
                    return;
                }
                const ActivityOpClass defClass = opClasses[defOp.index];
                if (defClass == ActivityOpClass::Source)
                {
                    if (commitRoot)
                    {
                        ++out.stats.directSourceInputsToCommitSupernodes;
                    }
                    builder.ensureSourceOwnerNode(defOp);
                    return;
                }
                if (defClass == ActivityOpClass::Sink)
                {
                    error = "activity-schedule compute root is defined by sink op value=" +
                            std::to_string(value.index) + " def=" + describeOp(graph, defOp);
                    return;
                }
                if (defClass == ActivityOpClass::Compute)
                {
                    const bool common =
                        semanticConsumerCount(graph,
                                              value,
                                              opClasses,
                                              kInvalidActivitySupernodeId,
                                              out.computeNodeOfOp) > 1;
                    builder.ensureComputeNodeForOp(defOp, common);
                }
            };

            for (const auto &commit : out.commitNodes)
            {
                for (const auto input : commit.inputValues)
                {
                    ensureRootValue(input, true);
                    if (!error.empty())
                    {
                        return false;
                    }
                }
            }
            for (const auto &port : graph.outputPorts())
            {
                ensureRootValue(port.value, false);
                if (!error.empty())
                {
                    return false;
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                ensureRootValue(port.out, false);
                if (!error.empty())
                {
                    return false;
                }
                ensureRootValue(port.oe, false);
                if (!error.empty())
                {
                    return false;
                }
            }
            for (const auto opId : opData.topoOps)
            {
                if (opId.index >= opClasses.size() || opClasses[opId.index] != ActivityOpClass::Compute)
                {
                    continue;
                }
                if (!graph.opResults(opId).empty())
                {
                    continue;
                }
                builder.ensureComputeNodeForOp(opId, false);
                if (!error.empty())
                {
                    return false;
                }
            }

            if (!splitDeclaredCutComputeNodes(out, graph, opData))
            {
                error = "activity-schedule declared-value compute-node boundary invariant failed";
                return false;
            }

            constexpr std::size_t kMaxComputeNodeCycleSplitIters = 1024;
            std::size_t cycleSplitIters = 0;
            while (true)
            {
                buildComputeDag(out, out.computeNodeOfOp, graph);
                try
                {
                    out.computeTopoOrder = topoOrderForDag(out.computeDag);
                    break;
                }
                catch (const std::exception &ex)
                {
                    const auto cycle = findCyclePath(out.computeDag);
                    if (cycleSplitIters < kMaxComputeNodeCycleSplitIters &&
                        splitCycleComputeNodesToSingletons(out, graph, opData, cycle))
                    {
                        ++cycleSplitIters;
                        if (!splitDeclaredCutComputeNodes(out, graph, opData))
                        {
                            error =
                                "activity-schedule declared-value compute-node boundary invariant failed after cycle split";
                            return false;
                        }
                        continue;
                    }

                    std::ostringstream oss;
                    oss << "activity-schedule compute-node topo failed: " << ex.what();
                    if (!cycle.empty())
                    {
                        oss << " cycle_path=";
                        const std::size_t nodeLimit = std::min<std::size_t>(cycle.size(), 12);
                        for (std::size_t i = 0; i < nodeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " -> ";
                            }
                            oss << cycle[i];
                        }
                        if (cycle.size() > nodeLimit)
                        {
                            oss << " -> ...";
                        }
                        oss << " cycle_nodes={";
                        for (std::size_t i = 0; i < nodeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " | ";
                            }
                            appendComputeNodeSummary(oss, graph, out, opData, cycle[i]);
                        }
                        oss << "}";
                        oss << " cycle_edges={";
                        const std::size_t edgeLimit =
                            cycle.empty() ? std::size_t{0} : std::min<std::size_t>(cycle.size() - 1, 12);
                        for (std::size_t i = 0; i < edgeLimit; ++i)
                        {
                            if (i != 0)
                            {
                                oss << " | ";
                            }
                            appendComputeNodeEdgeReasons(oss, graph, out, opData, cycle[i], cycle[i + 1]);
                        }
                        if (!cycle.empty() && cycle.size() - 1 > edgeLimit)
                        {
                            oss << " | ...";
                        }
                        oss << "}";
                    }
                    if (cycleSplitIters >= kMaxComputeNodeCycleSplitIters)
                    {
                        oss << " cycle_split_iters=" << cycleSplitIters;
                    }
                    error = oss.str();
                    return false;
                }
            }
            out.stats.computeNodes = out.computeNodes.size();
            out.stats.computeNodeCycleSplitIters = cycleSplitIters;
            out.stats.computeNodeOpsTotal = 0;
            for (const auto &node : out.computeNodes)
            {
                out.stats.computeNodeOpsTotal += node.ops.size();
            }
            return true;
        }

        bool isCloneableLocalSharedComputeOpKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            using wolvrix::lib::grh::OperationKind;
            switch (kind)
            {
            case OperationKind::kAdd:
            case OperationKind::kSub:
            case OperationKind::kEq:
            case OperationKind::kNe:
            case OperationKind::kCaseEq:
            case OperationKind::kCaseNe:
            case OperationKind::kWildcardEq:
            case OperationKind::kWildcardNe:
            case OperationKind::kLt:
            case OperationKind::kLe:
            case OperationKind::kGt:
            case OperationKind::kGe:
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
            case OperationKind::kLogicAnd:
            case OperationKind::kLogicOr:
            case OperationKind::kLogicNot:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceXor:
            case OperationKind::kReduceNor:
            case OperationKind::kReduceNand:
            case OperationKind::kReduceXnor:
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
            case OperationKind::kMux:
            case OperationKind::kAssign:
            case OperationKind::kConcat:
            case OperationKind::kReplicate:
            case OperationKind::kSliceStatic:
            case OperationKind::kSliceDynamic:
                return true;
            default:
                return false;
            }
        }

        bool hasLocalSharedCloneForbiddenAttr(const wolvrix::lib::grh::Operation &op)
        {
            for (const auto &attr : op.attrs())
            {
                if (std::string_view(attr.key).starts_with("regToMem.intent."))
                {
                    return true;
                }
            }
            return false;
        }

        struct LocalSharedComputeCloneStats
        {
            std::size_t scanned = 0;
            std::size_t eligible = 0;
            std::size_t planned = 0;
            std::size_t applied = 0;
            std::size_t rejectedKind = 0;
            std::size_t rejectedShape = 0;
            std::size_t rejectedWidth = 0;
            std::size_t rejectedFanout = 0;
            std::size_t rejectedConsumerNodes = 0;
            std::size_t rejectedIntent = 0;
            std::size_t rejectedSideEffect = 0;
            std::size_t rejectedDeclaredOrPort = 0;
            std::size_t rejectedOperand = 0;
            std::size_t rejectedCapacity = 0;
            std::size_t rejectedBudget = 0;
            std::size_t cloneLimit = 0;
        };

        struct LocalSharedComputeCommonOwnerProbeStats
        {
            using CountMap = ActivityScheduleSummaryStats::KindCountMap;

            std::size_t scanned = 0;
            std::size_t preUserGuardEligible = 0;
            std::size_t rejectedKind = 0;
            std::size_t rejectedShape = 0;
            std::size_t rejectedWidth = 0;
            std::size_t rejectedIntent = 0;
            std::size_t rejectedSideEffect = 0;
            std::size_t rejectedDeclaredOrPort = 0;
            std::size_t invalidOrNonComputeUser = 0;
            std::size_t distinctUserOpsNotTwo = 0;
            std::size_t consumerNodeCountZero = 0;
            std::size_t consumerNodeCountOne = 0;
            std::size_t consumerNodeCountTwo = 0;
            std::size_t consumerNodeCountMoreThanTwo = 0;
            std::size_t sourceOwnerInvalid = 0;
            std::size_t sourceOwnerIsConsumer = 0;
            std::size_t sourceOwnerThirdCommon = 0;
            std::size_t sourceOwnerThirdNonCommon = 0;
            std::size_t thirdCommonSingleton = 0;
            std::size_t thirdCommonMultiOp = 0;
            std::size_t thirdCommonSourceIntentOrIndivisible = 0;
            std::size_t thirdCommonLeftIntentOrIndivisible = 0;
            std::size_t thirdCommonRightIntentOrIndivisible = 0;
            std::size_t thirdCommonAnyIntentOrIndivisible = 0;
            std::size_t resultBoundaryBoth = 0;
            std::size_t resultBoundaryLeftOnly = 0;
            std::size_t resultBoundaryRightOnly = 0;
            std::size_t resultBoundaryNeither = 0;
            std::size_t operandLocalityBoth = 0;
            std::size_t operandLocalityLeftOnly = 0;
            std::size_t operandLocalityRightOnly = 0;
            std::size_t operandLocalityNeither = 0;
            std::size_t capacityBothPass = 0;
            std::size_t capacityLeftFail = 0;
            std::size_t capacityRightFail = 0;
            std::size_t capacityBothFail = 0;
            std::size_t exactEligible = 0;
            std::size_t projectedRemovedPairs = 0;
            CountMap thirdCommonByKind;
            CountMap thirdCommonByResultWidth;
            CountMap thirdCommonByOperandBits;
            CountMap thirdCommonByLeftHeadroom;
            CountMap thirdCommonByRightHeadroom;
            CountMap eligibleByKind;
            CountMap eligibleByResultWidth;
            CountMap eligibleByOperandBits;
        };

        std::string localSharedProbeSizeBucket(std::size_t value)
        {
            if (value == 0)
            {
                return "0";
            }
            if (value == 1)
            {
                return "1";
            }
            if (value <= 4)
            {
                return "2-4";
            }
            if (value <= 16)
            {
                return "5-16";
            }
            if (value <= 64)
            {
                return "17-64";
            }
            if (value <= 256)
            {
                return "65-256";
            }
            if (value <= 1024)
            {
                return "257-1024";
            }
            return ">1024";
        }

        bool localSharedProbeOperandAvailable(
            const wolvrix::lib::grh::Graph &graph,
            const std::vector<ActivityOpClass> &opClasses,
            const ComputeRewriteBuild &rewrite,
            uint32_t nodeId,
            wolvrix::lib::grh::ValueId operand)
        {
            if (nodeId >= rewrite.computeNodes.size())
            {
                return false;
            }
            const auto defOp = graph.valueDef(operand);
            if (defOp.valid() && defOp.index < opClasses.size() &&
                opClasses[defOp.index] == ActivityOpClass::Compute &&
                defOp.index < rewrite.computeNodeOfOp.size() &&
                rewrite.computeNodeOfOp[defOp.index] == nodeId)
            {
                return true;
            }
            return vectorContainsValue(rewrite.computeNodes[nodeId].boundaryInputs, operand);
        }

        std::size_t localSharedCloneEstimatedOpCost(
            wolvrix::lib::grh::OperationKind kind) noexcept
        {
            using wolvrix::lib::grh::OperationKind;
            switch (kind)
            {
            case OperationKind::kAssign:
            case OperationKind::kNot:
            case OperationKind::kLogicNot:
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceXor:
            case OperationKind::kReduceNor:
            case OperationKind::kReduceNand:
            case OperationKind::kReduceXnor:
            case OperationKind::kSliceStatic:
            case OperationKind::kSliceDynamic:
                return 1;
            case OperationKind::kMux:
                return 3;
            default:
                return 2;
            }
        }

        struct LocalSharedComputeCommonOwnerCandidate
        {
            wolvrix::lib::grh::OperationId sourceOp;
            wolvrix::lib::grh::ValueId sourceValue;
            uint32_t sourceNode = kInvalidActivitySupernodeId;
            uint32_t leftNode = kInvalidActivitySupernodeId;
            uint32_t rightNode = kInvalidActivitySupernodeId;
            std::vector<wolvrix::lib::grh::ValueUser> leftUses;
            std::vector<wolvrix::lib::grh::ValueUser> rightUses;
            std::size_t estimatedOpCost = 0;
            std::size_t operandBits = 0;
            std::size_t resultWidth = 0;
            uint32_t sourceTopoPos = kInvalidActivitySupernodeId;
        };

        LocalSharedComputeCommonOwnerProbeStats probeLocalSharedComputeCommonOwners(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const std::vector<ActivityOpClass> &opClasses,
            const ComputeRewriteBuild &rewrite,
            std::vector<LocalSharedComputeCommonOwnerCandidate> *eligibleCandidates = nullptr)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;
            using wolvrix::lib::grh::ValueUser;

            LocalSharedComputeCommonOwnerProbeStats stats;
            const auto nodeRestricted = [](const ComputeNode &node)
            {
                return node.indivisible || !node.intentGroup.empty();
            };
            const auto classifySides = [](bool left,
                                          bool right,
                                          std::size_t &both,
                                          std::size_t &leftOnly,
                                          std::size_t &rightOnly,
                                          std::size_t &neither)
            {
                if (left && right)
                {
                    ++both;
                }
                else if (left)
                {
                    ++leftOnly;
                }
                else if (right)
                {
                    ++rightOnly;
                }
                else
                {
                    ++neither;
                }
            };

            for (const OperationId opId : opData.topoOps)
            {
                if (opId.index >= opClasses.size() ||
                    opClasses[opId.index] != ActivityOpClass::Compute)
                {
                    continue;
                }
                ++stats.scanned;
                if (!isCloneableLocalSharedComputeOpKind(graph.opKind(opId)))
                {
                    ++stats.rejectedKind;
                    continue;
                }
                const auto op = graph.getOperation(opId);
                if (opHasSideEffects(op))
                {
                    ++stats.rejectedSideEffect;
                    continue;
                }
                if (hasLocalSharedCloneForbiddenAttr(op))
                {
                    ++stats.rejectedIntent;
                    continue;
                }
                if (op.results().size() != 1)
                {
                    ++stats.rejectedShape;
                    continue;
                }
                const ValueId value = op.results().front();
                const auto valueInfo = graph.getValue(value);
                if (valueInfo.type() != wolvrix::lib::grh::ValueType::Logic ||
                    valueInfo.width() <= 0 ||
                    static_cast<std::size_t>(valueInfo.width()) >
                        options.localSharedComputeMaxWidth)
                {
                    ++stats.rejectedWidth;
                    continue;
                }
                if (isDeclaredValue(graph, value) || valueInfo.isInput() ||
                    valueInfo.isOutput() || valueInfo.isInout())
                {
                    ++stats.rejectedDeclaredOrPort;
                    continue;
                }
                ++stats.preUserGuardEligible;

                std::map<uint32_t, std::vector<ValueUser>> usesByNode;
                std::unordered_set<OperationId, OperationIdHash> uniqueUserOps;
                bool invalidUser = false;
                for (const auto &user : valueInfo.users())
                {
                    if (!user.operation.valid() || user.operation.index >= opClasses.size() ||
                        opClasses[user.operation.index] != ActivityOpClass::Compute ||
                        user.operation.index >= rewrite.computeNodeOfOp.size())
                    {
                        invalidUser = true;
                        continue;
                    }
                    const uint32_t nodeId = rewrite.computeNodeOfOp[user.operation.index];
                    if (nodeId == kInvalidActivitySupernodeId ||
                        nodeId >= rewrite.computeNodes.size())
                    {
                        invalidUser = true;
                        continue;
                    }
                    uniqueUserOps.insert(user.operation);
                    usesByNode[nodeId].push_back(user);
                }
                if (invalidUser)
                {
                    ++stats.invalidOrNonComputeUser;
                    continue;
                }
                switch (usesByNode.size())
                {
                case 0:
                    ++stats.consumerNodeCountZero;
                    break;
                case 1:
                    ++stats.consumerNodeCountOne;
                    break;
                case 2:
                    ++stats.consumerNodeCountTwo;
                    break;
                default:
                    ++stats.consumerNodeCountMoreThanTwo;
                    break;
                }
                if (uniqueUserOps.size() != 2)
                {
                    ++stats.distinctUserOpsNotTwo;
                    continue;
                }
                if (usesByNode.size() != 2)
                {
                    continue;
                }
                if (opId.index >= rewrite.computeNodeOfOp.size())
                {
                    ++stats.sourceOwnerInvalid;
                    continue;
                }
                const uint32_t sourceNode = rewrite.computeNodeOfOp[opId.index];
                if (sourceNode == kInvalidActivitySupernodeId ||
                    sourceNode >= rewrite.computeNodes.size())
                {
                    ++stats.sourceOwnerInvalid;
                    continue;
                }
                if (usesByNode.contains(sourceNode))
                {
                    ++stats.sourceOwnerIsConsumer;
                    continue;
                }
                const auto &sourceNodeInfo = rewrite.computeNodes[sourceNode];
                if (!sourceNodeInfo.commonExpr)
                {
                    ++stats.sourceOwnerThirdNonCommon;
                    continue;
                }
                ++stats.sourceOwnerThirdCommon;

                auto nodeIt = usesByNode.begin();
                const uint32_t leftNode = nodeIt->first;
                ++nodeIt;
                const uint32_t rightNode = nodeIt->first;
                const auto &leftNodeInfo = rewrite.computeNodes[leftNode];
                const auto &rightNodeInfo = rewrite.computeNodes[rightNode];
                const bool singleton = sourceNodeInfo.ops.size() == 1 &&
                                       sourceNodeInfo.ops.front() == opId;
                if (singleton)
                {
                    ++stats.thirdCommonSingleton;
                }
                else
                {
                    ++stats.thirdCommonMultiOp;
                }

                const bool sourceRestricted = nodeRestricted(sourceNodeInfo);
                const bool leftRestricted = nodeRestricted(leftNodeInfo);
                const bool rightRestricted = nodeRestricted(rightNodeInfo);
                stats.thirdCommonSourceIntentOrIndivisible += sourceRestricted ? 1 : 0;
                stats.thirdCommonLeftIntentOrIndivisible += leftRestricted ? 1 : 0;
                stats.thirdCommonRightIntentOrIndivisible += rightRestricted ? 1 : 0;
                stats.thirdCommonAnyIntentOrIndivisible +=
                    sourceRestricted || leftRestricted || rightRestricted ? 1 : 0;

                const bool resultBoundaryLeft =
                    vectorContainsValue(leftNodeInfo.boundaryInputs, value);
                const bool resultBoundaryRight =
                    vectorContainsValue(rightNodeInfo.boundaryInputs, value);
                classifySides(resultBoundaryLeft,
                              resultBoundaryRight,
                              stats.resultBoundaryBoth,
                              stats.resultBoundaryLeftOnly,
                              stats.resultBoundaryRightOnly,
                              stats.resultBoundaryNeither);

                bool operandsLocalLeft = true;
                bool operandsLocalRight = true;
                std::size_t operandBits = 0;
                for (const ValueId operand : op.operands())
                {
                    operandsLocalLeft &= localSharedProbeOperandAvailable(
                        graph, opClasses, rewrite, leftNode, operand);
                    operandsLocalRight &= localSharedProbeOperandAvailable(
                        graph, opClasses, rewrite, rightNode, operand);
                    const int32_t width = graph.getValue(operand).width();
                    if (width > 0)
                    {
                        const std::size_t add = static_cast<std::size_t>(width);
                        operandBits = add > std::numeric_limits<std::size_t>::max() - operandBits
                                          ? std::numeric_limits<std::size_t>::max()
                                          : operandBits + add;
                    }
                }
                classifySides(operandsLocalLeft,
                              operandsLocalRight,
                              stats.operandLocalityBoth,
                              stats.operandLocalityLeftOnly,
                              stats.operandLocalityRightOnly,
                              stats.operandLocalityNeither);

                const std::size_t maxNodeOps =
                    options.maxOpInComputeNode == 0
                        ? std::numeric_limits<std::size_t>::max()
                        : options.maxOpInComputeNode;
                const bool capacityLeft = leftNodeInfo.ops.size() < maxNodeOps;
                const bool capacityRight = rightNodeInfo.ops.size() < maxNodeOps;
                if (capacityLeft && capacityRight)
                {
                    ++stats.capacityBothPass;
                }
                else if (!capacityLeft && capacityRight)
                {
                    ++stats.capacityLeftFail;
                }
                else if (capacityLeft && !capacityRight)
                {
                    ++stats.capacityRightFail;
                }
                else
                {
                    ++stats.capacityBothFail;
                }

                const std::string kind(wolvrix::lib::grh::toString(op.kind()));
                const std::string resultWidth = widthBucket(
                    static_cast<std::size_t>(valueInfo.width()));
                const std::string operandBitsBucket = localSharedProbeSizeBucket(operandBits);
                const auto headroomBucket = [&](std::size_t nodeOps)
                {
                    return options.maxOpInComputeNode == 0
                               ? std::string("unlimited")
                               : localSharedProbeSizeBucket(
                                     nodeOps < options.maxOpInComputeNode
                                         ? options.maxOpInComputeNode - nodeOps
                                         : 0);
                };
                ++stats.thirdCommonByKind[kind];
                ++stats.thirdCommonByResultWidth[resultWidth];
                ++stats.thirdCommonByOperandBits[operandBitsBucket];
                ++stats.thirdCommonByLeftHeadroom[headroomBucket(leftNodeInfo.ops.size())];
                ++stats.thirdCommonByRightHeadroom[headroomBucket(rightNodeInfo.ops.size())];

                const bool exactEligible = singleton &&
                                           options.localSharedComputeMaxFanout >= 2 &&
                                           !sourceRestricted &&
                                           !leftRestricted &&
                                           !rightRestricted &&
                                           resultBoundaryLeft &&
                                           resultBoundaryRight &&
                                           operandsLocalLeft &&
                                           operandsLocalRight &&
                                           capacityLeft &&
                                           capacityRight;
                if (exactEligible)
                {
                    ++stats.exactEligible;
                    stats.projectedRemovedPairs += 2;
                    ++stats.eligibleByKind[kind];
                    ++stats.eligibleByResultWidth[resultWidth];
                    ++stats.eligibleByOperandBits[operandBitsBucket];
                    if (eligibleCandidates != nullptr)
                    {
                        LocalSharedComputeCommonOwnerCandidate candidate;
                        candidate.sourceOp = opId;
                        candidate.sourceValue = value;
                        candidate.sourceNode = sourceNode;
                        candidate.leftNode = leftNode;
                        candidate.rightNode = rightNode;
                        candidate.leftUses = usesByNode.at(leftNode);
                        candidate.rightUses = usesByNode.at(rightNode);
                        candidate.estimatedOpCost =
                            localSharedCloneEstimatedOpCost(op.kind());
                        candidate.operandBits = operandBits;
                        candidate.resultWidth =
                            static_cast<std::size_t>(valueInfo.width());
                        candidate.sourceTopoPos =
                            opId.index < opData.topoPosByOpIndex.size()
                                ? opData.topoPosByOpIndex[opId.index]
                                : kInvalidActivitySupernodeId;
                        eligibleCandidates->push_back(std::move(candidate));
                    }
                }
            }
            return stats;
        }

        struct LocalSharedComputeCommonOwnerCloneStats
        {
            std::size_t rawEligible = 0;
            std::size_t selected = 0;
            std::size_t applied = 0;
            std::size_t cloneLimit = 0;
            std::size_t rejectedBudget = 0;
            std::size_t rejectedCapacity = 0;
            std::size_t rejectedRole = 0;
            std::size_t rejectedDependency = 0;
            std::size_t rejectedStale = 0;
            std::size_t projectedRemovedPairs = 0;
            std::size_t actualLocalizedPairs = 0;
            std::size_t graphOpsBefore = 0;
            std::size_t graphValuesBefore = 0;
        };

        struct LocalSharedComputeCommonOwnerClonePlan
        {
            wolvrix::lib::grh::OperationId sourceOp;
            wolvrix::lib::grh::ValueId sourceValue;
            wolvrix::lib::grh::OperationId cloneOp;
            wolvrix::lib::grh::ValueId cloneValue;
            uint32_t originalTargetNode = kInvalidActivitySupernodeId;
            uint32_t cloneTargetNode = kInvalidActivitySupernodeId;
            std::vector<wolvrix::lib::grh::ValueUser> originalUses;
            std::vector<wolvrix::lib::grh::ValueUser> cloneUses;
            wolvrix::lib::grh::OperationKind kind;
            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::AttrKV> attrs;
            std::optional<wolvrix::lib::grh::SrcLoc> opSrcLoc;
            int32_t resultWidth = 0;
            bool resultSigned = false;
            wolvrix::lib::grh::ValueType resultType =
                wolvrix::lib::grh::ValueType::Logic;
            std::optional<wolvrix::lib::grh::SrcLoc> resultSrcLoc;
        };

        std::size_t localSharedCommonOwnerCloneLimit(
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const std::vector<ActivityOpClass> &opClasses)
        {
            const std::uint64_t computeOps = static_cast<std::uint64_t>(std::count_if(
                opData.topoOps.begin(),
                opData.topoOps.end(),
                [&](wolvrix::lib::grh::OperationId opId)
                {
                    return opId.index < opClasses.size() &&
                           opClasses[opId.index] == ActivityOpClass::Compute;
                }));
            const std::uint64_t ppm = static_cast<std::uint64_t>(
                options.localSharedComputeCommonOwnerMaxClonedOpPpm);
            const bool overflow =
                ppm != 0 && computeOps > std::numeric_limits<std::uint64_t>::max() / ppm;
            const std::uint64_t numerator =
                overflow ? std::numeric_limits<std::uint64_t>::max() : computeOps * ppm;
            const std::uint64_t ppmLimit = numerator / 1000000ULL;
            const std::size_t boundedPpmLimit =
                ppmLimit > std::numeric_limits<std::size_t>::max()
                    ? std::numeric_limits<std::size_t>::max()
                    : static_cast<std::size_t>(ppmLimit);
            return std::min(options.localSharedComputeCommonOwnerMaxClones,
                            boundedPpmLimit);
        }

        bool applyLocalSharedComputeCommonOwnerClones(
            wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const std::vector<ActivityOpClass> &opClasses,
            const ComputeRewriteBuild &rewrite,
            std::vector<LocalSharedComputeCommonOwnerCandidate> candidates,
            LocalSharedComputeCommonOwnerCloneStats &stats,
            std::vector<LocalSharedComputeCommonOwnerClonePlan> &plans,
            std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;

            stats = LocalSharedComputeCommonOwnerCloneStats{};
            plans.clear();
            stats.rawEligible = candidates.size();
            stats.cloneLimit = localSharedCommonOwnerCloneLimit(options, opData, opClasses);
            stats.graphOpsBefore = graph.operations().size();
            stats.graphValuesBefore = graph.values().size();

            std::vector<uint32_t> nodeTopoPos(rewrite.computeNodes.size(),
                                              kInvalidActivitySupernodeId);
            for (uint32_t pos = 0; pos < rewrite.computeTopoOrder.size(); ++pos)
            {
                const uint32_t nodeId = rewrite.computeTopoOrder[pos];
                if (nodeId < nodeTopoPos.size())
                {
                    nodeTopoPos[nodeId] = pos;
                }
            }
            const auto orderedTargets = [&](const LocalSharedComputeCommonOwnerCandidate &candidate)
            {
                const auto key = [&](uint32_t nodeId)
                {
                    return std::pair{
                        nodeId < nodeTopoPos.size() ? nodeTopoPos[nodeId]
                                                   : kInvalidActivitySupernodeId,
                        nodeId};
                };
                return key(candidate.leftNode) <= key(candidate.rightNode)
                           ? std::pair{candidate.leftNode, candidate.rightNode}
                           : std::pair{candidate.rightNode, candidate.leftNode};
            };
            std::stable_sort(
                candidates.begin(),
                candidates.end(),
                [&](const auto &lhs, const auto &rhs)
                {
                    const auto lhsTargets = orderedTargets(lhs);
                    const auto rhsTargets = orderedTargets(rhs);
                    return std::tuple{lhs.estimatedOpCost,
                                      lhs.operandBits,
                                      lhs.resultWidth,
                                      lhs.sourceTopoPos,
                                      lhs.sourceOp.index,
                                      lhsTargets.first,
                                      lhsTargets.second} <
                           std::tuple{rhs.estimatedOpCost,
                                      rhs.operandBits,
                                      rhs.resultWidth,
                                      rhs.sourceTopoPos,
                                      rhs.sourceOp.index,
                                      rhsTargets.first,
                                      rhsTargets.second};
                });

            std::vector<std::size_t> plannedAddsByNode(rewrite.computeNodes.size(), 0);
            std::unordered_set<uint32_t> selectedSourceNodes;
            std::unordered_set<uint32_t> selectedTargetNodes;
            std::unordered_set<OperationId, OperationIdHash> selectedSourceOps;
            std::unordered_set<OperationId, OperationIdHash> selectedUserOps;
            const std::size_t maxNodeOps =
                options.maxOpInComputeNode == 0
                    ? std::numeric_limits<std::size_t>::max()
                    : options.maxOpInComputeNode;

            for (const auto &candidate : candidates)
            {
                const auto [earlyNode, lateNode] = orderedTargets(candidate);
                if (selectedTargetNodes.contains(candidate.sourceNode) ||
                    selectedSourceNodes.contains(earlyNode) ||
                    selectedSourceNodes.contains(lateNode))
                {
                    ++stats.rejectedDependency;
                    continue;
                }
                std::unordered_set<OperationId, OperationIdHash> candidateUsers;
                for (const auto &use : candidate.leftUses)
                {
                    candidateUsers.insert(use.operation);
                }
                for (const auto &use : candidate.rightUses)
                {
                    candidateUsers.insert(use.operation);
                }
                bool roleConflict = selectedUserOps.contains(candidate.sourceOp);
                for (const OperationId userOp : candidateUsers)
                {
                    roleConflict |= selectedSourceOps.contains(userOp);
                }
                if (roleConflict)
                {
                    ++stats.rejectedRole;
                    continue;
                }
                const auto noCapacity = [&](uint32_t nodeId)
                {
                    return nodeId >= rewrite.computeNodes.size() ||
                           plannedAddsByNode[nodeId] >= maxNodeOps ||
                           rewrite.computeNodes[nodeId].ops.size() >=
                               maxNodeOps - plannedAddsByNode[nodeId];
                };
                if (noCapacity(earlyNode) || noCapacity(lateNode))
                {
                    ++stats.rejectedCapacity;
                    continue;
                }
                if (plans.size() >= stats.cloneLimit)
                {
                    ++stats.rejectedBudget;
                    continue;
                }

                const auto op = graph.getOperation(candidate.sourceOp);
                const auto value = graph.getValue(candidate.sourceValue);
                LocalSharedComputeCommonOwnerClonePlan plan;
                plan.sourceOp = candidate.sourceOp;
                plan.sourceValue = candidate.sourceValue;
                plan.originalTargetNode = earlyNode;
                plan.cloneTargetNode = lateNode;
                plan.originalUses = earlyNode == candidate.leftNode
                                        ? candidate.leftUses
                                        : candidate.rightUses;
                plan.cloneUses = lateNode == candidate.leftNode
                                     ? candidate.leftUses
                                     : candidate.rightUses;
                plan.kind = op.kind();
                plan.operands.assign(op.operands().begin(), op.operands().end());
                plan.attrs.assign(op.attrs().begin(), op.attrs().end());
                plan.opSrcLoc = op.srcLoc();
                plan.resultWidth = value.width();
                plan.resultSigned = value.isSigned();
                plan.resultType = value.type();
                plan.resultSrcLoc = value.srcLoc();
                plans.push_back(std::move(plan));
                ++plannedAddsByNode[earlyNode];
                ++plannedAddsByNode[lateNode];
                selectedSourceNodes.insert(candidate.sourceNode);
                selectedTargetNodes.insert(earlyNode);
                selectedTargetNodes.insert(lateNode);
                selectedSourceOps.insert(candidate.sourceOp);
                selectedUserOps.insert(candidateUsers.begin(), candidateUsers.end());
            }
            stats.selected = plans.size();
            stats.projectedRemovedPairs = plans.size() * 2;

            for (auto &plan : plans)
            {
                try
                {
                    plan.cloneOp = graph.createOperation(plan.kind, graph.makeInternalOpSym());
                    if (plan.opSrcLoc)
                    {
                        graph.setOpSrcLoc(plan.cloneOp, *plan.opSrcLoc);
                    }
                    for (const auto &attr : plan.attrs)
                    {
                        graph.setAttr(plan.cloneOp, attr.key, attr.value);
                    }
                    for (const ValueId operand : plan.operands)
                    {
                        graph.addOperand(plan.cloneOp, operand);
                    }
                    plan.cloneValue = graph.createValue(graph.makeInternalValSym(),
                                                        plan.resultWidth,
                                                        plan.resultSigned,
                                                        plan.resultType);
                    if (plan.resultSrcLoc)
                    {
                        graph.setValueSrcLoc(plan.cloneValue, *plan.resultSrcLoc);
                    }
                    graph.addResult(plan.cloneOp, plan.cloneValue);
                    for (const auto &use : plan.cloneUses)
                    {
                        const auto operands = graph.opOperands(use.operation);
                        if (use.operandIndex >= operands.size() ||
                            operands[use.operandIndex] != plan.sourceValue)
                        {
                            ++stats.rejectedStale;
                            error = "activity-schedule common-owner clone selected use became stale";
                            return false;
                        }
                        graph.replaceOperand(use.operation,
                                             use.operandIndex,
                                             plan.cloneValue);
                    }
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule common-owner clone apply failed source=" +
                            describeOp(graph, plan.sourceOp) + ": " + ex.what();
                    return false;
                }
                ++stats.applied;
            }
            return true;
        }

        struct LocalSharedComputeCloneRecord
        {
            wolvrix::lib::grh::OperationId sourceOp;
            wolvrix::lib::grh::ValueId sourceValue;
            wolvrix::lib::grh::OperationId cloneOp;
            wolvrix::lib::grh::ValueId cloneValue;
            std::vector<wolvrix::lib::grh::OperationId> originalUsers;
            std::vector<wolvrix::lib::grh::OperationId> cloneUsers;
        };

        bool applyLocalSharedComputeClones(
            wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const std::vector<ActivityOpClass> &opClasses,
            const ComputeRewriteBuild &rewrite,
            LocalSharedComputeCloneStats &stats,
            std::vector<LocalSharedComputeCloneRecord> &records,
            std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;
            using wolvrix::lib::grh::ValueUser;

            stats = LocalSharedComputeCloneStats{};
            records.clear();
            const std::uint64_t baselineComputeOpCount =
                static_cast<std::uint64_t>(std::count_if(
                    opData.topoOps.begin(),
                    opData.topoOps.end(),
                    [&](OperationId opId)
                    {
                        return opId.index < opClasses.size() &&
                               opClasses[opId.index] == ActivityOpClass::Compute;
                    }));
            const std::uint64_t clonePpm =
                static_cast<std::uint64_t>(options.localSharedComputeMaxClonedOpPpm);
            const bool ppmProductOverflows =
                clonePpm != 0 &&
                baselineComputeOpCount >
                    std::numeric_limits<std::uint64_t>::max() / clonePpm;
            const std::uint64_t ppmNumerator =
                ppmProductOverflows
                    ? std::numeric_limits<std::uint64_t>::max()
                    : baselineComputeOpCount * clonePpm;
            const std::uint64_t ppmLimit = ppmNumerator / 1000000ULL;
            stats.cloneLimit = std::min<std::size_t>(
                options.localSharedComputeMaxClones,
                ppmLimit > std::numeric_limits<std::size_t>::max()
                    ? std::numeric_limits<std::size_t>::max()
                    : static_cast<std::size_t>(ppmLimit));
            if (!options.enableLocalSharedCompute)
            {
                return true;
            }

            struct Plan
            {
                OperationId sourceOp;
                ValueId sourceValue;
                uint32_t cloneNode = kInvalidActivitySupernodeId;
                std::vector<ValueUser> originalUses;
                std::vector<ValueUser> cloneUses;
                wolvrix::lib::grh::OperationKind kind;
                std::vector<ValueId> operands;
                std::vector<wolvrix::lib::grh::AttrKV> attrs;
                std::optional<wolvrix::lib::grh::SrcLoc> opSrcLoc;
                int32_t resultWidth = 0;
                bool resultSigned = false;
                wolvrix::lib::grh::ValueType resultType =
                    wolvrix::lib::grh::ValueType::Logic;
                std::optional<wolvrix::lib::grh::SrcLoc> resultSrcLoc;
            };
            std::vector<Plan> plans;
            plans.reserve(stats.cloneLimit);
            std::vector<std::size_t> plannedAddsByNode(rewrite.computeNodes.size(), 0);

            for (const OperationId opId : opData.topoOps)
            {
                if (opId.index >= opClasses.size() ||
                    opClasses[opId.index] != ActivityOpClass::Compute)
                {
                    continue;
                }
                ++stats.scanned;
                if (!isCloneableLocalSharedComputeOpKind(graph.opKind(opId)))
                {
                    ++stats.rejectedKind;
                    continue;
                }
                const auto op = graph.getOperation(opId);
                if (opHasSideEffects(op))
                {
                    ++stats.rejectedSideEffect;
                    continue;
                }
                if (hasLocalSharedCloneForbiddenAttr(op))
                {
                    ++stats.rejectedIntent;
                    continue;
                }
                if (op.results().size() != 1)
                {
                    ++stats.rejectedShape;
                    continue;
                }
                const ValueId value = op.results().front();
                const auto valueInfo = graph.getValue(value);
                if (valueInfo.type() != wolvrix::lib::grh::ValueType::Logic ||
                    valueInfo.width() <= 0 ||
                    static_cast<std::size_t>(valueInfo.width()) >
                        options.localSharedComputeMaxWidth)
                {
                    ++stats.rejectedWidth;
                    continue;
                }
                if (isDeclaredValue(graph, value) || valueInfo.isInput() ||
                    valueInfo.isOutput() || valueInfo.isInout())
                {
                    ++stats.rejectedDeclaredOrPort;
                    continue;
                }

                const std::vector<ValueUser> users(valueInfo.users().begin(),
                                                   valueInfo.users().end());
                std::map<uint32_t, std::vector<ValueUser>> usesByNode;
                std::unordered_set<OperationId, OperationIdHash> uniqueUserOps;
                bool invalidUser = false;
                for (const auto &user : users)
                {
                    if (!user.operation.valid() || user.operation.index >= opClasses.size() ||
                        opClasses[user.operation.index] != ActivityOpClass::Compute ||
                        user.operation.index >= rewrite.computeNodeOfOp.size())
                    {
                        invalidUser = true;
                        break;
                    }
                    const uint32_t nodeId = rewrite.computeNodeOfOp[user.operation.index];
                    if (nodeId == kInvalidActivitySupernodeId ||
                        nodeId >= rewrite.computeNodes.size())
                    {
                        invalidUser = true;
                        break;
                    }
                    uniqueUserOps.insert(user.operation);
                    usesByNode[nodeId].push_back(user);
                }
                if (uniqueUserOps.size() < 2 ||
                    uniqueUserOps.size() > options.localSharedComputeMaxFanout)
                {
                    ++stats.rejectedFanout;
                    continue;
                }
                if (invalidUser || usesByNode.size() != 2)
                {
                    ++stats.rejectedConsumerNodes;
                    continue;
                }
                if (opId.index >= rewrite.computeNodeOfOp.size())
                {
                    ++stats.rejectedConsumerNodes;
                    continue;
                }
                const uint32_t sourceNode = rewrite.computeNodeOfOp[opId.index];
                if (sourceNode == kInvalidActivitySupernodeId ||
                    sourceNode >= rewrite.computeNodes.size() ||
                    usesByNode.find(sourceNode) == usesByNode.end())
                {
                    ++stats.rejectedConsumerNodes;
                    continue;
                }

                auto nodeIt = usesByNode.begin();
                const uint32_t firstNode = nodeIt->first;
                ++nodeIt;
                const uint32_t secondNode = nodeIt->first;
                const uint32_t cloneNode = sourceNode == firstNode ? secondNode : firstNode;
                const uint32_t originalNode = sourceNode;
                const auto &sourceNodeInfo = rewrite.computeNodes[sourceNode];
                const auto &cloneNodeInfo = rewrite.computeNodes[cloneNode];
                const auto &originalNodeInfo = rewrite.computeNodes[originalNode];
                if (sourceNodeInfo.indivisible || !sourceNodeInfo.intentGroup.empty() ||
                    cloneNodeInfo.indivisible || !cloneNodeInfo.intentGroup.empty() ||
                    originalNodeInfo.indivisible || !originalNodeInfo.intentGroup.empty())
                {
                    ++stats.rejectedIntent;
                    continue;
                }
                const std::size_t maxNodeOps =
                    options.maxOpInComputeNode == 0
                        ? std::numeric_limits<std::size_t>::max()
                        : options.maxOpInComputeNode;
                if (plannedAddsByNode[cloneNode] >= maxNodeOps ||
                    cloneNodeInfo.ops.size() > maxNodeOps - plannedAddsByNode[cloneNode] ||
                    cloneNodeInfo.ops.size() + plannedAddsByNode[cloneNode] >= maxNodeOps)
                {
                    ++stats.rejectedCapacity;
                    continue;
                }

                bool invalidOperand = false;
                for (const ValueId operand : op.operands())
                {
                    const OperationId defOp = graph.valueDef(operand);
                    bool local = false;
                    if (defOp.valid() && defOp.index < opClasses.size() &&
                        opClasses[defOp.index] == ActivityOpClass::Compute &&
                        defOp.index < rewrite.computeNodeOfOp.size())
                    {
                        local = rewrite.computeNodeOfOp[defOp.index] == cloneNode;
                    }
                    if (!local && !vectorContainsValue(cloneNodeInfo.boundaryInputs, operand))
                    {
                        invalidOperand = true;
                        break;
                    }
                }
                if (invalidOperand)
                {
                    ++stats.rejectedOperand;
                    continue;
                }

                ++stats.eligible;
                if (plans.size() >= stats.cloneLimit)
                {
                    ++stats.rejectedBudget;
                    continue;
                }
                Plan plan;
                plan.sourceOp = opId;
                plan.sourceValue = value;
                plan.cloneNode = cloneNode;
                plan.cloneUses = usesByNode.at(cloneNode);
                for (const auto &[nodeId, nodeUses] : usesByNode)
                {
                    if (nodeId != cloneNode)
                    {
                        plan.originalUses.insert(plan.originalUses.end(),
                                                 nodeUses.begin(),
                                                 nodeUses.end());
                    }
                }
                plan.kind = op.kind();
                plan.operands.assign(op.operands().begin(), op.operands().end());
                plan.attrs.assign(op.attrs().begin(), op.attrs().end());
                plan.opSrcLoc = op.srcLoc();
                plan.resultWidth = valueInfo.width();
                plan.resultSigned = valueInfo.isSigned();
                plan.resultType = valueInfo.type();
                plan.resultSrcLoc = valueInfo.srcLoc();
                plans.push_back(std::move(plan));
                ++plannedAddsByNode[cloneNode];
            }
            stats.planned = plans.size();

            records.reserve(plans.size());
            for (const Plan &plan : plans)
            {
                OperationId cloneOp;
                ValueId cloneValue;
                try
                {
                    cloneOp = graph.createOperation(plan.kind, graph.makeInternalOpSym());
                    if (plan.opSrcLoc)
                    {
                        graph.setOpSrcLoc(cloneOp, *plan.opSrcLoc);
                    }
                    for (const auto &attr : plan.attrs)
                    {
                        graph.setAttr(cloneOp, attr.key, attr.value);
                    }
                    for (const ValueId operand : plan.operands)
                    {
                        graph.addOperand(cloneOp, operand);
                    }
                    cloneValue = graph.createValue(graph.makeInternalValSym(),
                                                   plan.resultWidth,
                                                   plan.resultSigned,
                                                   plan.resultType);
                    if (plan.resultSrcLoc)
                    {
                        graph.setValueSrcLoc(cloneValue, *plan.resultSrcLoc);
                    }
                    graph.addResult(cloneOp, cloneValue);
                    for (const auto &use : plan.cloneUses)
                    {
                        graph.replaceOperand(use.operation, use.operandIndex, cloneValue);
                    }
                }
                catch (const std::exception &ex)
                {
                    error = "activity-schedule local shared compute clone apply failed source=" +
                            describeOp(graph, plan.sourceOp) + ": " + ex.what();
                    return false;
                }

                LocalSharedComputeCloneRecord record;
                record.sourceOp = plan.sourceOp;
                record.sourceValue = plan.sourceValue;
                record.cloneOp = cloneOp;
                record.cloneValue = cloneValue;
                for (const auto &use : plan.originalUses)
                {
                    record.originalUsers.push_back(use.operation);
                }
                for (const auto &use : plan.cloneUses)
                {
                    record.cloneUsers.push_back(use.operation);
                }
                record.originalUsers = uniqueOpsPreservingOrder(record.originalUsers);
                record.cloneUsers = uniqueOpsPreservingOrder(record.cloneUsers);
                records.push_back(std::move(record));
                ++stats.applied;
            }
            return true;
        }

        bool validateLocalSharedComputeCloneRewrite(
            const ActivityScheduleOptions &options,
            const ComputeRewriteBuild &baseline,
            const ComputeRewriteBuild &candidate,
            const std::vector<LocalSharedComputeCloneRecord> &records,
            std::string &error)
        {
            const auto opIds = [](const std::vector<wolvrix::lib::grh::OperationId> &ops)
            {
                std::vector<uint32_t> out;
                out.reserve(ops.size());
                for (const auto opId : ops)
                {
                    out.push_back(opId.index);
                }
                return out;
            };
            const auto valueIds = [](const std::vector<wolvrix::lib::grh::ValueId> &values)
            {
                std::vector<uint32_t> out;
                out.reserve(values.size());
                for (const auto valueId : values)
                {
                    out.push_back(valueId.index);
                }
                return out;
            };
            const auto asSet = [](std::vector<uint32_t> ids)
            {
                std::sort(ids.begin(), ids.end());
                ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
                return ids;
            };
            const auto sampleIds = [](const std::vector<uint32_t> &ids)
            {
                std::ostringstream out;
                out << "[";
                const std::size_t limit = std::min<std::size_t>(ids.size(), 8);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (i != 0)
                    {
                        out << ",";
                    }
                    out << ids[i];
                }
                if (ids.size() > limit)
                {
                    out << ",...";
                }
                out << "]";
                return out.str();
            };
            const auto firstDiff = [](const std::vector<uint32_t> &lhs,
                                      const std::vector<uint32_t> &rhs)
            {
                const std::size_t common = std::min(lhs.size(), rhs.size());
                std::size_t pos = 0;
                while (pos < common && lhs[pos] == rhs[pos])
                {
                    ++pos;
                }
                std::ostringstream out;
                out << "pos=" << pos << " baseline=";
                if (pos < lhs.size())
                {
                    out << lhs[pos];
                }
                else
                {
                    out << "<end>";
                }
                out << " candidate=";
                if (pos < rhs.size())
                {
                    out << rhs[pos];
                }
                else
                {
                    out << "<end>";
                }
                return out.str();
            };
            const auto globalOpSet = [&](const ComputeRewriteBuild &build)
            {
                std::vector<uint32_t> ids;
                for (const auto &node : build.commitNodes)
                {
                    const auto nodeIds = opIds(node.ops);
                    ids.insert(ids.end(), nodeIds.begin(), nodeIds.end());
                }
                return asSet(std::move(ids));
            };
            const auto globalInputSet = [&](const ComputeRewriteBuild &build)
            {
                std::vector<uint32_t> ids;
                for (const auto &node : build.commitNodes)
                {
                    const auto nodeIds = valueIds(node.inputValues);
                    ids.insert(ids.end(), nodeIds.begin(), nodeIds.end());
                }
                return asSet(std::move(ids));
            };
            const auto normalizedOpPartition = [&](const ComputeRewriteBuild &build)
            {
                std::vector<std::vector<uint32_t>> partition;
                partition.reserve(build.commitNodes.size());
                for (const auto &node : build.commitNodes)
                {
                    auto ids = opIds(node.ops);
                    std::sort(ids.begin(), ids.end());
                    partition.push_back(std::move(ids));
                }
                std::sort(partition.begin(), partition.end());
                return partition;
            };
            const auto normalizedInputPartition = [&](const ComputeRewriteBuild &build)
            {
                std::vector<std::vector<uint32_t>> partition;
                partition.reserve(build.commitNodes.size());
                for (const auto &node : build.commitNodes)
                {
                    auto ids = valueIds(node.inputValues);
                    std::sort(ids.begin(), ids.end());
                    partition.push_back(std::move(ids));
                }
                std::sort(partition.begin(), partition.end());
                return partition;
            };
            const auto appendGlobalCommitDiagnostics = [&](std::ostringstream &out)
            {
                const auto baselineOps = globalOpSet(baseline);
                const auto candidateOps = globalOpSet(candidate);
                const auto baselineInputs = globalInputSet(baseline);
                const auto candidateInputs = globalInputSet(candidate);
                out << " global_sink_op_set_equal=" << (baselineOps == candidateOps ? "true" : "false")
                    << " global_sink_op_counts=" << baselineOps.size() << "/" << candidateOps.size()
                    << " global_input_value_set_equal="
                    << (baselineInputs == candidateInputs ? "true" : "false")
                    << " global_input_value_counts=" << baselineInputs.size() << "/"
                    << candidateInputs.size()
                    << " normalized_op_partition_equal="
                    << (normalizedOpPartition(baseline) == normalizedOpPartition(candidate)
                            ? "true"
                            : "false")
                    << " normalized_input_partition_equal="
                    << (normalizedInputPartition(baseline) == normalizedInputPartition(candidate)
                            ? "true"
                            : "false");
            };
            if (baseline.commitNodes.size() != candidate.commitNodes.size())
            {
                std::ostringstream out;
                out << "activity-schedule local shared compute clone changed commit node count"
                    << " baseline_nodes=" << baseline.commitNodes.size()
                    << " candidate_nodes=" << candidate.commitNodes.size();
                appendGlobalCommitDiagnostics(out);
                error = out.str();
                return false;
            }
            for (std::size_t i = 0; i < baseline.commitNodes.size(); ++i)
            {
                const bool opsEqual = baseline.commitNodes[i].ops == candidate.commitNodes[i].ops;
                const bool inputsEqual =
                    baseline.commitNodes[i].inputValues == candidate.commitNodes[i].inputValues;
                if (!opsEqual || !inputsEqual)
                {
                    const auto baselineOps = opIds(baseline.commitNodes[i].ops);
                    const auto candidateOps = opIds(candidate.commitNodes[i].ops);
                    const auto baselineInputs = valueIds(baseline.commitNodes[i].inputValues);
                    const auto candidateInputs = valueIds(candidate.commitNodes[i].inputValues);
                    std::ostringstream out;
                    out << "activity-schedule local shared compute clone changed commit partition"
                        << " node=" << i
                        << " ops_equal=" << (opsEqual ? "true" : "false")
                        << " inputs_equal=" << (inputsEqual ? "true" : "false")
                        << " op_sizes=" << baselineOps.size() << "/" << candidateOps.size()
                        << " input_sizes=" << baselineInputs.size() << "/"
                        << candidateInputs.size()
                        << " op_set_equal="
                        << (asSet(baselineOps) == asSet(candidateOps) ? "true" : "false")
                        << " input_set_equal="
                        << (asSet(baselineInputs) == asSet(candidateInputs) ? "true" : "false")
                        << " baseline_ops=" << sampleIds(baselineOps)
                        << " candidate_ops=" << sampleIds(candidateOps)
                        << " op_first_diff={" << firstDiff(baselineOps, candidateOps) << "}"
                        << " baseline_inputs=" << sampleIds(baselineInputs)
                        << " candidate_inputs=" << sampleIds(candidateInputs)
                        << " input_first_diff={" << firstDiff(baselineInputs, candidateInputs) << "}";
                    appendGlobalCommitDiagnostics(out);
                    error = out.str();
                    return false;
                }
            }
            if (candidate.stats.computeNodeCycleSplitIters >
                baseline.stats.computeNodeCycleSplitIters)
            {
                error = "activity-schedule local shared compute clone increased compute-node cycle splitting";
                return false;
            }

            std::unordered_map<wolvrix::lib::grh::OperationId,
                               std::string,
                               wolvrix::lib::grh::OperationIdHash>
                baselineIntent;
            for (const auto &node : baseline.computeNodes)
            {
                if (node.intentGroup.empty())
                {
                    continue;
                }
                for (const auto opId : node.ops)
                {
                    baselineIntent.emplace(opId, node.intentGroup);
                }
            }
            std::unordered_map<wolvrix::lib::grh::OperationId,
                               std::string,
                               wolvrix::lib::grh::OperationIdHash>
                candidateIntent;
            for (const auto &node : candidate.computeNodes)
            {
                if (node.intentGroup.empty())
                {
                    continue;
                }
                for (const auto opId : node.ops)
                {
                    candidateIntent.emplace(opId, node.intentGroup);
                }
            }
            if (baselineIntent != candidateIntent)
            {
                error = "activity-schedule local shared compute clone changed intent groups";
                return false;
            }

            const auto ownerOf = [&](wolvrix::lib::grh::OperationId opId) -> uint32_t {
                return opId.valid() && opId.index < candidate.computeNodeOfOp.size()
                           ? candidate.computeNodeOfOp[opId.index]
                           : kInvalidActivitySupernodeId;
            };
            for (const auto &record : records)
            {
                const uint32_t sourceNode = ownerOf(record.sourceOp);
                const uint32_t cloneNode = ownerOf(record.cloneOp);
                if (sourceNode == kInvalidActivitySupernodeId ||
                    cloneNode == kInvalidActivitySupernodeId || sourceNode == cloneNode)
                {
                    error = "activity-schedule local shared compute clone did not split source owners";
                    return false;
                }
                for (const auto userOp : record.originalUsers)
                {
                    if (ownerOf(userOp) != sourceNode)
                    {
                        error = "activity-schedule local shared compute original is not local to retained consumer";
                        return false;
                    }
                }
                for (const auto userOp : record.cloneUsers)
                {
                    if (ownerOf(userOp) != cloneNode)
                    {
                        error = "activity-schedule local shared compute clone is not local to rewritten consumer";
                        return false;
                    }
                }
            }
            if (options.maxOpInComputeNode != 0)
            {
                for (const auto &node : candidate.computeNodes)
                {
                    if (!node.indivisible && node.ops.size() > options.maxOpInComputeNode)
                    {
                        error = "activity-schedule local shared compute clone exceeded compute node cap";
                        return false;
                    }
                }
            }
            return true;
        }

        bool localSharedCloneSrcLocEqual(
            const std::optional<wolvrix::lib::grh::SrcLoc> &lhs,
            const std::optional<wolvrix::lib::grh::SrcLoc> &rhs)
        {
            if (lhs.has_value() != rhs.has_value())
            {
                return false;
            }
            if (!lhs)
            {
                return true;
            }
            return lhs->file == rhs->file && lhs->line == rhs->line &&
                   lhs->column == rhs->column && lhs->endLine == rhs->endLine &&
                   lhs->endColumn == rhs->endColumn && lhs->origin == rhs->origin &&
                   lhs->pass == rhs->pass && lhs->note == rhs->note;
        }

        bool localSharedCloneAttrsEqual(
            std::span<const wolvrix::lib::grh::AttrKV> lhs,
            const std::vector<wolvrix::lib::grh::AttrKV> &rhs)
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lhs[i].key != rhs[i].key || lhs[i].value != rhs[i].value)
                {
                    return false;
                }
            }
            return true;
        }

        bool localSharedCloneUsersEqual(
            std::span<const wolvrix::lib::grh::ValueUser> actual,
            const std::vector<wolvrix::lib::grh::ValueUser> &expected)
        {
            const auto key = [](const wolvrix::lib::grh::ValueUser &user)
            {
                return std::tuple{user.operation.index,
                                  user.operation.generation,
                                  user.operandIndex};
            };
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> actualKeys;
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> expectedKeys;
            actualKeys.reserve(actual.size());
            expectedKeys.reserve(expected.size());
            for (const auto &user : actual)
            {
                actualKeys.push_back(key(user));
            }
            for (const auto &user : expected)
            {
                expectedKeys.push_back(key(user));
            }
            std::sort(actualKeys.begin(), actualKeys.end());
            std::sort(expectedKeys.begin(), expectedKeys.end());
            return actualKeys == expectedKeys;
        }

        bool validateLocalSharedComputeCommonOwnerCloneRewrite(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ComputeRewriteBuild &baseline,
            const ComputeRewriteBuild &candidate,
            const std::vector<LocalSharedComputeCommonOwnerClonePlan> &plans,
            LocalSharedComputeCommonOwnerCloneStats &stats,
            std::string &error)
        {
            std::vector<LocalSharedComputeCloneRecord> records;
            records.reserve(plans.size());
            for (const auto &plan : plans)
            {
                LocalSharedComputeCloneRecord record;
                record.sourceOp = plan.sourceOp;
                record.sourceValue = plan.sourceValue;
                record.cloneOp = plan.cloneOp;
                record.cloneValue = plan.cloneValue;
                for (const auto &use : plan.originalUses)
                {
                    record.originalUsers.push_back(use.operation);
                }
                for (const auto &use : plan.cloneUses)
                {
                    record.cloneUsers.push_back(use.operation);
                }
                record.originalUsers = uniqueOpsPreservingOrder(record.originalUsers);
                record.cloneUsers = uniqueOpsPreservingOrder(record.cloneUsers);
                records.push_back(std::move(record));
            }
            if (!validateLocalSharedComputeCloneRewrite(options,
                                                        baseline,
                                                        candidate,
                                                        records,
                                                        error))
            {
                return false;
            }
            if (graph.operations().size() != stats.graphOpsBefore + plans.size() ||
                graph.values().size() != stats.graphValuesBefore + plans.size())
            {
                error = "activity-schedule common-owner clone graph growth mismatch";
                return false;
            }

            const auto ownerOf = [&](wolvrix::lib::grh::OperationId opId) -> uint32_t
            {
                return opId.valid() && opId.index < candidate.computeNodeOfOp.size()
                           ? candidate.computeNodeOfOp[opId.index]
                           : kInvalidActivitySupernodeId;
            };
            stats.actualLocalizedPairs = 0;
            for (const auto &plan : plans)
            {
                const auto sourceOp = graph.getOperation(plan.sourceOp);
                const auto cloneOp = graph.getOperation(plan.cloneOp);
                const auto sourceValue = graph.getValue(plan.sourceValue);
                const auto cloneValue = graph.getValue(plan.cloneValue);
                if (sourceOp.kind() != plan.kind || cloneOp.kind() != plan.kind ||
                    std::vector<wolvrix::lib::grh::ValueId>(sourceOp.operands().begin(),
                                                           sourceOp.operands().end()) != plan.operands ||
                    std::vector<wolvrix::lib::grh::ValueId>(cloneOp.operands().begin(),
                                                           cloneOp.operands().end()) != plan.operands ||
                    !localSharedCloneAttrsEqual(sourceOp.attrs(), plan.attrs) ||
                    !localSharedCloneAttrsEqual(cloneOp.attrs(), plan.attrs) ||
                    !localSharedCloneSrcLocEqual(sourceOp.srcLoc(), plan.opSrcLoc) ||
                    !localSharedCloneSrcLocEqual(cloneOp.srcLoc(), plan.opSrcLoc) ||
                    sourceValue.width() != plan.resultWidth ||
                    cloneValue.width() != plan.resultWidth ||
                    sourceValue.isSigned() != plan.resultSigned ||
                    cloneValue.isSigned() != plan.resultSigned ||
                    sourceValue.type() != plan.resultType ||
                    cloneValue.type() != plan.resultType ||
                    !localSharedCloneSrcLocEqual(sourceValue.srcLoc(), plan.resultSrcLoc) ||
                    !localSharedCloneSrcLocEqual(cloneValue.srcLoc(), plan.resultSrcLoc))
                {
                    error = "activity-schedule common-owner clone metadata mismatch";
                    return false;
                }
                if (!localSharedCloneUsersEqual(sourceValue.users(), plan.originalUses) ||
                    !localSharedCloneUsersEqual(cloneValue.users(), plan.cloneUses))
                {
                    error = "activity-schedule common-owner clone user set mismatch";
                    return false;
                }
                const uint32_t sourceOwner = ownerOf(plan.sourceOp);
                const uint32_t cloneOwner = ownerOf(plan.cloneOp);
                if (sourceOwner == kInvalidActivitySupernodeId ||
                    cloneOwner == kInvalidActivitySupernodeId || sourceOwner == cloneOwner)
                {
                    error = "activity-schedule common-owner clone owner mismatch";
                    return false;
                }
                stats.actualLocalizedPairs += 2;
            }

            if (candidate.computeTopoOrder.size() != candidate.computeNodes.size())
            {
                error = "activity-schedule common-owner clone compute topo size mismatch";
                return false;
            }
            std::vector<uint32_t> topoPos(candidate.computeNodes.size(),
                                          kInvalidActivitySupernodeId);
            for (uint32_t pos = 0; pos < candidate.computeTopoOrder.size(); ++pos)
            {
                const uint32_t nodeId = candidate.computeTopoOrder[pos];
                if (nodeId >= topoPos.size() ||
                    topoPos[nodeId] != kInvalidActivitySupernodeId)
                {
                    error = "activity-schedule common-owner clone compute topo invalid";
                    return false;
                }
                topoPos[nodeId] = pos;
            }
            for (uint32_t source = 0; source < candidate.computeDag.size(); ++source)
            {
                for (const uint32_t target : candidate.computeDag[source])
                {
                    if (target >= topoPos.size() || topoPos[source] >= topoPos[target])
                    {
                        error = "activity-schedule common-owner clone compute topo edge invalid";
                        return false;
                    }
                }
            }
            return true;
        }

        bool exportComputeDagJson(const wolvrix::lib::grh::Graph &graph,
                                  const ActivityScheduleOptions &options,
                                  const ComputeRewriteBuild &rewrite,
                                  std::string &error)
        {
            if (options.exportComputeDagPath.empty())
            {
                return true;
            }

            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;

            std::vector<OperationId> ops;
            std::unordered_set<OperationId, OperationIdHash> seenOps;
            for (const uint32_t computeNodeId : rewrite.computeTopoOrder)
            {
                if (computeNodeId >= rewrite.computeNodes.size())
                {
                    continue;
                }
                for (const OperationId opId : rewrite.computeNodes[computeNodeId].ops)
                {
                    if (opId.valid() && seenOps.insert(opId).second)
                    {
                        ops.push_back(opId);
                    }
                }
            }

            std::size_t maxOpIndex = 0;
            for (const OperationId opId : ops)
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            std::vector<uint32_t> nodeOfOp(maxOpIndex + 1, kInvalidActivitySupernodeId);
            for (uint32_t nodeId = 0; nodeId < ops.size(); ++nodeId)
            {
                if (ops[nodeId].index < nodeOfOp.size())
                {
                    nodeOfOp[ops[nodeId].index] = nodeId;
                }
            }

            std::map<std::pair<uint32_t, uint32_t>, std::vector<ValueId>> edgeValues;
            for (uint32_t dstNode = 0; dstNode < ops.size(); ++dstNode)
            {
                const OperationId dstOp = ops[dstNode];
                for (const ValueId operand : graph.opOperands(dstOp))
                {
                    const OperationId srcOp = graph.valueDef(operand);
                    if (!srcOp.valid() || srcOp.index >= nodeOfOp.size())
                    {
                        continue;
                    }
                    const uint32_t srcNode = nodeOfOp[srcOp.index];
                    if (srcNode == kInvalidActivitySupernodeId || srcNode == dstNode)
                    {
                        continue;
                    }
                    auto &values = edgeValues[{srcNode, dstNode}];
                    if (std::find(values.begin(), values.end(), operand) == values.end())
                    {
                        values.push_back(operand);
                    }
                }
            }

            std::vector<std::vector<uint32_t>> opDag(ops.size());
            for (const auto &[pair, values] : edgeValues)
            {
                (void)values;
                if (pair.first < opDag.size() && pair.second < opDag.size())
                {
                    opDag[pair.first].push_back(pair.second);
                }
            }
            for (auto &succs : opDag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            std::vector<uint32_t> opTopoOrder;
            try
            {
                opTopoOrder = topoOrderForDag(opDag);
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule op-level compute DAG topo failed: ") + ex.what();
                return false;
            }
            if (opTopoOrder.size() != ops.size())
            {
                error = "activity-schedule op-level compute DAG topo failed: missing ops";
                return false;
            }
            std::vector<uint32_t> oldToNew(ops.size(), kInvalidActivitySupernodeId);
            for (uint32_t newNode = 0; newNode < opTopoOrder.size(); ++newNode)
            {
                oldToNew[opTopoOrder[newNode]] = newNode;
            }
            std::map<std::pair<uint32_t, uint32_t>, std::vector<ValueId>> topoEdgeValues;
            for (const auto &[pair, values] : edgeValues)
            {
                const uint32_t src = pair.first < oldToNew.size() ? oldToNew[pair.first]
                                                                  : kInvalidActivitySupernodeId;
                const uint32_t dst = pair.second < oldToNew.size() ? oldToNew[pair.second]
                                                                   : kInvalidActivitySupernodeId;
                if (src == kInvalidActivitySupernodeId || dst == kInvalidActivitySupernodeId)
                {
                    continue;
                }
                topoEdgeValues[{src, dst}] = values;
            }
            edgeValues = std::move(topoEdgeValues);

            const auto valueBitWidth = [](int32_t width) -> std::size_t {
                return width > 0 ? static_cast<std::size_t>(width) : std::size_t{1};
            };
            const auto edgeWeightForValues = [&](const std::vector<ValueId> &values) -> std::size_t {
                std::size_t bits = 0;
                for (const ValueId value : values)
                {
                    bits += valueBitWidth(graph.getValue(value).width());
                }
                return std::max<std::size_t>(std::size_t{1}, (bits + 63) / 64);
            };

            std::size_t edgeWeightTotal = 0;
            for (const auto &[pair, values] : edgeValues)
            {
                (void)pair;
                edgeWeightTotal += edgeWeightForValues(values);
            }

            std::ostringstream out;
            out << "{\n";
            out << "  \"format\":\"wolvrix.compute-op-dag.v1\",\n";
            out << "  \"graph_id\":\"" << escapeJsonString(std::string(graph.symbol()) + ".activity_compute") << "\",\n";
            out << "  \"source\":{\"pass\":\"activity-schedule\",\"path\":\""
                << escapeJsonString(options.path) << "\"},\n";
            out << "  \"options\":{\"node_granularity\":\"op\",\"edge_weight\":\"value_bitwidth_words\"},\n";
            out << "  \"stats\":{\"nodes\":" << ops.size()
                << ",\"edges\":" << edgeValues.size()
                << ",\"edge_weight_total\":" << edgeWeightTotal << "},\n";
            out << "  \"nodes\":[\n";
            for (uint32_t nodeId = 0; nodeId < ops.size(); ++nodeId)
            {
                const OperationId opId = ops[opTopoOrder[nodeId]];
                const auto op = graph.getOperation(opId);
                out << "    {\"id\":" << nodeId
                    << ",\"op_id\":" << opId.index
                    << ",\"kind\":\"" << escapeJsonString(std::string(wolvrix::lib::grh::toString(op.kind())))
                    << "\",\"symbol\":\"" << escapeJsonString(std::string(op.symbolText()))
                    << "\",\"topo_pos\":" << nodeId
                    << ",\"attrs\":{\"granularity\":\"op\""
                    << "}}";
                if (nodeId + 1 != ops.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ],\n";
            out << "  \"edges\":[\n";
            std::size_t edgeIndex = 0;
            for (const auto &[pair, values] : edgeValues)
            {
                const std::size_t edgeWeight = edgeWeightForValues(values);
                out << "    {\"src\":" << pair.first
                    << ",\"dst\":" << pair.second
                    << ",\"weight\":" << edgeWeight
                    << ",\"values\":[";
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    if (i != 0)
                    {
                        out << ",";
                    }
                    const auto valueInfo = graph.getValue(values[i]);
                    const std::size_t width = valueBitWidth(valueInfo.width());
                    out << "{\"id\":" << values[i].index
                        << ",\"width\":" << width << "}";
                }
                out << "]}";
                if (++edgeIndex != edgeValues.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]\n";
            out << "}\n";

            try
            {
                const std::filesystem::path path(options.exportComputeDagPath);
                if (path.has_parent_path())
                {
                    std::filesystem::create_directories(path.parent_path());
                }
                std::ofstream file(options.exportComputeDagPath);
                if (!file)
                {
                    error = "activity-schedule failed to open compute DAG export path: " +
                            options.exportComputeDagPath;
                    return false;
                }
                file << out.str();
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule compute DAG export failed: ") + ex.what();
                return false;
            }
            return true;
        }

        bool materializeComputeNodeSchedule(const wolvrix::lib::grh::Graph &graph,
                                            const ActivityScheduleOptions &options,
                                            const ActivityOpData &opData,
                                            ComputeRewriteBuild &rewrite,
                                            ActivityScheduleBuild &build,
                                            ComputeNodeMaterializePerfStats *perf,
                                            std::string &error)
        {
            ComputeNodeMaterializePerfStats fallbackPerf;
            if (perf == nullptr)
            {
                perf = &fallbackPerf;
            }
            const std::size_t maxOpsPerComputeSupernode = options.maxOpInComputeSupernode;
            const std::size_t maxOpsPerSplitComputeNode =
                options.splitOversizeComputeNodeMaxOps != 0
                    ? options.splitOversizeComputeNodeMaxOps
                    : maxOpsPerComputeSupernode;
            const std::vector<uint32_t> nodeOpSizes = computeNodeOpSizes(rewrite);
            const auto initClustersStart = std::chrono::steady_clock::now();
            std::vector<uint32_t> nodeTopoPos(rewrite.computeNodes.size(), kInvalidActivitySupernodeId);
            for (uint32_t pos = 0; pos < rewrite.computeTopoOrder.size(); ++pos)
            {
                const uint32_t node = rewrite.computeTopoOrder[pos];
                if (node < nodeTopoPos.size())
                {
                    nodeTopoPos[node] = pos;
                }
            }
            std::vector<std::vector<uint32_t>> clusters;
            clusters.reserve(rewrite.computeNodes.size());
            for (const auto node : rewrite.computeTopoOrder)
            {
                clusters.push_back(std::vector<uint32_t>{node});
            }
            clusters = canonicalizeNodeClusters(std::move(clusters), nodeTopoPos);
            if (perf)
            {
                perf->initClustersMs = elapsedMs(initClustersStart);
                perf->clustersBeforeCoarsen = clusters.size();
            }

            const auto topoBeforeStart = std::chrono::steady_clock::now();
            if (!orderNodeClustersTopologically(clusters, rewrite.computeDag, rewrite.computeNodes.size(), &rewrite, &graph))
            {
                error = "activity-schedule compute-node cluster topo failed before coarsen";
                return false;
            }
            if (perf)
            {
                perf->topoBeforeCoarsenMs = elapsedMs(topoBeforeStart);
            }

            const auto coarsenStart = std::chrono::steady_clock::now();
            if (options.enableCoarsen)
            {
                const std::size_t coarsenMaxOps =
                    maxOpsPerComputeSupernode == 0 ? std::numeric_limits<std::size_t>::max()
                                                   : maxOpsPerComputeSupernode;
                bool changed = true;
                std::size_t tailIterations = 0;
                while (changed)
                {
                    const auto iterStart = std::chrono::steady_clock::now();
                    const std::size_t clustersBeforeIter = clusters.size();
                    changed = false;
                    bool out1Changed = false;
                    bool in1Changed = false;
                    bool siblingsChanged = false;
                    if (options.enableChainMerge)
                    {
                        const std::size_t clustersBeforeOut1 = clusters.size();
                        out1Changed = tryMergeNodeOut1(clusters,
                                                       rewrite.computeDag,
                                                       rewrite.computeNodes.size(),
                                                       nodeTopoPos,
                                                       nodeOpSizes,
                                                       coarsenMaxOps,
                                                       rewrite,
                                                       graph);
                        if (out1Changed && perf)
                        {
                            perf->coarsenOut1Merges += clustersBeforeOut1 >= clusters.size()
                                                           ? clustersBeforeOut1 - clusters.size()
                                                           : 0;
                        }
                        changed = out1Changed || changed;

                        const std::size_t clustersBeforeIn1 = clusters.size();
                        in1Changed = tryMergeNodeIn1(clusters,
                                                     rewrite.computeDag,
                                                     rewrite.computeNodes.size(),
                                                     nodeTopoPos,
                                                     nodeOpSizes,
                                                     coarsenMaxOps,
                                                     rewrite,
                                                     graph);
                        if (in1Changed && perf)
                        {
                            perf->coarsenIn1Merges += clustersBeforeIn1 >= clusters.size()
                                                          ? clustersBeforeIn1 - clusters.size()
                                                          : 0;
                        }
                        changed = in1Changed || changed;
                    }
                    const std::size_t clustersBeforeSiblings = clusters.size();
                    siblingsChanged = tryMergeNodeSiblings(clusters,
                                                           rewrite.computeDag,
                                                           rewrite.computeNodes.size(),
                                                           nodeTopoPos,
                                                           nodeOpSizes,
                                                           coarsenMaxOps,
                                                           rewrite,
                                                           graph);
                    if (siblingsChanged && perf)
                    {
                        perf->coarsenSiblingMerges += clustersBeforeSiblings >= clusters.size()
                                                          ? clustersBeforeSiblings - clusters.size()
                                                          : 0;
                    }
                    changed = siblingsChanged || changed;
                    if (perf)
                    {
                        const std::size_t clustersAfterIter = clusters.size();
                        const std::size_t clusterDelta =
                            clustersBeforeIter >= clustersAfterIter ? (clustersBeforeIter - clustersAfterIter) : 0;
                        const bool smallDeltaTail =
                            clustersBeforeIter >= kComputeNodeCoarsenTailLargeClusterThreshold &&
                            clusterDelta < kComputeNodeCoarsenTailMaxClusterDeltaExclusive;
                        if (changed && smallDeltaTail)
                        {
                            ++tailIterations;
                        }
                        else
                        {
                            tailIterations = 0;
                        }
                        const bool tailStopped =
                            tailIterations >= kComputeNodeCoarsenTailMaxConsecutiveIters;
                        if (tailStopped)
                        {
                            changed = false;
                            perf->coarsenTailStopped = true;
                            perf->coarsenTailIterations = tailIterations;
                        }
                        ++perf->coarsenIterations;
                        perf->coarsenIterationStats.push_back({
                            .iteration = perf->coarsenIterations,
                            .clusters = clustersAfterIter,
                            .clusterDelta = clusterDelta,
                            .changed = changed,
                            .out1Changed = out1Changed,
                            .in1Changed = in1Changed,
                            .siblingsChanged = siblingsChanged,
                            .tailStopped = tailStopped,
                            .elapsedMs = elapsedMs(iterStart),
                        });
                    }
                    else
                    {
                        const std::size_t clustersAfterIter = clusters.size();
                        const std::size_t clusterDelta =
                            clustersBeforeIter >= clustersAfterIter ? (clustersBeforeIter - clustersAfterIter) : 0;
                        const bool smallDeltaTail =
                            clustersBeforeIter >= kComputeNodeCoarsenTailLargeClusterThreshold &&
                            clusterDelta < kComputeNodeCoarsenTailMaxClusterDeltaExclusive;
                        if (changed && smallDeltaTail)
                        {
                            ++tailIterations;
                        }
                        else
                        {
                            tailIterations = 0;
                        }
                        if (tailIterations >= kComputeNodeCoarsenTailMaxConsecutiveIters)
                        {
                            changed = false;
                        }
                    }
                }
            }
            if (perf)
            {
                perf->coarsenMs = elapsedMs(coarsenStart);
                perf->clustersAfterCoarsen = clusters.size();
            }

            const auto topoAfterStart = std::chrono::steady_clock::now();
            if (!orderNodeClustersTopologically(clusters, rewrite.computeDag, rewrite.computeNodes.size(), &rewrite, &graph))
            {
                error = "activity-schedule compute-node cluster topo failed after coarsen";
                return false;
            }
            if (perf)
            {
                perf->topoAfterCoarsenMs = elapsedMs(topoAfterStart);
            }

            const auto buildClusterViewStart = std::chrono::steady_clock::now();
            NodeClusterView clusterView =
                buildNodeClusterView(clusters, rewrite.computeDag, rewrite.computeNodes.size());
            if (perf)
            {
                perf->buildClusterViewMs = elapsedMs(buildClusterViewStart);
            }
            const std::string coarsenShape =
                summarizeCoarsenClusterShape(clusterView, rewrite, graph, nodeOpSizes);
            std::fprintf(stderr,
                         "activity-schedule: activity-schedule compute-node coarsen shape: %s\n",
                         coarsenShape.c_str());

            const auto dpSegmentStart = std::chrono::steady_clock::now();
            ClusterValueEdges clusterValueEdges = buildClusterValueEdges(clusterView, rewrite, graph);
            recordInitialComputeSupernodeStats(clusterView,
                                               clusterValueEdges,
                                               rewrite,
                                               graph,
                                               nodeOpSizes);
            std::vector<std::vector<uint32_t>> segments =
                buildComputeSupernodeSegments(clusterView,
                                              clusterValueEdges,
                                              nodeOpSizes,
                                              maxOpsPerComputeSupernode,
                                              nullptr,
                                              static_cast<double>(options.dpSegmentPenaltyPpm) /
                                                  1000000.0);
            if (perf)
            {
                perf->dpSegmentMs = elapsedMs(dpSegmentStart);
                perf->segments = segments.size();
            }

            if (options.kahnLevelPackPolicy != "off")
            {
                const auto packStart = std::chrono::steady_clock::now();
                if (!packKahnLevelClusters(clusters,
                                           clusterView,
                                           clusterValueEdges,
                                           segments,
                                           nodeOpSizes,
                                           rewrite,
                                           graph,
                                           options,
                                           maxOpsPerComputeSupernode,
                                           maxOpsPerSplitComputeNode,
                                           *perf,
                                           error))
                {
                    return false;
                }
                perf->kahnLevelPackMs = elapsedMs(packStart);
                perf->segments = segments.size();
            }
            if (options.postDpRefinePolicy != "off")
            {
                const auto refineStart = std::chrono::steady_clock::now();
                if (!refinePostDpSegments(clusterView,
                                          clusterValueEdges,
                                          nodeOpSizes,
                                          rewrite,
                                          segments,
                                          options,
                                          maxOpsPerComputeSupernode,
                                          maxOpsPerSplitComputeNode,
                                          *perf,
                                          error))
                {
                    return false;
                }
                perf->postDpRefineMs = elapsedMs(refineStart);
            }

            const auto flattenSegmentsStart = std::chrono::steady_clock::now();
            const auto computeSupernodes = flattenNodeSegments(clusterView, segments, nodeTopoPos);
            if (perf)
            {
                perf->flattenSegmentsMs = elapsedMs(flattenSegmentsStart);
                perf->computeSupernodes = computeSupernodes.size();
            }

            const auto buildFinalSupernodesStart = std::chrono::steady_clock::now();
            build = ActivityScheduleBuild{};
            build.supernodeToOps.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            build.supernodeKinds.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            build.computeNodesBySupernode.reserve(computeSupernodes.size() + rewrite.commitNodes.size());
            std::vector<uint32_t> splitOwnerComputeNodeBySupernode;
            std::vector<uint32_t> splitOrdinalBySupernode;
            std::vector<uint32_t> splitCountByComputeNode(rewrite.computeNodes.size(), 0);
            auto noteNonSplitSupernode = [&]()
            {
                splitOwnerComputeNodeBySupernode.push_back(kInvalidActivitySupernodeId);
                splitOrdinalBySupernode.push_back(kInvalidActivitySupernodeId);
            };
            for (uint32_t segmentId = 0; segmentId < computeSupernodes.size(); ++segmentId)
            {
                std::vector<wolvrix::lib::grh::OperationId> ops;
                std::vector<uint32_t> supernodeComputeNodes;
                auto flushComputeSupernode = [&]() -> bool
                {
                    if (ops.empty())
                    {
                        return true;
                    }
                    std::vector<wolvrix::lib::grh::OperationId> orderedOps;
                    if (!topoSortLocalOps(graph, ops, orderedOps, error))
                    {
                        return false;
                    }
                    build.supernodeToOps.push_back(std::move(orderedOps));
                    build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Compute);
                    build.computeNodesBySupernode.push_back(supernodeComputeNodes);
                    noteNonSplitSupernode();
                    ops.clear();
                    supernodeComputeNodes.clear();
                    return true;
                };

                for (const auto computeNodeId : computeSupernodes[segmentId])
                {
                    if (computeNodeId >= rewrite.computeNodes.size())
                    {
                        continue;
                    }
                    const auto &nodeOps = rewrite.computeNodes[computeNodeId].ops;
                    if (options.splitOversizeComputeNodes &&
                        maxOpsPerSplitComputeNode != 0 &&
                        nodeOps.size() > maxOpsPerSplitComputeNode)
                    {
                        std::vector<wolvrix::lib::grh::OperationId> orderedNodeOps;
                        if (!topoSortLocalOps(graph, nodeOps, orderedNodeOps, error))
                        {
                            return false;
                        }
                        ++perf->splitOversizeComputeNodes;
                        for (std::size_t begin = 0; begin < orderedNodeOps.size(); begin += maxOpsPerSplitComputeNode)
                        {
                            if (!flushComputeSupernode())
                            {
                                return false;
                            }
                            const std::size_t end =
                                std::min(orderedNodeOps.size(), begin + maxOpsPerSplitComputeNode);
                            std::vector<wolvrix::lib::grh::OperationId> chunkOps(
                                orderedNodeOps.begin() + static_cast<std::ptrdiff_t>(begin),
                                orderedNodeOps.begin() + static_cast<std::ptrdiff_t>(end));
                            build.supernodeToOps.push_back(std::move(chunkOps));
                            build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Compute);
                            build.computeNodesBySupernode.push_back({computeNodeId});
                            splitOwnerComputeNodeBySupernode.push_back(computeNodeId);
                            splitOrdinalBySupernode.push_back(splitCountByComputeNode[computeNodeId]++);
                            ++perf->splitOversizeComputeNodeSupernodes;
                        }
                        continue;
                    }
                    ops.insert(ops.end(), nodeOps.begin(), nodeOps.end());
                    supernodeComputeNodes.push_back(computeNodeId);
                }
                if (!flushComputeSupernode())
                {
                    return false;
                }
            }
            const uint32_t commitBase = static_cast<uint32_t>(build.supernodeToOps.size());
            for (const auto &commit : rewrite.commitNodes)
            {
                build.supernodeToOps.push_back(commit.ops);
                build.supernodeKinds.push_back(ActivityScheduleSupernodeKind::Commit);
                build.computeNodesBySupernode.push_back({});
                noteNonSplitSupernode();
            }
            if (perf)
            {
                perf->buildFinalSupernodesMs = elapsedMs(buildFinalSupernodesStart);
            }

            const auto buildFinalDagStart = std::chrono::steady_clock::now();
            std::size_t maxOpIndex = 0;
            for (const auto opId : graph.operations())
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            build.opToSupernode.assign(maxOpIndex, kInvalidActivitySupernodeId);
            std::vector<uint32_t> supernodeOfOp(maxOpIndex + 1, kInvalidActivitySupernodeId);
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    if (opId.index == 0 || opId.index > maxOpIndex)
                    {
                        continue;
                    }
                    build.opToSupernode[opId.index - 1] = supernodeId;
                    supernodeOfOp[opId.index] = supernodeId;
                }
            }

            build.dag.assign(build.supernodeToOps.size(), {});
            if (!graph.values().empty())
            {
                build.valueFanout.assign(graph.values().back().index, {});
                build.valueSourceKind.assign(graph.values().back().index + 1,
                                             wolvrix::lib::grh::OperationKind::kConstant);
                build.valueSourceSupernode.assign(graph.values().back().index + 1, kInvalidActivitySupernodeId);
                for (const auto valueId : graph.values())
                {
                    if (!valueId.valid() || valueId.index >= build.valueSourceKind.size())
                    {
                        continue;
                    }
                    const auto defOpId = graph.valueDef(valueId);
                    if (defOpId.valid())
                    {
                        build.valueSourceKind[valueId.index] = graph.opKind(defOpId);
                        if (defOpId.index > 0 && defOpId.index - 1 < build.opToSupernode.size())
                        {
                            build.valueSourceSupernode[valueId.index] = build.opToSupernode[defOpId.index - 1];
                        }
                    }
                }
            }
            std::unordered_set<uint64_t> seenEdges;
            const auto addValueDependency = [&](wolvrix::lib::grh::ValueId value,
                                                uint32_t to,
                                                bool skipDagEdge = false) {
                if (!value.valid() || to >= build.supernodeToOps.size())
                {
                    return;
                }
                const auto defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    if (!skipDagEdge && value.index > 0 && value.index <= build.valueFanout.size())
                    {
                        build.valueFanout[value.index - 1].push_back(to);
                    }
                    return;
                }
                if (defOp.index >= supernodeOfOp.size())
                {
                    return;
                }
                const uint32_t from = supernodeOfOp[defOp.index];
                if (from == kInvalidActivitySupernodeId || from == to)
                {
                    return;
                }
                if (from < build.supernodeKinds.size() &&
                    build.supernodeKinds[from] == ActivityScheduleSupernodeKind::Commit)
                {
                    return;
                }
                if (!skipDagEdge)
                {
                    const uint64_t packed = (static_cast<uint64_t>(from) << 32) | to;
                    if (seenEdges.insert(packed).second)
                    {
                        build.dag[from].push_back(to);
                    }
                }
                if (!skipDagEdge && value.index > 0 && value.index <= build.valueFanout.size())
                {
                    build.valueFanout[value.index - 1].push_back(to);
                }
            };
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                for (const auto toOpId : build.supernodeToOps[supernodeId])
                {
                    const auto toOp = graph.getOperation(toOpId);
                    for (const auto operand : toOp.operands())
                    {
                        const auto defOp = graph.valueDef(operand);
                        if (!defOp.valid() || defOp.index >= supernodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t from = supernodeOfOp[defOp.index];
                        const uint32_t to = supernodeId;
                        bool skipDagEdge = false;
                        if (defOp.index < rewrite.computeNodeOfOp.size() &&
                            toOpId.index < rewrite.computeNodeOfOp.size())
                        {
                            const uint32_t defComputeNode = rewrite.computeNodeOfOp[defOp.index];
                            const uint32_t useComputeNode = rewrite.computeNodeOfOp[toOpId.index];
                            if (defComputeNode != kInvalidActivitySupernodeId &&
                                defComputeNode == useComputeNode &&
                                from != to)
                            {
                                const bool splitForward =
                                    from < splitOwnerComputeNodeBySupernode.size() &&
                                    to < splitOwnerComputeNodeBySupernode.size() &&
                                    splitOwnerComputeNodeBySupernode[from] == defComputeNode &&
                                    splitOwnerComputeNodeBySupernode[to] == defComputeNode &&
                                    from < splitOrdinalBySupernode.size() &&
                                    to < splitOrdinalBySupernode.size() &&
                                    splitOrdinalBySupernode[from] < splitOrdinalBySupernode[to];
                                skipDagEdge = !splitForward;
                            }
                        }
                        if (from == kInvalidActivitySupernodeId || from == to)
                        {
                            continue;
                        }
                        if (from < build.supernodeKinds.size() &&
                            build.supernodeKinds[from] == ActivityScheduleSupernodeKind::Commit)
                        {
                            continue;
                        }
                        if (!skipDagEdge)
                        {
                            const uint64_t packed = (static_cast<uint64_t>(from) << 32) | to;
                            if (seenEdges.insert(packed).second)
                            {
                                build.dag[from].push_back(to);
                            }
                        }
                        if (!skipDagEdge && operand.index > 0 && operand.index <= build.valueFanout.size())
                        {
                            build.valueFanout[operand.index - 1].push_back(to);
                        }
                    }
                    if (isRegToMemIntentSlice(toOp))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, toOp))
                        {
                            addValueDependency(*indexValue, supernodeId);
                        }
                    }
                }
            }
            for (auto &succs : build.dag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            for (auto &fanout : build.valueFanout)
            {
                std::sort(fanout.begin(), fanout.end());
                fanout.erase(std::unique(fanout.begin(), fanout.end()), fanout.end());
            }
            if ((perf->postDpRefineEvaluated || perf->kahnLevelPackEvaluated) &&
                perf->splitOversizeComputeNodes == 0)
            {
                std::size_t finalComputeBae = 0;
                for (const auto &fanout : build.valueFanout)
                {
                    for (const uint32_t target : fanout)
                    {
                        if (target < build.supernodeKinds.size() &&
                            build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Compute)
                        {
                            ++finalComputeBae;
                        }
                    }
                }
                std::size_t finalDagEdges = 0;
                for (const auto &succs : build.dag)
                {
                    finalDagEdges += succs.size();
                }
                const std::size_t expectedComputeBae =
                    perf->postDpRefineEvaluated
                        ? perf->postDpRefineAfterComputeBae
                        : perf->kahnLevelPackAfterComputeBae;
                const std::size_t expectedDagEdges =
                    perf->postDpRefineEvaluated
                        ? perf->postDpRefineAfterDagEdges
                        : perf->kahnLevelPackAfterDagEdges;
                if (finalComputeBae != expectedComputeBae ||
                    finalDagEdges != expectedDagEdges)
                {
                    error = std::string("activity-schedule ") +
                            (perf->postDpRefineEvaluated ? "post-DP refine" : "Kahn-level pack") +
                            " final recount mismatch: "
                            "predicted_compute_bae=" +
                            std::to_string(expectedComputeBae) +
                            " final_compute_bae=" + std::to_string(finalComputeBae) +
                            " predicted_dag_edges=" +
                            std::to_string(expectedDagEdges) +
                            " final_dag_edges=" + std::to_string(finalDagEdges);
                    return false;
                }
            }
            if (perf)
            {
                perf->buildFinalDagMs = elapsedMs(buildFinalDagStart);
            }

            const auto buildStateReadSetsStart = std::chrono::steady_clock::now();
            for (uint32_t supernodeId = 0; supernodeId < build.supernodeToOps.size(); ++supernodeId)
            {
                if (supernodeId < build.supernodeKinds.size() &&
                    build.supernodeKinds[supernodeId] == ActivityScheduleSupernodeKind::Commit)
                {
                    continue;
                }
                for (const auto opId : build.supernodeToOps[supernodeId])
                {
                    const auto stateSymbol = stateSymbolForReadOp(graph.getOperation(opId));
                    if (stateSymbol && !stateSymbol->empty())
                    {
                        build.stateReadSupernodes[*stateSymbol].push_back(supernodeId);
                    }
                    const auto op = graph.getOperation(opId);
                    if (isRegToMemIntentSlice(op))
                    {
                        for (const auto &storageSymbol : regToMemIntentSliceStorageReadSymbols(graph, op))
                        {
                            build.stateReadSupernodes[storageSymbol].push_back(supernodeId);
                        }
                    }
                }
            }
            for (auto &[_, supernodes] : build.stateReadSupernodes)
            {
                std::sort(supernodes.begin(), supernodes.end());
                supernodes.erase(std::unique(supernodes.begin(), supernodes.end()), supernodes.end());
            }
            if (perf)
            {
                perf->buildStateReadSetsMs = elapsedMs(buildStateReadSetsStart);
            }

            const auto finalTopoStart = std::chrono::steady_clock::now();
            try
            {
                if (options.finalTopoPolicy == "level-op" || options.finalTopoPolicy == "ready-op")
                {
                    const std::vector<std::size_t> layerOrderKeys = minOpIndexBySupernode(build);
                    if (options.finalTopoPolicy == "ready-op")
                    {
                        build.topoOrder = topoOrderForDagReadyStack(build.dag, layerOrderKeys);
                    }
                    else
                    {
                        build.topoOrder = topoOrderForDag(build.dag, &layerOrderKeys);
                    }
                }
                else
                {
                    build.topoOrder = topoOrderForDag(build.dag);
                }
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule final topo failed: ") + ex.what() + " " +
                        describeFinalScheduleCycle(graph,
                                                   rewrite,
                                                   build,
                                                   build.computeNodesBySupernode,
                                                   supernodeOfOp);
                return false;
            }
            if (build.topoOrder.size() != build.supernodeToOps.size())
            {
                error = "activity-schedule final topo failed: missing supernodes";
                return false;
            }
            if (perf)
            {
                perf->finalTopoMs = elapsedMs(finalTopoStart);
            }
            (void)commitBase;
            return true;
        }

        struct FinalFaninPullbackCandidate
        {
            uint32_t computeNode = kInvalidActivitySupernodeId;
            uint32_t source = kInvalidActivitySupernodeId;
            uint32_t target = kInvalidActivitySupernodeId;
            std::size_t opCount = 0;
            std::size_t maxValueWidth = 0;
            std::size_t removableInputs = 0;
            std::size_t gain = 0;
            std::vector<wolvrix::lib::grh::OperationId> touchedOps;
            std::vector<wolvrix::lib::grh::ValueId> touchedValues;
        };

        struct FinalFaninPullbackProbeStats
        {
            using CountMap = ActivityScheduleSummaryStats::KindCountMap;

            std::size_t scanned = 0;
            std::size_t pure = 0;
            std::size_t commonSource = 0;
            std::size_t exactEligible = 0;
            std::size_t selected = 0;
            std::size_t eligibleProjectedBaeGain = 0;
            std::size_t projectedBaeGain = 0;
            std::size_t movedOps = 0;
            std::size_t movedOpLimit = 0;
            std::size_t rejectedOwner = 0;
            std::size_t rejectedNodeSize = 0;
            std::size_t rejectedRestricted = 0;
            std::size_t rejectedKind = 0;
            std::size_t rejectedSideEffect = 0;
            std::size_t rejectedCloneForbidden = 0;
            std::size_t rejectedShape = 0;
            std::size_t rejectedWidth = 0;
            std::size_t rejectedPortOrDeclared = 0;
            std::size_t rejectedLiveoutCount = 0;
            std::size_t rejectedCone = 0;
            std::size_t rejectedNoDef = 0;
            std::size_t rejectedTargetPredecessor = 0;
            std::size_t rejectedThirdSource = 0;
            std::size_t rejectedSourceKind = 0;
            std::size_t rejectedExternalConsumer = 0;
            std::size_t rejectedTargetEmpty = 0;
            std::size_t rejectedCapacity = 0;
            std::size_t rejectedNoRemovableInput = 0;
            std::size_t rejectedMinGain = 0;
            std::size_t rejectedSelectionMoveLimit = 0;
            std::size_t rejectedSelectionBudget = 0;
            std::size_t rejectedSelectionCapacity = 0;
            std::size_t rejectedSelectionTargetEmpty = 0;
            std::size_t rejectedSelectionNodeOverlap = 0;
            std::size_t rejectedSelectionValueOverlap = 0;
            bool skippedOversize = false;
            bool skippedFinalTopo = false;
            CountMap eligibleByGain;
            CountMap eligibleByNodeOps;
            CountMap eligibleByMaxWidth;
            CountMap selectedByGain;
            CountMap selectedByNodeOps;
            CountMap selectedByMaxWidth;
        };

        FinalFaninPullbackProbeStats evaluateFinalFaninPullback(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ComputeRewriteBuild &rewrite,
            const ActivityScheduleBuild &build,
            const ComputeNodeMaterializePerfStats &materializePerf,
            std::vector<FinalFaninPullbackCandidate> *selectedCandidates = nullptr)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;
            using wolvrix::lib::grh::ValueId;
            using wolvrix::lib::grh::ValueIdHash;
            using wolvrix::lib::grh::ValueType;

            FinalFaninPullbackProbeStats stats;
            if (selectedCandidates != nullptr)
            {
                selectedCandidates->clear();
            }
            const std::size_t computeSupernodeCap =
                options.maxOpInComputeSupernode == 0
                    ? std::numeric_limits<std::size_t>::max()
                    : options.maxOpInComputeSupernode;
            const auto ppmLimit =
                (static_cast<unsigned __int128>(rewrite.stats.computeNodeOpsTotal) *
                 options.finalFaninPullbackMaxMovedOpPpm) /
                1000000U;
            stats.movedOpLimit =
                ppmLimit > std::numeric_limits<std::size_t>::max()
                    ? std::numeric_limits<std::size_t>::max()
                    : static_cast<std::size_t>(ppmLimit);

            if (materializePerf.splitOversizeComputeNodes != 0)
            {
                stats.skippedOversize = true;
                return stats;
            }
            if (options.finalTopoPolicy != "level-id")
            {
                stats.skippedFinalTopo = true;
                return stats;
            }

            const auto ownerOfOp = [&](OperationId opId)
            {
                if (!opId.valid() || opId.index - 1 >= build.opToSupernode.size())
                {
                    return kInvalidActivitySupernodeId;
                }
                return build.opToSupernode[opId.index - 1];
            };
            const auto isComputeSupernode = [&](uint32_t supernode)
            {
                return supernode < build.supernodeKinds.size() &&
                       build.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Compute;
            };

            std::vector<uint32_t> ownerByComputeNode(rewrite.computeNodes.size(),
                                                     kInvalidActivitySupernodeId);
            std::vector<bool> ambiguousComputeNodeOwner(rewrite.computeNodes.size(), false);
            for (uint32_t supernode = 0; supernode < build.computeNodesBySupernode.size(); ++supernode)
            {
                for (const uint32_t computeNode : build.computeNodesBySupernode[supernode])
                {
                    if (computeNode >= ownerByComputeNode.size())
                    {
                        continue;
                    }
                    if (ownerByComputeNode[computeNode] != kInvalidActivitySupernodeId &&
                        ownerByComputeNode[computeNode] != supernode)
                    {
                        ambiguousComputeNodeOwner[computeNode] = true;
                    }
                    else
                    {
                        ownerByComputeNode[computeNode] = supernode;
                    }
                }
            }

            std::unordered_set<ValueId, ValueIdHash> portValues;
            for (const auto &port : graph.outputPorts())
            {
                portValues.insert(port.value);
            }
            for (const auto &port : graph.inoutPorts())
            {
                portValues.insert(port.in);
                portValues.insert(port.out);
                portValues.insert(port.oe);
            }

            std::unordered_map<ValueId, std::vector<OperationId>, ValueIdHash> implicitIndexUsers;
            for (const OperationId opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                {
                    implicitIndexUsers[*indexValue].push_back(opId);
                }
            }

            std::vector<FinalFaninPullbackCandidate> candidates;
            candidates.reserve(rewrite.computeNodes.size() / 16 + 1);
            for (uint32_t computeNodeId = 0; computeNodeId < rewrite.computeNodes.size(); ++computeNodeId)
            {
                ++stats.scanned;
                const auto &node = rewrite.computeNodes[computeNodeId];
                const uint32_t target = ownerByComputeNode[computeNodeId];
                if (ambiguousComputeNodeOwner[computeNodeId] ||
                    target == kInvalidActivitySupernodeId ||
                    !isComputeSupernode(target) ||
                    target >= build.supernodeToOps.size())
                {
                    ++stats.rejectedOwner;
                    continue;
                }
                if (node.ops.empty() || node.ops.size() > options.finalFaninPullbackMaxNodeOps)
                {
                    ++stats.rejectedNodeSize;
                    continue;
                }
                if (node.indivisible || !node.intentGroup.empty())
                {
                    ++stats.rejectedRestricted;
                    continue;
                }

                std::unordered_set<OperationId, OperationIdHash> nodeOps(node.ops.begin(), node.ops.end());
                bool invalidOwner = false;
                bool invalidKind = false;
                bool sideEffect = false;
                bool cloneForbidden = false;
                bool invalidShape = false;
                bool invalidWidth = false;
                bool anchoredResult = false;
                std::size_t maxValueWidth = 0;
                std::unordered_set<ValueId, ValueIdHash> nodeResults;
                std::unordered_set<ValueId, ValueIdHash> touchedValues;
                for (const OperationId opId : node.ops)
                {
                    invalidOwner |= ownerOfOp(opId) != target;
                    const auto op = graph.getOperation(opId);
                    invalidKind |= !isCloneableLocalSharedComputeOpKind(op.kind());
                    sideEffect |= opHasSideEffects(op);
                    cloneForbidden |= hasLocalSharedCloneForbiddenAttr(op);
                    invalidShape |= op.results().empty();
                    for (const ValueId value : op.operands())
                    {
                        touchedValues.insert(value);
                        const auto valueInfo = graph.getValue(value);
                        if (valueInfo.type() != ValueType::Logic || valueInfo.width() <= 0)
                        {
                            invalidWidth = true;
                            continue;
                        }
                        const std::size_t width = static_cast<std::size_t>(valueInfo.width());
                        maxValueWidth = std::max(maxValueWidth, width);
                        invalidWidth |= width > options.finalFaninPullbackMaxValueWidth;
                    }
                    for (const ValueId value : op.results())
                    {
                        nodeResults.insert(value);
                        touchedValues.insert(value);
                        const auto valueInfo = graph.getValue(value);
                        if (valueInfo.type() != ValueType::Logic || valueInfo.width() <= 0)
                        {
                            invalidWidth = true;
                        }
                        else
                        {
                            const std::size_t width = static_cast<std::size_t>(valueInfo.width());
                            maxValueWidth = std::max(maxValueWidth, width);
                            invalidWidth |= width > options.finalFaninPullbackMaxValueWidth;
                        }
                        anchoredResult |= isDeclaredCutValue(graph, rewrite.canonicalValues, value) ||
                                          valueInfo.isInput() || valueInfo.isOutput() ||
                                          valueInfo.isInout() || portValues.contains(value);
                    }
                }
                if (invalidOwner)
                {
                    ++stats.rejectedOwner;
                    continue;
                }
                if (invalidKind)
                {
                    ++stats.rejectedKind;
                    continue;
                }
                if (sideEffect)
                {
                    ++stats.rejectedSideEffect;
                    continue;
                }
                if (cloneForbidden)
                {
                    ++stats.rejectedCloneForbidden;
                    continue;
                }
                if (invalidShape)
                {
                    ++stats.rejectedShape;
                    continue;
                }
                if (invalidWidth)
                {
                    ++stats.rejectedWidth;
                    continue;
                }
                ++stats.pure;
                if (anchoredResult)
                {
                    ++stats.rejectedPortOrDeclared;
                    continue;
                }

                std::vector<ValueId> liveouts;
                for (const ValueId value : nodeResults)
                {
                    bool external = false;
                    for (const auto &user : graph.getValue(value).users())
                    {
                        if (!user.operation.valid() || !nodeOps.contains(user.operation))
                        {
                            external = true;
                            break;
                        }
                    }
                    if (!external)
                    {
                        const auto implicitIt = implicitIndexUsers.find(value);
                        if (implicitIt != implicitIndexUsers.end())
                        {
                            external = std::any_of(
                                implicitIt->second.begin(),
                                implicitIt->second.end(),
                                [&](OperationId user) { return !nodeOps.contains(user); });
                        }
                    }
                    if (external)
                    {
                        liveouts.push_back(value);
                    }
                }
                if (liveouts.size() != 1)
                {
                    ++stats.rejectedLiveoutCount;
                    continue;
                }
                const ValueId liveout = liveouts.front();

                std::unordered_set<OperationId, OperationIdHash> reverseCovered;
                std::vector<OperationId> reverseWorklist;
                const OperationId root = graph.valueDef(liveout);
                if (root.valid() && nodeOps.contains(root))
                {
                    reverseCovered.insert(root);
                    reverseWorklist.push_back(root);
                }
                while (!reverseWorklist.empty())
                {
                    const OperationId current = reverseWorklist.back();
                    reverseWorklist.pop_back();
                    for (const ValueId operand : graph.opOperands(current))
                    {
                        const OperationId def = graph.valueDef(operand);
                        if (def.valid() && nodeOps.contains(def) && reverseCovered.insert(def).second)
                        {
                            reverseWorklist.push_back(def);
                        }
                    }
                }
                if (reverseCovered.size() != node.ops.size())
                {
                    ++stats.rejectedCone;
                    continue;
                }

                std::unordered_set<ValueId, ValueIdHash> boundarySet;
                for (const OperationId opId : node.ops)
                {
                    for (const ValueId operand : graph.opOperands(opId))
                    {
                        const OperationId def = graph.valueDef(operand);
                        if (!def.valid() || !nodeOps.contains(def))
                        {
                            boundarySet.insert(operand);
                        }
                    }
                }
                std::vector<ValueId> boundaryInputs(boundarySet.begin(), boundarySet.end());
                std::sort(boundaryInputs.begin(),
                          boundaryInputs.end(),
                          [](ValueId lhs, ValueId rhs) { return lhs.index < rhs.index; });
                uint32_t source = kInvalidActivitySupernodeId;
                bool noDef = false;
                bool targetPredecessor = false;
                bool thirdSource = false;
                bool sourceKind = false;
                for (const ValueId value : boundaryInputs)
                {
                    const OperationId def = graph.valueDef(value);
                    if (!def.valid())
                    {
                        noDef = true;
                        continue;
                    }
                    const uint32_t owner = ownerOfOp(def);
                    if (owner == kInvalidActivitySupernodeId)
                    {
                        noDef = true;
                        continue;
                    }
                    if (owner == target)
                    {
                        targetPredecessor = true;
                        continue;
                    }
                    if (!isComputeSupernode(owner))
                    {
                        sourceKind = true;
                        continue;
                    }
                    if (source == kInvalidActivitySupernodeId)
                    {
                        source = owner;
                    }
                    else if (source != owner)
                    {
                        thirdSource = true;
                    }
                }
                if (noDef)
                {
                    ++stats.rejectedNoDef;
                    continue;
                }
                if (targetPredecessor)
                {
                    ++stats.rejectedTargetPredecessor;
                    continue;
                }
                if (thirdSource)
                {
                    ++stats.rejectedThirdSource;
                    continue;
                }
                if (sourceKind || source == kInvalidActivitySupernodeId || source == target)
                {
                    ++stats.rejectedSourceKind;
                    continue;
                }
                ++stats.commonSource;

                bool invalidExternalConsumer = false;
                bool haveExternalConsumer = false;
                for (const auto &user : graph.getValue(liveout).users())
                {
                    if (user.operation.valid() && nodeOps.contains(user.operation))
                    {
                        continue;
                    }
                    haveExternalConsumer = true;
                    if (!user.operation.valid() || ownerOfOp(user.operation) != target)
                    {
                        invalidExternalConsumer = true;
                    }
                }
                const auto implicitLiveoutIt = implicitIndexUsers.find(liveout);
                if (implicitLiveoutIt != implicitIndexUsers.end())
                {
                    for (const OperationId user : implicitLiveoutIt->second)
                    {
                        if (nodeOps.contains(user))
                        {
                            continue;
                        }
                        haveExternalConsumer = true;
                        if (ownerOfOp(user) != target)
                        {
                            invalidExternalConsumer = true;
                        }
                    }
                }
                if (!haveExternalConsumer || invalidExternalConsumer)
                {
                    ++stats.rejectedExternalConsumer;
                    continue;
                }
                if (build.supernodeToOps[target].size() <= node.ops.size())
                {
                    ++stats.rejectedTargetEmpty;
                    continue;
                }
                if (build.supernodeToOps[source].size() > computeSupernodeCap ||
                    node.ops.size() >
                        computeSupernodeCap - build.supernodeToOps[source].size())
                {
                    ++stats.rejectedCapacity;
                    continue;
                }

                std::size_t removableInputs = 0;
                for (const ValueId value : boundaryInputs)
                {
                    bool remainingTargetUse = false;
                    for (const auto &user : graph.getValue(value).users())
                    {
                        if (user.operation.valid() && !nodeOps.contains(user.operation) &&
                            ownerOfOp(user.operation) == target)
                        {
                            remainingTargetUse = true;
                            break;
                        }
                    }
                    if (!remainingTargetUse)
                    {
                        const auto implicitIt = implicitIndexUsers.find(value);
                        if (implicitIt != implicitIndexUsers.end())
                        {
                            remainingTargetUse = std::any_of(
                                implicitIt->second.begin(),
                                implicitIt->second.end(),
                                [&](OperationId user)
                                {
                                    return !nodeOps.contains(user) && ownerOfOp(user) == target;
                                });
                        }
                    }
                    removableInputs += remainingTargetUse ? 0 : 1;
                }
                if (removableInputs == 0)
                {
                    ++stats.rejectedNoRemovableInput;
                    continue;
                }
                const std::size_t gain = removableInputs - 1;
                if (gain < options.finalFaninPullbackMinGain)
                {
                    ++stats.rejectedMinGain;
                    continue;
                }

                FinalFaninPullbackCandidate candidate;
                candidate.computeNode = computeNodeId;
                candidate.source = source;
                candidate.target = target;
                candidate.opCount = node.ops.size();
                candidate.maxValueWidth = maxValueWidth;
                candidate.removableInputs = removableInputs;
                candidate.gain = gain;
                candidate.touchedOps = node.ops;
                candidate.touchedValues.assign(touchedValues.begin(), touchedValues.end());
                std::sort(candidate.touchedValues.begin(),
                          candidate.touchedValues.end(),
                          [](ValueId lhs, ValueId rhs) { return lhs.index < rhs.index; });
                candidates.push_back(std::move(candidate));
                ++stats.exactEligible;
                stats.eligibleProjectedBaeGain += gain;
                ++stats.eligibleByGain[std::to_string(gain)];
                ++stats.eligibleByNodeOps[std::to_string(node.ops.size())];
                ++stats.eligibleByMaxWidth[std::to_string(maxValueWidth)];
            }

            std::stable_sort(candidates.begin(),
                             candidates.end(),
                             [](const auto &lhs, const auto &rhs)
                             {
                                 if (lhs.gain != rhs.gain)
                                 {
                                     return lhs.gain > rhs.gain;
                                 }
                                 return std::tuple{lhs.opCount,
                                                   lhs.maxValueWidth,
                                                   lhs.computeNode} <
                                        std::tuple{rhs.opCount,
                                                   rhs.maxValueWidth,
                                                   rhs.computeNode};
                             });

            std::vector<std::size_t> projectedOps;
            projectedOps.reserve(build.supernodeToOps.size());
            for (const auto &ops : build.supernodeToOps)
            {
                projectedOps.push_back(ops.size());
            }
            std::unordered_set<OperationId, OperationIdHash> selectedOps;
            std::unordered_set<ValueId, ValueIdHash> selectedValues;
            for (const auto &candidate : candidates)
            {
                if (stats.selected >= options.finalFaninPullbackMaxMoves)
                {
                    ++stats.rejectedSelectionMoveLimit;
                    continue;
                }
                if (candidate.opCount > stats.movedOpLimit -
                                            std::min(stats.movedOps, stats.movedOpLimit))
                {
                    ++stats.rejectedSelectionBudget;
                    continue;
                }
                if (std::any_of(candidate.touchedOps.begin(),
                                candidate.touchedOps.end(),
                                [&](OperationId op) { return selectedOps.contains(op); }))
                {
                    ++stats.rejectedSelectionNodeOverlap;
                    continue;
                }
                if (std::any_of(candidate.touchedValues.begin(),
                                candidate.touchedValues.end(),
                                [&](ValueId value) { return selectedValues.contains(value); }))
                {
                    ++stats.rejectedSelectionValueOverlap;
                    continue;
                }
                if (candidate.source >= projectedOps.size() ||
                    projectedOps[candidate.source] > computeSupernodeCap ||
                    candidate.opCount >
                        computeSupernodeCap - projectedOps[candidate.source])
                {
                    ++stats.rejectedSelectionCapacity;
                    continue;
                }
                if (candidate.target >= projectedOps.size() ||
                    projectedOps[candidate.target] <= candidate.opCount)
                {
                    ++stats.rejectedSelectionTargetEmpty;
                    continue;
                }

                projectedOps[candidate.source] += candidate.opCount;
                projectedOps[candidate.target] -= candidate.opCount;
                selectedOps.insert(candidate.touchedOps.begin(), candidate.touchedOps.end());
                selectedValues.insert(candidate.touchedValues.begin(), candidate.touchedValues.end());
                ++stats.selected;
                stats.movedOps += candidate.opCount;
                stats.projectedBaeGain += candidate.gain;
                if (selectedCandidates != nullptr)
                {
                    selectedCandidates->push_back(candidate);
                }
                ++stats.selectedByGain[std::to_string(candidate.gain)];
                ++stats.selectedByNodeOps[std::to_string(candidate.opCount)];
                ++stats.selectedByMaxWidth[std::to_string(candidate.maxValueWidth)];
            }
            return stats;
        }

        struct FinalFaninPullbackStrictStats
        {
            std::size_t applied = 0;
            std::size_t computeBaeBefore = 0;
            std::size_t computeBaeAfter = 0;
            std::size_t computeCommitBefore = 0;
            std::size_t computeCommitAfter = 0;
            std::size_t dagEdgesBefore = 0;
            std::size_t dagEdgesAfter = 0;
            std::size_t actualBaeGain = 0;
            bool supernodesValid = false;
            bool kindsValid = false;
            bool scheduledOpsValid = false;
            bool capacityValid = false;
            bool commitValid = false;
            bool computePartitionValid = false;
            bool dagValid = false;
            bool topoValid = false;
            bool stateReadValid = false;
            bool computeCommitValid = false;
            bool baeGainValid = false;
        };

        std::size_t countFinalScheduleDagEdges(const ActivityScheduleBuild &build)
        {
            return std::accumulate(
                build.dag.begin(),
                build.dag.end(),
                std::size_t{0},
                [](std::size_t count, const auto &succs) { return count + succs.size(); });
        }

        std::vector<uint64_t> finalScheduleCommitValuePairs(const ActivityScheduleBuild &build)
        {
            std::vector<uint64_t> pairs;
            for (std::size_t valueIndex = 0; valueIndex < build.valueFanout.size(); ++valueIndex)
            {
                for (const uint32_t target : build.valueFanout[valueIndex])
                {
                    if (target < build.supernodeKinds.size() &&
                        build.supernodeKinds[target] == ActivityScheduleSupernodeKind::Commit)
                    {
                        pairs.push_back((static_cast<uint64_t>(valueIndex + 1) << 32) |
                                        target);
                    }
                }
            }
            std::sort(pairs.begin(), pairs.end());
            return pairs;
        }

        bool rebuildFinalScheduleDerivedNoSplit(const wolvrix::lib::grh::Graph &graph,
                                                ActivityScheduleBuild &build,
                                                std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::ValueId;

            if (build.supernodeToOps.size() != build.supernodeKinds.size() ||
                build.supernodeToOps.size() != build.computeNodesBySupernode.size())
            {
                error = "activity-schedule final-fanin pullback strict rebuild shape mismatch";
                return false;
            }

            std::size_t maxOpIndex = 0;
            for (const OperationId opId : graph.operations())
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, opId.index);
            }
            build.opToSupernode.assign(maxOpIndex, kInvalidActivitySupernodeId);
            std::vector<uint32_t> supernodeOfOp(maxOpIndex + 1,
                                                kInvalidActivitySupernodeId);
            for (uint32_t supernode = 0; supernode < build.supernodeToOps.size(); ++supernode)
            {
                for (const OperationId opId : build.supernodeToOps[supernode])
                {
                    if (!opId.valid() || opId.index > maxOpIndex)
                    {
                        error = "activity-schedule final-fanin pullback strict rebuild invalid op";
                        return false;
                    }
                    if (supernodeOfOp[opId.index] != kInvalidActivitySupernodeId)
                    {
                        error = "activity-schedule final-fanin pullback strict rebuild duplicate op=" +
                                std::to_string(opId.index);
                        return false;
                    }
                    build.opToSupernode[opId.index - 1] = supernode;
                    supernodeOfOp[opId.index] = supernode;
                }
            }

            build.dag.assign(build.supernodeToOps.size(), {});
            build.valueFanout.clear();
            build.valueSourceKind.clear();
            build.valueSourceSupernode.clear();
            if (!graph.values().empty())
            {
                build.valueFanout.assign(graph.values().back().index, {});
                build.valueSourceKind.assign(graph.values().back().index + 1,
                                             wolvrix::lib::grh::OperationKind::kConstant);
                build.valueSourceSupernode.assign(graph.values().back().index + 1,
                                                  kInvalidActivitySupernodeId);
                for (const ValueId value : graph.values())
                {
                    if (!value.valid() || value.index >= build.valueSourceKind.size())
                    {
                        continue;
                    }
                    const OperationId def = graph.valueDef(value);
                    if (!def.valid())
                    {
                        continue;
                    }
                    build.valueSourceKind[value.index] = graph.opKind(def);
                    if (def.index < supernodeOfOp.size())
                    {
                        build.valueSourceSupernode[value.index] = supernodeOfOp[def.index];
                    }
                }
            }

            std::unordered_set<uint64_t> seenEdges;
            const auto addDependency = [&](ValueId value,
                                           uint32_t target,
                                           bool includeNoDefFanout)
            {
                if (!value.valid() || target >= build.supernodeToOps.size())
                {
                    return;
                }
                const OperationId def = graph.valueDef(value);
                if (!def.valid())
                {
                    if (includeNoDefFanout && value.index > 0 &&
                        value.index <= build.valueFanout.size())
                    {
                        build.valueFanout[value.index - 1].push_back(target);
                    }
                    return;
                }
                if (def.index >= supernodeOfOp.size())
                {
                    return;
                }
                const uint32_t source = supernodeOfOp[def.index];
                if (source == kInvalidActivitySupernodeId || source == target)
                {
                    return;
                }
                if (source < build.supernodeKinds.size() &&
                    build.supernodeKinds[source] == ActivityScheduleSupernodeKind::Commit)
                {
                    return;
                }
                const uint64_t packed = (static_cast<uint64_t>(source) << 32) | target;
                if (seenEdges.insert(packed).second)
                {
                    build.dag[source].push_back(target);
                }
                if (value.index > 0 && value.index <= build.valueFanout.size())
                {
                    build.valueFanout[value.index - 1].push_back(target);
                }
            };

            for (uint32_t supernode = 0; supernode < build.supernodeToOps.size(); ++supernode)
            {
                for (const OperationId opId : build.supernodeToOps[supernode])
                {
                    const auto op = graph.getOperation(opId);
                    for (const ValueId operand : op.operands())
                    {
                        if (graph.valueDef(operand).valid())
                        {
                            addDependency(operand, supernode, false);
                        }
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        if (const auto indexValue = regToMemIntentSliceIndexValue(graph, op))
                        {
                            addDependency(*indexValue, supernode, true);
                        }
                    }
                }
            }
            for (auto &succs : build.dag)
            {
                std::sort(succs.begin(), succs.end());
                succs.erase(std::unique(succs.begin(), succs.end()), succs.end());
            }
            for (auto &fanout : build.valueFanout)
            {
                std::sort(fanout.begin(), fanout.end());
                fanout.erase(std::unique(fanout.begin(), fanout.end()), fanout.end());
            }

            build.stateReadSupernodes.clear();
            for (uint32_t supernode = 0; supernode < build.supernodeToOps.size(); ++supernode)
            {
                if (build.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Commit)
                {
                    continue;
                }
                for (const OperationId opId : build.supernodeToOps[supernode])
                {
                    const auto op = graph.getOperation(opId);
                    const auto stateSymbol = stateSymbolForReadOp(op);
                    if (stateSymbol && !stateSymbol->empty())
                    {
                        build.stateReadSupernodes[*stateSymbol].push_back(supernode);
                    }
                    if (isRegToMemIntentSlice(op))
                    {
                        for (const auto &storageSymbol :
                             regToMemIntentSliceStorageReadSymbols(graph, op))
                        {
                            build.stateReadSupernodes[storageSymbol].push_back(supernode);
                        }
                    }
                }
            }
            for (auto &[_, supernodes] : build.stateReadSupernodes)
            {
                std::sort(supernodes.begin(), supernodes.end());
                supernodes.erase(std::unique(supernodes.begin(), supernodes.end()),
                                 supernodes.end());
            }

            try
            {
                build.topoOrder = topoOrderForDag(build.dag);
            }
            catch (const std::exception &ex)
            {
                error = std::string("activity-schedule final-fanin pullback strict topo rebuild failed: ") +
                        ex.what();
                return false;
            }
            if (build.topoOrder.size() != build.supernodeToOps.size())
            {
                error = "activity-schedule final-fanin pullback strict topo rebuild missing supernodes";
                return false;
            }
            return true;
        }

        bool applyFinalFaninPullbackStrict(
            const wolvrix::lib::grh::Graph &graph,
            const ActivityScheduleOptions &options,
            const ActivityOpData &opData,
            const ComputeRewriteBuild &rewrite,
            const ActivityScheduleBuild &baseline,
            const std::vector<FinalFaninPullbackCandidate> &selected,
            std::size_t projectedBaeGain,
            ActivityScheduleBuild &build,
            FinalFaninPullbackStrictStats &stats,
            std::string &error)
        {
            using wolvrix::lib::grh::OperationId;
            using wolvrix::lib::grh::OperationIdHash;

            stats = FinalFaninPullbackStrictStats{};
            ActivityScheduleBuild candidate = baseline;
            const std::size_t computeSupernodeCap =
                options.maxOpInComputeSupernode == 0
                    ? std::numeric_limits<std::size_t>::max()
                    : options.maxOpInComputeSupernode;
            std::set<uint32_t> touchedSupernodes;

            for (const auto &move : selected)
            {
                if (move.computeNode >= rewrite.computeNodes.size() ||
                    move.source >= candidate.supernodeToOps.size() ||
                    move.target >= candidate.supernodeToOps.size() ||
                    move.source == move.target ||
                    candidate.supernodeKinds[move.source] != ActivityScheduleSupernodeKind::Compute ||
                    candidate.supernodeKinds[move.target] != ActivityScheduleSupernodeKind::Compute)
                {
                    error = "activity-schedule final-fanin pullback strict invalid selected owner node=" +
                            std::to_string(move.computeNode);
                    return false;
                }
                const auto &nodeOps = rewrite.computeNodes[move.computeNode].ops;
                if (nodeOps.size() != move.opCount || nodeOps != move.touchedOps)
                {
                    error = "activity-schedule final-fanin pullback strict stale selected node=" +
                            std::to_string(move.computeNode);
                    return false;
                }

                auto &sourceOps = candidate.supernodeToOps[move.source];
                auto &targetOps = candidate.supernodeToOps[move.target];
                std::unordered_set<OperationId, OperationIdHash> moveOps(nodeOps.begin(),
                                                                         nodeOps.end());
                const std::size_t targetMatches = static_cast<std::size_t>(std::count_if(
                    targetOps.begin(),
                    targetOps.end(),
                    [&](OperationId op) { return moveOps.contains(op); }));
                const bool sourceAlreadyContains = std::any_of(
                    sourceOps.begin(),
                    sourceOps.end(),
                    [&](OperationId op) { return moveOps.contains(op); });
                if (targetMatches != nodeOps.size() || sourceAlreadyContains ||
                    targetOps.size() <= nodeOps.size() ||
                    sourceOps.size() > computeSupernodeCap ||
                    nodeOps.size() > computeSupernodeCap - sourceOps.size())
                {
                    error = "activity-schedule final-fanin pullback strict move precondition failed node=" +
                            std::to_string(move.computeNode) +
                            " source=" + std::to_string(move.source) +
                            " target=" + std::to_string(move.target);
                    return false;
                }

                auto &sourceNodes = candidate.computeNodesBySupernode[move.source];
                auto &targetNodes = candidate.computeNodesBySupernode[move.target];
                const std::size_t targetNodeMatches = static_cast<std::size_t>(
                    std::count(targetNodes.begin(), targetNodes.end(), move.computeNode));
                if (targetNodeMatches != 1 ||
                    std::find(sourceNodes.begin(), sourceNodes.end(), move.computeNode) !=
                        sourceNodes.end())
                {
                    error = "activity-schedule final-fanin pullback strict node partition precondition failed node=" +
                            std::to_string(move.computeNode);
                    return false;
                }

                targetOps.erase(std::remove_if(targetOps.begin(),
                                               targetOps.end(),
                                               [&](OperationId op) { return moveOps.contains(op); }),
                                targetOps.end());
                sourceOps.insert(sourceOps.end(), nodeOps.begin(), nodeOps.end());
                targetNodes.erase(std::remove(targetNodes.begin(),
                                              targetNodes.end(),
                                              move.computeNode),
                                  targetNodes.end());
                sourceNodes.push_back(move.computeNode);
                touchedSupernodes.insert(move.source);
                touchedSupernodes.insert(move.target);
                ++stats.applied;
            }

            for (const uint32_t supernode : touchedSupernodes)
            {
                std::vector<OperationId> orderedOps;
                if (!topoSortLocalOps(graph,
                                      candidate.supernodeToOps[supernode],
                                      orderedOps,
                                      error))
                {
                    error = "activity-schedule final-fanin pullback strict local topo failed supernode=" +
                            std::to_string(supernode) + ": " + error;
                    return false;
                }
                if (orderedOps.size() != candidate.supernodeToOps[supernode].size())
                {
                    error = "activity-schedule final-fanin pullback strict local topo lost ops supernode=" +
                            std::to_string(supernode);
                    return false;
                }
                candidate.supernodeToOps[supernode] = std::move(orderedOps);
            }

            if (!rebuildFinalScheduleDerivedNoSplit(graph, candidate, error))
            {
                return false;
            }

            stats.supernodesValid =
                candidate.supernodeToOps.size() == baseline.supernodeToOps.size();
            if (!stats.supernodesValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: supernode count";
                return false;
            }
            stats.kindsValid = candidate.supernodeKinds == baseline.supernodeKinds;
            if (!stats.kindsValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: supernode kinds";
                return false;
            }

            std::vector<OperationId> baselineOps;
            std::vector<OperationId> candidateOps;
            std::unordered_set<OperationId, OperationIdHash> uniqueCandidateOps;
            bool candidateHasDuplicate = false;
            for (const auto &ops : baseline.supernodeToOps)
            {
                baselineOps.insert(baselineOps.end(), ops.begin(), ops.end());
            }
            for (const auto &ops : candidate.supernodeToOps)
            {
                candidateOps.insert(candidateOps.end(), ops.begin(), ops.end());
                for (const OperationId op : ops)
                {
                    candidateHasDuplicate |= !uniqueCandidateOps.insert(op).second;
                }
            }
            const auto opLess = [](OperationId lhs, OperationId rhs)
            {
                return lhs.index < rhs.index;
            };
            std::sort(baselineOps.begin(), baselineOps.end(), opLess);
            std::sort(candidateOps.begin(), candidateOps.end(), opLess);
            stats.scheduledOpsValid = !candidateHasDuplicate && baselineOps == candidateOps;
            if (!stats.scheduledOpsValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: scheduled ops";
                return false;
            }

            stats.capacityValid = true;
            stats.commitValid = true;
            for (uint32_t supernode = 0; supernode < candidate.supernodeToOps.size(); ++supernode)
            {
                if (candidate.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Compute)
                {
                    stats.capacityValid &= !candidate.supernodeToOps[supernode].empty() &&
                                           candidate.supernodeToOps[supernode].size() <=
                                               computeSupernodeCap;
                }
                else
                {
                    stats.commitValid &= candidate.supernodeToOps[supernode] ==
                                         baseline.supernodeToOps[supernode];
                }
            }
            if (!stats.capacityValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: compute capacity";
                return false;
            }
            if (!stats.commitValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: commit partition";
                return false;
            }

            std::vector<std::size_t> computeNodeUses(rewrite.computeNodes.size(), 0);
            stats.computePartitionValid =
                candidate.computeNodesBySupernode.size() == candidate.supernodeToOps.size();
            for (uint32_t supernode = 0;
                 stats.computePartitionValid &&
                 supernode < candidate.computeNodesBySupernode.size();
                 ++supernode)
            {
                const auto &nodeIds = candidate.computeNodesBySupernode[supernode];
                if (candidate.supernodeKinds[supernode] == ActivityScheduleSupernodeKind::Commit)
                {
                    stats.computePartitionValid &= nodeIds.empty();
                    continue;
                }
                std::unordered_set<OperationId, OperationIdHash> expectedOps;
                for (const uint32_t nodeId : nodeIds)
                {
                    if (nodeId >= rewrite.computeNodes.size())
                    {
                        stats.computePartitionValid = false;
                        break;
                    }
                    ++computeNodeUses[nodeId];
                    for (const OperationId op : rewrite.computeNodes[nodeId].ops)
                    {
                        stats.computePartitionValid &= expectedOps.insert(op).second;
                    }
                }
                std::unordered_set<OperationId, OperationIdHash> actualOps(
                    candidate.supernodeToOps[supernode].begin(),
                    candidate.supernodeToOps[supernode].end());
                stats.computePartitionValid &= expectedOps == actualOps &&
                                               actualOps.size() ==
                                                   candidate.supernodeToOps[supernode].size();
            }
            stats.computePartitionValid &= std::all_of(
                computeNodeUses.begin(),
                computeNodeUses.end(),
                [](std::size_t uses) { return uses == 1; });
            for (const auto &move : selected)
            {
                const auto &sourceNodes = candidate.computeNodesBySupernode[move.source];
                const auto &targetNodes = candidate.computeNodesBySupernode[move.target];
                stats.computePartitionValid &=
                    std::count(sourceNodes.begin(), sourceNodes.end(), move.computeNode) == 1 &&
                    std::find(targetNodes.begin(), targetNodes.end(), move.computeNode) ==
                        targetNodes.end();
            }
            if (!stats.computePartitionValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: compute-node partition";
                return false;
            }

            stats.dagEdgesBefore = countFinalScheduleDagEdges(baseline);
            stats.dagEdgesAfter = countFinalScheduleDagEdges(candidate);
            stats.dagValid = candidate.dag == baseline.dag;
            stats.topoValid = candidate.topoOrder == baseline.topoOrder;
            stats.stateReadValid =
                candidate.stateReadSupernodes == baseline.stateReadSupernodes;
            if (!stats.dagValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: DAG before=" +
                        std::to_string(stats.dagEdgesBefore) +
                        " after=" + std::to_string(stats.dagEdgesAfter);
                return false;
            }
            if (!stats.topoValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: topo order";
                return false;
            }
            if (!stats.stateReadValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: state-read sets";
                return false;
            }

            const ActivityScheduleSummaryStats baselineSummary =
                buildActivityScheduleSummaryStats(baseline, rewrite, opData, graph);
            const ActivityScheduleSummaryStats candidateSummary =
                buildActivityScheduleSummaryStats(candidate, rewrite, opData, graph);
            stats.computeBaeBefore = baselineSummary.computeComputeValuePairs;
            stats.computeBaeAfter = candidateSummary.computeComputeValuePairs;
            stats.computeCommitBefore = baselineSummary.computeCommitValuePairs;
            stats.computeCommitAfter = candidateSummary.computeCommitValuePairs;
            stats.computeCommitValid =
                stats.computeCommitBefore == stats.computeCommitAfter &&
                finalScheduleCommitValuePairs(baseline) ==
                    finalScheduleCommitValuePairs(candidate);
            if (!stats.computeCommitValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: compute-commit before=" +
                        std::to_string(stats.computeCommitBefore) +
                        " after=" + std::to_string(stats.computeCommitAfter);
                return false;
            }
            stats.actualBaeGain =
                stats.computeBaeBefore >= stats.computeBaeAfter
                    ? stats.computeBaeBefore - stats.computeBaeAfter
                    : 0;
            stats.baeGainValid = stats.computeBaeBefore >= stats.computeBaeAfter &&
                                 stats.actualBaeGain == projectedBaeGain;
            if (!stats.baeGainValid)
            {
                error = "activity-schedule final-fanin pullback strict invariant failed: BAE predicted=" +
                        std::to_string(projectedBaeGain) +
                        " actual=" + std::to_string(stats.actualBaeGain) +
                        " before=" + std::to_string(stats.computeBaeBefore) +
                        " after=" + std::to_string(stats.computeBaeAfter);
                return false;
            }

            build = std::move(candidate);
            return true;
        }

    } // namespace

    ActivitySchedulePass::ActivitySchedulePass()
        : Pass("activity-schedule",
               "activity-schedule",
               "Build activity schedule for a single graph"),
          options_({})
    {
    }

    ActivitySchedulePass::ActivitySchedulePass(ActivityScheduleOptions options)
        : Pass("activity-schedule",
               "activity-schedule",
               "Build activity schedule for a single graph"),
          options_(std::move(options))
    {
    }

    PassResult ActivitySchedulePass::run()
    {
        PassResult result;
        const auto totalStart = std::chrono::steady_clock::now();
        if (options_.path.empty())
        {
            error("activity-schedule requires -path");
            result.failed = true;
            return result;
        }
        if (options_.finalTopoPolicy != "level-id" &&
            options_.finalTopoPolicy != "level-op" &&
            options_.finalTopoPolicy != "ready-op")
        {
            error("activity-schedule final_topo_policy must be level-id, level-op, or ready-op");
            result.failed = true;
            return result;
        }
        if (options_.finalFaninPullbackPolicy != "off" &&
            options_.finalFaninPullbackPolicy != "probe" &&
            options_.finalFaninPullbackPolicy != "strict")
        {
            error("activity-schedule final_fanin_pullback_policy must be off, probe, or strict");
            result.failed = true;
            return result;
        }
        if (options_.finalFaninPullbackPolicy == "strict" &&
            options_.finalTopoPolicy != "level-id")
        {
            error("activity-schedule strict final-fanin pullback requires final_topo_policy=level-id");
            result.failed = true;
            return result;
        }
        if (options_.postDpRefinePolicy != "off" &&
            options_.postDpRefinePolicy != "strict" &&
            options_.postDpRefinePolicy != "bae-budget" &&
            options_.postDpRefinePolicy != "balanced" &&
            options_.postDpRefinePolicy != "swap-probe")
        {
            error("activity-schedule post_dp_refine_policy must be off, strict, bae-budget, balanced, or swap-probe");
            result.failed = true;
            return result;
        }
        if (options_.kahnLevelPackPolicy != "off" &&
            options_.kahnLevelPackPolicy != "strict" &&
            options_.kahnLevelPackPolicy != "bae-budget" &&
            options_.kahnLevelPackPolicy != "balanced")
        {
            error("activity-schedule kahn_level_pack_policy must be off, strict, bae-budget, or balanced");
            result.failed = true;
            return result;
        }
        if (options_.localSharedComputeCommonOwnerPolicy != "off" &&
            options_.localSharedComputeCommonOwnerPolicy != "probe" &&
            options_.localSharedComputeCommonOwnerPolicy != "strict")
        {
            error("activity-schedule local_shared_compute_common_owner_policy must be off, probe, or strict");
            result.failed = true;
            return result;
        }
        if (options_.localSharedComputeCommonOwnerPolicy == "strict" &&
            (!options_.enableLocalSharedCompute ||
             options_.localSharedComputeMaxClones != 0))
        {
            error("activity-schedule strict common-owner cloning requires enable_local_shared_compute=true and local_shared_compute_max_clones=0");
            result.failed = true;
            return result;
        }
        if (options_.postDpRefineMaxMovedOpPpm > 1000000 ||
            options_.postDpRefineMaxRegressionPpm > 1000000)
        {
            error("activity-schedule post-DP refine ppm options must be <= 1000000");
            result.failed = true;
            return result;
        }
        if (options_.kahnLevelPackMaxMovedOpPpm > 1000000 ||
            options_.kahnLevelPackMaxRegressionPpm > 1000000)
        {
            error("activity-schedule Kahn-level pack ppm options must be <= 1000000");
            result.failed = true;
            return result;
        }
        if (options_.localSharedComputeMaxClonedOpPpm > 1000000)
        {
            error("activity-schedule local shared compute cloned-op ppm must be <= 1000000");
            result.failed = true;
            return result;
        }
        if (options_.localSharedComputeCommonOwnerMaxClonedOpPpm > 1000000)
        {
            error("activity-schedule common-owner cloned-op ppm must be <= 1000000");
            result.failed = true;
            return result;
        }
        if (options_.dpSegmentPenaltyPpm > 1000000000)
        {
            error("activity-schedule dp_segment_penalty_ppm must be <= 1000000000");
            result.failed = true;
            return result;
        }
        if (options_.finalFaninPullbackMaxMovedOpPpm > 1000000)
        {
            error("activity-schedule final_fanin_pullback_max_moved_op_ppm must be <= 1000000");
            result.failed = true;
            return result;
        }

        const std::size_t maxOpsPerComputeSupernode = options_.maxOpInComputeSupernode;
        const std::size_t maxCommitOps = options_.maxOpInCommitSupernode;
        if (maxOpsPerComputeSupernode == 0)
        {
            error("activity-schedule max_op_in_compute_supernode must be >= 1");
            result.failed = true;
            return result;
        }
        if (maxCommitOps == 0)
        {
            error("activity-schedule max_op_in_commit_supernode must be >= 1");
            result.failed = true;
            return result;
        }
        options_.maxOpInComputeSupernode = maxOpsPerComputeSupernode;
        options_.maxOpInCommitSupernode = maxCommitOps;

        std::string resolveError;
        const std::optional<std::string> targetGraphName =
            resolveTargetGraphName(design(), options_.path, resolveError);
        if (!targetGraphName)
        {
            error(resolveError);
            result.failed = true;
            return result;
        }

        auto *graph = design().findGraph(*targetGraphName);
        if (graph == nullptr)
        {
            error("activity-schedule target graph not found: " + *targetGraphName);
            result.failed = true;
            return result;
        }

        bool graphChanged = false;
        std::vector<wolvrix::lib::grh::OperationId> opsNeedingSymbol;
        opsNeedingSymbol.reserve(graph->operations().size());
        for (const auto opId : graph->operations())
        {
            const auto op = graph->getOperation(opId);
            if (isHierLikeOpKind(op.kind()))
            {
                error(*graph,
                      op,
                      "activity-schedule guard: target graph must not contain hierarchical ops kind=" +
                          std::string(wolvrix::lib::grh::toString(op.kind())));
                result.failed = true;
            }
            if (!graph->operationSymbol(opId).valid() && isPartitionableOpKind(op.kind()))
            {
                opsNeedingSymbol.push_back(opId);
            }
        }
        if (result.failed)
        {
            return result;
        }

        for (const auto opId : opsNeedingSymbol)
        {
            graph->setOpSymbol(opId, graph->makeInternalOpSym());
            graphChanged = true;
        }

        const auto buildOpDataStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: build_op_data start graph=" + *targetGraphName);
        graph->freeze();
        std::string buildError;
        ActivityOpData opData = buildActivityOpData(*graph, buildError);
        const std::uint64_t buildOpDataMs = elapsedMs(buildOpDataStart);
        if (!buildError.empty())
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        logInfo("activity-schedule progress: build_op_data done ops=" +
                std::to_string(opData.topoOps.size()) +
                " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                " elapsed_ms=" + std::to_string(buildOpDataMs));

        std::vector<ActivityOpClass> opClasses = buildOpClasses(*graph, opData.maxOpIndex);
        ComputeNodeRewriteStats precloneStats;
        ValueCanonicalMap canonicalValues;
        bool sourceCloneGraphChanged = false;
        const auto sourceCloneStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: source_clone start");
        if (!cloneSourceUsesForCompute(*graph,
                                       opClasses,
                                       precloneStats,
                                       canonicalValues,
                                       sourceCloneGraphChanged,
                                       buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        logInfo("activity-schedule progress: source_clone done clones=" +
                std::to_string(precloneStats.sourceClonesInComputeNodes) +
                " graph_changed=" + std::string(sourceCloneGraphChanged ? "true" : "false") +
                " elapsed_ms=" + std::to_string(elapsedMs(sourceCloneStart)));
        if (sourceCloneGraphChanged)
        {
            graphChanged = true;
            const auto refreezeStart = std::chrono::steady_clock::now();
            logInfo("activity-schedule progress: source_clone_refreeze start");
            graph->freeze();
            opData = buildActivityOpData(*graph, buildError);
            if (!buildError.empty())
            {
                error(*graph, buildError);
                result.failed = true;
                return result;
            }
            opClasses = buildOpClasses(*graph, opData.maxOpIndex);
            logInfo("activity-schedule progress: source_clone_refreeze done ops=" +
                    std::to_string(opData.topoOps.size()) +
                    " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                    " elapsed_ms=" + std::to_string(elapsedMs(refreezeStart)));
        }
        ComputeRewriteBuild rewrite;
        const auto computeNodeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: compute_node_build start mode=default");
        const bool computeNodeBuildOk = buildComputeNodeRewrite(*graph,
                                                               options_,
                                                               opData,
                                                               opClasses,
                                                               canonicalValues,
                                                               rewrite,
                                                               buildError);
        if (!computeNodeBuildOk)
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        rewrite.stats.sourceClonesInComputeNodes = precloneStats.sourceClonesInComputeNodes;
        const std::uint64_t computeNodeMs = elapsedMs(computeNodeStart);
        logInfo("activity-schedule progress: compute_node_build done compute_nodes=" +
                std::to_string(rewrite.computeNodes.size()) +
                " commit_nodes=" + std::to_string(rewrite.commitNodes.size()) +
                " cycle_split_iters=" + std::to_string(rewrite.stats.computeNodeCycleSplitIters) +
                " elapsed_ms=" + std::to_string(computeNodeMs));

        if (options_.localSharedComputeCommonOwnerPolicy == "probe")
        {
            const auto commonOwnerProbeStart = std::chrono::steady_clock::now();
            logInfo("activity-schedule progress: local_shared_compute_common_owner_probe start");
            const LocalSharedComputeCommonOwnerProbeStats probe =
                probeLocalSharedComputeCommonOwners(*graph,
                                                    options_,
                                                    opData,
                                                    opClasses,
                                                    rewrite);
            logInfo(
                "activity-schedule local shared compute common-owner probe: scanned=" +
                std::to_string(probe.scanned) +
                " pre_user_guard_eligible=" + std::to_string(probe.preUserGuardEligible) +
                " rejected_kind=" + std::to_string(probe.rejectedKind) +
                " rejected_shape=" + std::to_string(probe.rejectedShape) +
                " rejected_width=" + std::to_string(probe.rejectedWidth) +
                " rejected_intent=" + std::to_string(probe.rejectedIntent) +
                " rejected_side_effect=" + std::to_string(probe.rejectedSideEffect) +
                " rejected_declared_or_port=" +
                std::to_string(probe.rejectedDeclaredOrPort) +
                " invalid_or_noncompute_user=" +
                std::to_string(probe.invalidOrNonComputeUser) +
                " distinct_user_ops_not_two=" +
                std::to_string(probe.distinctUserOpsNotTwo) +
                " consumer_nodes_0=" + std::to_string(probe.consumerNodeCountZero) +
                " consumer_nodes_1=" + std::to_string(probe.consumerNodeCountOne) +
                " consumer_nodes_2=" + std::to_string(probe.consumerNodeCountTwo) +
                " consumer_nodes_gt2=" +
                std::to_string(probe.consumerNodeCountMoreThanTwo) +
                " source_owner_invalid=" + std::to_string(probe.sourceOwnerInvalid) +
                " source_owner_is_consumer=" +
                std::to_string(probe.sourceOwnerIsConsumer) +
                " source_owner_third_common=" +
                std::to_string(probe.sourceOwnerThirdCommon) +
                " source_owner_third_noncommon=" +
                std::to_string(probe.sourceOwnerThirdNonCommon) +
                " third_common_singleton=" + std::to_string(probe.thirdCommonSingleton) +
                " third_common_multiop=" + std::to_string(probe.thirdCommonMultiOp) +
                " third_common_source_intent_or_indivisible=" +
                std::to_string(probe.thirdCommonSourceIntentOrIndivisible) +
                " third_common_left_intent_or_indivisible=" +
                std::to_string(probe.thirdCommonLeftIntentOrIndivisible) +
                " third_common_right_intent_or_indivisible=" +
                std::to_string(probe.thirdCommonRightIntentOrIndivisible) +
                " third_common_any_intent_or_indivisible=" +
                std::to_string(probe.thirdCommonAnyIntentOrIndivisible) +
                " result_boundary_both=" + std::to_string(probe.resultBoundaryBoth) +
                " result_boundary_left_only=" +
                std::to_string(probe.resultBoundaryLeftOnly) +
                " result_boundary_right_only=" +
                std::to_string(probe.resultBoundaryRightOnly) +
                " result_boundary_neither=" + std::to_string(probe.resultBoundaryNeither) +
                " operand_locality_both=" + std::to_string(probe.operandLocalityBoth) +
                " operand_locality_left_only=" +
                std::to_string(probe.operandLocalityLeftOnly) +
                " operand_locality_right_only=" +
                std::to_string(probe.operandLocalityRightOnly) +
                " operand_locality_neither=" +
                std::to_string(probe.operandLocalityNeither) +
                " capacity_both_pass=" + std::to_string(probe.capacityBothPass) +
                " capacity_left_fail=" + std::to_string(probe.capacityLeftFail) +
                " capacity_right_fail=" + std::to_string(probe.capacityRightFail) +
                " capacity_both_fail=" + std::to_string(probe.capacityBothFail) +
                " exact_eligible=" + std::to_string(probe.exactEligible) +
                " projected_removed_pairs=" +
                std::to_string(probe.projectedRemovedPairs));
            logInfo(
                "activity-schedule local shared compute common-owner probe detail: "
                "third_common_by_kind=" +
                formatTopCounts(probe.thirdCommonByKind, 32) +
                " third_common_by_result_width=" +
                formatTopCounts(probe.thirdCommonByResultWidth, 16) +
                " third_common_by_operand_bits=" +
                formatTopCounts(probe.thirdCommonByOperandBits, 16) +
                " third_common_by_left_headroom=" +
                formatTopCounts(probe.thirdCommonByLeftHeadroom, 16) +
                " third_common_by_right_headroom=" +
                formatTopCounts(probe.thirdCommonByRightHeadroom, 16) +
                " eligible_by_kind=" + formatTopCounts(probe.eligibleByKind, 32) +
                " eligible_by_result_width=" +
                formatTopCounts(probe.eligibleByResultWidth, 16) +
                " eligible_by_operand_bits=" +
                formatTopCounts(probe.eligibleByOperandBits, 16));
            logInfo(
                "activity-schedule progress: local_shared_compute_common_owner_probe done elapsed_ms=" +
                std::to_string(elapsedMs(commonOwnerProbeStart)));
        }

        if (options_.localSharedComputeCommonOwnerPolicy == "strict")
        {
            const auto commonOwnerCloneStart = std::chrono::steady_clock::now();
            logInfo("activity-schedule progress: local_shared_compute_common_owner_clone start");
            std::vector<LocalSharedComputeCommonOwnerCandidate> candidates;
            const LocalSharedComputeCommonOwnerProbeStats probe =
                probeLocalSharedComputeCommonOwners(*graph,
                                                    options_,
                                                    opData,
                                                    opClasses,
                                                    rewrite,
                                                    &candidates);
            LocalSharedComputeCommonOwnerCloneStats commonStats;
            std::vector<LocalSharedComputeCommonOwnerClonePlan> commonPlans;
            if (!applyLocalSharedComputeCommonOwnerClones(*graph,
                                                         options_,
                                                         opData,
                                                         opClasses,
                                                         rewrite,
                                                         std::move(candidates),
                                                         commonStats,
                                                         commonPlans,
                                                         buildError))
            {
                error(*graph, buildError);
                result.failed = true;
                return result;
            }
            if (!commonPlans.empty())
            {
                graphChanged = true;
                graph->freeze();
                ActivityOpData clonedOpData = buildActivityOpData(*graph, buildError);
                if (!buildError.empty())
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                std::vector<ActivityOpClass> clonedOpClasses =
                    buildOpClasses(*graph, clonedOpData.maxOpIndex);
                ComputeRewriteBuild clonedRewrite;
                if (!buildComputeNodeRewrite(*graph,
                                             options_,
                                             clonedOpData,
                                             clonedOpClasses,
                                             canonicalValues,
                                             clonedRewrite,
                                             buildError,
                                             &rewrite))
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                logInfo("activity-schedule local shared compute common-owner strict: fixed_commit_partition_seed_adopted=true commit_nodes=" +
                        std::to_string(clonedRewrite.commitNodes.size()));
                clonedRewrite.stats.sourceClonesInComputeNodes =
                    precloneStats.sourceClonesInComputeNodes;
                clonedRewrite.stats.localSharedComputeClonesInComputeNodes =
                    commonStats.applied;
                if (!validateLocalSharedComputeCommonOwnerCloneRewrite(*graph,
                                                                       options_,
                                                                       rewrite,
                                                                       clonedRewrite,
                                                                       commonPlans,
                                                                       commonStats,
                                                                       buildError))
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                opData = std::move(clonedOpData);
                opClasses = std::move(clonedOpClasses);
                rewrite = std::move(clonedRewrite);
            }
            logInfo(
                "activity-schedule local shared compute common-owner strict: raw_eligible=" +
                std::to_string(probe.exactEligible) +
                " selected=" + std::to_string(commonStats.selected) +
                " applied=" + std::to_string(commonStats.applied) +
                " clone_limit=" + std::to_string(commonStats.cloneLimit) +
                " rejected_budget=" + std::to_string(commonStats.rejectedBudget) +
                " rejected_capacity=" + std::to_string(commonStats.rejectedCapacity) +
                " rejected_role=" + std::to_string(commonStats.rejectedRole) +
                " rejected_dependency=" + std::to_string(commonStats.rejectedDependency) +
                " rejected_stale=" + std::to_string(commonStats.rejectedStale) +
                " projected_removed_pairs=" +
                std::to_string(commonStats.projectedRemovedPairs) +
                " actual_localized_pairs=" +
                std::to_string(commonStats.actualLocalizedPairs) +
                " graph_ops_before=" + std::to_string(commonStats.graphOpsBefore) +
                " graph_ops_after=" + std::to_string(graph->operations().size()) +
                " graph_ops_delta=" + std::to_string(commonStats.applied) +
                " graph_values_before=" + std::to_string(commonStats.graphValuesBefore) +
                " graph_values_after=" + std::to_string(graph->values().size()) +
                " graph_values_delta=" + std::to_string(commonStats.applied));
            logInfo(
                "activity-schedule progress: local_shared_compute_common_owner_clone done elapsed_ms=" +
                std::to_string(elapsedMs(commonOwnerCloneStart)));
        }

        LocalSharedComputeCloneStats localSharedCloneStats;
        std::vector<LocalSharedComputeCloneRecord> localSharedCloneRecords;
        const auto localSharedCloneStart = std::chrono::steady_clock::now();
        std::uint64_t localSharedCloneMs = 0;
        if (options_.enableLocalSharedCompute &&
            options_.localSharedComputeCommonOwnerPolicy != "strict")
        {
            logInfo("activity-schedule progress: local_shared_compute_clone start");
            if (!applyLocalSharedComputeClones(*graph,
                                               options_,
                                               opData,
                                               opClasses,
                                               rewrite,
                                               localSharedCloneStats,
                                               localSharedCloneRecords,
                                               buildError))
            {
                error(*graph, buildError);
                result.failed = true;
                return result;
            }
            if (!localSharedCloneRecords.empty())
            {
                graphChanged = true;
                graph->freeze();
                ActivityOpData clonedOpData = buildActivityOpData(*graph, buildError);
                if (!buildError.empty())
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                std::vector<ActivityOpClass> clonedOpClasses =
                    buildOpClasses(*graph, clonedOpData.maxOpIndex);
                ComputeRewriteBuild clonedRewrite;
                if (!buildComputeNodeRewrite(*graph,
                                             options_,
                                             clonedOpData,
                                             clonedOpClasses,
                                             canonicalValues,
                                             clonedRewrite,
                                             buildError))
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                clonedRewrite.stats.sourceClonesInComputeNodes =
                    precloneStats.sourceClonesInComputeNodes;
                clonedRewrite.stats.localSharedComputeClonesInComputeNodes =
                    localSharedCloneStats.applied;
                if (!validateLocalSharedComputeCloneRewrite(options_,
                                                            rewrite,
                                                            clonedRewrite,
                                                            localSharedCloneRecords,
                                                            buildError))
                {
                    error(*graph, buildError);
                    result.failed = true;
                    return result;
                }
                opData = std::move(clonedOpData);
                opClasses = std::move(clonedOpClasses);
                rewrite = std::move(clonedRewrite);
            }
            localSharedCloneMs = elapsedMs(localSharedCloneStart);
            logInfo("activity-schedule progress: local_shared_compute_clone done" +
                    std::string(" scanned=") + std::to_string(localSharedCloneStats.scanned) +
                    " eligible=" + std::to_string(localSharedCloneStats.eligible) +
                    " planned=" + std::to_string(localSharedCloneStats.planned) +
                    " applied=" + std::to_string(localSharedCloneStats.applied) +
                    " clone_limit=" + std::to_string(localSharedCloneStats.cloneLimit) +
                    " rejected_kind=" + std::to_string(localSharedCloneStats.rejectedKind) +
                    " rejected_shape=" + std::to_string(localSharedCloneStats.rejectedShape) +
                    " rejected_width=" + std::to_string(localSharedCloneStats.rejectedWidth) +
                    " rejected_fanout=" + std::to_string(localSharedCloneStats.rejectedFanout) +
                    " rejected_consumer_nodes=" +
                    std::to_string(localSharedCloneStats.rejectedConsumerNodes) +
                    " rejected_intent=" + std::to_string(localSharedCloneStats.rejectedIntent) +
                    " rejected_side_effect=" +
                    std::to_string(localSharedCloneStats.rejectedSideEffect) +
                    " rejected_declared_or_port=" +
                    std::to_string(localSharedCloneStats.rejectedDeclaredOrPort) +
                    " rejected_operand=" + std::to_string(localSharedCloneStats.rejectedOperand) +
                    " rejected_capacity=" +
                    std::to_string(localSharedCloneStats.rejectedCapacity) +
                    " rejected_budget=" + std::to_string(localSharedCloneStats.rejectedBudget) +
                    " elapsed_ms=" + std::to_string(localSharedCloneMs));
        }
        if (!exportComputeDagJson(*graph, options_, rewrite, buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        if (!options_.exportComputeDagPath.empty())
        {
            logInfo("activity-schedule compute DAG exported: path=" + options_.exportComputeDagPath);
        }
        if (rewrite.stats.sourceClonesInComputeNodes != 0)
        {
            graphChanged = true;
        }

        const auto freezeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: freeze_after_compute_node start");
        graph->freeze();
        const std::uint64_t freezeMs = elapsedMs(freezeStart);
        logInfo("activity-schedule progress: freeze_after_compute_node done elapsed_ms=" +
                std::to_string(freezeMs));

        ActivityScheduleBuild build;
        ComputeNodeMaterializePerfStats materializePerf;
        const auto materializeStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: final_materialize start");
        if (!materializeComputeNodeSchedule(*graph,
                                            options_,
                                            opData,
                                            rewrite,
                                            build,
                                            &materializePerf,
                                            buildError))
        {
            error(*graph, buildError);
            result.failed = true;
            return result;
        }
        const std::uint64_t materializeMs = elapsedMs(materializeStart);
        logInfo("activity-schedule progress: final_materialize done supernodes=" +
                std::to_string(build.supernodeToOps.size()) +
                " elapsed_ms=" + std::to_string(materializeMs));

        if (options_.finalFaninPullbackPolicy == "probe")
        {
            const auto probeStart = std::chrono::steady_clock::now();
            const FinalFaninPullbackProbeStats probe =
                evaluateFinalFaninPullback(*graph, options_, rewrite, build, materializePerf);
            const std::uint64_t probeMs = elapsedMs(probeStart);
            logInfo("activity-schedule final-fanin pullback probe: policy=" +
                    options_.finalFaninPullbackPolicy +
                    " max_node_ops=" +
                    std::to_string(options_.finalFaninPullbackMaxNodeOps) +
                    " max_value_width=" +
                    std::to_string(options_.finalFaninPullbackMaxValueWidth) +
                    " min_gain=" + std::to_string(options_.finalFaninPullbackMinGain) +
                    " max_moves=" + std::to_string(options_.finalFaninPullbackMaxMoves) +
                    " max_moved_op_ppm=" +
                    std::to_string(options_.finalFaninPullbackMaxMovedOpPpm) +
                    " scanned=" + std::to_string(probe.scanned) +
                    " pure=" + std::to_string(probe.pure) +
                    " common_source=" + std::to_string(probe.commonSource) +
                    " exact_eligible=" + std::to_string(probe.exactEligible) +
                    " selected=" + std::to_string(probe.selected) +
                    " eligible_projected_bae_gain=" +
                    std::to_string(probe.eligibleProjectedBaeGain) +
                    " projected_bae_gain=" + std::to_string(probe.projectedBaeGain) +
                    " moved_ops=" + std::to_string(probe.movedOps) +
                    " moved_op_limit=" + std::to_string(probe.movedOpLimit) +
                    " skipped_oversize=" +
                    std::string(probe.skippedOversize ? "true" : "false") +
                    " skipped_final_topo=" +
                    std::string(probe.skippedFinalTopo ? "true" : "false") +
                    " elapsed_ms=" + std::to_string(probeMs));
            logInfo("activity-schedule final-fanin pullback probe rejects: rejected_owner=" +
                    std::to_string(probe.rejectedOwner) +
                    " rejected_node_size=" + std::to_string(probe.rejectedNodeSize) +
                    " rejected_restricted=" + std::to_string(probe.rejectedRestricted) +
                    " rejected_kind=" + std::to_string(probe.rejectedKind) +
                    " rejected_side_effect=" + std::to_string(probe.rejectedSideEffect) +
                    " rejected_clone_forbidden=" + std::to_string(probe.rejectedCloneForbidden) +
                    " rejected_shape=" + std::to_string(probe.rejectedShape) +
                    " rejected_width=" + std::to_string(probe.rejectedWidth) +
                    " rejected_port_or_declared=" + std::to_string(probe.rejectedPortOrDeclared) +
                    " rejected_liveout_count=" + std::to_string(probe.rejectedLiveoutCount) +
                    " rejected_cone=" + std::to_string(probe.rejectedCone) +
                    " rejected_no_def=" + std::to_string(probe.rejectedNoDef) +
                    " rejected_target_predecessor=" +
                    std::to_string(probe.rejectedTargetPredecessor) +
                    " rejected_third_source=" + std::to_string(probe.rejectedThirdSource) +
                    " rejected_source_kind=" + std::to_string(probe.rejectedSourceKind) +
                    " rejected_external_consumer=" +
                    std::to_string(probe.rejectedExternalConsumer) +
                    " rejected_target_empty=" + std::to_string(probe.rejectedTargetEmpty) +
                    " rejected_capacity=" + std::to_string(probe.rejectedCapacity) +
                    " rejected_no_removable_input=" +
                    std::to_string(probe.rejectedNoRemovableInput) +
                    " rejected_min_gain=" + std::to_string(probe.rejectedMinGain) +
                    " rejected_selection_move_limit=" +
                    std::to_string(probe.rejectedSelectionMoveLimit) +
                    " rejected_selection_budget=" +
                    std::to_string(probe.rejectedSelectionBudget) +
                    " rejected_selection_capacity=" +
                    std::to_string(probe.rejectedSelectionCapacity) +
                    " rejected_selection_target_empty=" +
                    std::to_string(probe.rejectedSelectionTargetEmpty) +
                    " rejected_selection_node_overlap=" +
                    std::to_string(probe.rejectedSelectionNodeOverlap) +
                    " rejected_selection_value_overlap=" +
                    std::to_string(probe.rejectedSelectionValueOverlap));
            logInfo("activity-schedule final-fanin pullback probe distribution: eligible_by_gain=" +
                    formatTopCounts(probe.eligibleByGain, 32) +
                    " eligible_by_node_ops=" + formatTopCounts(probe.eligibleByNodeOps, 32) +
                    " eligible_by_max_width=" + formatTopCounts(probe.eligibleByMaxWidth, 32) +
                    " selected_by_gain=" + formatTopCounts(probe.selectedByGain, 32) +
                    " selected_by_node_ops=" + formatTopCounts(probe.selectedByNodeOps, 32) +
                    " selected_by_max_width=" + formatTopCounts(probe.selectedByMaxWidth, 32));
        }
        else if (options_.finalFaninPullbackPolicy == "strict")
        {
            if (materializePerf.splitOversizeComputeNodes != 0)
            {
                error(*graph,
                      "activity-schedule strict final-fanin pullback does not support split compute nodes: split_nodes=" +
                          std::to_string(materializePerf.splitOversizeComputeNodes) +
                          " split_supernodes=" +
                          std::to_string(materializePerf.splitOversizeComputeNodeSupernodes));
                result.failed = true;
                return result;
            }
            const auto strictStart = std::chrono::steady_clock::now();
            const ActivityScheduleBuild baselineBuild = build;
            std::vector<FinalFaninPullbackCandidate> selectedCandidates;
            const FinalFaninPullbackProbeStats evaluation =
                evaluateFinalFaninPullback(*graph,
                                           options_,
                                           rewrite,
                                           baselineBuild,
                                           materializePerf,
                                           &selectedCandidates);
            if (evaluation.skippedOversize || evaluation.skippedFinalTopo ||
                evaluation.selected != selectedCandidates.size())
            {
                error(*graph,
                      "activity-schedule strict final-fanin pullback evaluator gate failed: selected=" +
                          std::to_string(evaluation.selected) +
                          " plans=" + std::to_string(selectedCandidates.size()) +
                          " skipped_oversize=" +
                          std::string(evaluation.skippedOversize ? "true" : "false") +
                          " skipped_final_topo=" +
                          std::string(evaluation.skippedFinalTopo ? "true" : "false"));
                result.failed = true;
                return result;
            }
            FinalFaninPullbackStrictStats strictStats;
            std::string strictError;
            if (!applyFinalFaninPullbackStrict(*graph,
                                               options_,
                                               opData,
                                               rewrite,
                                               baselineBuild,
                                               selectedCandidates,
                                               evaluation.projectedBaeGain,
                                               build,
                                               strictStats,
                                               strictError))
            {
                error(*graph, strictError);
                result.failed = true;
                return result;
            }
            const std::uint64_t strictMs = elapsedMs(strictStart);
            logInfo("activity-schedule final-fanin pullback strict: policy=" +
                    options_.finalFaninPullbackPolicy +
                    " max_node_ops=" +
                    std::to_string(options_.finalFaninPullbackMaxNodeOps) +
                    " max_value_width=" +
                    std::to_string(options_.finalFaninPullbackMaxValueWidth) +
                    " min_gain=" + std::to_string(options_.finalFaninPullbackMinGain) +
                    " max_moves=" + std::to_string(options_.finalFaninPullbackMaxMoves) +
                    " max_moved_op_ppm=" +
                    std::to_string(options_.finalFaninPullbackMaxMovedOpPpm) +
                    " scanned=" + std::to_string(evaluation.scanned) +
                    " pure=" + std::to_string(evaluation.pure) +
                    " common_source=" + std::to_string(evaluation.commonSource) +
                    " exact_eligible=" + std::to_string(evaluation.exactEligible) +
                    " selected=" + std::to_string(evaluation.selected) +
                    " applied=" + std::to_string(strictStats.applied) +
                    " moved_ops=" + std::to_string(evaluation.movedOps) +
                    " moved_op_limit=" + std::to_string(evaluation.movedOpLimit) +
                    " eligible_projected_bae_gain=" +
                    std::to_string(evaluation.eligibleProjectedBaeGain) +
                    " projected_bae_gain=" +
                    std::to_string(evaluation.projectedBaeGain) +
                    " actual_bae_gain=" + std::to_string(strictStats.actualBaeGain) +
                    " compute_bae_before=" +
                    std::to_string(strictStats.computeBaeBefore) +
                    " compute_bae_after=" +
                    std::to_string(strictStats.computeBaeAfter) +
                    " compute_commit_before=" +
                    std::to_string(strictStats.computeCommitBefore) +
                    " compute_commit_after=" +
                    std::to_string(strictStats.computeCommitAfter) +
                    " dag_edges_before=" + std::to_string(strictStats.dagEdgesBefore) +
                    " dag_edges_after=" + std::to_string(strictStats.dagEdgesAfter) +
                    " elapsed_ms=" + std::to_string(strictMs));
            logInfo("activity-schedule final-fanin pullback strict validators: supernodes=" +
                    std::string(strictStats.supernodesValid ? "true" : "false") +
                    " kinds=" + std::string(strictStats.kindsValid ? "true" : "false") +
                    " scheduled_ops=" +
                    std::string(strictStats.scheduledOpsValid ? "true" : "false") +
                    " capacity=" + std::string(strictStats.capacityValid ? "true" : "false") +
                    " commit=" + std::string(strictStats.commitValid ? "true" : "false") +
                    " compute_partition=" +
                    std::string(strictStats.computePartitionValid ? "true" : "false") +
                    " dag=" + std::string(strictStats.dagValid ? "true" : "false") +
                    " topo=" + std::string(strictStats.topoValid ? "true" : "false") +
                    " state_read=" +
                    std::string(strictStats.stateReadValid ? "true" : "false") +
                    " compute_commit=" +
                    std::string(strictStats.computeCommitValid ? "true" : "false") +
                    " bae_gain=" + std::string(strictStats.baeGainValid ? "true" : "false"));
            logInfo("activity-schedule final-fanin pullback strict selection: rejected_move_limit=" +
                    std::to_string(evaluation.rejectedSelectionMoveLimit) +
                    " rejected_budget=" +
                    std::to_string(evaluation.rejectedSelectionBudget) +
                    " rejected_capacity=" +
                    std::to_string(evaluation.rejectedSelectionCapacity) +
                    " rejected_target_empty=" +
                    std::to_string(evaluation.rejectedSelectionTargetEmpty) +
                    " rejected_node_overlap=" +
                    std::to_string(evaluation.rejectedSelectionNodeOverlap) +
                    " rejected_value_overlap=" +
                    std::to_string(evaluation.rejectedSelectionValueOverlap) +
                    " eligible_by_gain=" + formatTopCounts(evaluation.eligibleByGain, 32) +
                    " selected_by_gain=" + formatTopCounts(evaluation.selectedByGain, 32) +
                    " selected_by_node_ops=" +
                    formatTopCounts(evaluation.selectedByNodeOps, 32) +
                    " selected_by_max_width=" +
                    formatTopCounts(evaluation.selectedByMaxWidth, 32));
        }

        const std::string keyPrefix = options_.path + ".activity_schedule.";
        const auto exportStart = std::chrono::steady_clock::now();
        logInfo("activity-schedule progress: export_session start");
        setSessionValue(keyPrefix + "supernode_to_ops",
                        build.supernodeToOps,
                        "activity-schedule.supernode-to-ops");
        setSessionValue(keyPrefix + "op_to_supernode",
                        build.opToSupernode,
                        "activity-schedule.op-to-supernode");
        setSessionValue(keyPrefix + "dag", build.dag, "activity-schedule.dag");
        setSessionValue(keyPrefix + "supernode_kind",
                        build.supernodeKinds,
                        "activity-schedule.supernode-kind");
        setSessionValue(keyPrefix + "compute_nodes_by_supernode",
                        build.computeNodesBySupernode,
                        "activity-schedule.compute-nodes-by-supernode");
        setSessionValue(keyPrefix + "value_fanout", build.valueFanout, "activity-schedule.value-fanout");
        setSessionValue(keyPrefix + "topo_order", build.topoOrder, "activity-schedule.topo-order");
        setSessionValue(keyPrefix + "state_read_supernodes",
                        build.stateReadSupernodes,
                        "activity-schedule.state-read-supernodes");
        const ActivityScheduleSummaryStats summaryStats =
            buildActivityScheduleSummaryStats(build, rewrite, opData, *graph);
        setSessionValue(keyPrefix + "summary_stats",
                        encodeActivityScheduleSummaryStatsJson(summaryStats),
                        "stats");
        const std::uint64_t exportMs = elapsedMs(exportStart);
        logInfo("activity-schedule progress: export_session done elapsed_ms=" +
                std::to_string(exportMs));

        const std::size_t computeSupernodes =
            std::count(build.supernodeKinds.begin(),
                       build.supernodeKinds.end(),
                       ActivityScheduleSupernodeKind::Compute);
        const std::size_t commitSupernodes =
            std::count(build.supernodeKinds.begin(),
                       build.supernodeKinds.end(),
                       ActivityScheduleSupernodeKind::Commit);

        logInfo("activity-schedule timing(ms): build_op_data=" + std::to_string(buildOpDataMs) +
                " compute_node_build=" + std::to_string(computeNodeMs) +
                " local_shared_compute_clone=" +
                std::to_string(localSharedCloneMs) +
                " freeze_after_compute_node=" + std::to_string(freezeMs) +
                " final_materialize=" + std::to_string(materializeMs) +
                " export_session=" + std::to_string(exportMs) +
                " total=" + std::to_string(elapsedMs(totalStart)));
        logInfo("activity-schedule compute-node materialize timing(ms): init_clusters=" +
                std::to_string(materializePerf.initClustersMs) +
                " topo_before_coarsen=" + std::to_string(materializePerf.topoBeforeCoarsenMs) +
                " coarsen=" + std::to_string(materializePerf.coarsenMs) +
                " topo_after_coarsen=" + std::to_string(materializePerf.topoAfterCoarsenMs) +
                " build_cluster_view=" + std::to_string(materializePerf.buildClusterViewMs) +
                " dp_segment=" + std::to_string(materializePerf.dpSegmentMs) +
                " flatten_segments=" + std::to_string(materializePerf.flattenSegmentsMs) +
                " build_final_supernodes=" + std::to_string(materializePerf.buildFinalSupernodesMs) +
                " build_final_dag=" + std::to_string(materializePerf.buildFinalDagMs) +
                " build_state_read_sets=" + std::to_string(materializePerf.buildStateReadSetsMs) +
                " final_topo=" + std::to_string(materializePerf.finalTopoMs));
        logInfo("activity-schedule compute-node final split detail: oversize_compute_nodes=" +
                std::to_string(materializePerf.splitOversizeComputeNodes) +
                " split_supernodes=" +
                std::to_string(materializePerf.splitOversizeComputeNodeSupernodes));
        logInfo("activity-schedule DP segment penalty: ppm=" +
                std::to_string(options_.dpSegmentPenaltyPpm));
        logInfo("activity-schedule final topo policy: " + options_.finalTopoPolicy);
        if (options_.kahnLevelPackPolicy != "off")
        {
            logInfo("activity-schedule Kahn-level pack detail: policy=" +
                    options_.kahnLevelPackPolicy +
                    " elapsed_ms=" + std::to_string(materializePerf.kahnLevelPackMs) +
                    " levels=" + std::to_string(materializePerf.kahnLevelPackLevels) +
                    " locked_clusters=" +
                    std::to_string(materializePerf.kahnLevelPackLockedClusters) +
                    " shared_values=" +
                    std::to_string(materializePerf.kahnLevelPackSharedValues) +
                    " affinity_pairs=" +
                    std::to_string(materializePerf.kahnLevelPackAffinityPairs) +
                    " candidates=" + std::to_string(materializePerf.kahnLevelPackCandidates) +
                    " swaps=" + std::to_string(materializePerf.kahnLevelPackSwaps) +
                    " moved_clusters=" +
                    std::to_string(materializePerf.kahnLevelPackMovedClusters) +
                    " moved_ops=" + std::to_string(materializePerf.kahnLevelPackMovedOps) +
                    " rejected_budget=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedBudget) +
                    " rejected_locked=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedLocked) +
                    " rejected_topo=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedTopo) +
                    " rejected_segment_count=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedSegmentCount) +
                    " rejected_segment_shape=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedSegmentShape) +
                    " rejected_policy=" +
                    std::to_string(materializePerf.kahnLevelPackRejectedPolicy) +
                    " baseline_segments=" +
                    std::to_string(materializePerf.kahnLevelPackBaselineSegments) +
                    " candidate_segments=" +
                    std::to_string(materializePerf.kahnLevelPackCandidateSegments) +
                    " compute_bae_before=" +
                    std::to_string(materializePerf.kahnLevelPackBeforeComputeBae) +
                    " compute_bae_candidate=" +
                    std::to_string(materializePerf.kahnLevelPackCandidateComputeBae) +
                    " compute_bae_after=" +
                    std::to_string(materializePerf.kahnLevelPackAfterComputeBae) +
                    " dag_edges_before=" +
                    std::to_string(materializePerf.kahnLevelPackBeforeDagEdges) +
                    " dag_edges_candidate=" +
                    std::to_string(materializePerf.kahnLevelPackCandidateDagEdges) +
                    " dag_edges_after=" +
                    std::to_string(materializePerf.kahnLevelPackAfterDagEdges) +
                    " candidate_built=" +
                    std::string(materializePerf.kahnLevelPackCandidateBuilt ? "true" : "false") +
                    " final_topo_valid=" +
                    std::string(materializePerf.kahnLevelPackFinalTopoValid ? "true" : "false") +
                    " skipped_oversize=" +
                    std::string(materializePerf.kahnLevelPackSkippedOversize ? "true" : "false") +
                    " skipped_split_sensitive=" +
                    std::string(materializePerf.kahnLevelPackSkippedSplitSensitive ? "true" : "false") +
                    " adopted=" +
                    std::string(materializePerf.kahnLevelPackAdopted ? "true" : "false"));
        }
        if (options_.postDpRefinePolicy != "off")
        {
            logInfo("activity-schedule post-DP refine detail: policy=" +
                    options_.postDpRefinePolicy +
                    " elapsed_ms=" + std::to_string(materializePerf.postDpRefineMs) +
                    " rounds=" + std::to_string(materializePerf.postDpRefineRounds) +
                    " candidates=" + std::to_string(materializePerf.postDpRefineCandidates) +
                    " moves=" + std::to_string(materializePerf.postDpRefineMoves) +
                    " swaps=" + std::to_string(materializePerf.postDpRefineSwaps) +
                    " moved_ops=" + std::to_string(materializePerf.postDpRefineMovedOps) +
                    " locked_clusters=" +
                    std::to_string(materializePerf.postDpRefineLockedClusters) +
                    " rejected_capacity=" +
                    std::to_string(materializePerf.postDpRefineRejectedCapacity) +
                    " rejected_topo=" +
                    std::to_string(materializePerf.postDpRefineRejectedTopo) +
                    " rejected_policy=" +
                    std::to_string(materializePerf.postDpRefineRejectedPolicy) +
                    " compute_bae_before=" +
                    std::to_string(materializePerf.postDpRefineBeforeComputeBae) +
                    " compute_bae_after=" +
                    std::to_string(materializePerf.postDpRefineAfterComputeBae) +
                    " dag_edges_before=" +
                    std::to_string(materializePerf.postDpRefineBeforeDagEdges) +
                    " dag_edges_after=" +
                    std::to_string(materializePerf.postDpRefineAfterDagEdges));
        }
        if (options_.postDpRefinePolicy == "swap-probe")
        {
            const auto &probe = materializePerf.postDpSwapProbe;
            std::ostringstream opDistribution;
            bool firstBucket = true;
            for (const auto &[ops, count] : probe.eligibleByClusterOps)
            {
                if (!firstBucket)
                {
                    opDistribution << ',';
                }
                firstBucket = false;
                opDistribution << ops << ':' << count;
            }
            logInfo(
                "activity-schedule post-DP swap probe: capacity_blocked_seeds=" +
                std::to_string(probe.capacityBlockedSeeds) +
                " enumerated_rhs=" + std::to_string(probe.enumeratedRhs) +
                " rejected_locked=" + std::to_string(probe.rejectedLocked) +
                " rejected_equal_load=" +
                std::to_string(probe.rejectedEqualLoad) +
                " swap_rejected_topo=" + std::to_string(probe.rejectedTopo) +
                " rejected_dag_support=" +
                std::to_string(probe.rejectedDagSupport) +
                " rejected_dag_edge_count=" +
                std::to_string(probe.rejectedDagEdgeCount) +
                " rejected_dag_support_key=" +
                std::to_string(probe.rejectedDagSupportKey) +
                " rejected_nonpositive_bae=" +
                std::to_string(probe.rejectedNonpositiveBae) +
                " rejected_scan_limit=" +
                std::to_string(probe.rejectedScanLimit) +
                " raw_eligible_swaps=" + std::to_string(probe.rawEligible) +
                " eligible_bae_gain=" + std::to_string(probe.eligibleBaeGain) +
                " eligible_cluster_ops=" + opDistribution.str() +
                " selected_swaps=" + std::to_string(probe.selected) +
                " selected_moved_clusters=" +
                std::to_string(probe.selectedMovedClusters) +
                " selected_moved_ops=" + std::to_string(probe.selectedMovedOps) +
                " rejected_conflict=" + std::to_string(probe.rejectedConflict) +
                " rejected_budget=" + std::to_string(probe.rejectedBudget) +
                " projected_bae_gain=" +
                std::to_string(probe.projectedBaeGain) +
                " actual_bae_gain=" + std::to_string(probe.actualBaeGain) +
                " swap_compute_bae_before=" +
                std::to_string(materializePerf.postDpRefineBeforeComputeBae) +
                " swap_compute_bae_candidate=" +
                std::to_string(probe.computeBaeCandidate) +
                " compute_boundary_values_before=" +
                std::to_string(probe.boundaryValuesBefore) +
                " compute_boundary_values_candidate=" +
                std::to_string(probe.boundaryValuesCandidate) +
                " swap_dag_edges_before=" +
                std::to_string(materializePerf.postDpRefineBeforeDagEdges) +
                " swap_dag_edges_candidate=" +
                std::to_string(probe.dagEdgesCandidate) +
                " segment_count_valid=" +
                std::string(probe.segmentCountValid ? "true" : "false") +
                " segment_ops_valid=" +
                std::string(probe.segmentOpsValid ? "true" : "false") +
                " segment_shape_valid=" +
                std::string(probe.segmentShapeValid ? "true" : "false") +
                " dag_support_valid=" +
                std::string(probe.dagSupportValid ? "true" : "false") +
                " topo_valid=" +
                std::string(probe.topoValid ? "true" : "false") +
                " projected_actual_valid=" +
                std::string(probe.projectedActualValid ? "true" : "false") +
                " valid=" + std::string(probe.valid ? "true" : "false"));
        }
        logInfo("activity-schedule compute-node coarsen detail: enabled=" +
                std::string(options_.enableCoarsen ? "true" : "false") +
                " chain_merge=" + std::string(options_.enableChainMerge ? "true" : "false") +
                " iterations=" + std::to_string(materializePerf.coarsenIterations) +
                " out1_merges=" + std::to_string(materializePerf.coarsenOut1Merges) +
                " in1_merges=" + std::to_string(materializePerf.coarsenIn1Merges) +
                " sibling_merges=" + std::to_string(materializePerf.coarsenSiblingMerges) +
                " clusters_before=" + std::to_string(materializePerf.clustersBeforeCoarsen) +
                " clusters_after=" + std::to_string(materializePerf.clustersAfterCoarsen) +
                " tail_stopped=" + std::string(materializePerf.coarsenTailStopped ? "true" : "false") +
                " tail_iterations=" + std::to_string(materializePerf.coarsenTailIterations) +
                " segments=" + std::to_string(materializePerf.segments) +
                " compute_supernodes=" + std::to_string(materializePerf.computeSupernodes));
        for (const auto &iter : materializePerf.coarsenIterationStats)
        {
            logInfo("activity-schedule timing: compute_node_coarsen_iter=" +
                    std::to_string(iter.iteration) +
                    " clusters=" + std::to_string(iter.clusters) +
                    " cluster_delta=" + std::to_string(iter.clusterDelta) +
                    " changed=" + (iter.changed ? std::string("true") : std::string("false")) +
                    " out1=" + (iter.out1Changed ? std::string("1") : std::string("0")) +
                    " in1=" + (iter.in1Changed ? std::string("1") : std::string("0")) +
                    " siblings=" + (iter.siblingsChanged ? std::string("1") : std::string("0")) +
                    " tail_stop=" + (iter.tailStopped ? std::string("1") : std::string("0")) +
                    " elapsed_ms=" + std::to_string(iter.elapsedMs));
        }
        logInfo("activity-schedule timing detail: compute_nodes=" +
                std::to_string(rewrite.stats.computeNodes) +
                " compute_node_ops_total=" + std::to_string(rewrite.stats.computeNodeOpsTotal) +
                " compute_node_cycle_split_iters=" +
                std::to_string(rewrite.stats.computeNodeCycleSplitIters) +
                " initial_compute_supernodes=" +
                std::to_string(rewrite.stats.initialComputeSupernodes) +
                " initial_compute_supernode_ops_total=" +
                std::to_string(rewrite.stats.initialComputeSupernodeOpsTotal) +
                " initial_compute_supernode_dag_edges=" +
                std::to_string(rewrite.stats.initialComputeSupernodeDagEdges) +
                " initial_boundary_values=" + std::to_string(rewrite.stats.initialBoundaryValues) +
                " initial_boundary_activation_edges=" +
                std::to_string(rewrite.stats.initialBoundaryActivationEdges) +
                " initial_compute_compute_value_pairs=" +
                std::to_string(rewrite.stats.initialComputeComputeValuePairs) +
                " initial_compute_commit_value_pairs=" +
                std::to_string(rewrite.stats.initialComputeCommitValuePairs) +
                " source_clones_in_compute_nodes=" +
                std::to_string(rewrite.stats.sourceClonesInComputeNodes) +
                " local_shared_compute_clones_in_compute_nodes=" +
                std::to_string(rewrite.stats.localSharedComputeClonesInComputeNodes) +
                " direct_source_inputs_to_commit_supernodes=" +
                std::to_string(rewrite.stats.directSourceInputsToCommitSupernodes) +
                " common_expr_compute_nodes=" + std::to_string(rewrite.stats.commonExprComputeNodes) +
                " compute_node_boundary_inputs_total=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputsTotal) +
                " boundary_no_def=" + std::to_string(rewrite.stats.computeNodeBoundaryInputNoDef) +
                " boundary_def_out_of_range=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputDefOutOfRange) +
                " boundary_declared=" + std::to_string(rewrite.stats.computeNodeBoundaryInputDeclared) +
                " declared_boundary_values=" +
                std::to_string(rewrite.stats.computeNodeBoundaryDeclaredValues) +
                " declared_boundary_edges=" +
                std::to_string(rewrite.stats.computeNodeBoundaryDeclaredEdges) +
                " declared_cut_fixed=" +
                std::to_string(rewrite.stats.computeNodeDeclaredCutViolationsFixed) +
                " declared_cut_fatal=" +
                std::to_string(rewrite.stats.computeNodeDeclaredCutViolationsFatal) +
                " boundary_source_spill=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputSourceSpill) +
                " boundary_unsupported=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputUnsupported) +
                " boundary_existing_owner=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputExistingOwner) +
                " boundary_existing_common_owner=" +
                std::to_string(rewrite.stats.computeNodeBoundaryInputExistingCommonOwner) +
                " boundary_shared=" + std::to_string(rewrite.stats.computeNodeBoundaryInputShared) +
                " boundary_capacity=" + std::to_string(rewrite.stats.computeNodeBoundaryInputCapacity) +
                " compute_node_boundary_values=" +
                std::to_string(rewrite.stats.computeNodeBoundaryValues) +
                " commit_input_root_values=" + std::to_string(rewrite.stats.commitInputRootValues) +
                " commit_sink_ops=" + std::to_string(rewrite.stats.commitSinkOps) +
                " commit_event_key_runs=" + std::to_string(rewrite.stats.commitEventKeyRuns) +
                " commit_event_keys=" + std::to_string(rewrite.stats.commitEventKeys) +
                " compute_supernodes=" + std::to_string(computeSupernodes) +
                " commit_supernodes=" + std::to_string(commitSupernodes) +
                " topo_edges=" + std::to_string(opData.topoEdges.size()) +
                " graph_ops=" + std::to_string(graph->operations().size()) +
                " graph_values=" + std::to_string(graph->values().size()));
        logInfo("activity-schedule compute-node existing common owner detail: by_kind_top=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByKind, 10) +
                " by_width=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByWidthBucket, 10) +
                " by_fanout=" +
                formatTopCounts(rewrite.stats.computeNodeBoundaryExistingCommonOwnerByFanoutBucket, 10));

        std::ostringstream summary;
        summary << "activity-schedule: path=" << options_.path
                << " graph=" << graph->symbol()
                << " supernodes=" << build.supernodeToOps.size()
                << " compute_supernodes=" << computeSupernodes
                << " commit_supernodes=" << commitSupernodes
                << " compute_nodes=" << rewrite.stats.computeNodes
                << " source_clones=" << rewrite.stats.sourceClonesInComputeNodes
                << " local_shared_compute_clones=" << rewrite.stats.localSharedComputeClonesInComputeNodes
                << " eligible_ops=" << opData.topoOps.size()
                << " state_read_sets=" << build.stateReadSupernodes.size()
                << " final_topo_policy=" << options_.finalTopoPolicy
                << " graph_changed=" << (graphChanged ? "true" : "false");
        logInfo(summary.str());

        result.changed = graphChanged;
        result.failed = false;
        return result;
    }

} // namespace wolvrix::lib::transform
