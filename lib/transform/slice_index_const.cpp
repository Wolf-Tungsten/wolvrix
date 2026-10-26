#include "transform/slice_index_const.hpp"

#include "core/grh.hpp"

#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>

namespace wolvrix::lib::transform
{

    namespace
    {
        std::optional<int64_t> parseConstIntLiteral(std::string_view literal)
        {
            if (literal.empty())
            {
                return std::nullopt;
            }
            if (literal.front() == '"' || literal.front() == '$')
            {
                return std::nullopt;
            }
            std::string cleaned;
            cleaned.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                cleaned.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (cleaned.empty())
            {
                return std::nullopt;
            }
            int base = 10;
            std::string digits;
            const std::size_t tick = cleaned.find('\'');
            if (tick != std::string::npos)
            {
                if (tick + 1 >= cleaned.size())
                {
                    return std::nullopt;
                }
                const char baseChar = cleaned[tick + 1];
                switch (baseChar)
                {
                case 'b':
                    base = 2;
                    break;
                case 'o':
                    base = 8;
                    break;
                case 'd':
                    base = 10;
                    break;
                case 'h':
                    base = 16;
                    break;
                default:
                    return std::nullopt;
                }
                if (tick + 2 >= cleaned.size())
                {
                    return std::nullopt;
                }
                digits = cleaned.substr(tick + 2);
            }
            else
            {
                digits = cleaned;
                if (digits.rfind("0x", 0) == 0)
                {
                    base = 16;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0b", 0) == 0)
                {
                    base = 2;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0o", 0) == 0)
                {
                    base = 8;
                    digits = digits.substr(2);
                }
            }
            if (digits.empty())
            {
                return std::nullopt;
            }
            for (char ch : digits)
            {
                if (ch == 'x' || ch == 'z' || ch == '?')
                {
                    return std::nullopt;
                }
            }
            try
            {
                return std::stoll(digits, nullptr, base);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<int64_t> evalConstInt(const wolvrix::lib::grh::Graph &graph,
                                            wolvrix::lib::grh::ValueId valueId,
                                            std::unordered_set<wolvrix::lib::grh::ValueId,
                                                               wolvrix::lib::grh::ValueIdHash> &visiting)
        {
            if (!valueId.valid())
            {
                return std::nullopt;
            }
            if (visiting.find(valueId) != visiting.end())
            {
                return std::nullopt;
            }
            visiting.insert(valueId);
            const wolvrix::lib::grh::Value value = graph.getValue(valueId);
            const wolvrix::lib::grh::OperationId defId = value.definingOp();
            if (!defId.valid())
            {
                visiting.erase(valueId);
                return std::nullopt;
            }
            const wolvrix::lib::grh::Operation defOp = graph.getOperation(defId);
            const auto operands = defOp.operands();

            switch (defOp.kind())
            {
            case wolvrix::lib::grh::OperationKind::kConstant:
            {
                auto attr = defOp.attr("constValue");
                if (!attr)
                {
                    visiting.erase(valueId);
                    return std::nullopt;
                }
                if (const auto *literal = std::get_if<std::string>(&*attr))
                {
                    auto parsed = parseConstIntLiteral(*literal);
                    if (!parsed || *parsed < 0)
                    {
                        visiting.erase(valueId);
                        return std::nullopt;
                    }
                    visiting.erase(valueId);
                    return parsed;
                }
                visiting.erase(valueId);
                return std::nullopt;
            }
            case wolvrix::lib::grh::OperationKind::kAssign:
                if (operands.size() != 1)
                {
                    break;
                }
                if (auto val = evalConstInt(graph, operands[0], visiting))
                {
                    visiting.erase(valueId);
                    return val;
                }
                break;
            case wolvrix::lib::grh::OperationKind::kSliceStatic:
            {
                if (operands.size() != 1)
                {
                    break;
                }
                auto startAttr = defOp.attr("sliceStart");
                auto endAttr = defOp.attr("sliceEnd");
                if (!startAttr || !endAttr)
                {
                    break;
                }
                const auto *startPtr = std::get_if<int64_t>(&*startAttr);
                const auto *endPtr = std::get_if<int64_t>(&*endAttr);
                if (!startPtr || !endPtr)
                {
                    break;
                }
                const int64_t start = *startPtr;
                const int64_t end = *endPtr;
                if (start < 0 || end < start)
                {
                    break;
                }
                const int64_t width = end - start + 1;
                if (width <= 0 || width > 63)
                {
                    break;
                }
                auto base = evalConstInt(graph, operands[0], visiting);
                if (!base)
                {
                    break;
                }
                const uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1ULL);
                const uint64_t sliced = (static_cast<uint64_t>(*base) >> static_cast<uint64_t>(start)) & mask;
                visiting.erase(valueId);
                return static_cast<int64_t>(sliced);
            }
            case wolvrix::lib::grh::OperationKind::kConcat:
            {
                if (operands.empty())
                {
                    break;
                }
                uint64_t acc = 0;
                int64_t totalWidth = 0;
                for (const auto operand : operands)
                {
                    const int64_t width = graph.getValue(operand).width();
                    if (width <= 0 || width > 63)
                    {
                        totalWidth = -1;
                        break;
                    }
                    auto part = evalConstInt(graph, operand, visiting);
                    if (!part)
                    {
                        totalWidth = -1;
                        break;
                    }
                    if (totalWidth + width > 63)
                    {
                        totalWidth = -1;
                        break;
                    }
                    acc = (acc << width) | (static_cast<uint64_t>(*part) & ((1ULL << width) - 1ULL));
                    totalWidth += width;
                }
                if (totalWidth >= 0)
                {
                    visiting.erase(valueId);
                    return static_cast<int64_t>(acc);
                }
                break;
            }
            case wolvrix::lib::grh::OperationKind::kAdd:
            case wolvrix::lib::grh::OperationKind::kSub:
            case wolvrix::lib::grh::OperationKind::kMul:
            case wolvrix::lib::grh::OperationKind::kShl:
            case wolvrix::lib::grh::OperationKind::kLShr:
            {
                if (operands.size() != 2)
                {
                    break;
                }
                auto lhs = evalConstInt(graph, operands[0], visiting);
                auto rhs = evalConstInt(graph, operands[1], visiting);
                if (!lhs || !rhs)
                {
                    break;
                }
                if (*lhs < 0 || *rhs < 0)
                {
                    break;
                }
                __int128 result = 0;
                switch (defOp.kind())
                {
                case wolvrix::lib::grh::OperationKind::kAdd:
                    result = static_cast<__int128>(*lhs) + static_cast<__int128>(*rhs);
                    break;
                case wolvrix::lib::grh::OperationKind::kSub:
                    result = static_cast<__int128>(*lhs) - static_cast<__int128>(*rhs);
                    break;
                case wolvrix::lib::grh::OperationKind::kMul:
                    result = static_cast<__int128>(*lhs) * static_cast<__int128>(*rhs);
                    break;
                case wolvrix::lib::grh::OperationKind::kShl:
                    if (*rhs > 62)
                    {
                        break;
                    }
                    result = static_cast<__int128>(*lhs) << static_cast<int>(*rhs);
                    break;
                case wolvrix::lib::grh::OperationKind::kLShr:
                    if (*rhs > 62)
                    {
                        break;
                    }
                    result = static_cast<__int128>(*lhs) >> static_cast<int>(*rhs);
                    break;
                default:
                    break;
                }
                if (result < 0 || result > std::numeric_limits<int64_t>::max())
                {
                    break;
                }
                visiting.erase(valueId);
                return static_cast<int64_t>(result);
            }
            default:
                break;
            }
            visiting.erase(valueId);
            return std::nullopt;
        }

        std::optional<int64_t> getConstIntValue(const wolvrix::lib::grh::Graph &graph,
                                                wolvrix::lib::grh::ValueId valueId)
        {
            std::unordered_set<wolvrix::lib::grh::ValueId,
                               wolvrix::lib::grh::ValueIdHash> visiting;
            return evalConstInt(graph, valueId, visiting);
        }
    } // namespace

    SliceIndexConstPass::SliceIndexConstPass()
        : Pass("slice-index-const", "slice-index-const",
               "Convert constant-index dynamic slices into static slices")
    {
    }

    PassResult SliceIndexConstPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        for (const auto &entry : design().graphs())
        {
            wolvrix::lib::grh::Graph &graph = *entry.second;
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kSliceDynamic)
                {
                    continue;
                }
                const auto operands = op.operands();
                const auto results = op.results();
                if (operands.size() != 2 || results.size() != 1)
                {
                    continue;
                }
                const wolvrix::lib::grh::ValueId base = operands[0];
                const wolvrix::lib::grh::ValueId index = operands[1];
                const wolvrix::lib::grh::ValueId resultId = results[0];
                if (!base.valid() || !index.valid() || !resultId.valid())
                {
                    continue;
                }

                auto widthAttr = op.attr("sliceWidth");
                if (!widthAttr)
                {
                    continue;
                }
                const auto *widthPtr = std::get_if<int64_t>(&*widthAttr);
                if (!widthPtr || *widthPtr <= 0)
                {
                    continue;
                }
                const int64_t sliceWidth = *widthPtr;
                const auto indexOpt = getConstIntValue(graph, index);
                if (!indexOpt)
                {
                    continue;
                }
                const int64_t start = *indexOpt;
                const int64_t end = start + sliceWidth - 1;
                const int64_t baseWidth = graph.getValue(base).width();
                const int64_t resultWidth = graph.getValue(resultId).width();
                if (baseWidth <= 0 || resultWidth != sliceWidth)
                {
                    continue;
                }
                if (start < 0 || end < start || end >= baseWidth)
                {
                    continue;
                }

                graph.setAttr(opId, "sliceStart", start);
                graph.setAttr(opId, "sliceEnd", end);
                graph.eraseAttr(opId, "sliceWidth");
                graph.setOpKind(opId, wolvrix::lib::grh::OperationKind::kSliceStatic);
                graph.eraseOperand(opId, 1);

                result.changed = true;
                logDebug("slice-index-const: converted dynamic slice to static range");
            }
        }

        return result;
    }

} // namespace wolvrix::lib::transform
