#include "transform/repcut.hpp"
#include "transform/repcut_boundary_bundle.hpp"
#include "transform/repcut_partition_set.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if WOLVRIX_HAVE_MT_KAHYPAR
#include <mtkahypar.h>
#endif

namespace wolvrix::lib::transform
{

    namespace detail
    {

        RepcutBoundaryBundleResult buildRepcutBoundaryBundles(std::span<const RepcutBoundaryValueDesc> values)
        {
            struct GroupKey
            {
                uint32_t ownerPart = 0;
                std::string dstSignature;

                bool operator==(const GroupKey &other) const
                {
                    return ownerPart == other.ownerPart && dstSignature == other.dstSignature;
                }
            };

            struct GroupKeyHash
            {
                std::size_t operator()(const GroupKey &key) const
                {
                    std::size_t seed = std::hash<uint32_t>{}(key.ownerPart);
                    seed ^= std::hash<std::string>{}(key.dstSignature) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
                    return seed;
                }
            };

            auto formatSignature = [](const std::vector<uint32_t> &parts) {
                std::ostringstream oss;
                for (std::size_t i = 0; i < parts.size(); ++i)
                {
                    if (i > 0)
                    {
                        oss << "_";
                    }
                    oss << "p" << parts[i];
                }
                return oss.str();
            };

            std::unordered_map<GroupKey, std::vector<uint64_t>, GroupKeyHash> membersByKey;
            std::unordered_map<GroupKey, std::vector<uint32_t>, GroupKeyHash> dstPartsByKey;
            std::unordered_map<uint64_t, int64_t> widthByValue;
            membersByKey.reserve(values.size());
            dstPartsByKey.reserve(values.size());
            widthByValue.reserve(values.size());

            for (const auto &value : values)
            {
                if (value.width <= 0 || value.dstParts.empty())
                {
                    continue;
                }
                std::vector<uint32_t> dstParts = value.dstParts;
                std::sort(dstParts.begin(), dstParts.end());
                dstParts.erase(std::unique(dstParts.begin(), dstParts.end()), dstParts.end());
                if (dstParts.empty())
                {
                    continue;
                }

                GroupKey key;
                key.ownerPart = value.ownerPart;
                key.dstSignature = formatSignature(dstParts);
                membersByKey[key].push_back(value.valueKey);
                dstPartsByKey.emplace(key, dstParts);
                widthByValue.emplace(value.valueKey, value.width);
            }

            RepcutBoundaryBundleResult result;
            result.groups.reserve(membersByKey.size());
            for (auto &[key, members] : membersByKey)
            {
                if (members.size() < 2)
                {
                    continue;
                }
                std::sort(members.begin(), members.end());
                RepcutBoundaryBundle bundle;
                bundle.ownerPart = key.ownerPart;
                bundle.dstSignature = key.dstSignature;
                auto dstIt = dstPartsByKey.find(key);
                if (dstIt != dstPartsByKey.end())
                {
                    bundle.dstParts = dstIt->second;
                }
                bundle.members = std::move(members);
                for (const uint64_t member : bundle.members)
                {
                    auto widthIt = widthByValue.find(member);
                    if (widthIt != widthByValue.end())
                    {
                        bundle.totalWidth += widthIt->second;
                    }
                }
                result.groups.push_back(std::move(bundle));
            }

            std::sort(result.groups.begin(),
                      result.groups.end(),
                      [](const RepcutBoundaryBundle &lhs, const RepcutBoundaryBundle &rhs) {
                          if (lhs.ownerPart != rhs.ownerPart)
                          {
                              return lhs.ownerPart < rhs.ownerPart;
                          }
                          if (lhs.dstSignature != rhs.dstSignature)
                          {
                              return lhs.dstSignature < rhs.dstSignature;
                          }
                          const uint64_t lhsFirst =
                              lhs.members.empty() ? std::numeric_limits<uint64_t>::max() : lhs.members.front();
                          const uint64_t rhsFirst =
                              rhs.members.empty() ? std::numeric_limits<uint64_t>::max() : rhs.members.front();
                          return lhsFirst < rhsFirst;
                      });

            result.members.reserve(values.size());
            for (std::size_t groupIndex = 0; groupIndex < result.groups.size(); ++groupIndex)
            {
                const RepcutBoundaryBundle &bundle = result.groups[groupIndex];
                int64_t bitCursor = bundle.totalWidth;
                for (const uint64_t member : bundle.members)
                {
                    auto widthIt = widthByValue.find(member);
                    if (widthIt == widthByValue.end() || widthIt->second <= 0 || bitCursor < widthIt->second)
                    {
                        continue;
                    }
                    RepcutBoundaryBundleMember layout;
                    layout.groupIndex = groupIndex;
                    layout.sliceStart = bitCursor - widthIt->second;
                    layout.sliceEnd = bitCursor - 1;
                    result.members.emplace(member, layout);
                    bitCursor -= widthIt->second;
                }
            }

            return result;
        }

    } // namespace detail

    namespace
    {
        using NodeId = uint32_t;
        using AscId = uint32_t;
        using PieceId = uint32_t;
        constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();
        constexpr PieceId kInvalidPiece = std::numeric_limits<PieceId>::max();
        constexpr std::size_t kMaxConeCollectThreads = 8;
        constexpr std::size_t kConeCollectChunkSize = 64;
        constexpr std::size_t kForbiddenCrossDiagLimit = 12;
        constexpr std::size_t kMaxPartitionCount = 4096;

        using PartitionSet = wolvrix::lib::transform::detail::PartitionSet;

        template <typename T>
        std::optional<T> getAttr(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            std::optional<wolvrix::lib::grh::AttributeValue> attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto ptr = std::get_if<T>(&*attr))
            {
                return *ptr;
            }
            return std::nullopt;
        }

        std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation &op,
                                                               std::string_view key)
        {
            return getAttr<std::vector<std::string>>(op, key);
        }

        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            return getAttr<std::string>(op, key);
        }

        wolvrix::lib::grh::OperationId findMatchingDpicImport(const wolvrix::lib::grh::Graph &graph,
                                                              const wolvrix::lib::grh::Operation &dpiCall)
        {
            if (dpiCall.kind() != wolvrix::lib::grh::OperationKind::kDpicCall)
            {
                return wolvrix::lib::grh::OperationId::invalid();
            }
            const auto importSym = getAttrString(dpiCall, "targetImportSymbol");
            if (!importSym || importSym->empty())
            {
                return wolvrix::lib::grh::OperationId::invalid();
            }
            const wolvrix::lib::grh::OperationId importOpId = graph.findOperation(*importSym);
            if (!importOpId.valid())
            {
                return wolvrix::lib::grh::OperationId::invalid();
            }
            if (graph.opKind(importOpId) != wolvrix::lib::grh::OperationKind::kDpicImport)
            {
                return wolvrix::lib::grh::OperationId::invalid();
            }
            return importOpId;
        }

        std::optional<bool> getAttrBool(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            return getAttr<bool>(op, key);
        }

        bool opHasEvents(const wolvrix::lib::grh::Operation &op)
        {
            auto edges = getAttrStrings(op, "eventEdge");
            return edges && !edges->empty();
        }

        bool isEffectOpKind(wolvrix::lib::grh::OperationKind kind)
        {
            return kind == wolvrix::lib::grh::OperationKind::kDpicCall ||
                   kind == wolvrix::lib::grh::OperationKind::kSystemTask;
        }

        bool opProducesEffectResult(const wolvrix::lib::grh::Operation &op)
        {
            if (!isEffectOpKind(op.kind()))
            {
                return false;
            }

            const auto outArgNames = getAttrStrings(op, "outArgName");
            const auto inoutArgNames = getAttrStrings(op, "inoutArgName");
            return getAttrBool(op, "hasReturn").value_or(false) ||
                   (outArgNames && !outArgNames->empty()) ||
                   (inoutArgNames && !inoutArgNames->empty());
        }

        bool isEffectExecutionAnchorOp(const wolvrix::lib::grh::Operation &op)
        {
            return isEffectOpKind(op.kind());
        }

        bool isSinkOpKind(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                return true;
            default:
                return false;
            }
        }

        bool isSinkOp(const wolvrix::lib::grh::Operation &op)
        {
            return isSinkOpKind(op.kind()) || isEffectExecutionAnchorOp(op);
        }

        bool isHierOpKind(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
                return true;
            default:
                return false;
            }
        }

        bool isSourceOpKind(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                return true;
            default:
                return false;
            }
        }

        bool isCombOp(const wolvrix::lib::grh::Operation &op)
        {
            if (opHasEvents(op))
            {
                return false;
            }

            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            case wolvrix::lib::grh::OperationKind::kAdd:
            case wolvrix::lib::grh::OperationKind::kSub:
            case wolvrix::lib::grh::OperationKind::kMul:
            case wolvrix::lib::grh::OperationKind::kDiv:
            case wolvrix::lib::grh::OperationKind::kMod:
            case wolvrix::lib::grh::OperationKind::kEq:
            case wolvrix::lib::grh::OperationKind::kNe:
            case wolvrix::lib::grh::OperationKind::kCaseEq:
            case wolvrix::lib::grh::OperationKind::kCaseNe:
            case wolvrix::lib::grh::OperationKind::kWildcardEq:
            case wolvrix::lib::grh::OperationKind::kWildcardNe:
            case wolvrix::lib::grh::OperationKind::kLt:
            case wolvrix::lib::grh::OperationKind::kLe:
            case wolvrix::lib::grh::OperationKind::kGt:
            case wolvrix::lib::grh::OperationKind::kGe:
            case wolvrix::lib::grh::OperationKind::kAnd:
            case wolvrix::lib::grh::OperationKind::kOr:
            case wolvrix::lib::grh::OperationKind::kXor:
            case wolvrix::lib::grh::OperationKind::kXnor:
            case wolvrix::lib::grh::OperationKind::kNot:
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
            case wolvrix::lib::grh::OperationKind::kLogicOr:
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            case wolvrix::lib::grh::OperationKind::kReduceAnd:
            case wolvrix::lib::grh::OperationKind::kReduceOr:
            case wolvrix::lib::grh::OperationKind::kReduceXor:
            case wolvrix::lib::grh::OperationKind::kReduceNor:
            case wolvrix::lib::grh::OperationKind::kReduceNand:
            case wolvrix::lib::grh::OperationKind::kReduceXnor:
            case wolvrix::lib::grh::OperationKind::kShl:
            case wolvrix::lib::grh::OperationKind::kLShr:
            case wolvrix::lib::grh::OperationKind::kAShr:
            case wolvrix::lib::grh::OperationKind::kMux:
            case wolvrix::lib::grh::OperationKind::kAssign:
            case wolvrix::lib::grh::OperationKind::kConcat:
            case wolvrix::lib::grh::OperationKind::kReplicate:
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            case wolvrix::lib::grh::OperationKind::kSliceArray:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return true;
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            {
                auto sideEffects = getAttrBool(op, "hasSideEffects");
                return !sideEffects || !*sideEffects;
            }
            default:
                return false;
            }
        }

        bool isAscConeTraversalOp(const wolvrix::lib::grh::Operation &op)
        {
            return isCombOp(op) || opProducesEffectResult(op);
        }

        bool isSourceValue(const wolvrix::lib::grh::Graph &graph,
                           wolvrix::lib::grh::ValueId value,
                           const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues)
        {
            if (graph.valueIsInput(value) || inoutInputValues.find(value) != inoutInputValues.end())
            {
                return true;
            }

            const wolvrix::lib::grh::OperationId defOp = graph.valueDef(value);
            if (!defOp.valid())
            {
                return true;
            }

            const wolvrix::lib::grh::Operation op = graph.getOperation(defOp);
            return isSourceOpKind(op.kind());
        }

        struct PhaseAData
        {
            std::unordered_map<wolvrix::lib::grh::OperationId, NodeId, wolvrix::lib::grh::OperationIdHash> opToNode;
            std::vector<wolvrix::lib::grh::OperationId> nodeToOp;
            std::vector<std::vector<NodeId>> inNeighbors;
            std::vector<std::vector<NodeId>> outNeighbors;
        };

        struct SinkRef
        {
            enum class Kind
            {
                Operation,
                OutputValue
            };

            Kind kind = Kind::Operation;
            wolvrix::lib::grh::OperationId op = wolvrix::lib::grh::OperationId::invalid();
            wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
        };

        struct AscInfo
        {
            std::vector<size_t> sinks;
            std::vector<NodeId> combOps;
        };

        struct PhaseBData
        {
            std::vector<SinkRef> sinks;
            std::vector<AscInfo> ascs;
            std::vector<AscId> sinkToAsc;
            std::vector<std::vector<AscId>> nodeToAscs;
            std::vector<std::unordered_set<NodeId>> pieces;
            std::vector<PieceId> nodeToPiece;
            std::vector<std::vector<AscId>> pieceToAscs;
        };

        using ProgressLogger = std::function<void(const std::string &)>;

        struct HyperGraph
        {
            struct HyperEdge
            {
                std::vector<AscId> nodes;
                uint32_t weight = 1;
            };

            std::vector<uint32_t> nodeWeights;
            std::vector<HyperEdge> edges;
        };

        struct StorageInfo
        {
            wolvrix::lib::grh::OperationId declOp = wolvrix::lib::grh::OperationId::invalid();
            std::vector<wolvrix::lib::grh::OperationId> readPorts;
            std::vector<wolvrix::lib::grh::OperationId> writePorts;
        };

        struct ValueInfo
        {
            std::string symbol;
            int32_t width = 0;
            bool isSigned = false;
            wolvrix::lib::grh::ValueType type = wolvrix::lib::grh::ValueType::Logic;
            std::optional<wolvrix::lib::grh::SrcLoc> srcLoc;
        };

        struct BoundaryPortGroupKey
        {
            uint32_t ownerPart = 0;
            std::string dstSignature;

            bool operator==(const BoundaryPortGroupKey &other) const
            {
                return ownerPart == other.ownerPart && dstSignature == other.dstSignature;
            }
        };

        struct BoundaryPortGroupKeyHash
        {
            std::size_t operator()(const BoundaryPortGroupKey &key) const
            {
                std::size_t seed = std::hash<uint32_t>{}(key.ownerPart);
                seed ^= std::hash<std::string>{}(key.dstSignature) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        struct BoundaryPortGroup
        {
            uint32_t ownerPart = 0;
            std::vector<uint32_t> dstParts;
            std::string dstSignature;
            std::vector<wolvrix::lib::grh::ValueId> members;
            int64_t totalWidth = 0;
        };

        struct BoundaryPortGroupMemberInfo
        {
            std::size_t groupIndex = 0;
            int64_t sliceStart = 0;
            int64_t sliceEnd = 0;
        };

        struct CrossPartitionValue
        {
            wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
            uint32_t srcPart = 0;
            uint32_t dstPart = 0;
            bool requiresPort = false;
            bool allowed = true;
        };

        struct ForbiddenCrossSample
        {
            wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
            wolvrix::lib::grh::OperationId defOp = wolvrix::lib::grh::OperationId::invalid();
            wolvrix::lib::grh::OperationKind defKind = wolvrix::lib::grh::OperationKind::kConstant;
            bool defKindKnown = false;
            wolvrix::lib::grh::OperationId useOp = wolvrix::lib::grh::OperationId::invalid();
            uint32_t srcPart = 0;
            uint32_t dstPart = 0;
        };

        std::string jsonEscape(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const char ch : text)
            {
                switch (ch)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
            }
            return out;
        }

        std::string formatValueId(wolvrix::lib::grh::ValueId id)
        {
            std::ostringstream oss;
            oss << "v" << id.index << "g" << id.generation;
            return oss.str();
        }

        std::string formatOperationRef(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationId opId)
        {
            if (!opId.valid())
            {
                return "<invalid>";
            }
            const auto op = graph.getOperation(opId);
            std::ostringstream oss;
            oss << "o" << opId.index << "g" << opId.generation
                << ":" << wolvrix::lib::grh::toString(op.kind())
                << ":" << op.symbolText();
            return oss.str();
        }

        ValueInfo captureValueInfo(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId valueId)
        {
            const auto value = graph.getValue(valueId);
            ValueInfo info;
            info.symbol = std::string(value.symbolText());
            info.width = value.width();
            info.isSigned = value.isSigned();
            info.type = value.type();
            info.srcLoc = value.srcLoc();
            return info;
        }

        std::string normalizePortBase(std::string_view text, std::string_view fallback)
        {
            std::string normalized = wolvrix::lib::grh::Graph::normalizeComponent(text);
            if (normalized.empty())
            {
                normalized = std::string(fallback);
            }
            if (!normalized.empty() && normalized.front() >= '0' && normalized.front() <= '9')
            {
                normalized.insert(normalized.begin(), '_');
            }
            return normalized;
        }

        std::vector<uint32_t> collectPartitionSetParts(const PartitionSet &set)
        {
            std::vector<uint32_t> parts;
            set.forEach([&](uint32_t partId) { parts.push_back(partId); });
            return parts;
        }

        std::string formatPartitionSignature(const std::vector<uint32_t> &parts)
        {
            std::ostringstream oss;
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                if (i > 0)
                {
                    oss << "_";
                }
                oss << "p" << parts[i];
            }
            return oss.str();
        }

        std::string uniquePortName(std::unordered_set<std::string> &used, std::string base)
        {
            std::string candidate = std::move(base);
            if (used.insert(candidate).second)
            {
                return candidate;
            }
            const std::string root = candidate;
            int suffix = 0;
            while (true)
            {
                candidate = root + "_" + std::to_string(++suffix);
                if (used.insert(candidate).second)
                {
                    return candidate;
                }
            }
        }

        std::string uniqueGraphName(wolvrix::lib::grh::Design &design, std::string base)
        {
            std::string candidate = std::move(base);
            const std::string root = candidate;
            int suffix = 0;
            while (design.findGraph(candidate) != nullptr)
            {
                candidate = root + "_" + std::to_string(++suffix);
            }
            return candidate;
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
                if (!opId.valid())
                {
                    continue;
                }
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
                error = "repcut path must not be empty";
                return std::nullopt;
            }
            if (segments.size() == 1)
            {
                if (design.findGraph(segments.front()) == nullptr)
                {
                    error = "repcut graph not found: " + segments.front();
                    return std::nullopt;
                }
                return segments.front();
            }

            auto *current = design.findGraph(segments.front());
            if (current == nullptr)
            {
                error = "repcut root graph not found: " + segments.front();
                return std::nullopt;
            }
            for (std::size_t i = 1; i < segments.size(); ++i)
            {
                const auto instOp = findUniqueInstance(*current, segments[i]);
                if (!instOp.valid())
                {
                    error = "repcut instance not found or not unique: " + segments[i];
                    return std::nullopt;
                }
                const auto op = current->getOperation(instOp);
                const auto moduleName = getAttrString(op, "moduleName");
                if (!moduleName || moduleName->empty())
                {
                    error = "repcut instance missing moduleName: " + segments[i];
                    return std::nullopt;
                }
                current = design.findGraph(*moduleName);
                if (current == nullptr)
                {
                    error = "repcut target module graph not found: " + *moduleName;
                    return std::nullopt;
                }
            }
            return current->symbol();
        }

        wolvrix::lib::grh::SymbolId internUniqueSymbol(wolvrix::lib::grh::Graph &graph, std::string base)
        {
            if (base.empty())
            {
                base = "value";
            }
            std::string candidate = std::move(base);
            const std::string root = candidate;
            int suffix = 0;
            while (true)
            {
                const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(candidate);
                if (sym.valid() && !graph.findValue(sym).valid() && !graph.findOperation(sym).valid())
                {
                    return sym;
                }
                candidate = root + "_" + std::to_string(++suffix);
            }
        }

        wolvrix::lib::grh::OperationId buildInstance(
            wolvrix::lib::grh::Graph &parent,
            std::string_view moduleName,
            std::string_view instanceBase,
            const wolvrix::lib::grh::Graph &target,
            const std::unordered_map<std::string, wolvrix::lib::grh::ValueId> &inputMapping,
            const std::unordered_map<std::string, wolvrix::lib::grh::ValueId> &outputMapping)
        {
            const std::string opBase = std::string("inst_") + std::string(instanceBase);
            const wolvrix::lib::grh::SymbolId opSym = internUniqueSymbol(parent, opBase);
            const wolvrix::lib::grh::OperationId inst = parent.createOperation(wolvrix::lib::grh::OperationKind::kInstance, opSym);
            parent.setAttr(inst, "moduleName", std::string(moduleName));

            std::vector<std::string> inputNames;
            inputNames.reserve(target.inputPorts().size());
            for (const auto &port : target.inputPorts())
            {
                inputNames.push_back(port.name);
            }
            std::vector<std::string> outputNames;
            outputNames.reserve(target.outputPorts().size());
            for (const auto &port : target.outputPorts())
            {
                outputNames.push_back(port.name);
            }
            parent.setAttr(inst, "inputPortName", inputNames);
            parent.setAttr(inst, "outputPortName", outputNames);
            parent.setAttr(inst, "instanceName", std::string(instanceBase));

            for (const auto &portName : inputNames)
            {
                auto it = inputMapping.find(portName);
                if (it != inputMapping.end())
                {
                    parent.addOperand(inst, it->second);
                }
            }
            for (const auto &portName : outputNames)
            {
                auto it = outputMapping.find(portName);
                if (it != outputMapping.end())
                {
                    parent.addResult(inst, it->second);
                }
            }
            return inst;
        }

        class DisjointSet
        {
        public:
            explicit DisjointSet(size_t count)
                : parent_(count), rank_(count, 0)
            {
                for (size_t i = 0; i < count; ++i)
                {
                    parent_[i] = i;
                }
            }

            size_t find(size_t value)
            {
                size_t root = value;
                while (parent_[root] != root)
                {
                    root = parent_[root];
                }
                while (parent_[value] != value)
                {
                    const size_t next = parent_[value];
                    parent_[value] = root;
                    value = next;
                }
                return root;
            }

            void unite(size_t lhs, size_t rhs)
            {
                lhs = find(lhs);
                rhs = find(rhs);
                if (lhs == rhs)
                {
                    return;
                }
                if (rank_[lhs] < rank_[rhs])
                {
                    parent_[lhs] = rhs;
                }
                else if (rank_[lhs] > rank_[rhs])
                {
                    parent_[rhs] = lhs;
                }
                else
                {
                    parent_[rhs] = lhs;
                    ++rank_[lhs];
                }
            }

        private:
            std::vector<size_t> parent_;
            std::vector<uint32_t> rank_;
        };

        PhaseAData buildPhaseAData(const wolvrix::lib::grh::Graph &graph)
        {
            PhaseAData data;

            const std::span<const wolvrix::lib::grh::OperationId> ops = graph.operations();
            data.nodeToOp.reserve(ops.size());

            for (const auto opId : ops)
            {
                const NodeId node = static_cast<NodeId>(data.nodeToOp.size());
                data.nodeToOp.push_back(opId);
                data.opToNode.emplace(opId, node);
            }

            data.inNeighbors.resize(data.nodeToOp.size());
            data.outNeighbors.resize(data.nodeToOp.size());

            std::vector<std::unordered_set<NodeId>> inSets(data.nodeToOp.size());
            std::vector<std::unordered_set<NodeId>> outSets(data.nodeToOp.size());

            for (NodeId node = 0; node < data.nodeToOp.size(); ++node)
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(data.nodeToOp[node]);
                for (const auto operand : op.operands())
                {
                    const wolvrix::lib::grh::OperationId predOp = graph.valueDef(operand);
                    if (!predOp.valid())
                    {
                        continue;
                    }
                    auto predIt = data.opToNode.find(predOp);
                    if (predIt == data.opToNode.end())
                    {
                        continue;
                    }
                    const NodeId pred = predIt->second;
                    if (pred == node)
                    {
                        continue;
                    }
                    if (inSets[node].insert(pred).second)
                    {
                        data.inNeighbors[node].push_back(pred);
                    }
                    if (outSets[pred].insert(node).second)
                    {
                        data.outNeighbors[pred].push_back(node);
                    }
                }
            }

            for (NodeId node = 0; node < data.nodeToOp.size(); ++node)
            {
                std::sort(data.inNeighbors[node].begin(), data.inNeighbors[node].end());
                std::sort(data.outNeighbors[node].begin(), data.outNeighbors[node].end());
            }

            return data;
        }

        std::vector<SinkRef> collectSinks(const wolvrix::lib::grh::Graph &graph)
        {
            std::vector<SinkRef> sinks;
            sinks.reserve(graph.operations().size() + graph.outputPorts().size() + graph.inoutPorts().size() * 2);

            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (!isSinkOp(op))
                {
                    continue;
                }
                SinkRef sink;
                sink.kind = SinkRef::Kind::Operation;
                sink.op = opId;
                sinks.push_back(sink);
            }
            for (const auto &port : graph.outputPorts())
            {
                SinkRef sink;
                sink.kind = SinkRef::Kind::OutputValue;
                sink.value = port.value;
                sinks.push_back(sink);
            }
            for (const auto &port : graph.inoutPorts())
            {
                SinkRef outSink;
                outSink.kind = SinkRef::Kind::OutputValue;
                outSink.value = port.out;
                sinks.push_back(outSink);

                SinkRef oeSink;
                oeSink.kind = SinkRef::Kind::OutputValue;
                oeSink.value = port.oe;
                sinks.push_back(oeSink);
            }

            return sinks;
        }

        struct MemSymbolCollectStats
        {
            uint64_t visitedNodes = 0;
            uint64_t memSymbolHits = 0;
        };

        struct MemSymbolIntern
        {
            uint32_t intern(const std::string &symbol)
            {
                auto it = symbolToId.find(symbol);
                if (it != symbolToId.end())
                {
                    return it->second;
                }
                const uint32_t id = static_cast<uint32_t>(idToSymbol.size());
                idToSymbol.push_back(symbol);
                symbolToId.emplace(idToSymbol.back(), id);
                return id;
            }

            std::unordered_map<std::string, uint32_t> symbolToId;
            std::vector<std::string> idToSymbol;
        };

        struct MemSymbolMemo
        {
            std::vector<std::vector<uint32_t>> nodeToMemSymbolIds;
            std::vector<uint8_t> nodeState;
        };

        const std::vector<uint32_t> &collectNodeMemSymbolIds(const wolvrix::lib::grh::Graph &graph,
                                                              const PhaseAData &phaseA,
                                                              NodeId node,
                                                              const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
                                                              MemSymbolIntern &intern,
                                                              MemSymbolMemo &memo,
                                                              MemSymbolCollectStats &stats)
        {
            static const std::vector<uint32_t> kEmpty;
            if (node >= memo.nodeToMemSymbolIds.size() || node >= memo.nodeState.size())
            {
                return kEmpty;
            }

            uint8_t &state = memo.nodeState[node];
            if (state == 2)
            {
                return memo.nodeToMemSymbolIds[node];
            }
            if (state == 1)
            {
                return kEmpty;
            }

            state = 1;
            ++stats.visitedNodes;
            std::vector<uint32_t> &result = memo.nodeToMemSymbolIds[node];
            result.clear();

            const wolvrix::lib::grh::OperationId opId = phaseA.nodeToOp[node];
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
            {
                if (auto sym = getAttrString(op, "memSymbol"))
                {
                    ++stats.memSymbolHits;
                    result.push_back(intern.intern(*sym));
                }
            }

            if (isAscConeTraversalOp(op))
            {
                for (const auto operand : op.operands())
                {
                    if (isSourceValue(graph, operand, inoutInputValues))
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationId predOp = graph.valueDef(operand);
                    if (!predOp.valid())
                    {
                        continue;
                    }
                    auto it = phaseA.opToNode.find(predOp);
                    if (it == phaseA.opToNode.end())
                    {
                        continue;
                    }
                    const std::vector<uint32_t> &childIds = collectNodeMemSymbolIds(graph,
                                                                                    phaseA,
                                                                                    it->second,
                                                                                    inoutInputValues,
                                                                                    intern,
                                                                                    memo,
                                                                                    stats);
                    result.insert(result.end(), childIds.begin(), childIds.end());
                }
            }

            if (result.size() > 1)
            {
                std::sort(result.begin(), result.end());
                result.erase(std::unique(result.begin(), result.end()), result.end());
            }

            state = 2;
            return result;
        }

        void collectConeMemSymbolIds(const wolvrix::lib::grh::Graph &graph,
                                     const PhaseAData &phaseA,
                                     const SinkRef &sink,
                                     const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
                                     MemSymbolIntern &intern,
                                     MemSymbolMemo &memo,
                                     std::unordered_set<uint32_t> &memSymbolIds,
                                     MemSymbolCollectStats &stats)
        {
            auto collectFromValue = [&](wolvrix::lib::grh::ValueId value) {
                if (isSourceValue(graph, value, inoutInputValues))
                {
                    return;
                }

                const wolvrix::lib::grh::OperationId defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    return;
                }
                auto it = phaseA.opToNode.find(defOp);
                if (it == phaseA.opToNode.end())
                {
                    return;
                }

                const std::vector<uint32_t> &ids = collectNodeMemSymbolIds(graph,
                                                                            phaseA,
                                                                            it->second,
                                                                            inoutInputValues,
                                                                            intern,
                                                                            memo,
                                                                            stats);
                memSymbolIds.insert(ids.begin(), ids.end());
            };

            if (sink.kind == SinkRef::Kind::Operation)
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(sink.op);
                for (const auto operand : op.operands())
                {
                    collectFromValue(operand);
                }
            }
            else
            {
                collectFromValue(sink.value);
            }
        }

        struct EffectResultCollectStats
        {
            uint64_t visitedNodes = 0;
            uint64_t effectResultHits = 0;
        };

        struct EffectResultMemo
        {
            std::vector<std::vector<wolvrix::lib::grh::OperationId>> nodeToEffectResultOps;
            std::vector<uint8_t> nodeState;
        };

        const std::vector<wolvrix::lib::grh::OperationId> &
        collectNodeEffectResultOps(
            const wolvrix::lib::grh::Graph &graph,
            const PhaseAData &phaseA,
            NodeId node,
            const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
            EffectResultMemo &memo,
            EffectResultCollectStats &stats)
        {
            static const std::vector<wolvrix::lib::grh::OperationId> kEmpty;
            if (node >= memo.nodeToEffectResultOps.size() || node >= memo.nodeState.size())
            {
                return kEmpty;
            }

            uint8_t &state = memo.nodeState[node];
            if (state == 2)
            {
                return memo.nodeToEffectResultOps[node];
            }
            if (state == 1)
            {
                return kEmpty;
            }

            state = 1;
            ++stats.visitedNodes;
            std::vector<wolvrix::lib::grh::OperationId> &result = memo.nodeToEffectResultOps[node];
            result.clear();

            const wolvrix::lib::grh::OperationId opId = phaseA.nodeToOp[node];
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            if (opProducesEffectResult(op))
            {
                ++stats.effectResultHits;
                result.push_back(opId);
            }

            if (isAscConeTraversalOp(op))
            {
                for (const auto operand : op.operands())
                {
                    if (isSourceValue(graph, operand, inoutInputValues))
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationId predOp = graph.valueDef(operand);
                    if (!predOp.valid())
                    {
                        continue;
                    }
                    auto it = phaseA.opToNode.find(predOp);
                    if (it == phaseA.opToNode.end())
                    {
                        continue;
                    }
                    const auto &childOps =
                        collectNodeEffectResultOps(graph, phaseA, it->second, inoutInputValues, memo, stats);
                    result.insert(result.end(), childOps.begin(), childOps.end());
                }
            }

            if (result.size() > 1)
            {
                std::sort(result.begin(),
                          result.end(),
                          [](wolvrix::lib::grh::OperationId lhs, wolvrix::lib::grh::OperationId rhs) {
                              if (lhs.index != rhs.index)
                              {
                                  return lhs.index < rhs.index;
                              }
                              return lhs.generation < rhs.generation;
                          });
                result.erase(std::unique(result.begin(), result.end()), result.end());
            }

            state = 2;
            return result;
        }

        void collectConeEffectResultOps(
            const wolvrix::lib::grh::Graph &graph,
            const PhaseAData &phaseA,
            const SinkRef &sink,
            const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
            EffectResultMemo &memo,
            std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> &effectResultOps,
            EffectResultCollectStats &stats)
        {
            auto collectFromValue = [&](wolvrix::lib::grh::ValueId value) {
                if (isSourceValue(graph, value, inoutInputValues))
                {
                    return;
                }

                const wolvrix::lib::grh::OperationId defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    return;
                }
                auto it = phaseA.opToNode.find(defOp);
                if (it == phaseA.opToNode.end())
                {
                    return;
                }

                const auto &ops =
                    collectNodeEffectResultOps(graph, phaseA, it->second, inoutInputValues, memo, stats);
                effectResultOps.insert(ops.begin(), ops.end());
            };

            if (sink.kind == SinkRef::Kind::Operation)
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(sink.op);
                if (opProducesEffectResult(op))
                {
                    effectResultOps.insert(sink.op);
                }
                for (const auto operand : op.operands())
                {
                    collectFromValue(operand);
                }
            }
            else
            {
                collectFromValue(sink.value);
            }
        }

        void collectAscCone(const wolvrix::lib::grh::Graph &graph,
                            const PhaseAData &phaseA,
                            const std::vector<SinkRef> &sinks,
                            const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
                            std::vector<uint32_t> &visitStamp,
                            const uint32_t visitEpoch,
                            AscInfo &asc)
        {
            asc.combOps.clear();

            auto pushFromValue = [&](std::vector<NodeId> &stack, wolvrix::lib::grh::ValueId value) {
                if (isSourceValue(graph, value, inoutInputValues))
                {
                    return;
                }

                const wolvrix::lib::grh::OperationId defOp = graph.valueDef(value);
                if (!defOp.valid())
                {
                    return;
                }
                auto it = phaseA.opToNode.find(defOp);
                if (it == phaseA.opToNode.end())
                {
                    return;
                }
                const NodeId node = it->second;
                if (node >= visitStamp.size() || visitStamp[node] == visitEpoch)
                {
                    return;
                }
                visitStamp[node] = visitEpoch;
                stack.push_back(node);
            };

            if (asc.sinks.size() >= 32)
            {
                const size_t reserveHint = std::min<size_t>(phaseA.nodeToOp.size(), asc.sinks.size() * 8);
                asc.combOps.reserve(reserveHint);
            }

            std::vector<NodeId> stack;
            stack.reserve(asc.sinks.size() * 2 + 4);
            for (const size_t sinkIndex : asc.sinks)
            {
                const SinkRef &sink = sinks[sinkIndex];
                if (sink.kind == SinkRef::Kind::Operation)
                {
                    const wolvrix::lib::grh::Operation op = graph.getOperation(sink.op);
                    for (const auto operand : op.operands())
                    {
                        pushFromValue(stack, operand);
                    }
                }
                else
                {
                    pushFromValue(stack, sink.value);
                }
            }

            while (!stack.empty())
            {
                const NodeId node = stack.back();
                stack.pop_back();

                const wolvrix::lib::grh::Operation op = graph.getOperation(phaseA.nodeToOp[node]);
                if (isSourceOpKind(op.kind()))
                {
                    continue;
                }
                if (!isAscConeTraversalOp(op))
                {
                    continue;
                }

                asc.combOps.push_back(node);
                for (const NodeId pred : phaseA.inNeighbors[node])
                {
                    if (pred >= visitStamp.size() || visitStamp[pred] == visitEpoch)
                    {
                        continue;
                    }
                    visitStamp[pred] = visitEpoch;
                    stack.push_back(pred);
                }
            }
        }

        PhaseBData buildAscs(const wolvrix::lib::grh::Graph &graph,
                             const PhaseAData &phaseA,
                             const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutInputValues,
                             const ProgressLogger &progressLogger)
        {
            const auto phaseStart = std::chrono::steady_clock::now();
            auto msSince = [&](const std::chrono::steady_clock::time_point &start) -> uint64_t {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count());
            };

            PhaseBData data;
            data.sinks = collectSinks(graph);
            data.sinkToAsc.assign(data.sinks.size(), 0);

            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: sinks_collected=" + std::to_string(data.sinks.size()));
            }

            if (data.sinks.empty())
            {
                return data;
            }

            DisjointSet dsu(data.sinks.size());

            std::unordered_map<std::string, std::vector<size_t>> regWriteSinks;
            std::unordered_map<std::string, std::vector<size_t>> latchWriteSinks;
            std::unordered_map<uint32_t, std::vector<size_t>> memWriteSinks;
            MemSymbolIntern memSymbolIntern;

            const auto indexStart = std::chrono::steady_clock::now();
            const size_t sinkCount = data.sinks.size();
            const size_t indexProgressEvery = sinkCount < 50000 ? 10000 : 50000;
            for (size_t i = 0; i < data.sinks.size(); ++i)
            {
                const SinkRef &sink = data.sinks[i];
                if (sink.kind != SinkRef::Kind::Operation)
                {
                    if (progressLogger && i > 0 && (i % indexProgressEvery) == 0)
                    {
                        progressLogger("repcut phase-b/ascs: index_sink_progress=" + std::to_string(i) +
                                       "/" + std::to_string(sinkCount) +
                                       " elapsed_ms=" + std::to_string(msSince(indexStart)));
                    }
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph.getOperation(sink.op);
                if (auto sym = getAttrString(op, "regSymbol"))
                {
                    regWriteSinks[*sym].push_back(i);
                }
                else if (auto sym = getAttrString(op, "latchSymbol"))
                {
                    latchWriteSinks[*sym].push_back(i);
                }
                else if (auto sym = getAttrString(op, "memSymbol"))
                {
                    const uint32_t symbolId = memSymbolIntern.intern(*sym);
                    memWriteSinks[symbolId].push_back(i);
                }

                if (progressLogger && i > 0 && (i % indexProgressEvery) == 0)
                {
                    progressLogger("repcut phase-b/ascs: index_sink_progress=" + std::to_string(i) +
                                   "/" + std::to_string(sinkCount) +
                                   " elapsed_ms=" + std::to_string(msSince(indexStart)));
                }
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: index_sink_done reg_groups=" +
                               std::to_string(regWriteSinks.size()) + " latch_groups=" +
                               std::to_string(latchWriteSinks.size()) + " mem_groups=" +
                               std::to_string(memWriteSinks.size()) +
                               " elapsed_ms=" + std::to_string(msSince(indexStart)));
            }

            auto unionGroup = [&](const auto &groups) {
                for (const auto &[symbol, indices] : groups)
                {
                    (void)symbol;
                    if (indices.size() < 2)
                    {
                        continue;
                    }
                    const size_t root = indices[0];
                    for (size_t i = 1; i < indices.size(); ++i)
                    {
                        dsu.unite(root, indices[i]);
                    }
                }
            };
            const auto unionStart = std::chrono::steady_clock::now();
            unionGroup(regWriteSinks);
            unionGroup(latchWriteSinks);
            unionGroup(memWriteSinks);
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: union_symbol_groups_done elapsed_ms=" +
                               std::to_string(msSince(unionStart)));
            }

            const auto memConeStart = std::chrono::steady_clock::now();
            const size_t memProgressEvery = sinkCount < 20000 ? 5000 : 20000;
            MemSymbolMemo memMemo;
            memMemo.nodeToMemSymbolIds.resize(phaseA.nodeToOp.size());
            memMemo.nodeState.resize(phaseA.nodeToOp.size(), 0);
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: collect_mem_symbols_begin mem_cone_union=enabled");
            }
            size_t batchStartIndex = 0;
            auto batchStart = memConeStart;
            uint64_t batchVisitedNodes = 0;
            uint64_t batchMemSymbolHits = 0;
            uint64_t batchDsuUnions = 0;
            std::unordered_set<uint32_t> batchUniqueMemSymbols;
            for (size_t i = 0; i < data.sinks.size(); ++i)
            {
                MemSymbolCollectStats sinkStats;
                std::unordered_set<uint32_t> sinkMemSymbolIds;
                collectConeMemSymbolIds(graph,
                                        phaseA,
                                        data.sinks[i],
                                        inoutInputValues,
                                        memSymbolIntern,
                                        memMemo,
                                        sinkMemSymbolIds,
                                        sinkStats);
                batchVisitedNodes += sinkStats.visitedNodes;
                batchMemSymbolHits += sinkStats.memSymbolHits;
                for (const uint32_t memSymbolId : sinkMemSymbolIds)
                {
                    batchUniqueMemSymbols.insert(memSymbolId);
                    auto it = memWriteSinks.find(memSymbolId);
                    if (it == memWriteSinks.end())
                    {
                        continue;
                    }
                    for (const size_t writeSinkIndex : it->second)
                    {
                        const size_t sinkRoot = dsu.find(i);
                        const size_t writeRoot = dsu.find(writeSinkIndex);
                        if (sinkRoot != writeRoot)
                        {
                            ++batchDsuUnions;
                            dsu.unite(sinkRoot, writeRoot);
                        }
                    }
                }

                if (progressLogger && ((i + 1) % memProgressEvery) == 0)
                {
                    const size_t processed = i + 1;
                    progressLogger("repcut phase-b/ascs: collect_mem_symbols_progress=" + std::to_string(processed) +
                                   "/" + std::to_string(sinkCount) +
                                   " elapsed_ms=" + std::to_string(msSince(memConeStart)) +
                                   " batch_elapsed_ms=" + std::to_string(msSince(batchStart)) +
                                   " visited_nodes=" + std::to_string(batchVisitedNodes) +
                                   " mem_symbol_hits=" + std::to_string(batchMemSymbolHits) +
                                   " unique_mem_symbols=" + std::to_string(batchUniqueMemSymbols.size()) +
                                   " dsu_unions=" + std::to_string(batchDsuUnions));
                    batchStartIndex = processed;
                    batchStart = std::chrono::steady_clock::now();
                    batchVisitedNodes = 0;
                    batchMemSymbolHits = 0;
                    batchDsuUnions = 0;
                    batchUniqueMemSymbols.clear();
                }
            }
            if (progressLogger && batchStartIndex < sinkCount)
            {
                progressLogger("repcut phase-b/ascs: collect_mem_symbols_progress=" + std::to_string(sinkCount) +
                               "/" + std::to_string(sinkCount) +
                               " elapsed_ms=" + std::to_string(msSince(memConeStart)) +
                               " batch_elapsed_ms=" + std::to_string(msSince(batchStart)) +
                               " visited_nodes=" + std::to_string(batchVisitedNodes) +
                               " mem_symbol_hits=" + std::to_string(batchMemSymbolHits) +
                               " unique_mem_symbols=" + std::to_string(batchUniqueMemSymbols.size()) +
                               " dsu_unions=" + std::to_string(batchDsuUnions));
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: collect_mem_symbols_done elapsed_ms=" +
                               std::to_string(msSince(memConeStart)));
            }

            const auto effectConeStart = std::chrono::steady_clock::now();
            const size_t effectProgressEvery = sinkCount < 20000 ? 5000 : 20000;
            EffectResultMemo effectMemo;
            effectMemo.nodeToEffectResultOps.resize(phaseA.nodeToOp.size());
            effectMemo.nodeState.resize(phaseA.nodeToOp.size(), 0);
            std::unordered_map<wolvrix::lib::grh::OperationId, size_t, wolvrix::lib::grh::OperationIdHash>
                firstSinkByEffectResult;
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: collect_effect_result_begin");
            }
            batchStartIndex = 0;
            batchStart = effectConeStart;
            uint64_t batchEffectVisitedNodes = 0;
            uint64_t batchEffectResultHits = 0;
            uint64_t batchEffectDsuUnions = 0;
            std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>
                batchUniqueEffectResults;
            for (size_t i = 0; i < data.sinks.size(); ++i)
            {
                EffectResultCollectStats sinkStats;
                std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>
                    sinkEffectResults;
                collectConeEffectResultOps(graph,
                                           phaseA,
                                           data.sinks[i],
                                           inoutInputValues,
                                           effectMemo,
                                           sinkEffectResults,
                                           sinkStats);
                batchEffectVisitedNodes += sinkStats.visitedNodes;
                batchEffectResultHits += sinkStats.effectResultHits;
                for (const auto effectResultOp : sinkEffectResults)
                {
                    batchUniqueEffectResults.insert(effectResultOp);
                    const auto [it, inserted] = firstSinkByEffectResult.emplace(effectResultOp, i);
                    if (inserted)
                    {
                        continue;
                    }
                    const size_t sinkRoot = dsu.find(i);
                    const size_t firstRoot = dsu.find(it->second);
                    if (sinkRoot != firstRoot)
                    {
                        ++batchEffectDsuUnions;
                        dsu.unite(sinkRoot, firstRoot);
                    }
                }

                if (progressLogger && ((i + 1) % effectProgressEvery) == 0)
                {
                    const size_t processed = i + 1;
                    progressLogger("repcut phase-b/ascs: collect_effect_result_progress=" +
                                   std::to_string(processed) + "/" + std::to_string(sinkCount) +
                                   " elapsed_ms=" + std::to_string(msSince(effectConeStart)) +
                                   " batch_elapsed_ms=" + std::to_string(msSince(batchStart)) +
                                   " visited_nodes=" + std::to_string(batchEffectVisitedNodes) +
                                   " effect_result_hits=" + std::to_string(batchEffectResultHits) +
                                   " unique_effect_results=" + std::to_string(batchUniqueEffectResults.size()) +
                                   " dsu_unions=" + std::to_string(batchEffectDsuUnions));
                    batchStartIndex = processed;
                    batchStart = std::chrono::steady_clock::now();
                    batchEffectVisitedNodes = 0;
                    batchEffectResultHits = 0;
                    batchEffectDsuUnions = 0;
                    batchUniqueEffectResults.clear();
                }
            }
            if (progressLogger && batchStartIndex < sinkCount)
            {
                progressLogger("repcut phase-b/ascs: collect_effect_result_progress=" +
                               std::to_string(sinkCount) + "/" + std::to_string(sinkCount) +
                               " elapsed_ms=" + std::to_string(msSince(effectConeStart)) +
                               " batch_elapsed_ms=" + std::to_string(msSince(batchStart)) +
                               " visited_nodes=" + std::to_string(batchEffectVisitedNodes) +
                               " effect_result_hits=" + std::to_string(batchEffectResultHits) +
                               " unique_effect_results=" + std::to_string(batchUniqueEffectResults.size()) +
                               " dsu_unions=" + std::to_string(batchEffectDsuUnions));
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: collect_effect_result_done elapsed_ms=" +
                               std::to_string(msSince(effectConeStart)));
            }

            std::unordered_map<size_t, AscId> ascByRoot;
            const auto rootBuildStart = std::chrono::steady_clock::now();
            for (size_t i = 0; i < data.sinks.size(); ++i)
            {
                const size_t root = dsu.find(i);
                auto [it, inserted] = ascByRoot.emplace(root, static_cast<AscId>(data.ascs.size()));
                if (inserted)
                {
                    data.ascs.emplace_back();
                }
                const AscId aid = it->second;
                data.ascs[aid].sinks.push_back(i);
                data.sinkToAsc[i] = aid;
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: build_asc_roots_done ascs=" +
                               std::to_string(data.ascs.size()) +
                               " elapsed_ms=" + std::to_string(msSince(rootBuildStart)));
            }

            const auto collectConeStart = std::chrono::steady_clock::now();
            const size_t ascProgressEvery = data.ascs.size() < 2000 ? 200 : 1000;
            const size_t totalAscs = data.ascs.size();
            const size_t hwThreads =
                std::max<size_t>(1, static_cast<size_t>(std::thread::hardware_concurrency()));
            const size_t threadCount = std::min<size_t>(
                totalAscs,
                std::min<size_t>(kMaxConeCollectThreads, hwThreads));

            if (threadCount <= 1 || totalAscs < kConeCollectChunkSize * 2)
            {
                std::vector<uint32_t> coneVisitStamp(phaseA.nodeToOp.size(), 0);
                uint32_t coneVisitEpoch = 1;
                for (AscId aid = 0; aid < totalAscs; ++aid)
                {
                    collectAscCone(graph,
                                   phaseA,
                                   data.sinks,
                                   inoutInputValues,
                                   coneVisitStamp,
                                   coneVisitEpoch,
                                   data.ascs[aid]);
                    ++coneVisitEpoch;
                    if (coneVisitEpoch == 0)
                    {
                        std::fill(coneVisitStamp.begin(), coneVisitStamp.end(), 0);
                        coneVisitEpoch = 1;
                    }

                    if (progressLogger && aid > 0 && (aid % ascProgressEvery) == 0)
                    {
                        progressLogger("repcut phase-b/ascs: collect_cones_progress=" +
                                       std::to_string(aid) + "/" + std::to_string(totalAscs) +
                                       " elapsed_ms=" + std::to_string(msSince(collectConeStart)));
                    }
                }
            }
            else
            {
                if (progressLogger)
                {
                    progressLogger("repcut phase-b/ascs: collect_cones_parallel threads=" +
                                   std::to_string(threadCount) +
                                   " chunk=" + std::to_string(kConeCollectChunkSize));
                }

                std::atomic<size_t> nextAsc{0};
                std::atomic<size_t> processedAscs{0};
                std::vector<std::thread> workers;
                workers.reserve(threadCount);

                for (size_t t = 0; t < threadCount; ++t)
                {
                    workers.emplace_back([&, t]() {
                        (void)t;
                        std::vector<uint32_t> visitStamp(phaseA.nodeToOp.size(), 0);
                        uint32_t visitEpoch = 1;
                        while (true)
                        {
                            const size_t begin = nextAsc.fetch_add(kConeCollectChunkSize, std::memory_order_relaxed);
                            if (begin >= totalAscs)
                            {
                                break;
                            }
                            const size_t end = std::min(totalAscs, begin + kConeCollectChunkSize);
                            for (size_t aid = begin; aid < end; ++aid)
                            {
                                collectAscCone(graph,
                                               phaseA,
                                               data.sinks,
                                               inoutInputValues,
                                               visitStamp,
                                               visitEpoch,
                                               data.ascs[aid]);
                                ++visitEpoch;
                                if (visitEpoch == 0)
                                {
                                    std::fill(visitStamp.begin(), visitStamp.end(), 0);
                                    visitEpoch = 1;
                                }
                            }
                            processedAscs.fetch_add(end - begin, std::memory_order_relaxed);
                        }
                    });
                }

                if (progressLogger)
                {
                    size_t nextProgress = ascProgressEvery;
                    while (nextProgress < totalAscs)
                    {
                        const size_t done = processedAscs.load(std::memory_order_relaxed);
                        if (done >= nextProgress)
                        {
                            progressLogger("repcut phase-b/ascs: collect_cones_progress=" +
                                           std::to_string(nextProgress) + "/" + std::to_string(totalAscs) +
                                           " elapsed_ms=" + std::to_string(msSince(collectConeStart)));
                            nextProgress += ascProgressEvery;
                            continue;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }

                for (auto &worker : workers)
                {
                    worker.join();
                }
            }

            if (progressLogger)
            {
                progressLogger("repcut phase-b/ascs: collect_cones_done elapsed_ms=" +
                               std::to_string(msSince(collectConeStart)) +
                               " total_elapsed_ms=" + std::to_string(msSince(phaseStart)));
            }

            if (progressLogger && !data.ascs.empty())
            {
                std::vector<AscId> orderedAscs;
                orderedAscs.reserve(data.ascs.size());
                for (AscId aid = 0; aid < data.ascs.size(); ++aid)
                {
                    orderedAscs.push_back(aid);
                }
                std::sort(orderedAscs.begin(),
                          orderedAscs.end(),
                          [&](AscId lhs, AscId rhs) {
                              const std::size_t lhsSize = data.ascs[lhs].combOps.size();
                              const std::size_t rhsSize = data.ascs[rhs].combOps.size();
                              if (lhsSize != rhsSize)
                              {
                                  return lhsSize > rhsSize;
                              }
                              return lhs < rhs;
                          });

                const std::size_t limit = std::min<std::size_t>(5, orderedAscs.size());
                constexpr std::size_t kTopAscSymbolLogLimit = 64;
                for (std::size_t rank = 0; rank < limit; ++rank)
                {
                    const AscId aid = orderedAscs[rank];
                    const AscInfo &asc = data.ascs[aid];
                    std::vector<std::string> opSymbols;
                    opSymbols.reserve(asc.combOps.size());
                    for (const NodeId node : asc.combOps)
                    {
                        if (node >= phaseA.nodeToOp.size())
                        {
                            continue;
                        }
                        const auto opId = phaseA.nodeToOp[node];
                        if (!opId.valid())
                        {
                            continue;
                        }
                        const auto op = graph.getOperation(opId);
                        std::string symbol(op.symbolText());
                        if (symbol.empty())
                        {
                            symbol = formatOperationRef(graph, opId);
                        }
                        opSymbols.push_back(std::move(symbol));
                    }
                    std::sort(opSymbols.begin(), opSymbols.end());

                    std::ostringstream ascLog;
                    ascLog << "repcut phase-b/ascs: top_asc rank=" << (rank + 1)
                           << " aid=" << aid
                           << " comb_ops=" << asc.combOps.size()
                           << " sinks=" << asc.sinks.size()
                           << " op_symbols=[";
                    const std::size_t emittedCount = std::min<std::size_t>(kTopAscSymbolLogLimit, opSymbols.size());
                    for (std::size_t i = 0; i < emittedCount; ++i)
                    {
                        if (i > 0)
                        {
                            ascLog << ", ";
                        }
                        ascLog << opSymbols[i];
                    }
                    if (opSymbols.size() > emittedCount)
                    {
                        if (emittedCount > 0)
                        {
                            ascLog << ", ";
                        }
                        ascLog << "... +" << (opSymbols.size() - emittedCount) << " more";
                    }
                    ascLog << "]";
                    progressLogger(ascLog.str());
                }
            }

            return data;
        }

        void normalizeNodeToAscs(std::vector<std::vector<AscId>> &nodeToAscs)
        {
            for (auto &ascs : nodeToAscs)
            {
                std::sort(ascs.begin(), ascs.end());
                ascs.erase(std::unique(ascs.begin(), ascs.end()), ascs.end());
            }
        }

        NodeId chooseAscSeedNode(const AscInfo &asc,
                                 const std::vector<SinkRef> &sinks,
                                 const PhaseAData &phaseA)
        {
            for (const size_t sinkIndex : asc.sinks)
            {
                const SinkRef &sink = sinks[sinkIndex];
                if (sink.kind != SinkRef::Kind::Operation)
                {
                    continue;
                }
                auto it = phaseA.opToNode.find(sink.op);
                if (it != phaseA.opToNode.end())
                {
                    return it->second;
                }
            }
            if (!asc.combOps.empty())
            {
                return *asc.combOps.begin();
            }
            return kInvalidNode;
        }

        void findPiece(NodeId seed,
                       PieceId pid,
                       const PhaseAData &phaseA,
                       const std::vector<std::vector<AscId>> &nodeToAscs,
                       std::vector<std::unordered_set<NodeId>> &pieces,
                       std::vector<PieceId> &nodeToPiece,
                       std::vector<std::vector<AscId>> &pieceToAscs)
        {
            if (seed == kInvalidNode || seed >= nodeToPiece.size() || nodeToPiece[seed] != kInvalidPiece)
            {
                return;
            }

            const std::vector<AscId> targetAscs = nodeToAscs[seed];
            std::vector<NodeId> stack;
            stack.push_back(seed);

            while (!stack.empty())
            {
                const NodeId node = stack.back();
                stack.pop_back();

                if (nodeToPiece[node] != kInvalidPiece)
                {
                    continue;
                }
                if (nodeToAscs[node] != targetAscs)
                {
                    continue;
                }

                nodeToPiece[node] = pid;
                pieces[pid].insert(node);

                for (const NodeId pred : phaseA.inNeighbors[node])
                {
                    if (nodeToPiece[pred] == kInvalidPiece && nodeToAscs[pred] == targetAscs)
                    {
                        stack.push_back(pred);
                    }
                }
                for (const NodeId succ : phaseA.outNeighbors[node])
                {
                    if (nodeToPiece[succ] == kInvalidPiece && nodeToAscs[succ] == targetAscs)
                    {
                        stack.push_back(succ);
                    }
                }
            }

            pieceToAscs[pid] = targetAscs;
        }

        void buildPieces(PhaseBData &phaseB, const PhaseAData &phaseA, const ProgressLogger &progressLogger)
        {
            const auto phaseStart = std::chrono::steady_clock::now();
            auto msSince = [&](const std::chrono::steady_clock::time_point &start) -> uint64_t {
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count());
            };

            phaseB.nodeToAscs.clear();
            phaseB.nodeToAscs.resize(phaseA.nodeToOp.size());

            const auto nodeToAscsStart = std::chrono::steady_clock::now();
            const size_t ascProgressEvery = phaseB.ascs.size() < 2000 ? 200 : 1000;
            for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
            {
                const AscInfo &asc = phaseB.ascs[aid];

                for (const size_t sinkIndex : asc.sinks)
                {
                    const SinkRef &sink = phaseB.sinks[sinkIndex];
                    if (sink.kind != SinkRef::Kind::Operation)
                    {
                        continue;
                    }
                    auto it = phaseA.opToNode.find(sink.op);
                    if (it != phaseA.opToNode.end())
                    {
                        phaseB.nodeToAscs[it->second].push_back(aid);
                    }
                }
                for (const NodeId node : asc.combOps)
                {
                    phaseB.nodeToAscs[node].push_back(aid);
                }

                if (progressLogger && aid > 0 && (aid % ascProgressEvery) == 0)
                {
                    progressLogger("repcut phase-b/pieces: build_node_to_ascs_progress=" +
                                   std::to_string(aid) + "/" + std::to_string(phaseB.ascs.size()) +
                                   " elapsed_ms=" + std::to_string(msSince(nodeToAscsStart)));
                }
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/pieces: build_node_to_ascs_done elapsed_ms=" +
                               std::to_string(msSince(nodeToAscsStart)));
            }

            const auto normalizeStart = std::chrono::steady_clock::now();
            normalizeNodeToAscs(phaseB.nodeToAscs);
            if (progressLogger)
            {
                progressLogger("repcut phase-b/pieces: normalize_node_to_ascs_done elapsed_ms=" +
                               std::to_string(msSince(normalizeStart)));
            }

            phaseB.pieces.clear();
            phaseB.pieceToAscs.clear();
            phaseB.nodeToPiece.assign(phaseA.nodeToOp.size(), kInvalidPiece);

            const auto ascPieceStart = std::chrono::steady_clock::now();
            for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
            {
                const PieceId pid = static_cast<PieceId>(phaseB.pieces.size());
                phaseB.pieces.emplace_back();
                phaseB.pieceToAscs.emplace_back(std::vector<AscId>{aid});

                const NodeId seed = chooseAscSeedNode(phaseB.ascs[aid], phaseB.sinks, phaseA);
                findPiece(seed, pid, phaseA, phaseB.nodeToAscs, phaseB.pieces, phaseB.nodeToPiece, phaseB.pieceToAscs);

                if (progressLogger && aid > 0 && (aid % ascProgressEvery) == 0)
                {
                    progressLogger("repcut phase-b/pieces: asc_piece_progress=" +
                                   std::to_string(aid) + "/" + std::to_string(phaseB.ascs.size()) +
                                   " elapsed_ms=" + std::to_string(msSince(ascPieceStart)));
                }
            }
            if (progressLogger)
            {
                progressLogger("repcut phase-b/pieces: asc_piece_done elapsed_ms=" +
                               std::to_string(msSince(ascPieceStart)) +
                               " pieces_now=" + std::to_string(phaseB.pieces.size()));
            }

            const auto residualStart = std::chrono::steady_clock::now();
            const size_t nodeCount = phaseB.nodeToPiece.size();
            const size_t nodeProgressEvery = nodeCount < 200000 ? 50000 : 200000;
            for (NodeId node = 0; node < phaseB.nodeToPiece.size(); ++node)
            {
                if (phaseB.nodeToPiece[node] != kInvalidPiece)
                {
                    if (progressLogger && node > 0 && (node % nodeProgressEvery) == 0)
                    {
                        progressLogger("repcut phase-b/pieces: residual_scan_progress=" +
                                       std::to_string(node) + "/" + std::to_string(nodeCount) +
                                       " elapsed_ms=" + std::to_string(msSince(residualStart)));
                    }
                    continue;
                }
                const PieceId pid = static_cast<PieceId>(phaseB.pieces.size());
                phaseB.pieces.emplace_back();
                phaseB.pieceToAscs.emplace_back();
                findPiece(node, pid, phaseA, phaseB.nodeToAscs, phaseB.pieces, phaseB.nodeToPiece, phaseB.pieceToAscs);

                if (progressLogger && node > 0 && (node % nodeProgressEvery) == 0)
                {
                    progressLogger("repcut phase-b/pieces: residual_scan_progress=" +
                                   std::to_string(node) + "/" + std::to_string(nodeCount) +
                                   " elapsed_ms=" + std::to_string(msSince(residualStart)));
                }
            }

            if (progressLogger)
            {
                progressLogger("repcut phase-b/pieces: residual_scan_done elapsed_ms=" +
                               std::to_string(msSince(residualStart)) +
                               " total_elapsed_ms=" + std::to_string(msSince(phaseStart)) +
                               " pieces_total=" + std::to_string(phaseB.pieces.size()));
            }
        }

        bool validatePhaseB(const PhaseAData &phaseA,
                            const PhaseBData &phaseB,
                            std::string &errorMessage)
        {
            for (size_t sinkIndex = 0; sinkIndex < phaseB.sinks.size(); ++sinkIndex)
            {
                const AscId aid = phaseB.sinkToAsc[sinkIndex];
                if (aid >= phaseB.ascs.size())
                {
                    errorMessage = "repcut phase-b: sink->asc mapping out of range";
                    return false;
                }
            }

            for (NodeId node = 0; node < phaseA.nodeToOp.size(); ++node)
            {
                if (phaseB.nodeToPiece[node] == kInvalidPiece)
                {
                    errorMessage = "repcut phase-b: node without piece assignment";
                    return false;
                }
                if (phaseB.nodeToPiece[node] >= phaseB.pieces.size())
                {
                    errorMessage = "repcut phase-b: node->piece mapping out of range";
                    return false;
                }
            }

            for (PieceId pid = 0; pid < phaseB.pieces.size(); ++pid)
            {
                const auto &piece = phaseB.pieces[pid];
                const auto &pieceAscs = phaseB.pieceToAscs[pid];
                for (const NodeId node : piece)
                {
                    if (node >= phaseB.nodeToAscs.size())
                    {
                        errorMessage = "repcut phase-b: piece contains invalid node";
                        return false;
                    }
                    if (phaseB.nodeToAscs[node] != pieceAscs)
                    {
                        errorMessage = "repcut phase-b: pieceToAscs mismatch with nodeToAscs";
                        return false;
                    }
                }
            }

            return true;
        }

        bool validateHyperNodeAndHyperSinkTypes(
            const wolvrix::lib::grh::Graph &graph,
            const PhaseAData &phaseA,
            const PhaseBData &phaseB,
            const std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> &inoutOutputValues,
            std::string &errorMessage)
        {
            constexpr std::size_t kGuardDiagLimit = 24;
            std::size_t issueCount = 0;
            std::vector<std::string> samples;
            samples.reserve(kGuardDiagLimit);

            auto addIssue = [&](std::string msg) {
                ++issueCount;
                if (samples.size() < kGuardDiagLimit)
                {
                    samples.push_back(std::move(msg));
                }
            };

            for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
            {
                const AscInfo &asc = phaseB.ascs[aid];
                if (asc.sinks.empty())
                {
                    addIssue("hypernode without sinks: asc=" + std::to_string(aid));
                }

                for (std::size_t sinkPos = 0; sinkPos < asc.sinks.size(); ++sinkPos)
                {
                    const std::size_t sinkIndex = asc.sinks[sinkPos];
                    if (sinkIndex >= phaseB.sinks.size())
                    {
                        addIssue("hypersink index out of range: asc=" + std::to_string(aid) +
                                 " sink_pos=" + std::to_string(sinkPos) +
                                 " sink_index=" + std::to_string(sinkIndex) +
                                 " sinks_size=" + std::to_string(phaseB.sinks.size()));
                        continue;
                    }
                    const SinkRef &sink = phaseB.sinks[sinkIndex];
                    if (sink.kind == SinkRef::Kind::Operation)
                    {
                        if (!sink.op.valid())
                        {
                            addIssue("hypersink operation id invalid: asc=" + std::to_string(aid) +
                                     " sink_pos=" + std::to_string(sinkPos) +
                                     " sink_index=" + std::to_string(sinkIndex));
                            continue;
                        }
                        if (phaseA.opToNode.find(sink.op) == phaseA.opToNode.end())
                        {
                            addIssue("hypersink operation missing node index: asc=" + std::to_string(aid) +
                                     " sink_pos=" + std::to_string(sinkPos) +
                                     " sink_index=" + std::to_string(sinkIndex) +
                                     " op=" + formatOperationRef(graph, sink.op));
                            continue;
                        }
                        const wolvrix::lib::grh::Operation op = graph.getOperation(sink.op);
                        if (!isSinkOp(op))
                        {
                            addIssue("hypersink operation kind invalid: asc=" + std::to_string(aid) +
                                     " sink_pos=" + std::to_string(sinkPos) +
                                     " sink_index=" + std::to_string(sinkIndex) +
                                     " op=" + formatOperationRef(graph, sink.op) +
                                     " expected_sink_kind={kRegisterWritePort|kLatchWritePort|kMemoryWritePort|effect-op}");
                        }
                    }
                    else
                    {
                        if (!sink.value.valid())
                        {
                            addIssue("hypersink output value invalid: asc=" + std::to_string(aid) +
                                     " sink_pos=" + std::to_string(sinkPos) +
                                     " sink_index=" + std::to_string(sinkIndex));
                            continue;
                        }
                        const bool isOutput = graph.valueIsOutput(sink.value);
                        const bool isInoutOut = inoutOutputValues.find(sink.value) != inoutOutputValues.end();
                        if (!isOutput && !isInoutOut)
                        {
                            const wolvrix::lib::grh::Value value = graph.getValue(sink.value);
                            addIssue("hypersink output value kind invalid: asc=" + std::to_string(aid) +
                                     " sink_pos=" + std::to_string(sinkPos) +
                                     " sink_index=" + std::to_string(sinkIndex) +
                                     " value=" + formatValueId(sink.value) +
                                     " symbol=" + std::string(value.symbolText()) +
                                     " is_output=" + std::string(isOutput ? "true" : "false") +
                                     " is_inout_output=" + std::string(isInoutOut ? "true" : "false"));
                        }
                    }
                }

                for (const NodeId node : asc.combOps)
                {
                    if (node >= phaseA.nodeToOp.size())
                    {
                        addIssue("hypernode comb node out of range: asc=" + std::to_string(aid) +
                                 " node=" + std::to_string(node) +
                                 " node_count=" + std::to_string(phaseA.nodeToOp.size()));
                        continue;
                    }
                    const wolvrix::lib::grh::OperationId opId = phaseA.nodeToOp[node];
                    if (!opId.valid())
                    {
                        addIssue("hypernode comb node has invalid op id: asc=" + std::to_string(aid) +
                                 " node=" + std::to_string(node));
                        continue;
                    }
                    const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                    if (!isAscConeTraversalOp(op))
                    {
                        addIssue("hypernode comb node kind invalid: asc=" + std::to_string(aid) +
                                 " node=" + std::to_string(node) +
                                 " op=" + formatOperationRef(graph, opId) +
                                 " expected=isAscConeTraversalOp(op)==true");
                    }
                }
            }
            if (issueCount > 0)
            {
                std::ostringstream oss;
                oss << "repcut phase-b guard: invalid hypernode/hypersink node types"
                    << " total_issues=" << issueCount
                    << " sample_count=" << samples.size();
                for (std::size_t i = 0; i < samples.size(); ++i)
                {
                    oss << " | sample[" << i << "] " << samples[i];
                }
                errorMessage = oss.str();
                return false;
            }
            return true;
        }

        int32_t safeValueWidth(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId value)
        {
            if (!value.valid())
            {
                return 1;
            }
            const int32_t width = graph.valueWidth(value);
            return width > 0 ? width : 1;
        }

        int32_t getResultWidth(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op)
        {
            if (op.results().empty())
            {
                return 1;
            }
            int32_t maxWidth = 1;
            for (const auto value : op.results())
            {
                maxWidth = std::max(maxWidth, safeValueWidth(graph, value));
            }
            return maxWidth;
        }

        int32_t getMaxOperandWidth(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op)
        {
            int32_t maxWidth = 1;
            for (const auto operand : op.operands())
            {
                maxWidth = std::max(maxWidth, safeValueWidth(graph, operand));
            }
            return maxWidth;
        }

        int32_t getOperandWidth(const wolvrix::lib::grh::Graph &graph,
                                const wolvrix::lib::grh::Operation &op,
                                size_t operandIndex)
        {
            if (operandIndex >= op.operands().size())
            {
                return 1;
            }
            return safeValueWidth(graph, op.operands()[operandIndex]);
        }

        uint32_t calculateNodeWeight(const wolvrix::lib::grh::Graph &graph,
                                     const PhaseAData &phaseA,
                                     NodeId node,
                                     std::vector<uint32_t> &nodeWeights)
        {
            if (node >= phaseA.nodeToOp.size())
            {
                return 1;
            }
            if (nodeWeights[node] != std::numeric_limits<uint32_t>::max())
            {
                return nodeWeights[node];
            }

            const wolvrix::lib::grh::Operation op = graph.getOperation(phaseA.nodeToOp[node]);
            uint32_t weight = 1;
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
                weight = 1;
                break;
            case wolvrix::lib::grh::OperationKind::kAdd:
            case wolvrix::lib::grh::OperationKind::kSub:
                weight = 2;
                break;
            case wolvrix::lib::grh::OperationKind::kMul:
                weight = 4;
                break;
            case wolvrix::lib::grh::OperationKind::kDiv:
            case wolvrix::lib::grh::OperationKind::kMod:
                weight = 6;
                break;
            case wolvrix::lib::grh::OperationKind::kEq:
            case wolvrix::lib::grh::OperationKind::kNe:
            case wolvrix::lib::grh::OperationKind::kCaseEq:
            case wolvrix::lib::grh::OperationKind::kCaseNe:
            case wolvrix::lib::grh::OperationKind::kWildcardEq:
            case wolvrix::lib::grh::OperationKind::kWildcardNe:
            case wolvrix::lib::grh::OperationKind::kLt:
            case wolvrix::lib::grh::OperationKind::kLe:
            case wolvrix::lib::grh::OperationKind::kGt:
            case wolvrix::lib::grh::OperationKind::kGe:
                weight = 2;
                break;
            case wolvrix::lib::grh::OperationKind::kShl:
            case wolvrix::lib::grh::OperationKind::kLShr:
            case wolvrix::lib::grh::OperationKind::kAShr:
                weight = 2;
                break;
            case wolvrix::lib::grh::OperationKind::kAnd:
            case wolvrix::lib::grh::OperationKind::kOr:
            case wolvrix::lib::grh::OperationKind::kXor:
            case wolvrix::lib::grh::OperationKind::kXnor:
            case wolvrix::lib::grh::OperationKind::kNot:
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
            case wolvrix::lib::grh::OperationKind::kLogicOr:
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            case wolvrix::lib::grh::OperationKind::kReduceAnd:
            case wolvrix::lib::grh::OperationKind::kReduceOr:
            case wolvrix::lib::grh::OperationKind::kReduceXor:
            case wolvrix::lib::grh::OperationKind::kReduceNor:
            case wolvrix::lib::grh::OperationKind::kReduceNand:
            case wolvrix::lib::grh::OperationKind::kReduceXnor:
                weight = (static_cast<uint32_t>(getResultWidth(graph, op)) + 63u) / 64u;
                weight = std::max(weight, 1u);
                break;
            case wolvrix::lib::grh::OperationKind::kMux:
            {
                const uint32_t nWords = (static_cast<uint32_t>(getResultWidth(graph, op)) + 63u) / 64u;
                weight = std::max(1u, nWords * 6u);
                break;
            }
            case wolvrix::lib::grh::OperationKind::kConcat:
                weight = std::max<uint32_t>(1u, static_cast<uint32_t>(op.operands().size()));
                break;
            case wolvrix::lib::grh::OperationKind::kReplicate:
                weight = 2;
                break;
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            case wolvrix::lib::grh::OperationKind::kSliceArray:
                weight = 1;
                break;
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                weight = 4;
                break;
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            {
                const uint32_t nWords = (static_cast<uint32_t>(getOperandWidth(graph, op, 1)) + 63u) / 64u;
                weight = std::max(2u, nWords + 1u);
                break;
            }
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
            {
                const uint32_t wrEnWords = (static_cast<uint32_t>(getOperandWidth(graph, op, 0)) + 63u) / 64u;
                const uint32_t wrDataWords = (static_cast<uint32_t>(getOperandWidth(graph, op, 1)) + 63u) / 64u;
                weight = 1u + std::max(1u, wrEnWords) + std::max(1u, wrDataWords);
                break;
            }
            default:
            {
                if (isCombOp(op))
                {
                    const uint32_t widthScaled = (static_cast<uint32_t>(getMaxOperandWidth(graph, op)) + 63u) / 64u;
                    weight = std::max(1u, widthScaled);
                }
                else
                {
                    weight = 1;
                }
                break;
            }
            }

            auto getResultFanout = [&](const wolvrix::lib::grh::Operation &curOp) -> std::size_t {
                std::size_t totalFanout = 0;
                for (const auto resultValue : curOp.results())
                {
                    if (!resultValue.valid())
                    {
                        continue;
                    }
                    totalFanout += graph.getValue(resultValue).users().size();
                }
                return totalFanout;
            };

            auto isWideLogicSensitiveKind = [](wolvrix::lib::grh::OperationKind kind) noexcept {
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kEq:
                case wolvrix::lib::grh::OperationKind::kNe:
                case wolvrix::lib::grh::OperationKind::kCaseEq:
                case wolvrix::lib::grh::OperationKind::kCaseNe:
                case wolvrix::lib::grh::OperationKind::kWildcardEq:
                case wolvrix::lib::grh::OperationKind::kWildcardNe:
                case wolvrix::lib::grh::OperationKind::kLt:
                case wolvrix::lib::grh::OperationKind::kLe:
                case wolvrix::lib::grh::OperationKind::kGt:
                case wolvrix::lib::grh::OperationKind::kGe:
                case wolvrix::lib::grh::OperationKind::kAnd:
                case wolvrix::lib::grh::OperationKind::kOr:
                case wolvrix::lib::grh::OperationKind::kXor:
                case wolvrix::lib::grh::OperationKind::kXnor:
                case wolvrix::lib::grh::OperationKind::kNot:
                case wolvrix::lib::grh::OperationKind::kLogicAnd:
                case wolvrix::lib::grh::OperationKind::kLogicOr:
                case wolvrix::lib::grh::OperationKind::kLogicNot:
                case wolvrix::lib::grh::OperationKind::kReduceAnd:
                case wolvrix::lib::grh::OperationKind::kReduceOr:
                case wolvrix::lib::grh::OperationKind::kReduceXor:
                case wolvrix::lib::grh::OperationKind::kReduceNor:
                case wolvrix::lib::grh::OperationKind::kReduceNand:
                case wolvrix::lib::grh::OperationKind::kReduceXnor:
                case wolvrix::lib::grh::OperationKind::kMux:
                    return true;
                default:
                    return false;
                }
            };

            const int32_t clampedResultWidth = std::max(1, getResultWidth(graph, op));
            const std::size_t resultWords = static_cast<std::size_t>((clampedResultWidth + 63) / 64);
            double fanoutScale = 1.0 + 0.12 * std::log2(1.0 + static_cast<double>(getResultFanout(op)));
            double widthScale = 1.0;
            if (resultWords == 2)
            {
                widthScale = 1.15;
            }
            else if (resultWords >= 3 && resultWords <= 4)
            {
                widthScale = 1.30;
            }
            else if (resultWords > 4)
            {
                widthScale = 1.50;
            }
            const double specialScale = isWideLogicSensitiveKind(op.kind()) ? widthScale : 1.0;
            const double scaledWeight =
                static_cast<double>(std::max(1u, weight)) * std::max(1.0, fanoutScale) * std::max(1.0, specialScale);

            const std::size_t roundedWeight =
                (scaledWeight <= 1.0) ? 1u : static_cast<std::size_t>(std::llround(scaledWeight));
            nodeWeights[node] = roundedWeight > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())
                                    ? std::numeric_limits<uint32_t>::max()
                                    : static_cast<uint32_t>(roundedWeight);
            return nodeWeights[node];
        }

        uint32_t calculatePieceWeight(const wolvrix::lib::grh::Graph &graph,
                                      const PhaseAData &phaseA,
                                      const PhaseBData &phaseB,
                                      PieceId pid,
                                      std::vector<uint32_t> &nodeWeights,
                                      std::vector<uint32_t> &pieceWeights)
        {
            if (pid >= phaseB.pieces.size())
            {
                return 1;
            }
            if (pieceWeights[pid] != std::numeric_limits<uint32_t>::max())
            {
                return pieceWeights[pid];
            }

            const auto &piece = phaseB.pieces[pid];
            if (piece.empty())
            {
                pieceWeights[pid] = 1;
                return pieceWeights[pid];
            }

            std::vector<NodeId> pieceSinks;
            pieceSinks.reserve(piece.size());
            for (const NodeId node : piece)
            {
                bool hasOutEdgeInPiece = false;
                for (const NodeId succ : phaseA.outNeighbors[node])
                {
                    if (piece.find(succ) != piece.end())
                    {
                        hasOutEdgeInPiece = true;
                        break;
                    }
                }
                if (!hasOutEdgeInPiece)
                {
                    pieceSinks.push_back(node);
                }
            }
            if (pieceSinks.empty())
            {
                for (const NodeId node : piece)
                {
                    pieceSinks.push_back(node);
                }
            }

            std::unordered_set<NodeId> visited;
            std::function<uint32_t(NodeId)> stmtWeight = [&](NodeId node) -> uint32_t {
                if (!visited.insert(node).second)
                {
                    return 0;
                }
                uint32_t w = calculateNodeWeight(graph, phaseA, node, nodeWeights);
                for (const NodeId pred : phaseA.inNeighbors[node])
                {
                    if (piece.find(pred) != piece.end())
                    {
                        w += stmtWeight(pred);
                    }
                }
                return w;
            };

            uint32_t totalWeight = 0;
            for (const NodeId sink : pieceSinks)
            {
                totalWeight += stmtWeight(sink);
            }

            pieceWeights[pid] = std::max(1u, totalWeight);
            return pieceWeights[pid];
        }

        uint32_t clampToUint32(std::size_t value)
        {
            return value > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())
                       ? std::numeric_limits<uint32_t>::max()
                       : static_cast<uint32_t>(value);
        }

        std::size_t compressWeightMetric(std::size_t value,
                                         std::size_t linearLimit,
                                         double tailDivisor,
                                         double tailScale)
        {
            if (value <= linearLimit)
            {
                return value;
            }
            const double safeTailDivisor = std::max(1.0, tailDivisor);
            const double tail = static_cast<double>(value - linearLimit) / safeTailDivisor;
            const double compressed =
                static_cast<double>(linearLimit) + std::max(0.0, tailScale) * std::log2(1.0 + tail);
            return std::max<std::size_t>(
                linearLimit,
                static_cast<std::size_t>(std::llround(std::max<double>(static_cast<double>(linearLimit), compressed))));
        }

        struct PieceCommStats
        {
            std::size_t outSignalCount = 0;
            std::size_t outWords = 0;
            std::size_t wide64Words = 0;
            std::size_t wide256Words = 0;
            std::size_t fanoutExcess = 0;
            std::size_t stateBoundaryWords = 0;
        };

        bool isStateBoundaryDefKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                return true;
            default:
                return false;
            }
        }

        bool isStateBoundaryUserKind(wolvrix::lib::grh::OperationKind kind) noexcept
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                return true;
            default:
                return false;
            }
        }

        std::size_t percentileThreshold(std::vector<std::size_t> values, double quantile)
        {
            if (values.empty())
            {
                return 0;
            }
            std::sort(values.begin(), values.end());
            const double clampedQuantile = std::clamp(quantile, 0.0, 1.0);
            if (clampedQuantile <= 0.0)
            {
                return values.front();
            }
            const std::size_t index = std::min<std::size_t>(
                values.size() - 1,
                static_cast<std::size_t>(std::ceil(clampedQuantile * static_cast<double>(values.size()))) - 1u);
            return values[index];
        }

        PieceCommStats calculatePieceCommStats(const wolvrix::lib::grh::Graph &graph,
                                               const PhaseAData &phaseA,
                                               const PhaseBData &phaseB,
                                               PieceId pid)
        {
            PieceCommStats stats;
            if (pid >= phaseB.pieces.size())
            {
                return stats;
            }

            const auto &piece = phaseB.pieces[pid];
            if (piece.empty())
            {
                return stats;
            }

            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> pieceResults;
            for (const NodeId node : piece)
            {
                if (node >= phaseA.nodeToOp.size())
                {
                    continue;
                }
                const auto op = graph.getOperation(phaseA.nodeToOp[node]);
                for (const auto resultValue : op.results())
                {
                    if (resultValue.valid())
                    {
                        pieceResults.insert(resultValue);
                    }
                }
            }

            for (const auto valueId : pieceResults)
            {
                const auto value = graph.getValue(valueId);
                std::unordered_set<AscId> userAscs;
                bool touchesStateBoundary = false;
                const auto defOpId = graph.valueDef(valueId);
                if (defOpId.valid())
                {
                    const auto defOp = graph.getOperation(defOpId);
                    touchesStateBoundary = isStateBoundaryDefKind(defOp.kind());
                }
                for (const auto &user : value.users())
                {
                    if (!user.operation.valid())
                    {
                        continue;
                    }
                    const auto userOp = graph.getOperation(user.operation);
                    touchesStateBoundary = touchesStateBoundary || isStateBoundaryUserKind(userOp.kind());
                    auto itNode = phaseA.opToNode.find(user.operation);
                    if (itNode == phaseA.opToNode.end())
                    {
                        continue;
                    }
                    const NodeId userNode = itNode->second;
                    if (userNode >= phaseB.nodeToPiece.size())
                    {
                        continue;
                    }
                    const PieceId userPid = phaseB.nodeToPiece[userNode];
                    if (userPid == kInvalidPiece || userPid >= phaseB.pieceToAscs.size())
                    {
                        continue;
                    }
                    for (const AscId aid : phaseB.pieceToAscs[userPid])
                    {
                        userAscs.insert(aid);
                    }
                }

                if (userAscs.size() < 2)
                {
                    continue;
                }

                const int32_t width = std::max(1, safeValueWidth(graph, valueId));
                const std::size_t words = static_cast<std::size_t>((width + 63) / 64);
                const std::size_t fanoutFactor = std::max<std::size_t>(1, userAscs.size() - 1);
                stats.outSignalCount += 1;
                stats.outWords += words;
                stats.fanoutExcess += fanoutFactor;
                if (width > 64)
                {
                    stats.wide64Words += words;
                }
                if (width > 256)
                {
                    stats.wide256Words += words;
                }
                if (touchesStateBoundary)
                {
                    stats.stateBoundaryWords += words;
                }
            }

            return stats;
        }

        std::size_t widthToWordCount(int32_t width)
        {
            const int32_t clampedWidth = std::max(1, width);
            return static_cast<std::size_t>((clampedWidth + 63) / 64);
        }

        std::size_t widthBucketIndex(int32_t width)
        {
            if (width <= 1)
            {
                return 0;
            }
            if (width <= 8)
            {
                return 1;
            }
            if (width <= 16)
            {
                return 2;
            }
            if (width <= 32)
            {
                return 3;
            }
            if (width <= 64)
            {
                return 4;
            }
            if (width <= 128)
            {
                return 5;
            }
            if (width <= 256)
            {
                return 6;
            }
            return 7;
        }

        const char *widthBucketName(std::size_t index)
        {
            static constexpr std::array<const char *, 8> kNames = {
                "w1",
                "w2_8",
                "w9_16",
                "w17_32",
                "w33_64",
                "w65_128",
                "w129_256",
                "w257_plus",
            };
            return index < kNames.size() ? kNames[index] : "unknown";
        }

        struct PartitionStaticFeatureRecord
        {
            std::size_t opCount = 0;
            std::size_t combOpCount = 0;
            std::size_t sinkOpCount = 0;
            std::size_t sourceOpCount = 0;
            std::size_t phaseANodeCount = 0;
            std::size_t nonPhaseAOpCount = 0;
            std::size_t operandCount = 0;
            std::size_t resultCount = 0;
            std::size_t operandWordCount = 0;
            std::size_t resultWordCount = 0;
            std::size_t fanoutCount = 0;
            std::size_t estimatedNodeWeightSum = 0;
            std::size_t hyperNodeWeight = 0;
            std::size_t crossInValueCount = 0;
            std::size_t crossOutValueCount = 0;
            std::size_t crossInWordCount = 0;
            std::size_t crossOutWordCount = 0;
            int32_t maxOpWidth = 1;
            std::unordered_map<std::string, std::size_t> opKindCounts;
            std::array<std::size_t, 8> widthBucketCounts{};
        };

        bool belongsToGraph(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationId opId) noexcept
        {
            return opId.valid() && opId.graph == graph.id();
        }

        bool belongsToGraph(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId valueId) noexcept
        {
            return valueId.valid() && valueId.graph == graph.id();
        }

        std::vector<PartitionStaticFeatureRecord> buildPartitionStaticFeatureRecords(
            const wolvrix::lib::grh::Graph &graph,
            const PhaseAData &phaseA,
            const std::vector<std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>> &partitionOps,
            const std::vector<CrossPartitionValue> &crossValues,
            const std::vector<std::size_t> &partitionWeights)
        {
            std::vector<PartitionStaticFeatureRecord> records(partitionOps.size());
            std::vector<uint32_t> nodeWeights(phaseA.nodeToOp.size(), std::numeric_limits<uint32_t>::max());

            for (std::size_t partId = 0; partId < partitionOps.size(); ++partId)
            {
                auto &record = records[partId];
                if (partId < partitionWeights.size())
                {
                    record.hyperNodeWeight = partitionWeights[partId];
                }
                for (const auto opId : partitionOps[partId])
                {
                    if (!belongsToGraph(graph, opId))
                    {
                        continue;
                    }
                    const auto op = graph.getOperation(opId);
                    ++record.opCount;
                    record.operandCount += op.operands().size();
                    record.resultCount += op.results().size();
                    record.opKindCounts[std::string(wolvrix::lib::grh::toString(op.kind()))] += 1;
                    if (isCombOp(op))
                    {
                        ++record.combOpCount;
                    }
                    if (isSinkOp(op))
                    {
                        ++record.sinkOpCount;
                    }
                    if (isSourceOpKind(op.kind()))
                    {
                        ++record.sourceOpCount;
                    }

                    int32_t opWidth = 1;
                    opWidth = std::max(opWidth, getResultWidth(graph, op));
                    opWidth = std::max(opWidth, getMaxOperandWidth(graph, op));
                    record.maxOpWidth = std::max(record.maxOpWidth, opWidth);
                    record.widthBucketCounts[widthBucketIndex(opWidth)] += 1;

                    for (const auto operand : op.operands())
                    {
                        if (!belongsToGraph(graph, operand))
                        {
                            continue;
                        }
                        record.operandWordCount += widthToWordCount(safeValueWidth(graph, operand));
                    }
                    for (const auto resultValue : op.results())
                    {
                        if (!belongsToGraph(graph, resultValue))
                        {
                            continue;
                        }
                        record.resultWordCount += widthToWordCount(safeValueWidth(graph, resultValue));
                        record.fanoutCount += graph.getValue(resultValue).users().size();
                    }

                    const auto nodeIt = phaseA.opToNode.find(opId);
                    if (nodeIt != phaseA.opToNode.end())
                    {
                        ++record.phaseANodeCount;
                        record.estimatedNodeWeightSum +=
                            calculateNodeWeight(graph, phaseA, nodeIt->second, nodeWeights);
                    }
                    else
                    {
                        ++record.nonPhaseAOpCount;
                    }
                }
            }

            for (const auto &cv : crossValues)
            {
                if (!cv.allowed || !cv.requiresPort)
                {
                    continue;
                }
                if (!belongsToGraph(graph, cv.value))
                {
                    continue;
                }
                const std::size_t wordCount = widthToWordCount(safeValueWidth(graph, cv.value));
                if (cv.srcPart < records.size())
                {
                    records[cv.srcPart].crossOutValueCount += 1;
                    records[cv.srcPart].crossOutWordCount += wordCount;
                }
                if (cv.dstPart < records.size())
                {
                    records[cv.dstPart].crossInValueCount += 1;
                    records[cv.dstPart].crossInWordCount += wordCount;
                }
            }

            return records;
        }

        HyperGraph buildHyperGraph(const wolvrix::lib::grh::Graph &graph,
                                   const PhaseAData &phaseA,
                                   const PhaseBData &phaseB,
                                   std::vector<uint32_t> &nodeWeights,
                                   std::vector<uint32_t> &pieceWeights,
                                   std::vector<uint32_t> &edgeCutWeights)
        {
            HyperGraph hg;

            nodeWeights.assign(phaseA.nodeToOp.size(), std::numeric_limits<uint32_t>::max());
            pieceWeights.assign(phaseB.pieces.size(), std::numeric_limits<uint32_t>::max());

            for (PieceId pid = 0; pid < phaseB.pieces.size(); ++pid)
            {
                (void)calculatePieceWeight(graph, phaseA, phaseB, pid, nodeWeights, pieceWeights);
            }

            std::vector<PieceCommStats> pieceCommStats(phaseB.pieces.size());
            for (PieceId pid = 0; pid < phaseB.pieces.size(); ++pid)
            {
                pieceCommStats[pid] = calculatePieceCommStats(graph, phaseA, phaseB, pid);
            }

            std::vector<std::size_t> edgeSignalCounts;
            std::vector<std::size_t> edgeFanoutExcesses;
            std::vector<std::size_t> edgeWide64Words;
            for (PieceId pid = static_cast<PieceId>(phaseB.ascs.size()); pid < phaseB.pieces.size(); ++pid)
            {
                if (phaseB.pieceToAscs[pid].empty())
                {
                    continue;
                }
                edgeSignalCounts.push_back(pieceCommStats[pid].outSignalCount);
                edgeFanoutExcesses.push_back(pieceCommStats[pid].fanoutExcess);
                edgeWide64Words.push_back(pieceCommStats[pid].wide64Words);
            }

            const std::size_t p90SignalCount = percentileThreshold(std::move(edgeSignalCounts), 0.90);
            const std::size_t p90FanoutExcess = percentileThreshold(std::move(edgeFanoutExcesses), 0.90);
            const std::size_t p90Wide64Words = percentileThreshold(std::move(edgeWide64Words), 0.90);
            constexpr std::size_t kMaxCutEdgeWeight = 65535;

            std::vector<uint32_t> balanceWeights(phaseB.pieces.size(), 1u);
            edgeCutWeights.assign(phaseB.pieces.size(), 1u);
            for (PieceId pid = 0; pid < phaseB.pieces.size(); ++pid)
            {
                const PieceCommStats &stats = pieceCommStats[pid];
                const double sourceUpdateWeight =
                    static_cast<double>(stats.outWords) + 0.5 * static_cast<double>(stats.fanoutExcess) +
                    2.0 * static_cast<double>(stats.stateBoundaryWords);
                const double balanced =
                    static_cast<double>((pid < pieceWeights.size()) ? pieceWeights[pid] : 1u) + 0.35 * sourceUpdateWeight;
                balanceWeights[pid] =
                    std::max<uint32_t>(1u, clampToUint32(static_cast<std::size_t>(std::llround(std::max(1.0, balanced)))));

                const bool hubBonus =
                    (p90SignalCount > 0 && stats.outSignalCount >= p90SignalCount) ||
                    (p90FanoutExcess > 0 && stats.fanoutExcess >= p90FanoutExcess) ||
                    (p90Wide64Words > 0 && stats.wide64Words >= p90Wide64Words);
                const std::size_t outWordScore = compressWeightMetric(stats.outWords, 128, 128.0, 32.0);
                const std::size_t wide64Score = compressWeightMetric(stats.wide64Words, 64, 64.0, 24.0);
                const std::size_t wide256Score = compressWeightMetric(stats.wide256Words, 32, 32.0, 24.0);
                const std::size_t fanoutScore = compressWeightMetric(stats.fanoutExcess, 64, 64.0, 24.0);
                const std::size_t stateBoundaryScore =
                    compressWeightMetric(stats.stateBoundaryWords, 64, 64.0, 24.0);
                const std::size_t edgeWeight =
                    1u + outWordScore + 2u * wide64Score + 4u * wide256Score + 2u * fanoutScore +
                    3u * stateBoundaryScore + (hubBonus ? 32u : 0u);
                edgeCutWeights[pid] = std::max<uint32_t>(
                    1u,
                    clampToUint32(std::min<std::size_t>(kMaxCutEdgeWeight, edgeWeight)));
            }

            hg.nodeWeights.reserve(phaseB.ascs.size());
            for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
            {
                const uint32_t weight = (aid < balanceWeights.size()) ? balanceWeights[aid] : 1u;
                hg.nodeWeights.push_back(std::max(1u, weight));
            }

            for (PieceId pid = static_cast<PieceId>(phaseB.ascs.size()); pid < phaseB.pieces.size(); ++pid)
            {
                HyperGraph::HyperEdge edge;
                edge.weight = (pid < edgeCutWeights.size()) ? std::max(1u, edgeCutWeights[pid]) : 1u;
                edge.nodes = phaseB.pieceToAscs[pid];
                if (!edge.nodes.empty())
                {
                    hg.edges.push_back(std::move(edge));
                }
            }

            return hg;
        }

        bool validateHyperGraph(const PhaseBData &phaseB,
                                const HyperGraph &hg,
                                std::string &errorMessage)
        {
            if (hg.nodeWeights.size() != phaseB.ascs.size())
            {
                errorMessage = "repcut phase-c: hypergraph node count must match asc count";
                return false;
            }

            for (const uint32_t weight : hg.nodeWeights)
            {
                if (weight == 0)
                {
                    errorMessage = "repcut phase-c: hypergraph node weight must be >= 1";
                    return false;
                }
            }

            for (const auto &edge : hg.edges)
            {
                if (edge.weight == 0)
                {
                    errorMessage = "repcut phase-c: hyperedge weight must be >= 1";
                    return false;
                }
                if (edge.nodes.empty())
                {
                    errorMessage = "repcut phase-c: hyperedge must connect at least one asc";
                    return false;
                }
                for (const AscId aid : edge.nodes)
                {
                    if (aid >= phaseB.ascs.size())
                    {
                        errorMessage = "repcut phase-c: hyperedge references invalid asc id";
                        return false;
                    }
                }
            }

            return true;
        }

        bool validateHyperEdgeContentGuard(const PhaseBData &phaseB,
                                           const HyperGraph &hg,
                                           const std::vector<uint32_t> &edgeProxyWeights,
                                           std::string &errorMessage)
        {
            constexpr std::size_t kGuardDiagLimit = 24;
            std::size_t issueCount = 0;
            std::vector<std::string> samples;
            samples.reserve(kGuardDiagLimit);

            auto addIssue = [&](std::string msg) {
                ++issueCount;
                if (samples.size() < kGuardDiagLimit)
                {
                    samples.push_back(std::move(msg));
                }
            };

            const std::size_t ascCount = phaseB.ascs.size();
            std::size_t expectedEdgeCount = 0;
            for (PieceId pid = static_cast<PieceId>(ascCount); pid < phaseB.pieces.size(); ++pid)
            {
                if (!phaseB.pieceToAscs[pid].empty())
                {
                    ++expectedEdgeCount;
                }
            }

            if (hg.edges.size() != expectedEdgeCount)
            {
                addIssue("hyperedge count mismatch: expected=" + std::to_string(expectedEdgeCount) +
                         " actual=" + std::to_string(hg.edges.size()));
            }

            std::size_t edgeIndex = 0;
            for (PieceId pid = static_cast<PieceId>(ascCount); pid < phaseB.pieces.size(); ++pid)
            {
                const auto &expectedNodesRaw = phaseB.pieceToAscs[pid];
                if (expectedNodesRaw.empty())
                {
                    continue;
                }

                if (edgeIndex >= hg.edges.size())
                {
                    addIssue("missing hyperedge for non-empty piece: piece=" + std::to_string(pid) +
                             " expected_nodes=" + std::to_string(expectedNodesRaw.size()));
                    continue;
                }

                const HyperGraph::HyperEdge &edge = hg.edges[edgeIndex];
                if (edge.weight == 0)
                {
                    addIssue("hyperedge weight is zero: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid));
                }

                const uint32_t expectedWeight =
                    (pid < edgeProxyWeights.size()) ? std::max<uint32_t>(1u, edgeProxyWeights[pid]) : 1u;
                if (edge.weight != expectedWeight)
                {
                    addIssue("hyperedge weight mismatch: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid) +
                             " expected_weight=" + std::to_string(expectedWeight) +
                             " actual_weight=" + std::to_string(edge.weight));
                }

                if (edge.nodes.empty())
                {
                    addIssue("hyperedge has empty nodes: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid));
                }

                std::vector<AscId> actualNodes = edge.nodes;
                std::sort(actualNodes.begin(), actualNodes.end());
                const auto actualUniqueEnd = std::unique(actualNodes.begin(), actualNodes.end());
                const bool hasDuplicateAsc = actualUniqueEnd != actualNodes.end();
                actualNodes.erase(actualUniqueEnd, actualNodes.end());
                if (hasDuplicateAsc)
                {
                    addIssue("hyperedge contains duplicate asc ids: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid));
                }

                bool hasOutOfRangeAsc = false;
                AscId firstOutOfRangeAsc = 0;
                for (const AscId aid : actualNodes)
                {
                    if (aid >= ascCount)
                    {
                        hasOutOfRangeAsc = true;
                        firstOutOfRangeAsc = aid;
                        break;
                    }
                }
                if (hasOutOfRangeAsc)
                {
                    addIssue("hyperedge asc id out of range: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid) +
                             " asc_id=" + std::to_string(firstOutOfRangeAsc) +
                             " asc_count=" + std::to_string(ascCount));
                }

                std::vector<AscId> expectedNodes = expectedNodesRaw;
                std::sort(expectedNodes.begin(), expectedNodes.end());
                const auto expectedUniqueEnd = std::unique(expectedNodes.begin(), expectedNodes.end());
                const bool pieceHasDuplicateAsc = expectedUniqueEnd != expectedNodes.end();
                expectedNodes.erase(expectedUniqueEnd, expectedNodes.end());
                if (pieceHasDuplicateAsc)
                {
                    addIssue("pieceToAscs contains duplicate asc ids: piece=" + std::to_string(pid));
                }

                if (actualNodes != expectedNodes)
                {
                    addIssue("hyperedge nodes mismatch pieceToAscs: edge_index=" + std::to_string(edgeIndex) +
                             " piece=" + std::to_string(pid) +
                             " expected_nodes=" + std::to_string(expectedNodes.size()) +
                             " actual_nodes=" + std::to_string(actualNodes.size()));
                }

                ++edgeIndex;
            }

            if (edgeIndex < hg.edges.size())
            {
                addIssue("unexpected extra hyperedges: matched=" + std::to_string(edgeIndex) +
                         " actual=" + std::to_string(hg.edges.size()));
            }

            if (issueCount > 0)
            {
                std::ostringstream oss;
                oss << "repcut phase-c guard: invalid hyperedge content"
                    << " total_issues=" << issueCount
                    << " sample_count=" << samples.size();
                for (std::size_t i = 0; i < samples.size(); ++i)
                {
                    oss << " | sample[" << i << "] " << samples[i];
                }
                errorMessage = oss.str();
                return false;
            }
            return true;
        }

        std::string toFixedString(double value, int precision = 6)
        {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss.precision(precision);
            oss << value;
            return oss.str();
        }

        bool writeTextFile(const std::filesystem::path &path,
                           const std::string &content,
                           std::string &errorMessage)
        {
            std::ofstream out(path);
            if (!out)
            {
                errorMessage = "cannot open file for writing: " + path.string();
                return false;
            }
            out << content;
            out.flush();
            if (!out.good())
            {
                errorMessage = "failed writing file: " + path.string();
                return false;
            }
            return true;
        }

        bool writeHyperGraphToHmetis(const HyperGraph &hg,
                                     const std::filesystem::path &path,
                                     std::string &errorMessage)
        {
            std::ofstream out(path);
            if (!out)
            {
                errorMessage = "cannot open hMETIS file for writing: " + path.string();
                return false;
            }

            out << hg.edges.size() << " " << hg.nodeWeights.size() << " 11\n";
            for (const auto &edge : hg.edges)
            {
                out << edge.weight;
                for (const AscId aid : edge.nodes)
                {
                    out << " " << (aid + 1);
                }
                out << "\n";
            }
            for (const uint32_t weight : hg.nodeWeights)
            {
                out << weight << "\n";
            }
            out.flush();
            if (!out.good())
            {
                errorMessage = "failed writing hMETIS file: " + path.string();
                return false;
            }
            return true;
        }

        std::string escapeJson(std::string_view value)
        {
            std::string out;
            out.reserve(value.size() + 8);
            for (const char ch : value)
            {
                switch (ch)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
            }
            return out;
        }

        std::string normalizeBackendToken(std::string_view value)
        {
            std::string out(value);
            for (char &ch : out)
            {
                if (ch >= 'A' && ch <= 'Z')
                {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
                else if (ch == '_')
                {
                    ch = '-';
                }
            }
            return out;
        }

#if WOLVRIX_HAVE_MT_KAHYPAR
        struct ResolvedMtKaHyParPreset
        {
            mt_kahypar_preset_type_t preset = DETERMINISTIC_QUALITY;
            std::string requestedToken;
            std::string effectiveToken;
            bool overridden = false;
        };

        std::optional<ResolvedMtKaHyParPreset> resolveMtKaHyParPreset(std::string_view presetText)
        {
            const std::string normalized = normalizeBackendToken(presetText);

            if (normalized.empty() || normalized == "quality" || normalized == "highest-quality")
            {
                return ResolvedMtKaHyParPreset{
                    DETERMINISTIC_QUALITY,
                    normalized,
                    "deterministic-quality",
                    normalized != "deterministic-quality"};
            }
            if (normalized == "default" || normalized == "deterministic")
            {
                return ResolvedMtKaHyParPreset{
                    DETERMINISTIC,
                    normalized,
                    "deterministic",
                    normalized != "deterministic"};
            }
            if (normalized == "deterministic-quality")
            {
                return ResolvedMtKaHyParPreset{
                    DETERMINISTIC_QUALITY,
                    normalized,
                    "deterministic-quality",
                    false};
            }

            return std::nullopt;
        }

        std::string takeMtKaHyParError(mt_kahypar_error_t &error)
        {
            std::string message;
            if (error.msg != nullptr && error.msg_len > 0)
            {
                message.assign(error.msg, error.msg_len);
            }
            if (error.msg != nullptr)
            {
                mt_kahypar_free_error_content(&error);
            }
            return message;
        }
#endif

        enum class PartitionBackendErrorKind
        {
            kNone,
            kUnavailable,
            kInvalidConfig,
            kExecutionFailed,
        };

        struct PartitionBackendRequest
        {
            std::filesystem::path hmetisPath;
            std::filesystem::path partitionPath;
            std::size_t partitionCount = 0;
            double imbalanceFactor = 0.0;
            std::size_t ascCount = 0;
            std::string preset;
            std::size_t threadCount = 0;
        };

        struct PartitionBackendResponse
        {
            PartitionBackendErrorKind errorKind = PartitionBackendErrorKind::kNone;
            std::string errorMessage;
            std::vector<std::string> backendLogs;
            uint64_t solverRunMs = 0;
            uint64_t parsePartitionMs = 0;
            std::filesystem::path partitionPath;
            bool partitionComplete = true;
            std::string partitionWarning;
            std::vector<uint32_t> partition;
        };

        class PartitionBackend
        {
        public:
            virtual ~PartitionBackend() = default;
            virtual std::string_view name() const = 0;
            virtual bool run(const PartitionBackendRequest &request,
                             PartitionBackendResponse &response) const = 0;
        };

        class MtKaHyParBackend final : public PartitionBackend
        {
        public:
            std::string_view name() const override
            {
                return "mt-kahypar";
            }

            bool run(const PartitionBackendRequest &request,
                     PartitionBackendResponse &response) const override
            {
                response = {};
                auto addBackendLog = [&](std::string message) {
                    response.backendLogs.push_back(std::move(message));
                };
#if !WOLVRIX_HAVE_MT_KAHYPAR
                (void)request;
                response.errorKind = PartitionBackendErrorKind::kUnavailable;
                response.errorMessage = "repcut was built without mt-kahypar support";
                return false;
#else
                {
                    std::ostringstream oss;
                    oss << "invoke hgr_path=" << request.hmetisPath.string()
                        << " partition_path=" << request.partitionPath.string()
                        << " k=" << request.partitionCount
                        << " imbalance_factor=" << toFixedString(request.imbalanceFactor, 6)
                        << " asc_count=" << request.ascCount
                        << " requested_preset_token=" << request.preset
                        << " requested_threads=" << request.threadCount;
                    addBackendLog(oss.str());
                }
                const std::optional<ResolvedMtKaHyParPreset> resolvedPreset = resolveMtKaHyParPreset(request.preset);
                if (!resolvedPreset)
                {
                    response.errorKind = PartitionBackendErrorKind::kInvalidConfig;
                    response.errorMessage =
                        "unsupported mt-kahypar preset for deterministic repcut: " + std::string(request.preset);
                    return false;
                }
                {
                    std::ostringstream oss;
                    oss << "resolved_preset requested_token="
                        << (resolvedPreset->requestedToken.empty() ? std::string("<empty>") : resolvedPreset->requestedToken)
                        << " effective_preset_token=" << resolvedPreset->effectiveToken
                        << " overridden=" << (resolvedPreset->overridden ? "true" : "false");
                    addBackendLog(oss.str());
                }
                addBackendLog("context_from_preset preset_enum=" +
                              std::to_string(static_cast<int>(resolvedPreset->preset)));

                static std::once_flag initOnce;
                static std::size_t initializedThreadCount = 0;
                std::size_t threadCount = request.threadCount;
                if (threadCount == 0)
                {
                    threadCount = std::thread::hardware_concurrency();
                    if (threadCount == 0)
                    {
                        threadCount = 1;
                    }
                }
                bool initializedThisRun = false;
                std::call_once(initOnce, [&]() {
                    mt_kahypar_initialize(threadCount, false);
                    initializedThreadCount = threadCount;
                    initializedThisRun = true;
                });
                const std::size_t activeThreadCount = initializedThreadCount == 0 ? threadCount : initializedThreadCount;
                {
                    std::ostringstream oss;
                    oss << "initialize requested_threads=" << threadCount
                        << " active_threads=" << activeThreadCount
                        << " interleaved_numa=false"
                        << " initialized_this_run=" << (initializedThisRun ? "true" : "false");
                    addBackendLog(oss.str());
                }
                if (!initializedThisRun && threadCount != activeThreadCount)
                {
                    addBackendLog("thread_pool_reuse requested_threads=" + std::to_string(threadCount) +
                                  " active_threads=" + std::to_string(activeThreadCount));
                }

                mt_kahypar_context_t *context = nullptr;
                mt_kahypar_hypergraph_t hypergraph{nullptr, NULLPTR_HYPERGRAPH};
                mt_kahypar_partitioned_hypergraph_t partitionedHg{nullptr, NULLPTR_PARTITION};
                auto cleanup = [&]() {
                    if (partitionedHg.partitioned_hg != nullptr)
                    {
                        mt_kahypar_free_partitioned_hypergraph(partitionedHg);
                        partitionedHg = {nullptr, NULLPTR_PARTITION};
                    }
                    if (hypergraph.hypergraph != nullptr)
                    {
                        mt_kahypar_free_hypergraph(hypergraph);
                        hypergraph = {nullptr, NULLPTR_HYPERGRAPH};
                    }
                    if (context != nullptr)
                    {
                        mt_kahypar_free_context(context);
                        context = nullptr;
                    }
                };

                mt_kahypar_error_t mtError{};
                context = mt_kahypar_context_from_preset(resolvedPreset->preset);
                if (context == nullptr)
                {
                    response.errorKind = PartitionBackendErrorKind::kExecutionFailed;
                    response.errorMessage = "mt-kahypar failed to create partition context";
                    cleanup();
                    return false;
                }

                constexpr int kMtKaHyParSeed = 0;
                mt_kahypar_set_seed(kMtKaHyParSeed);
                addBackendLog("set_seed seed=" + std::to_string(kMtKaHyParSeed));
                mt_kahypar_set_partitioning_parameters(
                    context,
                    static_cast<mt_kahypar_partition_id_t>(request.partitionCount),
                    request.imbalanceFactor,
                    KM1);
                {
                    std::ostringstream oss;
                    oss << "set_partitioning_parameters k=" << request.partitionCount
                        << " imbalance_factor=" << toFixedString(request.imbalanceFactor, 6)
                        << " objective=KM1";
                    addBackendLog(oss.str());
                }

                const std::string hgrPath = request.hmetisPath.string();
                addBackendLog("read_hypergraph_from_file path=" + hgrPath + " format=HMETIS");
                hypergraph = mt_kahypar_read_hypergraph_from_file(
                    hgrPath.c_str(), context, HMETIS, &mtError);
                if (hypergraph.hypergraph == nullptr)
                {
                    response.errorKind = PartitionBackendErrorKind::kExecutionFailed;
                    response.errorMessage = "mt-kahypar failed to load hMETIS file: " + takeMtKaHyParError(mtError);
                    cleanup();
                    return false;
                }
                addBackendLog("read_hypergraph_done node_count=" +
                              std::to_string(static_cast<std::size_t>(mt_kahypar_num_hypernodes(hypergraph))));

                addBackendLog("partition begin");
                const auto solverStart = std::chrono::steady_clock::now();
                partitionedHg = mt_kahypar_partition(hypergraph, context, &mtError);
                response.solverRunMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - solverStart)
                        .count());
                if (partitionedHg.partitioned_hg == nullptr)
                {
                    response.errorKind = PartitionBackendErrorKind::kExecutionFailed;
                    response.errorMessage = "mt-kahypar partition failed: " + takeMtKaHyParError(mtError);
                    cleanup();
                    return false;
                }
                addBackendLog("partition done run_ms=" + std::to_string(response.solverRunMs));

                response.partitionPath = request.partitionPath;
                if (!request.partitionPath.empty())
                {
                    const std::string partPath = request.partitionPath.string();
                    addBackendLog("write_partition_to_file path=" + partPath);
                    const mt_kahypar_status_t writeStatus =
                        mt_kahypar_write_partition_to_file(partitionedHg, partPath.c_str(), &mtError);
                    if (writeStatus != SUCCESS)
                    {
                        response.errorKind = PartitionBackendErrorKind::kExecutionFailed;
                        response.errorMessage = "mt-kahypar failed to write partition file: " +
                                                takeMtKaHyParError(mtError);
                        cleanup();
                        return false;
                    }
                }

                const auto parseStart = std::chrono::steady_clock::now();
                const std::size_t nodeCount = static_cast<std::size_t>(mt_kahypar_num_hypernodes(hypergraph));
                std::vector<mt_kahypar_partition_id_t> rawPartition(nodeCount, 0);
                if (!rawPartition.empty())
                {
                    mt_kahypar_get_partition(partitionedHg, rawPartition.data());
                }
                addBackendLog("get_partition entries=" + std::to_string(rawPartition.size()));

                response.partition.assign(request.ascCount, 0);
                std::size_t negativeCount = 0;
                std::size_t outOfRangeCount = 0;
                const std::size_t limit = std::min(request.ascCount, rawPartition.size());
                for (std::size_t i = 0; i < limit; ++i)
                {
                    int64_t part = static_cast<int64_t>(rawPartition[i]);
                    if (part < 0)
                    {
                        ++negativeCount;
                        part = 0;
                    }
                    if (part >= static_cast<int64_t>(request.partitionCount))
                    {
                        ++outOfRangeCount;
                        part = static_cast<int64_t>(request.partitionCount - 1);
                    }
                    response.partition[i] = static_cast<uint32_t>(part);
                }

                std::ostringstream warning;
                if (rawPartition.size() < request.ascCount)
                {
                    response.partitionComplete = false;
                    warning << "fewer entries than ASC count (" << rawPartition.size()
                            << " < " << request.ascCount
                            << "), missing entries default to 0";
                }
                else if (rawPartition.size() > request.ascCount)
                {
                    response.partitionComplete = false;
                    warning << "extra entries in partition result (" << rawPartition.size()
                            << " > " << request.ascCount
                            << "), extras ignored";
                }
                if (negativeCount > 0)
                {
                    if (warning.tellp() > 0)
                    {
                        warning << "; ";
                    }
                    response.partitionComplete = false;
                    warning << "negative ids clamped=" << negativeCount;
                }
                if (outOfRangeCount > 0)
                {
                    if (warning.tellp() > 0)
                    {
                        warning << "; ";
                    }
                    response.partitionComplete = false;
                    warning << "ids >= max-part-count clamped=" << outOfRangeCount;
                }
                response.partitionWarning = warning.str();
                response.parsePartitionMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - parseStart)
                        .count());

                cleanup();
                return true;
#endif
            }
        };

        std::unique_ptr<PartitionBackend> createPartitionBackend(const RepcutOptions &options)
        {
            const std::string normalized = normalizeBackendToken(options.partitioner);
            if (normalized == "mt-kahypar" || normalized == "mtkahypar")
            {
                return std::make_unique<MtKaHyParBackend>();
            }
            return nullptr;
        }

        void collectStorageInfos(const wolvrix::lib::grh::Graph &graph,
                                 std::unordered_map<std::string, StorageInfo> &regInfos,
                                 std::unordered_map<std::string, StorageInfo> &latchInfos,
                                 std::unordered_map<std::string, StorageInfo> &memInfos)
        {
            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                switch (op.kind())
                {
                case wolvrix::lib::grh::OperationKind::kRegister:
                {
                    const std::string symbol(op.symbolText());
                    if (!symbol.empty())
                    {
                        regInfos[symbol].declOp = opId;
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                    if (auto sym = getAttrString(op, "regSymbol"))
                    {
                        regInfos[*sym].readPorts.push_back(opId);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
                    if (auto sym = getAttrString(op, "regSymbol"))
                    {
                        regInfos[*sym].writePorts.push_back(opId);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kLatch:
                {
                    const std::string symbol(op.symbolText());
                    if (!symbol.empty())
                    {
                        latchInfos[symbol].declOp = opId;
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                    if (auto sym = getAttrString(op, "latchSymbol"))
                    {
                        latchInfos[*sym].readPorts.push_back(opId);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                    if (auto sym = getAttrString(op, "latchSymbol"))
                    {
                        latchInfos[*sym].writePorts.push_back(opId);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kMemory:
                {
                    const std::string symbol(op.symbolText());
                    if (!symbol.empty())
                    {
                        memInfos[symbol].declOp = opId;
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                    if (auto sym = getAttrString(op, "memSymbol"))
                    {
                        memInfos[*sym].readPorts.push_back(opId);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                    if (auto sym = getAttrString(op, "memSymbol"))
                    {
                        memInfos[*sym].writePorts.push_back(opId);
                    }
                    break;
                default:
                    break;
                }
            }
        }

        void assignStorageOpsToPartition(
            const std::unordered_map<std::string, StorageInfo> &infos,
            const std::unordered_map<std::string, uint32_t> &partitionBySymbol,
            std::unordered_map<wolvrix::lib::grh::OperationId, uint32_t, wolvrix::lib::grh::OperationIdHash> &opPartition,
            std::unordered_map<wolvrix::lib::grh::OperationId, PartitionSet, wolvrix::lib::grh::OperationIdHash> &opPartitionSet)
        {
            for (const auto &[symbol, partId] : partitionBySymbol)
            {
                auto it = infos.find(symbol);
                if (it == infos.end())
                {
                    continue;
                }
                const StorageInfo &info = it->second;
                if (info.declOp.valid())
                {
                    opPartition.emplace(info.declOp, partId);
                    opPartitionSet[info.declOp].add(partId);
                }
                for (const auto opId : info.readPorts)
                {
                    opPartition.emplace(opId, partId);
                    opPartitionSet[opId].add(partId);
                }
                for (const auto opId : info.writePorts)
                {
                    opPartition.emplace(opId, partId);
                    opPartitionSet[opId].add(partId);
                }
            }
        }

        std::optional<uint32_t> inferStoragePartitionFromReadUsers(
            const wolvrix::lib::grh::Graph &graph,
            const StorageInfo &info,
            const std::unordered_map<wolvrix::lib::grh::OperationId, PartitionSet, wolvrix::lib::grh::OperationIdHash>
                &opPartitionSet)
        {
            std::unordered_map<uint32_t, std::size_t> scoreByPart;
            for (const auto readOpId : info.readPorts)
            {
                if (!readOpId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation readOp = graph.getOperation(readOpId);
                for (const auto resultValue : readOp.results())
                {
                    if (!resultValue.valid())
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::Value value = graph.getValue(resultValue);
                    for (const auto &user : value.users())
                    {
                        auto it = opPartitionSet.find(user.operation);
                        if (it == opPartitionSet.end() || it->second.empty())
                        {
                            continue;
                        }
                        it->second.forEach([&](uint32_t partId) {
                            scoreByPart[partId] += 1;
                        });
                    }
                }
            }
            if (scoreByPart.empty())
            {
                return std::nullopt;
            }

            std::optional<uint32_t> bestPart;
            std::size_t bestScore = 0;
            for (const auto &[partId, score] : scoreByPart)
            {
                if (!bestPart || score > bestScore || (score == bestScore && partId < *bestPart))
                {
                    bestPart = partId;
                    bestScore = score;
                }
            }
            return bestPart;
        }

        std::size_t inferMissingStoragePartitions(
            const wolvrix::lib::grh::Graph &graph,
            const std::unordered_map<std::string, StorageInfo> &infos,
            std::unordered_map<std::string, uint32_t> &partitionBySymbol,
            const std::unordered_map<wolvrix::lib::grh::OperationId, PartitionSet, wolvrix::lib::grh::OperationIdHash>
                &opPartitionSet)
        {
            std::size_t assignedCount = 0;
            for (const auto &[symbol, info] : infos)
            {
                if (partitionBySymbol.find(symbol) != partitionBySymbol.end() || info.readPorts.empty())
                {
                    continue;
                }
                std::optional<uint32_t> owner = inferStoragePartitionFromReadUsers(graph, info, opPartitionSet);
                if (!owner)
                {
                    continue;
                }
                partitionBySymbol.emplace(symbol, *owner);
                assignedCount += 1;
            }
            return assignedCount;
        }

    } // namespace

    RepcutPass::RepcutPass()
        : Pass("repcut", "repcut", "Partition a single graph via RepCut hypergraph partitioning"),
          options_({})
    {
    }

    RepcutPass::RepcutPass(RepcutOptions options)
        : Pass("repcut", "repcut", "Partition a single graph via RepCut hypergraph partitioning"),
          options_(std::move(options))
    {
    }

    PassResult RepcutPass::run()
    {
        PassResult result;
        const auto totalStart = std::chrono::steady_clock::now();
        const auto msSince = [&](const std::chrono::steady_clock::time_point &start) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                    .count());
        };

        if (options_.path.empty())
        {
            error("repcut requires -path");
            result.failed = true;
            return result;
        }
        if (options_.partitionCount < 2)
        {
            error("repcut partition_count must be >= 2");
            result.failed = true;
            return result;
        }
        if (options_.partitionCount > kMaxPartitionCount)
        {
            error("repcut partition_count must be <= " + std::to_string(kMaxPartitionCount));
            result.failed = true;
            return result;
        }
        if (options_.imbalanceFactor < 0.0)
        {
            error("repcut imbalance_factor must be >= 0");
            result.failed = true;
            return result;
        }
        std::string resolveError;
        const std::optional<std::string> targetGraphName = resolveTargetGraphName(design(), options_.path, resolveError);
        if (!targetGraphName)
        {
            error(resolveError);
            result.failed = true;
            return result;
        }

        wolvrix::lib::grh::Graph *graph = design().findGraph(*targetGraphName);
        if (!graph)
        {
            error("repcut target graph not found: " + *targetGraphName);
            result.failed = true;
            return result;
        }

        {
            std::ostringstream boot;
            boot << "repcut start: path=" << options_.path
                 << " graph=" << graph->symbol()
                 << " partition_count=" << options_.partitionCount
                 << " imbalance_factor=" << toFixedString(options_.imbalanceFactor, 6)
                 << " work_dir=" << (options_.workDir.empty() ? std::string(".") : options_.workDir)
                 << " partitioner=" << options_.partitioner
                 << " mtkahypar_preset=" << options_.mtKaHyParPreset
                 << " mtkahypar_threads=" << options_.mtKaHyParThreads
                 << " mem_cone_union=true"
                 << " keep_intermediate=" << (options_.keepIntermediateFiles ? "true" : "false");
            logInfo(boot.str());
        }

        for (const auto opId : graph->operations())
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            if (isHierOpKind(op.kind()))
            {
                error(*graph,
                      op,
                      "repcut guard: target graph must not contain hierarchical ops before partition kind=" +
                          std::string(wolvrix::lib::grh::toString(op.kind())));
                result.failed = true;
            }
        }
        if (result.failed)
        {
            return result;
        }

        const auto phaseAStart = std::chrono::steady_clock::now();
        const PhaseAData data = buildPhaseAData(*graph);

        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> inoutInputValues;
        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> inoutOutputValues;
        for (const auto &port : graph->inoutPorts())
        {
            inoutInputValues.insert(port.in);
            inoutOutputValues.insert(port.out);
            inoutOutputValues.insert(port.oe);
        }

        std::size_t sinkOpCount = 0;
        std::size_t sinkOutputValueCount = 0;
        std::size_t sourceValueCount = 0;
        std::size_t combOpCount = 0;

        for (const auto opId : graph->operations())
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            if (isSinkOp(op))
            {
                ++sinkOpCount;
            }
            if (isCombOp(op))
            {
                ++combOpCount;
            }
        }
        for (const auto valueId : graph->values())
        {
            if (graph->valueIsOutput(valueId) || inoutOutputValues.find(valueId) != inoutOutputValues.end())
            {
                ++sinkOutputValueCount;
            }
            if (isSourceValue(*graph, valueId, inoutInputValues))
            {
                ++sourceValueCount;
            }
        }

        std::size_t edgeCount = 0;
        std::size_t maxInDegree = 0;
        std::size_t maxOutDegree = 0;
        for (NodeId node = 0; node < data.nodeToOp.size(); ++node)
        {
            edgeCount += data.inNeighbors[node].size();
            maxInDegree = std::max(maxInDegree, data.inNeighbors[node].size());
            maxOutDegree = std::max(maxOutDegree, data.outNeighbors[node].size());
        }

        std::ostringstream summary;
        summary << "repcut phase-a: graph=" << graph->symbol()
                << " nodes=" << data.nodeToOp.size()
                << " edges=" << edgeCount
                << " sink_ops=" << sinkOpCount
                << " sink_values=" << sinkOutputValueCount
                << " source_values=" << sourceValueCount
                << " comb_ops=" << combOpCount
                << " max_in_degree=" << maxInDegree
                << " max_out_degree=" << maxOutDegree;
            const uint64_t phaseAMs = msSince(phaseAStart);
            summary << " elapsed_ms=" << phaseAMs;
            logInfo(summary.str());

            const auto phaseBStart = std::chrono::steady_clock::now();
            logInfo("repcut phase-b: begin build_ascs");
            const auto buildAscsStart = std::chrono::steady_clock::now();
            PhaseBData phaseB = buildAscs(
                *graph,
                data,
                inoutInputValues,
                [&](const std::string &message) {
                    logInfo(message);
                });
            const uint64_t buildAscsMs = msSince(buildAscsStart);
            logInfo("repcut phase-b: build_ascs done elapsed_ms=" + std::to_string(buildAscsMs));

            logInfo("repcut phase-b: begin build_pieces");
            const auto buildPiecesStart = std::chrono::steady_clock::now();
            buildPieces(
                phaseB,
                data,
                [&](const std::string &message) {
                    logInfo(message);
                });
            const uint64_t buildPiecesMs = msSince(buildPiecesStart);
            logInfo("repcut phase-b: build_pieces done elapsed_ms=" + std::to_string(buildPiecesMs));

        std::string phaseBError;
        if (!validatePhaseB(data, phaseB, phaseBError))
        {
            error(phaseBError);
            result.failed = true;
            return result;
        }

        const auto phaseBGuardStart = std::chrono::steady_clock::now();
        std::string phaseBGuardError;
        if (!validateHyperNodeAndHyperSinkTypes(*graph, data, phaseB, inoutOutputValues, phaseBGuardError))
        {
            error(phaseBGuardError);
            result.failed = true;
            return result;
        }
        logInfo("repcut phase-b guard: hypernode/hypersink node types validated elapsed_ms=" +
                std::to_string(msSince(phaseBGuardStart)));

        std::size_t opSinkCountInAsc = 0;
        std::size_t valueSinkCountInAsc = 0;
        std::size_t totalCombOpsInAsc = 0;
        std::size_t maxAscSinkCount = 0;
        std::size_t maxAscCombOpCount = 0;
        for (const auto &asc : phaseB.ascs)
        {
            maxAscSinkCount = std::max(maxAscSinkCount, asc.sinks.size());
            maxAscCombOpCount = std::max(maxAscCombOpCount, asc.combOps.size());
            totalCombOpsInAsc += asc.combOps.size();
            for (const size_t sinkIndex : asc.sinks)
            {
                if (phaseB.sinks[sinkIndex].kind == SinkRef::Kind::Operation)
                {
                    ++opSinkCountInAsc;
                }
                else
                {
                    ++valueSinkCountInAsc;
                }
            }
        }

        std::size_t nonAscPieceCount = 0;
        std::size_t maxPieceSize = 0;
        std::size_t emptyPieceCount = 0;
        for (PieceId pid = 0; pid < phaseB.pieces.size(); ++pid)
        {
            const auto &piece = phaseB.pieces[pid];
            if (piece.empty())
            {
                ++emptyPieceCount;
            }
            maxPieceSize = std::max(maxPieceSize, piece.size());
            if (pid >= phaseB.ascs.size())
            {
                ++nonAscPieceCount;
            }
        }

        std::ostringstream phaseBSummary;
        phaseBSummary << "repcut phase-b: graph=" << graph->symbol()
                      << " sinks=" << phaseB.sinks.size()
                      << " sink_ops=" << opSinkCountInAsc
                      << " sink_values=" << valueSinkCountInAsc
                      << " ascs=" << phaseB.ascs.size()
                      << " total_asc_comb_ops=" << totalCombOpsInAsc
                      << " max_asc_sinks=" << maxAscSinkCount
                      << " max_asc_comb_ops=" << maxAscCombOpCount
                      << " pieces=" << phaseB.pieces.size()
                      << " non_asc_pieces=" << nonAscPieceCount
                      << " empty_pieces=" << emptyPieceCount
                          << " max_piece_size=" << maxPieceSize;
                const uint64_t phaseBMs = msSince(phaseBStart);
                phaseBSummary << " build_ascs_ms=" << buildAscsMs
                          << " build_pieces_ms=" << buildPiecesMs
                          << " elapsed_ms=" << phaseBMs;
                logInfo(phaseBSummary.str());

                const auto phaseCStart = std::chrono::steady_clock::now();
        std::vector<uint32_t> nodeWeights;
        std::vector<uint32_t> pieceWeights;
        std::vector<uint32_t> edgeProxyWeights;
                const auto hyperBuildStart = std::chrono::steady_clock::now();
        const HyperGraph hg = buildHyperGraph(*graph, data, phaseB, nodeWeights, pieceWeights, edgeProxyWeights);
                const uint64_t hyperBuildMs = msSince(hyperBuildStart);

        const auto phaseCGuardStart = std::chrono::steady_clock::now();
        std::string phaseCGuardError;
        if (!validateHyperEdgeContentGuard(phaseB, hg, edgeProxyWeights, phaseCGuardError))
        {
            error(phaseCGuardError);
            result.failed = true;
            return result;
        }
        logInfo("repcut phase-c guard: hyperedge content validated elapsed_ms=" +
                std::to_string(msSince(phaseCGuardStart)));

        std::string phaseCError;
        if (!validateHyperGraph(phaseB, hg, phaseCError))
        {
            error(phaseCError);
            result.failed = true;
            return result;
        }

        std::size_t totalPieceWeight = 0;
        std::size_t maxPieceWeight = 0;
        for (const uint32_t pieceWeight : pieceWeights)
        {
            totalPieceWeight += pieceWeight;
            maxPieceWeight = std::max(maxPieceWeight, static_cast<std::size_t>(pieceWeight));
        }

        std::size_t totalNodeWeight = 0;
        std::size_t weightedNodeCount = 0;
        std::size_t maxNodeWeight = 0;
        for (const uint32_t nodeWeight : nodeWeights)
        {
            if (nodeWeight == std::numeric_limits<uint32_t>::max())
            {
                continue;
            }
            totalNodeWeight += nodeWeight;
            weightedNodeCount += 1;
            maxNodeWeight = std::max(maxNodeWeight, static_cast<std::size_t>(nodeWeight));
        }

        std::size_t maxHyperNodeWeight = 0;
        std::size_t maxHyperEdgeWeight = 0;
        for (const uint32_t w : hg.nodeWeights)
        {
            maxHyperNodeWeight = std::max(maxHyperNodeWeight, static_cast<std::size_t>(w));
        }
        for (const auto &edge : hg.edges)
        {
            maxHyperEdgeWeight = std::max(maxHyperEdgeWeight, static_cast<std::size_t>(edge.weight));
        }

        std::ostringstream phaseCSummary;
        phaseCSummary << "repcut phase-c: graph=" << graph->symbol()
                      << " weighted_nodes=" << weightedNodeCount
                      << " total_node_weight=" << totalNodeWeight
                      << " max_node_weight=" << maxNodeWeight
                      << " total_piece_weight=" << totalPieceWeight
                      << " max_piece_weight=" << maxPieceWeight
                      << " hyper_nodes=" << hg.nodeWeights.size()
                      << " hyper_edges=" << hg.edges.size()
                      << " max_hyper_node_weight=" << maxHyperNodeWeight
                      << " max_hyper_edge_weight=" << maxHyperEdgeWeight;
        const uint64_t phaseCMs = msSince(phaseCStart);
        phaseCSummary << " hyper_build_ms=" << hyperBuildMs
                      << " elapsed_ms=" << phaseCMs;
        logInfo(phaseCSummary.str());

        if (hg.nodeWeights.empty())
        {
            warning("repcut phase-d: hypergraph has no nodes, skipping partitioning");
            return result;
        }

        const auto phaseDStart = std::chrono::steady_clock::now();
        std::filesystem::path outputDir = options_.workDir.empty() ? std::filesystem::path(".")
                                        : std::filesystem::path(options_.workDir);
        std::error_code fsError;
        std::filesystem::create_directories(outputDir, fsError);
        if (fsError)
        {
            error("repcut phase-d: cannot create output directory: " + outputDir.string());
            result.failed = true;
            return result;
        }

        const std::string graphBase = wolvrix::lib::grh::Graph::normalizeComponent(graph->symbol());
        const std::string stem = graphBase + "_repcut_k" + std::to_string(options_.partitionCount);
        const std::filesystem::path hmetisPath = outputDir / (stem + ".hgr");
        const std::filesystem::path partitionPath = std::filesystem::path(hmetisPath.string() + ".part" +
                                                                           std::to_string(options_.partitionCount));

        std::string ioError;
        const auto writeHgrStart = std::chrono::steady_clock::now();
        if (!writeHyperGraphToHmetis(hg, hmetisPath, ioError))
        {
            error("repcut phase-d: " + ioError);
            result.failed = true;
            return result;
        }
        const uint64_t writeHgrMs = msSince(writeHgrStart);
        result.artifacts.push_back(hmetisPath.string());

        std::error_code sizeEc;
        const uintmax_t hgrBytes = std::filesystem::file_size(hmetisPath, sizeEc);

        {
            std::ostringstream dprep;
            dprep << "repcut phase-d prep: hgr_path=" << hmetisPath.string()
                  << " hgr_bytes=" << hgrBytes
                  << " write_hgr_ms=" << writeHgrMs;
            logInfo(dprep.str());
        }

        const std::unique_ptr<PartitionBackend> partitionBackend = createPartitionBackend(options_);
        if (!partitionBackend)
        {
            error("repcut phase-d: unsupported partitioner: " + options_.partitioner);
            result.failed = true;
            return result;
        }

        PartitionBackendRequest backendRequest;
        backendRequest.hmetisPath = hmetisPath;
        backendRequest.partitionPath = partitionPath;
        backendRequest.partitionCount = options_.partitionCount;
        backendRequest.imbalanceFactor = options_.imbalanceFactor;
        backendRequest.ascCount = phaseB.ascs.size();
        backendRequest.preset = options_.mtKaHyParPreset;
        backendRequest.threadCount = options_.mtKaHyParThreads;

        PartitionBackendResponse backendResponse;
        const bool backendOk = partitionBackend->run(backendRequest, backendResponse);
        for (const auto &backendLog : backendResponse.backendLogs)
        {
            logInfo("repcut phase-d " + std::string(partitionBackend->name()) + " call: " + backendLog);
        }
        if (!backendOk)
        {
            std::ostringstream diag;
            diag << "repcut phase-d: " << partitionBackend->name() << " backend failed";
            if (!backendResponse.errorMessage.empty())
            {
                diag << ": " << backendResponse.errorMessage;
            }
            diag << "; graph=" << graph->symbol()
                 << "; hyper_nodes=" << hg.nodeWeights.size()
                 << "; hyper_edges=" << hg.edges.size()
                 << "; hmetis=" << hmetisPath.string();
            if (!backendResponse.partitionPath.empty())
            {
                diag << "; partition_file=" << backendResponse.partitionPath.string();
            }
            if (hg.edges.empty())
            {
                diag << "; hint=hypergraph has zero hyper-edges (degenerate partitioning input)";
            }
            error(diag.str());
            result.failed = true;
            return result;
        }

        const uint64_t partitionRunMs = backendResponse.solverRunMs;
        const uint64_t parsePartMs = backendResponse.parsePartitionMs;
        const std::filesystem::path &partitionOutPath = backendResponse.partitionPath;
        const std::vector<uint32_t> &ascPartition = backendResponse.partition;
        const bool partitionComplete = backendResponse.partitionComplete;
        const std::string &partitionWarning = backendResponse.partitionWarning;

        logInfo("repcut phase-d " + std::string(partitionBackend->name()) + ": run_ms=" +
                std::to_string(partitionRunMs));

        if (!partitionOutPath.empty())
        {
            result.artifacts.push_back(partitionOutPath.string());
        }

        if (!partitionComplete && !partitionWarning.empty())
        {
            warning("repcut phase-d: " + partitionWarning);
        }

        uint32_t maxPartId = 0;
        std::unordered_map<uint32_t, size_t> partSizes;
        for (const uint32_t part : ascPartition)
        {
            maxPartId = std::max(maxPartId, part);
            partSizes[part] += 1;
        }

        std::ostringstream phaseDSummary;
        phaseDSummary << "repcut phase-d: graph=" << graph->symbol()
                      << " backend=" << partitionBackend->name()
                      << " hmetis=" << hmetisPath.string()
                      << " partition_file=" << (partitionOutPath.empty() ? "<none>" : partitionOutPath.string())
                      << " asc_count=" << ascPartition.size()
                      << " part_count_observed=" << (partSizes.empty() ? 0 : (maxPartId + 1))
                      << " partition_complete=" << (partitionComplete ? "true" : "false");
        const uint64_t phaseDMs = msSince(phaseDStart);
        phaseDSummary << " parse_partition_ms=" << parsePartMs
                      << " partition_run_ms=" << partitionRunMs
                      << " elapsed_ms=" << phaseDMs;
        logInfo(phaseDSummary.str());

        const auto phaseEStart = std::chrono::steady_clock::now();
        const auto phaseEMapStart = std::chrono::steady_clock::now();
        std::unordered_map<wolvrix::lib::grh::OperationId, uint32_t, wolvrix::lib::grh::OperationIdHash> opPartition;
        std::unordered_map<wolvrix::lib::grh::OperationId, PartitionSet, wolvrix::lib::grh::OperationIdHash>
            opPartitionSet;
        opPartition.reserve(data.nodeToOp.size());
        opPartitionSet.reserve(data.nodeToOp.size());

        auto assignOpPartition = [&](wolvrix::lib::grh::OperationId opId, uint32_t partId) {
            if (!opId.valid())
            {
                return;
            }
            opPartition.emplace(opId, partId);
            opPartitionSet[opId].add(partId);
        };

        for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
        {
            uint32_t partId = 0;
            if (aid < ascPartition.size())
            {
                partId = ascPartition[aid];
            }

            for (const size_t sinkIndex : phaseB.ascs[aid].sinks)
            {
                const SinkRef &sink = phaseB.sinks[sinkIndex];
                if (sink.kind == SinkRef::Kind::Operation && sink.op.valid())
                {
                    assignOpPartition(sink.op, partId);
                }
            }
            for (const NodeId node : phaseB.ascs[aid].combOps)
            {
                if (node < data.nodeToOp.size())
                {
                    assignOpPartition(data.nodeToOp[node], partId);
                }
            }
        }

        std::unordered_map<std::string, uint32_t> regPartition;
        std::unordered_map<std::string, uint32_t> latchPartition;
        std::unordered_map<std::string, uint32_t> memPartition;
        for (AscId aid = 0; aid < phaseB.ascs.size(); ++aid)
        {
            const uint32_t partId = (aid < ascPartition.size()) ? ascPartition[aid] : 0u;
            for (const size_t sinkIndex : phaseB.ascs[aid].sinks)
            {
                const SinkRef &sink = phaseB.sinks[sinkIndex];
                if (sink.kind != SinkRef::Kind::Operation)
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph->getOperation(sink.op);
                if (auto sym = getAttrString(op, "regSymbol"))
                {
                    regPartition[*sym] = partId;
                }
                else if (auto sym = getAttrString(op, "latchSymbol"))
                {
                    latchPartition[*sym] = partId;
                }
                else if (auto sym = getAttrString(op, "memSymbol"))
                {
                    memPartition[*sym] = partId;
                }
            }
        }

        std::unordered_map<std::string, StorageInfo> regInfos;
        std::unordered_map<std::string, StorageInfo> latchInfos;
        std::unordered_map<std::string, StorageInfo> memInfos;
        collectStorageInfos(*graph, regInfos, latchInfos, memInfos);

        const std::size_t inferredRegPartitionCount =
            inferMissingStoragePartitions(*graph, regInfos, regPartition, opPartitionSet);
        const std::size_t inferredLatchPartitionCount =
            inferMissingStoragePartitions(*graph, latchInfos, latchPartition, opPartitionSet);
        if (inferredRegPartitionCount > 0 || inferredLatchPartitionCount > 0)
        {
            logInfo("repcut phase-e: inferred storage partitions from read users regs=" +
                    std::to_string(inferredRegPartitionCount) +
                    " latches=" + std::to_string(inferredLatchPartitionCount));
        }

        assignStorageOpsToPartition(regInfos, regPartition, opPartition, opPartitionSet);
        assignStorageOpsToPartition(latchInfos, latchPartition, opPartition, opPartitionSet);
        assignStorageOpsToPartition(memInfos, memPartition, opPartition, opPartitionSet);
        const uint64_t phaseEMapMs = msSince(phaseEMapStart);

        std::unordered_map<wolvrix::lib::grh::ValueId, uint32_t, wolvrix::lib::grh::ValueIdHash> valueDefPartition;
        std::unordered_map<wolvrix::lib::grh::ValueId, PartitionSet, wolvrix::lib::grh::ValueIdHash> valueDefPartSet;
        valueDefPartition.reserve(graph->values().size());
        valueDefPartSet.reserve(graph->values().size());
        for (const auto &[opId, pid] : opPartition)
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            for (const auto resultValue : op.results())
            {
                valueDefPartition.emplace(resultValue, pid);
            }
        }
        for (const auto &[opId, partSet] : opPartitionSet)
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            for (const auto resultValue : op.results())
            {
                valueDefPartSet[resultValue].merge(partSet);
                if (valueDefPartition.find(resultValue) == valueDefPartition.end() && !partSet.empty())
                {
                    const std::optional<uint32_t> firstPart = partSet.first();
                    if (firstPart)
                    {
                        valueDefPartition.emplace(resultValue, *firstPart);
                    }
                }
            }
        }

        std::vector<CrossPartitionValue> crossValues;
        bool hasForbiddenCross = false;
        std::size_t forbiddenMemoryReadCrossCount = 0;
        std::size_t forbiddenCrossCount = 0;
        std::unordered_map<wolvrix::lib::grh::OperationKind, std::size_t> forbiddenCrossByDefKind;
        std::vector<ForbiddenCrossSample> forbiddenCrossSamples;
        forbiddenCrossSamples.reserve(kForbiddenCrossDiagLimit);
        const auto phaseECrossStart = std::chrono::steady_clock::now();

        for (const auto &[value, defPartSet] : valueDefPartSet)
        {
            uint32_t defPart = 0;
            auto itDefPart = valueDefPartition.find(value);
            if (itDefPart != valueDefPartition.end())
            {
                defPart = itDefPart->second;
            }
            else if (const std::optional<uint32_t> firstPart = defPartSet.first(); firstPart)
            {
                defPart = *firstPart;
            }

            const wolvrix::lib::grh::Value val = graph->getValue(value);
            const wolvrix::lib::grh::OperationId defOpId = val.definingOp();
            wolvrix::lib::grh::OperationKind defKind = wolvrix::lib::grh::OperationKind::kConstant;
            bool defKindKnown = false;
            bool defIsResultProducingEffect = false;
            if (defOpId.valid())
            {
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                defKind = defOp.kind();
                defIsResultProducingEffect = opProducesEffectResult(defOp);
                defKindKnown = true;
            }

            for (const auto &user : val.users())
            {
                auto itUserSet = opPartitionSet.find(user.operation);
                if (itUserSet == opPartitionSet.end() || itUserSet->second.empty())
                {
                    continue;
                }
                const PartitionSet &useSet = itUserSet->second;
                useSet.forEach([&](uint32_t usePart) {
                    if (defPartSet.contains(usePart))
                    {
                        return;
                    }

                    bool allowCross = false;
                    bool requiresPort = false;
                    if (defKindKnown)
                    {
                        if (defKind == wolvrix::lib::grh::OperationKind::kRegisterReadPort ||
                            defKind == wolvrix::lib::grh::OperationKind::kLatchReadPort)
                        {
                            allowCross = true;
                            requiresPort = true;
                        }
                        else if (defKind == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
                        {
                            allowCross = false;
                        }
                        else if (defKind == wolvrix::lib::grh::OperationKind::kConstant)
                        {
                            allowCross = true;
                            requiresPort = false;
                        }
                        else if (defIsResultProducingEffect)
                        {
                            allowCross = true;
                            requiresPort = true;
                        }
                        else
                        {
                            allowCross = false;
                        }
                    }
                    else if (val.isInput() || inoutInputValues.find(value) != inoutInputValues.end())
                    {
                        allowCross = true;
                        requiresPort = true;
                    }

                    if (!allowCross)
                    {
                        hasForbiddenCross = true;
                        ++forbiddenCrossCount;
                        if (defKindKnown)
                        {
                            forbiddenCrossByDefKind[defKind] += 1;
                        }
                        if (defKindKnown && defKind == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
                        {
                            ++forbiddenMemoryReadCrossCount;
                        }
                        if (forbiddenCrossSamples.size() < kForbiddenCrossDiagLimit)
                        {
                            ForbiddenCrossSample sample;
                            sample.value = value;
                            sample.defOp = defOpId;
                            sample.defKind = defKind;
                            sample.defKindKnown = defKindKnown;
                            sample.useOp = user.operation;
                            sample.srcPart = defPart;
                            sample.dstPart = usePart;
                            forbiddenCrossSamples.push_back(sample);
                        }
                    }

                    CrossPartitionValue cv;
                    cv.value = value;
                    cv.srcPart = defPart;
                    cv.dstPart = usePart;
                    cv.allowed = allowCross;
                    cv.requiresPort = requiresPort;
                    crossValues.push_back(cv);
                });
            }
        }
        const uint64_t phaseECrossMs = msSince(phaseECrossStart);

        std::unordered_map<wolvrix::lib::grh::ValueId, uint32_t, wolvrix::lib::grh::ValueIdHash> firstInputOwner;
        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> sourceInputValues =
            inoutInputValues;
        for (const auto &port : graph->inputPorts())
        {
            sourceInputValues.insert(port.value);
        }
        for (const auto valueId : sourceInputValues)
        {
            const wolvrix::lib::grh::Value val = graph->getValue(valueId);
            for (const auto &user : val.users())
            {
                auto itUserSet = opPartitionSet.find(user.operation);
                if (itUserSet == opPartitionSet.end() || itUserSet->second.empty())
                {
                    continue;
                }
                itUserSet->second.forEach([&](uint32_t usePart) {
                    auto [it, inserted] = firstInputOwner.emplace(valueId, usePart);
                    if (inserted || it->second == usePart)
                    {
                        return;
                    }

                    CrossPartitionValue cv;
                    cv.value = valueId;
                    cv.srcPart = it->second;
                    cv.dstPart = usePart;
                    cv.allowed = true;
                    cv.requiresPort = true;
                    crossValues.push_back(cv);
                });
            }
        }

        std::size_t partitionCount = options_.partitionCount;
        for (const auto &[opId, partSet] : opPartitionSet)
        {
            (void)opId;
            partSet.forEach([&](uint32_t partId) {
                partitionCount = std::max(partitionCount, static_cast<std::size_t>(partId) + 1);
            });
        }

        std::vector<std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>>
            partitionOps(partitionCount);
        for (const auto &[opId, partSet] : opPartitionSet)
        {
            partSet.forEach([&](uint32_t partId) {
                if (partId < partitionOps.size())
                {
                    partitionOps[partId].insert(opId);
                }
            });
        }

        if (hasForbiddenCross)
        {
            {
                std::ostringstream details;
                details << "repcut phase-e: forbidden_cross_total=" << forbiddenCrossCount
                        << " memory_read_forbidden=" << forbiddenMemoryReadCrossCount
                        << " sample_count=" << forbiddenCrossSamples.size();
                logInfo(details.str());
            }

            if (!forbiddenCrossByDefKind.empty())
            {
                std::vector<std::pair<wolvrix::lib::grh::OperationKind, std::size_t>> kindCounts;
                kindCounts.reserve(forbiddenCrossByDefKind.size());
                for (const auto &[kind, count] : forbiddenCrossByDefKind)
                {
                    kindCounts.emplace_back(kind, count);
                }
                std::sort(kindCounts.begin(),
                          kindCounts.end(),
                          [](const auto &lhs, const auto &rhs) {
                              if (lhs.second != rhs.second)
                              {
                                  return lhs.second > rhs.second;
                              }
                              return static_cast<int>(lhs.first) < static_cast<int>(rhs.first);
                          });
                std::ostringstream kinds;
                kinds << "repcut phase-e: forbidden_cross_def_kinds";
                const std::size_t limit = std::min<std::size_t>(5, kindCounts.size());
                for (std::size_t i = 0; i < limit; ++i)
                {
                    kinds << (i == 0 ? " " : ", ")
                          << wolvrix::lib::grh::toString(kindCounts[i].first)
                          << "=" << kindCounts[i].second;
                }
                logInfo(kinds.str());
            }

            for (std::size_t i = 0; i < forbiddenCrossSamples.size(); ++i)
            {
                const ForbiddenCrossSample &sample = forbiddenCrossSamples[i];
                const auto value = graph->getValue(sample.value);
                std::ostringstream detail;
                detail << "repcut phase-e forbidden[" << i << "]"
                       << " value=" << formatValueId(sample.value)
                       << " value_symbol=" << value.symbolText()
                       << " def_kind="
                       << (sample.defKindKnown ? std::string(wolvrix::lib::grh::toString(sample.defKind))
                                               : std::string("<unknown>"))
                       << " src_part=" << sample.srcPart
                       << " dst_part=" << sample.dstPart
                       << " def_op=" << formatOperationRef(*graph, sample.defOp)
                       << " use_op=" << formatOperationRef(*graph, sample.useOp);
                logInfo(detail.str());
            }

            if (forbiddenMemoryReadCrossCount > 0)
            {
                error("repcut phase-e: detected memory-read cross-partition usage (" +
                      std::to_string(forbiddenMemoryReadCrossCount) + ")");
            }
            else
            {
                error("repcut phase-e: detected forbidden cross-partition values");
            }
            result.failed = true;
            return result;
        }

        std::size_t crossAllowedCount = 0;
        std::size_t crossNeedsPortCount = 0;
        for (const auto &cv : crossValues)
        {
            if (cv.allowed)
            {
                ++crossAllowedCount;
            }
            if (cv.allowed && cv.requiresPort)
            {
                ++crossNeedsPortCount;
            }
        }

        std::ostringstream phaseESummary;
        phaseESummary << "repcut phase-e: graph=" << graph->symbol()
                      << " op_partitioned=" << opPartition.size()
                      << " reg_partitions=" << regPartition.size()
                      << " latch_partitions=" << latchPartition.size()
                      << " mem_partitions=" << memPartition.size()
                      << " cross_values_total=" << crossValues.size()
                      << " cross_values_allowed=" << crossAllowedCount
                      << " cross_values_need_ports=" << crossNeedsPortCount
                      << " map_storage_ms=" << phaseEMapMs
                      << " cross_scan_ms=" << phaseECrossMs;
        logInfo(phaseESummary.str());

        std::vector<std::size_t> partitionWeights(options_.partitionCount, 0);
        for (AscId aid = 0; aid < hg.nodeWeights.size(); ++aid)
        {
            const uint32_t partId = (aid < ascPartition.size()) ? ascPartition[aid] : 0u;
            if (partId >= partitionWeights.size())
            {
                partitionWeights.resize(static_cast<std::size_t>(partId) + 1u, 0);
            }
            partitionWeights[partId] += static_cast<std::size_t>(hg.nodeWeights[aid]);
        }
        const std::vector<PartitionStaticFeatureRecord> partitionFeatureRecords =
            buildPartitionStaticFeatureRecords(*graph, data, partitionOps, crossValues, partitionWeights);

        const auto phaseERebuildStart = std::chrono::steady_clock::now();
        logInfo("repcut phase-e rebuild: begin graph=" + graph->symbol() +
                " partition_count=" + std::to_string(partitionOps.size()) +
                " op_partitioned=" + std::to_string(opPartition.size()) +
                " cross_values_need_ports=" + std::to_string(crossNeedsPortCount));
        struct DefInfo
        {
            uint32_t owner = std::numeric_limits<uint32_t>::max();
            uint32_t count = 0;
        };

        const auto values = graph->values();
        std::unordered_map<wolvrix::lib::grh::ValueId, ValueInfo, wolvrix::lib::grh::ValueIdHash> valueInfos;
        valueInfos.reserve(values.size());
        for (const auto valueId : values)
        {
            valueInfos.emplace(valueId, captureValueInfo(*graph, valueId));
        }
        logInfo("repcut phase-e rebuild: captured value metadata values=" +
                std::to_string(valueInfos.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        for (const auto opId : graph->operations())
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            if (op.kind() != wolvrix::lib::grh::OperationKind::kConstant)
            {
                continue;
            }
            for (const auto resultValue : op.results())
            {
                const wolvrix::lib::grh::Value value = graph->getValue(resultValue);
                for (const auto &user : value.users())
                {
                    auto itUserSet = opPartitionSet.find(user.operation);
                    if (itUserSet == opPartitionSet.end() || itUserSet->second.empty())
                    {
                        continue;
                    }
                    itUserSet->second.forEach([&](uint32_t partId) {
                        if (partId >= partitionOps.size())
                        {
                            return;
                        }
                        partitionOps[partId].insert(opId);
                        assignOpPartition(opId, partId);
                    });
                }
            }
        }
        logInfo("repcut phase-e rebuild: constant partition propagation done elapsed_ms=" +
                std::to_string(msSince(phaseERebuildStart)));

        std::size_t dpicImportAssignments = 0;
        std::size_t dpicImportMisses = 0;
        for (const auto opId : graph->operations())
        {
            const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
            if (op.kind() != wolvrix::lib::grh::OperationKind::kDpicCall)
            {
                continue;
            }
            auto itSet = opPartitionSet.find(opId);
            if (itSet == opPartitionSet.end() || itSet->second.empty())
            {
                continue;
            }

            const wolvrix::lib::grh::OperationId importOpId = findMatchingDpicImport(*graph, op);
            if (!importOpId.valid())
            {
                ++dpicImportMisses;
                continue;
            }

            itSet->second.forEach([&](uint32_t partId) {
                if (partId >= partitionOps.size())
                {
                    return;
                }
                const auto [_, inserted] = partitionOps[partId].insert(importOpId);
                assignOpPartition(importOpId, partId);
                if (inserted)
                {
                    ++dpicImportAssignments;
                }
            });
        }
        logInfo("repcut phase-e rebuild: dpi import propagation done assigned=" +
                std::to_string(dpicImportAssignments) +
                " missing=" + std::to_string(dpicImportMisses) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        std::unordered_map<wolvrix::lib::grh::ValueId, DefInfo, wolvrix::lib::grh::ValueIdHash> defInfo;
        defInfo.reserve(values.size());
        for (std::size_t p = 0; p < partitionOps.size(); ++p)
        {
            for (const auto opId : partitionOps[p])
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                for (const auto resultValue : op.results())
                {
                    if (!resultValue.valid())
                    {
                        continue;
                    }
                    auto &info = defInfo[resultValue];
                    if (info.count == 0)
                    {
                        info.owner = static_cast<uint32_t>(p);
                        info.count = 1;
                    }
                    else if (info.owner != p)
                    {
                        info.owner = std::numeric_limits<uint32_t>::max();
                        info.count += 1;
                    }
                }
            }
        }
        logInfo("repcut phase-e rebuild: def ownership table ready defs=" +
                std::to_string(defInfo.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> origInputs;
        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> origOutputs;
        for (const auto &port : graph->inputPorts())
        {
            origInputs.insert(port.value);
        }
        for (const auto &port : graph->outputPorts())
        {
            origOutputs.insert(port.value);
        }
        for (const auto &port : graph->inoutPorts())
        {
            origInputs.insert(port.in);
            origOutputs.insert(port.out);
            origOutputs.insert(port.oe);
        }

        uint32_t maxValueIndex = 0;
        for (const auto valueId : values)
        {
            maxValueIndex = std::max(maxValueIndex, valueId.index);
        }
        std::vector<uint8_t> origInputFlags(static_cast<std::size_t>(maxValueIndex) + 1u, 0);
        for (const auto valueId : origInputs)
        {
            if (valueId.valid())
            {
                origInputFlags[static_cast<std::size_t>(valueId.index)] = 1u;
            }
        }

        auto isValueDefinedInPartition =
            [&](wolvrix::lib::grh::ValueId valueId, std::size_t partId) -> bool {
                if (!valueId.valid() || partId >= partitionOps.size())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOp = graph->valueDef(valueId);
                if (!defOp.valid())
                {
                    return false;
                }
                auto itSet = opPartitionSet.find(defOp);
                if (itSet == opPartitionSet.end())
                {
                    return false;
                }
                return itSet->second.contains(static_cast<uint32_t>(partId));
            };

        auto primaryOwnerOfValue =
            [&](wolvrix::lib::grh::ValueId valueId) -> std::optional<uint32_t> {
            auto it = defInfo.find(valueId);
            if (it != defInfo.end() &&
                it->second.owner != std::numeric_limits<uint32_t>::max())
            {
                return it->second.owner;
            }

            const wolvrix::lib::grh::OperationId defOp = graph->valueDef(valueId);
            if (!defOp.valid())
            {
                return std::nullopt;
            }
            auto itSet = opPartitionSet.find(defOp);
            if (itSet == opPartitionSet.end() || itSet->second.empty())
            {
                return std::nullopt;
            }
            return itSet->second.first();
        };

        std::size_t partitionOpMemberships = 0;
        for (const auto &opsInPart : partitionOps)
        {
            partitionOpMemberships += opsInPart.size();
        }

        std::vector<std::vector<wolvrix::lib::grh::ValueId>> partRequiredValueCandidates(partitionOps.size());
        std::vector<std::vector<wolvrix::lib::grh::ValueId>> partInputValueCandidates(partitionOps.size());
        std::vector<std::vector<wolvrix::lib::grh::ValueId>> partOutputValueCandidates(partitionOps.size());
        for (std::size_t p = 0; p < partitionOps.size(); ++p)
        {
            const std::size_t sourceOpCount = partitionOps[p].size();
            partRequiredValueCandidates[p].reserve(sourceOpCount * 2);
            partInputValueCandidates[p].reserve(sourceOpCount / 2 + 64);
            partOutputValueCandidates[p].reserve(sourceOpCount / 4 + 32);
        }
        auto resetStampVector = [](std::vector<uint32_t> &marks, uint32_t &stamp) {
            if (stamp == std::numeric_limits<uint32_t>::max())
            {
                std::fill(marks.begin(), marks.end(), 0u);
                stamp = 1u;
                return;
            }
            ++stamp;
        };
        auto appendUniqueValue = [](std::vector<wolvrix::lib::grh::ValueId> &list,
                                    std::vector<uint32_t> &marks,
                                    uint32_t stamp,
                                    wolvrix::lib::grh::ValueId valueId) {
            if (!valueId.valid())
            {
                return;
            }
            const std::size_t idx = static_cast<std::size_t>(valueId.index);
            if (idx >= marks.size() || marks[idx] == stamp)
            {
                return;
            }
            marks[idx] = stamp;
            list.push_back(valueId);
        };
        std::vector<uint32_t> scanRequiredValueMarks(static_cast<std::size_t>(maxValueIndex) + 1u, 0u);
        std::vector<uint32_t> scanInputValueMarks(static_cast<std::size_t>(maxValueIndex) + 1u, 0u);
        uint32_t scanRequiredValueStamp = 0;
        uint32_t scanInputValueStamp = 0;

        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> usedByOther;
        const std::size_t usedScanProgressEvery = 250000;
        std::size_t usedScanVisited = 0;
        const auto usedByOtherStart = std::chrono::steady_clock::now();
        for (std::size_t p = 0; p < partitionOps.size(); ++p)
        {
            resetStampVector(scanRequiredValueMarks, scanRequiredValueStamp);
            resetStampVector(scanInputValueMarks, scanInputValueStamp);
            for (const auto opId : partitionOps[p])
            {
                ++usedScanVisited;
                if (usedScanVisited > 0 && (usedScanVisited % usedScanProgressEvery) == 0)
                {
                    logInfo("repcut phase-e rebuild: used_by_other_scan_progress visited_ops=" +
                            std::to_string(usedScanVisited) + "/" + std::to_string(partitionOpMemberships) +
                            " current_part=" + std::to_string(p) +
                            " elapsed_ms=" + std::to_string(msSince(usedByOtherStart)));
                }
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                for (const auto operand : op.operands())
                {
                    if (!operand.valid())
                    {
                        continue;
                    }
                    appendUniqueValue(partRequiredValueCandidates[p],
                                      scanRequiredValueMarks,
                                      scanRequiredValueStamp,
                                      operand);
                    const bool isOrigInput =
                        operand.index < origInputFlags.size() &&
                        origInputFlags[static_cast<std::size_t>(operand.index)] != 0u;
                    if (isOrigInput)
                    {
                        appendUniqueValue(partInputValueCandidates[p],
                                          scanInputValueMarks,
                                          scanInputValueStamp,
                                          operand);
                        continue;
                    }
                    if (!isValueDefinedInPartition(operand, p))
                    {
                        appendUniqueValue(partInputValueCandidates[p],
                                          scanInputValueMarks,
                                          scanInputValueStamp,
                                          operand);
                        const wolvrix::lib::grh::OperationId operandDef = graph->valueDef(operand);
                        if (operandDef.valid())
                        {
                            usedByOther.insert(operand);
                        }
                    }
                }
                for (const auto resultValue : op.results())
                {
                    if (!resultValue.valid())
                    {
                        continue;
                    }
                    appendUniqueValue(partRequiredValueCandidates[p],
                                      scanRequiredValueMarks,
                                      scanRequiredValueStamp,
                                      resultValue);
                }
            }
        }
        for (const auto valueId : usedByOther)
        {
            const std::optional<uint32_t> owner = primaryOwnerOfValue(valueId);
            if (!owner || *owner >= partOutputValueCandidates.size())
            {
                continue;
            }
            partOutputValueCandidates[*owner].push_back(valueId);
        }
        for (const auto valueId : origOutputs)
        {
            const std::optional<uint32_t> owner = primaryOwnerOfValue(valueId);
            if (!owner || *owner >= partOutputValueCandidates.size())
            {
                continue;
            }
            partOutputValueCandidates[*owner].push_back(valueId);
        }
        logInfo("repcut phase-e rebuild: used_by_other_scan_done used_by_other_values=" +
                std::to_string(usedByOther.size()) +
                " elapsed_ms=" + std::to_string(msSince(usedByOtherStart)) +
                " total_elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> boundaryValues;
        boundaryValues.reserve(usedByOther.size() + origOutputs.size() + 1024);
        for (const auto valueId : usedByOther)
        {
            boundaryValues.insert(valueId);
        }
        for (const auto &valuesInPart : partInputValueCandidates)
        {
            for (const auto valueId : valuesInPart)
            {
                boundaryValues.insert(valueId);
            }
        }
        for (const auto &valuesInPart : partOutputValueCandidates)
        {
            for (const auto valueId : valuesInPart)
            {
                boundaryValues.insert(valueId);
            }
        }

        std::unordered_map<wolvrix::lib::grh::ValueId, uint32_t, wolvrix::lib::grh::ValueIdHash> ownerByValue;
        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::OperationId, wolvrix::lib::grh::ValueIdHash>
            defOpByValue;
        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::OperationKind, wolvrix::lib::grh::ValueIdHash>
            defKindByValue;
        ownerByValue.reserve(boundaryValues.size());
        defOpByValue.reserve(boundaryValues.size());
        defKindByValue.reserve(boundaryValues.size());
        for (const auto valueId : boundaryValues)
        {
            if (const std::optional<uint32_t> owner = primaryOwnerOfValue(valueId))
            {
                ownerByValue.emplace(valueId, *owner);
            }

            const wolvrix::lib::grh::OperationId defOp = graph->valueDef(valueId);
            if (!defOp.valid())
            {
                continue;
            }
            defOpByValue.emplace(valueId, defOp);
            defKindByValue.emplace(valueId, graph->getOperation(defOp).kind());
        }

        std::unordered_map<wolvrix::lib::grh::ValueId, PartitionSet, wolvrix::lib::grh::ValueIdHash> consumerPartsByValue;
        consumerPartsByValue.reserve(usedByOther.size() + 1024);
        for (std::size_t p = 0; p < partInputValueCandidates.size(); ++p)
        {
            for (const auto valueId : partInputValueCandidates[p])
            {
                if (origInputs.find(valueId) != origInputs.end())
                {
                    continue;
                }
                auto ownerIt = ownerByValue.find(valueId);
                if (ownerIt == ownerByValue.end() || ownerIt->second == p)
                {
                    continue;
                }
                consumerPartsByValue[valueId].add(static_cast<uint32_t>(p));
            }
        }

        std::unordered_map<BoundaryPortGroupKey,
                           std::vector<wolvrix::lib::grh::ValueId>,
                           BoundaryPortGroupKeyHash>
            boundaryGroupMembersByKey;
        std::unordered_map<BoundaryPortGroupKey,
                           std::vector<uint32_t>,
                           BoundaryPortGroupKeyHash>
            boundaryGroupDstPartsByKey;
        boundaryGroupMembersByKey.reserve(consumerPartsByValue.size());
        boundaryGroupDstPartsByKey.reserve(consumerPartsByValue.size());
        for (const auto &[valueId, dstSet] : consumerPartsByValue)
        {
            const std::vector<uint32_t> dstParts = collectPartitionSetParts(dstSet);
            if (dstParts.empty())
            {
                continue;
            }
            auto ownerIt = ownerByValue.find(valueId);
            if (ownerIt == ownerByValue.end())
            {
                continue;
            }
            BoundaryPortGroupKey key;
            key.ownerPart = ownerIt->second;
            key.dstSignature = formatPartitionSignature(dstParts);
            boundaryGroupMembersByKey[key].push_back(valueId);
            boundaryGroupDstPartsByKey.emplace(key, dstParts);
        }

        std::vector<BoundaryPortGroup> boundaryGroups;
        boundaryGroups.reserve(boundaryGroupMembersByKey.size());
        for (auto &[key, members] : boundaryGroupMembersByKey)
        {
            if (members.size() < 2)
            {
                continue;
            }
            std::sort(members.begin(), members.end(),
                      [](wolvrix::lib::grh::ValueId lhs, wolvrix::lib::grh::ValueId rhs) {
                          return lhs.index < rhs.index;
                      });
            BoundaryPortGroup group;
            group.ownerPart = key.ownerPart;
            group.dstSignature = key.dstSignature;
            auto dstIt = boundaryGroupDstPartsByKey.find(key);
            if (dstIt != boundaryGroupDstPartsByKey.end())
            {
                group.dstParts = dstIt->second;
            }
            group.members = std::move(members);
            for (const auto valueId : group.members)
            {
                auto vinfoIt = valueInfos.find(valueId);
                if (vinfoIt == valueInfos.end())
                {
                    continue;
                }
                group.totalWidth += static_cast<int64_t>(vinfoIt->second.width);
            }
            boundaryGroups.push_back(std::move(group));
        }
        std::sort(boundaryGroups.begin(),
                  boundaryGroups.end(),
                  [](const BoundaryPortGroup &lhs, const BoundaryPortGroup &rhs) {
                      if (lhs.ownerPart != rhs.ownerPart)
                      {
                          return lhs.ownerPart < rhs.ownerPart;
                      }
                      if (lhs.dstSignature != rhs.dstSignature)
                      {
                          return lhs.dstSignature < rhs.dstSignature;
                      }
                      const uint32_t lhsFirst =
                          lhs.members.empty() ? std::numeric_limits<uint32_t>::max() : lhs.members.front().index;
                      const uint32_t rhsFirst =
                          rhs.members.empty() ? std::numeric_limits<uint32_t>::max() : rhs.members.front().index;
                      return lhsFirst < rhsFirst;
                  });

        std::unordered_map<wolvrix::lib::grh::ValueId, BoundaryPortGroupMemberInfo, wolvrix::lib::grh::ValueIdHash>
            groupedValueMembers;
        groupedValueMembers.reserve(boundaryValues.size());
        std::vector<std::vector<std::size_t>> partInputGroups(partitionOps.size());
        std::vector<std::vector<std::size_t>> partOutputGroups(partitionOps.size());
        std::size_t groupedValueCount = 0;
        for (std::size_t groupIndex = 0; groupIndex < boundaryGroups.size(); ++groupIndex)
        {
            const BoundaryPortGroup &group = boundaryGroups[groupIndex];
            if (group.ownerPart < partOutputGroups.size())
            {
                partOutputGroups[group.ownerPart].push_back(groupIndex);
            }
            for (const uint32_t dstPart : group.dstParts)
            {
                if (dstPart < partInputGroups.size())
                {
                    partInputGroups[dstPart].push_back(groupIndex);
                }
            }

            int64_t bitCursor = group.totalWidth;
            for (const auto valueId : group.members)
            {
                auto vinfoIt = valueInfos.find(valueId);
                if (vinfoIt == valueInfos.end())
                {
                    continue;
                }
                const int64_t width = static_cast<int64_t>(vinfoIt->second.width);
                if (width <= 0 || bitCursor < width)
                {
                    continue;
                }
                BoundaryPortGroupMemberInfo info;
                info.groupIndex = groupIndex;
                info.sliceStart = bitCursor - width;
                info.sliceEnd = bitCursor - 1;
                groupedValueMembers.emplace(valueId, info);
                bitCursor -= width;
                ++groupedValueCount;
            }
        }
        logInfo("repcut phase-e rebuild: grouped boundary bundles groups=" +
                std::to_string(boundaryGroups.size()) +
                " values=" + std::to_string(groupedValueCount) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        struct PartitionGraphInfo
        {
            wolvrix::lib::grh::Graph *graph = nullptr;
            std::string name;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> inputPortByValue;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> outputPortByValue;
            std::unordered_map<std::size_t, std::string> inputPortByGroup;
            std::unordered_map<std::size_t, std::string> outputPortByGroup;
        };

        std::vector<PartitionGraphInfo> partInfos;
        partInfos.reserve(partitionOps.size());
        logInfo("repcut phase-e rebuild: begin partition graph cloning partition_count=" +
                std::to_string(partitionOps.size()));
        std::unordered_set<uint32_t> sourceDeclaredSymbols;
        sourceDeclaredSymbols.reserve(graph->declaredSymbols().size());
        for (const auto sym : graph->declaredSymbols())
        {
            sourceDeclaredSymbols.insert(sym.value);
        }
        std::vector<uint32_t> requiredValueMarks(static_cast<std::size_t>(maxValueIndex) + 1u, 0u);
        std::vector<uint32_t> inputValueMarks(static_cast<std::size_t>(maxValueIndex) + 1u, 0u);
        std::vector<uint32_t> outputValueMarks(static_cast<std::size_t>(maxValueIndex) + 1u, 0u);
        uint32_t requiredValueStamp = 0;
        uint32_t inputValueStamp = 0;
        uint32_t outputValueStamp = 0;

        for (std::size_t p = 0; p < partitionOps.size(); ++p)
        {
            const auto partBuildStart = std::chrono::steady_clock::now();
            logInfo("repcut phase-e rebuild: partition_clone_begin index=" +
                    std::to_string(p + 1) + "/" + std::to_string(partitionOps.size()) +
                    " source_ops=" + std::to_string(partitionOps[p].size()));
            const auto &sourceOpsInPart = partitionOps[p];
            std::vector<wolvrix::lib::grh::OperationId> sourceOps;
            sourceOps.reserve(sourceOpsInPart.size());
            for (const auto opId : sourceOpsInPart)
            {
                sourceOps.push_back(opId);
            }
            std::sort(sourceOps.begin(), sourceOps.end(),
                      [](wolvrix::lib::grh::OperationId lhs, wolvrix::lib::grh::OperationId rhs) {
                          return lhs.index < rhs.index;
                      });

            resetStampVector(requiredValueMarks, requiredValueStamp);
            resetStampVector(inputValueMarks, inputValueStamp);
            resetStampVector(outputValueMarks, outputValueStamp);

            std::vector<wolvrix::lib::grh::ValueId> sourceValues;
            sourceValues.reserve(partRequiredValueCandidates[p].size() +
                                 partInputValueCandidates[p].size() +
                                 partOutputValueCandidates[p].size());
            std::vector<wolvrix::lib::grh::ValueId> inputList;
            inputList.reserve(partInputValueCandidates[p].size());
            std::vector<wolvrix::lib::grh::ValueId> outputList;
            outputList.reserve(partOutputValueCandidates[p].size());
            std::vector<std::size_t> inputGroupIds = partInputGroups[p];
            std::vector<std::size_t> outputGroupIds = partOutputGroups[p];
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> groupedInputValues;
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> groupedOutputValues;
            for (const std::size_t groupId : inputGroupIds)
            {
                if (groupId >= boundaryGroups.size())
                {
                    continue;
                }
                for (const auto valueId : boundaryGroups[groupId].members)
                {
                    groupedInputValues.insert(valueId);
                }
            }
            for (const std::size_t groupId : outputGroupIds)
            {
                if (groupId >= boundaryGroups.size())
                {
                    continue;
                }
                for (const auto valueId : boundaryGroups[groupId].members)
                {
                    groupedOutputValues.insert(valueId);
                }
            }

            for (const auto valueId : partRequiredValueCandidates[p])
            {
                if (groupedInputValues.find(valueId) != groupedInputValues.end())
                {
                    continue;
                }
                appendUniqueValue(sourceValues, requiredValueMarks, requiredValueStamp, valueId);
            }
            for (const auto valueId : partInputValueCandidates[p])
            {
                if (groupedInputValues.find(valueId) == groupedInputValues.end())
                {
                    appendUniqueValue(inputList, inputValueMarks, inputValueStamp, valueId);
                    appendUniqueValue(sourceValues, requiredValueMarks, requiredValueStamp, valueId);
                }
            }
            for (const auto valueId : partOutputValueCandidates[p])
            {
                if (groupedOutputValues.find(valueId) == groupedOutputValues.end())
                {
                    appendUniqueValue(outputList, outputValueMarks, outputValueStamp, valueId);
                }
                appendUniqueValue(sourceValues, requiredValueMarks, requiredValueStamp, valueId);
            }

            std::sort(sourceValues.begin(), sourceValues.end(),
                      [](wolvrix::lib::grh::ValueId lhs, wolvrix::lib::grh::ValueId rhs) {
                          return lhs.index < rhs.index;
                      });
            std::sort(inputList.begin(), inputList.end(),
                      [](wolvrix::lib::grh::ValueId lhs, wolvrix::lib::grh::ValueId rhs) {
                          return lhs.index < rhs.index;
                      });
            std::sort(outputList.begin(), outputList.end(),
                      [](wolvrix::lib::grh::ValueId lhs, wolvrix::lib::grh::ValueId rhs) {
                          return lhs.index < rhs.index;
                      });

            const std::string partName =
                uniqueGraphName(design(), graph->symbol() + "_repcut_part" + std::to_string(p));
            wolvrix::lib::grh::Graph &partGraph = design().createGraph(partName);
            const std::size_t symbolReserveCount = sourceValues.size() + sourceOps.size() + 32;
            partGraph.reserveSymbolCapacity(symbolReserveCount);
            partGraph.reserveDeclaredSymbolCapacity(symbolReserveCount);
            partGraph.reserveValueCapacity(sourceValues.size());
            partGraph.reserveOperationCapacity(sourceOps.size());

            std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
                valueMap;
            valueMap.reserve(sourceValues.size());
            std::unordered_map<uint32_t, wolvrix::lib::grh::SymbolId> symbolMap;
            symbolMap.reserve(symbolReserveCount);

            auto mapSymbol = [&](wolvrix::lib::grh::SymbolId srcSym) -> wolvrix::lib::grh::SymbolId {
                if (!srcSym.valid())
                {
                    return wolvrix::lib::grh::SymbolId::invalid();
                }
                auto it = symbolMap.find(srcSym.value);
                if (it != symbolMap.end())
                {
                    return it->second;
                }
                const std::string_view text = graph->symbolText(srcSym);
                if (text.empty())
                {
                    throw std::runtime_error("repcut phase-e rebuild: source symbol text is empty");
                }
                wolvrix::lib::grh::SymbolId dstSym = partGraph.lookupSymbol(text);
                if (!dstSym.valid())
                {
                    dstSym = partGraph.internSymbol(text);
                }
                if (!dstSym.valid())
                {
                    throw std::runtime_error("repcut phase-e rebuild: failed to intern symbol " + std::string(text));
                }
                if (sourceDeclaredSymbols.find(srcSym.value) != sourceDeclaredSymbols.end())
                {
                    partGraph.addDeclaredSymbol(dstSym);
                }
                symbolMap.emplace(srcSym.value, dstSym);
                return dstSym;
            };

            for (const auto sourceValueId : sourceValues)
            {
                const wolvrix::lib::grh::Value srcValue = graph->getValue(sourceValueId);
                const wolvrix::lib::grh::SymbolId dstSym = mapSymbol(srcValue.symbol());
                const wolvrix::lib::grh::ValueId dstValue =
                    partGraph.createValue(dstSym, srcValue.width(), srcValue.isSigned(), srcValue.type());
                if (srcValue.srcLoc())
                {
                    partGraph.setValueSrcLoc(dstValue, *srcValue.srcLoc());
                }
                valueMap.emplace(sourceValueId, dstValue);
            }

            PartitionGraphInfo info;
            info.graph = &partGraph;
            info.name = partName;

            std::unordered_set<std::string> usedPortNames;

            for (const std::size_t groupId : inputGroupIds)
            {
                if (groupId >= boundaryGroups.size())
                {
                    continue;
                }
                const BoundaryPortGroup &group = boundaryGroups[groupId];
                if (group.totalWidth <= 0 || group.totalWidth > std::numeric_limits<int32_t>::max())
                {
                    error("repcut phase-e rebuild: invalid grouped input width group=" + std::to_string(groupId) +
                          " width=" + std::to_string(group.totalWidth));
                    result.failed = true;
                    return result;
                }

                wolvrix::lib::grh::ValueId bundleValue = wolvrix::lib::grh::ValueId::invalid();
                if (group.members.size() == 1)
                {
                    const auto valueId = group.members.front();
                    const auto vinfoIt = valueInfos.find(valueId);
                    if (vinfoIt == valueInfos.end())
                    {
                        continue;
                    }
                    const ValueInfo &vinfo = vinfoIt->second;
                    const std::string bundleBase = "repcut_in_bundle_" + std::to_string(group.ownerPart) +
                                                   "_to_" + group.dstSignature;
                    const auto sym = internUniqueSymbol(partGraph, bundleBase);
                    bundleValue = partGraph.createValue(sym, vinfo.width, vinfo.isSigned, vinfo.type);
                    if (vinfo.srcLoc)
                    {
                        partGraph.setValueSrcLoc(bundleValue, *vinfo.srcLoc);
                    }
                    valueMap[valueId] = bundleValue;
                }
                else
                {
                    const std::string bundleBase = "repcut_in_bundle_" + std::to_string(group.ownerPart) +
                                                   "_to_" + group.dstSignature;
                    const auto sym = internUniqueSymbol(partGraph, bundleBase);
                    bundleValue = partGraph.createValue(sym,
                                                        static_cast<int32_t>(group.totalWidth),
                                                        false,
                                                        wolvrix::lib::grh::ValueType::Logic);
                    for (const auto valueId : group.members)
                    {
                        auto memberIt = groupedValueMembers.find(valueId);
                        auto vinfoIt = valueInfos.find(valueId);
                        if (memberIt == groupedValueMembers.end() || vinfoIt == valueInfos.end())
                        {
                            continue;
                        }
                        const BoundaryPortGroupMemberInfo &memberInfo = memberIt->second;
                        const ValueInfo &vinfo = vinfoIt->second;
                        const auto sliceSym = internUniqueSymbol(
                            partGraph,
                            "repcut_in_slice_" + normalizePortBase(vinfo.symbol,
                                                                   std::string("value") + std::to_string(valueId.index)));
                        const auto sliceOp =
                            partGraph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, sliceSym);
                        partGraph.addOperand(sliceOp, bundleValue);
                        partGraph.setAttr(sliceOp, "sliceStart", memberInfo.sliceStart);
                        partGraph.setAttr(sliceOp, "sliceEnd", memberInfo.sliceEnd);
                        const auto memberSym = mapSymbol(graph->getValue(valueId).symbol());
                        const auto memberValue =
                            partGraph.createValue(memberSym, vinfo.width, vinfo.isSigned, vinfo.type);
                        if (vinfo.srcLoc)
                        {
                            partGraph.setValueSrcLoc(memberValue, *vinfo.srcLoc);
                        }
                        partGraph.addResult(sliceOp, memberValue);
                        valueMap[valueId] = memberValue;
                    }
                }

                const std::string portName =
                    uniquePortName(usedPortNames,
                                   "in_bundle_from_p" + std::to_string(group.ownerPart) + "_to_" + group.dstSignature);
                partGraph.bindInputPort(portName, bundleValue);
                info.inputPortByGroup.emplace(groupId, portName);
            }

            for (const auto sourceOpId : sourceOps)
            {
                const wolvrix::lib::grh::Operation srcOp = graph->getOperation(sourceOpId);
                const wolvrix::lib::grh::SymbolId dstSym = mapSymbol(srcOp.symbol());
                const wolvrix::lib::grh::OperationId dstOp = partGraph.createOperation(srcOp.kind(), dstSym);
                partGraph.reserveOpOperandCapacity(dstOp, srcOp.operands().size());
                partGraph.reserveOpResultCapacity(dstOp, srcOp.results().size());
                partGraph.reserveOpAttrCapacity(dstOp, srcOp.attrs().size());
                for (const auto &attr : srcOp.attrs())
                {
                    partGraph.setAttr(dstOp, attr.key, attr.value);
                }
                for (const auto operand : srcOp.operands())
                {
                    auto it = valueMap.find(operand);
                    if (it == valueMap.end())
                    {
                        error("repcut phase-e rebuild: missing operand clone for op " +
                              formatOperationRef(*graph, sourceOpId) +
                              " operand_value=" + std::to_string(operand.index));
                        result.failed = true;
                        return result;
                    }
                    partGraph.addOperand(dstOp, it->second);
                }
                for (const auto resultValue : srcOp.results())
                {
                    auto it = valueMap.find(resultValue);
                    if (it == valueMap.end())
                    {
                        error("repcut phase-e rebuild: missing result clone for op " +
                              formatOperationRef(*graph, sourceOpId) +
                              " result_value=" + std::to_string(resultValue.index));
                        result.failed = true;
                        return result;
                    }
                    partGraph.addResult(dstOp, it->second);
                }
                if (srcOp.srcLoc())
                {
                    partGraph.setOpSrcLoc(dstOp, *srcOp.srcLoc());
                }
            }

            for (const auto valueId : inputList)
            {
                auto it = valueMap.find(valueId);
                if (it == valueMap.end())
                {
                    error("repcut phase-e rebuild: missing input-port clone value=" + std::to_string(valueId.index));
                    result.failed = true;
                    return result;
                }
                const auto vinfoIt = valueInfos.find(valueId);
                if (vinfoIt == valueInfos.end())
                {
                    continue;
                }
                const ValueInfo &vinfo = vinfoIt->second;
                const std::string fallback = std::string("value") + std::to_string(valueId.index);
                const std::string base = normalizePortBase(vinfo.symbol, fallback);
                const std::string portName = uniquePortName(usedPortNames, "in_" + base);
                partGraph.bindInputPort(portName, it->second);
                info.inputPortByValue.emplace(valueId, portName);
            }

            for (const auto valueId : outputList)
            {
                auto it = valueMap.find(valueId);
                if (it == valueMap.end())
                {
                    error("repcut phase-e rebuild: missing output-port clone value=" + std::to_string(valueId.index));
                    result.failed = true;
                    return result;
                }
                const auto vinfoIt = valueInfos.find(valueId);
                if (vinfoIt == valueInfos.end())
                {
                    continue;
                }
                const ValueInfo &vinfo = vinfoIt->second;
                const std::string fallback = std::string("value") + std::to_string(valueId.index);
                const std::string base = normalizePortBase(vinfo.symbol, fallback);
                const std::string portName = uniquePortName(usedPortNames, "out_" + base);
                partGraph.bindOutputPort(portName, it->second);
                info.outputPortByValue.emplace(valueId, portName);
            }

            for (const std::size_t groupId : outputGroupIds)
            {
                if (groupId >= boundaryGroups.size())
                {
                    continue;
                }
                const BoundaryPortGroup &group = boundaryGroups[groupId];
                if (group.totalWidth <= 0 || group.totalWidth > std::numeric_limits<int32_t>::max())
                {
                    error("repcut phase-e rebuild: invalid grouped output width group=" + std::to_string(groupId) +
                          " width=" + std::to_string(group.totalWidth));
                    result.failed = true;
                    return result;
                }

                wolvrix::lib::grh::ValueId bundleValue = wolvrix::lib::grh::ValueId::invalid();
                if (group.members.size() == 1)
                {
                    auto it = valueMap.find(group.members.front());
                    if (it == valueMap.end())
                    {
                        error("repcut phase-e rebuild: missing grouped output member clone value=" +
                              std::to_string(group.members.front().index));
                        result.failed = true;
                        return result;
                    }
                    bundleValue = it->second;
                }
                else
                {
                    const auto concatSym =
                        internUniqueSymbol(partGraph,
                                           "repcut_out_bundle_" + std::to_string(group.ownerPart) +
                                               "_to_" + group.dstSignature);
                    const auto concatOp =
                        partGraph.createOperation(wolvrix::lib::grh::OperationKind::kConcat, concatSym);
                    for (const auto valueId : group.members)
                    {
                        auto it = valueMap.find(valueId);
                        if (it == valueMap.end())
                        {
                            error("repcut phase-e rebuild: missing grouped output member clone value=" +
                                  std::to_string(valueId.index));
                            result.failed = true;
                            return result;
                        }
                        partGraph.addOperand(concatOp, it->second);
                    }
                    const auto bundleSym = internUniqueSymbol(partGraph,
                                                              "repcut_out_bundle_val_" + std::to_string(group.ownerPart) +
                                                                  "_to_" + group.dstSignature);
                    bundleValue = partGraph.createValue(bundleSym,
                                                        static_cast<int32_t>(group.totalWidth),
                                                        false,
                                                        wolvrix::lib::grh::ValueType::Logic);
                    partGraph.addResult(concatOp, bundleValue);
                }

                const std::string portName =
                    uniquePortName(usedPortNames, "out_bundle_to_" + group.dstSignature);
                partGraph.bindOutputPort(portName, bundleValue);
                info.outputPortByGroup.emplace(groupId, portName);
            }

            partInfos.push_back(std::move(info));
            logInfo("repcut phase-e rebuild: partition_clone_done index=" +
                    std::to_string(p + 1) + "/" + std::to_string(partitionOps.size()) +
                    " graph=" + partName +
                    " source_values=" + std::to_string(sourceValues.size()) +
                    " source_ops=" + std::to_string(sourceOps.size()) +
                    " input_ports=" +
                    std::to_string(partInfos.back().inputPortByValue.size() + partInfos.back().inputPortByGroup.size()) +
                    " output_ports=" +
                    std::to_string(partInfos.back().outputPortByValue.size() + partInfos.back().outputPortByGroup.size()) +
                    " elapsed_ms=" + std::to_string(msSince(partBuildStart)) +
                    " total_elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));
        }
        logInfo("repcut phase-e rebuild: partition graph cloning done part_graphs=" +
                std::to_string(partInfos.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));
        logInfo("repcut phase-e rebuild: boundary metadata captured values=" +
                std::to_string(boundaryValues.size()) +
                " owner_values=" + std::to_string(ownerByValue.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        struct PortSnapshot
        {
            std::string name;
            wolvrix::lib::grh::ValueId value;
            ValueInfo info;
        };
        struct InoutSnapshot
        {
            std::string name;
            wolvrix::lib::grh::ValueId in;
            wolvrix::lib::grh::ValueId out;
            wolvrix::lib::grh::ValueId oe;
            ValueInfo infoIn;
            ValueInfo infoOut;
            ValueInfo infoOe;
        };

        std::vector<PortSnapshot> inputSnapshot;
        std::vector<PortSnapshot> outputSnapshot;
        std::vector<InoutSnapshot> inoutSnapshot;
        inputSnapshot.reserve(graph->inputPorts().size());
        outputSnapshot.reserve(graph->outputPorts().size());
        inoutSnapshot.reserve(graph->inoutPorts().size());

        for (const auto &port : graph->inputPorts())
        {
            inputSnapshot.push_back(PortSnapshot{port.name, port.value, captureValueInfo(*graph, port.value)});
        }
        for (const auto &port : graph->outputPorts())
        {
            outputSnapshot.push_back(PortSnapshot{port.name, port.value, captureValueInfo(*graph, port.value)});
        }
        for (const auto &port : graph->inoutPorts())
        {
            InoutSnapshot snap;
            snap.name = port.name;
            snap.in = port.in;
            snap.out = port.out;
            snap.oe = port.oe;
            snap.infoIn = captureValueInfo(*graph, port.in);
            snap.infoOut = captureValueInfo(*graph, port.out);
            snap.infoOe = captureValueInfo(*graph, port.oe);
            inoutSnapshot.push_back(std::move(snap));
        }
        logInfo("repcut phase-e rebuild: top port snapshot captured inputs=" +
                std::to_string(inputSnapshot.size()) +
                " outputs=" + std::to_string(outputSnapshot.size()) +
                " inouts=" + std::to_string(inoutSnapshot.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));
        const std::size_t origTopOpsCount = graph->operations().size();
        const std::size_t origTopValuesCount = graph->values().size();
        const std::size_t origTopInputsCount = inputSnapshot.size();
        const std::size_t origTopOutputsCount = outputSnapshot.size();
        const std::size_t origTopInoutsCount = inoutSnapshot.size();

        std::vector<std::string> topAliases = design().aliasesForGraph(graph->symbol());
        bool wasTop = false;
        for (const auto &topName : design().topGraphs())
        {
            if (topName == graph->symbol())
            {
                wasTop = true;
                break;
            }
        }

        const std::string topName = graph->symbol();
        logInfo("repcut phase-e rebuild: rebuilding top graph graph=" + topName +
                " aliases=" + std::to_string(topAliases.size()) +
                " was_top=" + std::string(wasTop ? "true" : "false"));
        design().deleteGraph(topName);
        wolvrix::lib::grh::Graph &newTop = design().createGraph(topName);

        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
            topValueBySource;

        for (const auto &port : inputSnapshot)
        {
            const std::string base = normalizePortBase(port.info.symbol, "in");
            const wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(newTop, base);
            const wolvrix::lib::grh::ValueId value =
                newTop.createValue(sym, port.info.width, port.info.isSigned, port.info.type);
            if (port.info.srcLoc)
            {
                newTop.setValueSrcLoc(value, *port.info.srcLoc);
            }
            newTop.bindInputPort(port.name, value);
            topValueBySource.emplace(port.value, value);
        }

        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
            linkValues;
        std::unordered_map<std::size_t, wolvrix::lib::grh::ValueId> groupLinkValues;
        groupLinkValues.reserve(boundaryGroups.size());
        for (std::size_t groupId = 0; groupId < boundaryGroups.size(); ++groupId)
        {
            const BoundaryPortGroup &group = boundaryGroups[groupId];
            if (group.members.empty() || group.totalWidth <= 0 || group.totalWidth > std::numeric_limits<int32_t>::max())
            {
                error("repcut phase-e rebuild: invalid grouped top link width group=" + std::to_string(groupId) +
                      " width=" + std::to_string(group.totalWidth));
                result.failed = true;
                return result;
            }

            wolvrix::lib::grh::ValueId linkVal = wolvrix::lib::grh::ValueId::invalid();
            if (group.members.size() == 1)
            {
                const auto valueId = group.members.front();
                auto vinfoIt = valueInfos.find(valueId);
                if (vinfoIt == valueInfos.end())
                {
                    continue;
                }
                const ValueInfo &vinfo = vinfoIt->second;
                const auto sym =
                    internUniqueSymbol(newTop,
                                       "repcut_link_bundle_p" + std::to_string(group.ownerPart) +
                                           "_to_" + group.dstSignature);
                linkVal = newTop.createValue(sym, vinfo.width, vinfo.isSigned, vinfo.type);
                if (vinfo.srcLoc)
                {
                    newTop.setValueSrcLoc(linkVal, *vinfo.srcLoc);
                }
            }
            else
            {
                const auto sym =
                    internUniqueSymbol(newTop,
                                       "repcut_link_bundle_p" + std::to_string(group.ownerPart) +
                                           "_to_" + group.dstSignature);
                linkVal = newTop.createValue(sym,
                                             static_cast<int32_t>(group.totalWidth),
                                             false,
                                             wolvrix::lib::grh::ValueType::Logic);
            }
            groupLinkValues.emplace(groupId, linkVal);
        }
        for (const auto valueId : usedByOther)
        {
            if (groupedValueMembers.find(valueId) != groupedValueMembers.end())
            {
                continue;
            }
            if (ownerByValue.find(valueId) == ownerByValue.end())
            {
                continue;
            }
            auto vinfoIt = valueInfos.find(valueId);
            if (vinfoIt == valueInfos.end())
            {
                continue;
            }
            const ValueInfo &vinfo = vinfoIt->second;
            const std::string base = normalizePortBase(vinfo.symbol, "link");
            const wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(newTop, "repcut_link_" + base);
            const wolvrix::lib::grh::ValueId linkVal =
                newTop.createValue(sym, vinfo.width, vinfo.isSigned, vinfo.type);
            if (vinfo.srcLoc)
            {
                newTop.setValueSrcLoc(linkVal, *vinfo.srcLoc);
            }
            linkValues.emplace(valueId, linkVal);
        }
        logInfo("repcut phase-e rebuild: top link values created cross_links=" +
                std::to_string(linkValues.size()) +
                " grouped_links=" + std::to_string(groupLinkValues.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
            groupedTopSlices;
        auto getTopValueForSource =
            [&](wolvrix::lib::grh::ValueId sourceValue) -> std::optional<wolvrix::lib::grh::ValueId> {
            if (auto it = topValueBySource.find(sourceValue); it != topValueBySource.end())
            {
                return it->second;
            }
            if (auto it = groupedTopSlices.find(sourceValue); it != groupedTopSlices.end())
            {
                return it->second;
            }
            if (auto memberIt = groupedValueMembers.find(sourceValue); memberIt != groupedValueMembers.end())
            {
                const BoundaryPortGroupMemberInfo &memberInfo = memberIt->second;
                if (memberInfo.groupIndex >= boundaryGroups.size())
                {
                    return std::nullopt;
                }
                auto linkIt = groupLinkValues.find(memberInfo.groupIndex);
                if (linkIt == groupLinkValues.end())
                {
                    return std::nullopt;
                }
                const BoundaryPortGroup &group = boundaryGroups[memberInfo.groupIndex];
                wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
                if (group.members.size() == 1)
                {
                    value = linkIt->second;
                }
                else
                {
                    auto vinfoIt = valueInfos.find(sourceValue);
                    if (vinfoIt == valueInfos.end())
                    {
                        return std::nullopt;
                    }
                    const ValueInfo &vinfo = vinfoIt->second;
                    const auto sliceSym = internUniqueSymbol(
                        newTop,
                        "repcut_link_slice_" +
                            normalizePortBase(vinfo.symbol, std::string("value") + std::to_string(sourceValue.index)));
                    const auto sliceOp =
                        newTop.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, sliceSym);
                    newTop.addOperand(sliceOp, linkIt->second);
                    newTop.setAttr(sliceOp, "sliceStart", memberInfo.sliceStart);
                    newTop.setAttr(sliceOp, "sliceEnd", memberInfo.sliceEnd);
                    const auto resultSym = internUniqueSymbol(
                        newTop,
                        "repcut_link_value_" +
                            normalizePortBase(vinfo.symbol, std::string("value") + std::to_string(sourceValue.index)));
                    value = newTop.createValue(resultSym, vinfo.width, vinfo.isSigned, vinfo.type);
                    if (vinfo.srcLoc)
                    {
                        newTop.setValueSrcLoc(value, *vinfo.srcLoc);
                    }
                    newTop.addResult(sliceOp, value);
                }
                groupedTopSlices.emplace(sourceValue, value);
                topValueBySource.emplace(sourceValue, value);
                return value;
            }
            if (auto it = linkValues.find(sourceValue); it != linkValues.end())
            {
                return it->second;
            }
            return std::nullopt;
        };

        for (const auto &port : outputSnapshot)
        {
            if (auto mapped = getTopValueForSource(port.value))
            {
                newTop.bindOutputPort(port.name, *mapped);
                topValueBySource.emplace(port.value, *mapped);
                continue;
            }
            auto itTop = topValueBySource.find(port.value);
            if (itTop != topValueBySource.end())
            {
                newTop.bindOutputPort(port.name, itTop->second);
                continue;
            }

            const std::string base = normalizePortBase(port.info.symbol, "out");
            const wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(newTop, base);
            const wolvrix::lib::grh::ValueId value =
                newTop.createValue(sym, port.info.width, port.info.isSigned, port.info.type);
            if (port.info.srcLoc)
            {
                newTop.setValueSrcLoc(value, *port.info.srcLoc);
            }
            newTop.bindOutputPort(port.name, value);
            topValueBySource.emplace(port.value, value);
        }

        for (const auto &port : inoutSnapshot)
        {
            const wolvrix::lib::grh::SymbolId inSym = internUniqueSymbol(newTop, normalizePortBase(port.infoIn.symbol, "in"));
            const wolvrix::lib::grh::ValueId inVal =
                newTop.createValue(inSym, port.infoIn.width, port.infoIn.isSigned, port.infoIn.type);
            if (port.infoIn.srcLoc)
            {
                newTop.setValueSrcLoc(inVal, *port.infoIn.srcLoc);
            }

            wolvrix::lib::grh::ValueId outVal;
            if (auto mapped = getTopValueForSource(port.out))
            {
                outVal = *mapped;
            }
            else
            {
                const wolvrix::lib::grh::SymbolId outSym = internUniqueSymbol(newTop, normalizePortBase(port.infoOut.symbol, "out"));
                outVal = newTop.createValue(outSym, port.infoOut.width, port.infoOut.isSigned, port.infoOut.type);
                if (port.infoOut.srcLoc)
                {
                    newTop.setValueSrcLoc(outVal, *port.infoOut.srcLoc);
                }
            }

            wolvrix::lib::grh::ValueId oeVal;
            if (auto mapped = getTopValueForSource(port.oe))
            {
                oeVal = *mapped;
            }
            else
            {
                const wolvrix::lib::grh::SymbolId oeSym = internUniqueSymbol(newTop, normalizePortBase(port.infoOe.symbol, "oe"));
                oeVal = newTop.createValue(oeSym, port.infoOe.width, port.infoOe.isSigned, port.infoOe.type);
                if (port.infoOe.srcLoc)
                {
                    newTop.setValueSrcLoc(oeVal, *port.infoOe.srcLoc);
                }
            }

            newTop.bindInoutPort(port.name, inVal, outVal, oeVal);
            topValueBySource.emplace(port.in, inVal);
            topValueBySource.emplace(port.out, outVal);
            topValueBySource.emplace(port.oe, oeVal);
        }

        std::unordered_map<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash>
            undrivenTopValues;
        auto mapUndrivenInputToTop =
            [&](wolvrix::lib::grh::ValueId sourceValue) -> std::optional<wolvrix::lib::grh::ValueId> {
            if (auto it = undrivenTopValues.find(sourceValue); it != undrivenTopValues.end())
            {
                return it->second;
            }
            if (defOpByValue.find(sourceValue) != defOpByValue.end())
            {
                return std::nullopt;
            }
            auto infoIt = valueInfos.find(sourceValue);
            if (infoIt == valueInfos.end())
            {
                return std::nullopt;
            }

            const ValueInfo &vinfo = infoIt->second;
            const std::string base = normalizePortBase(vinfo.symbol, "undriven");
            const wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(newTop, "repcut_undriven_" + base);
            const wolvrix::lib::grh::ValueId value = newTop.createValue(sym, vinfo.width, vinfo.isSigned, vinfo.type);
            if (vinfo.srcLoc)
            {
                newTop.setValueSrcLoc(value, *vinfo.srcLoc);
            }
            undrivenTopValues.emplace(sourceValue, value);
            topValueBySource.emplace(sourceValue, value);
            return value;
        };

        for (std::size_t p = 0; p < partInfos.size(); ++p)
        {
            PartitionGraphInfo &part = partInfos[p];
            if (!part.graph)
            {
                continue;
            }

            std::unordered_map<std::string, wolvrix::lib::grh::ValueId> inputMapping;
            std::unordered_map<std::string, wolvrix::lib::grh::ValueId> outputMapping;

            for (const auto &[sourceValue, portName] : part.inputPortByValue)
            {
                if (auto mapped = getTopValueForSource(sourceValue))
                {
                    inputMapping.emplace(portName, *mapped);
                    continue;
                }
                if (auto undrivenTop = mapUndrivenInputToTop(sourceValue))
                {
                    inputMapping.emplace(portName, *undrivenTop);
                }
            }
            for (const auto &[groupId, portName] : part.inputPortByGroup)
            {
                auto it = groupLinkValues.find(groupId);
                if (it != groupLinkValues.end())
                {
                    inputMapping.emplace(portName, it->second);
                }
            }

            for (const auto &[sourceValue, portName] : part.outputPortByValue)
            {
                if (auto mapped = getTopValueForSource(sourceValue))
                {
                    outputMapping.emplace(portName, *mapped);
                }
            }
            for (const auto &[groupId, portName] : part.outputPortByGroup)
            {
                auto it = groupLinkValues.find(groupId);
                if (it != groupLinkValues.end())
                {
                    outputMapping.emplace(portName, it->second);
                }
            }

            if (inputMapping.size() != part.graph->inputPorts().size())
            {
                constexpr std::size_t kMissingPortDiagLimit = 8;
                std::size_t missingInputCount = 0;
                for (const auto &[groupId, portName] : part.inputPortByGroup)
                {
                    if (inputMapping.find(portName) != inputMapping.end())
                    {
                        continue;
                    }
                    ++missingInputCount;
                    if (missingInputCount > kMissingPortDiagLimit)
                    {
                        continue;
                    }
                    if (groupId < boundaryGroups.size())
                    {
                        const BoundaryPortGroup &group = boundaryGroups[groupId];
                        error("repcut phase-e rebuild: missing grouped input port mapping part=" + part.graph->symbol() +
                              " port=" + portName +
                              " owner_part=" + std::to_string(group.ownerPart) +
                              " dsts=" + group.dstSignature);
                    }
                }
                for (const auto &[sourceValue, portName] : part.inputPortByValue)
                {
                    if (inputMapping.find(portName) != inputMapping.end())
                    {
                        continue;
                    }
                    ++missingInputCount;
                    if (missingInputCount > kMissingPortDiagLimit)
                    {
                        continue;
                    }

                    const bool hasTopValue = topValueBySource.find(sourceValue) != topValueBySource.end();
                    const bool hasLinkValue = linkValues.find(sourceValue) != linkValues.end();
                    const bool inUsedByOther = usedByOther.find(sourceValue) != usedByOther.end();
                    const bool isOrigInput = origInputs.find(sourceValue) != origInputs.end();
                    const bool isOrigOutput = origOutputs.find(sourceValue) != origOutputs.end();
                    auto ownerIt = ownerByValue.find(sourceValue);
                    const std::optional<uint32_t> owner =
                        (ownerIt == ownerByValue.end()) ? std::nullopt : std::optional<uint32_t>(ownerIt->second);
                    wolvrix::lib::grh::OperationId defOp = wolvrix::lib::grh::OperationId::invalid();
                    if (auto defOpIt = defOpByValue.find(sourceValue); defOpIt != defOpByValue.end())
                    {
                        defOp = defOpIt->second;
                    }
                    std::string defKindText = "<none>";
                    if (auto defKindIt = defKindByValue.find(sourceValue); defKindIt != defKindByValue.end())
                    {
                        defKindText = std::string(wolvrix::lib::grh::toString(defKindIt->second));
                    }
                    const ValueInfo &vinfo = valueInfos.at(sourceValue);

                    std::ostringstream detail;
                    detail << "repcut phase-e rebuild: missing input port mapping"
                           << " part=" << part.graph->symbol()
                           << " port=" << portName
                           << " source_value=" << formatValueId(sourceValue)
                           << " source_symbol=" << vinfo.symbol
                           << " def_op_id=" << (defOp.valid() ? std::to_string(defOp.index) : std::string("<none>"))
                           << " def_kind=" << defKindText
                           << " owner=" << (owner ? std::to_string(*owner) : std::string("<none>"))
                           << " has_top_value=" << (hasTopValue ? "true" : "false")
                           << " has_link_value=" << (hasLinkValue ? "true" : "false")
                           << " used_by_other=" << (inUsedByOther ? "true" : "false")
                           << " orig_input=" << (isOrigInput ? "true" : "false")
                           << " orig_output=" << (isOrigOutput ? "true" : "false");
                    error(detail.str());
                }
                if (missingInputCount > kMissingPortDiagLimit)
                {
                    warning("repcut phase-e rebuild: additional missing input-port mappings omitted count=" +
                            std::to_string(missingInputCount - kMissingPortDiagLimit));
                }
                error("repcut phase-e: incomplete instance input mapping for " + part.graph->symbol() +
                      " mapped=" + std::to_string(inputMapping.size()) +
                      " expected=" + std::to_string(part.graph->inputPorts().size()));
                result.failed = true;
                return result;
            }
            if (outputMapping.size() != part.graph->outputPorts().size())
            {
                constexpr std::size_t kMissingPortDiagLimit = 8;
                std::size_t missingOutputCount = 0;
                for (const auto &[groupId, portName] : part.outputPortByGroup)
                {
                    if (outputMapping.find(portName) != outputMapping.end())
                    {
                        continue;
                    }
                    ++missingOutputCount;
                    if (missingOutputCount > kMissingPortDiagLimit)
                    {
                        continue;
                    }
                    if (groupId < boundaryGroups.size())
                    {
                        const BoundaryPortGroup &group = boundaryGroups[groupId];
                        error("repcut phase-e rebuild: missing grouped output port mapping part=" + part.graph->symbol() +
                              " port=" + portName +
                              " owner_part=" + std::to_string(group.ownerPart) +
                              " dsts=" + group.dstSignature);
                    }
                }
                for (const auto &[sourceValue, portName] : part.outputPortByValue)
                {
                    if (outputMapping.find(portName) != outputMapping.end())
                    {
                        continue;
                    }
                    ++missingOutputCount;
                    if (missingOutputCount > kMissingPortDiagLimit)
                    {
                        continue;
                    }

                    const bool hasTopValue = topValueBySource.find(sourceValue) != topValueBySource.end();
                    const bool hasLinkValue = linkValues.find(sourceValue) != linkValues.end();
                    const bool inUsedByOther = usedByOther.find(sourceValue) != usedByOther.end();
                    const bool isOrigInput = origInputs.find(sourceValue) != origInputs.end();
                    const bool isOrigOutput = origOutputs.find(sourceValue) != origOutputs.end();
                    auto ownerIt = ownerByValue.find(sourceValue);
                    const std::optional<uint32_t> owner =
                        (ownerIt == ownerByValue.end()) ? std::nullopt : std::optional<uint32_t>(ownerIt->second);
                    wolvrix::lib::grh::OperationId defOp = wolvrix::lib::grh::OperationId::invalid();
                    if (auto defOpIt = defOpByValue.find(sourceValue); defOpIt != defOpByValue.end())
                    {
                        defOp = defOpIt->second;
                    }
                    std::string defKindText = "<none>";
                    if (auto defKindIt = defKindByValue.find(sourceValue); defKindIt != defKindByValue.end())
                    {
                        defKindText = std::string(wolvrix::lib::grh::toString(defKindIt->second));
                    }
                    const ValueInfo &vinfo = valueInfos.at(sourceValue);

                    std::ostringstream detail;
                    detail << "repcut phase-e rebuild: missing output port mapping"
                           << " part=" << part.graph->symbol()
                           << " port=" << portName
                           << " source_value=" << formatValueId(sourceValue)
                           << " source_symbol=" << vinfo.symbol
                           << " def_op_id=" << (defOp.valid() ? std::to_string(defOp.index) : std::string("<none>"))
                           << " def_kind=" << defKindText
                           << " owner=" << (owner ? std::to_string(*owner) : std::string("<none>"))
                           << " has_top_value=" << (hasTopValue ? "true" : "false")
                           << " has_link_value=" << (hasLinkValue ? "true" : "false")
                           << " used_by_other=" << (inUsedByOther ? "true" : "false")
                           << " orig_input=" << (isOrigInput ? "true" : "false")
                           << " orig_output=" << (isOrigOutput ? "true" : "false");
                    error(detail.str());
                }
                if (missingOutputCount > kMissingPortDiagLimit)
                {
                    warning("repcut phase-e rebuild: additional missing output-port mappings omitted count=" +
                            std::to_string(missingOutputCount - kMissingPortDiagLimit));
                }
                error("repcut phase-e: incomplete instance output mapping for " + part.graph->symbol() +
                      " mapped=" + std::to_string(outputMapping.size()) +
                      " expected=" + std::to_string(part.graph->outputPorts().size()));
                result.failed = true;
                return result;
            }

            buildInstance(newTop, part.graph->symbol(), "part_" + std::to_string(p), *part.graph, inputMapping, outputMapping);
            if (((p + 1) % 8) == 0 || (p + 1) == partInfos.size())
            {
                logInfo("repcut phase-e rebuild: instance_wiring_progress done=" +
                        std::to_string(p + 1) + "/" + std::to_string(partInfos.size()) +
                        " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));
            }
        }
        if (!undrivenTopValues.empty())
        {
            logInfo("repcut phase-e rebuild: synthesized undriven top values count=" +
                    std::to_string(undrivenTopValues.size()));
        }
        logInfo("repcut phase-e rebuild: instance wiring done instances=" +
                std::to_string(partInfos.size()) +
                " elapsed_ms=" + std::to_string(msSince(phaseERebuildStart)));

        for (const auto &alias : topAliases)
        {
            design().registerGraphAlias(alias, newTop);
        }
        if (wasTop)
        {
            design().markAsTop(topName);
        }

        result.changed = true;

        std::ostringstream phaseEReconstructSummary;
        phaseEReconstructSummary << "repcut phase-e reconstruct: graph=" << topName
                                 << " partition_graphs=" << partInfos.size()
                                 << " cross_links=" << linkValues.size()
                                 << " grouped_links=" << groupLinkValues.size()
                                 << " grouped_values=" << groupedValueCount
                                 << " rebuild_ms=" << msSince(phaseERebuildStart);
        logInfo(phaseEReconstructSummary.str());
        const uint64_t phaseEMs = msSince(phaseEStart);

        if (!options_.keepIntermediateFiles)
        {
            std::error_code cleanupError;
            std::filesystem::remove(hmetisPath, cleanupError);
            cleanupError.clear();
            std::filesystem::remove(partitionPath, cleanupError);
        }

        std::size_t partitionWeightSum = 0;
        std::size_t partitionWeightMax = 0;
        std::size_t weightedPartitionCount = 0;
        for (const std::size_t partWeight : partitionWeights)
        {
            partitionWeightSum += partWeight;
            partitionWeightMax = std::max(partitionWeightMax, partWeight);
            weightedPartitionCount += 1;
        }
        const double partitionWeightAvg =
            (weightedPartitionCount == 0)
                ? 0.0
                : static_cast<double>(partitionWeightSum) / static_cast<double>(weightedPartitionCount);
        const double originalOverMaxWeightRatio =
            (partitionWeightMax == 0) ? 0.0 : static_cast<double>(partitionWeightSum) / static_cast<double>(partitionWeightMax);
        const double originalOverAvgWeightRatio =
            (partitionWeightAvg <= 0.0) ? 0.0 : static_cast<double>(partitionWeightSum) / partitionWeightAvg;
        const double maxWeightFractionOfOriginal =
            (partitionWeightSum == 0) ? 0.0 : static_cast<double>(partitionWeightMax) / static_cast<double>(partitionWeightSum);
        const double avgWeightFractionOfOriginal =
            (partitionWeightSum == 0) ? 0.0 : partitionWeightAvg / static_cast<double>(partitionWeightSum);
        std::ostringstream stats;
        std::size_t partitionOpsSum = 0;
        std::size_t partitionOpsMax = 0;
        std::size_t partitionGraphCount = 0;
        for (const auto &part : partInfos)
        {
            if (part.graph == nullptr)
            {
                continue;
            }
            const std::size_t partOps = part.graph->operations().size();
            partitionOpsSum += partOps;
            partitionOpsMax = std::max(partitionOpsMax, partOps);
            partitionGraphCount += 1;
        }
        const double partitionOpsAvg =
            (partitionGraphCount == 0) ? 0.0 : static_cast<double>(partitionOpsSum) / static_cast<double>(partitionGraphCount);
        const int64_t partitionOpsDelta =
            static_cast<int64_t>(partitionOpsSum) - static_cast<int64_t>(origTopOpsCount);
        const double origOverMaxOpsRatio =
            (partitionOpsMax == 0) ? 0.0 : static_cast<double>(origTopOpsCount) / static_cast<double>(partitionOpsMax);
        const double origOverAvgOpsRatio =
            (partitionOpsAvg <= 0.0) ? 0.0 : static_cast<double>(origTopOpsCount) / partitionOpsAvg;

        const auto appendGraphStatsJson = [&](std::ostringstream &oss,
                                              std::string_view key,
                                              std::size_t ops,
                                              std::size_t values,
                                              std::size_t inputs,
                                              std::size_t outputs,
                                              std::size_t inouts) {
            oss << ",\"" << key << "\":{"
                << "\"ops\":" << ops
                << ",\"values\":" << values
                << ",\"inputs\":" << inputs
                << ",\"outputs\":" << outputs
                << ",\"inouts\":" << inouts
                << "}";
        };
        stats << "{"
              << "\"pass\":\"repcut\""
              << ",\"graph\":\"" << escapeJson(topName) << "\""
              << ",\"partition_count_requested\":" << options_.partitionCount
              << ",\"partition_count_observed\":" << partInfos.size()
              << ",\"asc_count\":" << phaseB.ascs.size()
              << ",\"piece_count\":" << phaseB.pieces.size()
              << ",\"hyper_edge_count\":" << hg.edges.size()
              << ",\"cross_values_total\":" << crossValues.size()
              << ",\"cross_values_need_ports\":" << crossNeedsPortCount
              << ",\"cross_links\":" << linkValues.size()
              << ",\"time_ms_total\":" << msSince(totalStart)
              << ",\"time_ms_phase_a\":" << phaseAMs
              << ",\"time_ms_phase_b\":" << phaseBMs
              << ",\"time_ms_phase_c\":" << phaseCMs
              << ",\"time_ms_phase_d\":" << phaseDMs
              << ",\"time_ms_phase_e\":" << phaseEMs
              << ",\"op_partition_stats\":{"
              << "\"original_ops\":" << origTopOpsCount
              << ",\"partition_count\":" << partitionGraphCount
              << ",\"max_partition_ops\":" << partitionOpsMax
              << ",\"avg_partition_ops\":" << toFixedString(partitionOpsAvg, 6)
              << ",\"partition_ops_sum\":" << partitionOpsSum
              << ",\"partition_ops_delta\":" << partitionOpsDelta
              << ",\"original_over_max_ops_ratio\":" << toFixedString(origOverMaxOpsRatio, 6)
              << ",\"original_over_avg_ops_ratio\":" << toFixedString(origOverAvgOpsRatio, 6)
              << "}"
              << ",\"weight_partition_stats\":{"
              << "\"original_total_weight\":" << partitionWeightSum
              << ",\"partition_count\":" << weightedPartitionCount
              << ",\"max_partition_weight\":" << partitionWeightMax
              << ",\"avg_partition_weight\":" << toFixedString(partitionWeightAvg, 6)
              << ",\"partition_weight_sum\":" << partitionWeightSum
              << ",\"original_over_max_weight_ratio\":" << toFixedString(originalOverMaxWeightRatio, 6)
              << ",\"original_over_avg_weight_ratio\":" << toFixedString(originalOverAvgWeightRatio, 6)
              << ",\"max_partition_weight_fraction_of_original\":" << toFixedString(maxWeightFractionOfOriginal, 6)
              << ",\"avg_partition_weight_fraction_of_original\":" << toFixedString(avgWeightFractionOfOriginal, 6)
              << "}";
        appendGraphStatsJson(
            stats,
            "original_top_graph_stats",
            origTopOpsCount,
            origTopValuesCount,
            origTopInputsCount,
            origTopOutputsCount,
            origTopInoutsCount);
        stats << ",\"partition_graph_stats\":[";
        bool firstPartitionStats = true;
        for (std::size_t i = 0; i < partInfos.size(); ++i)
        {
            const PartitionGraphInfo &part = partInfos[i];
            if (part.graph == nullptr)
            {
                continue;
            }
            if (!firstPartitionStats)
            {
                stats << ",";
            }
            stats << "{"
                  << "\"index\":" << i
                  << ",\"graph\":\"" << escapeJson(part.graph->symbol()) << "\""
                  << ",\"weight\":" << (i < partitionWeights.size() ? partitionWeights[i] : 0u)
                  << ",\"ops\":" << part.graph->operations().size()
                  << ",\"values\":" << part.graph->values().size()
                  << ",\"inputs\":" << part.graph->inputPorts().size()
                  << ",\"outputs\":" << part.graph->outputPorts().size()
                  << ",\"inouts\":" << part.graph->inoutPorts().size()
                  << "}";
            firstPartitionStats = false;
        }
        stats << "]";
        stats << ",\"partition_static_features\":[";
        bool firstPartitionFeature = true;
        for (std::size_t i = 0; i < partitionFeatureRecords.size(); ++i)
        {
            if (i >= partInfos.size() || partInfos[i].graph == nullptr)
            {
                continue;
            }
            const auto &record = partitionFeatureRecords[i];
            if (!firstPartitionFeature)
            {
                stats << ",";
            }
            stats << "{"
                  << "\"part_id\":" << i
                  << ",\"part_name\":\"part_" << i << "\""
                  << ",\"partition_graph_name\":\"" << escapeJson(partInfos[i].graph->symbol()) << "\""
                  << ",\"op_count\":" << record.opCount
                  << ",\"comb_op_count\":" << record.combOpCount
                  << ",\"sink_op_count\":" << record.sinkOpCount
                  << ",\"source_op_count\":" << record.sourceOpCount
                  << ",\"phase_a_node_count\":" << record.phaseANodeCount
                  << ",\"non_phase_a_op_count\":" << record.nonPhaseAOpCount
                  << ",\"operand_count\":" << record.operandCount
                  << ",\"result_count\":" << record.resultCount
                  << ",\"operand_word_count\":" << record.operandWordCount
                  << ",\"result_word_count\":" << record.resultWordCount
                  << ",\"fanout_count\":" << record.fanoutCount
                  << ",\"estimated_node_weight_sum\":" << record.estimatedNodeWeightSum
                  << ",\"hyper_partition_weight\":" << record.hyperNodeWeight
                  << ",\"cross_in_value_count\":" << record.crossInValueCount
                  << ",\"cross_out_value_count\":" << record.crossOutValueCount
                  << ",\"cross_in_word_count\":" << record.crossInWordCount
                  << ",\"cross_out_word_count\":" << record.crossOutWordCount
                  << ",\"max_op_width\":" << record.maxOpWidth
                  << ",\"width_bucket_counts\":{";
            for (std::size_t bucket = 0; bucket < record.widthBucketCounts.size(); ++bucket)
            {
                if (bucket != 0)
                {
                    stats << ",";
                }
                stats << "\"" << widthBucketName(bucket) << "\":" << record.widthBucketCounts[bucket];
            }
            stats << "},\"op_kind_counts\":{";
            std::vector<std::pair<std::string, std::size_t>> kindCounts(
                record.opKindCounts.begin(), record.opKindCounts.end());
            std::sort(kindCounts.begin(), kindCounts.end(),
                      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
            for (std::size_t k = 0; k < kindCounts.size(); ++k)
            {
                if (k != 0)
                {
                    stats << ",";
                }
                stats << "\"" << escapeJson(kindCounts[k].first) << "\":" << kindCounts[k].second;
            }
            stats << "}}";
            firstPartitionFeature = false;
        }
        stats << "]}";
        const std::string statsMessage = stats.str();
        info(statsMessage);
        result.artifacts.push_back(statsMessage);
        logInfo("repcut: completed in " + std::to_string(msSince(totalStart)) + "ms");
        logInfo("repcut timing breakdown(ms): phase_a=" + std::to_string(phaseAMs) +
            " phase_b=" + std::to_string(phaseBMs) +
            " phase_c=" + std::to_string(phaseCMs) +
            " phase_d=" + std::to_string(phaseDMs) +
            " phase_e=" + std::to_string(phaseEMs));

        if (verbosity() == PassVerbosity::Debug)
        {
            debug("repcut phase-a: M0 index/adjacency ready");
            debug("repcut phase-a: M1 sink/source/comb classification ready");
            debug("repcut phase-b: ASC and piece initialization ready");
            debug("repcut phase-c: node/piece weights and hypergraph ready");
            debug("repcut phase-d: hmetis generation and mt-kahypar partitioning ready");
            debug("repcut phase-e: op partition mapping and cross-partition checks ready");
        }

        return result;
    }

} // namespace wolvrix::lib::transform
