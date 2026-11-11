#include "transform/split_static_array.hpp"

#include "core/grh.hpp"
#include "core/state.hpp"
#include "ingest/node_attributes.hpp"
#include "constant_index.hpp"
#include "slang/numeric/SVInt.h"
#include "slang/util/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::LogicalType;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::SemanticId;
        using wolvrix::lib::grh::SymbolId;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;
        using wolvrix::lib::grh::ValueType;

        struct SliceGeometry
        {
            int64_t offset = 0;
            int64_t width = 0;
        };

        struct SelectionKey
        {
            ValueId base;
            int64_t offset = 0;
            int64_t width = 0;
            LogicalType type;

            bool operator==(const SelectionKey &) const = default;
        };

        struct SelectionKeyHash
        {
            std::size_t operator()(const SelectionKey &key) const noexcept
            {
                std::size_t seed = ValueIdHash{}(key.base);
                const auto mix = [&seed](std::size_t value) {
                    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                };
                mix(static_cast<std::size_t>(key.offset));
                mix(static_cast<std::size_t>(key.width));
                mix(static_cast<std::size_t>(key.type.kind));
                mix(static_cast<std::size_t>(key.type.width));
                mix(static_cast<std::size_t>(key.type.isSigned));
                mix(static_cast<std::size_t>(key.type.domain));
                for (const int64_t dim : key.type.shape)
                {
                    mix(static_cast<std::size_t>(dim));
                }
                return seed;
            }
        };

        std::optional<SliceGeometry> sliceGeometry(const Graph &graph, OperationId opId)
        {
            const auto operands = graph.opOperands(opId);
            const auto results = graph.opResults(opId);
            if (graph.opKind(opId) != OperationKind::kSliceArray || operands.size() < 2 ||
                results.size() != 1)
            {
                return std::nullopt;
            }
            const ValueId base = operands[0];
            const auto shape = graph.valueShape(base);
            const int64_t baseWidth = graph.valueWidth(base);
            const std::size_t indexCount = operands.size() - 1;
            if (shape.empty() || baseWidth <= 0 || indexCount > shape.size())
            {
                return std::nullopt;
            }
            int64_t elementCount = 1;
            for (const int64_t dim : shape)
            {
                if (dim <= 0 || elementCount > std::numeric_limits<int64_t>::max() / dim)
                {
                    return std::nullopt;
                }
                elementCount *= dim;
            }
            if (baseWidth % elementCount != 0)
            {
                return std::nullopt;
            }
            int64_t width = baseWidth / elementCount;
            for (std::size_t i = indexCount; i < shape.size(); ++i)
            {
                if (width > std::numeric_limits<int64_t>::max() / shape[i])
                {
                    return std::nullopt;
                }
                width *= shape[i];
            }
            if (width <= 0 || graph.valueWidth(results[0]) != width)
            {
                return std::nullopt;
            }
            const auto resultShape = graph.valueShape(results[0]);
            if (!std::equal(resultShape.begin(), resultShape.end(), shape.begin() + indexCount,
                            shape.end()))
            {
                return std::nullopt;
            }

            int64_t linearIndex = 0;
            for (std::size_t i = 0; i < indexCount; ++i)
            {
                const auto index = detail::constantIndex(graph, operands[i + 1]);
                if (!index || *index >= static_cast<uint64_t>(shape[i]))
                {
                    return std::nullopt;
                }
                linearIndex = linearIndex * shape[i] + static_cast<int64_t>(*index);
            }
            if (linearIndex > (std::numeric_limits<int64_t>::max() - width + 1) / width)
            {
                return std::nullopt;
            }
            const int64_t offset = linearIndex * width;
            if (offset < 0 || offset > baseWidth - width)
            {
                return std::nullopt;
            }
            return SliceGeometry{offset, width};
        }

        std::optional<SliceGeometry> staticSliceGeometry(const Graph &graph, OperationId opId)
        {
            const auto operands = graph.opOperands(opId);
            const auto results = graph.opResults(opId);
            if (graph.opKind(opId) != OperationKind::kSliceStatic || operands.size() != 1 ||
                results.size() != 1)
            {
                return std::nullopt;
            }
            std::optional<int64_t> start;
            std::optional<int64_t> end;
            for (const auto &attribute : graph.opAttrRange(opId))
            {
                if (attribute.key() == "sliceStart") start = attribute.value().get<int64_t>();
                if (attribute.key() == "sliceEnd") end = attribute.value().get<int64_t>();
            }
            if (!start || !end || *start < 0 || *end < *start ||
                *end >= graph.valueWidth(operands[0]))
            {
                return std::nullopt;
            }
            const int64_t width = *end - *start + 1;
            if (graph.valueWidth(results[0]) != width)
            {
                return std::nullopt;
            }
            return SliceGeometry{*start, width};
        }

        void copyOperationMetadata(Graph &graph, OperationId target,
                                   const wolvrix::lib::grh::Operation &source,
                                   std::span<const SemanticId> semantics)
        {
            for (const auto &attribute : source.attrs())
            {
                graph.setAttr(target, attribute.key, attribute.value);
            }
            if (source.srcLoc())
            {
                graph.setOpSrcLoc(target, *source.srcLoc());
            }
            graph.setOpSemanticIds(target, semantics);
        }

        void replaceOperationOperands(Graph &graph, OperationId op, std::span<const ValueId> operands)
        {
            const std::vector<ValueId> replacement(operands.begin(), operands.end());
            while (!graph.opOperands(op).empty())
            {
                graph.eraseOperand(op, 0);
            }
            for (const ValueId operand : replacement)
            {
                graph.addOperand(op, operand);
            }
        }

        void mergeRewriteMetadata(Graph &graph, OperationId target, OperationId source,
                                  OperationId producer)
        {
            inheritOrigin(graph, target, {source, producer});
            auto attributes = ingest::node::read(graph, target);
            if (source.valid() && source != target)
            {
                attributes = ingest::node::unite(std::move(attributes), ingest::node::read(graph, source),
                                                 ingest::node::MergeReason::SameComputation);
            }
            if (producer.valid() && producer != target)
            {
                attributes = ingest::node::unite(std::move(attributes), ingest::node::read(graph, producer),
                                                 ingest::node::MergeReason::SameComputation);
            }
            ingest::node::write(graph, target, std::move(attributes));

            if (source.valid() && source != target)
            {
                auto semantics = graph.opSemanticIds(source);
                if (semantics.empty() && producer.valid())
                {
                    semantics = graph.opSemanticIds(producer);
                }
                graph.setOpSemanticIds(target, semantics);
            }
        }

        class GraphSplitter
        {
        public:
            GraphSplitter(Graph &graph, SplitStaticArrayPass::Stats &stats)
                : graph_(graph), stats_(stats)
            {
            }

            bool run()
            {
                std::vector<OperationId> operations(graph_.operations().begin(), graph_.operations().end());
                for (std::size_t index = 0; index < operations.size(); ++index)
                {
                    const OperationId opId = operations[index];
                    const bool arraySlice = graph_.opKind(opId) == OperationKind::kSliceArray;
                    const bool staticSlice = requestedStaticSlices_.contains(opId) &&
                        graph_.opKind(opId) == OperationKind::kSliceStatic;
                    if (!arraySlice && !staticSlice)
                    {
                        continue;
                    }
                    if (arraySlice)
                    {
                        ++stats_.slicesVisited;
                    }
                    else
                    {
                        ++stats_.staticSlicesVisited;
                    }
                    const auto geometry = arraySlice ? sliceGeometry(graph_, opId) : staticSliceGeometry(graph_, opId);
                    if (!geometry)
                    {
                        if (arraySlice)
                        {
                            ++stats_.dynamicSlicesSkipped;
                        }
                        else
                        {
                            ++stats_.unsupportedSlicesSkipped;
                        }
                        continue;
                    }
                    const ValueId base = graph_.opOperands(opId)[0];
                    const ValueId result = graph_.opResults(opId)[0];
                    if (graph_.valueIsInput(base) || graph_.valueIsOutput(base) || graph_.valueIsInout(base))
                    {
                        ++stats_.unsupportedSlicesSkipped;
                        continue;
                    }
                    SelectionKey key{base, geometry->offset, geometry->width,
                                     graph_.valueLogicalType(result)};
                    if (const auto found = cache_.find(key); found != cache_.end() && found->second == result)
                    {
                        cache_.erase(found);
                    }
                    const std::size_t createdCheckpoint = createdOps_.size();
                    const ValueId selected = select(key, opId, 0);
                    operations.insert(operations.end(), createdOps_.begin() + createdCheckpoint, createdOps_.end());
                    if (!selected.valid() || selected == base || selected == result)
                    {
                        ++stats_.unsupportedSlicesSkipped;
                        continue;
                    }
                    if (graph_.valueLogicalType(selected) != key.type)
                    {
                        const auto describeType = [](const LogicalType &type) {
                            std::string text = std::string(wolvrix::lib::grh::toString(type.kind)) +
                                " width=" + std::to_string(type.width) +
                                (type.isSigned ? " signed " : " unsigned ") +
                                std::string(wolvrix::lib::grh::toString(type.domain)) + " shape=[";
                            for (std::size_t i = 0; i < type.shape.size(); ++i)
                            {
                                if (i != 0) text += ",";
                                text += std::to_string(type.shape[i]);
                            }
                            return text + "]";
                        };
                        throw std::runtime_error("split-static-array selected value type differs from slice result at op " +
                                                 std::to_string(opId.index) + ": expected " + describeType(key.type) +
                                                 ", selected value " + std::to_string(selected.index) + " has " +
                                                 describeType(graph_.valueLogicalType(selected)));
                    }
                    const OperationId producer = graph_.valueDef(base);
                    replaceOperands(opId, {selected});
                    graph_.setOpKind(opId, OperationKind::kAssign);
                    graph_.eraseAttr(opId, "sliceStart");
                    graph_.eraseAttr(opId, "sliceEnd");
                    graph_.eraseAttr(opId, "sliceWidth");
                    mergeRewriteMetadata(graph_, opId, opId, producer);
                    splitValues_.insert(base);
                    if (arraySlice)
                    {
                        ++stats_.slicesRewritten;
                    }
                    else
                    {
                        ++stats_.staticSlicesRewritten;
                    }
                    changed_ = true;
                }
                stats_.valuesWithReadsSplit += splitValues_.size();
                return changed_;
            }

            ValueId selectStateValue(ValueId base, int64_t offset, const LogicalType &type,
                                     OperationId sourceOp)
            {
                SelectionKey key{base, offset, type.width, type};
                if (const ValueId selected = select(key, sourceOp, 1); selected.valid())
                {
                    return selected;
                }
                const ValueId fallback = createView(OperationKind::kSliceStatic, {base}, key, sourceOp,
                                                    graph_.valueDef(base));
                requestStaticSlice(graph_.valueDef(fallback));
                return fallback;
            }

            void requestStaticSlice(OperationId op)
            {
                requestedStaticSlices_.insert(op);
            }

        private:
            ValueId select(const SelectionKey &key, OperationId sourceOp, unsigned depth)
            {
                if (depth >= 256)
                {
                    return ValueId::invalid();
                }
                if (const auto found = cache_.find(key); found != cache_.end())
                {
                    return found->second;
                }
                if (!visiting_.insert(key).second)
                {
                    return ValueId::invalid();
                }
                const std::size_t opCheckpoint = createdOps_.size();
                const std::size_t cacheCheckpoint = cacheOrder_.size();
                const auto finish = [&](ValueId value) {
                    visiting_.erase(key);
                    if (value.valid())
                    {
                        if (cache_.emplace(key, value).second)
                        {
                            cacheOrder_.push_back(key);
                        }
                        return value;
                    }
                    while (cacheOrder_.size() > cacheCheckpoint)
                    {
                        cache_.erase(cacheOrder_.back());
                        cacheOrder_.pop_back();
                    }
                    while (createdOps_.size() > opCheckpoint)
                    {
                        requestedStaticSlices_.erase(createdOps_.back());
                        ingest::node::recordDead(graph_, createdOps_.back());
                        graph_.eraseOpUnchecked(createdOps_.back());
                        createdOps_.pop_back();
                        --stats_.opsCreated;
                    }
                    while (createdValues_.size() > opCheckpoint)
                    {
                        graph_.eraseValueUnchecked(createdValues_.back());
                        createdValues_.pop_back();
                    }
                    return ValueId::invalid();
                };

                const int64_t baseWidth = graph_.valueWidth(key.base);
                if (key.offset < 0 || key.width <= 0 || key.offset > baseWidth - key.width)
                {
                    return finish(ValueId::invalid());
                }
                const bool whole = key.offset == 0 && key.width == baseWidth;
                if (depth > 0 && whole && graph_.valueLogicalType(key.base) == key.type)
                {
                    return finish(key.base);
                }

                const OperationId producer = graph_.valueDef(key.base);
                if (producer.valid() && ingest::node::mayInline(graph_, sourceOp, producer))
                {
                    // Recursive selection creates views in the same operand
                    // arena. Keep IDs owned across those mutations.
                    slang::SmallVector<ValueId, 4> operands;
                    const auto sourceOperands = graph_.opOperands(producer);
                    operands.append(sourceOperands.begin(), sourceOperands.end());
                    switch (graph_.opKind(producer))
                    {
                    case OperationKind::kAssign:
                        if (operands.size() == 1 && graph_.valueWidth(operands[0]) == baseWidth)
                        {
                            SelectionKey nested{operands[0], key.offset, key.width, key.type};
                            return finish(select(nested, sourceOp, depth + 1));
                        }
                        break;
                    case OperationKind::kArrayCreate:
                    {
                        const auto shape = graph_.valueShape(key.base);
                        if (shape.empty() || shape.front() <= 0 ||
                            operands.size() != static_cast<std::size_t>(shape.front()) ||
                            baseWidth % shape.front() != 0)
                        {
                            break;
                        }
                        const int64_t elementWidth = baseWidth / shape.front();
                        if (key.offset % elementWidth == 0 && key.width % elementWidth == 0)
                        {
                            const int64_t first = key.offset / elementWidth;
                            const int64_t count = key.width / elementWidth;
                            if (count > 1 && first >= 0 && first + count <= shape.front())
                            {
                                return finish(createArrayGroup(
                                    key, std::span<const ValueId>(operands).subspan(static_cast<std::size_t>(first),
                                                                                   static_cast<std::size_t>(count)),
                                    sourceOp, producer));
                            }
                        }
                        const int64_t elementIndex = key.offset / elementWidth;
                        const int64_t localOffset = key.offset % elementWidth;
                        if (elementIndex < 0 || elementIndex >= shape.front() ||
                            localOffset > elementWidth - key.width)
                        {
                            break;
                        }
                        SelectionKey nested{operands[static_cast<std::size_t>(elementIndex)], localOffset,
                                            key.width, key.type};
                        return finish(select(nested, sourceOp, depth + 1));
                    }
                    case OperationKind::kConcat:
                    {
                        if (key.type.shape.empty())
                        {
                            std::vector<ValueId> selectedOperands;
                            int64_t high = baseWidth;
                            for (const ValueId operand : operands)
                            {
                                const int64_t operandWidth = graph_.valueWidth(operand);
                                const int64_t low = high - operandWidth;
                                const int64_t begin = std::max(key.offset, low);
                                const int64_t end = std::min(key.offset + key.width, high);
                                if (begin < end)
                                {
                                    LogicalType nestedType = key.type;
                                    nestedType.width = end - begin;
                                    SelectionKey nested{operand, begin - low, end - begin, nestedType};
                                    const ValueId selected = select(nested, sourceOp, depth + 1);
                                    if (!selected.valid())
                                    {
                                        return finish(ValueId::invalid());
                                    }
                                    selectedOperands.push_back(selected);
                                }
                                high = low;
                            }
                            if (selectedOperands.size() == 1)
                            {
                                return finish(selectedOperands.front());
                            }
                            if (!selectedOperands.empty())
                            {
                                return finish(createView(OperationKind::kConcat, selectedOperands, key,
                                                         sourceOp, producer));
                            }
                            break;
                        }
                        int64_t high = baseWidth;
                        for (const ValueId operand : operands)
                        {
                            const int64_t operandWidth = graph_.valueWidth(operand);
                            const int64_t low = high - operandWidth;
                            if (key.offset >= low && key.offset + key.width <= high)
                            {
                                SelectionKey nested{operand, key.offset - low, key.width, key.type};
                                return finish(select(nested, sourceOp, depth + 1));
                            }
                            high = low;
                        }
                        break;
                    }
                    case OperationKind::kSliceStatic:
                    {
                        if (operands.size() != 1)
                        {
                            break;
                        }
                        std::optional<int64_t> start;
                        std::optional<int64_t> end;
                        for (const auto &attribute : graph_.opAttrRange(producer))
                        {
                            if (attribute.key() == "sliceStart") start = attribute.value().get<int64_t>();
                            if (attribute.key() == "sliceEnd") end = attribute.value().get<int64_t>();
                        }
                        if (start && end && *start >= 0 && *end >= *start &&
                            *end - *start + 1 == baseWidth &&
                            key.offset <= baseWidth - key.width &&
                            *start <= std::numeric_limits<int64_t>::max() - key.offset)
                        {
                            SelectionKey nested{operands[0], *start + key.offset, key.width, key.type};
                            return finish(select(nested, sourceOp, depth + 1));
                        }
                        break;
                    }
                    case OperationKind::kMux:
                    {
                        if (operands.size() != 3 || graph_.valueWidth(operands[0]) != 1 ||
                            graph_.valueWidth(operands[1]) != baseWidth ||
                            graph_.valueWidth(operands[2]) != baseWidth)
                        {
                            break;
                        }
                        const ValueId condition = operands[0];
                        SelectionKey trueKey{operands[1], key.offset, key.width, key.type};
                        SelectionKey falseKey{operands[2], key.offset, key.width, key.type};
                        const ValueId trueValue = select(trueKey, sourceOp, depth + 1);
                        if (!trueValue.valid())
                        {
                            return finish(ValueId::invalid());
                        }
                        const ValueId falseValue = select(falseKey, sourceOp, depth + 1);
                        if (!falseValue.valid())
                        {
                            return finish(ValueId::invalid());
                        }
                        return finish(createView(OperationKind::kMux,
                                                 {condition, trueValue, falseValue}, key, sourceOp, producer));
                    }
                    default:
                        break;
                    }
                }

                if (depth > 0)
                {
                    if (whole)
                    {
                        return finish(createView(OperationKind::kAssign, {key.base}, key, sourceOp, producer));
                    }
                    const ValueId fallback = createView(OperationKind::kSliceStatic, {key.base}, key,
                                                        sourceOp, producer);
                    requestStaticSlice(graph_.valueDef(fallback));
                    return finish(fallback);
                }
                // A top-level unsupported selection is a no-op, not a
                // selection result. Caching its whole base would poison a
                // later recursive request for the same partial selection.
                return finish(ValueId::invalid());
            }

            ValueId createView(OperationKind kind, std::initializer_list<ValueId> operands,
                               const SelectionKey &key, OperationId sourceOp, OperationId producer,
                               const LogicalType *resultType = nullptr)
            {
                return createView(kind, std::span<const ValueId>(operands.begin(), operands.size()), key,
                                  sourceOp, producer, resultType);
            }

            ValueId createView(OperationKind kind, std::span<const ValueId> operands,
                               const SelectionKey &key, OperationId sourceOp, OperationId producer,
                               const LogicalType *resultType = nullptr)
            {
                if (createdOps_.empty())
                {
                    constexpr std::size_t kReserveBatch = 131072;
                    graph_.reserveValueCapacity(graph_.values().size() + kReserveBatch);
                    graph_.reserveOperationCapacity(graph_.operations().size() + kReserveBatch);
                }
                const ValueId result = graph_.createTemporaryValue(resultType ? *resultType : key.type);
                const OperationId op = graph_.createTemporaryOperation(kind);
                for (const ValueId operand : operands)
                {
                    graph_.addOperand(op, operand);
                }
                graph_.addResult(op, result);
                if (std::find(operands.begin(), operands.end(), result) != operands.end())
                {
                    throw std::runtime_error("split-static-array created a self-referential view");
                }
                if (kind == OperationKind::kSliceStatic)
                {
                    graph_.setAttr(op, "sliceStart", key.offset);
                    graph_.setAttr(op, "sliceEnd", key.offset + key.width - 1);
                }
                if (const auto location = graph_.opSrcLoc(sourceOp))
                {
                    graph_.setOpSrcLoc(op, *location);
                }
                mergeRewriteMetadata(graph_, op, sourceOp, producer);

                const auto sourceResults = graph_.opResults(sourceOp);
                auto semantics = sourceResults.empty() ? std::span<const SemanticId>{}
                    : graph_.valueSemanticIds(sourceResults[0]);
                if (semantics.empty())
                {
                    semantics = graph_.valueSemanticIds(key.base);
                }
                graph_.setValueSemanticIds(result, semantics);
                createdOps_.push_back(op);
                createdValues_.push_back(result);
                ++stats_.opsCreated;
                return result;
            }

            ValueId createArrayGroup(const SelectionKey &key, std::span<const ValueId> elements,
                                     OperationId sourceOp, OperationId producer)
            {
                const std::vector<ValueId> groupElements(elements.begin(), elements.end());
                LogicalType groupType = graph_.valueLogicalType(groupElements.front());
                groupType.width = key.width;
                groupType.shape.insert(groupType.shape.begin(), static_cast<int64_t>(groupElements.size()));
                const ValueId group = createView(OperationKind::kArrayCreate, groupElements, key,
                                                 sourceOp, producer, &groupType);
                if (groupType == key.type)
                {
                    return group;
                }
                return createView(OperationKind::kAssign, {group}, key, sourceOp, producer);
            }

            void replaceOperands(OperationId op, std::initializer_list<ValueId> operands)
            {
                while (!graph_.opOperands(op).empty())
                {
                    graph_.eraseOperand(op, 0);
                }
                for (const ValueId operand : operands)
                {
                    graph_.addOperand(op, operand);
                }
            }

            Graph &graph_;
            SplitStaticArrayPass::Stats &stats_;
            std::unordered_map<SelectionKey, ValueId, SelectionKeyHash> cache_;
            std::unordered_set<SelectionKey, SelectionKeyHash> visiting_;
            std::unordered_set<ValueId, ValueIdHash> splitValues_;
            std::unordered_set<OperationId, wolvrix::lib::grh::OperationIdHash> requestedStaticSlices_;
            std::vector<SelectionKey> cacheOrder_;
            std::vector<OperationId> createdOps_;
            std::vector<ValueId> createdValues_;
            bool changed_ = false;
        };

        struct IndexedStatePort
        {
            OperationId op;
            int64_t element = 0;
        };

        struct StateFamily
        {
            OperationId declaration;
            std::string symbol;
            LogicalType type;
            int64_t elements = 0;
            int32_t leafWidth = 0;
            bool declared = false;
            bool dynamic = false;
            bool unsupported = false;
            std::vector<OperationId> wholeReads;
            std::vector<OperationId> wholeWrites;
            std::vector<IndexedStatePort> indexedReads;
            std::vector<IndexedStatePort> indexedWrites;
            std::vector<std::string> memberNames;
            std::vector<std::string> memberInitValues;
        };

        class StateSplitter
        {
        public:
            StateSplitter(Graph &graph, GraphSplitter &splitter, SplitStaticArrayPass::Stats &stats,
                          bool keepDeclared)
                : graph_(graph), splitter_(splitter), stats_(stats), keepDeclared_(keepDeclared)
            {
            }

            bool run()
            {
                collectFamilies();
                std::vector<StateFamily *> eligible;
                for (StateFamily &family : families_)
                {
                    ++stats_.stateArraysVisited;
                    if (keepDeclared_ && family.declared)
                    {
                        ++stats_.stateArraysDeclaredSkipped;
                        continue;
                    }
                    if (family.dynamic)
                    {
                        ++stats_.stateArraysDynamicSkipped;
                        continue;
                    }
                    if (family.unsupported || (family.wholeReads.empty() && family.indexedReads.empty()) ||
                        !prepareMembers(family))
                    {
                        ++stats_.stateArraysUnsupportedSkipped;
                        continue;
                    }
                    eligible.push_back(&family);
                }
                for (StateFamily *family : eligible)
                {
                    if (!prepareMemberNames(*family))
                    {
                        ++stats_.stateArraysUnsupportedSkipped;
                        continue;
                    }
                    splitFamily(*family);
                    changed_ = true;
                }
                return changed_;
            }

        private:
            std::optional<int64_t> staticElementIndex(const StateFamily &family,
                                                       std::span<const ValueId> indices) const
            {
                if (indices.size() != family.type.shape.size())
                {
                    return std::nullopt;
                }
                int64_t linear = 0;
                for (std::size_t i = 0; i < indices.size(); ++i)
                {
                    const auto index = detail::constantIndex(graph_, indices[i]);
                    if (!index || *index >= static_cast<uint64_t>(family.type.shape[i]))
                    {
                        return std::nullopt;
                    }
                    linear = linear * family.type.shape[i] + static_cast<int64_t>(*index);
                }
                return linear;
            }

            void collectFamilies()
            {
                std::unordered_map<std::string, std::size_t> bySymbol;
                const std::vector<OperationId> operations(graph_.operations().begin(), graph_.operations().end());
                for (const OperationId op : operations)
                {
                    if (graph_.opKind(op) != OperationKind::kRegister)
                    {
                        continue;
                    }
                    LogicalType type = wolvrix::lib::grh::stateLogicalType(graph_, op);
                    if (type.shape.empty())
                    {
                        continue;
                    }
                    StateFamily family;
                    family.declaration = op;
                    family.symbol = std::string(graph_.symbolText(graph_.operationSymbol(op)));
                    family.type = std::move(type);
                    family.declared = graph_.isDeclaredSymbol(graph_.operationSymbol(op));
                    if (graph_.opAttrRange(op).find("firrtl.memory"))
                    {
                        family.unsupported = true;
                    }
                    int64_t elements = 1;
                    for (const int64_t dim : family.type.shape)
                    {
                        if (dim <= 0 || elements > std::numeric_limits<int64_t>::max() / dim)
                        {
                            family.unsupported = true;
                            break;
                        }
                        elements *= dim;
                    }
                    family.elements = elements;
                    if (family.symbol.empty())
                    {
                        family.unsupported = true;
                    }
                    bySymbol[family.symbol] = families_.size();
                    familyByDeclaration_[op] = families_.size();
                    families_.push_back(std::move(family));
                }
                for (const OperationId op : operations)
                {
                    const auto attribute = graph_.opAttrRange(op).find("regSymbol");
                    if (!attribute)
                    {
                        continue;
                    }
                    const auto text = attribute->value().get<std::string>();
                    if (!text)
                    {
                        continue;
                    }
                    registerPortsBySymbol_[std::string(*text)].push_back(op);
                    const auto found = bySymbol.find(std::string(*text));
                    if (found == bySymbol.end())
                    {
                        continue;
                    }
                    StateFamily &family = families_[found->second];
                    switch (graph_.opKind(op))
                    {
                    case OperationKind::kRegisterReadPort:
                        family.wholeReads.push_back(op);
                        break;
                    case OperationKind::kRegisterReadArrayPort:
                    {
                        const auto element = staticElementIndex(family, graph_.opOperands(op));
                        if (!element)
                        {
                            family.dynamic = true;
                        }
                        else
                        {
                            family.indexedReads.push_back({op, *element});
                        }
                        break;
                    }
                    case OperationKind::kRegisterWritePort:
                    case OperationKind::kRegisterWriteMaskPort:
                    case OperationKind::kRegisterFillPort:
                        family.wholeWrites.push_back(op);
                        break;
                    case OperationKind::kRegisterWriteArrayPort:
                    case OperationKind::kRegisterWriteArrayMaskPort:
                    {
                        const auto indexCount = wolvrix::lib::grh::arrayPortIndexCount(graph_, op);
                        if (!indexCount)
                        {
                            family.unsupported = true;
                            break;
                        }
                        const auto operands = graph_.opOperands(op);
                        if (*indexCount >= operands.size())
                        {
                            family.unsupported = true;
                            break;
                        }
                        const auto element = staticElementIndex(
                            family, std::span<const ValueId>(operands).subspan(1, *indexCount));
                        if (!element)
                        {
                            family.dynamic = true;
                        }
                        else
                        {
                            family.indexedWrites.push_back({op, *element});
                        }
                        break;
                    }
                    case OperationKind::kRegister:
                        break;
                    default:
                        family.unsupported = true;
                        break;
                    }
                }
            }

            bool prepareMembers(StateFamily &family)
            {
                if (family.type.kind != ValueType::Logic || family.elements <= 1)
                {
                    return false;
                }
                family.leafWidth = family.type.leafWidth();
                if (family.leafWidth <= 0 || family.elements > std::numeric_limits<int32_t>::max() ||
                    family.type.width != family.elements * family.leafWidth)
                {
                    return false;
                }
                try
                {
                    const wolvrix::lib::grh::InitSpec init = wolvrix::lib::grh::stateInitSpec(
                        graph_, family.declaration);
                    if (init.size() != 1)
                    {
                        return false;
                    }
                    const auto &step = init.front();
                    if (step.start.value_or(0) != 0 || step.count.value_or(family.elements) != family.elements ||
                        step.seed || step.random)
                    {
                        return false;
                    }
                    family.memberInitValues.resize(static_cast<std::size_t>(family.elements));
                    if (step.kind == wolvrix::lib::grh::InitKind::Constant)
                    {
                        auto bits = slang::SVInt::fromString(step.value);
                        bits.setSigned(family.type.isSigned);
                        bits = bits.resize(family.type.width);
                        for (int64_t i = 0; i < family.elements; ++i)
                        {
                            family.memberInitValues[static_cast<std::size_t>(i)] =
                                bits.slice((i + 1) * family.leafWidth - 1, i * family.leafWidth)
                                    .toString(slang::LiteralBase::Hex, true, family.leafWidth);
                        }
                    }
                    else if (step.kind == wolvrix::lib::grh::InitKind::Fill)
                    {
                        auto bits = slang::SVInt::fromString(step.value);
                        bits.setSigned(family.type.isSigned);
                        bits = bits.resize(family.leafWidth);
                        const std::string literal = bits.toString(slang::LiteralBase::Hex, true, family.leafWidth);
                        std::fill(family.memberInitValues.begin(), family.memberInitValues.end(), literal);
                    }
                    else
                    {
                        return false;
                    }
                }
                catch (const std::exception &)
                {
                    return false;
                }

                assignMemberNames(family);
                return true;
            }

            void assignMemberNames(StateFamily &family)
            {
                family.memberNames.clear();
                family.memberNames.reserve(static_cast<std::size_t>(family.elements));
                for (int64_t linear = 0; linear < family.elements; ++linear)
                {
                    std::vector<int64_t> coordinates(family.type.shape.size());
                    int64_t remaining = linear;
                    for (std::size_t i = family.type.shape.size(); i-- > 0;)
                    {
                        coordinates[i] = remaining % family.type.shape[i];
                        remaining /= family.type.shape[i];
                    }
                    std::string name = family.symbol;
                    for (std::size_t i = 0; i < coordinates.size(); ++i)
                    {
                        if (family.type.shape[i] != 1)
                        {
                            name += "_" + std::to_string(coordinates[i]);
                        }
                    }
                    family.memberNames.push_back(std::move(name));
                }
            }

            void updateDeclaredSymbol(SymbolId old, const std::string &name)
            {
                if (!graph_.isDeclaredSymbol(old))
                {
                    return;
                }
                graph_.removeDeclaredSymbol(old);
                graph_.addDeclaredSymbol(graph_.lookupSymbol(name));
            }

            bool renameCollision(const std::string &name)
            {
                const OperationId op = graph_.findOperation(name);
                const ValueId value = graph_.findValue(name);
                if (!op.valid() && !value.valid())
                {
                    return true;
                }
                const std::string renamed = name + "_0";
                if (graph_.findOperation(renamed).valid() || graph_.findValue(renamed).valid())
                {
                    return false;
                }
                if (value.valid())
                {
                    if (graph_.valueIsInput(value) || graph_.valueIsOutput(value) || graph_.valueIsInout(value))
                    {
                        return false;
                    }
                    const SymbolId old = graph_.valueSymbol(value);
                    graph_.setValueSymbol(value, graph_.internSymbol(renamed));
                    updateDeclaredSymbol(old, renamed);
                }
                else
                {
                    if (graph_.opKind(op) != OperationKind::kRegister)
                    {
                        return false;
                    }
                    const SymbolId old = graph_.operationSymbol(op);
                    graph_.setOpSymbol(op, graph_.internSymbol(renamed));
                    if (const auto found = registerPortsBySymbol_.find(name); found != registerPortsBySymbol_.end())
                    {
                        std::vector<OperationId> references = std::move(found->second);
                        registerPortsBySymbol_.erase(found);
                        for (const OperationId reference : references)
                        {
                            graph_.setAttr(reference, "regSymbol", renamed);
                        }
                        registerPortsBySymbol_[renamed] = std::move(references);
                    }
                    updateDeclaredSymbol(old, renamed);
                    if (const auto found = familyByDeclaration_.find(op); found != familyByDeclaration_.end())
                    {
                        StateFamily &other = families_[found->second];
                        other.symbol = renamed;
                        assignMemberNames(other);
                    }
                }
                ++stats_.stateSymbolsRenamed;
                changed_ = true;
                return true;
            }

            bool prepareMemberNames(StateFamily &family)
            {
                return std::all_of(family.memberNames.begin(), family.memberNames.end(),
                                   [&](const std::string &name) { return renameCollision(name); });
            }

            LogicalType memberType(const StateFamily &family) const
            {
                LogicalType type = family.type;
                type.width = family.leafWidth;
                type.shape.clear();
                return type;
            }

            void splitFamily(StateFamily &family)
            {
                const LogicalType scalarType = memberType(family);
                registerPortsBySymbol_.erase(family.symbol);
                const auto declarationSource = graph_.getOperation(family.declaration);
                const std::vector<SemanticId> declarationSemantics(
                    graph_.opSemanticIds(family.declaration).begin(), graph_.opSemanticIds(family.declaration).end());
                for (int64_t i = 0; i < family.elements; ++i)
                {
                    const std::string &name = family.memberNames[static_cast<std::size_t>(i)];
                    const SymbolId memberSymbol = graph_.internSymbol(name);
                    if (!memberSymbol.valid())
                    {
                        throw std::runtime_error("split-static-array member symbol is already bound: " + name);
                    }
                    const OperationId member = graph_.createOperation(OperationKind::kRegister, memberSymbol);
                    copyOperationMetadata(graph_, member, declarationSource, declarationSemantics);
                    wolvrix::lib::grh::setStateLogicalType(graph_, member, scalarType);
                    wolvrix::lib::grh::setStateInitSpec(
                        graph_, member, {{wolvrix::lib::grh::InitKind::Constant,
                                          family.memberInitValues[static_cast<std::size_t>(i)]}});
                    if (family.declared)
                    {
                        graph_.addDeclaredSymbol(graph_.operationSymbol(member));
                    }
                    ++stats_.opsCreated;
                    ++stats_.stateMembersCreated;
                }

                for (const IndexedStatePort &port : family.indexedReads)
                {
                    replaceOperationOperands(graph_, port.op, {});
                    graph_.setOpKind(port.op, OperationKind::kRegisterReadPort);
                    graph_.setAttr(port.op, "regSymbol", family.memberNames[static_cast<std::size_t>(port.element)]);
                    registerPortsBySymbol_[family.memberNames[static_cast<std::size_t>(port.element)]].push_back(port.op);
                    ++stats_.stateReadsRewritten;
                }
                for (const OperationId op : family.wholeReads)
                {
                    rewriteWholeRead(family, op, scalarType);
                }
                for (const IndexedStatePort &port : family.indexedWrites)
                {
                    rewriteIndexedWrite(family, port);
                }
                for (const OperationId op : family.wholeWrites)
                {
                    rewriteWholeWrite(family, op, scalarType);
                }
                if (family.declared)
                {
                    graph_.removeDeclaredSymbol(graph_.operationSymbol(family.declaration));
                }
                graph_.eraseOpUnchecked(family.declaration);
                ++stats_.stateArraysSplit;
            }

            void rewriteWholeRead(const StateFamily &family, OperationId op, const LogicalType &scalarType)
            {
                const ValueId originalResult = graph_.opResults(op)[0];
                const auto source = graph_.getOperation(op);
                const std::vector<SemanticId> opSemantics(graph_.opSemanticIds(op).begin(), graph_.opSemanticIds(op).end());
                const std::vector<SemanticId> valueSemantics(
                    graph_.valueSemanticIds(originalResult).begin(), graph_.valueSemanticIds(originalResult).end());
                const auto valueLocation = graph_.valueSrcLoc(originalResult);
                std::vector<ValueId> members;
                members.reserve(static_cast<std::size_t>(family.elements));
                for (int64_t i = 0; i < family.elements; ++i)
                {
                    const ValueId value = graph_.createTemporaryValue(scalarType);
                    const OperationId read = graph_.createTemporaryOperation(OperationKind::kRegisterReadPort);
                    graph_.addResult(read, value);
                    copyOperationMetadata(graph_, read, source, opSemantics);
                    graph_.setAttr(read, "regSymbol", family.memberNames[static_cast<std::size_t>(i)]);
                    registerPortsBySymbol_[family.memberNames[static_cast<std::size_t>(i)]].push_back(read);
                    graph_.setValueSemanticIds(value, valueSemantics);
                    if (valueLocation)
                    {
                        graph_.setValueSrcLoc(value, *valueLocation);
                    }
                    members.push_back(value);
                    ++stats_.opsCreated;
                }

                std::vector<ValueId> level = std::move(members);
                for (std::size_t dimension = family.type.shape.size(); dimension-- > 1;)
                {
                    LogicalType rowType = scalarType;
                    rowType.shape.assign(family.type.shape.begin() + dimension, family.type.shape.end());
                    rowType.width = family.leafWidth;
                    for (const int64_t dim : rowType.shape)
                    {
                        rowType.width *= static_cast<int32_t>(dim);
                    }
                    const std::size_t dim = static_cast<std::size_t>(family.type.shape[dimension]);
                    std::vector<ValueId> rows;
                    rows.reserve(level.size() / dim);
                    for (std::size_t offset = 0; offset < level.size(); offset += dim)
                    {
                        const ValueId row = graph_.createTemporaryValue(rowType);
                        const OperationId create = graph_.createTemporaryOperation(OperationKind::kArrayCreate);
                        for (std::size_t i = 0; i < dim; ++i)
                        {
                            graph_.addOperand(create, level[offset + i]);
                        }
                        graph_.addResult(create, row);
                        copyOperationMetadata(graph_, create, source, opSemantics);
                        graph_.eraseAttr(create, "regSymbol");
                        graph_.setValueSemanticIds(row, valueSemantics);
                        if (valueLocation)
                        {
                            graph_.setValueSrcLoc(row, *valueLocation);
                        }
                        rows.push_back(row);
                        ++stats_.opsCreated;
                    }
                    level = std::move(rows);
                }

                if (graph_.valueLogicalType(originalResult) == family.type)
                {
                    replaceOperationOperands(graph_, op, level);
                    graph_.setOpKind(op, OperationKind::kArrayCreate);
                }
                else
                {
                    const ValueId packed = graph_.createTemporaryValue(family.type);
                    const OperationId create = graph_.createTemporaryOperation(OperationKind::kArrayCreate);
                    for (const ValueId member : level)
                    {
                        graph_.addOperand(create, member);
                    }
                    graph_.addResult(create, packed);
                    copyOperationMetadata(graph_, create, source, opSemantics);
                    graph_.eraseAttr(create, "regSymbol");
                    graph_.setValueSemanticIds(packed, valueSemantics);
                    if (valueLocation)
                    {
                        graph_.setValueSrcLoc(packed, *valueLocation);
                    }
                    replaceOperationOperands(graph_, op, std::span<const ValueId>(&packed, 1));
                    graph_.setOpKind(op, OperationKind::kAssign);
                    ++stats_.opsCreated;
                }
                graph_.eraseAttr(op, "regSymbol");
                requestWholeReadViews(originalResult);
                ++stats_.stateWholeReadsRebuilt;
                ++stats_.stateReadsRewritten;
            }

            void requestWholeReadViews(ValueId root)
            {
                std::unordered_set<ValueId, ValueIdHash> visited;
                std::vector<ValueId> pending{root};
                visited.insert(root);
                while (!pending.empty())
                {
                    const ValueId value = pending.back();
                    pending.pop_back();
                    for (const auto &user : graph_.valueUsers(value))
                    {
                        const OperationId op = user.operation;
                        bool forward = false;
                        switch (graph_.opKind(op))
                        {
                        case OperationKind::kSliceStatic:
                            splitter_.requestStaticSlice(op);
                            forward = true;
                            break;
                        case OperationKind::kSliceArray:
                        case OperationKind::kConcat:
                        case OperationKind::kArrayCreate:
                            forward = true;
                            break;
                        case OperationKind::kAssign:
                            forward = graph_.opResults(op).size() == 1 && graph_.opOperands(op).size() == 1 &&
                                graph_.valueWidth(graph_.opResults(op)[0]) == graph_.valueWidth(value);
                            break;
                        case OperationKind::kMux:
                            forward = user.operandIndex > 0 && graph_.opResults(op).size() == 1 &&
                                graph_.opOperands(op).size() == 3 &&
                                graph_.valueWidth(graph_.opResults(op)[0]) == graph_.valueWidth(value);
                            break;
                        default:
                            break;
                        }
                        if (!forward || graph_.opResults(op).size() != 1)
                        {
                            continue;
                        }
                        const ValueId result = graph_.opResults(op)[0];
                        if (visited.insert(result).second)
                        {
                            pending.push_back(result);
                        }
                    }
                }
            }

            void rewriteIndexedWrite(const StateFamily &family, const IndexedStatePort &port)
            {
                const auto source = graph_.getOperation(port.op);
                const auto indexCount = wolvrix::lib::grh::arrayPortIndexCount(source);
                const auto eventStart = wolvrix::lib::grh::firstEventOperandIndex(source);
                if (!indexCount || !eventStart)
                {
                    throw std::invalid_argument("validated array write lost its operand geometry");
                }
                const bool masked = source.kind() == OperationKind::kRegisterWriteArrayMaskPort;
                const std::size_t data = 1 + *indexCount;
                std::vector<ValueId> operands{source.operands()[0], source.operands()[data]};
                if (masked)
                {
                    operands.push_back(source.operands()[data + 1]);
                }
                operands.insert(operands.end(), source.operands().begin() + *eventStart, source.operands().end());
                replaceOperationOperands(graph_, port.op, operands);
                graph_.setOpKind(port.op, masked ? OperationKind::kRegisterWriteMaskPort
                                                 : OperationKind::kRegisterWritePort);
                graph_.setAttr(port.op, "regSymbol", family.memberNames[static_cast<std::size_t>(port.element)]);
                registerPortsBySymbol_[family.memberNames[static_cast<std::size_t>(port.element)]].push_back(port.op);
                ++stats_.stateWritesRewritten;
            }

            void rewriteWholeWrite(const StateFamily &family, OperationId op, const LogicalType &scalarType)
            {
                const auto source = graph_.getOperation(op);
                const auto eventStart = wolvrix::lib::grh::firstEventOperandIndex(source);
                if (!eventStart)
                {
                    throw std::invalid_argument("validated whole write lost its operand geometry");
                }
                const std::vector<SemanticId> semantics(graph_.opSemanticIds(op).begin(), graph_.opSemanticIds(op).end());
                const bool fill = source.kind() == OperationKind::kRegisterFillPort;
                const bool masked = source.kind() == OperationKind::kRegisterWriteMaskPort;
                const OperationKind targetKind = masked ? OperationKind::kRegisterWriteMaskPort
                                                        : OperationKind::kRegisterWritePort;
                const ValueId dataBase = source.operands()[1];
                LogicalType maskType = scalarType;
                if (masked)
                {
                    maskType = graph_.valueLogicalType(source.operands()[2]);
                    maskType.width = family.leafWidth;
                    maskType.shape.clear();
                }
                for (int64_t i = 0; i < family.elements; ++i)
                {
                    const ValueId data = fill ? dataBase : splitter_.selectStateValue(
                        dataBase, i * family.leafWidth, scalarType, op);
                    std::vector<ValueId> operands{source.operands()[0], data};
                    if (masked)
                    {
                        const ValueId mask = splitter_.selectStateValue(
                            source.operands()[2], i * family.leafWidth, maskType, op);
                        operands.push_back(mask);
                    }
                    operands.insert(operands.end(), source.operands().begin() + *eventStart, source.operands().end());
                    OperationId target = op;
                    if (i != 0)
                    {
                        target = graph_.createTemporaryOperation(targetKind);
                        copyOperationMetadata(graph_, target, source, semantics);
                        ++stats_.opsCreated;
                    }
                    replaceOperationOperands(graph_, target, operands);
                    graph_.setOpKind(target, targetKind);
                    graph_.setAttr(target, "regSymbol", family.memberNames[static_cast<std::size_t>(i)]);
                    registerPortsBySymbol_[family.memberNames[static_cast<std::size_t>(i)]].push_back(target);
                }
                ++stats_.stateWholeWritesSplit;
                ++stats_.stateWritesRewritten;
            }

            Graph &graph_;
            GraphSplitter &splitter_;
            SplitStaticArrayPass::Stats &stats_;
            bool keepDeclared_ = false;
            bool changed_ = false;
            std::vector<StateFamily> families_;
            std::unordered_map<OperationId, std::size_t, wolvrix::lib::grh::OperationIdHash> familyByDeclaration_;
            std::unordered_map<std::string, std::vector<OperationId>> registerPortsBySymbol_;
        };
    } // namespace

    SplitStaticArrayPass::SplitStaticArrayPass()
        : Pass("split-static-array", "split-static-array",
               "Scalarize statically indexed combinational and state arrays")
    {
    }

    PassResult SplitStaticArrayPass::run()
    {
        stats_ = {};
        PassResult result;
        const auto selectedGraphs = graphs();
        stats_.graphs = selectedGraphs.size();
        for (Graph *graph : selectedGraphs)
        {
            try
            {
                GraphSplitter splitter(*graph, stats_);
                StateSplitter stateSplitter(*graph, splitter, stats_, keepDeclaredSymbols());
                bool changed = stateSplitter.run();
                changed = splitter.run() || changed;
                if (changed)
                {
                    ++stats_.changedGraphs;
                    result.changed = true;
                }
            }
            catch (const std::exception &exception)
            {
                error(*graph, exception.what());
                result.failed = true;
                return result;
            }
        }
        logDebug("graphs=" + std::to_string(stats_.graphs) +
                 ", changedGraphs=" + std::to_string(stats_.changedGraphs) +
                 ", slices=" + std::to_string(stats_.slicesVisited) +
                 ", rewritten=" + std::to_string(stats_.slicesRewritten) +
                 ", staticSlices=" + std::to_string(stats_.staticSlicesVisited) +
                 ", staticRewritten=" + std::to_string(stats_.staticSlicesRewritten) +
                 ", stateArrays=" + std::to_string(stats_.stateArraysVisited) +
                 ", stateSplit=" + std::to_string(stats_.stateArraysSplit) +
                 ", stateMembers=" + std::to_string(stats_.stateMembersCreated) +
                 ", created=" + std::to_string(stats_.opsCreated));
        return result;
    }

} // namespace wolvrix::lib::transform
