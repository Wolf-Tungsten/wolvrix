#include "transform/const_fold.hpp"

#include "core/grh.hpp"

#include "topo_guard.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    // Definitions for forward-declared types from the header
    struct ConstantValue
    {
        slang::SVInt value;
        bool hasUnknown = false;
    };

    struct ConstantKey
    {
        std::string literal;
        int32_t width = 0;
        bool isSigned = false;
        // Shape is part of the identity: stage-01 values carry it, and
        // downstream shape-aware consumers (kSliceArray) reject a flat
        // substitute even when the bits are identical.
        std::vector<int64_t> shape;

        bool operator==(const ConstantKey &other) const
        {
            return width == other.width &&
                   isSigned == other.isSigned &&
                   shape == other.shape &&
                   literal == other.literal;
        }
    };

    struct ConstantKeyHash
    {
        std::size_t operator()(const ConstantKey &key) const
        {
            std::size_t seed = std::hash<std::string>{}(key.literal);
            seed ^= std::hash<int32_t>{}(key.width) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<bool>{}(key.isSigned) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            for (const int64_t dim : key.shape)
                seed ^= std::hash<int64_t>{}(dim) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    namespace
    {
        struct FoldOptions
        {
            ConstantFoldOptions::XFoldMode xFold = ConstantFoldOptions::XFoldMode::Known;
            ConstantFoldOptions::Semantics semantics = ConstantFoldOptions::Semantics::FourState;
        };

        using ConstantStore = std::unordered_map<wolvrix::lib::grh::ValueId, ConstantValue, wolvrix::lib::grh::ValueIdHash>;
        using ConstantPool = std::unordered_map<ConstantKey, wolvrix::lib::grh::ValueId, ConstantKeyHash>;

        bool isFoldable(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
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
                return true;
            default:
                return false;
            }
        }

        std::optional<std::string> getStringAttr(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            (void)graph;
            auto attr = op.attr(key);
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

        std::optional<bool> getBoolAttr(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            (void)graph;
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<bool>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<int64_t> getIntAttr(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            (void)graph;
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<int64_t>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        // Lean overload: reads the attribute span directly (no Operation
        // materialization) for hot screening loops.
        std::optional<int64_t> getIntAttr(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationId op, std::string_view key)
        {
            for (const auto &kv : graph.opAttrs(op))
            {
                if (kv.key == key)
                {
                    if (const auto *value = std::get_if<int64_t>(&kv.value))
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        bool isValuePortBound(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId value)
        {
            for (const auto &port : graph.inputPorts())
            {
                if (port.value == value)
                {
                    return true;
                }
            }
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == value)
                {
                    return true;
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                if (port.in == value || port.out == value || port.oe == value)
                {
                    return true;
                }
            }
            return false;
        }

        std::optional<ConstantValue> parseConstLiteral(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, const wolvrix::lib::grh::Value &value, const std::string &literal, const std::function<void(std::string)> &onError)
        {
            if (!literal.empty() && literal.front() == '"')
            {
                return std::nullopt;
            }
            if (!literal.empty() && literal.front() == '$')
            {
                return std::nullopt;
            }
            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(literal);
                if (value.width() <= 0)
                {
                    onError("Value width must be positive for constant propagation: " + std::string(value.symbolText()));
                    return std::nullopt;
                }
                parsed.setSigned(value.isSigned());
                parsed = parsed.resize(static_cast<slang::bitwidth_t>(value.width()));
                ConstantValue result{parsed, parsed.hasUnknown()};
                return result;
            }
            catch (const std::exception &ex)
            {
                std::ostringstream oss;
                oss << "Failed to parse constValue '" << literal << "': " << ex.what();
                onError(oss.str());
                return std::nullopt;
            }
        }

        std::optional<ConstantValue> parseConstValue(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, const wolvrix::lib::grh::Value &value, const std::function<void(std::string)> &onError)
        {
            auto literalOpt = getStringAttr(graph, op, "constValue");
            if (!literalOpt)
            {
                onError("kConstant missing constValue attribute");
                return std::nullopt;
            }
            return parseConstLiteral(graph, op, value, *literalOpt, onError);
        }

        // Lean variant of parseConstValue: borrows the constValue attribute
        // text from the op's attribute span and reads width/signedness through
        // the Graph's id accessors — no Operation/Value materialization and no
        // literal string copy (stage-07: aggregate reset literals are huge).
        std::optional<ConstantValue> parseConstValueLean(const wolvrix::lib::grh::Graph &graph,
                                                         wolvrix::lib::grh::OperationId opId,
                                                         wolvrix::lib::grh::ValueId valueId,
                                                         const std::function<void(std::string)> &onError)
        {
            std::string_view literal;
            bool found = false;
            for (const auto &kv : graph.opAttrs(opId))
            {
                if (kv.key == "constValue")
                {
                    if (const auto *text = std::get_if<std::string>(&kv.value))
                    {
                        literal = *text;
                        found = true;
                    }
                    break;
                }
            }
            if (!found)
            {
                onError("kConstant missing constValue attribute");
                return std::nullopt;
            }
            if (!literal.empty() && (literal.front() == '"' || literal.front() == '$'))
            {
                return std::nullopt;
            }
            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(literal);
                const int32_t width = graph.valueWidth(valueId);
                if (width <= 0)
                {
                    onError("Value width must be positive for constant propagation");
                    return std::nullopt;
                }
                parsed.setSigned(graph.valueSigned(valueId));
                parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
                return ConstantValue{parsed, parsed.hasUnknown()};
            }
            catch (const std::exception &ex)
            {
                onError("Failed to parse constValue '" + std::string(literal) + "': " + ex.what());
                return std::nullopt;
            }
        }

        slang::SVInt normalizeToValue(const wolvrix::lib::grh::Value &value, const slang::SVInt &raw)
        {
            slang::SVInt adjusted = raw;
            adjusted.setSigned(value.isSigned());
            adjusted = adjusted.resize(static_cast<slang::bitwidth_t>(value.width()));
            adjusted.setSigned(value.isSigned());
            return adjusted;
        }

        std::optional<slang::SVInt> foldBinary(const wolvrix::lib::grh::Operation &op, wolvrix::lib::grh::OperationKind kind, const std::vector<slang::SVInt> &operands)
        {
            const slang::SVInt &lhs = operands[0];
            const slang::SVInt &rhs = operands[1];
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kAdd:
                return lhs + rhs;
            case wolvrix::lib::grh::OperationKind::kSub:
                return lhs - rhs;
            case wolvrix::lib::grh::OperationKind::kMul:
                return lhs * rhs;
            case wolvrix::lib::grh::OperationKind::kDiv:
                return lhs / rhs;
            case wolvrix::lib::grh::OperationKind::kMod:
                return lhs % rhs;
            case wolvrix::lib::grh::OperationKind::kAnd:
                return lhs & rhs;
            case wolvrix::lib::grh::OperationKind::kOr:
                return lhs | rhs;
            case wolvrix::lib::grh::OperationKind::kXor:
                return lhs ^ rhs;
            case wolvrix::lib::grh::OperationKind::kXnor:
                return ~(lhs ^ rhs);
            case wolvrix::lib::grh::OperationKind::kEq:
                return slang::SVInt(lhs == rhs);
            case wolvrix::lib::grh::OperationKind::kNe:
                return slang::SVInt(lhs != rhs);
            case wolvrix::lib::grh::OperationKind::kCaseEq:
                return slang::SVInt(exactlyEqual(lhs, rhs));
            case wolvrix::lib::grh::OperationKind::kCaseNe:
                return slang::SVInt(!exactlyEqual(lhs, rhs));
            case wolvrix::lib::grh::OperationKind::kWildcardEq:
                return slang::SVInt(caseXWildcardEqual(lhs, rhs));
            case wolvrix::lib::grh::OperationKind::kWildcardNe:
                return slang::SVInt(!caseXWildcardEqual(lhs, rhs));
            case wolvrix::lib::grh::OperationKind::kLt:
                return slang::SVInt(lhs < rhs);
            case wolvrix::lib::grh::OperationKind::kLe:
                return slang::SVInt(lhs <= rhs);
            case wolvrix::lib::grh::OperationKind::kGt:
                return slang::SVInt(lhs > rhs);
            case wolvrix::lib::grh::OperationKind::kGe:
                return slang::SVInt(lhs >= rhs);
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
                return slang::SVInt(lhs && rhs);
            case wolvrix::lib::grh::OperationKind::kLogicOr:
                return slang::SVInt(lhs || rhs);
            case wolvrix::lib::grh::OperationKind::kShl:
                return lhs.shl(rhs);
            case wolvrix::lib::grh::OperationKind::kLShr:
                return lhs.lshr(rhs);
            case wolvrix::lib::grh::OperationKind::kAShr:
                return lhs.ashr(rhs);
            default:
                (void)op;
                return std::nullopt;
            }
        }

        std::optional<slang::SVInt> foldUnary(const wolvrix::lib::grh::Operation &op, wolvrix::lib::grh::OperationKind kind, const slang::SVInt &operand)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kNot:
                return ~operand;
            case wolvrix::lib::grh::OperationKind::kLogicNot:
                return slang::SVInt(!operand);
            case wolvrix::lib::grh::OperationKind::kReduceAnd:
                return slang::SVInt(operand.reductionAnd());
            case wolvrix::lib::grh::OperationKind::kReduceOr:
                return slang::SVInt(operand.reductionOr());
            case wolvrix::lib::grh::OperationKind::kReduceXor:
                return slang::SVInt(operand.reductionXor());
            case wolvrix::lib::grh::OperationKind::kReduceNor:
                return slang::SVInt(!operand.reductionOr());
            case wolvrix::lib::grh::OperationKind::kReduceNand:
                return slang::SVInt(!operand.reductionAnd());
            case wolvrix::lib::grh::OperationKind::kReduceXnor:
                return slang::SVInt(!operand.reductionXor());
            default:
                (void)op;
                return std::nullopt;
            }
        }

        std::optional<std::vector<slang::SVInt>> foldOperation(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, const ConstantStore &store, const FoldOptions options, const std::function<void(std::string)> &onError, const std::function<void(std::string)> &onWarning)
        {
            if (op.results().empty())
            {
                return std::nullopt;
            }

            std::vector<slang::SVInt> operands;
            operands.reserve(op.operands().size());
            bool hasUnknown = false;
            for (std::size_t i = 0; i < op.operands().size(); ++i)
            {
                const auto valId = op.operands()[i];
                if (!valId.valid())
                {
                    onError("Operand missing during constant propagation");
                    return std::nullopt;
                }
                auto it = store.find(valId);
                if (it == store.end())
                {
                    return std::nullopt;
                }
                hasUnknown = hasUnknown || it->second.hasUnknown;
                operands.push_back(it->second.value);
            }

            if (options.xFold == ConstantFoldOptions::XFoldMode::Strict && hasUnknown)
            {
                return std::nullopt;
            }

            std::optional<slang::SVInt> folded;
            switch (op.kind())
            {
            case wolvrix::lib::grh::OperationKind::kSystemFunction: {
                auto name = getStringAttr(graph, op, "name");
                if (!name || name->empty())
                {
                    return std::nullopt;
                }
                if (auto sideEffect = getBoolAttr(graph, op, "hasSideEffects"); sideEffect && *sideEffect)
                {
                    return std::nullopt;
                }
                if (*name == "clog2")
                {
                    if (operands.size() != 1)
                    {
                        onError("$clog2 expects exactly one operand");
                        return std::nullopt;
                    }
                    const uint32_t result = slang::clog2(operands[0]);
                    folded = slang::SVInt(static_cast<uint64_t>(result));
                }
                else
                {
                    return std::nullopt;
                }
                break;
            }
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
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
            case wolvrix::lib::grh::OperationKind::kLogicOr:
            case wolvrix::lib::grh::OperationKind::kShl:
            case wolvrix::lib::grh::OperationKind::kLShr:
            case wolvrix::lib::grh::OperationKind::kAShr:
                folded = foldBinary(op, op.kind(), operands);
                break;
            case wolvrix::lib::grh::OperationKind::kNot:
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            case wolvrix::lib::grh::OperationKind::kReduceAnd:
            case wolvrix::lib::grh::OperationKind::kReduceOr:
            case wolvrix::lib::grh::OperationKind::kReduceXor:
            case wolvrix::lib::grh::OperationKind::kReduceNor:
            case wolvrix::lib::grh::OperationKind::kReduceNand:
            case wolvrix::lib::grh::OperationKind::kReduceXnor:
                folded = foldUnary(op, op.kind(), operands[0]);
                break;
            case wolvrix::lib::grh::OperationKind::kAssign:
                folded = operands[0];
                break;
            case wolvrix::lib::grh::OperationKind::kConcat:
                folded = slang::SVInt::concat(operands);
                break;
            case wolvrix::lib::grh::OperationKind::kReplicate:
            {
                auto repOpt = getIntAttr(graph, op, "rep");
                if (!repOpt)
                {
                    onError("kReplicate missing required 'rep' attribute");
                    return std::nullopt;
                }
                if (*repOpt <= 0)
                {
                    onError("kReplicate requires positive rep attribute");
                    return std::nullopt;
                }
                slang::SVInt times(static_cast<uint64_t>(*repOpt));
                folded = operands[0].replicate(times);
                break;
            }
            case wolvrix::lib::grh::OperationKind::kMux:
                folded = slang::SVInt::conditional(operands[0], operands[1], operands[2]);
                break;
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            {
                auto startOpt = getIntAttr(graph, op, "sliceStart");
                auto endOpt = getIntAttr(graph, op, "sliceEnd");
                if (!startOpt || !endOpt)
                {
                    onError("kSliceStatic missing sliceStart/sliceEnd attributes");
                    return std::nullopt;
                }
                int64_t start = *startOpt;
                int64_t end = *endOpt;
                if (start < 0 || end < start)
                {
                    onError("kSliceStatic has invalid slice range");
                    return std::nullopt;
                }
                uint64_t width = static_cast<uint64_t>(end - start + 1);
                folded = operands[0].lshr(static_cast<slang::bitwidth_t>(start)).trunc(static_cast<slang::bitwidth_t>(width));
                break;
            }
            case wolvrix::lib::grh::OperationKind::kSliceDynamic:
            {
                auto widthOpt = getIntAttr(graph, op, "sliceWidth");
                if (!widthOpt)
                {
                    onError("Slice operation missing sliceWidth attribute");
                    return std::nullopt;
                }
                if (*widthOpt <= 0)
                {
                    onError("sliceWidth must be positive");
                    return std::nullopt;
                }
                uint64_t width = static_cast<uint64_t>(*widthOpt);
                const slang::SVInt &input = operands[0];
                const slang::SVInt &offset = operands[1];
                slang::SVInt shifted = input.lshr(offset);
                folded = shifted.trunc(static_cast<slang::bitwidth_t>(width));
                break;
            }
            case wolvrix::lib::grh::OperationKind::kSliceArray:
            {
                // stage-02: no sliceWidth attr; geometry comes from the base
                // value's shape. All operands are constants here.
                const auto baseId = op.operands().front();
                const std::span<const int64_t> shape = graph.valueShape(baseId);
                if (shape.empty())
                {
                    const wolvrix::lib::grh::OperationId baseDefId = graph.valueDef(baseId);
                    onError("kSliceArray base value must carry a shape; base index="
                            + std::to_string(baseId.index) + " gen=" + std::to_string(baseId.generation)
                            + " width=" + std::to_string(graph.valueWidth(baseId))
                            + " defKind=" + (baseDefId.valid()
                                                 ? std::string(wolvrix::lib::grh::toString(graph.opKind(baseDefId)))
                                                 : std::string("none"))
                            + " baseSym=" + std::string(graph.valueSymbol(baseId).valid()
                                                           ? graph.symbolText(graph.valueSymbol(baseId))
                                                           : std::string_view("<none>")));
                    return std::nullopt;
                }
                const int64_t baseWidth = graph.valueWidth(baseId);
                int64_t elementCount = 1;
                for (const int64_t dim : shape)
                {
                    elementCount *= dim;
                }
                if (elementCount <= 0 || baseWidth % elementCount != 0)
                {
                    onError("kSliceArray base shape does not divide its width");
                    return std::nullopt;
                }
                const int64_t elementWidth = baseWidth / elementCount;
                const std::size_t indexCount = operands.size() - 1;
                if (indexCount < 1 || indexCount > shape.size())
                {
                    onError("kSliceArray index count out of range for base rank");
                    return std::nullopt;
                }
                int64_t linearIndex = 0;
                for (std::size_t i = 0; i < indexCount; ++i)
                {
                    auto idx = operands[i + 1].as<uint64_t>();
                    if (!idx)
                    {
                        return std::nullopt;
                    }
                    linearIndex = linearIndex * shape[i] + static_cast<int64_t>(*idx);
                }
                int64_t subVolume = elementWidth;
                for (std::size_t i = indexCount; i < shape.size(); ++i)
                {
                    subVolume *= shape[i];
                }
                const int64_t resultWidth = graph.valueWidth(op.results().front());
                if (subVolume != resultWidth)
                {
                    std::string shapeText;
                    for (const int64_t dim : shape)
                    {
                        shapeText += (shapeText.empty() ? "[" : "x[") + std::to_string(dim);
                        shapeText += "]";
                    }
                    onError("kSliceArray result width does not match the sliced sub-volume; base index="
                            + std::to_string(baseId.index) + " width=" + std::to_string(baseWidth)
                            + " shape=" + shapeText + " indexCount=" + std::to_string(indexCount)
                            + " subVolume=" + std::to_string(subVolume)
                            + " resultWidth=" + std::to_string(resultWidth)
                            + " baseDefKind=" + ([&]() {
                                  const auto d = graph.valueDef(baseId);
                                  return d.valid()
                                             ? std::string(wolvrix::lib::grh::toString(graph.opKind(d)))
                                             : std::string("none");
                              })());
                    return std::nullopt;
                }
                folded = operands[0]
                             .lshr(static_cast<slang::bitwidth_t>(linearIndex * subVolume))
                             .trunc(static_cast<slang::bitwidth_t>(resultWidth));
                break;
            }
            default:
                return std::nullopt;
            }

            if (!folded)
            {
                return std::nullopt;
            }

            if (options.xFold == ConstantFoldOptions::XFoldMode::Known && folded->hasUnknown())
            {
                return std::nullopt;
            }
            if (options.xFold == ConstantFoldOptions::XFoldMode::Propagate && folded->hasUnknown())
            {
                onWarning("Folding produced X/Z result while xFold=propagate");
            }

            std::vector<slang::SVInt> results;
            results.reserve(op.results().size());
            for (std::size_t i = 0; i < op.results().size(); ++i)
            {
                const auto resId = op.results()[i];
                if (!resId.valid())
                {
                    onError("Result missing during constant propagation");
                    return std::nullopt;
                }
                results.push_back(normalizeToValue(graph.getValue(resId), *folded));
            }
            return results;
        }

        std::string formatConstLiteral(const slang::SVInt &value)
        {
            const slang::bitwidth_t width = value.getBitWidth();
            return value.toString(slang::LiteralBase::Hex, true, width);
        }

        bool isDeclaredValue(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId valueId)
        {
            if (!valueId.valid())
            {
                return false;
            }
            const wolvrix::lib::grh::Value value = graph.getValue(valueId);
            return graph.isDeclaredSymbol(value.symbol());
        }

        struct DeclaredMaterialization
        {
            wolvrix::lib::grh::ValueId result;
            wolvrix::lib::grh::ValueId operand;
            std::optional<std::string> constLiteral;
            std::optional<wolvrix::lib::grh::SrcLoc> srcLoc;
        };

        using DeclaredMaterializationMap =
            std::unordered_map<wolvrix::lib::grh::OperationId,
                               std::vector<DeclaredMaterialization>,
                               wolvrix::lib::grh::OperationIdHash>;

        void recordDeclaredAssign(DeclaredMaterializationMap &assigns,
                                  const wolvrix::lib::grh::Operation &op,
                                  wolvrix::lib::grh::ValueId result,
                                  wolvrix::lib::grh::ValueId operand)
        {
            DeclaredMaterialization entry{result, operand, std::nullopt, op.srcLoc()};
            if (!entry.srcLoc)
            {
                entry.srcLoc = makeTransformSrcLoc("const-fold", "declared_assign");
            }
            assigns[op.id()].push_back(std::move(entry));
        }

        void recordDeclaredConst(DeclaredMaterializationMap &assigns,
                                 const wolvrix::lib::grh::Operation &op,
                                 wolvrix::lib::grh::ValueId result,
                                 std::string_view literal)
        {
            DeclaredMaterialization entry{result, wolvrix::lib::grh::ValueId::invalid(),
                                          std::string(literal), op.srcLoc()};
            if (!entry.srcLoc)
            {
                entry.srcLoc = makeTransformSrcLoc("const-fold", "declared_const");
            }
            assigns[op.id()].push_back(std::move(entry));
        }

        void materializeDeclaredSymbols(wolvrix::lib::grh::Graph &graph,
                                        const DeclaredMaterializationMap &assigns,
                                        wolvrix::lib::grh::OperationId opId,
                                        const std::function<void(std::string)> &onError)
        {
            auto it = assigns.find(opId);
            if (it == assigns.end())
            {
                return;
            }
            for (const auto &entry : it->second)
            {
                try
                {
                    if (entry.constLiteral)
                    {
                        const wolvrix::lib::grh::OperationId constOp =
                            graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant);
                        if (entry.srcLoc)
                        {
                            graph.setOpSrcLoc(constOp, *entry.srcLoc);
                        }
                        graph.addResult(constOp, entry.result);
                        graph.setAttr(constOp, "constValue", *entry.constLiteral);
                    }
                    else
                    {
                        if (!entry.operand.valid())
                        {
                            onError("Declared assign missing operand");
                            continue;
                        }
                        const wolvrix::lib::grh::OperationId assignOp =
                            graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign);
                        graph.addOperand(assignOp, entry.operand);
                        graph.addResult(assignOp, entry.result);
                        if (entry.srcLoc)
                        {
                            graph.setOpSrcLoc(assignOp, *entry.srcLoc);
                        }
                    }
                }
                catch (const std::exception &ex)
                {
                    onError(std::string("Failed to materialize declared symbol: ") + ex.what());
                }
            }
        }

        ConstantKey makeConstantKey(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId valueId, const slang::SVInt &sv)
        {
            ConstantKey key;
            key.width = graph.valueWidth(valueId);
            key.isSigned = graph.valueSigned(valueId);
            const auto shape = graph.valueShape(valueId);
            key.shape.assign(shape.begin(), shape.end());
            key.literal = formatConstLiteral(sv);
            return key;
        }

        wolvrix::lib::grh::ValueId createConstant(wolvrix::lib::grh::Graph &graph, ConstantPool &pool,
                                        const wolvrix::lib::grh::Operation &sourceOp, std::size_t resultIndex,
                                        wolvrix::lib::grh::ValueId resultValueId,
                                        const slang::SVInt &value)
        {
            ConstantKey key = makeConstantKey(graph, resultValueId, value);
            if (auto it = pool.find(key); it != pool.end())
            {
                return it->second;
            }

            const wolvrix::lib::grh::SymbolId valueSym = graph.makeInternalValSym();
            const wolvrix::lib::grh::SymbolId opSym = graph.makeInternalOpSym();
            int32_t width = graph.valueWidth(resultValueId);
            if (width <= 0)
            {
                width = static_cast<int32_t>(value.getBitWidth());
            }
            const wolvrix::lib::grh::ValueId newValue =
                graph.createValue(valueSym, width, graph.valueSigned(resultValueId),
                                  graph.valueType(resultValueId), graph.valueShape(resultValueId));
            const wolvrix::lib::grh::OperationId constOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
            graph.addResult(constOp, newValue);
            // Reuse the canonical literal already computed for the pool key
            // (mega aggregate literals are expensive to format twice).
            graph.setAttr(constOp, "constValue", key.literal);
            std::string note = "from_";
            note.append(wolvrix::lib::grh::toString(sourceOp.kind()));
            const wolvrix::lib::grh::SrcLoc genLoc = makeTransformSrcLoc("const-fold", note);
            graph.setValueSrcLoc(newValue, genLoc);
            graph.setOpSrcLoc(constOp, genLoc);
            pool.emplace(std::move(key), newValue);
            return newValue;
        }

        wolvrix::lib::grh::ValueId createConstantClone(wolvrix::lib::grh::Graph &graph,
                                                       wolvrix::lib::grh::ValueId resultValueId,
                                                       const slang::SVInt &value,
                                                       std::string_view note)
        {
            const wolvrix::lib::grh::SymbolId valueSym = graph.makeInternalValSym();
            const wolvrix::lib::grh::SymbolId opSym = graph.makeInternalOpSym();
            int32_t width = graph.valueWidth(resultValueId);
            if (width <= 0)
            {
                width = static_cast<int32_t>(value.getBitWidth());
            }
            const wolvrix::lib::grh::ValueId newValue =
                graph.createValue(valueSym, width, graph.valueSigned(resultValueId),
                                  graph.valueType(resultValueId), graph.valueShape(resultValueId));
            const wolvrix::lib::grh::OperationId constOp =
                graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
            graph.addResult(constOp, newValue);
            graph.setAttr(constOp, "constValue", formatConstLiteral(value));
            const wolvrix::lib::grh::SrcLoc genLoc = makeTransformSrcLoc("const-fold", note);
            graph.setValueSrcLoc(newValue, genLoc);
            graph.setOpSrcLoc(constOp, genLoc);
            return newValue;
        }

        using ConstantStoreLocal = std::unordered_map<wolvrix::lib::grh::ValueId,
                                                      ConstantValue,
                                                      wolvrix::lib::grh::ValueIdHash>;

        wolvrix::lib::grh::ValueId createReplacementConstant(wolvrix::lib::grh::Graph &graph,
                                                             ConstantPool &pool,
                                                             ConstantStoreLocal &constants,
                                                             bool protectDeclaredSymbols,
                                                             const wolvrix::lib::grh::Operation &sourceOp,
                                                             std::size_t resultIndex,
                                                             wolvrix::lib::grh::ValueId resultValueId,
                                                             const slang::SVInt &value)
        {
            wolvrix::lib::grh::ValueId newValue =
                createConstant(graph, pool, sourceOp, resultIndex, resultValueId, value);
            if (protectDeclaredSymbols && isDeclaredValue(graph, newValue))
            {
                ConstantKey key = makeConstantKey(graph, resultValueId, value);
                newValue = createConstantClone(graph, resultValueId, value, "clone_const");
                pool.insert_or_assign(std::move(key), newValue);
            }
            constants[newValue] = ConstantValue{value, value.hasUnknown()};
            return newValue;
        }

        void replaceUsers(wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId oldValue, wolvrix::lib::grh::ValueId newValue, const std::function<void(std::string)> &onError)
        {
            // Shape views survive aliasing: Graph::replaceAllUses backfills the
            // shape onto flat replacements and rejects mismatched non-empty
            // shapes (stage-07 core guarantee).
            try
            {
                graph.replaceAllUses(oldValue, newValue);
            }
            catch (const std::exception &ex)
            {
                onError(std::string("Failed to replace operands for constant folding: ") + ex.what());
            }

            std::vector<std::string> outputPortsToUpdate;
            for (const auto &port : graph.outputPorts())
            {
                if (port.value == oldValue)
                {
                    outputPortsToUpdate.push_back(port.name);
                }
            }

            for (const auto &portName : outputPortsToUpdate)
            {
                try
                {
                    graph.bindOutputPort(portName, newValue);
                }
                catch (const std::exception &ex)
                {
                    onError(std::string("Failed to rebind output port during constant folding: ") + ex.what());
                }
            }
        }

    } // namespace

    ConstantFoldPass::ConstantFoldPass()
        : Pass("const-fold", "const-fold"), options_({})
    {
    }

    ConstantFoldPass::ConstantFoldPass(ConstantFoldOptions options)
        : Pass("const-fold", "const-fold"), options_(options)
    {
    }

    bool ConstantFoldPass::collectConstants(GraphFoldContext &ctx)
    {
        bool dedupedConstants = false;

        for (const auto opId : ctx.graph.operations())
        {
            // Lean screening: no Operation/Value materialization per op —
            // aggregate reset literals make the constValue attribute strings
            // huge, and the fat copies dominated this sweep (stage-07).
            if (ctx.graph.opKind(opId) != wolvrix::lib::grh::OperationKind::kConstant)
            {
                continue;
            }
            for (const auto resId : ctx.graph.opResults(opId))
            {
                if (!resId.valid())
                {
                    error(ctx.graph, ctx.graph.getOperation(opId), "kConstant missing result");
                    ctx.failed = true;
                    continue;
                }
                if (ctx.constants.find(resId) != ctx.constants.end())
                {
                    continue;
                }
                if (ctx.graph.valueType(resId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    continue;
                }
                auto reportError = [&](const std::string &msg)
                { this->error(ctx.graph, ctx.graph.getOperation(opId), msg); ctx.failed = true; };
                auto parsed = parseConstValueLean(ctx.graph, opId, resId, reportError);
                if (!parsed)
                {
                    continue;
                }
                ctx.constants.emplace(resId, *parsed);
                ConstantKey key = makeConstantKey(ctx.graph, resId, parsed->value);
                if (auto it = ctx.pool->find(key); it != ctx.pool->end())
                {
                    if (it->second != resId && !ctx.graph.valueIsInput(resId) && !ctx.graph.valueIsInout(resId))
                    {
                        if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resId))
                        {
                            continue;
                        }
                        replaceUsers(ctx.graph, resId, it->second, reportError);
                        dedupedConstants = true;
                        ++ctx.dedupedConstants;
                    }
                    continue;
                }
                ctx.pool->emplace(std::move(key), resId);
            }
        }

        return dedupedConstants;
    }

    bool ConstantFoldPass::iterativeFolding(GraphFoldContext &ctx)
    {
        bool anyChanged = false;
        FoldOptions foldOpts{options_.xFold, options_.semantics};
        if (options_.semantics == ConstantFoldOptions::Semantics::TwoState)
        {
            foldOpts.xFold = ConstantFoldOptions::XFoldMode::Strict;
        }

        bool iterationChanged = false;
        std::vector<wolvrix::lib::grh::OperationId> opOrder(ctx.graph.operations().begin(), ctx.graph.operations().end());
        std::vector<wolvrix::lib::grh::OperationId> opsToErase;
        DeclaredMaterializationMap declaredAssigns;

        for (const auto opId : opOrder)
        {
            // Lean screening first (stage-07): kind/result-type/constant-operand
            // checks read id/span accessors only; the fat Operation is
            // materialized once the op is known to fold.
            const wolvrix::lib::grh::OperationKind opKind = ctx.graph.opKind(opId);
            if (opKind == wolvrix::lib::grh::OperationKind::kConstant || !isFoldable(opKind))
            {
                continue;
            }
            if (ctx.foldedOps.find(opId) != ctx.foldedOps.end())
            {
                continue;
            }
            bool nonLogicResult = false;
            for (const auto resId : ctx.graph.opResults(opId))
            {
                if (resId.valid() &&
                    ctx.graph.valueType(resId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    nonLogicResult = true;
                    break;
                }
            }
            if (nonLogicResult)
            {
                continue;
            }
            bool allConstant = true;
            for (const auto operandId : ctx.graph.opOperands(opId))
            {
                if (!operandId.valid() || ctx.constants.find(operandId) == ctx.constants.end())
                {
                    allConstant = false;
                    break;
                }
            }
            if (!allConstant)
            {
                continue;
            }
            const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
            auto onError = [&](const std::string &msg)
            { this->error(ctx.graph, op, msg); ctx.failed = true; };
            auto onWarning = [&](const std::string &msg)
            { this->warning(ctx.graph, op, msg); };

            std::optional<std::vector<slang::SVInt>> folded = foldOperation(ctx.graph, op, ctx.constants, foldOpts, onError, onWarning);
            if (!folded)
            {
                continue;
            }

            bool createdAllResults = true;
            for (std::size_t idx = 0; idx < folded->size(); ++idx)
            {
                const slang::SVInt &sv = (*folded)[idx];
                const auto resId = op.results()[idx];
                if (!resId.valid())
                {
                    error(ctx.graph, op, "Result missing during folding");
                    ctx.failed = true;
                    createdAllResults = false;
                    continue;
                }
                wolvrix::lib::grh::ValueId newValue =
                    createReplacementConstant(ctx.graph, *ctx.pool, ctx.constants,
                                              ctx.protectDeclaredSymbols, op, idx, resId, sv);
                replaceUsers(ctx.graph, resId, newValue, [&](const std::string &msg)
                             { this->error(ctx.graph, op, msg); ctx.failed = true; });
                if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resId))
                {
                    recordDeclaredConst(declaredAssigns, op, resId, formatConstLiteral(sv));
                }
                iterationChanged = true;
            }

            if (createdAllResults)
            {
                ctx.foldedOps.insert(opId);
                opsToErase.push_back(opId);
                ++ctx.foldedOpsCount;
            }
        }

        for (const auto opId : opsToErase)
        {
            if (!ctx.graph.eraseOp(opId))
            {
                const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
                error(ctx.graph, op, "Failed to erase folded operation");
                ctx.failed = true;
            }
            else
            {
                ++ctx.opsErased;
                materializeDeclaredSymbols(ctx.graph, declaredAssigns, opId,
                                           [&](const std::string &msg)
                                           { this->error(ctx.graph, msg); ctx.failed = true; });
            }
        }

        anyChanged = anyChanged || iterationChanged;

        return anyChanged;
    }

    bool ConstantFoldPass::simplifySlices(GraphFoldContext &ctx)
    {
        bool simplifiedSlices = false;
        std::vector<wolvrix::lib::grh::OperationId> opOrder(ctx.graph.operations().begin(), ctx.graph.operations().end());
        std::vector<wolvrix::lib::grh::OperationId> opsToErase;
        DeclaredMaterializationMap declaredAssigns;
        // Computed once per sweep; remains a valid topological order across the
        // sweep's mutations (see TopoValueRanks), so the per-candidate cycle
        // guard becomes O(1) instead of a fresh cone DFS per candidate.
        const topo_guard::TopoValueRanks topoRanks = topo_guard::computeTopoValueRanks(ctx.graph);
        topo_guard::ConeDfsScratch dfsScratch;

        for (const auto opId : opOrder)
        {
            // Lean read-only screening through the Graph's id/span accessors
            // (no fat Value/Operation materialization); the fat Operation is
            // fetched only for accepted candidates (error/declared-assign
            // reporting) and the erase pass below.
            if (ctx.graph.opKind(opId) != wolvrix::lib::grh::OperationKind::kSliceStatic)
            {
                continue;
            }
            const auto operands = ctx.graph.opOperands(opId);
            const auto results = ctx.graph.opResults(opId);
            if (operands.size() != 1 || results.size() != 1)
            {
                continue;
            }
            auto sliceStart = getIntAttr(ctx.graph, opId, "sliceStart");
            auto sliceEnd = getIntAttr(ctx.graph, opId, "sliceEnd");
            if (!sliceStart || !sliceEnd)
            {
                continue;
            }
            const int64_t low = *sliceStart;
            const int64_t high = *sliceEnd;
            if (low < 0 || high < low)
            {
                continue;
            }
            const wolvrix::lib::grh::ValueId baseValueId = operands[0];
            if (!baseValueId.valid())
            {
                continue;
            }
            const wolvrix::lib::grh::OperationId baseDefId = ctx.graph.valueDef(baseValueId);
            if (!baseDefId.valid())
            {
                continue;
            }
            if (ctx.graph.opKind(baseDefId) != wolvrix::lib::grh::OperationKind::kConcat)
            {
                continue;
            }
            const auto concatOperands = ctx.graph.opOperands(baseDefId);
            if (concatOperands.empty())
            {
                continue;
            }
            std::vector<int64_t> widths;
            widths.reserve(concatOperands.size());
            int64_t totalWidth = 0;
            bool widthsOk = true;
            for (const auto operandId : concatOperands)
            {
                if (!operandId.valid())
                {
                    widthsOk = false;
                    break;
                }
                const int64_t width = ctx.graph.valueWidth(operandId);
                if (width <= 0)
                {
                    widthsOk = false;
                    break;
                }
                widths.push_back(width);
                totalWidth += width;
            }
            if (!widthsOk || totalWidth <= 0)
            {
                continue;
            }
            if (high >= totalWidth)
            {
                continue;
            }
            const wolvrix::lib::grh::ValueId resultId = results[0];
            if (!resultId.valid())
            {
                continue;
            }
            const int64_t resultWidth = ctx.graph.valueWidth(resultId);
            const bool resultSigned = ctx.graph.valueSigned(resultId);
            int64_t cursor = totalWidth;
            for (std::size_t i = 0; i < concatOperands.size(); ++i)
            {
                const int64_t width = widths[i];
                const int64_t hi = cursor - 1;
                const int64_t lo = cursor - width;
                cursor = lo;
                if (lo != low || hi != high)
                {
                    continue;
                }
                const wolvrix::lib::grh::ValueId operandId = concatOperands[i];
                if (!operandId.valid())
                {
                    break;
                }
                if (!topoRanks.definitelyNotDependent(operandId, resultId)
                    && topo_guard::dependsTransitivelyOn(ctx.graph, operandId, resultId, dfsScratch))
                {
                    break;
                }
                if (ctx.graph.valueWidth(operandId) != resultWidth ||
                    ctx.graph.valueSigned(operandId) != resultSigned)
                {
                    break;
                }
                if (!topo_guard::shapesAliasable(ctx.graph, resultId, operandId))
                {
                    break;
                }
                const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
                auto onError = [&](const std::string &msg)
                { this->error(ctx.graph, op, msg); ctx.failed = true; };
                replaceUsers(ctx.graph, resultId, operandId, onError);
                if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resultId))
                {
                    recordDeclaredAssign(declaredAssigns, op, resultId, operandId);
                }
                const wolvrix::lib::grh::Value updatedResult = ctx.graph.getValue(resultId);
                if (updatedResult.users().empty())
                {
                    opsToErase.push_back(opId);
                }
                else
                {
                    debug(ctx.graph, "Skipping erase of simplified kSliceStatic (still has users)");
                }
                simplifiedSlices = true;
                ++ctx.simplifiedSlices;
                break;
            }
        }

        for (const auto opId : opsToErase)
        {
            const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
            if (!ctx.graph.eraseOp(opId))
            {
                debug(ctx.graph, op, "Failed to erase simplified kSliceStatic op (still used)");
            }
            else
            {
                ++ctx.opsErased;
                materializeDeclaredSymbols(ctx.graph, declaredAssigns, opId,
                                           [&](const std::string &msg)
                                           { this->error(ctx.graph, msg); ctx.failed = true; });
            }
        }

        if (std::getenv("WOLVRIX_CONST_FOLD_GUARD_STATS") != nullptr)
        {
            std::fprintf(stderr,
                         "[const-fold-guard] graph=%s values=%zu unranked=%zu guardFastSafe=%zu guardDfsFallback=%zu\n",
                         ctx.graph.symbol().c_str(), topoRanks.totalValues, topoRanks.unrankedValues,
                         topoRanks.fastSafe, topoRanks.dfsFallback);
        }

        return simplifiedSlices;
    }

    bool ConstantFoldPass::eliminateDeadConstants(GraphFoldContext &ctx)
    {
        bool removedDeadConstants = false;
        std::vector<wolvrix::lib::grh::OperationId> deadConstOps;

        for (const auto opId : ctx.graph.operations())
        {
            const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
            if (op.kind() != wolvrix::lib::grh::OperationKind::kConstant)
            {
                continue;
            }
            const auto &results = op.results();
            if (results.empty())
            {
                error(ctx.graph, op, "kConstant missing result");
                ctx.failed = true;
                continue;
            }
            bool live = false;
            for (const auto resId : results)
            {
                if (!resId.valid())
                {
                    error(ctx.graph, op, "kConstant missing result");
                    ctx.failed = true;
                    live = true;
                    break;
                }
                if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resId))
                {
                    live = true;
                    break;
                }
                if (isValuePortBound(ctx.graph, resId))
                {
                    live = true;
                    break;
                }
                if (!ctx.graph.getValue(resId).users().empty())
                {
                    live = true;
                    break;
                }
            }
            if (!live)
            {
                deadConstOps.push_back(opId);
            }
        }

        for (const auto opId : deadConstOps)
        {
            const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
            if (!ctx.graph.eraseOp(opId))
            {
                error(ctx.graph, op, "Failed to erase dead kConstant op");
                ctx.failed = true;
            }
            else
            {
                removedDeadConstants = true;
                ++ctx.deadConstantsRemoved;
                ++ctx.opsErased;
            }
        }

        return removedDeadConstants;
    }

    bool ConstantFoldPass::simplifyUnsignedComparisons(GraphFoldContext &ctx)
    {
        bool simplified = false;
        std::vector<wolvrix::lib::grh::OperationId> opOrder(ctx.graph.operations().begin(), ctx.graph.operations().end());
        std::vector<wolvrix::lib::grh::OperationId> opsToErase;
        DeclaredMaterializationMap declaredAssigns;

        for (const auto opId : opOrder)
        {
            const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
            const auto kind = op.kind();
            
            // Handle unsigned >= 0 (always true) and unsigned <= max (always true)
            bool isGe = (kind == wolvrix::lib::grh::OperationKind::kGe);
            bool isLe = (kind == wolvrix::lib::grh::OperationKind::kLe);
            bool isGt = (kind == wolvrix::lib::grh::OperationKind::kGt);
            bool isLt = (kind == wolvrix::lib::grh::OperationKind::kLt);
            const bool isTwoState =
                options_.semantics == ConstantFoldOptions::Semantics::TwoState;
            
            if (!isGe && !isLe && !(isTwoState && (isGt || isLt)))
            {
                continue;
            }
            
            const auto &operands = op.operands();
            const auto &results = op.results();
            if (operands.size() < 2 || results.empty())
            {
                continue;
            }
            
            const wolvrix::lib::grh::ValueId lhsId = operands[0];
            const wolvrix::lib::grh::ValueId rhsId = operands[1];
            const wolvrix::lib::grh::ValueId resultId = results[0];
            
            if (!lhsId.valid() || !rhsId.valid() || !resultId.valid())
            {
                continue;
            }
            
            const wolvrix::lib::grh::Value lhsValue = ctx.graph.getValue(lhsId);
            const wolvrix::lib::grh::Value rhsValue = ctx.graph.getValue(rhsId);

            const auto isUnsignedCompare = [&](const wolvrix::lib::grh::Value &lhs,
                                               const wolvrix::lib::grh::Value &rhs) -> bool {
                return !(lhs.isSigned() && rhs.isSigned());
            };

            const auto isConstZero = [&](wolvrix::lib::grh::ValueId valueId) -> bool {
                auto it = ctx.constants.find(valueId);
                if (it == ctx.constants.end())
                {
                    return false;
                }
                if (it->second.hasUnknown)
                {
                    return false;
                }
                const auto &sv = it->second.value;
                return sv.getBitWidth() > 0 && sv.getActiveBits() == 0;
            };

            const auto foldToBool = [&](bool value) {
                slang::SVInt foldedValue(1, value ? 1 : 0, false);
                auto onError = [&](const std::string &msg)
                { this->error(ctx.graph, op, msg); ctx.failed = true; };
                wolvrix::lib::grh::ValueId newValue =
                    createReplacementConstant(ctx.graph, *ctx.pool, ctx.constants,
                                              ctx.protectDeclaredSymbols, op, 0, resultId, foldedValue);
                replaceUsers(ctx.graph, resultId, newValue, onError);
                if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resultId))
                {
                    recordDeclaredConst(declaredAssigns, op, resultId, formatConstLiteral(foldedValue));
                }
                opsToErase.push_back(opId);
                simplified = true;
                ++ctx.unsignedCmpSimplified;
            };

            if (isTwoState && isUnsignedCompare(lhsValue, rhsValue))
            {
                // In 2-state semantics, unsigned 0 > x and x < 0 are always false.
                if ((isGt && isConstZero(lhsId)) || (isLt && isConstZero(rhsId)))
                {
                    foldToBool(false);
                    continue;
                }
                // In 2-state semantics, unsigned 0 <= x and x >= 0 are always true.
                if ((isLe && isConstZero(lhsId)) || (isGe && isConstZero(rhsId)))
                {
                    foldToBool(true);
                    continue;
                }
            }
            
            // Check for unsigned >= 0 (always true for unsigned)
            // LHS must be unsigned, RHS must be constant 0
            if (isGe && !lhsValue.isSigned())
            {
                auto rhsConstIt = ctx.constants.find(rhsId);
                if (rhsConstIt != ctx.constants.end())
                {
                    const auto &rhsSv = rhsConstIt->second.value;
                    // Check if RHS is zero
                    if (rhsSv.getBitWidth() > 0 && rhsSv.getActiveBits() == 0)
                    {
                        // Replace with constant 1'b1
                        slang::SVInt trueValue(1, 1, false);
                        auto onError = [&](const std::string &msg)
                        { this->error(ctx.graph, op, msg); ctx.failed = true; };
                        wolvrix::lib::grh::ValueId newValue =
                            createReplacementConstant(ctx.graph, *ctx.pool, ctx.constants,
                                                      ctx.protectDeclaredSymbols, op, 0, resultId, trueValue);
                        replaceUsers(ctx.graph, resultId, newValue, onError);
                        if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resultId))
                        {
                            recordDeclaredConst(declaredAssigns, op, resultId, formatConstLiteral(trueValue));
                        }
                        opsToErase.push_back(opId);
                        simplified = true;
                        ++ctx.unsignedCmpSimplified;
                        continue;
                    }
                }
            }
            
            // Check for unsigned <= max_value (always true for unsigned)
            // LHS must be unsigned, RHS must be constant with all 1s at LHS width
            if (isLe && !lhsValue.isSigned())
            {
                auto rhsConstIt = ctx.constants.find(rhsId);
                if (rhsConstIt != ctx.constants.end())
                {
                    const auto &rhsSv = rhsConstIt->second.value;
                    int64_t lhsWidth = lhsValue.width();
                    
                    if (lhsWidth > 0 && rhsSv.getBitWidth() > 0)
                    {
                        // Check if RHS is all 1s up to LHS width
                        // Resize RHS to LHS width and check if all bits are 1
                        slang::SVInt resizedRhs = rhsSv.resize(static_cast<slang::bitwidth_t>(lhsWidth));
                        bool allOnes = true;
                        for (int i = 0; i < lhsWidth; ++i)
                        {
                            if (!resizedRhs[i])
                            {
                                allOnes = false;
                                break;
                            }
                        }
                        
                        if (allOnes)
                        {
                            // Replace with constant 1'b1
                            slang::SVInt trueValue(1, 1, false);
                            auto onError = [&](const std::string &msg)
                            { this->error(ctx.graph, op, msg); ctx.failed = true; };
                            wolvrix::lib::grh::ValueId newValue =
                                createReplacementConstant(ctx.graph, *ctx.pool, ctx.constants,
                                                          ctx.protectDeclaredSymbols, op, 0, resultId, trueValue);
                            replaceUsers(ctx.graph, resultId, newValue, onError);
                            if (ctx.protectDeclaredSymbols && isDeclaredValue(ctx.graph, resultId))
                            {
                                recordDeclaredConst(declaredAssigns, op, resultId, formatConstLiteral(trueValue));
                            }
                            opsToErase.push_back(opId);
                            simplified = true;
                            ++ctx.unsignedCmpSimplified;
                        }
                    }
                }
            }
        }
        
        for (const auto opId : opsToErase)
        {
            if (!ctx.graph.eraseOp(opId))
            {
                const wolvrix::lib::grh::Operation op = ctx.graph.getOperation(opId);
                error(ctx.graph, op, "Failed to erase simplified unsigned comparison op");
                ctx.failed = true;
            }
            else
            {
                ++ctx.opsErased;
                materializeDeclaredSymbols(ctx.graph, declaredAssigns, opId,
                                           [&](const std::string &msg)
                                           { this->error(ctx.graph, msg); ctx.failed = true; });
            }
        }
        
        return simplified;
    }

    bool ConstantFoldPass::processSingleGraph(GraphFoldContext &ctx)
    {
        bool graphChanged = false;

        const bool phaseTiming = std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr;
        auto phaseStart = std::chrono::steady_clock::now();
        auto phaseTick = [&](const char *name) {
            if (!phaseTiming)
            {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[const-fold-phase] graph=%s phase=%s %lldms\n",
                         ctx.graph.symbol().c_str(), name,
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    now - phaseStart)
                                                    .count()));
            phaseStart = now;
        };

        // Phase 1: Collect constants from existing kConstant ops and dedupe
        graphChanged = collectConstants(ctx) || graphChanged;
        phaseTick("collectConstants");

        // Phase 2: Iterative folding until convergence or max iterations
        graphChanged = iterativeFolding(ctx) || graphChanged;
        phaseTick("iterativeFolding");

        // Phase 3: Simplify slice operations
        graphChanged = simplifySlices(ctx) || graphChanged;
        phaseTick("simplifySlices");

        // Phase 4: Eliminate dead constants
        graphChanged = eliminateDeadConstants(ctx) || graphChanged;
        phaseTick("eliminateDeadConstants");

        // Phase 5: Simplify unsigned comparisons (e.g., x >= 0, x <= MAX)
        graphChanged = simplifyUnsignedComparisons(ctx) || graphChanged;
        phaseTick("simplifyUnsignedComparisons");

        return graphChanged;
    }

    PassResult ConstantFoldPass::run()
    {
        PassResult result;
        ConstantStore constants;
        bool failed = false;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));
        std::size_t changedGraphs = 0;
        std::size_t totalDedupedConstants = 0;
        std::size_t totalFoldedOps = 0;
        std::size_t totalSimplifiedSlices = 0;
        std::size_t totalDeadConstants = 0;
        std::size_t totalUnsignedCmp = 0;
        std::size_t totalOpsErased = 0;

        auto handleException = [&](const std::exception &ex)
        {
            this->error(std::string("Unhandled exception: ") + ex.what());
            failed = true;
        };

        try
        {
            // Iterate over all graphs
            for (const auto &graphEntry : design().graphs())
            {
                wolvrix::lib::grh::Graph &graph = *graphEntry.second;

                // Create per-graph context with fresh state
                GraphFoldContext ctx{
                    graph,
                    constants,  // Shared across graphs (constants can reference values from other graphs)
                    std::make_unique<ConstantPool>(),  // Fresh constant pool for this graph
                    {},         // Fresh folded operations set
                    failed
                };
                ctx.protectDeclaredSymbols = keepDeclaredSymbols();

                // Process the graph
                bool graphChanged = processSingleGraph(ctx);
                result.changed = result.changed || graphChanged;
                if (graphChanged)
                {
                    ++changedGraphs;
                }
                totalDedupedConstants += ctx.dedupedConstants;
                totalFoldedOps += ctx.foldedOpsCount;
                totalSimplifiedSlices += ctx.simplifiedSlices;
                totalDeadConstants += ctx.deadConstantsRemoved;
                totalUnsignedCmp += ctx.unsignedCmpSimplified;
                totalOpsErased += ctx.opsErased;
            }

            if (failed)
            {
                result.failed = true;
            }

            std::string message = "graphs=" + std::to_string(graphCount);
            message.append(", changedGraphs=");
            message.append(std::to_string(changedGraphs));
            message.append(", foldedOps=");
            message.append(std::to_string(totalFoldedOps));
            message.append(", dedupedConsts=");
            message.append(std::to_string(totalDedupedConstants));
            message.append(", sliceSimplified=");
            message.append(std::to_string(totalSimplifiedSlices));
            message.append(", deadConsts=");
            message.append(std::to_string(totalDeadConstants));
            message.append(", unsignedCmp=");
            message.append(std::to_string(totalUnsignedCmp));
            message.append(", opsErased=");
            message.append(std::to_string(totalOpsErased));
            message.append(result.failed ? ", failed=true" : ", failed=false");
            logDebug(std::move(message));
            return result;
        }
        catch (const std::exception &ex)
        {
            handleException(ex);
            logError(std::string("aborted: ") + ex.what());
            result.failed = true;
            return result;
        }
    }

} // namespace wolvrix::lib::transform
