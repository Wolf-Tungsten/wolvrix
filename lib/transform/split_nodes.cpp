#include "transform/split_nodes.hpp"

#include "core/op_descriptor.hpp"
#include "core/state.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include "slang/util/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform {
namespace {

using namespace grh;

void requireWholeGrh(Graph& graph)
{
    if (graph.metadata(grhsim::kStage))
        throw std::invalid_argument("split-nodes requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations()) {
        if (grhsim::attribute<std::vector<std::string>>(graph, op, grhsim::kPartition))
            throw std::invalid_argument("split-nodes requires an unpartitioned GRH graph");
    }
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = grhsim::verifyGraph(graph); !problem.empty()) throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

struct Range {
    int64_t low = 0;
    int64_t high = 0; // exclusive
};

bool operator==(const Range& left, const Range& right) noexcept
{
    return left.low == right.low && left.high == right.high;
}

bool validRange(const Graph& graph, ValueId value, int64_t low, int64_t width)
{
    return low >= 0 && width > 0 && low <= graph.valueWidth(value) &&
        width <= graph.valueWidth(value) - low;
}

std::optional<Range> staticSlice(const Graph& graph, OperationId op, ValueId source)
{
    if (graph.opKind(op) != OperationKind::kSliceStatic || graph.opOperands(op).size() != 1 ||
        graph.opOperands(op)[0] != source || graph.opResults(op).size() != 1)
        return std::nullopt;
    const auto start = grhsim::attribute<int64_t>(graph, op, "sliceStart");
    const auto end = grhsim::attribute<int64_t>(graph, op, "sliceEnd");
    if (!start || !end || *end < *start || !validRange(graph, source, *start, *end - *start + 1))
        return std::nullopt;
    const auto result = graph.opResults(op)[0];
    if (*end - *start + 1 != graph.valueWidth(result)) return std::nullopt;
    return Range{*start, *end + 1};
}

bool scalarUnsignedLogic(const Graph& graph, ValueId value)
{
    const auto type = graph.valueLogicalType(value);
    return type.kind == ValueType::Logic && type.shape.empty() && !type.isSigned && type.width > 0;
}

bool splitEligibleKind(OperationKind kind)
{
    switch (kind) {
    case OperationKind::kAssign:
    case OperationKind::kAnd:
    case OperationKind::kOr:
    case OperationKind::kXor:
    case OperationKind::kXnor:
    case OperationKind::kNot:
    case OperationKind::kMux:
    case OperationKind::kConcat:
    case OperationKind::kSliceStatic:
        return true;
    default:
        return false;
    }
}

void mergeRange(std::vector<Range>& ranges, Range range)
{
    if (range.low >= range.high) return;
    ranges.push_back(range);
    std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) {
        return left.low < right.low || (left.low == right.low && left.high < right.high);
    });
    std::vector<Range> merged;
    merged.reserve(ranges.size());
    for (const auto current : ranges) {
        if (merged.empty() || current.low > merged.back().high) merged.push_back(current);
        else merged.back().high = std::max(merged.back().high, current.high);
    }
    ranges.swap(merged);
}

bool copyableAttribute(std::string_view key)
{
    // These attributes describe the original expression's provenance and are
    // safe on a structural clone.  Slice bounds are recreated by the caller.
    return key != "sliceStart" && key != "sliceEnd";
}

class Splitter {
public:
    explicit Splitter(Graph& graph) : graph_(graph) {}

    bool run()
    {
        std::vector<OperationId> originalOps(graph_.operations().begin(), graph_.operations().end());
        bool changed = false;
        for (const auto op : originalOps) {
            if (!op || graph_.opResults(op).size() != 1 || !splitEligibleKind(graph_.opKind(op))) continue;
            const auto result = graph_.opResults(op)[0];
            if (!scalarUnsignedLogic(graph_, result)) continue;
            // A port binding is an observable whole-value use even though it
            // is not represented in the operation use-list.
            if (graph_.valueIsOutput(result) || graph_.valueIsInout(result)) continue;
            const auto ranges = demandedRanges(result);
            if (ranges.size() < 2 && (ranges.empty() || ranges.front().low == 0 &&
                                      ranges.front().high == graph_.valueWidth(result))) continue;
            if (ranges.empty() || (ranges.size() == 1 && ranges.front().low == 0 &&
                                   ranges.front().high == graph_.valueWidth(result))) continue;
            const auto users = sliceUsers(result);
            if (users.empty()) continue;
            for (const auto& [user, range] : users) {
                const auto replacement = sliceValue(result, range.low, range.high - range.low);
                if (!replacement) continue;
                if (rewriteSliceUser(user, replacement)) {
                    changed = true;
                    ++rewrites_;
                }
            }
        }
        return changed;
    }

    std::size_t rewrites() const noexcept { return rewrites_; }

private:
    struct SliceKey {
        ValueId value;
        int64_t low = 0;
        int64_t width = 0;

        friend bool operator==(const SliceKey&, const SliceKey&) = default;
    };

    struct SliceKeyHash {
        std::size_t operator()(const SliceKey& key) const noexcept {
            std::size_t seed = ValueIdHash{}(key.value);
            seed = seed * 1315423911u + static_cast<std::size_t>(key.low);
            seed = seed * 1315423911u + static_cast<std::size_t>(key.width);
            return seed;
        }
    };

    struct SliceUse {
        OperationId op;
        Range range;
    };

    std::vector<Range> demandedRanges(ValueId value) const
    {
        std::vector<Range> ranges;
        for (const auto user : graph_.valueUsers(value)) {
            const auto range = staticSlice(graph_, user.operation, value);
            if (!range) return {};
            mergeRange(ranges, *range);
        }
        return ranges;
    }

    std::vector<SliceUse> sliceUsers(ValueId value) const
    {
        std::vector<SliceUse> users;
        for (const auto user : graph_.valueUsers(value)) {
            const auto range = staticSlice(graph_, user.operation, value);
            if (range) users.push_back({user.operation, *range});
        }
        return users;
    }

    ValueId makeSliceView(ValueId source, int64_t low, int64_t width)
    {
        if (low == 0 && width == graph_.valueWidth(source)) return source;
        const auto sourceType = graph_.valueLogicalType(source);
        auto type = sourceType;
        type.width = static_cast<int32_t>(width);
        // A bit-range view is a scalar even when its source is an array or a
        // signed value reached through an equal-width assignment.
        type.shape.clear();
        type.isSigned = false;
        const auto op = graph_.createOperation(OperationKind::kSliceStatic);
        const auto result = graph_.createValue(type);
        graph_.addOperand(op, source);
        graph_.addResult(op, result);
        graph_.setAttr(op, "sliceStart", low);
        graph_.setAttr(op, "sliceEnd", low + width - 1);
        if (const auto producer = graph_.valueDef(source)) graph_.mergeOpSemanticIds(op, producer);
        graph_.mergeValueSemanticIds(result, source);
        if (const auto loc = graph_.valueSrcLoc(source)) {
            graph_.setOpSrcLoc(op, *loc);
            graph_.setValueSrcLoc(result, *loc);
        }
        return result;
    }

    ValueId cloneOperation(OperationId source, std::span<const ValueId> operands, const LogicalType& type)
    {
        std::vector<std::pair<std::string, AttributeValue>> attributes;
        for (const auto attr : graph_.opAttrRange(source))
            if (copyableAttribute(attr.key()))
                attributes.emplace_back(std::string(attr.key()), attr.value().toOwned());
        const auto op = graph_.createOperation(graph_.opKind(source));
        const auto result = graph_.createValue(type);
        for (const auto operand : operands) graph_.addOperand(op, operand);
        graph_.addResult(op, result);
        for (const auto& [key, value] : attributes) graph_.setAttr(op, key, value);
        graph_.mergeOpSemanticIds(op, source);
        graph_.mergeValueSemanticIds(result, graph_.opResults(source).front());
        if (const auto loc = graph_.opSrcLoc(source)) {
            graph_.setOpSrcLoc(op, *loc);
            graph_.setValueSrcLoc(result, *loc);
        }
        return result;
    }

    ValueId sliceValue(ValueId value, int64_t low, int64_t width)
    {
        if (!validRange(graph_, value, low, width)) return {};
        if (low == 0 && width == graph_.valueWidth(value)) return value;
        const SliceKey key{value, low, width};
        if (const auto it = cache_.find(key); it != cache_.end()) return it->second;
        if (!active_.insert(key).second) return {};

        const auto producer = graph_.valueDef(value);
        ValueId result;
        if (!producer || !splitEligibleKind(graph_.opKind(producer)) || graph_.opResults(producer).size() != 1 ||
            !scalarUnsignedLogic(graph_, value)) {
            result = makeSliceView(value, low, width);
        } else {
            const auto kind = graph_.opKind(producer);
            // Recursive slicing appends operands to the same shared arena.
            // Keep source IDs stable across those mutations.
            slang::SmallVector<ValueId, 4> args;
            const auto sourceArgs = graph_.opOperands(producer);
            args.append(sourceArgs.begin(), sourceArgs.end());
            std::vector<ValueId> operands;
            bool supported = true;
            if (kind == OperationKind::kSliceStatic) {
                const auto sourceRange = staticSlice(graph_, producer, args.empty() ? ValueId{} : args[0]);
                if (!sourceRange || !validRange(graph_, args[0], sourceRange->low + low, width)) supported = false;
                else result = sliceValue(args[0], sourceRange->low + low, width);
            } else if (kind == OperationKind::kAssign) {
                if (args.size() != 1) supported = false;
                else result = sliceValue(args[0], low, width);
            } else if (kind == OperationKind::kConcat) {
                int64_t offset = 0;
                std::vector<std::pair<ValueId, Range>> pieces;
                for (std::size_t i = args.size(); i-- > 0;) {
                    const auto argWidth = graph_.valueWidth(args[i]);
                    const auto begin = std::max<int64_t>(low, offset);
                    const auto end = std::min<int64_t>(low + width, offset + argWidth);
                    if (begin < end) pieces.push_back({args[i], {begin - offset, end - offset}});
                    offset += argWidth;
                }
                std::reverse(pieces.begin(), pieces.end());
                for (const auto& [arg, range] : pieces) {
                    const auto part = sliceValue(arg, range.low, range.high - range.low);
                    if (!part) { supported = false; break; }
                    operands.push_back(part);
                }
                if (supported && operands.size() == 1) result = operands.front();
                else if (supported && !operands.empty()) {
                    auto type = graph_.valueLogicalType(value);
                    type.width = static_cast<int32_t>(width);
                    result = cloneOperation(producer, operands, type);
                } else supported = false;
            } else if (kind == OperationKind::kMux) {
                if (args.size() != 3 || !scalarUnsignedLogic(graph_, args[0])) supported = false;
                else {
                    operands.push_back(args[0]);
                    for (std::size_t i = 1; i < args.size(); ++i) {
                        const auto part = sliceValue(args[i], low, width);
                        if (!part) { supported = false; break; }
                        operands.push_back(part);
                    }
                    if (supported) {
                        auto type = graph_.valueLogicalType(value);
                        type.width = static_cast<int32_t>(width);
                        result = cloneOperation(producer, operands, type);
                    }
                }
            } else {
                for (const auto arg : args) {
                    const auto part = sliceValue(arg, low, width);
                    if (!part) { supported = false; break; }
                    operands.push_back(part);
                }
                if (supported) {
                    auto type = graph_.valueLogicalType(value);
                    type.width = static_cast<int32_t>(width);
                    result = cloneOperation(producer, operands, type);
                }
            }
            if (!supported || !result) result = makeSliceView(value, low, width);
        }
        active_.erase(key);
        cache_.emplace(key, result);
        return result;
    }

    bool rewriteSliceUser(OperationId user, ValueId replacement)
    {
        if (!user || graph_.opKind(user) != OperationKind::kSliceStatic || graph_.opOperands(user).size() != 1 ||
            graph_.opResults(user).size() != 1) return false;
        const auto oldResult = graph_.opResults(user)[0];
        if (graph_.valueWidth(replacement) != graph_.valueWidth(oldResult)) return false;
        graph_.replaceOperand(user, 0, replacement);
        graph_.setOpKind(user, OperationKind::kAssign);
        graph_.eraseAttr(user, "sliceStart");
        graph_.eraseAttr(user, "sliceEnd");
        graph_.mergeValueSemanticIds(replacement, oldResult);
        return true;
    }

    Graph& graph_;
    std::unordered_map<SliceKey, ValueId, SliceKeyHash> cache_;
    std::unordered_set<SliceKey, SliceKeyHash> active_;
    std::size_t rewrites_ = 0;
};

} // namespace

SplitNodesPass::SplitNodesPass(SplitNodesOptions options)
    : Pass("split-nodes", "split-nodes", "Split independent static bit ranges in GRH expressions"),
      options_(std::move(options))
{}

bool SplitNodesPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh";
}

PassResult SplitNodesPass::run()
{
    PassResult result;
    try {
        Graph& graph = [&]() -> Graph& {
            if (options_.top.empty()) {
                if (design().topGraphs().size() != 1)
                    throw std::invalid_argument("Select exactly one top graph with -top");
                return *design().findGraph(design().topGraphs()[0]);
            }
            auto* selected = design().findGraph(options_.top);
            if (!selected) throw std::invalid_argument("Top graph does not exist: " + options_.top);
            return *selected;
        }();
        requireWholeGrh(graph);
        verifyWhole(graph);
        const auto before = graph.operations().size();
        Splitter splitter(graph);
        const bool changed = splitter.run();
        result.changed = changed;
        verifyWhole(graph);
        const auto summary = "split-slices=" + std::to_string(splitter.rewrites()) +
            " operations=" + std::to_string(before) + "->" + std::to_string(graph.operations().size());
        info(summary);
        logInfo(summary);
    } catch (const std::exception& error) {
        this->error(error.what());
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform
