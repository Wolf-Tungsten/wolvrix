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

            std::uint64_t initClustersMs = 0;
            std::uint64_t topoBeforeCoarsenMs = 0;
            std::uint64_t coarsenMs = 0;
            std::uint64_t topoAfterCoarsenMs = 0;
            std::uint64_t buildClusterViewMs = 0;
            std::uint64_t dpSegmentMs = 0;
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
                const std::size_t mergeLimit =
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
                    std::vector<uint32_t> positions;
                    auto flushPositions = [&]()
                    {
                        if (positions.empty())
                        {
                            return;
                        }
                        partition.clusters.push_back(std::move(positions));
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
                        if (guardPositions.size() > mergeLimit)
                        {
                            flushPositions();
                            partition.clusters.emplace_back(guardPositions.begin(), guardPositions.end());
                            continue;
                        }
                        if (!positions.empty() && positions.size() + guardPositions.size() > mergeLimit)
                        {
                            flushPositions();
                        }
                        positions.insert(positions.end(), guardPositions.begin(), guardPositions.end());
                    }
                    flushPositions();
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
                    const auto originalOperands = graph_.opOperands(opId);
                    std::vector<wolvrix::lib::grh::ValueId> operands(originalOperands.begin(), originalOperands.end());
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
                        if (!defOp.valid() || defOp.index >= rewrite.computeNodeOfOp.size())
                        {
                            continue;
                        }
                        const uint32_t predNode = rewrite.computeNodeOfOp[defOp.index];
                        if (predNode == kInvalidActivitySupernodeId || predNode >= view.clusterOfNode.size())
                        {
                            continue;
                        }
                        const uint32_t fromCluster = view.clusterOfNode[predNode];
                        if (fromCluster == kInvalidActivitySupernodeId || fromCluster == toCluster)
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
                            if (fromCluster < out.sourceValuesByCluster.size())
                            {
                                out.sourceValuesByCluster[fromCluster].push_back(it->second);
                            }
                        }
                        auto &targets = out.valueFanouts[it->second].targetClusters;
                        if (std::find(targets.begin(), targets.end(), toCluster) == targets.end())
                        {
                            targets.push_back(toCluster);
                            ++out.weights[packClusterPair(fromCluster, toCluster)];
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
                    dp[end] = dp[end - 1] + 1;
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
                                     std::string &error)
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
            const NodeClusterView clusterView =
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
            const auto clusterValueEdges = buildClusterValueEdges(clusterView, rewrite, graph);
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
                                              1.0);
            if (perf)
            {
                perf->dpSegmentMs = elapsedMs(dpSegmentStart);
                perf->segments = segments.size();
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
        logInfo("activity-schedule final topo policy: " + options_.finalTopoPolicy);
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
