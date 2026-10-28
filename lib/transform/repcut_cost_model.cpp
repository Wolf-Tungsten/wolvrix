#include "transform/repcut_cost_model.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace wolvrix::lib::transform
{
    namespace
    {
        using grh::OperationKind;

        bool legacyIsCombOp(const grh::Operation &op)
        {
            const auto events = op.attr("eventEdge");
            if (events)
            {
                if (const auto *edges = std::get_if<std::vector<std::string>>(&*events);
                    edges && !edges->empty())
                    return false;
            }
            if (op.kind() == OperationKind::kSystemFunction)
            {
                const auto attr = op.attr("hasSideEffects");
                const auto *sideEffects = attr ? std::get_if<bool>(&*attr) : nullptr;
                return !sideEffects || !*sideEffects;
            }
            return (op.kind() >= OperationKind::kConstant && op.kind() <= OperationKind::kSliceArray) ||
                   op.kind() == OperationKind::kMemoryReadPort;
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

        uint32_t legacyWeight(const grh::Graph &graph, const grh::Operation &op)
        {
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
                if (legacyIsCombOp(op))
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

            return roundedWeight > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())
                       ? std::numeric_limits<uint32_t>::max()
                       : static_cast<uint32_t>(roundedWeight);
        }

        uint64_t workWords(int32_t width)
        {
            // Verilator uses native integers through 64 bits, then 32-bit WData limbs.
            return width <= 64 ? 1u : (static_cast<uint64_t>(width) + 31u) / 32u;
        }

        std::optional<uint64_t> constantMaskOnes(
            const grh::Graph &graph, const grh::Operation &op, std::size_t index, int32_t width)
        {
            if (index >= op.operands().size())
                return std::nullopt;
            const auto def = graph.valueDef(op.operands()[index]);
            if (!def.valid() || graph.opKind(def) != OperationKind::kConstant)
                return std::nullopt;
            const auto attr = graph.getOperation(def).attr("constValue");
            const auto *literal = attr ? std::get_if<std::string>(&*attr) : nullptr;
            if (!literal)
                return std::nullopt;
            if (*literal == "'0")
                return 0;
            // Other unbased literals are left conservative until emitter handling is aligned.
            try
            {
                auto bits = slang::SVInt::fromString(*literal).resize(width);
                if (!bits.hasUnknown())
                    return bits.countOnes();
            }
            catch (const std::exception &)
            {
                // Unrecognized constants retain the conservative dynamic-mask rule.
            }
            return std::nullopt;
        }

        std::optional<uint64_t> constantMaskRuns(
            const grh::Graph &graph, const grh::Operation &op, std::size_t index, int32_t width)
        {
            if (index >= op.operands().size())
                return std::nullopt;
            const auto def = graph.valueDef(op.operands()[index]);
            if (!def.valid() || graph.opKind(def) != OperationKind::kConstant)
                return std::nullopt;
            const auto attr = graph.getOperation(def).attr("constValue");
            const auto *literal = attr ? std::get_if<std::string>(&*attr) : nullptr;
            if (!literal || width <= 0)
                return std::nullopt;
            if (*literal == "'0")
                return 0;
            try
            {
                auto bits = slang::SVInt::fromString(*literal).resize(width);
                if (bits.hasUnknown())
                    return std::nullopt;
                uint64_t runs = 0;
                bool inRun = false;
                for (int32_t bit = 0; bit < width; ++bit)
                {
                    const bool one = bits[static_cast<std::size_t>(bit)].toChar() == '1';
                    if (one && !inRun)
                    {
                        ++runs;
                    }
                    inRun = one;
                }
                return runs;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<uint64_t> constantUnsignedValue(
            const grh::Graph &graph, const grh::Operation &op, std::size_t index)
        {
            if (index >= op.operands().size())
                return std::nullopt;
            const auto def = graph.valueDef(op.operands()[index]);
            if (!def.valid() || graph.opKind(def) != OperationKind::kConstant)
                return std::nullopt;
            const auto attr = graph.getOperation(def).attr("constValue");
            const auto *literal = attr ? std::get_if<std::string>(&*attr) : nullptr;
            if (!literal)
                return std::nullopt;
            try
            {
                const auto bits = slang::SVInt::fromString(*literal);
                if (bits.hasUnknown())
                    return std::nullopt;
                uint64_t value = 0;
                const auto width = bits.getBitWidth();
                for (std::size_t bit = 0; bit < width; ++bit)
                {
                    if (bits[bit].toChar() != '1')
                        continue;
                    if (bit >= 64)
                        return std::numeric_limits<uint64_t>::max();
                    value |= uint64_t{1} << bit;
                }
                return value;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<uint64_t> verilatorWeight(const grh::Graph &graph, const grh::Operation &op)
        {
            const auto resultWidth = getResultWidth(graph, op);
            const auto operandWidth = getMaxOperandWidth(graph, op);
            const uint64_t outWords = workWords(resultWidth);
            const uint64_t inWords = workWords(operandWidth);
            switch (op.kind())
            {
            case OperationKind::kAdd:
            case OperationKind::kSub:
                return 2u * outWords;
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
            case OperationKind::kAssign:
            case OperationKind::kReplicate:
                return outWords;
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceNor:
            case OperationKind::kReduceNand:
            case OperationKind::kLogicNot:
                return inWords;
            case OperationKind::kReduceXor:
            case OperationKind::kReduceXnor:
                return 2u * inWords;
            case OperationKind::kLogicAnd:
            case OperationKind::kLogicOr:
            {
                uint64_t words = 0;
                for (const auto operand : op.operands())
                    words += workWords(safeValueWidth(graph, operand));
                return words + 1u;
            }
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
                return 2u * inWords;
            case OperationKind::kMux:
                return 1u + outWords;
            case OperationKind::kConcat:
                return outWords + op.operands().size();
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
                return 2u * outWords;
            case OperationKind::kSliceStatic:
                return outWords;
            case OperationKind::kSliceDynamic:
            case OperationKind::kSliceArray:
                // A narrow indexed select reads adjacent words, not the entire input.
                return 2u + 2u * outWords + (op.kind() == OperationKind::kSliceArray ? 1u : 0u);
            case OperationKind::kRegisterWritePort:
            case OperationKind::kLatchWritePort:
            case OperationKind::kMemoryWritePort:
            {
                const bool memory = op.kind() == OperationKind::kMemoryWritePort;
                const auto dataIndex = memory ? 2u : 1u;
                const auto maskIndex = memory ? 3u : 2u;
                const int32_t width = getOperandWidth(graph, op, dataIndex);
                const uint64_t words = workWords(width);
                const auto ones = constantMaskOnes(graph, op, maskIndex, width);
                if (ones && *ones == 0)
                    return 1u;
                if (ones && *ones == static_cast<uint64_t>(width))
                    return 1u + words;
                if (memory)
                {
                    // The SV emitter emits per-bit writes for a partial/dynamic memory mask.
                    return 1u + (ones ? *ones : 2u * static_cast<uint64_t>(width));
                }
                // Partial register/latch writes may be coalesced by the backend.
                // Keep legacy until generated-code probes establish their lowering cost.
                return std::nullopt;
            }
            default:
                // Multiplication, division, memory reads and effects need separate calibration.
                return std::nullopt;
            }
        }

        uint64_t grhsimWords(int32_t width)
        {
            return (static_cast<uint64_t>(width) + 63u) / 64u;
        }

        uint64_t grhsimBoundaryStorageWeight(const grh::Graph &graph, const grh::Operation &op)
        {
            // Scheduling is not available before partitioning. Distinct
            // consumers are a structural proxy for a supernode boundary;
            // a staged-write operand definitely survives the compute phase.
            // Constants, aliases and direct state views do not create stores.
            if (!legacyIsCombOp(op) || op.kind() == OperationKind::kConstant ||
                op.kind() == OperationKind::kAssign)
                return 0u;
            uint64_t weight = 0u;
            for (const auto result : op.results())
            {
                if (graph.valueType(result) != grh::ValueType::Logic)
                    continue;
                grh::OperationId firstUser;
                bool distinctConsumers = false;
                bool staged = false;
                const auto valueInfo = graph.getValue(result);
                for (const auto &user : valueInfo.users())
                {
                    if (!user.operation.valid())
                        continue;
                    if (!firstUser.valid())
                        firstUser = user.operation;
                    else if (firstUser != user.operation)
                        distinctConsumers = true;
                    const auto kind = graph.opKind(user.operation);
                    staged |= kind == OperationKind::kRegisterWritePort ||
                              kind == OperationKind::kLatchWritePort ||
                              kind == OperationKind::kMemoryWritePort;
                    if (distinctConsumers)
                        break;
                }
                const auto words = grhsimWords(safeValueWidth(graph, result));
                // Old-value comparison, persistent store and dirty marking.
                // A local value used twice by ONE operation is not a boundary.
                if (distinctConsumers)
                    weight += 2u * words + 1u;
                else if (staged)
                    weight += words;
            }
            return weight;
        }

        std::optional<uint64_t> grhsimWeight(const grh::Graph &graph, const grh::Operation &op)
        {
            // Static work per activation, not a prediction of toggle frequency.
            // Do not charge per-bit SV staging or Verilator's 32-bit WData here.
            const uint64_t outWords = grhsimWords(getResultWidth(graph, op));
            const uint64_t inWords = grhsimWords(getMaxOperandWidth(graph, op));
            switch (op.kind())
            {
            case OperationKind::kConstant:
            case OperationKind::kRegister:
            case OperationKind::kLatch:
            case OperationKind::kMemory:
                // Solver weights must remain positive; storage declarations and
                // folded constants have no recurring word-copy work.
                return 1u;
            case OperationKind::kAssign:
            case OperationKind::kRegisterReadPort:
            case OperationKind::kLatchReadPort:
            case OperationKind::kAnd:
            case OperationKind::kOr:
            case OperationKind::kXor:
            case OperationKind::kXnor:
            case OperationKind::kNot:
            case OperationKind::kSliceStatic:
                return outWords;
            case OperationKind::kAdd:
            case OperationKind::kSub:
                // Native integer through 64 bits, carry propagation above it.
                return outWords == 1u ? 1u : 3u * outWords;
            case OperationKind::kMul:
                return outWords == 1u ? 3u : 3u * outWords * outWords;
            case OperationKind::kDiv:
            case OperationKind::kMod:
                return outWords == 1u ? 8u : 64u * outWords * outWords;
            case OperationKind::kReduceAnd:
            case OperationKind::kReduceOr:
            case OperationKind::kReduceNor:
            case OperationKind::kReduceNand:
            case OperationKind::kLogicNot:
                return inWords;
            case OperationKind::kReduceXor:
            case OperationKind::kReduceXnor:
                return 2u * inWords;
            case OperationKind::kLogicAnd:
            case OperationKind::kLogicOr:
            {
                uint64_t words = 1u;
                for (const auto value : op.operands())
                {
                    // Boolean inputs need no extra truth-value conversion.
                    if (safeValueWidth(graph, value) > 1)
                        words += grhsimWords(safeValueWidth(graph, value));
                }
                return words;
            }
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
                return inWords == 1u ? 1u : 2u * inWords;
            case OperationKind::kMux:
                return 1u + outWords;
            case OperationKind::kConcat:
            {
                uint64_t words = 0;
                for (const auto value : op.operands())
                    words += grhsimWords(safeValueWidth(graph, value));
                // Word-wise insertion, not one iteration per result bit.
                return outWords + words;
            }
            case OperationKind::kReplicate:
                return 2u * outWords;
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
                return constantUnsignedValue(graph, op, 1) ? outWords : 2u * outWords;
            case OperationKind::kSliceDynamic:
            case OperationKind::kSliceArray:
                return 2u + 2u * outWords;
            case OperationKind::kMemoryReadPort:
                return 2u + outWords;
            case OperationKind::kRegisterWritePort:
            case OperationKind::kLatchWritePort:
            case OperationKind::kMemoryWritePort:
            {
                const bool memory = op.kind() == OperationKind::kMemoryWritePort;
                const int32_t width = getOperandWidth(graph, op, memory ? 2u : 1u);
                const uint64_t words = grhsimWords(width);
                const auto ones = constantMaskOnes(graph, op, memory ? 3u : 2u, width);
                if (ones && *ones == 0u)
                    return 1u;
                // Staging, state comparison and commit copy, plus the shared
                // enable/dirty-activation bookkeeping. Partial masks add a
                // word-wise merge; they never imply a per-bit write loop.
                const uint64_t wordPasses = ones && *ones == static_cast<uint64_t>(width) ? 3u : 5u;
                return 2u + wordPasses * words;
            }
            default:
                // DPI/system effects and unfamiliar kinds retain an explicit
                // fallback; their external work cannot be inferred from GRH.
                return std::nullopt;
            }
        }

        std::optional<uint64_t> verilatorLoweredWeight(const grh::Graph &graph, const grh::Operation &op)
        {
            const auto resultWidth = getResultWidth(graph, op);
            const auto operandWidth = getMaxOperandWidth(graph, op);
            const uint64_t outWords = workWords(resultWidth);
            const uint64_t inWords = workWords(operandWidth);
            switch (op.kind())
            {
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
                // A constant shift is folded to a fixed shift/mask sequence by
                // the generated-code compiler.  Keep one word of expression
                // overhead instead of charging the dynamic-shift path twice.
                return constantUnsignedValue(graph, op, 1) ? 1u + outWords : 2u * outWords;
            case OperationKind::kRegisterWritePort:
            case OperationKind::kLatchWritePort:
            case OperationKind::kMemoryWritePort:
            {
                const bool memory = op.kind() == OperationKind::kMemoryWritePort;
                const auto dataIndex = memory ? 2u : 1u;
                const auto maskIndex = memory ? 3u : 2u;
                const int32_t width = getOperandWidth(graph, op, dataIndex);
                const uint64_t words = workWords(width);
                const auto runs = constantMaskRuns(graph, op, maskIndex, width);
                if (!runs)
                {
                    // Unknown/dynamic masks still use the conservative bit
                    // staging estimate used by verilator-v1.
                    return memory ? std::optional<uint64_t>(1u + 2u * static_cast<uint64_t>(width))
                                   : std::nullopt;
                }
                if (*runs == 0)
                    return 1u;
                if (*runs == 1)
                {
                    // One contiguous run is emitted as one NBA/part-select;
                    // the run width is approximated by the enclosing data word
                    // count, which is available before C++ generation.
                    return 1u + words;
                }
                return 1u + *runs + words;
            }
            default:
                return verilatorWeight(graph, op);
            }
        }
    }

    std::string_view repcutCostModelName(RepcutCostModel model)
    {
        switch (model)
        {
        case RepcutCostModel::kLegacy: return "legacy";
        case RepcutCostModel::kVerilatorV1: return "verilator-v1";
        case RepcutCostModel::kVerilatorLoweredV1: return "verilator-lowered-v1";
        case RepcutCostModel::kGrhsimV1: return "grhsim-v1";
        }
        return "unknown";
    }

    RepcutOperationCost estimateRepcutOperationCost(
        const grh::Graph &graph, const grh::Operation &op, RepcutCostModel model)
    {
        if (model == RepcutCostModel::kLegacy)
            return {legacyWeight(graph, op), false};
        if (model == RepcutCostModel::kGrhsimV1)
        {
            if (const auto weight = grhsimWeight(graph, op))
            {
                return {static_cast<uint32_t>(std::clamp<uint64_t>(
                            *weight + grhsimBoundaryStorageWeight(graph, op), 1u,
                            std::numeric_limits<uint32_t>::max() - 1u)), false};
            }
            return {legacyWeight(graph, op), true};
        }
        if (model != RepcutCostModel::kVerilatorV1)
        {
            if (model != RepcutCostModel::kVerilatorLoweredV1)
                throw std::invalid_argument("unknown repcut cost model");
            if (const auto weight = verilatorLoweredWeight(graph, op))
            {
                return {static_cast<uint32_t>(std::clamp<uint64_t>(
                            *weight, 1u, std::numeric_limits<uint32_t>::max() - 1u)), false};
            }
            return {legacyWeight(graph, op), true};
        }
        if (const auto weight = verilatorWeight(graph, op))
        {
            // UINT32_MAX is reserved by the caller as the uncomputed-cache sentinel.
            return {static_cast<uint32_t>(std::clamp<uint64_t>(
                        *weight, 1u, std::numeric_limits<uint32_t>::max() - 1u)), false};
        }
        return {legacyWeight(graph, op), true};
    }
}
