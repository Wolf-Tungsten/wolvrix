#include "emit/system_verilog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <queue>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <unordered_map>
#include <tuple>
#include <string_view>

#include "slang/numeric/SVInt.h"

namespace wolvrix::lib::emit
{

    namespace
    {
        std::string escapeSvString(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (unsigned char ch : text)
            {
                switch (ch)
                {
                case '\\':
                    out.append("\\\\");
                    break;
                case '"':
                    out.append("\\\"");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                case '\b':
                    out.append("\\b");
                    break;
                case '\f':
                    out.append("\\f");
                    break;
                case '\v':
                    out.append("\\v");
                    break;
                default:
                    if (ch < 0x20 || ch == 0x7f)
                    {
                        char buf[5];
                        std::snprintf(buf, sizeof(buf), "\\x%02x", ch);
                        out.append(buf);
                    }
                    else
                    {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }
            return out;
        }

        std::optional<std::string> sizedLiteralIfUnsized(std::string_view literal, int64_t width)
        {
            if (width <= 0)
            {
                return std::nullopt;
            }
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_')
                {
                    continue;
                }
                compact.push_back(ch);
            }
            if (compact.empty())
            {
                return std::nullopt;
            }
            std::size_t start = 0;
            bool negative = false;
            if (compact[0] == '-' || compact[0] == '+')
            {
                negative = compact[0] == '-';
                start = 1;
            }
            auto allDigits = [&](std::size_t from, std::size_t to) -> bool
            {
                if (from >= to)
                {
                    return false;
                }
                for (std::size_t i = from; i < to; ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(compact[i])))
                    {
                        return false;
                    }
                }
                return true;
            };
            const std::size_t quotePos = compact.find('\'', start);
            if (quotePos != std::string::npos)
            {
                if (quotePos > start && allDigits(start, quotePos))
                {
                    return std::nullopt;
                }
                if (quotePos + 1 >= compact.size())
                {
                    return std::nullopt;
                }
                const char baseChar = compact[quotePos + 1];
                char base = 'b';
                std::string digits;
                if (baseChar == 'b' || baseChar == 'B' ||
                    baseChar == 'd' || baseChar == 'D' ||
                    baseChar == 'h' || baseChar == 'H' ||
                    baseChar == 'o' || baseChar == 'O')
                {
                    base = static_cast<char>(std::tolower(static_cast<unsigned char>(baseChar)));
                    digits = compact.substr(quotePos + 2);
                    if (digits.empty())
                    {
                        digits = "0";
                    }
                }
                else
                {
                    digits = compact.substr(quotePos + 1);
                }
                std::string sized = std::to_string(width);
                sized.push_back('\'');
                sized.push_back(base);
                sized.append(digits);
                if (negative)
                {
                    sized.insert(sized.begin(), '-');
                }
                return sized;
            }
            if (!allDigits(start, compact.size()))
            {
                return std::nullopt;
            }
            std::string sized = std::to_string(width);
            sized.append("'d");
            sized.append(compact.substr(start));
            if (negative)
            {
                sized.insert(sized.begin(), '-');
            }
            return sized;
        }

        std::optional<std::string> signBitLiteralForConst(std::string_view literal,
                                                          int64_t width,
                                                          bool isSigned)
        {
            if (width <= 0)
            {
                return std::nullopt;
            }
            std::string compact;
            compact.reserve(literal.size());
            for (char ch : literal)
            {
                if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_')
                {
                    continue;
                }
                compact.push_back(ch);
            }
            if (compact.empty())
            {
                return std::nullopt;
            }
            if (compact.size() >= 2 && compact.front() == '(' && compact.back() == ')')
            {
                bool nested = false;
                for (std::size_t i = 1; i + 1 < compact.size(); ++i)
                {
                    if (compact[i] == '(' || compact[i] == ')')
                    {
                        nested = true;
                        break;
                    }
                }
                if (!nested)
                {
                    compact = compact.substr(1, compact.size() - 2);
                }
            }
            if (compact.empty())
            {
                return std::nullopt;
            }
            bool negative = false;
            if (compact.front() == '-' || compact.front() == '+')
            {
                negative = compact.front() == '-';
                compact.erase(compact.begin());
            }
            if (compact.empty())
            {
                return std::nullopt;
            }
            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(compact);
                if (negative)
                {
                    parsed = -parsed;
                }
                parsed.setSigned(isSigned);
                parsed = parsed.resize(static_cast<slang::bitwidth_t>(width));
                const slang::logic_t bit = parsed[static_cast<int32_t>(width - 1)];
                char bitChar = bit.toChar();
                bitChar = static_cast<char>(std::tolower(static_cast<unsigned char>(bitChar)));
                std::string out = "1'b";
                out.push_back(bitChar);
                return out;
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<std::vector<uint8_t>> parseConstMaskBits(std::string_view literal,
                                                               int64_t targetWidth)
        {
            if (targetWidth <= 0)
            {
                return std::nullopt;
            }

            std::string text;
            text.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                text.push_back(ch);
            }
            if (text.empty() || text.front() == '-')
            {
                return std::nullopt;
            }

            bool isSigned = false;
            int64_t literalWidth = -1;
            char base = 'd';
            std::string digits;

            const std::size_t quotePos = text.find('\'');
            if (quotePos != std::string::npos)
            {
                const std::string widthText = text.substr(0, quotePos);
                if (!widthText.empty())
                {
                    for (char ch : widthText)
                    {
                        if (!std::isdigit(static_cast<unsigned char>(ch)))
                        {
                            return std::nullopt;
                        }
                    }
                    try
                    {
                        literalWidth = std::stoll(widthText);
                    }
                    catch (const std::exception &)
                    {
                        return std::nullopt;
                    }
                    if (literalWidth <= 0)
                    {
                        return std::nullopt;
                    }
                }
                std::size_t basePos = quotePos + 1;
                if (basePos >= text.size())
                {
                    return std::nullopt;
                }
                if (text[basePos] == 's' || text[basePos] == 'S')
                {
                    isSigned = true;
                    basePos++;
                }
                if (basePos >= text.size())
                {
                    return std::nullopt;
                }
                bool unbasedLiteral = false;
                const char unbased = text[basePos];
                if ((unbased == '0' || unbased == '1') && basePos + 1 == text.size())
                {
                    base = 'b';
                    literalWidth = targetWidth;
                    digits.assign(1, unbased);
                    unbasedLiteral = true;
                }
                if (!unbasedLiteral)
                {
                    base = static_cast<char>(std::tolower(static_cast<unsigned char>(text[basePos])));
                    basePos++;
                    digits = text.substr(basePos);
                }
            }
            else
            {
                digits = text;
            }

            if (digits.empty())
            {
                return std::nullopt;
            }

            std::vector<uint8_t> bits;
            switch (base)
            {
            case 'b':
            {
                bits.reserve(digits.size());
                for (auto it = digits.rbegin(); it != digits.rend(); ++it)
                {
                    const char ch = *it;
                    if (ch == '0' || ch == '1')
                    {
                        bits.push_back(static_cast<uint8_t>(ch - '0'));
                    }
                    else
                    {
                        return std::nullopt;
                    }
                }
                break;
            }
            case 'h':
            {
                bits.reserve(digits.size() * 4);
                for (auto it = digits.rbegin(); it != digits.rend(); ++it)
                {
                    const char ch = *it;
                    int value = 0;
                    if (ch >= '0' && ch <= '9')
                    {
                        value = ch - '0';
                    }
                    else if (ch >= 'a' && ch <= 'f')
                    {
                        value = 10 + (ch - 'a');
                    }
                    else if (ch >= 'A' && ch <= 'F')
                    {
                        value = 10 + (ch - 'A');
                    }
                    else
                    {
                        return std::nullopt;
                    }
                    for (int bit = 0; bit < 4; ++bit)
                    {
                        bits.push_back(static_cast<uint8_t>((value >> bit) & 1));
                    }
                }
                break;
            }
            case 'o':
            {
                bits.reserve(digits.size() * 3);
                for (auto it = digits.rbegin(); it != digits.rend(); ++it)
                {
                    const char ch = *it;
                    if (ch < '0' || ch > '7')
                    {
                        return std::nullopt;
                    }
                    const int value = ch - '0';
                    for (int bit = 0; bit < 3; ++bit)
                    {
                        bits.push_back(static_cast<uint8_t>((value >> bit) & 1));
                    }
                }
                break;
            }
            case 'd':
            {
                for (char ch : digits)
                {
                    if (!std::isdigit(static_cast<unsigned char>(ch)))
                    {
                        return std::nullopt;
                    }
                }
                std::string dec = digits;
                if (dec == "0")
                {
                    bits.push_back(0);
                    break;
                }
                while (!(dec.size() == 1 && dec[0] == '0'))
                {
                    int carry = 0;
                    std::string next;
                    next.reserve(dec.size());
                    for (char ch : dec)
                    {
                        const int digit = ch - '0';
                        const int value = carry * 10 + digit;
                        const int q = value / 2;
                        carry = value % 2;
                        if (!next.empty() || q != 0)
                        {
                            next.push_back(static_cast<char>('0' + q));
                        }
                    }
                    bits.push_back(static_cast<uint8_t>(carry));
                    if (next.empty())
                    {
                        next = "0";
                    }
                    dec = std::move(next);
                }
                break;
            }
            default:
                return std::nullopt;
            }

            if (bits.empty())
            {
                bits.push_back(0);
            }

            auto resizeBits = [&](int64_t width, bool signedExtend)
            {
                if (width <= 0)
                {
                    return;
                }
                uint8_t fill = 0;
                if (signedExtend && !bits.empty())
                {
                    fill = bits.back();
                }
                if (static_cast<int64_t>(bits.size()) > width)
                {
                    bits.resize(static_cast<std::size_t>(width));
                }
                else if (static_cast<int64_t>(bits.size()) < width)
                {
                    bits.resize(static_cast<std::size_t>(width), fill);
                }
            };

            if (literalWidth > 0)
            {
                resizeBits(literalWidth, isSigned);
            }
            resizeBits(targetWidth, isSigned);

            return bits;
        }

        std::vector<const wolvrix::lib::grh::Graph *> graphsInDesignOrder(
            const wolvrix::lib::grh::Design &design,
            const std::unordered_set<std::string> &graphSymbols)
        {
            std::vector<const wolvrix::lib::grh::Graph *> graphs;
            graphs.reserve(graphSymbols.size());
            for (const auto &symbol : design.graphOrder())
            {
                if (!graphSymbols.contains(symbol))
                {
                    continue;
                }
                if (auto it = design.graphs().find(symbol); it != design.graphs().end())
                {
                    graphs.push_back(it->second.get());
                }
            }
            return graphs;
        }

        std::vector<const wolvrix::lib::grh::Graph *> reachableGraphsFromTops(
            const wolvrix::lib::grh::Design &design,
            std::span<const wolvrix::lib::grh::Graph *const> topGraphs)
        {
            std::unordered_set<std::string> reachableSymbols;
            std::vector<const wolvrix::lib::grh::Graph *> worklist;
            worklist.reserve(topGraphs.size());

            for (const wolvrix::lib::grh::Graph *graph : topGraphs)
            {
                if (graph == nullptr)
                {
                    continue;
                }
                if (reachableSymbols.insert(graph->symbol()).second)
                {
                    worklist.push_back(graph);
                }
            }

            for (std::size_t index = 0; index < worklist.size(); ++index)
            {
                const wolvrix::lib::grh::Graph *graph = worklist[index];
                if (graph == nullptr)
                {
                    continue;
                }

                for (const auto opId : graph->operations())
                {
                    if (graph->opKind(opId) != wolvrix::lib::grh::OperationKind::kInstance)
                    {
                        continue;
                    }

                    const auto op = graph->getOperation(opId);
                    const auto moduleAttr = op.attr("moduleName");
                    if (!moduleAttr)
                    {
                        continue;
                    }

                    const auto *moduleName = std::get_if<std::string>(&*moduleAttr);
                    if (moduleName == nullptr || moduleName->empty())
                    {
                        continue;
                    }

                    const wolvrix::lib::grh::Graph *targetGraph = design.findGraph(*moduleName);
                    if (targetGraph == nullptr)
                    {
                        continue;
                    }

                    if (reachableSymbols.insert(targetGraph->symbol()).second)
                    {
                        worklist.push_back(targetGraph);
                    }
                }
            }

            return graphsInDesignOrder(design, reachableSymbols);
        }

        std::string formatSrcAttribute(const std::optional<wolvrix::lib::grh::SrcLoc> &srcLoc)
        {
            if (!srcLoc || srcLoc->file.empty() || srcLoc->line == 0)
            {
                return {};
            }

            const uint32_t startLine = srcLoc->line;
            const uint32_t startCol = srcLoc->column;
            const uint32_t endLine = srcLoc->endLine ? srcLoc->endLine : startLine;
            const uint32_t endCol = srcLoc->endColumn ? srcLoc->endColumn : startCol;

            std::string file = srcLoc->file;
            for (char &ch : file)
            {
                if (ch == '\n' || ch == '\r')
                {
                    ch = ' ';
                }
            }
            std::string sanitized;
            sanitized.reserve(file.size() + 8);
            for (std::size_t i = 0; i < file.size(); ++i)
            {
                if (i + 1 < file.size() && file[i] == '*' && file[i + 1] == '/')
                {
                    sanitized.append("* /");
                    ++i;
                    continue;
                }
                if (i + 1 < file.size() && file[i] == '/' && file[i + 1] == '*')
                {
                    sanitized.append("/ *");
                    ++i;
                    continue;
                }
                sanitized.push_back(file[i]);
            }

            std::ostringstream oss;
            oss << "/* src: " << sanitized << ":" << startLine;
            if (startCol != 0)
            {
                oss << "." << startCol;
            }
            if (endLine != 0 || endCol != 0)
            {
                oss << "-" << endLine;
                if (endCol != 0)
                {
                    oss << "." << endCol;
                }
            }
            oss << " */";
            return oss.str();
        }

        std::string graphSymbolRequired(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::SymbolId sym, std::string_view context)
        {
            if (!sym.valid())
            {
                throw std::runtime_error(std::string(context) + " symbol is invalid during emit");
            }
            std::string text(graph.symbolText(sym));
            if (!text.empty())
            {
                return text;
            }
            throw std::runtime_error(std::string(context) + " symbol is empty during emit");
        }

        std::string valueSymbolRequired(const wolvrix::lib::grh::Value &value)
        {
            std::string sym(value.symbolText());
            if (!sym.empty())
            {
                return sym;
            }
            throw std::runtime_error("Value missing symbol during emit");
        }

        std::string valueSymbolRequired(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::ValueId value)
        {
            return graphSymbolRequired(graph, graph.valueSymbol(value), "Value");
        }

        std::string opSymbolRequired(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationId op)
        {
            return graphSymbolRequired(graph, graph.operationSymbol(op), "Operation");
        }

        void validateGraphSymbols(const wolvrix::lib::grh::Graph &graph)
        {
            if (graph.symbol().empty())
            {
                throw std::runtime_error("Graph missing symbol during emit");
            }
            for (const auto valueId : graph.values())
            {
                const wolvrix::lib::grh::SymbolId sym = graph.valueSymbol(valueId);
                if (!sym.valid() || graph.symbolText(sym).empty())
                {
                    throw std::runtime_error("Graph value missing symbol during emit");
                }
            }
            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::SymbolId sym = graph.operationSymbol(opId);
                if (!sym.valid() || graph.symbolText(sym).empty())
                {
                    throw std::runtime_error("Graph operation missing symbol during emit");
                }
            }
            for (const auto &port : graph.inputPorts())
            {
                if (port.name.empty())
                {
                    throw std::runtime_error("Input port name is empty during emit");
                }
            }
            for (const auto &port : graph.outputPorts())
            {
                if (port.name.empty())
                {
                    throw std::runtime_error("Output port name is empty during emit");
                }
            }
            for (const auto &port : graph.inoutPorts())
            {
                if (port.name.empty())
                {
                    throw std::runtime_error("Inout port name is empty during emit");
                }
            }
        }

    } // namespace

    namespace
    {
        constexpr int kIndentSizeSv = 2;

        enum class PortDir
        {
            Input,
            Output,
            Inout
        };

        struct NetDecl
        {
            int64_t width = 1;
            bool isSigned = false;
            std::optional<wolvrix::lib::grh::SrcLoc> debug;
        };

        struct VarDecl
        {
            wolvrix::lib::grh::ValueType type = wolvrix::lib::grh::ValueType::Logic;
            std::optional<wolvrix::lib::grh::SrcLoc> debug;
            std::optional<std::string> initValue;  // For compile-time constants (e.g., strings)
        };

        struct PortDecl
        {
            PortDir dir = PortDir::Input;
            int64_t width = 1;
            bool isSigned = false;
            bool isReg = false;
            wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
            std::optional<wolvrix::lib::grh::SrcLoc> debug;
        };

        struct SeqEvent
        {
            std::string edge;
            wolvrix::lib::grh::ValueId signal = wolvrix::lib::grh::ValueId::invalid();
        };

        struct SeqKey
        {
            std::vector<SeqEvent> events;
        };

        struct SeqStmt
        {
            std::string text;
            wolvrix::lib::grh::OperationId op = wolvrix::lib::grh::OperationId::invalid();
        };

        struct SeqBlock
        {
            SeqKey key{};
            std::vector<SeqStmt> stmts;
            wolvrix::lib::grh::OperationId op = wolvrix::lib::grh::OperationId::invalid();
        };

        struct SimpleBlock
        {
            std::string header;
            std::vector<std::string> stmts;
            wolvrix::lib::grh::OperationId op = wolvrix::lib::grh::OperationId::invalid();
        };

        void appendIndented(std::ostringstream &out, int indent, const std::string &text)
        {
            out << std::string(static_cast<std::size_t>(indent * kIndentSizeSv), ' ') << text << '\n';
        }

        std::string widthRange(int64_t width)
        {
            if (width <= 1)
            {
                return {};
            }
            return "[" + std::to_string(width - 1) + ":0]";
        }

        std::string signedPrefix(bool isSigned)
        {
            return isSigned ? "signed " : "";
        }

        bool isSimpleIdentifier(std::string_view expr)
        {
            if (expr.empty())
            {
                return false;
            }
            const auto isIdentStart = [](char ch) -> bool
            {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
            };
            const auto isIdentChar = [](char ch) -> bool
            {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
            };
            bool first = true;
            for (char ch : expr)
            {
                if (first)
                {
                    if (!isIdentStart(ch))
                    {
                        return false;
                    }
                    first = false;
                    continue;
                }
                if (!isIdentChar(ch))
                {
                    return false;
                }
            }
            return true;
        }

        std::string parenIfNeeded(std::string_view expr)
        {
            if (expr.empty() || isSimpleIdentifier(expr))
            {
                return std::string(expr);
            }
            if (expr.front() == '(' && expr.back() == ')')
            {
                return std::string(expr);
            }
            std::string out;
            out.reserve(expr.size() + 2);
            out.push_back('(');
            out.append(expr);
            out.push_back(')');
            return out;
        }

        struct ExprTools
        {
            std::function<wolvrix::lib::grh::ValueType(wolvrix::lib::grh::ValueId)> valueType;
            std::function<int64_t(wolvrix::lib::grh::ValueId)> valueWidth;
            std::function<bool(wolvrix::lib::grh::ValueId)> valueSigned;
            std::function<std::optional<std::string>(wolvrix::lib::grh::ValueId)> constLiteralRawFor;
            std::function<std::string(wolvrix::lib::grh::ValueId)> valueExpr;
            bool parenReduce = false;
            bool allowConstSignBit = true;
            bool allowInlineLiteral = true;

            std::string logicalOperand(wolvrix::lib::grh::ValueId valueId) const
            {
                if (valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return valueExpr(valueId);
                }
                const int64_t width = valueWidth(valueId);
                const std::string name = valueExpr(valueId);
                if (width == 1)
                {
                    return name;
                }
                if (parenReduce)
                {
                    return "(|" + parenIfNeeded(name) + ")";
                }
                return "(|" + name + ")";
            }

            std::string sizedOperandExpr(wolvrix::lib::grh::ValueId valueId) const
            {
                if (valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return valueExpr(valueId);
                }
                const std::string expr = valueExpr(valueId);
                if (!expr.empty() && isSimpleIdentifier(expr))
                {
                    return expr;
                }
                if (allowInlineLiteral)
                {
                    if (auto literal = constLiteralRawFor(valueId))
                    {
                        if (auto sized = sizedLiteralIfUnsized(*literal, valueWidth(valueId)))
                        {
                            return *sized;
                        }
                    }
                }
                return expr;
            }

            std::string extendOperand(wolvrix::lib::grh::ValueId valueId, int64_t targetWidth) const
            {
                if (valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return valueExpr(valueId);
                }
                const int64_t width = valueWidth(valueId);
                const std::string name = sizedOperandExpr(valueId);
                if (targetWidth <= 0 || width <= 0 || width == targetWidth)
                {
                    return name;
                }
                const int64_t diff = targetWidth - width;
                if (diff == 0)
                {
                    return name;
                }
                if (diff < 0)
                {
                    // Truncation needed: generate explicit slice to avoid WIDTHTRUNC warnings
                    if (targetWidth == 1)
                    {
                        return name + "[0]";
                    }
                    return name + "[" + std::to_string(targetWidth - 1) + ":0]";
                }
                if (valueSigned(valueId))
                {
                    if (allowConstSignBit)
                    {
                        if (auto literal = constLiteralRawFor(valueId))
                        {
                            if (auto signBit = signBitLiteralForConst(*literal, width, valueSigned(valueId)))
                            {
                                return "{{" + std::to_string(diff) + "{" + *signBit + "}}," + name + "}";
                            }
                        }
                    }
                    const std::string indexed = parenIfNeeded(name);
                    const std::string signBit =
                        width == 1 ? indexed
                                   : indexed + "[" + std::to_string(width - 1) + "]";
                    return "{{" + std::to_string(diff) + "{" + signBit + "}}," + name + "}";
                }
                return "{{" + std::to_string(diff) + "{1'b0}}," + name + "}";
            }

            std::string extendShiftOperand(wolvrix::lib::grh::ValueId valueId,
                                           int64_t targetWidth,
                                           bool signExtend) const
            {
                if (valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return valueExpr(valueId);
                }
                const int64_t width = valueWidth(valueId);
                const std::string name = sizedOperandExpr(valueId);
                if (targetWidth <= 0 || width <= 0 || width == targetWidth)
                {
                    return name;
                }
                const int64_t diff = targetWidth - width;
                if (diff <= 0)
                {
                    return name;
                }
                if (signExtend)
                {
                    if (allowConstSignBit)
                    {
                        if (auto literal = constLiteralRawFor(valueId))
                        {
                            if (auto signBit = signBitLiteralForConst(*literal, width, valueSigned(valueId)))
                            {
                                return "{{" + std::to_string(diff) + "{" + *signBit + "}}," + name + "}";
                            }
                        }
                    }
                    const std::string indexed = parenIfNeeded(name);
                    const std::string signBit =
                        width == 1 ? indexed
                                   : indexed + "[" + std::to_string(width - 1) + "]";
                    return "{{" + std::to_string(diff) + "{" + signBit + "}}," + name + "}";
                }
                return "{{" + std::to_string(diff) + "{1'b0}}," + name + "}";
            }

            std::string concatOperandExpr(wolvrix::lib::grh::ValueId valueId) const
            {
                if (valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return valueExpr(valueId);
                }
                const std::string expr = valueExpr(valueId);
                if (!expr.empty() && isSimpleIdentifier(expr))
                {
                    return expr;
                }
                if (allowInlineLiteral)
                {
                    if (auto literal = constLiteralRawFor(valueId))
                    {
                        if (auto sized = sizedLiteralIfUnsized(*literal, valueWidth(valueId)))
                        {
                            return *sized;
                        }
                    }
                }
                return expr;
            }

            std::string clampIndexExpr(wolvrix::lib::grh::ValueId indexId, int64_t operandWidth) const
            {
                std::string indexExpr = parenIfNeeded(valueExpr(indexId));
                if (operandWidth <= 1)
                {
                    return indexExpr;
                }
                const int64_t indexWidth = valueWidth(indexId);
                if (indexWidth <= 0)
                {
                    return indexExpr;
                }
                uint64_t temp = static_cast<uint64_t>(operandWidth - 1);
                int64_t clampWidth = 0;
                while (temp > 0)
                {
                    ++clampWidth;
                    temp >>= 1;
                }
                if (clampWidth <= 0 || indexWidth <= clampWidth)
                {
                    return indexExpr;
                }
                std::ostringstream expr;
                expr << clampWidth << "'(" << indexExpr << ")";
                return expr.str();
            }
        };

        template <typename T>
        std::optional<T> getAttribute(const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            (void)graph;
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *ptr = std::get_if<T>(&*attr))
            {
                return *ptr;
            }
            return std::nullopt;
        }

        std::string binOpToken(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kAdd:
                return "+";
            case wolvrix::lib::grh::OperationKind::kSub:
                return "-";
            case wolvrix::lib::grh::OperationKind::kMul:
                return "*";
            case wolvrix::lib::grh::OperationKind::kDiv:
                return "/";
            case wolvrix::lib::grh::OperationKind::kMod:
                return "%";
            case wolvrix::lib::grh::OperationKind::kEq:
                return "==";
            case wolvrix::lib::grh::OperationKind::kNe:
                return "!=";
            case wolvrix::lib::grh::OperationKind::kCaseEq:
                return "===";
            case wolvrix::lib::grh::OperationKind::kCaseNe:
                return "!==";
            case wolvrix::lib::grh::OperationKind::kWildcardEq:
                return "==?";
            case wolvrix::lib::grh::OperationKind::kWildcardNe:
                return "!=?";
            case wolvrix::lib::grh::OperationKind::kLt:
                return "<";
            case wolvrix::lib::grh::OperationKind::kLe:
                return "<=";
            case wolvrix::lib::grh::OperationKind::kGt:
                return ">";
            case wolvrix::lib::grh::OperationKind::kGe:
                return ">=";
            case wolvrix::lib::grh::OperationKind::kAnd:
                return "&";
            case wolvrix::lib::grh::OperationKind::kOr:
                return "|";
            case wolvrix::lib::grh::OperationKind::kXor:
                return "^";
            case wolvrix::lib::grh::OperationKind::kXnor:
                return "~^";
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
                return "&&";
            case wolvrix::lib::grh::OperationKind::kLogicOr:
                return "||";
            case wolvrix::lib::grh::OperationKind::kShl:
                return "<<";
            case wolvrix::lib::grh::OperationKind::kLShr:
                return ">>";
            case wolvrix::lib::grh::OperationKind::kAShr:
                return ">>>";
            default:
                return {};
            }
        }

        std::string unaryOpToken(wolvrix::lib::grh::OperationKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kNot:
                return "~";
            case wolvrix::lib::grh::OperationKind::kLogicNot:
                return "!";
            case wolvrix::lib::grh::OperationKind::kReduceAnd:
                return "&";
            case wolvrix::lib::grh::OperationKind::kReduceOr:
                return "|";
            case wolvrix::lib::grh::OperationKind::kReduceXor:
                return "^";
            case wolvrix::lib::grh::OperationKind::kReduceNor:
                return "~|";
            case wolvrix::lib::grh::OperationKind::kReduceNand:
                return "~&";
            case wolvrix::lib::grh::OperationKind::kReduceXnor:
                return "~^";
            default:
                return {};
            }
        }

        std::string formatNetDecl(std::string_view type, const std::string &name, const NetDecl &decl)
        {
            std::string out;
            out.append(type);
            out.push_back(' ');
            out.append(signedPrefix(decl.isSigned));
            const std::string range = widthRange(decl.width);
            if (!range.empty())
            {
                out.append(range);
                out.push_back(' ');
            }
            out.append(name);
            out.push_back(';');
            return out;
        }

        std::string formatVarDecl(wolvrix::lib::grh::ValueType type, const std::string &name,
                                   const std::optional<std::string> &initValue = std::nullopt)
        {
            std::string out;
            switch (type)
            {
            case wolvrix::lib::grh::ValueType::Real:
                out.append("real ");
                break;
            case wolvrix::lib::grh::ValueType::String:
                out.append("string ");
                break;
            default:
                out.append("logic ");
                break;
            }
            out.append(name);
            if (initValue.has_value())
            {
                out.append(" = ");
                out.append(*initValue);
            }
            out.push_back(';');
            return out;
        }

        int findNameIndex(const std::vector<std::string> &names, const std::string &target)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (names[i] == target)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        std::string sensitivityList(const wolvrix::lib::grh::Graph &graph, const SeqKey &key)
        {
            if (key.events.empty())
            {
                return {};
            }
            std::string out = "@(";
            for (std::size_t i = 0; i < key.events.size(); ++i)
            {
                if (i != 0)
                {
                    out.append(" or ");
                }
                const auto &event = key.events[i];
                out.append(event.edge);
                out.push_back(' ');
                out.append(valueSymbolRequired(graph, event.signal));
            }
            out.push_back(')');
            return out;
        }
    } // namespace

    EmitResult EmitSystemVerilog::emitImpl(const wolvrix::lib::grh::Design &design,
                                           std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                           const EmitOptions &options)
    {
        EmitResult result;

        // Index DPI imports across the design for later resolution.
        struct DpiImportRef
        {
            const wolvrix::lib::grh::Graph *graph = nullptr;
            wolvrix::lib::grh::OperationId op = wolvrix::lib::grh::OperationId::invalid();
        };
        std::unordered_map<std::string, DpiImportRef> dpicImports;
        for (const auto &graphSymbol : design.graphOrder())
        {
            auto graphIt = design.graphs().find(graphSymbol);
            if (graphIt == design.graphs().end() || !graphIt->second)
            {
                continue;
            }
            const wolvrix::lib::grh::Graph &graph = *graphIt->second;
            for (const auto opId : graph.operations())
            {
                if (graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kDpicImport)
                {
                    dpicImports.emplace(opSymbolRequired(graph, opId), DpiImportRef{&graph, opId});
                }
            }
        }

        const std::vector<const wolvrix::lib::grh::Graph *> emittedGraphs =
            reachableGraphsFromTops(design, topGraphs);

        std::unordered_map<std::string, std::string> emittedModuleNames;
        std::unordered_set<std::string> usedModuleNames;
        for (const wolvrix::lib::grh::Graph *graph : emittedGraphs)
        {
            if (!graph)
            {
                continue;
            }
            const std::string &graphSymbol = graph->symbol();
            std::string emittedName = graphSymbol;
            auto aliases = design.aliasesForGraph(graphSymbol);
            for (const auto &alias : aliases)
            {
                if (usedModuleNames.find(alias) == usedModuleNames.end())
                {
                    emittedName = alias;
                    break;
                }
            }
            usedModuleNames.insert(emittedName);
            emittedModuleNames.emplace(graphSymbol, std::move(emittedName));
        }

        auto emitGraph = [&](const wolvrix::lib::grh::Graph *graph, std::ostream &out)
        {
            validateGraphSymbols(*graph);

            const auto moduleNameIt = emittedModuleNames.find(graph->symbol());
            const std::string &moduleName = moduleNameIt != emittedModuleNames.end() ? moduleNameIt->second : graph->symbol();

            uint32_t maxValueIndex = 0;
            for (const auto valueId : graph->values())
            {
                maxValueIndex = std::max(maxValueIndex, valueId.index);
            }
            uint32_t maxOpIndex = 0;
            for (const auto opId : graph->operations())
            {
                maxOpIndex = std::max(maxOpIndex, opId.index);
            }
            std::vector<std::string> valueNameCache(maxValueIndex + 1);
            std::vector<std::string> opNameCache(maxOpIndex + 1);
            const std::string emptyName;
            auto valueName = [&](wolvrix::lib::grh::ValueId valueId) -> const std::string &
            {
                if (!valueId.valid() || valueId.graph != graph->id())
                {
                    return emptyName;
                }
                const uint32_t idx = valueId.index;
                if (idx == 0 || idx > maxValueIndex)
                {
                    return emptyName;
                }
                std::string &slot = valueNameCache[idx];
                if (slot.empty())
                {
                    slot = valueSymbolRequired(*graph, valueId);
                }
                return slot;
            };
            auto opName = [&](wolvrix::lib::grh::OperationId opId) -> const std::string &
            {
                if (!opId.valid() || opId.graph != graph->id())
                {
                    return emptyName;
                }
                const uint32_t idx = opId.index;
                if (idx == 0 || idx > maxOpIndex)
                {
                    return emptyName;
                }
                std::string &slot = opNameCache[idx];
                if (slot.empty())
                {
                    slot = opSymbolRequired(*graph, opId);
                }
                return slot;
            };
            auto isPortValue = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                return graph->valueIsInput(valueId) || graph->valueIsOutput(valueId) || graph->valueIsInout(valueId);
            };
            auto valueType = [&](wolvrix::lib::grh::ValueId valueId) -> wolvrix::lib::grh::ValueType
            {
                return graph->valueType(valueId);
            };
            auto valueWidth = [&](wolvrix::lib::grh::ValueId valueId) -> int64_t
            {
                return graph->valueWidth(valueId);
            };
            auto valueSigned = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                return graph->valueSigned(valueId);
            };
            auto formatConstLiteral = [&](wolvrix::lib::grh::ValueId valueId,
                                          std::string_view literal) -> std::string
            {
                if (valueType(valueId) == wolvrix::lib::grh::ValueType::String)
                {
                    if (literal.size() >= 2 && literal.front() == '"' && literal.back() == '"')
                    {
                        return std::string(literal);
                    }
                    return "\"" + escapeSvString(literal) + "\"";
                }
                return std::string(literal);
            };
            auto constLiteralRawFor = [&](wolvrix::lib::grh::ValueId valueId) -> std::optional<std::string>
            {
                if (!valueId.valid())
                {
                    return std::nullopt;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    return std::nullopt;
                }
                if (graph->opKind(defOpId) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    return std::nullopt;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                return getAttribute<std::string>(*graph, defOp, "constValue");
            };
            auto constLiteralFor = [&](wolvrix::lib::grh::ValueId valueId) -> std::optional<std::string>
            {
                auto raw = constLiteralRawFor(valueId);
                if (!raw)
                {
                    return std::nullopt;
                }
                return formatConstLiteral(valueId, *raw);
            };
            std::unordered_map<std::string, std::unordered_set<std::string>> regWritePortEventKeys;
            regWritePortEventKeys.reserve(static_cast<std::size_t>(graph->operations().size()));
            auto writePortEventKey = [&](const wolvrix::lib::grh::Operation &op,
                                         std::size_t eventStart) -> std::optional<std::string>
            {
                auto eventEdges = getAttribute<std::vector<std::string>>(*graph, op, "eventEdge");
                if (!eventEdges)
                {
                    return std::nullopt;
                }
                const auto &operands = op.operands();
                const std::size_t eventCount =
                    operands.size() > eventStart ? operands.size() - eventStart : 0;
                if (eventCount == 0 || eventEdges->size() != eventCount)
                {
                    return std::nullopt;
                }
                std::ostringstream key;
                for (std::size_t i = 0; i < eventCount; ++i)
                {
                    if (i != 0)
                    {
                        key << ";";
                    }
                    const std::string edge = (*eventEdges)[i];
                    key << edge << ":";
                    const wolvrix::lib::grh::ValueId eventValue = operands[eventStart + i];
                    if (eventValue.valid())
                    {
                        key << valueName(eventValue);
                    }
                }
                return key.str();
            };
            for (const auto opId : graph->operations())
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kRegisterWritePort)
                {
                    continue;
                }
                auto regSymbolAttr = getAttribute<std::string>(*graph, op, "regSymbol");
                if (!regSymbolAttr)
                {
                    continue;
                }
                auto key = writePortEventKey(op, 3);
                if (!key)
                {
                    continue;
                }
                regWritePortEventKeys[*regSymbolAttr].insert(*key);
            }
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> readPortAliases;
            for (const auto opId : graph->operations())
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kRegisterReadPort)
                {
                    continue;
                }
                if (op.results().empty())
                {
                    continue;
                }
                auto regSymbolAttr = getAttribute<std::string>(*graph, op, "regSymbol");
                if (!regSymbolAttr)
                {
                    continue;
                }
                auto itKeys = regWritePortEventKeys.find(*regSymbolAttr);
                if (itKeys == regWritePortEventKeys.end() || itKeys->second.size() <= 1)
                {
                    continue;
                }
                readPortAliases.emplace(op.results()[0], *regSymbolAttr);
            }
            auto inlineConstExprFor = [&](wolvrix::lib::grh::ValueId valueId) -> std::optional<std::string>
            {
                auto rawLiteral = constLiteralRawFor(valueId);
                if (!rawLiteral)
                {
                    return std::nullopt;
                }
                if (valueType(valueId) == wolvrix::lib::grh::ValueType::String)
                {
                    return formatConstLiteral(valueId, *rawLiteral);
                }
                std::string literal = *rawLiteral;
                if (valueType(valueId) == wolvrix::lib::grh::ValueType::Logic)
                {
                    if (auto sized = sizedLiteralIfUnsized(literal, valueWidth(valueId)))
                    {
                        literal = *sized;
                    }
                }
                return literal;
            };
            std::function<std::string(wolvrix::lib::grh::ValueId)> valueExpr =
                [&](wolvrix::lib::grh::ValueId valueId) -> std::string
            {
                if (!valueId.valid())
                {
                    return {};
                }
                if (auto it = readPortAliases.find(valueId); it != readPortAliases.end())
                {
                    return it->second;
                }
                return valueName(valueId);
            };
            ExprTools baseExpr;
            baseExpr.valueType = valueType;
            baseExpr.valueWidth = valueWidth;
            baseExpr.valueSigned = valueSigned;
            baseExpr.constLiteralRawFor = constLiteralRawFor;
            baseExpr.valueExpr = valueExpr;
            baseExpr.parenReduce = false;
            baseExpr.allowConstSignBit = true;
            baseExpr.allowInlineLiteral = false;

            // -------------------------
            // Ports
            // -------------------------
            std::map<std::string, PortDecl, std::less<>> portDecls;
            for (const auto &port : graph->inputPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                const std::string &name = port.name;
                portDecls[name] = PortDecl{PortDir::Input,
                                           graph->valueWidth(port.value),
                                           graph->valueSigned(port.value),
                                           false,
                                           graph->valueType(port.value),
                                           graph->valueSrcLoc(port.value)};
            }
            for (const auto &port : graph->outputPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                const std::string &name = port.name;
                portDecls[name] = PortDecl{PortDir::Output,
                                           graph->valueWidth(port.value),
                                           graph->valueSigned(port.value),
                                           false,
                                           graph->valueType(port.value),
                                           graph->valueSrcLoc(port.value)};
            }
            for (const auto &port : graph->inoutPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                const std::string &name = port.name;
                portDecls[name] = PortDecl{PortDir::Inout,
                                           graph->valueWidth(port.out),
                                           graph->valueSigned(port.out),
                                           false,
                                           graph->valueType(port.out),
                                           graph->valueSrcLoc(port.out)};
            }

            // Book-keeping for declarations.
            std::unordered_set<std::string> declaredNames;
            for (const auto &[name, _] : portDecls)
            {
                declaredNames.insert(name);
            }

            std::unordered_set<std::string> storageBackedPorts;
            for (const auto opId : graph->operations())
            {
                const auto opKind = graph->opKind(opId);
                if (opKind != wolvrix::lib::grh::OperationKind::kRegister &&
                    opKind != wolvrix::lib::grh::OperationKind::kLatch)
                {
                    continue;
                }
                const wolvrix::lib::grh::SymbolId opSym = graph->operationSymbol(opId);
                if (!opSym.valid())
                {
                    continue;
                }
                const std::string opName = std::string(graph->symbolText(opSym));
                if (opName.empty())
                {
                    continue;
                }
                auto itPort = portDecls.find(opName);
                if (itPort == portDecls.end())
                {
                    continue;
                }
                if (itPort->second.dir != PortDir::Output)
                {
                    reportError("Storage symbol conflicts with non-output port", opName);
                    continue;
                }
                itPort->second.isReg = true;
                storageBackedPorts.insert(opName);
            }

            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> elidedReadPortValues;
            for (const auto &port : graph->outputPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                const std::string &portName = port.name;
                if (storageBackedPorts.find(portName) == storageBackedPorts.end())
                {
                    continue;
                }
                if (!port.value.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Value portValue = graph->getValue(port.value);
                if (!portValue.users().empty())
                {
                    continue;
                }
                const wolvrix::lib::grh::OperationId defOpId = portValue.definingOp();
                if (!defOpId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                if (defOp.kind() == wolvrix::lib::grh::OperationKind::kRegisterReadPort)
                {
                    auto regSymbolAttr = getAttribute<std::string>(*graph, defOp, "regSymbol");
                    if (regSymbolAttr && *regSymbolAttr == portName)
                    {
                        elidedReadPortValues.insert(port.value);
                    }
                }
                else if (defOp.kind() == wolvrix::lib::grh::OperationKind::kLatchReadPort)
                {
                    auto latchSymbolAttr = getAttribute<std::string>(*graph, defOp, "latchSymbol");
                    if (latchSymbolAttr && *latchSymbolAttr == portName)
                    {
                        elidedReadPortValues.insert(port.value);
                    }
                }
            }
            for (const auto &alias : readPortAliases)
            {
                elidedReadPortValues.insert(alias.first);
            }

            // Storage for various sections.
            std::map<std::string, VarDecl, std::less<>> varDecls;
            std::map<std::string, NetDecl, std::less<>> regDecls;
            std::map<std::string, NetDecl, std::less<>> wireDecls;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> memoryDecls;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> instanceDecls;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> dpiImportDecls;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> assignStmts;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> portBindingStmts;
            std::vector<std::pair<std::string, wolvrix::lib::grh::OperationId>> latchBlocks;
            std::vector<SimpleBlock> simpleBlocks;
            std::vector<SeqBlock> seqBlocks;
            std::unordered_map<wolvrix::lib::grh::OperationId,
                               std::unordered_set<wolvrix::lib::grh::OperationId,
                                                  wolvrix::lib::grh::OperationIdHash>,
                               wolvrix::lib::grh::OperationIdHash>
                sequentialStmtPredecessors;
            std::unordered_set<std::string> instanceNamesUsed;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> dpiTempNames;
            int concatTempIndex = 0;
            auto makeConcatTempName = [&]() -> std::string
            {
                std::string base = "__concat_tmp";
                std::string name = base + "_" + std::to_string(concatTempIndex++);
                while (declaredNames.find(name) != declaredNames.end())
                {
                    name = base + "_" + std::to_string(concatTempIndex++);
                }
                declaredNames.insert(name);
                return name;
            };

            for (const auto opId : graph->operations())
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kDpicCall)
                {
                    continue;
                }
                for (const auto res : op.results())
                {
                    if (!res.valid())
                    {
                        continue;
                    }
                    const std::string name = valueName(res);
                    if (!name.empty())
                    {
                        dpiTempNames.emplace(res, name + "_intm");
                    }
                }
            }

            auto ensureVarDecl = [&](const std::string &name,
                                     wolvrix::lib::grh::ValueType type,
                                     const std::optional<wolvrix::lib::grh::SrcLoc> &debug = std::nullopt,
                                     const std::optional<std::string> &initValue = std::nullopt)
            {
                if (declaredNames.find(name) != declaredNames.end())
                {
                    auto it = varDecls.find(name);
                    if (it != varDecls.end())
                    {
                        if (debug && !it->second.debug)
                        {
                            it->second.debug = *debug;
                        }
                        if (initValue && !it->second.initValue)
                        {
                            it->second.initValue = *initValue;
                        }
                    }
                    return;
                }
                varDecls.emplace(name, VarDecl{type, debug, initValue});
                declaredNames.insert(name);
            };

            auto ensureRegDecl = [&](const std::string &name, int64_t width, bool isSigned,
                                     wolvrix::lib::grh::ValueType valueType,
                                     const std::optional<wolvrix::lib::grh::SrcLoc> &debug = std::nullopt)
            {
                if (valueType != wolvrix::lib::grh::ValueType::Logic)
                {
                    ensureVarDecl(name, valueType, debug);
                    return;
                }
                if (declaredNames.find(name) != declaredNames.end())
                {
                    auto it = regDecls.find(name);
                    if (it != regDecls.end() && debug && !it->second.debug)
                    {
                        it->second.debug = *debug;
                    }
                    return;
                }
                regDecls.emplace(name, NetDecl{width, isSigned, debug});
                declaredNames.insert(name);
            };

            auto ensureWireDecl = [&](wolvrix::lib::grh::ValueId valueId)
            {
                const wolvrix::lib::grh::SymbolId sym = graph->valueSymbol(valueId);
                const std::string name = std::string(graph->symbolText(sym));
                const auto type = graph->valueType(valueId);
                const auto debug = graph->valueSrcLoc(valueId);
                if (type != wolvrix::lib::grh::ValueType::Logic)
                {
                    ensureVarDecl(name, type, debug);
                    return;
                }
                if (declaredNames.find(name) != declaredNames.end())
                {
                    auto it = wireDecls.find(name);
                    if (it != wireDecls.end() && debug && !it->second.debug)
                    {
                        it->second.debug = *debug;
                    }
                    return;
                }
                wireDecls.emplace(name, NetDecl{graph->valueWidth(valueId), graph->valueSigned(valueId), debug});
                declaredNames.insert(name);
            };

            for (const auto sym : graph->declaredSymbols())
            {
                const wolvrix::lib::grh::ValueId valueId = graph->findValue(sym);
                if (valueId.valid())
                {
                    ensureWireDecl(valueId);
                }
            }

            auto addAssign = [&](std::string stmt, wolvrix::lib::grh::OperationId sourceOp)
            {
                assignStmts.emplace_back(std::move(stmt), sourceOp);
            };
            auto addSimpleStmt = [&](std::string header, std::string stmt,
                                     wolvrix::lib::grh::OperationId sourceOp)
            {
                for (auto &block : simpleBlocks)
                {
                    if (block.header == header)
                    {
                        block.stmts.push_back(std::move(stmt));
                        return;
                    }
                }
                simpleBlocks.push_back(
                    SimpleBlock{std::move(header), std::vector<std::string>{std::move(stmt)}, sourceOp});
            };
            auto addCombAssign = [&](const std::string &lhs, const std::string &rhs,
                                     wolvrix::lib::grh::OperationId sourceOp)
            {
                std::ostringstream stmt;
                appendIndented(stmt, 2, lhs + " = " + rhs + ";");
                addSimpleStmt("always_comb", stmt.str(), sourceOp);
            };
            auto addValueAssign = [&](wolvrix::lib::grh::ValueId result,
                                      const std::string &rhs,
                                      wolvrix::lib::grh::OperationId sourceOp)
            {
                if (valueType(result) != wolvrix::lib::grh::ValueType::Logic)
                {
                    // For String constants, use declaration-time initialization
                    // instead of always_comb to ensure value is available in initial blocks
                    if (valueType(result) == wolvrix::lib::grh::ValueType::String)
                    {
                        ensureVarDecl(valueName(result), valueType(result), 
                                      graph->valueSrcLoc(result), rhs);
                    }
                    else
                    {
                        addCombAssign(valueName(result), rhs, sourceOp);
                    }
                }
                else
                {
                    addAssign("assign " + valueName(result) + " = " + rhs + ";", sourceOp);
                }
            };

            auto sameSeqKey = [](const SeqKey &lhs, const SeqKey &rhs) -> bool
            {
                if (lhs.events.size() != rhs.events.size())
                {
                    return false;
                }
                for (std::size_t i = 0; i < lhs.events.size(); ++i)
                {
                    if (lhs.events[i].edge != rhs.events[i].edge ||
                        lhs.events[i].signal != rhs.events[i].signal)
                    {
                        return false;
                    }
                }
                return true;
            };
            auto addSequentialStmt = [&](const SeqKey &key, std::string stmt,
                                         wolvrix::lib::grh::OperationId sourceOp)
            {
                for (auto &block : seqBlocks)
                {
                    if (sameSeqKey(block.key, key))
                    {
                        block.stmts.push_back(SeqStmt{.text = std::move(stmt), .op = sourceOp});
                        return;
                    }
                }
                seqBlocks.push_back(SeqBlock{
                    key,
                    std::vector<SeqStmt>{SeqStmt{.text = std::move(stmt), .op = sourceOp}},
                    sourceOp,
                });
            };

            auto orderSequentialStmts = [&](const std::vector<SeqStmt> &source) {
                struct Entry
                {
                    std::size_t slot = 0;
                    SeqStmt stmt;
                    int64_t priority = 0;
                };
                std::vector<SeqStmt> ordered = source;
                std::unordered_map<std::string, std::vector<Entry>> entriesByGroup;
                for (std::size_t slot = 0; slot < source.size(); ++slot)
                {
                    if (!source[slot].op.valid())
                    {
                        continue;
                    }
                    const auto op = graph->getOperation(source[slot].op);
                    const auto group = getAttribute<std::string>(
                        *graph, op, wolvrix::lib::grh::kMemoryWritePriorityGroupAttr);
                    const auto priority = getAttribute<int64_t>(
                        *graph, op, wolvrix::lib::grh::kMemoryWritePriorityAttr);
                    if (!group || !priority)
                    {
                        continue;
                    }
                    entriesByGroup[*group].push_back(Entry{
                        .slot = slot,
                        .stmt = source[slot],
                        .priority = *priority,
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
                        return lhs.stmt.op.index < rhs.stmt.op.index;
                    });
                    for (std::size_t index = 0; index < entries.size(); ++index)
                    {
                        ordered[slots[index]] = std::move(entries[index].stmt);
                    }
                }

                std::unordered_map<wolvrix::lib::grh::OperationId,
                                   std::vector<std::size_t>,
                                   wolvrix::lib::grh::OperationIdHash>
                    slotsByOp;
                for (std::size_t slot = 0; slot < ordered.size(); ++slot)
                {
                    if (ordered[slot].op.valid())
                    {
                        slotsByOp[ordered[slot].op].push_back(slot);
                    }
                }
                std::vector<std::vector<std::size_t>> successors(ordered.size());
                std::vector<std::size_t> predecessorCount(ordered.size(), 0);
                auto addOrderingEdge = [&](std::size_t predecessor, std::size_t successor)
                {
                    if (predecessor == successor)
                    {
                        return;
                    }
                    auto &targets = successors[predecessor];
                    if (std::find(targets.begin(), targets.end(), successor) != targets.end())
                    {
                        return;
                    }
                    targets.push_back(successor);
                    ++predecessorCount[successor];
                };

                std::unordered_map<std::string, std::size_t> previousMemoryPrioritySlot;
                for (std::size_t slot = 0; slot < ordered.size(); ++slot)
                {
                    if (!ordered[slot].op.valid())
                    {
                        continue;
                    }
                    const auto op = graph->getOperation(ordered[slot].op);
                    const auto group = getAttribute<std::string>(
                        *graph, op, wolvrix::lib::grh::kMemoryWritePriorityGroupAttr);
                    const auto priority = getAttribute<int64_t>(
                        *graph, op, wolvrix::lib::grh::kMemoryWritePriorityAttr);
                    if (!group || !priority)
                    {
                        continue;
                    }
                    if (auto previous = previousMemoryPrioritySlot.find(*group);
                        previous != previousMemoryPrioritySlot.end())
                    {
                        addOrderingEdge(previous->second, slot);
                    }
                    previousMemoryPrioritySlot[*group] = slot;
                }
                for (const auto &[sinkOp, predecessorOps] : sequentialStmtPredecessors)
                {
                    auto sinkIt = slotsByOp.find(sinkOp);
                    if (sinkIt == slotsByOp.end())
                    {
                        continue;
                    }
                    for (const auto predecessorOp : predecessorOps)
                    {
                        auto predecessorIt = slotsByOp.find(predecessorOp);
                        if (predecessorIt == slotsByOp.end())
                        {
                            continue;
                        }
                        for (const std::size_t predecessorSlot : predecessorIt->second)
                        {
                            for (const std::size_t sinkSlot : sinkIt->second)
                            {
                                addOrderingEdge(predecessorSlot, sinkSlot);
                            }
                        }
                    }
                }
                std::priority_queue<std::size_t,
                                    std::vector<std::size_t>,
                                    std::greater<std::size_t>>
                    ready;
                for (std::size_t slot = 0; slot < ordered.size(); ++slot)
                {
                    if (predecessorCount[slot] == 0)
                    {
                        ready.push(slot);
                    }
                }
                std::vector<SeqStmt> dependencyOrdered;
                dependencyOrdered.reserve(ordered.size());
                while (!ready.empty())
                {
                    const std::size_t slot = ready.top();
                    ready.pop();
                    dependencyOrdered.push_back(ordered[slot]);
                    for (const std::size_t successor : successors[slot])
                    {
                        if (--predecessorCount[successor] == 0)
                        {
                            ready.push(successor);
                        }
                    }
                }
                if (dependencyOrdered.size() == ordered.size())
                {
                    return dependencyOrdered;
                }
                reportError("sequential statement ordering constraints contain a cycle",
                            graph->symbol());
                return ordered;
            };

            auto addLatchBlock = [&](std::string stmt, wolvrix::lib::grh::OperationId sourceOp)
            {
                latchBlocks.emplace_back(std::move(stmt), sourceOp);
            };

            auto isStateSinkKind = [](wolvrix::lib::grh::OperationKind kind) -> bool
            {
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
                case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                    return true;
                default:
                    return false;
                }
            };

            auto isDpiProcedurallyInlineableKind = [](wolvrix::lib::grh::OperationKind kind) -> bool
            {
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kConstant:
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
                case wolvrix::lib::grh::OperationKind::kShl:
                case wolvrix::lib::grh::OperationKind::kLShr:
                case wolvrix::lib::grh::OperationKind::kAShr:
                case wolvrix::lib::grh::OperationKind::kAdd:
                case wolvrix::lib::grh::OperationKind::kSub:
                case wolvrix::lib::grh::OperationKind::kMul:
                case wolvrix::lib::grh::OperationKind::kDiv:
                case wolvrix::lib::grh::OperationKind::kMod:
                case wolvrix::lib::grh::OperationKind::kLogicAnd:
                case wolvrix::lib::grh::OperationKind::kLogicOr:
                case wolvrix::lib::grh::OperationKind::kNot:
                case wolvrix::lib::grh::OperationKind::kReduceAnd:
                case wolvrix::lib::grh::OperationKind::kReduceOr:
                case wolvrix::lib::grh::OperationKind::kReduceXor:
                case wolvrix::lib::grh::OperationKind::kReduceNor:
                case wolvrix::lib::grh::OperationKind::kReduceNand:
                case wolvrix::lib::grh::OperationKind::kReduceXnor:
                case wolvrix::lib::grh::OperationKind::kLogicNot:
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
            };

            std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> dpiDrivenStateOps;
            std::unordered_map<wolvrix::lib::grh::ValueId,
                               std::vector<wolvrix::lib::grh::OperationId>,
                               wolvrix::lib::grh::ValueIdHash> valueUseMap;
            valueUseMap.reserve(static_cast<std::size_t>(maxValueIndex) + 8);
            for (const auto opId : graph->operations())
            {
                for (const auto operand : graph->opOperands(opId))
                {
                    if (!operand.valid() || operand.graph != graph->id())
                    {
                        continue;
                    }
                    valueUseMap[operand].push_back(opId);
                }
            }

            auto isConstOneValue = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    return false;
                }
                if (graph->opKind(defOpId) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    return false;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                auto constValue = getAttribute<std::string>(*graph, defOp, "constValue");
                if (!constValue)
                {
                    return false;
                }
                std::string text = *constValue;
                text.erase(std::remove_if(text.begin(), text.end(),
                                          [](unsigned char ch)
                                          { return std::isspace(ch) || ch == '_'; }),
                           text.end());
                if (text == "1")
                {
                    return true;
                }
                if (text.size() >= 3 && text.back() == '1')
                {
                    const std::size_t quote = text.find('\'');
                    if (quote != std::string::npos && quote + 1 < text.size())
                    {
                        return true;
                    }
                }
                return false;
            };

            auto computeEventKey = [&](const wolvrix::lib::grh::Operation &eventOp,
                                       std::size_t eventStart,
                                       std::string_view opContext) -> std::optional<SeqKey>
            {
                auto eventEdges = getAttribute<std::vector<std::string>>(*graph, eventOp, "eventEdge");
                if (!eventEdges)
                {
                    reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " missing eventEdge",
                                std::string(opContext));
                    return std::nullopt;
                }
                const auto &eventOperands = eventOp.operands();
                const std::size_t eventCount =
                    eventOperands.size() > eventStart ? eventOperands.size() - eventStart : 0;
                if (eventCount == 0 || eventEdges->size() != eventCount)
                {
                    reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " eventEdge size mismatch",
                                std::string(opContext));
                    return std::nullopt;
                }
                SeqKey key;
                key.events.reserve(eventCount);
                for (std::size_t i = 0; i < eventCount; ++i)
                {
                    const wolvrix::lib::grh::ValueId signal = eventOperands[eventStart + i];
                    if (!signal.valid())
                    {
                        reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " missing event operand",
                                    std::string(opContext));
                        return std::nullopt;
                    }
                    key.events.push_back(SeqEvent{(*eventEdges)[i], signal});
                }
                return key;
            };

            auto collectStateSinks =
                [&](auto &&self,
                    wolvrix::lib::grh::ValueId valueId,
                    std::unordered_map<wolvrix::lib::grh::ValueId, bool,
                                       wolvrix::lib::grh::ValueIdHash> &reachesStateMemo,
                    std::unordered_set<wolvrix::lib::grh::ValueId,
                                       wolvrix::lib::grh::ValueIdHash> &visiting,
                    std::unordered_set<wolvrix::lib::grh::OperationId,
                                       wolvrix::lib::grh::OperationIdHash> &sinks,
                    std::unordered_set<wolvrix::lib::grh::OperationId,
                                       wolvrix::lib::grh::OperationIdHash> &unsupportedOps,
                    std::unordered_set<wolvrix::lib::grh::OperationId,
                                       wolvrix::lib::grh::OperationIdHash> &effectConsumers) -> bool
            {
                if (!valueId.valid() || valueId.graph != graph->id())
                {
                    return false;
                }
                if (auto memo = reachesStateMemo.find(valueId); memo != reachesStateMemo.end())
                {
                    return memo->second;
                }
                if (!visiting.insert(valueId).second)
                {
                    return false;
                }
                auto itUses = valueUseMap.find(valueId);
                if (itUses == valueUseMap.end())
                {
                    visiting.erase(valueId);
                    reachesStateMemo.emplace(valueId, false);
                    return false;
                }
                bool reachesState = false;
                for (const auto userOpId : itUses->second)
                {
                    if (!userOpId.valid())
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::Operation userOp = graph->getOperation(userOpId);
                    const bool isEffectConsumer =
                        userOp.kind() == wolvrix::lib::grh::OperationKind::kSystemTask ||
                        userOp.kind() == wolvrix::lib::grh::OperationKind::kDpicCall ||
                        (userOp.kind() == wolvrix::lib::grh::OperationKind::kSystemFunction &&
                         getAttribute<bool>(*graph, userOp, "hasSideEffects").value_or(false));
                    if (isEffectConsumer)
                    {
                        effectConsumers.insert(userOpId);
                        continue;
                    }
                    if (isStateSinkKind(userOp.kind()))
                    {
                        sinks.insert(userOpId);
                        reachesState = true;
                        continue;
                    }
                    bool userReachesState = false;
                    for (const auto result : userOp.results())
                    {
                        userReachesState |= self(self,
                                                 result,
                                                 reachesStateMemo,
                                                 visiting,
                                                 sinks,
                                                 unsupportedOps,
                                                 effectConsumers);
                    }
                    if (userReachesState && !isDpiProcedurallyInlineableKind(userOp.kind()))
                    {
                        unsupportedOps.insert(userOpId);
                    }
                    reachesState |= userReachesState;
                }
                visiting.erase(valueId);
                reachesStateMemo.emplace(valueId, reachesState);
                return reachesState;
            };
            auto formatSinkName = [&](wolvrix::lib::grh::OperationId sinkOpId) -> std::string
            {
                if (!sinkOpId.valid())
                {
                    return "<invalid>";
                }
                const wolvrix::lib::grh::Operation sinkOp = graph->getOperation(sinkOpId);
                const std::string opName = std::string(sinkOp.symbolText());
                if (!opName.empty())
                {
                    return opName;
                }
                if (!sinkOp.results().empty())
                {
                    const std::string &valueSym = valueName(sinkOp.results().front());
                    if (!valueSym.empty())
                    {
                        return valueSym;
                    }
                }
                std::ostringstream label;
                label << wolvrix::lib::grh::toString(sinkOp.kind()) << "#" << sinkOpId.index;
                return label.str();
            };
            auto formatDpiName = [&](const wolvrix::lib::grh::Operation &dpiOp) -> std::string
            {
                std::string name = std::string(dpiOp.symbolText());
                if (!name.empty())
                {
                    return name;
                }
                if (auto importSym = getAttribute<std::string>(*graph, dpiOp, "targetImportSymbol");
                    importSym && !importSym->empty())
                {
                    return *importSym;
                }
                if (!dpiOp.results().empty())
                {
                    const std::string &valueSym = valueName(dpiOp.results().front());
                    if (!valueSym.empty())
                    {
                        return valueSym;
                    }
                }
                std::ostringstream label;
                label << "kDpicCall#" << dpiOp.id().index;
                return label.str();
            };
            auto dpiStateSequenceMatches =
                [&](const wolvrix::lib::grh::Operation &dpiOp,
                    const wolvrix::lib::grh::Operation &sinkOp) -> bool
            {
                const std::string opContext = std::string(dpiOp.symbolText());
                const auto &operands = dpiOp.operands();
                if (operands.empty())
                {
                    reportError("kDpicCall missing updateCond operand", opContext);
                    return false;
                }

                const wolvrix::lib::grh::ValueId dpiUpdateCond = operands[0];
                if (sinkOp.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort ||
                    sinkOp.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort ||
                    sinkOp.kind() == wolvrix::lib::grh::OperationKind::kMemoryWritePort)
                {
                    const auto &sinkOperands = sinkOp.operands();
                    if (sinkOperands.empty())
                    {
                        reportError("DPI sink missing updateCond operand", opContext);
                        return false;
                    }
                    // DPI results retain their last value while the call guard is false. A
                    // merged write port may therefore have a broader guard and select the
                    // current DPI result only on the call branch.
                }
                else if (sinkOp.kind() == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
                {
                    if (!isConstOneValue(dpiUpdateCond))
                    {
                        reportError("kDpicCall updateCond must be constant 1 for memory read port inline",
                                    opContext);
                        return false;
                    }
                }

                if (sinkOp.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort ||
                    sinkOp.kind() == wolvrix::lib::grh::OperationKind::kMemoryWritePort)
                {
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, dpiOp, "eventEdge");
                    if (!eventEdges)
                    {
                        reportError("kDpicCall missing eventEdge", opContext);
                        return false;
                    }
                    if (operands.size() < eventEdges->size())
                    {
                        reportError("kDpicCall operand count does not match eventEdge", opContext);
                        return false;
                    }
                    const std::size_t eventStart = operands.size() - eventEdges->size();
                    auto dpiKey = computeEventKey(dpiOp, eventStart, opContext);
                    auto sinkKey = computeEventKey(
                        sinkOp,
                        sinkOp.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort ? 3U : 4U,
                        std::string(sinkOp.symbolText()));
                    if (!dpiKey || !sinkKey || !sameSeqKey(*dpiKey, *sinkKey))
                    {
                        reportError("kDpicCall eventEdge does not match sink eventEdge for atomic state update",
                                    opContext);
                        return false;
                    }
                }
                else if (sinkOp.kind() == wolvrix::lib::grh::OperationKind::kLatchWritePort ||
                         sinkOp.kind() == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
                {
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, dpiOp, "eventEdge");
                    if (eventEdges && !eventEdges->empty())
                    {
                        reportError("kDpicCall eventEdge must be empty for comb/latch inline", opContext);
                        return false;
                    }
                }
                return true;
            };

            for (const auto opId : graph->operations())
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kDpicCall)
                {
                    continue;
                }
                const std::string opContext = std::string(op.symbolText());
                const std::string dpiName = formatDpiName(op);
                const auto hasReturn = getAttribute<bool>(*graph, op, "hasReturn").value_or(false);
                bool resultsOk = true;
                const auto &results = op.results();
                std::vector<wolvrix::lib::grh::OperationId> resultSinkOps(
                    results.size(), wolvrix::lib::grh::OperationId::invalid());
                for (std::size_t resIdx = 0; resIdx < results.size(); ++resIdx)
                {
                    const auto res = results[resIdx];
                    if (!res.valid())
                    {
                        continue;
                    }
                    std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash> sinks;
                    std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>
                        unsupportedOps;
                    std::unordered_set<wolvrix::lib::grh::OperationId, wolvrix::lib::grh::OperationIdHash>
                        effectConsumers;
                    std::unordered_map<wolvrix::lib::grh::ValueId, bool,
                                       wolvrix::lib::grh::ValueIdHash> reachesStateMemo;
                    std::unordered_set<wolvrix::lib::grh::ValueId,
                                       wolvrix::lib::grh::ValueIdHash> visiting;
                    collectStateSinks(collectStateSinks,
                                      res,
                                      reachesStateMemo,
                                      visiting,
                                      sinks,
                                      unsupportedOps,
                                      effectConsumers);
                    if (!effectConsumers.empty())
                    {
                        std::ostringstream details;
                        bool firstOp = true;
                        for (const auto effectOpId : effectConsumers)
                        {
                            if (!firstOp)
                            {
                                details << ", ";
                            }
                            firstOp = false;
                            const auto effectOp = graph->getOperation(effectOpId);
                            details << wolvrix::lib::grh::toString(effectOp.kind());
                            if (!effectOp.symbolText().empty())
                            {
                                details << "(" << effectOp.symbolText() << ")";
                            }
                        }
                        reportError("kDpicCall result is consumed by effect operations that cannot be "
                                        "ordered atomically: " + details.str(),
                                    opContext);
                        resultsOk = false;
                        continue;
                    }
                    if (!unsupportedOps.empty())
                    {
                        std::ostringstream details;
                        bool firstOp = true;
                        for (const auto unsupportedOpId : unsupportedOps)
                        {
                            if (!firstOp)
                            {
                                details << ", ";
                            }
                            firstOp = false;
                            const auto unsupportedOp = graph->getOperation(unsupportedOpId);
                            details << wolvrix::lib::grh::toString(unsupportedOp.kind());
                            if (!unsupportedOp.symbolText().empty())
                            {
                                details << "(" << unsupportedOp.symbolText() << ")";
                            }
                        }
                        reportError("kDpicCall result reaches state through operations that cannot be "
                                        "inlined atomically: " + details.str(),
                                    opContext);
                        resultsOk = false;
                        continue;
                    }
                    if (sinks.size() > 1)
                    {
                        std::ostringstream details;
                        bool firstSink = true;
                        for (const auto sinkOpId : sinks)
                        {
                            if (!firstSink)
                            {
                                details << ", ";
                            }
                            firstSink = false;
                            details << formatSinkName(sinkOpId);
                        }
                        const std::string &resName = valueName(res);
                        const std::string resLabel = resName.empty() ? "<unnamed>" : resName;
                        reportError("kDpicCall result drives multiple state elements: dpic=" +
                                        dpiName + " result=" + resLabel + " sinks=[" +
                                        details.str() + "]",
                                    opContext);
                        resultsOk = false;
                        continue;
                    }
                    if (sinks.size() == 1)
                    {
                        resultSinkOps[resIdx] = *sinks.begin();
                    }
                }
                if (!resultsOk)
                {
                    continue;
                }

                auto outArgName = getAttribute<std::vector<std::string>>(*graph, op, "outArgName");
                auto inoutArgName = getAttribute<std::vector<std::string>>(*graph, op, "inoutArgName");
                const std::size_t expectedResultCount =
                    (hasReturn ? 1U : 0U) +
                    (outArgName ? outArgName->size() : 0U) +
                    (inoutArgName ? inoutArgName->size() : 0U);
                if (results.size() < expectedResultCount)
                {
                    continue;
                }

                std::unordered_set<wolvrix::lib::grh::OperationId,
                                   wolvrix::lib::grh::OperationIdHash>
                    atomicSinks;
                for (std::size_t resIdx = 0; resIdx < expectedResultCount; ++resIdx)
                {
                    if (resultSinkOps[resIdx].valid())
                    {
                        atomicSinks.insert(resultSinkOps[resIdx]);
                    }
                }
                bool atomicSinksOk = true;
                for (const auto sinkOpId : atomicSinks)
                {
                    const wolvrix::lib::grh::Operation sinkOp = graph->getOperation(sinkOpId);
                    if (sinkOp.kind() != wolvrix::lib::grh::OperationKind::kRegisterWritePort &&
                        sinkOp.kind() != wolvrix::lib::grh::OperationKind::kMemoryWritePort)
                    {
                        reportError("kDpicCall output/inout result requires a supported atomic state sink",
                                    opContext);
                        atomicSinksOk = false;
                        continue;
                    }
                    if (!dpiStateSequenceMatches(op, sinkOp))
                    {
                        atomicSinksOk = false;
                    }
                }
                if (!atomicSinksOk)
                {
                    continue;
                }
                for (const auto sinkOpId : atomicSinks)
                {
                    dpiDrivenStateOps.insert(sinkOpId);
                    sequentialStmtPredecessors[sinkOpId].insert(opId);
                }
            }
            const uint32_t dpiMaxValueIndex = maxValueIndex;
            std::vector<int8_t> dpiDependsDense(dpiMaxValueIndex + 1, -1);
            std::vector<uint8_t> dpiDependsVisiting(dpiMaxValueIndex + 1, 0);
            std::vector<uint8_t> dpiTempDense(dpiMaxValueIndex + 1, 0);
            for (const auto &entry : dpiTempNames)
            {
                const uint32_t idx = entry.first.index;
                if (idx <= dpiMaxValueIndex)
                {
                    dpiTempDense[idx] = 1;
                }
            }
            auto computeDpiDepends = [&](auto&& self, wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                if (valueId.graph != graph->id())
                {
                    return false;
                }
                const uint32_t idx = valueId.index;
                if (idx == 0 || idx > dpiMaxValueIndex)
                {
                    return false;
                }
                if (dpiTempDense[idx])
                {
                    return true;
                }
                const int8_t cached = dpiDependsDense[idx];
                if (cached >= 0)
                {
                    return cached != 0;
                }
                if (dpiDependsVisiting[idx])
                {
                    return false;
                }
                dpiDependsVisiting[idx] = 1;
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    dpiDependsVisiting[idx] = 0;
                    dpiDependsDense[idx] = 0;
                    return false;
                }
                const auto ops = graph->opOperands(defOpId);
                auto dependsForOperands = [&]() -> bool {
                    for (const auto operand : ops)
                    {
                        if (self(self, operand))
                        {
                            return true;
                        }
                    }
                    return false;
                };
                const bool depends = isDpiProcedurallyInlineableKind(graph->opKind(defOpId)) &&
                                     dependsForOperands();
                dpiDependsVisiting[idx] = 0;
                dpiDependsDense[idx] = depends ? 1 : 0;
                return depends;
            };
            for (const auto valueId : graph->values())
            {
                computeDpiDepends(computeDpiDepends, valueId);
            }
            auto valueDependsOnDpi = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                if (valueId.graph != graph->id())
                {
                    return false;
                }
                const uint32_t idx = valueId.index;
                if (idx == 0 || idx > dpiMaxValueIndex)
                {
                    return false;
                }
                return dpiDependsDense[idx] > 0;
            };

            wolvrix::lib::grh::OperationId dpiInlineSink = wolvrix::lib::grh::OperationId::invalid();
            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> dpiInlineResolving;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string, wolvrix::lib::grh::ValueIdHash> dpiInlineCache;
            std::function<std::string(wolvrix::lib::grh::ValueId)> dpiInlineExpr;
            ExprTools inlineExpr = baseExpr;
            inlineExpr.parenReduce = true;
            inlineExpr.allowConstSignBit = false;
            inlineExpr.allowInlineLiteral = true;
            inlineExpr.valueExpr = [&](wolvrix::lib::grh::ValueId valueId) -> std::string
            {
                if (dpiInlineSink.valid() || valueDependsOnDpi(valueId))
                {
                    return dpiInlineExpr(valueId);
                }
                return valueExpr(valueId);
            };
            dpiInlineExpr = [&](wolvrix::lib::grh::ValueId valueId) -> std::string
            {
                if (!valueId.valid())
                {
                    return {};
                }
                if (auto cached = dpiInlineCache.find(valueId); cached != dpiInlineCache.end())
                {
                    return cached->second;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (auto it = dpiTempNames.find(valueId); it != dpiTempNames.end())
                {
                    return it->second;
                }
                if (!valueDependsOnDpi(valueId))
                {
                    return valueExpr(valueId);
                }
                if (!dpiInlineResolving.insert(valueId).second)
                {
                    return valueExpr(valueId);
                }
                if (!defOpId.valid())
                {
                    dpiInlineResolving.erase(valueId);
                    return valueExpr(valueId);
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                const auto &ops = defOp.operands();
                std::string expr = valueExpr(valueId);
                switch (defOp.kind())
                {
                case wolvrix::lib::grh::OperationKind::kConstant:
                    if (auto literal = constLiteralFor(valueId))
                    {
                        expr = *literal;
                    }
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
                {
                    if (ops.size() >= 2)
                    {
                        int64_t resultWidth =
                            std::max(graph->valueWidth(ops[0]),
                                     graph->valueWidth(ops[1]));
                        const std::string tok = binOpToken(defOp.kind());
                        const std::string lhs = resultWidth > 0
                                                    ? inlineExpr.extendOperand(ops[0], resultWidth)
                                                    : inlineExpr.valueExpr(ops[0]);
                        const std::string rhs = resultWidth > 0
                                                    ? inlineExpr.extendOperand(ops[1], resultWidth)
                                                    : inlineExpr.valueExpr(ops[1]);
                        const bool signedCompare =
                            (defOp.kind() == wolvrix::lib::grh::OperationKind::kLt ||
                             defOp.kind() == wolvrix::lib::grh::OperationKind::kLe ||
                             defOp.kind() == wolvrix::lib::grh::OperationKind::kGt ||
                             defOp.kind() == wolvrix::lib::grh::OperationKind::kGe) &&
                            graph->valueSigned(ops[0]) &&
                            graph->valueSigned(ops[1]);
                        const std::string lhsExpr = signedCompare ? "$signed(" + lhs + ")" : lhs;
                        const std::string rhsExpr = signedCompare ? "$signed(" + rhs + ")" : rhs;
                        expr = lhsExpr + " " + tok + " " + rhsExpr;
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kAnd:
                case wolvrix::lib::grh::OperationKind::kOr:
                case wolvrix::lib::grh::OperationKind::kXor:
                case wolvrix::lib::grh::OperationKind::kXnor:
                    if (ops.size() >= 2)
                    {
                        int64_t resultWidth = graph->valueWidth(valueId);
                        if (resultWidth <= 0)
                        {
                            resultWidth = std::max(graph->valueWidth(ops[0]),
                                                   graph->valueWidth(ops[1]));
                        }
                        const std::string tok = binOpToken(defOp.kind());
                        const std::string lhs = resultWidth > 0
                                                    ? inlineExpr.extendOperand(ops[0], resultWidth)
                                                    : inlineExpr.valueExpr(ops[0]);
                        const std::string rhs = resultWidth > 0
                                                    ? inlineExpr.extendOperand(ops[1], resultWidth)
                                                    : inlineExpr.valueExpr(ops[1]);
                        expr = lhs + " " + tok + " " + rhs;
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kShl:
                case wolvrix::lib::grh::OperationKind::kLShr:
                case wolvrix::lib::grh::OperationKind::kAShr:
                    if (ops.size() >= 2)
                    {
                        const int64_t resultWidth = graph->valueWidth(valueId);
                        const bool signExtend =
                            defOp.kind() == wolvrix::lib::grh::OperationKind::kAShr &&
                            graph->valueSigned(ops[0]);
                        const std::string tok = binOpToken(defOp.kind());
                        const std::string lhs = resultWidth > 0
                                                    ? inlineExpr.extendShiftOperand(ops[0], resultWidth, signExtend)
                                                    : inlineExpr.valueExpr(ops[0]);
                        expr = lhs + " " + tok + " " + inlineExpr.valueExpr(ops[1]);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kAdd:
                case wolvrix::lib::grh::OperationKind::kSub:
                case wolvrix::lib::grh::OperationKind::kMul:
                case wolvrix::lib::grh::OperationKind::kDiv:
                case wolvrix::lib::grh::OperationKind::kMod:
                    if (ops.size() >= 2)
                    {
                        int64_t resultWidth = graph->valueWidth(valueId);
                        if (resultWidth <= 0)
                        {
                            resultWidth = std::max(graph->valueWidth(ops[0]),
                                                   graph->valueWidth(ops[1]));
                        }
                        const std::string tok = binOpToken(defOp.kind());
                        expr = inlineExpr.extendOperand(ops[0], resultWidth) + " " + tok + " " +
                               inlineExpr.extendOperand(ops[1], resultWidth);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kLogicAnd:
                case wolvrix::lib::grh::OperationKind::kLogicOr:
                    if (ops.size() >= 2)
                    {
                        const std::string tok = binOpToken(defOp.kind());
                        expr = inlineExpr.logicalOperand(ops[0]) + " " + tok + " " +
                               inlineExpr.logicalOperand(ops[1]);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kNot:
                case wolvrix::lib::grh::OperationKind::kReduceAnd:
                case wolvrix::lib::grh::OperationKind::kReduceOr:
                case wolvrix::lib::grh::OperationKind::kReduceXor:
                case wolvrix::lib::grh::OperationKind::kReduceNor:
                case wolvrix::lib::grh::OperationKind::kReduceNand:
                case wolvrix::lib::grh::OperationKind::kReduceXnor:
                    if (!ops.empty())
                    {
                        const std::string tok = unaryOpToken(defOp.kind());
                        expr = tok + inlineExpr.valueExpr(ops[0]);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kLogicNot:
                    if (!ops.empty())
                    {
                        expr = "!" + inlineExpr.logicalOperand(ops[0]);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kMux:
                    if (ops.size() >= 3)
                    {
                        const int64_t resultWidth = graph->valueWidth(valueId);
                        const std::string lhs = inlineExpr.extendOperand(ops[1], resultWidth);
                        const std::string rhs = inlineExpr.extendOperand(ops[2], resultWidth);
                        expr = inlineExpr.valueExpr(ops[0]) + " ? " + lhs + " : " + rhs;
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kAssign:
                    if (!ops.empty())
                    {
                        int64_t resultWidth = graph->valueWidth(valueId);
                        expr = inlineExpr.extendOperand(ops[0], resultWidth);
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kConcat:
                    if (!ops.empty())
                    {
                        int64_t concatWidth = 0;
                        bool concatWidthKnown = true;
                        for (const auto operand : ops)
                        {
                            if (!operand.valid() ||
                                graph->valueType(operand) != wolvrix::lib::grh::ValueType::Logic)
                            {
                                concatWidthKnown = false;
                                break;
                            }
                            const int64_t operandWidth = graph->valueWidth(operand);
                            if (operandWidth <= 0)
                            {
                                concatWidthKnown = false;
                                break;
                            }
                            concatWidth += operandWidth;
                        }
                        if (ops.size() == 1)
                        {
                            expr = inlineExpr.valueExpr(ops[0]);
                        }
                        else if (ops.size() <= 4)
                        {
                            // Short concat: single line
                            std::ostringstream exprStream;
                            exprStream << "{";
                            for (std::size_t i = 0; i < ops.size(); ++i)
                            {
                                if (i != 0)
                                {
                                    exprStream << ", ";
                                }
                                exprStream << inlineExpr.concatOperandExpr(ops[i]);
                            }
                            exprStream << "}";
                            expr = exprStream.str();
                        }
                        else
                        {
                            // Long concat: multi-line format
                            std::ostringstream exprStream;
                            exprStream << "{\n";
                            for (std::size_t i = 0; i < ops.size(); ++i)
                            {
                                exprStream << "    " << inlineExpr.concatOperandExpr(ops[i]);
                                if (i + 1 < ops.size())
                                {
                                    exprStream << ",";
                                }
                                exprStream << "\n";
                            }
                            exprStream << "  }";
                            expr = exprStream.str();
                        }
                        const int64_t resultWidth = graph->valueWidth(valueId);
                        if (concatWidthKnown && resultWidth > 0 && concatWidth > resultWidth)
                        {
                            expr = std::to_string(resultWidth) + "'(" + expr + ")";
                        }
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kReplicate:
                {
                    if (!ops.empty())
                    {
                        auto rep = getAttribute<int64_t>(*graph, defOp, "rep");
                        if (rep)
                        {
                            std::ostringstream exprStream;
                            exprStream << "{" << *rep << "{" << inlineExpr.concatOperandExpr(ops[0]) << "}}";
                            expr = exprStream.str();
                            const int64_t resultWidth = graph->valueWidth(valueId);
                            const int64_t operandWidth = graph->valueWidth(ops[0]);
                            if (resultWidth > 0 && operandWidth > 0 &&
                                graph->valueType(ops[0]) == wolvrix::lib::grh::ValueType::Logic &&
                                operandWidth * (*rep) > resultWidth)
                            {
                                expr = std::to_string(resultWidth) + "'(" + expr + ")";
                            }
                        }
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceStatic:
                {
                    if (!ops.empty())
                    {
                        auto sliceStart = getAttribute<int64_t>(*graph, defOp, "sliceStart");
                        auto sliceEnd = getAttribute<int64_t>(*graph, defOp, "sliceEnd");
                        if (sliceStart && sliceEnd)
                        {
                            const int64_t operandWidth = graph->valueWidth(ops[0]);
                            std::ostringstream exprStream;
                            if (operandWidth == 1)
                            {
                                exprStream << inlineExpr.valueExpr(ops[0]);
                            }
                            else
                            {
                                exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[0])) << "[";
                                if (*sliceStart == *sliceEnd)
                                {
                                    exprStream << *sliceStart;
                                }
                                else
                                {
                                    exprStream << *sliceEnd << ":" << *sliceStart;
                                }
                                exprStream << "]";
                            }
                            expr = exprStream.str();
                        }
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceDynamic:
                {
                    if (ops.size() >= 2)
                    {
                        auto width = getAttribute<int64_t>(*graph, defOp, "sliceWidth");
                        if (width)
                        {
                            const int64_t operandWidth = graph->valueWidth(ops[0]);
                            auto indexExpr = [&](wolvrix::lib::grh::ValueId indexId) -> std::string
                            {
                                if (auto literal = inlineConstExprFor(indexId))
                                {
                                    return *literal;
                                }
                                return inlineExpr.clampIndexExpr(indexId, operandWidth);
                            };
                            std::ostringstream exprStream;
                            if (operandWidth == 1)
                            {
                                exprStream << inlineExpr.valueExpr(ops[0]);
                            }
                            else if (*width == 1)
                            {
                                exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[0])) << "[" <<
                                    indexExpr(ops[1]) << "]";
                            }
                            else
                            {
                                exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[0])) << "[" <<
                                    indexExpr(ops[1]) << " +: " << *width << "]";
                            }
                            expr = exprStream.str();
                        }
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceArray:
                {
                    if (ops.size() >= 2)
                    {
                        auto width = getAttribute<int64_t>(*graph, defOp, "sliceWidth");
                        if (width)
                        {
                            const int64_t operandWidth = graph->valueWidth(ops[0]);
                            std::ostringstream exprStream;
                            if (*width == 1 && operandWidth == 1)
                            {
                                exprStream << inlineExpr.valueExpr(ops[0]);
                            }
                            else
                            {
                                exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[0])) << "[";
                                if (*width == 1)
                                {
                                    exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[1]));
                                }
                                else
                                {
                                    exprStream << parenIfNeeded(inlineExpr.valueExpr(ops[1])) << " * " << *width << " +: " << *width;
                                }
                                exprStream << "]";
                            }
                            expr = exprStream.str();
                        }
                    }
                    break;
                }
                default:
                    break;
                }
                if (graph->valueType(valueId) == wolvrix::lib::grh::ValueType::Logic &&
                    !expr.empty())
                {
                    expr = std::string(graph->valueSigned(valueId) ? "$signed(" : "$unsigned(") +
                           expr + ")";
                }
                dpiInlineResolving.erase(valueId);
                dpiInlineCache.emplace(valueId, expr);
                return expr;
            };
            auto valueExprSeq = [&](wolvrix::lib::grh::ValueId valueId) -> std::string
            {
                if (dpiInlineSink.valid() || valueDependsOnDpi(valueId))
                {
                    return dpiInlineExpr(valueId);
                }
                return valueExpr(valueId);
            };
            auto withDpiInlineSink = [&](wolvrix::lib::grh::OperationId sinkOp, auto &&fn)
            {
                const wolvrix::lib::grh::OperationId prev = dpiInlineSink;
                dpiInlineSink = sinkOp;
                dpiInlineCache.clear();
                dpiInlineResolving.clear();
                fn();
                dpiInlineCache.clear();
                dpiInlineResolving.clear();
                dpiInlineSink = prev;
            };
            auto isConstOne = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    return false;
                }
                if (graph->opKind(defOpId) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    return false;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                auto constValue = getAttribute<std::string>(*graph, defOp, "constValue");
                if (!constValue)
                {
                    return false;
                }
                std::string text = *constValue;
                text.erase(std::remove_if(text.begin(), text.end(),
                                          [](unsigned char ch)
                                          { return std::isspace(ch) || ch == '_'; }),
                           text.end());
                if (text == "1")
                {
                    return true;
                }
                if (text.size() >= 3 && text.back() == '1')
                {
                    const std::size_t quote = text.find('\'');
                    if (quote != std::string::npos && quote + 1 < text.size())
                    {
                        return true;
                    }
                }
                return false;
            };
            auto isConstZero = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    return false;
                }
                if (graph->opKind(defOpId) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    return false;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                auto constValue = getAttribute<std::string>(*graph, defOp, "constValue");
                if (!constValue)
                {
                    return false;
                }
                std::string text = *constValue;
                text.erase(std::remove_if(text.begin(), text.end(),
                                          [](unsigned char ch)
                                          { return std::isspace(ch) || ch == '_'; }),
                           text.end());
                if (text == "0")
                {
                    return true;
                }
                if (text.size() >= 3 && text.back() == '0')
                {
                    const std::size_t quote = text.find('\'');
                    if (quote != std::string::npos && quote + 1 < text.size())
                    {
                        return true;
                    }
                }
                return false;
            };
            auto isLogicNotOf = [&](wolvrix::lib::grh::ValueId maybeNot,
                                    wolvrix::lib::grh::ValueId operand) -> bool
            {
                if (!maybeNot.valid() || !operand.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOpId =
                    graph->valueDef(maybeNot);
                if (!defOpId.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                if (defOp.kind() != wolvrix::lib::grh::OperationKind::kLogicNot &&
                    defOp.kind() != wolvrix::lib::grh::OperationKind::kNot)
                {
                    return false;
                }
                const auto &ops = defOp.operands();
                return !ops.empty() && ops.front() == operand;
            };
            auto isAlwaysTrueByTruthTable = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (!valueId.valid())
                {
                    return false;
                }
                std::vector<wolvrix::lib::grh::ValueId> leaves;
                std::unordered_map<wolvrix::lib::grh::ValueId, std::size_t, wolvrix::lib::grh::ValueIdHash> leafIndex;

                std::function<void(wolvrix::lib::grh::ValueId)> collectLeaves =
                    [&](wolvrix::lib::grh::ValueId id) {
                        if (!id.valid())
                        {
                            return;
                        }
                        if (isConstOne(id) || isConstZero(id))
                        {
                            return;
                        }
                        const wolvrix::lib::grh::OperationId defId =
                            graph->valueDef(id);
                        if (!defId.valid())
                        {
                            if (leafIndex.find(id) == leafIndex.end())
                            {
                                leafIndex[id] = leaves.size();
                                leaves.push_back(id);
                            }
                            return;
                        }
                        const wolvrix::lib::grh::Operation op = graph->getOperation(defId);
                        if (op.kind() == wolvrix::lib::grh::OperationKind::kLogicAnd ||
                            op.kind() == wolvrix::lib::grh::OperationKind::kLogicOr ||
                            op.kind() == wolvrix::lib::grh::OperationKind::kLogicNot ||
                            op.kind() == wolvrix::lib::grh::OperationKind::kNot)
                        {
                            for (const auto operand : op.operands())
                            {
                                collectLeaves(operand);
                            }
                            return;
                        }
                        if (leafIndex.find(id) == leafIndex.end())
                        {
                            leafIndex[id] = leaves.size();
                            leaves.push_back(id);
                        }
                    };

                collectLeaves(valueId);
                if (leaves.size() > 8)
                {
                    return false;
                }

                auto evalExpr = [&](wolvrix::lib::grh::ValueId id,
                                    const std::vector<bool>& assignment,
                                    auto&& self) -> bool {
                    if (!id.valid())
                    {
                        return false;
                    }
                    if (isConstOne(id))
                    {
                        return true;
                    }
                    if (isConstZero(id))
                    {
                        return false;
                    }
                    const wolvrix::lib::grh::OperationId defId =
                        graph->valueDef(id);
                    if (!defId.valid())
                    {
                        auto it = leafIndex.find(id);
                        return it != leafIndex.end() && assignment[it->second];
                    }
                    const wolvrix::lib::grh::Operation op = graph->getOperation(defId);
                    if (op.kind() == wolvrix::lib::grh::OperationKind::kLogicAnd)
                    {
                        for (const auto operand : op.operands())
                        {
                            if (!self(operand, assignment, self))
                            {
                                return false;
                            }
                        }
                        return true;
                    }
                    if (op.kind() == wolvrix::lib::grh::OperationKind::kLogicOr)
                    {
                        for (const auto operand : op.operands())
                        {
                            if (self(operand, assignment, self))
                            {
                                return true;
                            }
                        }
                        return false;
                    }
                    if (op.kind() == wolvrix::lib::grh::OperationKind::kLogicNot ||
                        op.kind() == wolvrix::lib::grh::OperationKind::kNot)
                    {
                        const auto &ops = op.operands();
                        if (ops.empty())
                        {
                            return false;
                        }
                        return !self(ops.front(), assignment, self);
                    }
                    auto it = leafIndex.find(id);
                    return it != leafIndex.end() && assignment[it->second];
                };

                const std::size_t total =
                    leaves.empty() ? 1u : (static_cast<std::size_t>(1) << leaves.size());
                std::vector<bool> assignment(leaves.size(), false);
                for (std::size_t mask = 0; mask < total; ++mask)
                {
                    for (std::size_t i = 0; i < leaves.size(); ++i)
                    {
                        assignment[i] = ((mask >> i) & 1U) != 0U;
                    }
                    if (!evalExpr(valueId, assignment, evalExpr))
                    {
                        return false;
                    }
                }
                return true;
            };
            auto isAlwaysTrue = [&](wolvrix::lib::grh::ValueId valueId) -> bool
            {
                if (isConstOne(valueId))
                {
                    return true;
                }
                if (!valueId.valid())
                {
                    return false;
                }
                const wolvrix::lib::grh::OperationId defOpId =
                    graph->valueDef(valueId);
                if (defOpId.valid())
                {
                    const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                    if (defOp.kind() == wolvrix::lib::grh::OperationKind::kLogicOr)
                    {
                        const auto &ops = defOp.operands();
                        for (std::size_t i = 0; i < ops.size(); ++i)
                        {
                            for (std::size_t j = i + 1; j < ops.size(); ++j)
                            {
                                if (isLogicNotOf(ops[i], ops[j]) ||
                                    isLogicNotOf(ops[j], ops[i]))
                                {
                                    return true;
                                }
                            }
                        }
                    }
                }
                return isAlwaysTrueByTruthTable(valueId);
            };

            auto markPortAsRegIfNeeded = [&](wolvrix::lib::grh::ValueId valueId)
            {
                if (graph->valueType(valueId) != wolvrix::lib::grh::ValueType::Logic)
                {
                    return;
                }
                const std::string name = valueName(valueId);
                auto itPort = portDecls.find(name);
                if (itPort != portDecls.end() && itPort->second.dir == PortDir::Output)
                {
                    itPort->second.isReg = true;
                }
            };

            auto resolveMemorySymbol = [&](const wolvrix::lib::grh::Operation &userOp) -> std::optional<std::string>
            {
                auto attr = getAttribute<std::string>(*graph, userOp, "memSymbol");
                if (attr)
                {
                    return attr;
                }

                std::optional<std::string> candidate;
                for (const auto maybeId : graph->operations())
                {
                    const wolvrix::lib::grh::Operation maybeOp = graph->getOperation(maybeId);
                    if (maybeOp.kind() == wolvrix::lib::grh::OperationKind::kMemory)
                    {
                        if (candidate)
                        {
                            return std::nullopt;
                        }
                        candidate = std::string(maybeOp.symbolText());
                    }
                }
                return candidate;
            };

            std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> elidedConstValues;
            auto maskAllOnesLiteral = [&](std::string_view literal, int64_t width) -> bool
            {
                auto bits = parseConstMaskBits(literal, width);
                if (!bits || bits->empty())
                {
                    return false;
                }
                for (uint8_t bit : *bits)
                {
                    if (bit != 1)
                    {
                        return false;
                    }
                }
                return true;
            };
            auto registerWidthForSymbol = [&](const std::string &symbol,
                                              wolvrix::lib::grh::ValueId nextValue) -> int64_t
            {
                int64_t width = graph->valueWidth(nextValue);
                const wolvrix::lib::grh::OperationId regOpId = graph->findOperation(symbol);
                if (regOpId.valid())
                {
                    const wolvrix::lib::grh::Operation regOp = graph->getOperation(regOpId);
                    if (regOp.kind() == wolvrix::lib::grh::OperationKind::kRegister)
                    {
                        width = getAttribute<int64_t>(*graph, regOp, "width").value_or(width);
                    }
                }
                if (width <= 0)
                {
                    width = 1;
                }
                return width;
            };
            auto latchWidthForSymbol = [&](const std::string &symbol,
                                           wolvrix::lib::grh::ValueId nextValue) -> int64_t
            {
                int64_t width = graph->valueWidth(nextValue);
                const wolvrix::lib::grh::OperationId latchOpId = graph->findOperation(symbol);
                if (latchOpId.valid())
                {
                    const wolvrix::lib::grh::Operation latchOp = graph->getOperation(latchOpId);
                    if (latchOp.kind() == wolvrix::lib::grh::OperationKind::kLatch)
                    {
                        width = getAttribute<int64_t>(*graph, latchOp, "width").value_or(width);
                    }
                }
                if (width <= 0)
                {
                    width = 1;
                }
                return width;
            };
            auto memoryWidthForSymbol = [&](const std::string &symbol) -> int64_t
            {
                int64_t width = 1;
                const wolvrix::lib::grh::OperationId memOpId = graph->findOperation(symbol);
                if (memOpId.valid())
                {
                    const wolvrix::lib::grh::Operation memOp = graph->getOperation(memOpId);
                    width = getAttribute<int64_t>(*graph, memOp, "width").value_or(width);
                }
                if (width <= 0)
                {
                    width = 1;
                }
                return width;
            };
            for (const auto valueId : graph->values())
            {
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (!defOpId.valid())
                {
                    continue;
                }
                if (graph->opKind(defOpId) != wolvrix::lib::grh::OperationKind::kConstant)
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation defOp = graph->getOperation(defOpId);
                auto literalAttr = getAttribute<std::string>(*graph, defOp, "constValue");
                if (!literalAttr)
                {
                    continue;
                }
                auto itUses = valueUseMap.find(valueId);
                if (itUses == valueUseMap.end())
                {
                    continue;
                }
                bool allUsesAreFullMask = true;
                for (const auto userOpId : itUses->second)
                {
                    if (!userOpId.valid())
                    {
                        continue;
                    }
                    const auto ops = graph->opOperands(userOpId);
                    const auto userOpKind = graph->opKind(userOpId);
                    auto usedOnlyAt = [&](std::size_t index) -> bool
                    {
                        bool found = false;
                        for (std::size_t i = 0; i < ops.size(); ++i)
                        {
                            if (ops[i] != valueId)
                            {
                                continue;
                            }
                            if (i != index)
                            {
                                return false;
                            }
                            found = true;
                        }
                        return found;
                    };
                    switch (userOpKind)
                    {
                    case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
                    {
                        if (ops.size() < 3 || !usedOnlyAt(2))
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const wolvrix::lib::grh::Operation userOp = graph->getOperation(userOpId);
                        auto regSymbolAttr = getAttribute<std::string>(*graph, userOp, "regSymbol");
                        if (!regSymbolAttr)
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const int64_t width = registerWidthForSymbol(*regSymbolAttr, ops[1]);
                        if (!maskAllOnesLiteral(*literalAttr, width))
                        {
                            allUsesAreFullMask = false;
                        }
                        break;
                    }
                    case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                    {
                        if (ops.size() < 3 || !usedOnlyAt(2))
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const wolvrix::lib::grh::Operation userOp = graph->getOperation(userOpId);
                        auto latchSymbolAttr = getAttribute<std::string>(*graph, userOp, "latchSymbol");
                        if (!latchSymbolAttr)
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const int64_t width = latchWidthForSymbol(*latchSymbolAttr, ops[1]);
                        if (!maskAllOnesLiteral(*literalAttr, width))
                        {
                            allUsesAreFullMask = false;
                        }
                        break;
                    }
                    case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                    {
                        if (ops.size() < 4 || !usedOnlyAt(3))
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const wolvrix::lib::grh::Operation userOp = graph->getOperation(userOpId);
                        auto memSymbolAttr = resolveMemorySymbol(userOp);
                        if (!memSymbolAttr)
                        {
                            allUsesAreFullMask = false;
                            break;
                        }
                        const int64_t width = memoryWidthForSymbol(*memSymbolAttr);
                        if (!maskAllOnesLiteral(*literalAttr, width))
                        {
                            allUsesAreFullMask = false;
                        }
                        break;
                    }
                    default:
                        allUsesAreFullMask = false;
                        break;
                    }
                    if (!allUsesAreFullMask)
                    {
                        break;
                    }
                }
                if (allUsesAreFullMask)
                {
                    elidedConstValues.insert(valueId);
                }
            }

            // -------------------------
            // Port bindings (handle ports mapped to internal nets with different names)
            // -------------------------
            auto bindInputPort = [&](const std::string &portName, wolvrix::lib::grh::ValueId valueId)
            {
                const std::string valueSymbol = valueName(valueId);
                if (portName == valueSymbol)
                {
                    return;
                }
                ensureWireDecl(valueId);
                portBindingStmts.emplace_back(
                    "assign " + valueSymbol + " = " + portName + ";", wolvrix::lib::grh::OperationId::invalid());
            };

            auto bindOutputPort = [&](const std::string &portName, wolvrix::lib::grh::ValueId valueId)
            {
                const std::string valueSymbol = valueName(valueId);
                const std::string rhsExpr = valueExpr(valueId);
                if (storageBackedPorts.find(portName) != storageBackedPorts.end())
                {
                    return;
                }
                if (rhsExpr.empty() || portName == rhsExpr)
                {
                    return;
                }
                // If output RHS is the direct value symbol, ensure it is declared even
                // when that value is also tagged as a port value in GRH.
                if (!valueSymbol.empty() && rhsExpr == valueSymbol)
                {
                    ensureWireDecl(valueId);
                }
                portBindingStmts.emplace_back(
                    "assign " + portName + " = " + rhsExpr + ";", wolvrix::lib::grh::OperationId::invalid());
            };

            for (const auto &port : graph->inputPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                bindInputPort(port.name, port.value);
            }
            for (const auto &port : graph->outputPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                bindOutputPort(port.name, port.value);
            }
            auto emitInoutAssign = [&](const std::string &dest,
                                       wolvrix::lib::grh::ValueId oeValue,
                                       wolvrix::lib::grh::ValueId outValue,
                                       int64_t width,
                                       wolvrix::lib::grh::OperationId sourceOp) {
                const std::string oeExpr = valueExpr(oeValue);
                const std::string outExpr = valueExpr(outValue);
                const int64_t oeWidth = graph->valueWidth(oeValue);
                auto oeConstBits = [&]() -> std::optional<std::vector<uint8_t>>
                {
                    if (auto literal = constLiteralFor(oeValue))
                    {
                        std::string literalText = *literal;
                        if (auto sized = sizedLiteralIfUnsized(literalText, oeWidth))
                        {
                            literalText = *sized;
                        }
                        return parseConstMaskBits(literalText, oeWidth);
                    }
                    return std::nullopt;
                }();
                auto bitExpr = [&](wolvrix::lib::grh::ValueId valueId, int64_t bit, int64_t valueWidth) -> std::string {
                    if (auto literal = constLiteralFor(valueId))
                    {
                        std::string literalText = *literal;
                        if (auto sized = sizedLiteralIfUnsized(literalText, valueWidth))
                        {
                            literalText = *sized;
                        }
                        return "((" + literalText + " >> " + std::to_string(bit) + ") & 1'b1)";
                    }
                    return parenIfNeeded(valueExpr(valueId)) + "[" + std::to_string(bit) + "]";
                };
                if (oeConstBits && !oeConstBits->empty())
                {
                    const auto &bits = *oeConstBits;
                    bool allZero = true;
                    bool allOne = true;
                    for (uint8_t bit : bits)
                    {
                        allZero = allZero && (bit == 0);
                        allOne = allOne && (bit == 1);
                    }
                    if (allZero)
                    {
                        portBindingStmts.emplace_back(
                            "assign " + dest + " = {" + std::to_string(width) + "{1'bz}};",
                            sourceOp);
                        return;
                    }
                    if (allOne)
                    {
                        portBindingStmts.emplace_back(
                            "assign " + dest + " = " + outExpr + ";",
                            sourceOp);
                        return;
                    }
                }
                if (width <= 1 || oeWidth == 1)
                {
                    portBindingStmts.emplace_back(
                        "assign " + dest + " = " + oeExpr + " ? " + outExpr + " : {" +
                            std::to_string(width) + "{1'bz}};",
                        sourceOp);
                    return;
                }
                if (oeWidth == width)
                {
                    for (int64_t bit = 0; bit < width; ++bit)
                    {
                        const std::string oeBit =
                            (oeConstBits && bit < static_cast<int64_t>(oeConstBits->size()))
                                ? ((*oeConstBits)[bit] ? "1'b1" : "1'b0")
                                : bitExpr(oeValue, bit, oeWidth);
                        const std::string outBit = bitExpr(outValue, bit, width);
                        if (oeBit == "1'b0")
                        {
                            portBindingStmts.emplace_back(
                                "assign " + dest + "[" + std::to_string(bit) + "] = 1'bz;",
                                sourceOp);
                        }
                        else if (oeBit == "1'b1")
                        {
                            portBindingStmts.emplace_back(
                                "assign " + dest + "[" + std::to_string(bit) + "] = " + outBit + ";",
                                sourceOp);
                        }
                        else
                        {
                            portBindingStmts.emplace_back(
                                "assign " + dest + "[" + std::to_string(bit) + "] = " + oeBit +
                                    " ? " + outBit + " : 1'bz;",
                                sourceOp);
                        }
                    }
                    return;
                }
                portBindingStmts.emplace_back(
                    "assign " + dest + " = (" + parenIfNeeded(oeExpr) + " != {" + std::to_string(oeWidth) +
                        "{1'b0}}) ? " + valueExpr(outValue) + " : {" + std::to_string(width) + "{1'bz}};",
                    sourceOp);
            };
            for (const auto &port : graph->inoutPorts())
            {
                if (port.name.empty())
                {
                    continue;
                }
                const std::string &portName = port.name;
                const std::string inName = valueName(port.in);
                const int64_t width = graph->valueWidth(port.out);
                ensureWireDecl(port.in);
                ensureWireDecl(port.out);
                ensureWireDecl(port.oe);
                portBindingStmts.emplace_back(
                    "assign " + inName + " = " + portName + ";", wolvrix::lib::grh::OperationId::invalid());
                emitInoutAssign(portName, port.oe, port.out, width, wolvrix::lib::grh::OperationId::invalid());
            }

            // -------------------------
            // Operation traversal
            // -------------------------
            for (const auto opId : graph->operations())
            {
                const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
                const auto &operands = op.operands();
                const auto &results = op.results();
                const std::string opContext = std::string(op.symbolText());
                auto buildEventKey = [&](const wolvrix::lib::grh::Operation &eventOp,
                                         std::size_t eventStart) -> std::optional<SeqKey>
                {
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, eventOp, "eventEdge");
                    if (!eventEdges)
                    {
                        reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " missing eventEdge", opContext);
                        return std::nullopt;
                    }
                    const std::size_t eventCount = operands.size() > eventStart ? operands.size() - eventStart : 0;
                    if (eventCount == 0 || eventEdges->size() != eventCount)
                    {
                        reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " eventEdge size mismatch",
                                    opContext);
                        return std::nullopt;
                    }
                    SeqKey key;
                    key.events.reserve(eventCount);
                    for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        const wolvrix::lib::grh::ValueId signal = operands[eventStart + i];
                        if (!signal.valid())
                        {
                            reportError(std::string(wolvrix::lib::grh::toString(eventOp.kind())) + " missing event operand",
                                        opContext);
                            return std::nullopt;
                        }
                        key.events.push_back(SeqEvent{(*eventEdges)[i], signal});
                    }
                    return key;
                };
                auto buildSingleEventKey = [&](wolvrix::lib::grh::ValueId signal,
                                               const std::optional<std::string> &edgeAttr,
                                               std::string_view opName) -> std::optional<SeqKey>
                {
                    if (!signal.valid() || !edgeAttr || edgeAttr->empty())
                    {
                        reportError(std::string(opName) + " missing clk/clkPolarity", opContext);
                        return std::nullopt;
                    }
                    SeqKey key;
                    key.events.push_back(SeqEvent{*edgeAttr, signal});
                    return key;
                };

                switch (op.kind())
                {
                case wolvrix::lib::grh::OperationKind::kConstant:
                {
                    if (results.empty())
                    {
                        reportError("kConstant missing result", opContext);
                        break;
                    }
                    if (elidedConstValues.find(results[0]) != elidedConstValues.end())
                    {
                        break;
                    }
                    const wolvrix::lib::grh::Value constVal = graph->getValue(results[0]);
                    if (!constVal.isInput() && !constVal.isOutput() && !constVal.isInout() &&
                        constVal.users().empty())
                    {
                        break;
                    }
                    auto constAttr = getAttribute<std::string>(*graph, op, "constValue");
                    if (!constAttr)
                    {
                        reportError("kConstant missing constValue attribute", opContext);
                        break;
                    }
                    addValueAssign(results[0], formatConstLiteral(results[0], *constAttr), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSystemFunction:
                {
                    if (results.empty())
                    {
                        reportError("kSystemFunction missing result", opContext);
                        break;
                    }
                    auto name = getAttribute<std::string>(*graph, op, "name");
                    if (!name || name->empty())
                    {
                        reportError("kSystemFunction missing name", opContext);
                        break;
                    }
                    std::ostringstream expr;
                    expr << "$" << *name;
                    if (!operands.empty())
                    {
                        if (operands.size() <= 4)
                        {
                            // Short arg list: single line
                            expr << "(";
                            for (std::size_t i = 0; i < operands.size(); ++i)
                            {
                                if (i != 0)
                                {
                                    expr << ", ";
                                }
                                expr << valueExpr(operands[i]);
                            }
                            expr << ")";
                        }
                        else
                        {
                            // Long arg list: multi-line format
                            expr << "(\n";
                            for (std::size_t i = 0; i < operands.size(); ++i)
                            {
                                expr << "    " << valueExpr(operands[i]);
                                if (i + 1 < operands.size())
                                {
                                    expr << ",";
                                }
                                expr << "\n";
                            }
                            expr << "  )";
                        }
                    }
                    addValueAssign(results[0], expr.str(), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
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
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("Binary operation missing operands or results", opContext);
                        break;
                    }
                    int64_t resultWidth =
                        std::max(graph->valueWidth(operands[0]),
                                 graph->valueWidth(operands[1]));
                    const std::string tok = binOpToken(op.kind());
                    const std::string lhs = resultWidth > 0
                                                ? baseExpr.extendOperand(operands[0], resultWidth)
                                                : valueExpr(operands[0]);
                    const std::string rhs = resultWidth > 0
                                                ? baseExpr.extendOperand(operands[1], resultWidth)
                                                : valueExpr(operands[1]);
                    const bool signedCompare =
                        (op.kind() == wolvrix::lib::grh::OperationKind::kLt ||
                         op.kind() == wolvrix::lib::grh::OperationKind::kLe ||
                         op.kind() == wolvrix::lib::grh::OperationKind::kGt ||
                         op.kind() == wolvrix::lib::grh::OperationKind::kGe) &&
                        graph->valueSigned(operands[0]) &&
                        graph->valueSigned(operands[1]);
                    const std::string lhsExpr = signedCompare ? "$signed(" + lhs + ")" : lhs;
                    const std::string rhsExpr = signedCompare ? "$signed(" + rhs + ")" : rhs;
                    addValueAssign(results[0], lhsExpr + " " + tok + " " + rhsExpr, opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kAnd:
                case wolvrix::lib::grh::OperationKind::kOr:
                case wolvrix::lib::grh::OperationKind::kXor:
                case wolvrix::lib::grh::OperationKind::kXnor:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("Binary operation missing operands or results", opContext);
                        break;
                    }
                    int64_t resultWidth = graph->valueWidth(results[0]);
                    if (resultWidth <= 0)
                    {
                        resultWidth = std::max(graph->valueWidth(operands[0]),
                                               graph->valueWidth(operands[1]));
                    }
                    const std::string tok = binOpToken(op.kind());
                    const std::string lhs = resultWidth > 0
                                                ? baseExpr.extendOperand(operands[0], resultWidth)
                                                : valueExpr(operands[0]);
                    const std::string rhs = resultWidth > 0
                                                ? baseExpr.extendOperand(operands[1], resultWidth)
                                                : valueExpr(operands[1]);
                    addValueAssign(results[0], lhs + " " + tok + " " + rhs, opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kShl:
                case wolvrix::lib::grh::OperationKind::kLShr:
                case wolvrix::lib::grh::OperationKind::kAShr:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("Binary operation missing operands or results", opContext);
                        break;
                    }
                    const int64_t resultWidth = graph->valueWidth(results[0]);
                    const bool signExtend =
                        op.kind() == wolvrix::lib::grh::OperationKind::kAShr &&
                        graph->valueSigned(operands[0]);
                    const std::string tok = binOpToken(op.kind());
                    const std::string lhs = resultWidth > 0
                                                ? baseExpr.extendShiftOperand(operands[0], resultWidth, signExtend)
                                                : valueExpr(operands[0]);
                    addValueAssign(results[0],
                                   lhs + " " + tok + " " + valueExpr(operands[1]),
                                   opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kAdd:
                case wolvrix::lib::grh::OperationKind::kSub:
                case wolvrix::lib::grh::OperationKind::kMul:
                case wolvrix::lib::grh::OperationKind::kDiv:
                case wolvrix::lib::grh::OperationKind::kMod:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("Binary operation missing operands or results", opContext);
                        break;
                    }
                    int64_t resultWidth = graph->valueWidth(results[0]);
                    if (resultWidth <= 0)
                    {
                        resultWidth = std::max(graph->valueWidth(operands[0]),
                                               graph->valueWidth(operands[1]));
                    }
                    const std::string tok = binOpToken(op.kind());
                    addValueAssign(results[0],
                                   baseExpr.extendOperand(operands[0], resultWidth) + " " + tok + " " +
                                       baseExpr.extendOperand(operands[1], resultWidth),
                                   opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLogicAnd:
                case wolvrix::lib::grh::OperationKind::kLogicOr:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("Binary operation missing operands or results", opContext);
                        break;
                    }
                    const std::string tok = binOpToken(op.kind());
                    addValueAssign(results[0],
                                   baseExpr.logicalOperand(operands[0]) + " " + tok + " " +
                                       baseExpr.logicalOperand(operands[1]),
                                   opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kNot:
                case wolvrix::lib::grh::OperationKind::kReduceAnd:
                case wolvrix::lib::grh::OperationKind::kReduceOr:
                case wolvrix::lib::grh::OperationKind::kReduceXor:
                case wolvrix::lib::grh::OperationKind::kReduceNor:
                case wolvrix::lib::grh::OperationKind::kReduceNand:
                case wolvrix::lib::grh::OperationKind::kReduceXnor:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("Unary operation missing operands or results", opContext);
                        break;
                    }
                    const std::string tok = unaryOpToken(op.kind());
                    addValueAssign(results[0], tok + valueExpr(operands[0]), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLogicNot:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("Unary operation missing operands or results", opContext);
                        break;
                    }
                    addValueAssign(results[0], "!" + baseExpr.logicalOperand(operands[0]), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMux:
                {
                    if (operands.size() < 3 || results.empty())
                    {
                        reportError("kMux missing operands or results", opContext);
                        break;
                    }
                    int64_t resultWidth = graph->valueWidth(results[0]);
                    const std::string lhs = baseExpr.extendOperand(operands[1], resultWidth);
                    const std::string rhs = baseExpr.extendOperand(operands[2], resultWidth);
                    addValueAssign(results[0],
                                   valueExpr(operands[0]) + " ? " + lhs + " : " + rhs,
                                   opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kAssign:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("kAssign missing operands or results", opContext);
                        break;
                    }
                    int64_t resultWidth = graph->valueWidth(results[0]);
                    const std::string rhs = baseExpr.extendOperand(operands[0], resultWidth);
                    addValueAssign(results[0], rhs, opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kConcat:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("kConcat missing operands or results", opContext);
                        break;
                    }
                    const int64_t resultWidth = graph->valueWidth(results[0]);
                    int64_t concatWidth = 0;
                    bool concatWidthKnown = true;
                    for (const auto operand : operands)
                    {
                        if (!operand.valid())
                        {
                            concatWidthKnown = false;
                            break;
                        }
                        if (graph->valueType(operand) != wolvrix::lib::grh::ValueType::Logic)
                        {
                            concatWidthKnown = false;
                            break;
                        }
                        const int64_t w = graph->valueWidth(operand);
                        if (w <= 0)
                        {
                            concatWidthKnown = false;
                            break;
                        }
                        concatWidth += w;
                    }
                    if (operands.size() == 1)
                    {
                        addValueAssign(results[0], valueExpr(operands[0]), opId);
                        ensureWireDecl(results[0]);
                        break;
                    }
                    std::ostringstream expr;
                    if (operands.size() <= 4)
                    {
                        // Short concat: single line
                        expr << "{";
                        for (std::size_t i = 0; i < operands.size(); ++i)
                        {
                            if (i != 0)
                            {
                                expr << ", ";
                            }
                            expr << baseExpr.concatOperandExpr(operands[i]);
                        }
                        expr << "}";
                    }
                    else
                    {
                        // Long concat: multi-line format
                        expr << "{\n";
                        for (std::size_t i = 0; i < operands.size(); ++i)
                        {
                            expr << "    " << baseExpr.concatOperandExpr(operands[i]);
                            if (i + 1 < operands.size())
                            {
                                expr << ",";
                            }
                            expr << "\n";
                        }
                        expr << "  }";
                    }
                    std::string exprText = expr.str();
                    if (concatWidthKnown && resultWidth > 0 && concatWidth > resultWidth)
                    {
                        const std::string tempName = makeConcatTempName();
                        wireDecls.emplace(tempName, NetDecl{concatWidth, false, std::nullopt});
                        addAssign("assign " + tempName + " = " + exprText + ";", opId);
                        if (resultWidth == 1)
                        {
                            exprText = tempName + "[0]";
                        }
                        else
                        {
                            exprText = tempName + "[" + std::to_string(resultWidth - 1) + ":0]";
                        }
                    }
                    addValueAssign(results[0], exprText, opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kReplicate:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("kReplicate missing operands or results", opContext);
                        break;
                    }
                    auto rep = getAttribute<int64_t>(*graph, op, "rep");
                    if (!rep)
                    {
                        reportError("kReplicate missing rep attribute", opContext);
                        break;
                    }
                    std::ostringstream expr;
                    expr << "{" << *rep << "{" << baseExpr.concatOperandExpr(operands[0]) << "}}";
                    std::string exprText = expr.str();
                    const int64_t resultWidth = graph->valueWidth(results[0]);
                    if (resultWidth > 0 && graph->valueType(operands[0]) == wolvrix::lib::grh::ValueType::Logic)
                    {
                        const int64_t opWidth = graph->valueWidth(operands[0]);
                        if (opWidth > 0)
                        {
                            const int64_t repWidth = opWidth * (*rep);
                            if (repWidth > resultWidth)
                            {
                                const std::string tempName = makeConcatTempName();
                                wireDecls.emplace(tempName, NetDecl{repWidth, false, std::nullopt});
                                addAssign("assign " + tempName + " = " + exprText + ";", opId);
                                if (resultWidth == 1)
                                {
                                    exprText = tempName + "[0]";
                                }
                                else
                                {
                                    exprText = tempName + "[" + std::to_string(resultWidth - 1) + ":0]";
                                }
                            }
                        }
                    }
                    addValueAssign(results[0], exprText, opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceStatic:
                {
                    if (operands.empty() || results.empty())
                    {
                        reportError("kSliceStatic missing operands or results", opContext);
                        break;
                    }
                    auto sliceStart = getAttribute<int64_t>(*graph, op, "sliceStart");
                    auto sliceEnd = getAttribute<int64_t>(*graph, op, "sliceEnd");
                    if (!sliceStart || !sliceEnd)
                    {
                        reportError("kSliceStatic missing sliceStart or sliceEnd", opContext);
                        break;
                    }
                    const int64_t operandWidth = graph->valueWidth(operands[0]);
                    std::ostringstream expr;
                    if (operandWidth == 1)
                    {
                        if (*sliceStart != 0 || *sliceEnd != 0)
                        {
                            reportError("kSliceStatic index out of range for scalar", opContext);
                        }
                        expr << valueExpr(operands[0]);
                    }
                    else
                    {
                        expr << parenIfNeeded(valueExpr(operands[0])) << "[";
                        if (*sliceStart == *sliceEnd)
                        {
                            expr << *sliceStart;
                        }
                        else
                        {
                            expr << *sliceEnd << ":" << *sliceStart;
                        }
                        expr << "]";
                    }
                    addValueAssign(results[0], expr.str(), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceDynamic:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("kSliceDynamic missing operands or results", opContext);
                        break;
                    }
                    auto width = getAttribute<int64_t>(*graph, op, "sliceWidth");
                    if (!width)
                    {
                        reportError("kSliceDynamic missing sliceWidth", opContext);
                        break;
                    }
                    const int64_t operandWidth = graph->valueWidth(operands[0]);
                    auto indexExpr = [&](wolvrix::lib::grh::ValueId indexId) -> std::string
                    {
                        if (auto literal = inlineConstExprFor(indexId))
                        {
                            return *literal;
                        }
                        return baseExpr.clampIndexExpr(indexId, operandWidth);
                    };
                    std::ostringstream expr;
                    if (operandWidth == 1)
                    {
                        if (*width != 1)
                        {
                            reportError("kSliceDynamic width exceeds scalar", opContext);
                        }
                        expr << valueExpr(operands[0]);
                    }
                    else if (*width == 1)
                    {
                        expr << parenIfNeeded(valueExpr(operands[0])) << "["
                             << indexExpr(operands[1]) << "]";
                    }
                    else
                    {
                        expr << parenIfNeeded(valueExpr(operands[0])) << "["
                             << indexExpr(operands[1]) << " +: " << *width << "]";
                    }
                    addValueAssign(results[0], expr.str(), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSliceArray:
                {
                    if (operands.size() < 2 || results.empty())
                    {
                        reportError("kSliceArray missing operands or results", opContext);
                        break;
                    }
                    auto width = getAttribute<int64_t>(*graph, op, "sliceWidth");
                    if (!width)
                    {
                        reportError("kSliceArray missing sliceWidth", opContext);
                        break;
                    }
                    const int64_t operandWidth = graph->valueWidth(operands[0]);
                    auto indexExpr = [&](wolvrix::lib::grh::ValueId indexId) -> std::string
                    {
                        if (auto literal = inlineConstExprFor(indexId))
                        {
                            return *literal;
                        }
                        return valueExpr(indexId);
                    };
                    std::ostringstream expr;
                    if (*width == 1 && operandWidth == 1)
                    {
                        expr << valueExpr(operands[0]);
                    }
                    else
                    {
                        expr << parenIfNeeded(valueExpr(operands[0])) << "[";
                        if (*width == 1)
                        {
                            expr << parenIfNeeded(indexExpr(operands[1]));
                        }
                        else
                        {
                            expr << parenIfNeeded(indexExpr(operands[1])) << " * " << *width << " +: " << *width;
                        }
                        expr << "]";
                    }
                    addValueAssign(results[0], expr.str(), opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kRegister:
                {
                    if (!operands.empty() || !results.empty())
                    {
                        reportError("kRegister should not have operands or results", opContext);
                        break;
                    }
                    auto widthAttr = getAttribute<int64_t>(*graph, op, "width");
                    auto isSignedAttr = getAttribute<bool>(*graph, op, "isSigned");
                    if (!widthAttr || !isSignedAttr)
                    {
                        reportError("kRegister missing width/isSigned", opContext);
                        break;
                    }
                    if (opContext.empty())
                    {
                        reportError("kRegister missing symbol", opContext);
                        break;
                    }
                    int64_t width = *widthAttr;
                    if (width <= 0)
                    {
                        reportError("kRegister width must be > 0", opContext);
                        width = 1;
                    }
                    if (storageBackedPorts.find(opContext) == storageBackedPorts.end())
                    {
                        ensureRegDecl(opContext, width, *isSignedAttr, wolvrix::lib::grh::ValueType::Logic,
                                      op.srcLoc());
                    }

                    auto initValue = getAttribute<std::string>(*graph, op, "initValue");
                    if (initValue)
                    {
                        std::ostringstream oss;
                        appendIndented(oss, 2, opContext + " = " + *initValue + ";");
                        addSimpleStmt("initial", oss.str(), opId);
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLatch:
                {
                    if (!operands.empty() || !results.empty())
                    {
                        reportError("kLatch should not have operands or results", opContext);
                        break;
                    }
                    auto widthAttr = getAttribute<int64_t>(*graph, op, "width");
                    auto isSignedAttr = getAttribute<bool>(*graph, op, "isSigned");
                    if (!widthAttr || !isSignedAttr)
                    {
                        reportError("kLatch missing width/isSigned", opContext);
                        break;
                    }
                    if (opContext.empty())
                    {
                        reportError("kLatch missing symbol", opContext);
                        break;
                    }
                    int64_t width = *widthAttr;
                    if (width <= 0)
                    {
                        reportError("kLatch width must be > 0", opContext);
                        width = 1;
                    }
                    if (storageBackedPorts.find(opContext) == storageBackedPorts.end())
                    {
                        ensureRegDecl(opContext, width, *isSignedAttr, wolvrix::lib::grh::ValueType::Logic,
                                      op.srcLoc());
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                {
                    if (!operands.empty() || results.empty())
                    {
                        reportError("kRegisterReadPort missing results", opContext);
                        break;
                    }
                    if (elidedReadPortValues.find(results[0]) != elidedReadPortValues.end())
                    {
                        break;
                    }
                    auto regSymbolAttr = getAttribute<std::string>(*graph, op, "regSymbol");
                    if (!regSymbolAttr)
                    {
                        reportError("kRegisterReadPort missing regSymbol", opContext);
                        break;
                    }
                    const std::string &regName = *regSymbolAttr;
                    if (valueName(results[0]) == regName)
                    {
                        markPortAsRegIfNeeded(results[0]);
                        break;
                    }
                    addAssign("assign " + valueName(results[0]) + " = " + regName + ";", opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                {
                    if (!operands.empty() || results.empty())
                    {
                        reportError("kLatchReadPort missing results", opContext);
                        break;
                    }
                    if (elidedReadPortValues.find(results[0]) != elidedReadPortValues.end())
                    {
                        break;
                    }
                    auto latchSymbolAttr = getAttribute<std::string>(*graph, op, "latchSymbol");
                    if (!latchSymbolAttr)
                    {
                        reportError("kLatchReadPort missing latchSymbol", opContext);
                        break;
                    }
                    const std::string &latchName = *latchSymbolAttr;
                    if (valueName(results[0]) == latchName)
                    {
                        markPortAsRegIfNeeded(results[0]);
                        break;
                    }
                    addAssign("assign " + valueName(results[0]) + " = " + latchName + ";", opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
                {
                    if (operands.size() < 3)
                    {
                        reportError("kRegisterWritePort missing operands", opContext);
                        break;
                    }
                    const wolvrix::lib::grh::ValueId updateCond = operands[0];
                    const wolvrix::lib::grh::ValueId nextValue = operands[1];
                    const wolvrix::lib::grh::ValueId mask = operands[2];
                    if (!updateCond.valid() || !nextValue.valid() || !mask.valid())
                    {
                        reportError("kRegisterWritePort missing operands", opContext);
                        break;
                    }
                    if (graph->valueWidth(updateCond) != 1)
                    {
                        reportError("kRegisterWritePort updateCond must be 1 bit", opContext);
                        break;
                    }
                    auto regSymbolAttr = getAttribute<std::string>(*graph, op, "regSymbol");
                    if (!regSymbolAttr)
                    {
                        reportError("kRegisterWritePort missing regSymbol", opContext);
                        break;
                    }
                    const std::string &regName = *regSymbolAttr;
                    auto seqKey = buildEventKey(op, 3);
                    if (!seqKey)
                    {
                        break;
                    }

                    int64_t regWidth = graph->valueWidth(nextValue);
                    const wolvrix::lib::grh::OperationId regOpId = graph->findOperation(*regSymbolAttr);
                    if (regOpId.valid())
                    {
                        const wolvrix::lib::grh::Operation regOp = graph->getOperation(regOpId);
                        if (regOp.kind() == wolvrix::lib::grh::OperationKind::kRegister)
                        {
                            regWidth = getAttribute<int64_t>(*graph, regOp, "width").value_or(regWidth);
                        }
                    }
                    if (regWidth <= 0)
                    {
                        regWidth = 1;
                    }

                    std::string updateExpr;
                    std::string nextExpr;
                    std::string maskExpr;
                    std::string nextExprFull;
                    auto extendSeqOperand = [&](wolvrix::lib::grh::ValueId valueId, int64_t targetWidth) -> std::string
                    {
                        ExprTools seqExpr = baseExpr;
                        seqExpr.valueExpr = valueExprSeq;
                        return seqExpr.extendOperand(valueId, targetWidth);
                    };
                    if (dpiDrivenStateOps.find(opId) != dpiDrivenStateOps.end())
                    {
                        withDpiInlineSink(opId, [&]() {
                            updateExpr = valueExprSeq(updateCond);
                            nextExpr = valueExprSeq(nextValue);
                            maskExpr = valueExprSeq(mask);
                            nextExprFull = extendSeqOperand(nextValue, regWidth);
                        });
                    }
                    else
                    {
                        updateExpr = valueExprSeq(updateCond);
                        nextExpr = valueExprSeq(nextValue);
                        maskExpr = valueExprSeq(mask);
                        nextExprFull = extendSeqOperand(nextValue, regWidth);
                    }
                    if (auto inlineUpdate = inlineConstExprFor(updateCond))
                    {
                        updateExpr = *inlineUpdate;
                    }
                    if (nextExprFull.empty())
                    {
                        nextExprFull = extendSeqOperand(nextValue, regWidth);
                    }

                    const bool guardUpdate = !isConstOne(updateCond);
                    const int baseIndent = guardUpdate ? 3 : 2;
                    std::optional<std::vector<uint8_t>> maskBits;
                    if (auto maskLiteral = constLiteralFor(mask))
                    {
                        maskBits = parseConstMaskBits(*maskLiteral, regWidth);
                    }
                    auto maskAll = [&](uint8_t value) -> bool
                    {
                        if (!maskBits)
                        {
                            return false;
                        }
                        for (uint8_t bit : *maskBits)
                        {
                            if (bit != value)
                            {
                                return false;
                            }
                        }
                        return true;
                    };
                    const int64_t dataWidth = graph->valueWidth(nextValue);
                    const bool maskAllOnes = maskBits ? maskAll(1) : false;
                    if (dataWidth > 0 && regWidth > 0 && dataWidth != regWidth && !maskAllOnes)
                    {
                        reportWarning("kRegisterWritePort data width does not match register width", opContext);
                    }
                    std::ostringstream stmt;
                    if (guardUpdate)
                    {
                        appendIndented(stmt, 2, "if (" + updateExpr + ") begin");
                    }
                    if (maskBits)
                    {
                        const bool allZero = maskAll(0);
                        const bool allOnes = maskAll(1);
                        if (allZero)
                        {
                            break;
                        }
                        if (allOnes || regWidth <= 1)
                        {
                            appendIndented(stmt, baseIndent, regName + " <= " + nextExprFull + ";");
                        }
                        else
                        {
                            for (int64_t bit = 0; bit < regWidth; ++bit)
                            {
                                if ((*maskBits)[static_cast<std::size_t>(bit)] != 0)
                                {
                                    appendIndented(stmt, baseIndent,
                                                   regName + "[" + std::to_string(bit) + "] <= " +
                                                       nextExpr + "[" + std::to_string(bit) + "];");
                                }
                            }
                        }
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                        addSequentialStmt(*seqKey, stmt.str(), opId);
                        break;
                    }
                    if (regWidth <= 1)
                    {
                        appendIndented(stmt, baseIndent, "if (" + maskExpr + ") begin");
                        appendIndented(stmt, baseIndent + 1, regName + " <= " + nextExprFull + ";");
                        appendIndented(stmt, baseIndent, "end");
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                        addSequentialStmt(*seqKey, stmt.str(), opId);
                        break;
                    }
                    appendIndented(stmt, baseIndent,
                                   "if (" + maskExpr + " == {" + std::to_string(regWidth) + "{1'b1}}) begin");
                    appendIndented(stmt, baseIndent + 1, regName + " <= " + nextExprFull + ";");
                    appendIndented(stmt, baseIndent, "end else begin");
                    appendIndented(stmt, baseIndent + 1, "integer i;");
                    appendIndented(stmt, baseIndent + 1,
                                   "for (i = 0; i < " + std::to_string(regWidth) + "; i = i + 1) begin");
                    appendIndented(stmt, baseIndent + 2, "if (" + maskExpr + "[i]) begin");
                    appendIndented(stmt, baseIndent + 3,
                                   regName + "[i] <= " + nextExpr + "[i];");
                    appendIndented(stmt, baseIndent + 2, "end");
                    appendIndented(stmt, baseIndent + 1, "end");
                    appendIndented(stmt, baseIndent, "end");
                    if (guardUpdate)
                    {
                        appendIndented(stmt, 2, "end");
                    }
                    addSequentialStmt(*seqKey, stmt.str(), opId);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                {
                    if (operands.size() < 3)
                    {
                        reportError("kLatchWritePort missing operands", opContext);
                        break;
                    }
                    const wolvrix::lib::grh::ValueId updateCond = operands[0];
                    const wolvrix::lib::grh::ValueId nextValue = operands[1];
                    const wolvrix::lib::grh::ValueId mask = operands[2];
                    if (!updateCond.valid() || !nextValue.valid() || !mask.valid())
                    {
                        reportError("kLatchWritePort missing operands", opContext);
                        break;
                    }
                    if (graph->valueWidth(updateCond) != 1)
                    {
                        reportError("kLatchWritePort updateCond must be 1 bit", opContext);
                        break;
                    }
                    auto latchSymbolAttr = getAttribute<std::string>(*graph, op, "latchSymbol");
                    if (!latchSymbolAttr)
                    {
                        reportError("kLatchWritePort missing latchSymbol", opContext);
                        break;
                    }
                    const std::string &latchName = *latchSymbolAttr;
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, op, "eventEdge");
                    if (eventEdges && !eventEdges->empty())
                    {
                        reportError("kLatchWritePort must not have eventEdge", opContext);
                        break;
                    }

                    int64_t latchWidth = graph->valueWidth(nextValue);
                    const wolvrix::lib::grh::OperationId latchOpId = graph->findOperation(*latchSymbolAttr);
                    if (latchOpId.valid())
                    {
                        const wolvrix::lib::grh::Operation latchOp = graph->getOperation(latchOpId);
                        if (latchOp.kind() == wolvrix::lib::grh::OperationKind::kLatch)
                        {
                            latchWidth = getAttribute<int64_t>(*graph, latchOp, "width").value_or(latchWidth);
                        }
                    }
                    if (latchWidth <= 0)
                    {
                        latchWidth = 1;
                    }

                    std::string updateExpr;
                    std::string nextExpr;
                    std::string maskExpr;
                    std::string nextExprFull;
                    auto extendSeqOperand = [&](wolvrix::lib::grh::ValueId valueId, int64_t targetWidth) -> std::string
                    {
                        ExprTools seqExpr = baseExpr;
                        seqExpr.valueExpr = valueExprSeq;
                        return seqExpr.extendOperand(valueId, targetWidth);
                    };
                    if (dpiDrivenStateOps.find(opId) != dpiDrivenStateOps.end())
                    {
                        withDpiInlineSink(opId, [&]() {
                            updateExpr = valueExprSeq(updateCond);
                            nextExpr = valueExprSeq(nextValue);
                            maskExpr = valueExprSeq(mask);
                            nextExprFull = extendSeqOperand(nextValue, latchWidth);
                        });
                    }
                    else
                    {
                        updateExpr = valueExprSeq(updateCond);
                        nextExpr = valueExprSeq(nextValue);
                        maskExpr = valueExprSeq(mask);
                        nextExprFull = extendSeqOperand(nextValue, latchWidth);
                    }
                    if (auto inlineUpdate = inlineConstExprFor(updateCond))
                    {
                        updateExpr = *inlineUpdate;
                    }
                    if (nextExprFull.empty())
                    {
                        nextExprFull = extendSeqOperand(nextValue, latchWidth);
                    }

                    const bool guardUpdate = !isAlwaysTrue(updateCond);
                    const int baseIndent = guardUpdate ? 3 : 2;
                    std::optional<std::vector<uint8_t>> maskBits;
                    if (auto maskLiteral = constLiteralFor(mask))
                    {
                        maskBits = parseConstMaskBits(*maskLiteral, latchWidth);
                    }
                    auto maskAll = [&](uint8_t value) -> bool
                    {
                        if (!maskBits)
                        {
                            return false;
                        }
                        for (uint8_t bit : *maskBits)
                        {
                            if (bit != value)
                            {
                                return false;
                            }
                        }
                        return true;
                    };
                    const int64_t dataWidth = graph->valueWidth(nextValue);
                    const bool maskAllOnes = maskBits ? maskAll(1) : false;
                    if (dataWidth > 0 && latchWidth > 0 && dataWidth != latchWidth && !maskAllOnes)
                    {
                        reportWarning("kLatchWritePort data width does not match latch width", opContext);
                    }
                    if (!guardUpdate && maskBits && maskAllOnes)
                    {
                        std::ostringstream combStmt;
                        appendIndented(combStmt, 2, latchName + " = " + nextExprFull + ";");
                        addSimpleStmt("always_comb", combStmt.str(), opId);
                        break;
                    }

                    std::ostringstream stmt;
                    if (guardUpdate)
                    {
                        appendIndented(stmt, 2, "if (" + updateExpr + ") begin");
                    }
                    if (maskBits)
                    {
                        const bool allZero = maskAll(0);
                        const bool allOnes = maskAll(1);
                        if (allZero)
                        {
                            break;
                        }
                        if (allOnes || latchWidth <= 1)
                        {
                            appendIndented(stmt, baseIndent, latchName + " = " + nextExprFull + ";");
                        }
                        else
                        {
                            for (int64_t bit = 0; bit < latchWidth; ++bit)
                            {
                                if ((*maskBits)[static_cast<std::size_t>(bit)] != 0)
                                {
                                    appendIndented(stmt, baseIndent,
                                                   latchName + "[" + std::to_string(bit) + "] = " +
                                                       nextExpr + "[" + std::to_string(bit) + "];");
                                }
                            }
                        }
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                    }
                    else if (latchWidth <= 1)
                    {
                        appendIndented(stmt, baseIndent, "if (" + maskExpr + ") begin");
                        appendIndented(stmt, baseIndent + 1, latchName + " = " + nextExprFull + ";");
                        appendIndented(stmt, baseIndent, "end");
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                    }
                    else
                    {
                        appendIndented(stmt, baseIndent,
                                       "if (" + maskExpr + " == {" + std::to_string(latchWidth) + "{1'b1}}) begin");
                        appendIndented(stmt, baseIndent + 1, latchName + " = " + nextExprFull + ";");
                        appendIndented(stmt, baseIndent, "end else begin");
                        appendIndented(stmt, baseIndent + 1, "integer i;");
                        appendIndented(stmt, baseIndent + 1,
                                       "for (i = 0; i < " + std::to_string(latchWidth) + "; i = i + 1) begin");
                        appendIndented(stmt, baseIndent + 2, "if (" + maskExpr + "[i]) begin");
                        appendIndented(stmt, baseIndent + 3,
                                       latchName + "[i] = " + nextExpr + "[i];");
                        appendIndented(stmt, baseIndent + 2, "end");
                        appendIndented(stmt, baseIndent + 1, "end");
                        appendIndented(stmt, baseIndent, "end");
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                    }

                    if (!stmt.str().empty())
                    {
                        std::ostringstream block;
                        block << "  always_latch begin\n";
                        block << stmt.str();
                        block << "  end";
                        addLatchBlock(block.str(), opId);
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMemory:
                {
                    auto widthAttr = getAttribute<int64_t>(*graph, op, "width");
                    auto rowAttr = getAttribute<int64_t>(*graph, op, "row");
                    auto isSignedAttr = getAttribute<bool>(*graph, op, "isSigned");
                    if (!widthAttr || !rowAttr || !isSignedAttr)
                    {
                        reportError("kMemory missing width/row/isSigned", opContext);
                        break;
                    }
                    std::ostringstream decl;
                    decl << "reg " << (*isSignedAttr ? "signed " : "") << "[" << (*widthAttr - 1) << ":0] " << opContext << " [0:" << (*rowAttr - 1) << "];";
                    memoryDecls.emplace_back(decl.str(), opId);
                    declaredNames.insert(opContext);

                    auto initKinds = getAttribute<std::vector<std::string>>(*graph, op, "initKind");
                    auto initFiles = getAttribute<std::vector<std::string>>(*graph, op, "initFile");
                    auto initValues = getAttribute<std::vector<std::string>>(*graph, op, "initValue");
                    if (initKinds || initFiles)
                    {
                        if (!initKinds || !initFiles || initKinds->size() != initFiles->size())
                        {
                            reportError("kMemory initKind/initFile size mismatch", opContext);
                            break;
                        }
                        const std::size_t count = initKinds->size();
                        auto starts = getAttribute<std::vector<int64_t>>(*graph, op, "initStart");
                        auto lens = getAttribute<std::vector<int64_t>>(*graph, op, "initLen");
                        if (!starts || !lens || starts->size() != count || lens->size() != count)
                        {
                            reportError("kMemory initStart/initLen size mismatch", opContext);
                            break;
                        }
                        const std::vector<int64_t> &localStarts = *starts;
                        const std::vector<int64_t> &localLens = *lens;
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            const std::string& kind = (*initKinds)[i];
                            std::string stmt;
                            
                            if (kind == "readmemh" || kind == "readmemb")
                            {
                                // readmemh/readmemb: $readmemh("file", memory, start, finish);
                                stmt = "$" + kind + "(\"" + escapeSvString((*initFiles)[i]) + "\", " + opContext;
                                if (localStarts[i] >= 0)
                                {
                                    stmt.append(", ");
                                    stmt.append(std::to_string(localStarts[i]));
                                    if (localLens[i] > 0)
                                    {
                                        stmt.append(", ");
                                        stmt.append(std::to_string(localStarts[i] + localLens[i] - 1));
                                    }
                                }
                                stmt.append(");");
                            }
                            else if (kind == "literal")
                            {
                                // Direct assignment: memory[addr] = value;
                                std::string value = (initValues && i < initValues->size()) ? (*initValues)[i] : "0";
                                if (localStarts[i] < 0)
                                {
                                    // Full init
                                    stmt = "for (int __i = 0; __i < " + std::to_string(*rowAttr) + "; __i = __i + 1) " +
                                           opContext + "[__i] = " + value + ";";
                                }
                                else if (localLens[i] <= 0)
                                {
                                    reportError("kMemory literal initLen must be > 0", opContext);
                                    break;
                                }
                                else if (localLens[i] == 1)
                                {
                                    // Specific address
                                    stmt = opContext + "[" + std::to_string(localStarts[i]) + "] = " + value + ";";
                                }
                                else
                                {
                                    // Address range init
                                    stmt = "for (int __i = " + std::to_string(localStarts[i]) + "; __i < " +
                                           std::to_string(localStarts[i] + localLens[i]) +
                                           "; __i = __i + 1) " + opContext + "[__i] = " + value + ";";
                                }
                            }
                            else
                            {
                                // Unknown kind, skip
                                continue;
                            }
                            
                            std::ostringstream oss;
                            appendIndented(oss, 2, stmt);
                            addSimpleStmt("initial", oss.str(), opId);
                        }
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMemoryReadPort:
                {
                    if (operands.size() < 1 || results.empty())
                    {
                        reportError("kMemoryReadPort missing operands or results", opContext);
                        break;
                    }
                    auto memSymbolAttr = resolveMemorySymbol(op);
                    if (!memSymbolAttr)
                    {
                        reportError("kMemoryReadPort missing memSymbol", opContext);
                        break;
                    }
                    std::string addrExpr;
                    if (dpiDrivenStateOps.find(opId) != dpiDrivenStateOps.end())
                    {
                        withDpiInlineSink(opId, [&]() {
                            addrExpr = valueExprSeq(operands[0]);
                        });
                    }
                    else
                    {
                        addrExpr = valueExprSeq(operands[0]);
                    }
                    addAssign("assign " + valueName(results[0]) + " = " + *memSymbolAttr + "[" +
                                  addrExpr + "];",
                              opId);
                    ensureWireDecl(results[0]);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMemoryWritePort:
                {
                    if (operands.size() < 5)
                    {
                        reportError("kMemoryWritePort missing operands", opContext);
                        break;
                    }
                    const wolvrix::lib::grh::ValueId updateCond = operands[0];
                    const wolvrix::lib::grh::ValueId addr = operands[1];
                    const wolvrix::lib::grh::ValueId data = operands[2];
                    const wolvrix::lib::grh::ValueId mask = operands[3];
                    if (!updateCond.valid() || !addr.valid() || !data.valid() || !mask.valid())
                    {
                        reportError("kMemoryWritePort missing operands", opContext);
                        break;
                    }
                    if (graph->valueWidth(updateCond) != 1)
                    {
                        reportError("kMemoryWritePort updateCond must be 1 bit", opContext);
                        break;
                    }
                    auto memSymbolAttr = resolveMemorySymbol(op);
                    if (!memSymbolAttr)
                    {
                        reportError("kMemoryWritePort missing memSymbol", opContext);
                        break;
                    }
                    auto seqKey = buildEventKey(op, 4);
                    if (!seqKey)
                    {
                        break;
                    }

                    const std::string &memSymbol = *memSymbolAttr;
                    const wolvrix::lib::grh::OperationId memOpId = graph->findOperation(memSymbol);
                    int64_t memWidth = 1;
                    if (memOpId.valid())
                    {
                        const wolvrix::lib::grh::Operation memOp = graph->getOperation(memOpId);
                        memWidth = getAttribute<int64_t>(*graph, memOp, "width").value_or(1);
                    }

                    std::string updateExpr;
                    std::string addrExpr;
                    std::string dataExpr;
                    std::string maskExpr;
                    std::string dataExprFull;
                    auto extendSeqOperand = [&](wolvrix::lib::grh::ValueId valueId, int64_t targetWidth) -> std::string
                    {
                        ExprTools seqExpr = baseExpr;
                        seqExpr.valueExpr = valueExprSeq;
                        return seqExpr.extendOperand(valueId, targetWidth);
                    };
                    if (dpiDrivenStateOps.find(opId) != dpiDrivenStateOps.end())
                    {
                        withDpiInlineSink(opId, [&]() {
                            updateExpr = valueExprSeq(updateCond);
                            addrExpr = valueExprSeq(addr);
                            dataExpr = valueExprSeq(data);
                            maskExpr = valueExprSeq(mask);
                            dataExprFull = extendSeqOperand(data, memWidth);
                        });
                    }
                    else
                    {
                        updateExpr = valueExprSeq(updateCond);
                        addrExpr = valueExprSeq(addr);
                        dataExpr = valueExprSeq(data);
                        maskExpr = valueExprSeq(mask);
                        dataExprFull = extendSeqOperand(data, memWidth);
                    }
                    if (auto inlineUpdate = inlineConstExprFor(updateCond))
                    {
                        updateExpr = *inlineUpdate;
                    }
                    if (dataExprFull.empty())
                    {
                        dataExprFull = extendSeqOperand(data, memWidth);
                    }

                    const bool guardUpdate = !isConstOne(updateCond);
                    const int baseIndent = guardUpdate ? 3 : 2;
                    std::optional<std::vector<uint8_t>> maskBits;
                    if (auto maskLiteral = constLiteralFor(mask))
                    {
                        maskBits = parseConstMaskBits(*maskLiteral, memWidth);
                    }
                    auto maskAll = [&](uint8_t value) -> bool
                    {
                        if (!maskBits)
                        {
                            return false;
                        }
                        for (uint8_t bit : *maskBits)
                        {
                            if (bit != value)
                            {
                                return false;
                            }
                        }
                        return true;
                    };
                    const int64_t dataWidth = graph->valueWidth(data);
                    const bool maskAllOnes = maskBits ? maskAll(1) : false;
                    if (dataWidth > 0 && memWidth > 0 && dataWidth != memWidth && !maskAllOnes)
                    {
                        reportWarning("kMemoryWritePort data width does not match memory width", opContext);
                    }
                    std::ostringstream stmt;
                    if (guardUpdate)
                    {
                        appendIndented(stmt, 2, "if (" + updateExpr + ") begin");
                    }
                    if (maskBits)
                    {
                        const bool allZero = maskAll(0);
                        const bool allOnes = maskAll(1);
                        if (allZero)
                        {
                            break;
                        }
                        if (allOnes || memWidth <= 1)
                        {
                            appendIndented(stmt, baseIndent, memSymbol + "[" + addrExpr + "] <= " + dataExprFull + ";");
                        }
                        else
                        {
                            for (int64_t bit = 0; bit < memWidth; ++bit)
                            {
                                if ((*maskBits)[static_cast<std::size_t>(bit)] != 0)
                                {
                                    appendIndented(stmt, baseIndent,
                                                   memSymbol + "[" + addrExpr + "][" + std::to_string(bit) + "] <= " +
                                                       dataExpr + "[" + std::to_string(bit) + "];");
                                }
                            }
                        }
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                        addSequentialStmt(*seqKey, stmt.str(), opId);
                        break;
                    }
                    if (memWidth <= 1)
                    {
                        appendIndented(stmt, baseIndent, "if (" + maskExpr + ") begin");
                        appendIndented(stmt, baseIndent + 1, memSymbol + "[" + addrExpr + "] <= " + dataExprFull + ";");
                        appendIndented(stmt, baseIndent, "end");
                        if (guardUpdate)
                        {
                            appendIndented(stmt, 2, "end");
                        }
                        addSequentialStmt(*seqKey, stmt.str(), opId);
                        break;
                    }
                    appendIndented(stmt, baseIndent, "if (" + maskExpr + " == {" + std::to_string(memWidth) + "{1'b1}}) begin");
                    appendIndented(stmt, baseIndent + 1, memSymbol + "[" + addrExpr + "] <= " + dataExprFull + ";");
                    appendIndented(stmt, baseIndent, "end else begin");
                    appendIndented(stmt, baseIndent + 1, "integer i;");
                    appendIndented(stmt, baseIndent + 1, "for (i = 0; i < " + std::to_string(memWidth) + "; i = i + 1) begin");
                    appendIndented(stmt, baseIndent + 2, "if (" + maskExpr + "[i]) begin");
                    appendIndented(stmt, baseIndent + 3, memSymbol + "[" + addrExpr + "][i] <= " + dataExpr + "[i];");
                    appendIndented(stmt, baseIndent + 2, "end");
                    appendIndented(stmt, baseIndent + 1, "end");
                    appendIndented(stmt, baseIndent, "end");
                    if (guardUpdate)
                    {
                        appendIndented(stmt, 2, "end");
                    }
                    addSequentialStmt(*seqKey, stmt.str(), opId);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kMemoryFillPort:
                {
                    if (operands.size() < 3)
                    {
                        reportError("kMemoryFillPort missing operands", opContext);
                        break;
                    }
                    const wolvrix::lib::grh::ValueId updateCond = operands[0];
                    const wolvrix::lib::grh::ValueId data = operands[1];
                    if (!updateCond.valid() || !data.valid())
                    {
                        reportError("kMemoryFillPort missing operands", opContext);
                        break;
                    }
                    if (graph->valueWidth(updateCond) != 1)
                    {
                        reportError("kMemoryFillPort updateCond must be 1 bit", opContext);
                        break;
                    }
                    auto memSymbolAttr = resolveMemorySymbol(op);
                    if (!memSymbolAttr)
                    {
                        reportError("kMemoryFillPort missing memSymbol", opContext);
                        break;
                    }
                    auto seqKey = buildEventKey(op, 2);
                    if (!seqKey)
                    {
                        break;
                    }

                    const std::string &memSymbol = *memSymbolAttr;
                    const wolvrix::lib::grh::OperationId memOpId = graph->findOperation(memSymbol);
                    int64_t memWidth = 1;
                    int64_t rowCount = 0;
                    if (memOpId.valid())
                    {
                        const wolvrix::lib::grh::Operation memOp = graph->getOperation(memOpId);
                        memWidth = getAttribute<int64_t>(*graph, memOp, "width").value_or(1);
                        rowCount = getAttribute<int64_t>(*graph, memOp, "row").value_or(0);
                    }
                    if (rowCount <= 0)
                    {
                        reportError("kMemoryFillPort target memory missing row attr", opContext);
                        break;
                    }

                    std::string updateExpr;
                    std::string dataExprFull;
                    auto extendSeqOperand = [&](wolvrix::lib::grh::ValueId valueId, int64_t targetWidth) -> std::string
                    {
                        ExprTools seqExpr = baseExpr;
                        seqExpr.valueExpr = valueExprSeq;
                        return seqExpr.extendOperand(valueId, targetWidth);
                    };
                    if (dpiDrivenStateOps.find(opId) != dpiDrivenStateOps.end())
                    {
                        withDpiInlineSink(opId, [&]() {
                            updateExpr = valueExprSeq(updateCond);
                            dataExprFull = extendSeqOperand(data, memWidth);
                        });
                    }
                    else
                    {
                        updateExpr = valueExprSeq(updateCond);
                        dataExprFull = extendSeqOperand(data, memWidth);
                    }
                    if (auto inlineUpdate = inlineConstExprFor(updateCond))
                    {
                        updateExpr = *inlineUpdate;
                    }
                    if (dataExprFull.empty())
                    {
                        dataExprFull = extendSeqOperand(data, memWidth);
                    }
                    if (graph->valueWidth(data) > 0 && memWidth > 0 && graph->valueWidth(data) != memWidth)
                    {
                        reportWarning("kMemoryFillPort data width does not match memory width", opContext);
                    }

                    std::ostringstream stmt;
                    if (!isConstOne(updateCond))
                    {
                        appendIndented(stmt, 2, "if (" + updateExpr + ") begin");
                        appendIndented(stmt, 3, "integer __fill_i;");
                        appendIndented(stmt, 3, "for (__fill_i = 0; __fill_i < " + std::to_string(rowCount) +
                                                   "; __fill_i = __fill_i + 1) begin");
                        appendIndented(stmt, 4, memSymbol + "[__fill_i] <= " + dataExprFull + ";");
                        appendIndented(stmt, 3, "end");
                        appendIndented(stmt, 2, "end");
                    }
                    else
                    {
                        appendIndented(stmt, 2, "integer __fill_i;");
                        appendIndented(stmt, 2, "for (__fill_i = 0; __fill_i < " + std::to_string(rowCount) +
                                                   "; __fill_i = __fill_i + 1) begin");
                        appendIndented(stmt, 3, memSymbol + "[__fill_i] <= " + dataExprFull + ";");
                        appendIndented(stmt, 2, "end");
                    }
                    addSequentialStmt(*seqKey, stmt.str(), opId);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kInstance:
                case wolvrix::lib::grh::OperationKind::kBlackbox:
                {
                    auto moduleName = getAttribute<std::string>(*graph, op, "moduleName");
                    auto inputNames = getAttribute<std::vector<std::string>>(*graph, op, "inputPortName");
                    auto outputNames = getAttribute<std::vector<std::string>>(*graph, op, "outputPortName");
                    auto inoutNames = getAttribute<std::vector<std::string>>(*graph, op, "inoutPortName");
                    auto instanceNameBase = getAttribute<std::string>(*graph, op, "instanceName").value_or(opContext);
                    std::string instanceName = instanceNameBase;
                    int instSuffix = 1;
                    while (!instanceNamesUsed.insert(instanceName).second)
                    {
                        instanceName = instanceNameBase + "_" + std::to_string(instSuffix++);
                    }
                    if (!moduleName || !inputNames || !outputNames)
                    {
                        reportError("Instance missing module or port names", opContext);
                        break;
                    }
                    const std::size_t inoutCount = inoutNames ? inoutNames->size() : 0;
                    if (operands.size() < inputNames->size() + inoutCount ||
                        results.size() < outputNames->size() + inoutCount * 2)
                    {
                        reportError("Instance port counts do not match operands/results", opContext);
                        break;
                    }
                    const auto moduleNameIt = emittedModuleNames.find(*moduleName);
                    const std::string &targetModuleName =
                        moduleNameIt != emittedModuleNames.end() ? moduleNameIt->second : *moduleName;

                    std::ostringstream decl;
                    if (op.kind() == wolvrix::lib::grh::OperationKind::kBlackbox)
                    {
                        auto paramNames = getAttribute<std::vector<std::string>>(*graph, op, "parameterNames");
                        auto paramValues = getAttribute<std::vector<std::string>>(*graph, op, "parameterValues");
                        if (paramNames && paramValues && !paramNames->empty() && paramNames->size() == paramValues->size())
                        {
                            decl << targetModuleName << " #(" << '\n';
                            for (std::size_t i = 0; i < paramNames->size(); ++i)
                            {
                                decl << "    ." << (*paramNames)[i] << "(" << (*paramValues)[i] << ")";
                                decl << (i + 1 == paramNames->size() ? "\n" : ",\n");
                            }
                            decl << ") ";
                        }
                        else
                        {
                            decl << targetModuleName << " ";
                        }
                    }
                    else
                    {
                        decl << targetModuleName << " ";
                    }
                    std::vector<std::pair<std::string, std::string>> connections;
                    connections.reserve(inputNames->size() + outputNames->size() + inoutCount);
                    for (std::size_t i = 0; i < inputNames->size(); ++i)
                    {
                        if (i < operands.size())
                        {
                            connections.emplace_back((*inputNames)[i], valueExpr(operands[i]));
                        }
                    }
                    for (std::size_t i = 0; i < outputNames->size(); ++i)
                    {
                        if (i < results.size())
                        {
                            connections.emplace_back((*outputNames)[i], valueName(results[i]));
                        }
                    }
                    auto findDirectInoutPort = [&](wolvrix::lib::grh::ValueId inValue,
                                                   wolvrix::lib::grh::ValueId outValue,
                                                   wolvrix::lib::grh::ValueId oeValue) -> std::optional<std::string>
                    {
                        for (const auto &port : graph->inoutPorts())
                        {
                            if (port.name.empty())
                            {
                                continue;
                            }
                            if (port.in == inValue && port.out == outValue && port.oe == oeValue)
                            {
                                return port.name;
                            }
                        }
                        return std::nullopt;
                    };
                    if (inoutNames)
                    {
                        auto ensureNamedWireDecl = [&](const std::string &base, int64_t width,
                                                       bool isSigned) -> std::string
                        {
                            const std::string root = base.empty() ? std::string("_inout") : base;
                            std::string candidate = root;
                            std::size_t suffix = 0;
                            while (declaredNames.find(candidate) != declaredNames.end())
                            {
                                candidate = root;
                                candidate.push_back('_');
                                candidate.append(std::to_string(++suffix));
                            }
                            wireDecls.emplace(candidate, NetDecl{width, isSigned, op.srcLoc()});
                            declaredNames.insert(candidate);
                            return candidate;
                        };
                        bool inoutOk = true;
                        for (std::size_t i = 0; i < inoutNames->size(); ++i)
                        {
                            const std::size_t inIndex = inputNames->size() + i;
                            const std::size_t outIndex = outputNames->size() + i;
                            const std::size_t oeIndex = outputNames->size() + inoutNames->size() + i;
                            if (outIndex >= results.size() || oeIndex >= results.size() ||
                                inIndex >= operands.size())
                            {
                                reportError("Instance inout index out of range", opContext);
                                inoutOk = false;
                                break;
                            }
                            if (auto directPort = findDirectInoutPort(operands[inIndex], results[outIndex], results[oeIndex]))
                            {
                                connections.emplace_back((*inoutNames)[i], *directPort);
                                continue;
                            }
                            const std::string wireBase =
                                instanceName.empty()
                                    ? (*inoutNames)[i] + "_inout"
                                    : instanceName + "_" + (*inoutNames)[i] + "_inout";
                            const int64_t width = graph->valueWidth(results[outIndex]);
                            const bool isSigned = graph->valueSigned(results[outIndex]);
                            const std::string wireName = ensureNamedWireDecl(wireBase, width, isSigned);
                            connections.emplace_back((*inoutNames)[i], wireName);
                            ensureWireDecl(results[outIndex]);
                            ensureWireDecl(results[oeIndex]);
                            ensureWireDecl(operands[inIndex]);
                            emitInoutAssign(wireName, results[oeIndex], results[outIndex], width, opId);
                            portBindingStmts.emplace_back(
                                "assign " + valueName(operands[inIndex]) + " = " + wireName + ";",
                                opId);
                        }
                        if (!inoutOk)
                        {
                            break;
                        }
                    }

                    decl << instanceName << " (\n";
                    for (std::size_t i = 0; i < connections.size(); ++i)
                    {
                        const auto &conn = connections[i];
                        decl << "    ." << conn.first << "(" << conn.second << ")";
                        decl << (i + 1 == connections.size() ? "\n" : ",\n");
                    }
                    decl << "  );";
                    instanceDecls.emplace_back(decl.str(), opId);

                    for (const auto res : results)
                    {
                        ensureWireDecl(res);
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kSystemTask:
                {
                    if (operands.empty())
                    {
                        reportError("kSystemTask missing operands", opContext);
                        break;
                    }
                    auto taskName = getAttribute<std::string>(*graph, op, "name");
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, op, "eventEdge");
                    auto procKind = getAttribute<std::string>(*graph, op, "procKind");
                    if (!taskName || taskName->empty())
                    {
                        reportError("kSystemTask missing name", opContext);
                        break;
                    }
                    const std::size_t eventCount = eventEdges ? eventEdges->size() : 0;
                    if (operands.size() < 1 + eventCount)
                    {
                        reportError("kSystemTask operand count does not match eventEdge", opContext);
                        break;
                    }
                    const std::size_t argCount = operands.size() - 1 - eventCount;
                    const std::size_t eventStart = 1 + argCount;
                    std::optional<SeqKey> seqKey;
                    if (eventCount > 0)
                    {
                        seqKey = buildEventKey(op, eventStart);
                        if (!seqKey)
                        {
                            break;
                        }
                    }

                    // Helper to inline string literals for system tasks
                    auto getSystemTaskArgExpr = [&](wolvrix::lib::grh::ValueId argId) -> std::string
                    {
                        // For string constants used in system tasks, inline the literal
                        // to avoid initialization order issues with initial blocks
                        if (valueType(argId) == wolvrix::lib::grh::ValueType::String)
                        {
                            if (auto literal = constLiteralFor(argId))
                            {
                                return *literal;
                            }
                        }
                        return valueExpr(argId);
                    };

                    const bool guardTask = !isConstOne(operands[0]);
                    const int baseIndent = guardTask ? 3 : 2;
                    std::ostringstream stmt;
                    if (guardTask)
                    {
                        appendIndented(stmt, 2, "if (" + valueExpr(operands[0]) + ") begin");
                    }
                    stmt << std::string(kIndentSizeSv * baseIndent, ' ') << "$" << *taskName;
                    const bool anyArgs = argCount > 0;
                    if (anyArgs)
                    {
                        bool firstArg = true;
                        auto appendArg = [&](const std::string &text)
                        {
                            if (!firstArg)
                            {
                                stmt << ", ";
                            }
                            stmt << text;
                            firstArg = false;
                        };
                        stmt << "(";
                        for (std::size_t i = 0; i < argCount; ++i)
                        {
                            appendArg(getSystemTaskArgExpr(operands[1 + i]));
                        }
                        stmt << ")";
                    }
                    stmt << ";\n";
                    if (guardTask)
                    {
                        appendIndented(stmt, 2, "end");
                    }

                    if (seqKey)
                    {
                        addSequentialStmt(*seqKey, stmt.str(), opId);
                    }
                    else
                    {
                        auto headerForKind = [&](const std::optional<std::string> &kind) -> std::string
                        {
                            if (!kind)
                            {
                                return "always @*";
                            }
                            if (*kind == "initial")
                            {
                                return "initial";
                            }
                            if (*kind == "final")
                            {
                                return "final";
                            }
                            if (*kind == "always_comb")
                            {
                                return "always_comb";
                            }
                            if (*kind == "always_latch")
                            {
                                return "always_latch";
                            }
                            return "always @*";
                        };
                        addSimpleStmt(headerForKind(procKind), stmt.str(), opId);
                    }
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kDpicImport:
                {
                    auto argsDir = getAttribute<std::vector<std::string>>(*graph, op, "argsDirection");
                    auto argsWidth = getAttribute<std::vector<int64_t>>(*graph, op, "argsWidth");
                    auto argsName = getAttribute<std::vector<std::string>>(*graph, op, "argsName");
                    auto argsSigned = getAttribute<std::vector<bool>>(*graph, op, "argsSigned");
                    auto argsType = getAttribute<std::vector<std::string>>(*graph, op, "argsType");
                    auto hasReturn = getAttribute<bool>(*graph, op, "hasReturn").value_or(false);
                    auto returnWidth = getAttribute<int64_t>(*graph, op, "returnWidth").value_or(0);
                    auto returnSigned = getAttribute<bool>(*graph, op, "returnSigned").value_or(false);
                    auto returnType = getAttribute<std::string>(*graph, op, "returnType");
                    if (!argsDir || !argsWidth || !argsName ||
                        argsDir->size() != argsWidth->size() || argsDir->size() != argsName->size())
                    {
                        reportError("kDpicImport missing or inconsistent arg metadata", opContext);
                        break;
                    }
                    if (argsSigned && argsSigned->size() != argsDir->size())
                    {
                        reportError("kDpicImport argsSigned size mismatch", opContext);
                        break;
                    }
                    if (argsType && argsType->size() != argsDir->size())
                    {
                        reportError("kDpicImport argsType size mismatch", opContext);
                        break;
                    }
                    std::ostringstream decl;
                    const auto isPackedVector = [](const std::string& typeName) {
                        return typeName == "logic" || typeName == "bit";
                    };
                    decl << "import \"DPI-C\" function ";
                    if (hasReturn)
                    {
                        std::string typeName = "logic";
                        if (returnType && !returnType->empty())
                        {
                            typeName = *returnType;
                        }
                        if (isPackedVector(typeName))
                        {
                            const int64_t width = returnWidth > 0 ? returnWidth : 1;
                            decl << typeName << " ";
                            if (returnSigned)
                            {
                                decl << "signed ";
                            }
                            if (width > 1)
                            {
                                decl << "[" << (width - 1) << ":0] ";
                            }
                        }
                        else
                        {
                            decl << typeName << " ";
                        }
                    }
                    else
                    {
                        decl << "void ";
                    }
                    decl << opContext << " (\n";
                    for (std::size_t i = 0; i < argsDir->size(); ++i)
                    {
                        std::string typeName = "logic";
                        if (argsType && i < argsType->size())
                        {
                            typeName = (*argsType)[i];
                        }
                        decl << "  " << (*argsDir)[i] << " ";
                        if (isPackedVector(typeName))
                        {
                            decl << typeName << " ";
                            if (argsSigned && (*argsSigned)[i])
                            {
                                decl << "signed ";
                            }
                            if ((*argsWidth)[i] > 1)
                            {
                                decl << "[" << ((*argsWidth)[i] - 1) << ":0] ";
                            }
                        }
                        else
                        {
                            decl << typeName << " ";
                        }
                        decl << (*argsName)[i];
                        decl << (i + 1 == argsDir->size() ? "\n" : ",\n");
                    }
                    decl << ");";
                    dpiImportDecls.emplace_back(decl.str(), opId);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kDpicCall:
                {
                    if (operands.empty())
                    {
                        reportError("kDpicCall missing operands", opContext);
                        break;
                    }
                    auto eventEdges = getAttribute<std::vector<std::string>>(*graph, op, "eventEdge");
                    auto targetImport = getAttribute<std::string>(*graph, op, "targetImportSymbol");
                    auto inArgName = getAttribute<std::vector<std::string>>(*graph, op, "inArgName");
                    auto outArgName = getAttribute<std::vector<std::string>>(*graph, op, "outArgName");
                    auto inoutArgName = getAttribute<std::vector<std::string>>(*graph, op, "inoutArgName");
                    auto hasReturn = getAttribute<bool>(*graph, op, "hasReturn").value_or(false);
                    if (!eventEdges || !targetImport || !inArgName || !outArgName)
                    {
                        reportError("kDpicCall missing metadata", opContext);
                        break;
                    }
                    if (eventEdges->empty())
                    {
                        reportError("kDpicCall missing eventEdge entries", opContext);
                        break;
                    }
                    if (operands.size() < 1 + eventEdges->size())
                    {
                        reportError("kDpicCall operand count does not match eventEdge", opContext);
                        break;
                    }
                    const std::size_t eventStart = operands.size() - eventEdges->size();
                    if (eventStart < 1)
                    {
                        reportError("kDpicCall missing updateCond operand", opContext);
                        break;
                    }
                    const std::size_t argCount = eventStart - 1;
                    const std::size_t inoutCount = inoutArgName ? inoutArgName->size() : 0;
                    if (argCount != inArgName->size() + inoutCount)
                    {
                        reportError("kDpicCall operand count does not match args", opContext);
                        break;
                    }
                    const std::size_t outputOffset = hasReturn ? 1 : 0;
                    if (results.size() < outputOffset + outArgName->size() + inoutCount)
                    {
                        reportError("kDpicCall result count does not match args", opContext);
                        break;
                    }
                    auto seqKey = buildEventKey(op, eventStart);
                    if (!seqKey)
                    {
                        break;
                    }
                    auto itImport = dpicImports.find(*targetImport);
                    if (itImport == dpicImports.end() || itImport->second.graph == nullptr)
                    {
                        reportError("kDpicCall cannot resolve import symbol", *targetImport);
                        break;
                    }
                    const DpiImportRef &importRef = itImport->second;
                    const wolvrix::lib::grh::Operation importOp = importRef.graph->getOperation(importRef.op);
                    auto importArgs = getAttribute<std::vector<std::string>>(*importRef.graph, importOp, "argsName");
                    auto importDirs = getAttribute<std::vector<std::string>>(*importRef.graph, importOp, "argsDirection");
                    if (!importArgs || !importDirs || importArgs->size() != importDirs->size())
                    {
                        reportError("kDpicCall found malformed import signature", *targetImport);
                        break;
                    }

                    // Declare intermediate regs for outputs and connect them back.
                    for (const auto res : results)
                    {
                        const wolvrix::lib::grh::SymbolId sym = graph->valueSymbol(res);
                        const std::string tempName = std::string(graph->symbolText(sym)) + "_intm";
                        ensureRegDecl(tempName,
                                      graph->valueWidth(res),
                                      graph->valueSigned(res),
                                      graph->valueType(res),
                                      op.srcLoc());
                        addValueAssign(res, tempName, opId);
                    }

                    bool callOk = true;
                    const bool guardCall = !isConstOne(operands[0]);
                    const int baseIndent = guardCall ? 3 : 2;
                    std::ostringstream stmt;
                    if (guardCall)
                    {
                        appendIndented(stmt, 2, "if (" + valueExpr(operands[0]) + ") begin");
                    }
                    if (inoutArgName && !inoutArgName->empty())
                    {
                        for (std::size_t i = 0; i < inoutArgName->size(); ++i)
                        {
                            const std::size_t operandIndex = 1 + inArgName->size() + i;
                            const std::size_t resultIndex = outputOffset + outArgName->size() + i;
                            if (operandIndex >= operands.size() || resultIndex >= results.size())
                            {
                                reportError("kDpicCall inout arg index out of range", opContext);
                                callOk = false;
                                break;
                            }
                            const std::string tempName = valueName(results[resultIndex]) + "_intm";
                            appendIndented(stmt, baseIndent, tempName + " = " + valueExpr(operands[operandIndex]) + ";");
                        }
                        if (!callOk)
                        {
                            break;
                        }
                    }
                    stmt << std::string(kIndentSizeSv * baseIndent, ' ');
                    if (hasReturn && !results.empty())
                    {
                        stmt << valueName(results[0]) << "_intm = ";
                    }
                    stmt << *targetImport << "(";
                    bool firstArg = true;
                    for (std::size_t i = 0; i < importArgs->size(); ++i)
                    {
                        if (!firstArg)
                        {
                            stmt << ", ";
                        }
                        firstArg = false;
                        const std::string &formal = (*importArgs)[i];
                        const std::string &dir = (*importDirs)[i];
                        if (dir == "input")
                        {
                            int idx = findNameIndex(*inArgName, formal);
                            if (idx < 0 || static_cast<std::size_t>(idx + 1) >= operands.size())
                            {
                                reportError("kDpicCall missing matching input arg " + formal, opContext);
                                callOk = false;
                                break;
                            }
                            stmt << valueExpr(operands[static_cast<std::size_t>(idx + 1)]);
                        }
                        else if (dir == "inout")
                        {
                            if (!inoutArgName)
                            {
                                reportError("kDpicCall missing inoutArgName for " + formal, opContext);
                                callOk = false;
                                break;
                            }
                            int idx = findNameIndex(*inoutArgName, formal);
                            const std::size_t resultIndex =
                                outputOffset + outArgName->size() + static_cast<std::size_t>(idx);
                            if (idx < 0 || resultIndex >= results.size())
                            {
                                reportError("kDpicCall missing matching inout arg " + formal, opContext);
                                callOk = false;
                                break;
                            }
                            stmt << valueName(results[resultIndex]) << "_intm";
                        }
                        else
                        {
                            int idx = findNameIndex(*outArgName, formal);
                            const std::size_t resultIndex =
                                outputOffset + static_cast<std::size_t>(idx);
                            if (idx < 0 || resultIndex >= results.size())
                            {
                                reportError("kDpicCall missing matching output arg " + formal, opContext);
                                callOk = false;
                                break;
                            }
                            stmt << valueName(results[resultIndex]) << "_intm";
                        }
                    }
                    if (!callOk)
                    {
                        break;
                    }
                    stmt << ");\n";
                    if (guardCall)
                    {
                        appendIndented(stmt, 2, "end");
                    }
                    addSequentialStmt(*seqKey, stmt.str(), opId);
                    break;
                }
                case wolvrix::lib::grh::OperationKind::kXMRRead:
                case wolvrix::lib::grh::OperationKind::kXMRWrite:
                    reportError("Unresolved XMR operation in emit", opContext);
                    break;
                default:
                    reportWarning("Unsupported operation for SystemVerilog emission", opContext);
                    break;
                }
            }

            // Declare remaining wires for non-port values not defined above.
            for (const auto valueId : graph->values())
            {
                if (graph->valueIsInput(valueId) || graph->valueIsOutput(valueId) || graph->valueIsInout(valueId))
                {
                    continue;
                }
                if (elidedReadPortValues.find(valueId) != elidedReadPortValues.end())
                {
                    continue;
                }
                const wolvrix::lib::grh::OperationId defOpId = graph->valueDef(valueId);
                if (defOpId.valid())
                {
                    if (graph->opKind(defOpId) == wolvrix::lib::grh::OperationKind::kConstant)
                    {
                        continue;
                    }
                }
                ensureWireDecl(valueId);
            }

            std::vector<std::string> traceAliasDecls;
            if (options.traceUnderscoreValues)
            {
                std::unordered_set<std::string> aliasNames = declaredNames;
                auto makeUniqueAlias = [&](const std::string &base) -> std::string
                {
                    std::string name = base;
                    int suffix = 0;
                    while (aliasNames.find(name) != aliasNames.end())
                    {
                        ++suffix;
                        name = base + "_" + std::to_string(suffix);
                    }
                    aliasNames.insert(name);
                    return name;
                };

                for (const auto valueId : graph->values())
                {
                    if (!valueId.valid() || valueId.graph != graph->id())
                    {
                        continue;
                    }
                    if (graph->valueIsInput(valueId) || graph->valueIsOutput(valueId) || graph->valueIsInout(valueId))
                    {
                        continue;
                    }
                    if (graph->valueType(valueId) != wolvrix::lib::grh::ValueType::Logic ||
                        graph->valueWidth(valueId) <= 0)
                    {
                        continue;
                    }
                    const std::string &orig = valueName(valueId);
                    if (orig.empty() || orig.front() != '_')
                    {
                        continue;
                    }
                    std::string alias = makeUniqueAlias("wd_" + orig);
                    std::string decl = "(* keep *) wire ";
                    decl.append(signedPrefix(graph->valueSigned(valueId)));
                    const std::string range = widthRange(graph->valueWidth(valueId));
                    if (!range.empty())
                    {
                        decl.append(range);
                        decl.push_back(' ');
                    }
                    decl.append(alias);
                    decl.append(" = ");
                    decl.append(orig);
                    decl.append(";");
                    traceAliasDecls.emplace_back(std::move(decl));
                }
            }

            // -------------------------
            // Module emission
            // -------------------------
            out << "module " << moduleName << " (\n";
            {
                bool first = true;
                auto emitPortLine = [&](const std::string &name)
                {
                    auto it = portDecls.find(name);
                    if (it == portDecls.end())
                    {
                        return;
                    }
                    const PortDecl &decl = it->second;
                    const std::string attr = formatSrcAttribute(decl.debug);
                    if (!first)
                    {
                        out << ",\n";
                    }
                    first = false;
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    if (decl.valueType != wolvrix::lib::grh::ValueType::Logic)
                    {
                        out << "  ";
                        if (decl.dir == PortDir::Input)
                        {
                            out << "input ";
                        }
                        else if (decl.dir == PortDir::Output)
                        {
                            out << "output ";
                        }
                        else
                        {
                            out << "inout ";
                        }
                        out << (decl.valueType == wolvrix::lib::grh::ValueType::Real ? "real " : "string ");
                        out << name;
                        return;
                    }
                    if (decl.dir == PortDir::Input)
                    {
                        out << "  input wire ";
                    }
                    else if (decl.dir == PortDir::Output)
                    {
                        out << "  " << (decl.isReg ? "output reg " : "output wire ");
                    }
                    else
                    {
                        out << "  inout wire ";
                    }
                    out << signedPrefix(decl.isSigned);
                    const std::string range = widthRange(decl.width);
                    if (!range.empty())
                    {
                        out << range << " ";
                    }
                    out << name;
                };

                for (const auto &port : graph->inputPorts())
                {
                    if (port.name.empty())
                    {
                        continue;
                    }
                    emitPortLine(port.name);
                }
                for (const auto &port : graph->outputPorts())
                {
                    if (port.name.empty())
                    {
                        continue;
                    }
                    emitPortLine(port.name);
                }
                for (const auto &port : graph->inoutPorts())
                {
                    if (port.name.empty())
                    {
                        continue;
                    }
                    emitPortLine(port.name);
                }
                out << "\n);\n";
            }

            if (!wireDecls.empty())
            {
                out << '\n';
                for (const auto &[name, decl] : wireDecls)
                {
                    out << "  " << formatNetDecl("wire", name, decl) << '\n';
                }
            }

            if (!regDecls.empty())
            {
                out << '\n';
                for (const auto &[name, decl] : regDecls)
                {
                    out << "  " << formatNetDecl("reg", name, decl) << '\n';
                }
            }

            if (!varDecls.empty())
            {
                out << '\n';
                for (const auto &[name, decl] : varDecls)
                {
                    out << "  " << formatVarDecl(decl.type, name, decl.initValue) << '\n';
                }
            }

            if (!traceAliasDecls.empty())
            {
                out << '\n';
                out << "  // Trace aliases for underscore-prefixed internals\n";
                for (const auto &decl : traceAliasDecls)
                {
                    out << "  " << decl << '\n';
                }
            }

            if (!memoryDecls.empty())
            {
                out << '\n';
                auto opSrcLoc = [&](wolvrix::lib::grh::OperationId opId) -> std::optional<wolvrix::lib::grh::SrcLoc>
                {
                    if (!opId.valid())
                    {
                        return std::nullopt;
                    }
                    return graph->getOperation(opId).srcLoc();
                };
                for (const auto &[decl, opPtr] : memoryDecls)
                {
                    const std::string attr =
                        formatSrcAttribute(opSrcLoc(opPtr));
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  " << decl << '\n';
                }
            }

            if (!instanceDecls.empty())
            {
                out << '\n';
                for (const auto &[inst, opPtr] : instanceDecls)
                {
                    const std::string attr =
                        formatSrcAttribute(opPtr.valid() ? graph->getOperation(opPtr).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  " << inst << '\n';
                }
            }

            if (!dpiImportDecls.empty())
            {
                out << '\n';
                for (const auto &[decl, opPtr] : dpiImportDecls)
                {
                    const std::string attr =
                        formatSrcAttribute(opPtr.valid() ? graph->getOperation(opPtr).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  " << decl << '\n';
                }
            }

            if (!portBindingStmts.empty())
            {
                out << '\n';
                for (const auto &[stmt, opPtr] : portBindingStmts)
                {
                    const std::string attr =
                        formatSrcAttribute(opPtr.valid() ? graph->getOperation(opPtr).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  " << stmt << '\n';
                }
            }

            if (!assignStmts.empty())
            {
                out << '\n';
                for (const auto &[stmt, opPtr] : assignStmts)
                {
                    const std::string attr =
                        formatSrcAttribute(opPtr.valid() ? graph->getOperation(opPtr).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  " << stmt << '\n';
                }
            }

            if (!latchBlocks.empty())
            {
                out << '\n';
                for (const auto &[block, opPtr] : latchBlocks)
                {
                    const std::string attr =
                        formatSrcAttribute(opPtr.valid() ? graph->getOperation(opPtr).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << '\n';
                    }
                    out << block << '\n';
                }
            }

            if (!simpleBlocks.empty())
            {
                out << '\n';
                for (const auto &block : simpleBlocks)
                {
                    const std::string attr =
                        formatSrcAttribute(block.op.valid() ? graph->getOperation(block.op).srcLoc()
                                                            : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << '\n';
                    }
                    out << "  " << block.header << " begin\n";
                    for (const auto &stmt : block.stmts)
                    {
                        out << stmt;
                        if (!stmt.empty() && stmt.back() != '\n')
                        {
                            out << '\n';
                        }
                    }
                    out << "  end\n";
                }
            }

            if (!seqBlocks.empty())
            {
                out << '\n';
                for (const auto &seq : seqBlocks)
                {
                    const std::string sens = sensitivityList(*graph, seq.key);
                    if (sens.empty())
                    {
                        reportError("Sequential block missing sensitivity list", graph->symbol());
                        continue;
                    }
                    const std::string attr =
                        formatSrcAttribute(seq.op.valid() ? graph->getOperation(seq.op).srcLoc() : std::optional<wolvrix::lib::grh::SrcLoc>{});
                    if (!attr.empty())
                    {
                        out << "  " << attr << "\n";
                    }
                    out << "  always " << sens << " begin\n";
                    for (const auto &stmt : orderSequentialStmts(seq.stmts))
                    {
                        out << stmt.text;
                        if (!stmt.text.empty() && stmt.text.back() != '\n')
                        {
                            out << '\n';
                        }
                    }
                    out << "  end\n";
                }
            }

            out << "endmodule\n";
        };

        if (options.splitModules)
        {
            const std::filesystem::path outputDir = resolveOutputDir(options);
            for (const wolvrix::lib::grh::Graph *graph : emittedGraphs)
            {
                if (!graph)
                {
                    continue;
                }

                const auto moduleNameIt = emittedModuleNames.find(graph->symbol());
                const std::string &moduleName =
                    moduleNameIt != emittedModuleNames.end() ? moduleNameIt->second : graph->symbol();
                const std::filesystem::path outputPath = outputDir / (moduleName + ".sv");
                auto stream = openOutputFile(outputPath);
                if (!stream)
                {
                    result.success = false;
                    return result;
                }

                emitGraph(graph, *stream);
                result.artifacts.push_back(outputPath.string());
            }
            return result;
        }

        const std::string filename = options.outputFilename.value_or(std::string("grh.sv"));
        const std::filesystem::path outputPath = resolveOutputDir(options) / filename;
        auto stream = openOutputFile(outputPath);
        if (!stream)
        {
            result.success = false;
            return result;
        }

        std::ostream &out = *stream;
        bool firstModule = true;
        for (const wolvrix::lib::grh::Graph *graph : emittedGraphs)
        {
            if (!graph)
            {
                continue;
            }
            if (!firstModule)
            {
                out << '\n';
            }
            firstModule = false;
            emitGraph(graph, out);
        }

        result.artifacts.push_back(outputPath.string());
        return result;
    }

    
} // namespace wolvrix::lib::emit
