#include "core/store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <unordered_map>
#include <tuple>
#include <string_view>

#include "slang/numeric/SVInt.h"
#include "slang/text/Json.h"

namespace wolvrix::lib::store
{

    namespace
    {
        using TimingClock = std::chrono::steady_clock;

        bool jsonTimingEnabled()
        {
            static const bool enabled = []() {
                const char *env = std::getenv("WOLVRIX_JSON_TIMING");
                if (!env || *env == '\0')
                {
                    return false;
                }
                return !(env[0] == '0' && env[1] == '\0');
            }();
            return enabled;
        }

        double toMillis(TimingClock::duration duration)
        {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        void logJsonTiming(std::string_view label, double ms, std::string_view details = {})
        {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(3);
            oss << "[wolvrix-json] " << label << " " << ms << "ms";
            if (!details.empty())
            {
                oss << " " << details;
            }
            std::cerr << oss.str() << '\n';
        }

        std::size_t jsonTimingStep()
        {
            static const std::size_t step = []() {
                const char *env = std::getenv("WOLVRIX_JSON_TIMING_STEP");
                if (!env || *env == '\0')
                {
                    return static_cast<std::size_t>(1000000);
                }
                char *end = nullptr;
                const unsigned long long value = std::strtoull(env, &end, 10);
                if (end == env || *end != '\0')
                {
                    return static_cast<std::size_t>(1000000);
                }
                return static_cast<std::size_t>(value);
            }();
            return step;
        }

        const char *jsonModeName(JsonPrintMode mode)
        {
            switch (mode)
            {
            case JsonPrintMode::Compact:
                return "compact";
            case JsonPrintMode::Pretty:
                return "pretty";
            case JsonPrintMode::PrettyCompact:
            default:
                return "pretty-compact";
            }
        }

        struct GraphWriteStats {
            double declaredMs = 0.0;
            double valuesMs = 0.0;
            double portsInMs = 0.0;
            double portsOutMs = 0.0;
            double portsInoutMs = 0.0;
            double opsMs = 0.0;
            std::size_t declaredCount = 0;
            std::size_t valuesCount = 0;
            std::size_t opsCount = 0;
            std::size_t portsInCount = 0;
            std::size_t portsOutCount = 0;
            std::size_t portsInoutCount = 0;
        };

        template <class... Ts>
        struct Overloaded : Ts...
        {
            using Ts::operator()...;
        };
        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

        constexpr int kIndentSize = 2;

        void appendDouble(std::string &out, double value)
        {
            std::ostringstream oss;
            oss << value;
            out.append(oss.str());
        }

        void appendQuotedString(std::string &out, std::string_view text)
        {
            out.push_back('"');
            for (char c : text)
            {
                switch (c)
                {
                case '"':
                    out.append("\\\"");
                    break;
                case '\\':
                    out.append("\\\\");
                    break;
                case '\b':
                    out.append("\\b");
                    break;
                case '\f':
                    out.append("\\f");
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
                default:
                    if (static_cast<unsigned char>(c) <= 0x1f)
                    {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out.append(buf);
                    }
                    else
                    {
                        out.push_back(c);
                    }
                    break;
                }
            }
            out.push_back('"');
        }

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

        void appendNewlineAndIndent(std::string &out, int indent)
        {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent * kIndentSize), ' ');
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

        std::vector<const wolvrix::lib::grh::Graph *> graphsSortedByName(const wolvrix::lib::grh::Design &design)
        {
            std::vector<const wolvrix::lib::grh::Graph *> graphs;
            graphs.reserve(design.graphs().size());
            for (const auto &symbol : design.graphOrder())
            {
                if (auto it = design.graphs().find(symbol); it != design.graphs().end())
                {
                    graphs.push_back(it->second.get());
                }
            }
            return graphs;
        }

        std::string serializeWithJsonWriter(const wolvrix::lib::grh::Design &design,
                                            std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                            bool pretty)
        {
            auto collectAliases = [&](const wolvrix::lib::grh::Design &source)
                -> std::vector<std::pair<std::string, std::string>> {
                std::vector<std::pair<std::string, std::string>> result;
                for (const auto &graphSymbol : source.graphOrder())
                {
                    for (const auto &alias : source.aliasesForGraph(graphSymbol))
                    {
                        result.emplace_back(alias, graphSymbol);
                    }
                }
                std::sort(result.begin(), result.end(),
                          [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
                return result;
            };

            slang::JsonWriter writer;
            writer.setPrettyPrint(pretty);
            writer.startObject();

            const bool timingEnabled = jsonTimingEnabled();
            writer.writeProperty("graphs");
            writer.startArray();
            for (const wolvrix::lib::grh::Graph *graph : graphsSortedByName(design))
            {
                TimingClock::time_point graphStart;
                if (timingEnabled)
                {
                    graphStart = TimingClock::now();
                }
                graph->writeJson(writer);
                if (timingEnabled)
                {
                    const auto graphEnd = TimingClock::now();
                    std::ostringstream details;
                    details << "symbol=" << graph->symbol()
                            << " values=" << graph->values().size()
                            << " ops=" << graph->operations().size()
                            << " ports_in=" << graph->inputPorts().size()
                            << " ports_out=" << graph->outputPorts().size()
                            << " ports_inout=" << graph->inoutPorts().size();
                    logJsonTiming("store_json.graph", toMillis(graphEnd - graphStart), details.str());
                }
            }
            writer.endArray();

            writer.writeProperty("aliases");
            writer.startObject();
            for (const auto &[alias, graphSymbol] : collectAliases(design))
            {
                writer.writeProperty(alias);
                writer.writeValue(graphSymbol);
            }
            writer.endObject();

            writer.writeProperty("declaredSymbols");
            writer.startArray();
            for (const auto sym : design.declaredSymbols())
            {
                std::string_view text = design.symbolText(sym);
                if (text.empty())
                {
                    throw std::runtime_error("Design declared symbol is empty");
                }
                writer.writeValue(text);
            }
            writer.endArray();

            writer.writeProperty("tops");
            writer.startArray();
            for (const wolvrix::lib::grh::Graph *graph : topGraphs)
            {
                writer.writeValue(graph->symbol());
            }
            writer.endArray();

            writer.endObject();
            return std::string(writer.view());
        }

        const char *colonToken(JsonPrintMode mode)
        {
            return mode == JsonPrintMode::Compact ? ":" : ": ";
        }

        const char *commaToken(JsonPrintMode mode)
        {
            return mode == JsonPrintMode::Compact ? "," : ", ";
        }

        template <typename Writer>
        void writeInlineObject(std::string &out, JsonPrintMode mode, Writer &&writer)
        {
            out.push_back('{');
            bool first = true;
            const char *comma = commaToken(mode);
            writer([&](std::string_view key, auto &&valueWriter)
                   {
                       if (!first)
                       {
                           out.append(comma);
                       }
                       appendQuotedString(out, key);
                       out.append(colonToken(mode));
                       valueWriter();
                       first = false;
                   });
            out.push_back('}');
        }

        void writeDebugInline(std::string &out, JsonPrintMode mode,
                              const std::optional<wolvrix::lib::grh::SrcLoc> &debugInfo)
        {
            writeInlineObject(out, mode, [&](auto &&prop)
                              {
                                  if (!debugInfo->file.empty())
                                  {
                                      prop("file", [&]
                                           { appendQuotedString(out, debugInfo->file); });
                                  }
                                  if (debugInfo->line != 0)
                                  {
                                      prop("line", [&]
                                           { out.append(std::to_string(debugInfo->line)); });
                                  }
                                  if (debugInfo->column != 0)
                                  {
                                      prop("col", [&]
                                           { out.append(std::to_string(debugInfo->column)); });
                                  }
                                  if (debugInfo->endLine != 0)
                                  {
                                      prop("endLine", [&]
                                           { out.append(std::to_string(debugInfo->endLine)); });
                                  }
                                  if (debugInfo->endColumn != 0)
                                  {
                                      prop("endCol", [&]
                                           { out.append(std::to_string(debugInfo->endColumn)); });
                                  }
                                  if (!debugInfo->origin.empty())
                                  {
                                      prop("origin", [&]
                                           { appendQuotedString(out, debugInfo->origin); });
                                  }
                                  if (!debugInfo->pass.empty())
                                  {
                                      prop("pass", [&]
                                           { appendQuotedString(out, debugInfo->pass); });
                                  }
                                  if (!debugInfo->note.empty())
                                  {
                                      prop("note", [&]
                                           { appendQuotedString(out, debugInfo->note); });
                                  }
                              });
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
                const wolvrix::lib::grh::Value value = graph.getValue(valueId);
                if (value.symbolText().empty())
                {
                    throw std::runtime_error("Graph value missing symbol during emit");
                }
            }
            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.symbolText().empty())
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

        void writeUsersInline(std::string &out, const wolvrix::lib::grh::Graph &graph, std::span<const wolvrix::lib::grh::ValueUser> users, JsonPrintMode mode)
        {
            const char *comma = commaToken(mode);
            out.push_back('[');
            bool first = true;
            for (const auto &user : users)
            {
                if (!first)
                {
                    out.append(comma);
                }
                writeInlineObject(out, mode, [&](auto &&prop)
                                  {
                                      prop("op", [&]
                                           {
                                               if (!user.operation.valid())
                                               {
                                                   throw std::runtime_error("Value user missing operation during emit");
                                               }
                                               appendQuotedString(out, opSymbolRequired(graph, user.operation));
                                           });
                                      prop("idx", [&]
                                           { out.append(std::to_string(static_cast<int64_t>(user.operandIndex))); });
                                  });
                first = false;
            }
            out.push_back(']');
        }

        void writeAttrsInline(std::string &out, std::span<const wolvrix::lib::grh::AttrKV> attrs, JsonPrintMode mode)
        {
            out.push_back('{');
            bool firstAttr = true;
            const char *comma = commaToken(mode);
            for (const auto &attr : attrs)
            {
                if (!firstAttr)
                {
                    out.append(comma);
                }
                appendQuotedString(out, attr.key);
                out.append(colonToken(mode));
                writeInlineObject(out, mode, [&](auto &&prop)
                                  {
                                      std::visit(
                                          Overloaded{
                                              [&](bool v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "bool"); });
                                                  prop("v", [&]
                                                       { out.append(v ? "true" : "false"); });
                                              },
                                              [&](int64_t v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "int"); });
                                                  prop("v", [&]
                                                       { out.append(std::to_string(v)); });
                                              },
                                              [&](double v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "double"); });
                                                  prop("v", [&]
                                                       { appendDouble(out, v); });
                                              },
                                              [&](const std::string &v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "string"); });
                                                  prop("v", [&]
                                                       { appendQuotedString(out, v); });
                                              },
                                              [&](const std::vector<bool> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "bool[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (bool entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               out.append(entry ? "true" : "false");
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<int64_t> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "int[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (int64_t entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               out.append(std::to_string(entry));
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<double> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "double[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                          for (double entry : arr)
                                                          {
                                                              if (!first)
                                                              {
                                                                  out.append(comma);
                                                              }
                                                              appendDouble(out, entry);
                                                              first = false;
                                                          }
                                                          out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<std::string> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "string[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (const auto &entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               appendQuotedString(out, entry);
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              }},
                                          attr.value);
                                  });
                firstAttr = false;
            }
            out.push_back('}');
        }

        void writeValueInline(std::string &out, const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Value &value, JsonPrintMode mode)
        {
            writeInlineObject(out, mode, [&](auto &&prop)
                              {
                                  prop("sym", [&]
                                       { appendQuotedString(out, valueSymbolRequired(value)); });
                                  prop("w", [&]
                                       { out.append(std::to_string(value.width())); });
                                  prop("sgn", [&]
                                       { out.append(value.isSigned() ? "true" : "false"); });
                                  prop("type", [&]
                                       { appendQuotedString(out, std::string(wolvrix::lib::grh::toString(value.type()))); });
                                  prop("in", [&]
                                       { out.append(value.isInput() ? "true" : "false"); });
                                  prop("out", [&]
                                       { out.append(value.isOutput() ? "true" : "false"); });
                                  prop("inout", [&]
                                       { out.append(value.isInout() ? "true" : "false"); });
                                  if (value.definingOp())
                                  {
                                      prop("def", [&]
                                           { appendQuotedString(out, opSymbolRequired(graph, value.definingOp())); });
                                  }
                                  prop("users", [&]
                                       { writeUsersInline(out, graph, value.users(), mode); });
                                  if (value.srcLoc())
                                  {
                                      prop("loc", [&]
                                           { writeDebugInline(out, mode, value.srcLoc()); });
                                  }
                              });
        }

        void writePortInline(std::string &out, std::string_view name, std::string_view valueSymbol, JsonPrintMode mode)
        {
            writeInlineObject(out, mode, [&](auto &&prop)
                              {
                                  if (name.empty() || valueSymbol.empty())
                                  {
                                      throw std::runtime_error("Port name or value symbol missing during emit");
                                  }
                                  prop("name", [&]
                                       { appendQuotedString(out, name); });
                                  prop("val", [&]
                                       { appendQuotedString(out, valueSymbol); });
                              });
        }

        void writeOperationInline(std::string &out, const wolvrix::lib::grh::Graph &graph, const wolvrix::lib::grh::Operation &op, JsonPrintMode mode)
        {
            writeInlineObject(out, mode, [&](auto &&prop)
                              {
                                  prop("sym", [&]
                                       { appendQuotedString(out, opSymbolRequired(graph, op.id())); });
                                  prop("kind", [&]
                                       { appendQuotedString(out, wolvrix::lib::grh::toString(op.kind())); });
                                  prop("in", [&]
                                       {
                                           out.push_back('[');
                                           bool first = true;
                                           const char *comma = commaToken(mode);
                                           for (const auto operandId : op.operands())
                                           {
                                               if (!first)
                                               {
                                                   out.append(comma);
                                               }
                                               appendQuotedString(out, valueSymbolRequired(graph, operandId));
                                               first = false;
                                           }
                                           out.push_back(']');
                                       });
                                  prop("out", [&]
                                       {
                                           out.push_back('[');
                                           bool first = true;
                                           const char *comma = commaToken(mode);
                                           for (const auto resultId : op.results())
                                           {
                                               if (!first)
                                               {
                                                   out.append(comma);
                                               }
                                               appendQuotedString(out, valueSymbolRequired(graph, resultId));
                                               first = false;
                                           }
                                           out.push_back(']');
                                       });
                                  if (!op.attrs().empty())
                                  {
                                      prop("attrs", [&]
                                           { writeAttrsInline(out, op.attrs(), mode); });
                                  }
                                  if (op.srcLoc())
                                  {
                                      prop("loc", [&]
                                           { writeDebugInline(out, mode, op.srcLoc()); });
                                  }
                              });
        }

        template <typename Range, typename Fn>
        void writeInlineArray(std::string &out, JsonPrintMode mode, int elementIndent, const Range &range, Fn &&writeElement)
        {
            (void)mode;
            out.push_back('[');
            bool first = true;
            for (const auto &entry : range)
            {
                if (!first)
                {
                    out.push_back(',');
                }
                appendNewlineAndIndent(out, elementIndent);
                writeElement(entry);
                first = false;
            }
            if (!range.empty())
            {
                appendNewlineAndIndent(out, elementIndent - 1);
            }
            out.push_back(']');
        }

        void writePortsPrettyCompact(std::string &out,
                                     const wolvrix::lib::grh::Graph &graph,
                                     std::span<const wolvrix::lib::grh::Port> ports,
                                     JsonPrintMode mode,
                                     int indent)
        {
            (void)mode;
            out.push_back('[');
            bool first = true;
            for (const auto &port : ports)
            {
                if (!first)
                {
                    out.push_back(',');
                }
                appendNewlineAndIndent(out, indent);
                writePortInline(out,
                                port.name,
                                valueSymbolRequired(graph, port.value),
                                mode);
                first = false;
            }
            if (!ports.empty())
            {
                appendNewlineAndIndent(out, indent - 1);
            }
            out.push_back(']');
        }

        void writeInoutPortsPrettyCompact(std::string &out,
                                          const wolvrix::lib::grh::Graph &graph,
                                          std::span<const wolvrix::lib::grh::InoutPort> ports,
                                          JsonPrintMode mode,
                                          int indent)
        {
            (void)mode;
            out.push_back('[');
            bool first = true;
            for (const auto &port : ports)
            {
                if (!first)
                {
                    out.push_back(',');
                }
                appendNewlineAndIndent(out, indent);
                writeInlineObject(out, mode, [&](auto &&prop)
                                  {
                                      prop("name", [&]
                                           {
                                               if (port.name.empty())
                                               {
                                                   throw std::runtime_error("Inout port name is empty during emit");
                                               }
                                               appendQuotedString(out, port.name);
                                           });
                                      prop("in", [&]
                                           { appendQuotedString(out, valueSymbolRequired(graph, port.in)); });
                                      prop("out", [&]
                                           { appendQuotedString(out, valueSymbolRequired(graph, port.out)); });
                                      prop("oe", [&]
                                           { appendQuotedString(out, valueSymbolRequired(graph, port.oe)); });
                                  });
                first = false;
            }
            if (!ports.empty())
            {
                appendNewlineAndIndent(out, indent - 1);
            }
            out.push_back(']');
        }

        void writeGenerateGroupsInline(std::string &out,
                                       const wolvrix::lib::grh::Graph &graph,
                                       int indent)
        {
            auto symbolTextRequired = [&](wolvrix::lib::grh::SymbolId sym) -> std::string_view
            {
                std::string_view text = graph.symbolText(sym);
                if (text.empty())
                {
                    throw std::runtime_error("Graph generate group symbol is empty");
                }
                return text;
            };
            out.push_back('[');
            bool firstGroup = true;
            for (const auto &group : graph.generateGroups())
            {
                if (!firstGroup)
                {
                    out.push_back(',');
                }
                appendNewlineAndIndent(out, indent + 1);
                out.push_back('{');
                appendQuotedString(out, "scope");
                out.append(": ");
                appendQuotedString(out, symbolTextRequired(group.scope));
                out.append(", ");
                appendQuotedString(out, "name");
                out.append(": ");
                appendQuotedString(out, symbolTextRequired(group.name));
                out.append(", ");
                appendQuotedString(out, "symbols");
                out.append(": [");
                bool firstSym = true;
                for (const auto sym : group.symbols)
                {
                    if (!firstSym)
                    {
                        out.append(", ");
                    }
                    appendQuotedString(out, symbolTextRequired(sym));
                    firstSym = false;
                }
                out.append("]}");
                firstGroup = false;
            }
            if (!graph.generateGroups().empty())
            {
                appendNewlineAndIndent(out, indent);
            }
            out.push_back(']');
        }

        void writeGraphPrettyCompact(std::string &out,
                                     const wolvrix::lib::grh::Graph &graph,
                                     int baseIndent,
                                     GraphWriteStats *stats,
                                     bool timingEnabled,
                                     std::size_t progressStep)
        {
            out.push_back('{');
            int indent = baseIndent + 1;
            const bool useTiming = timingEnabled && stats;

            if (stats)
            {
                stats->declaredCount = graph.declaredSymbols().size();
                stats->valuesCount = graph.values().size();
                stats->opsCount = graph.operations().size();
                stats->portsInCount = graph.inputPorts().size();
                stats->portsOutCount = graph.outputPorts().size();
                stats->portsInoutCount = graph.inoutPorts().size();
            }

            appendNewlineAndIndent(out, indent);
            appendQuotedString(out, "symbol");
            out.append(": ");
            appendQuotedString(out, graph.symbol());
            out.push_back(',');

            TimingClock::time_point sectionStart;
            if (useTiming)
            {
                sectionStart = TimingClock::now();
            }
            appendNewlineAndIndent(out, indent);
            appendQuotedString(out, "declaredSymbols");
            out.append(": ");
            writeInlineArray(out, JsonPrintMode::PrettyCompact, indent + 1, graph.declaredSymbols(),
                             [&](const wolvrix::lib::grh::SymbolId sym)
                             {
                                 std::string_view text = graph.symbolText(sym);
                                 if (text.empty())
                                 {
                                     throw std::runtime_error("Graph declared symbol is empty");
                                 }
                                 appendQuotedString(out, text);
                             });
            if (useTiming)
            {
                stats->declaredMs = toMillis(TimingClock::now() - sectionStart);
                sectionStart = TimingClock::now();
            }
            if (!graph.generateGroups().empty())
            {
                out.push_back(',');
                appendNewlineAndIndent(out, indent);
                appendQuotedString(out, "generateGroups");
                out.append(": ");
                writeGenerateGroupsInline(out, graph, indent);
            }
            out.push_back(',');

            appendNewlineAndIndent(out, indent);
            appendQuotedString(out, "vals");
            out.append(": ");
            {
                const auto values = graph.values();
                out.push_back('[');
                bool first = true;
                std::size_t valueIndex = 0;
                TimingClock::time_point valuesStart;
                if (useTiming)
                {
                    valuesStart = TimingClock::now();
                }
                for (const auto valueId : values)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    writeValueInline(out, graph, graph.getValue(valueId), JsonPrintMode::PrettyCompact);
                    first = false;
                    ++valueIndex;
                    if (useTiming && progressStep > 0 && (valueIndex % progressStep) == 0)
                    {
                        const auto now = TimingClock::now();
                        std::ostringstream details;
                        details << "symbol=" << graph.symbol()
                                << " count=" << valueIndex
                                << " elapsed=" << toMillis(now - valuesStart) << "ms";
                        logJsonTiming("store_json.values.progress", toMillis(now - valuesStart), details.str());
                    }
                }
                if (!values.empty())
                {
                    appendNewlineAndIndent(out, indent);
                }
                out.push_back(']');
                if (useTiming)
                {
                    stats->valuesMs = toMillis(TimingClock::now() - valuesStart);
                    sectionStart = TimingClock::now();
                }
            }
            out.push_back(',');

            appendNewlineAndIndent(out, indent);
            appendQuotedString(out, "ports");
            out.append(": ");
            out.push_back('{');
            int portsIndent = indent + 1;

            appendNewlineAndIndent(out, portsIndent);
            appendQuotedString(out, "in");
            out.append(": ");
            writePortsPrettyCompact(out, graph, graph.inputPorts(), JsonPrintMode::PrettyCompact, portsIndent + 1);
            if (useTiming)
            {
                stats->portsInMs = toMillis(TimingClock::now() - sectionStart);
                sectionStart = TimingClock::now();
            }
            out.push_back(',');

            appendNewlineAndIndent(out, portsIndent);
            appendQuotedString(out, "out");
            out.append(": ");
            writePortsPrettyCompact(out, graph, graph.outputPorts(), JsonPrintMode::PrettyCompact, portsIndent + 1);
            if (useTiming)
            {
                stats->portsOutMs = toMillis(TimingClock::now() - sectionStart);
                sectionStart = TimingClock::now();
            }
            out.push_back(',');

            appendNewlineAndIndent(out, portsIndent);
            appendQuotedString(out, "inout");
            out.append(": ");
            writeInoutPortsPrettyCompact(out, graph, graph.inoutPorts(), JsonPrintMode::PrettyCompact, portsIndent + 1);
            if (useTiming)
            {
                stats->portsInoutMs = toMillis(TimingClock::now() - sectionStart);
                sectionStart = TimingClock::now();
            }

            appendNewlineAndIndent(out, indent);
            out.push_back('}');
            out.push_back(',');

            appendNewlineAndIndent(out, indent);
            appendQuotedString(out, "ops");
            out.append(": ");
            {
                const auto ops = graph.operations();
                out.push_back('[');
                bool first = true;
                std::size_t opIndex = 0;
                TimingClock::time_point opsStart;
                if (useTiming)
                {
                    opsStart = TimingClock::now();
                }
                for (const auto opId : ops)
                {
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    writeOperationInline(out, graph, graph.getOperation(opId), JsonPrintMode::PrettyCompact);
                    first = false;
                    ++opIndex;
                    if (useTiming && progressStep > 0 && (opIndex % progressStep) == 0)
                    {
                        const auto now = TimingClock::now();
                        std::ostringstream details;
                        details << "symbol=" << graph.symbol()
                                << " count=" << opIndex
                                << " elapsed=" << toMillis(now - opsStart) << "ms";
                        logJsonTiming("store_json.ops.progress", toMillis(now - opsStart), details.str());
                    }
                }
                if (!ops.empty())
                {
                    appendNewlineAndIndent(out, indent);
                }
                out.push_back(']');
                if (useTiming)
                {
                    stats->opsMs = toMillis(TimingClock::now() - opsStart);
                }
            }

            appendNewlineAndIndent(out, baseIndent);
            out.push_back('}');
        }

        const std::string &lookupName(const std::vector<std::string> &names, uint32_t index)
        {
            static const std::string kEmpty;
            if (index == 0 || index > names.size())
            {
                return kEmpty;
            }
            return names[index - 1];
        }

        void writeUsersInlineIr(std::string &out,
                                std::span<const wolvrix::lib::grh::ValueUser> users,
                                const std::vector<std::string> &opNames,
                                JsonPrintMode mode)
        {
            const char *comma = commaToken(mode);
            out.push_back('[');
            bool first = true;
            for (const auto &user : users)
            {
                if (!first)
                {
                    out.append(comma);
                }
                writeInlineObject(out, mode, [&](auto &&prop)
                                  {
                                      const std::string &opName = lookupName(opNames, user.operation.index);
                                      prop("op", [&]
                                           { appendQuotedString(out, opName); });
                                      prop("idx", [&]
                                           { out.append(std::to_string(static_cast<int64_t>(user.operandIndex))); });
                                  });
                first = false;
            }
            out.push_back(']');
        }

        void writeAttrsInlineIr(std::string &out,
                                std::span<const wolvrix::lib::grh::AttrKV> attrs,
                                JsonPrintMode mode)
        {
            out.push_back('{');
            bool firstAttr = true;
            const char *comma = commaToken(mode);
            for (const auto &attr : attrs)
            {
                if (!firstAttr)
                {
                    out.append(comma);
                }
                appendQuotedString(out, attr.key);
                out.append(colonToken(mode));
                writeInlineObject(out, mode, [&](auto &&prop)
                                  {
                                      std::visit(
                                          Overloaded{
                                              [&](bool v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "bool"); });
                                                  prop("v", [&]
                                                       { out.append(v ? "true" : "false"); });
                                              },
                                              [&](int64_t v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "int"); });
                                                  prop("v", [&]
                                                       { out.append(std::to_string(v)); });
                                              },
                                              [&](double v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "double"); });
                                                  prop("v", [&]
                                                       { appendDouble(out, v); });
                                              },
                                              [&](const std::string &v)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "string"); });
                                                  prop("v", [&]
                                                       { appendQuotedString(out, v); });
                                              },
                                              [&](const std::vector<bool> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "bool[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (bool entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               out.append(entry ? "true" : "false");
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<int64_t> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "int[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (int64_t entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               out.append(std::to_string(entry));
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<double> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "double[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (double entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               appendDouble(out, entry);
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              },
                                              [&](const std::vector<std::string> &arr)
                                              {
                                                  prop("t", [&]
                                                       { appendQuotedString(out, "string[]"); });
                                                  prop("vs", [&]
                                                       {
                                                           out.push_back('[');
                                                           bool first = true;
                                                           for (const auto &entry : arr)
                                                           {
                                                               if (!first)
                                                               {
                                                                   out.append(comma);
                                                               }
                                                               appendQuotedString(out, entry);
                                                               first = false;
                                                           }
                                                           out.push_back(']');
                                                       });
                                              }},
                                          attr.value);
                                  });
                firstAttr = false;
            }
            out.push_back('}');
        }

        std::string serializePrettyCompact(const wolvrix::lib::grh::Design &design, std::span<const wolvrix::lib::grh::Graph *const> topGraphs)
        {
            std::string out;
            int indent = 0;

            out.push_back('{');
            indent = 1;
            appendNewlineAndIndent(out, indent);

            appendQuotedString(out, "graphs");
            out.append(": [");

            const auto graphs = graphsSortedByName(design);
            const bool timingEnabled = jsonTimingEnabled();
            const std::size_t progressStep = timingEnabled ? jsonTimingStep() : 0;
            if (!graphs.empty())
            {
                bool first = true;
                for (const wolvrix::lib::grh::Graph *graph : graphs)
                {
                    TimingClock::time_point graphStart;
                    if (timingEnabled)
                    {
                        graphStart = TimingClock::now();
                    }
                    if (!first)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    GraphWriteStats stats{};
                    GraphWriteStats *statsPtr = timingEnabled ? &stats : nullptr;
                    writeGraphPrettyCompact(out, *graph, indent + 1, statsPtr, timingEnabled, progressStep);
                    if (timingEnabled)
                    {
                        const auto graphEnd = TimingClock::now();
                        std::ostringstream details;
                        details << "symbol=" << graph->symbol()
                                << " values=" << stats.valuesCount
                                << " ops=" << stats.opsCount
                                << " ports_in=" << stats.portsInCount
                                << " ports_out=" << stats.portsOutCount
                                << " ports_inout=" << stats.portsInoutCount
                                << " sections_ms=decl:" << stats.declaredMs
                                << ",vals:" << stats.valuesMs
                                << ",ports_in:" << stats.portsInMs
                                << ",ports_out:" << stats.portsOutMs
                                << ",ports_inout:" << stats.portsInoutMs
                                << ",ops:" << stats.opsMs;
                        logJsonTiming("store_json.graph", toMillis(graphEnd - graphStart), details.str());
                    }
                    first = false;
                }
                appendNewlineAndIndent(out, indent);
            }
            out.push_back(']');
            out.push_back(',');
            appendNewlineAndIndent(out, indent);

            appendQuotedString(out, "aliases");
            out.append(": {");
            std::vector<std::pair<std::string, std::string>> aliases;
            for (const auto &graphSymbol : design.graphOrder())
            {
                for (const auto &alias : design.aliasesForGraph(graphSymbol))
                {
                    aliases.emplace_back(alias, graphSymbol);
                }
            }
            if (!aliases.empty())
            {
                std::sort(aliases.begin(), aliases.end(),
                          [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
                bool firstAlias = true;
                for (const auto &[alias, graphSymbol] : aliases)
                {
                    if (!firstAlias)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    appendQuotedString(out, alias);
                    out.append(": ");
                    appendQuotedString(out, graphSymbol);
                    firstAlias = false;
                }
                appendNewlineAndIndent(out, indent);
            }
            out.push_back('}');
            out.push_back(',');
            appendNewlineAndIndent(out, indent);

            appendQuotedString(out, "declaredSymbols");
            out.append(": [");
            const auto designDeclared = design.declaredSymbols();
            if (!designDeclared.empty())
            {
                bool firstDecl = true;
                for (const auto sym : designDeclared)
                {
                    if (!firstDecl)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    std::string_view text = design.symbolText(sym);
                    if (text.empty())
                    {
                        throw std::runtime_error("Design declared symbol is empty");
                    }
                    appendQuotedString(out, text);
                    firstDecl = false;
                }
                appendNewlineAndIndent(out, indent);
            }
            out.push_back(']');
            out.push_back(',');
            appendNewlineAndIndent(out, indent);

            appendQuotedString(out, "tops");
            out.append(": [");
            if (!topGraphs.empty())
            {
                bool firstTop = true;
                for (const wolvrix::lib::grh::Graph *graph : topGraphs)
                {
                    if (!firstTop)
                    {
                        out.push_back(',');
                    }
                    appendNewlineAndIndent(out, indent + 1);
                    appendQuotedString(out, graph->symbol());
                    firstTop = false;
                }
                appendNewlineAndIndent(out, indent);
            }
            out.push_back(']');

            appendNewlineAndIndent(out, indent - 1);
            out.push_back('}');
            return out;
        }

        std::string serializeDesignJson(const wolvrix::lib::grh::Design &design,
                                         std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                         JsonPrintMode mode)
        {
            switch (mode)
            {
            case JsonPrintMode::Compact:
                return serializeWithJsonWriter(design, topGraphs, /* pretty */ false);
            case JsonPrintMode::Pretty:
                return serializeWithJsonWriter(design, topGraphs, /* pretty */ true);
            case JsonPrintMode::PrettyCompact:
            default:
                return serializePrettyCompact(design, topGraphs);
            }
        }
    } // namespace

    Store::Store(StoreDiagnostics *diagnostics) : diagnostics_(diagnostics) {}

    void Store::reportError(std::string message, std::string context) const
    {
        if (diagnostics_ != nullptr)
        {
            diagnostics_->error(std::move(message), std::move(context));
        }
    }

    void Store::reportWarning(std::string message, std::string context) const
    {
        if (diagnostics_ != nullptr)
        {
            diagnostics_->warning(std::move(message), std::move(context));
        }
    }

    bool Store::validateTopGraphs(const std::vector<const wolvrix::lib::grh::Graph *> &topGraphs) const
    {
        if (topGraphs.empty())
        {
            reportError("No top graphs available for emission");
            return false;
        }
        return true;
    }

    std::vector<const wolvrix::lib::grh::Graph *> Store::resolveTopGraphs(const wolvrix::lib::grh::Design &design,
                                                           const StoreOptions &options) const
    {
        std::vector<const wolvrix::lib::grh::Graph *> result;
        std::unordered_set<std::string> seen;

        auto tryAdd = [&](std::string_view name)
        {
            if (seen.find(std::string(name)) != seen.end())
            {
                return;
            }

            const wolvrix::lib::grh::Graph *graph = design.findGraph(name);
            if (graph == nullptr)
            {
                reportError("Top graph not found", std::string(name));
                return;
            }

            seen.insert(std::string(graph->symbol()));
            result.push_back(graph);
        };

        if (!options.topOverrides.empty())
        {
            for (const auto &name : options.topOverrides)
            {
                tryAdd(name);
            }
        }
        else
        {
            for (const auto &name : design.topGraphs())
            {
                tryAdd(name);
            }
        }

        return result;
    }

    std::filesystem::path Store::resolveOutputDir(const StoreOptions &options) const
    {
        if (options.outputDir && !options.outputDir->empty())
        {
            return std::filesystem::path(*options.outputDir);
        }
        return std::filesystem::current_path();
    }

    bool Store::ensureParentDirectory(const std::filesystem::path &path) const
    {
        const std::filesystem::path parent = path.parent_path();
        if (parent.empty())
        {
            return true;
        }

        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            reportError("Failed to create output directory: " + ec.message(), parent.string());
            return false;
        }
        return true;
    }

    std::unique_ptr<std::ofstream> Store::openOutputFile(const std::filesystem::path &path) const
    {
        if (!ensureParentDirectory(path))
        {
            return nullptr;
        }

        auto stream = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::trunc);
        if (!stream->is_open())
        {
            reportError("Failed to open output file for writing", path.string());
            return nullptr;
        }
        return stream;
    }

    StoreResult Store::store(const wolvrix::lib::grh::Design &design, const StoreOptions &options)
    {
        StoreResult result;

        std::vector<const wolvrix::lib::grh::Graph *> topGraphs = resolveTopGraphs(design, options);
        if (!validateTopGraphs(topGraphs))
        {
            result.success = false;
            return result;
        }

        result = storeImpl(design, topGraphs, options);
        if (diagnostics_ && diagnostics_->hasError())
        {
            result.success = false;
        }
        return result;
    }

    std::optional<std::string> StoreJson::storeToString(const wolvrix::lib::grh::Design &design, const StoreOptions &options)
    {
        std::vector<const wolvrix::lib::grh::Graph *> topGraphs = resolveTopGraphs(design, options);
        if (!validateTopGraphs(topGraphs))
        {
            return std::nullopt;
        }

        try
        {
            return serializeDesignJson(design, topGraphs, options.jsonMode);
        }
        catch (const std::exception &ex)
        {
            reportError("Failed to serialize design to JSON: " + std::string(ex.what()));
            return std::nullopt;
        }
    }

    StoreResult StoreJson::storeImpl(const wolvrix::lib::grh::Design &design,
                                  std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                  const StoreOptions &options)
    {
        StoreResult result;

        std::string jsonText;
        const bool timingEnabled = jsonTimingEnabled();
        TimingClock::time_point totalStart;
        TimingClock::time_point serializeStart;
        if (timingEnabled)
        {
            totalStart = TimingClock::now();
            serializeStart = totalStart;
        }
        try
        {
            jsonText = serializeDesignJson(design, topGraphs, options.jsonMode);
        }
        catch (const std::exception &ex)
        {
            reportError("Failed to serialize design to JSON: " + std::string(ex.what()));
            result.success = false;
            return result;
        }
        if (timingEnabled)
        {
            const auto serializeEnd = TimingClock::now();
            std::ostringstream details;
            details << "bytes=" << jsonText.size()
                    << " mode=" << jsonModeName(options.jsonMode);
            logJsonTiming("store_json.serialize", toMillis(serializeEnd - serializeStart), details.str());
        }

        const std::string filename = options.outputFilename.value_or(std::string("grh.json"));
        const std::filesystem::path outputPath = resolveOutputDir(options) / filename;
        auto stream = openOutputFile(outputPath);
        if (!stream)
        {
            result.success = false;
            return result;
        }

        TimingClock::time_point writeStart;
        if (timingEnabled)
        {
            writeStart = TimingClock::now();
        }
        *stream << jsonText;
        if (timingEnabled)
        {
            const auto writeEnd = TimingClock::now();
            std::ostringstream details;
            details << "bytes=" << jsonText.size()
                    << " path=" << outputPath.string();
            logJsonTiming("store_json.write_file", toMillis(writeEnd - writeStart), details.str());
            const auto totalEnd = TimingClock::now();
            logJsonTiming("store_json.total", toMillis(totalEnd - totalStart));
        }
        result.artifacts.push_back(outputPath.string());
        return result;
    }

} // namespace wolvrix::lib::store
