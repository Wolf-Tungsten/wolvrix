#include "core/load.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::load
{

    namespace
    {

        using wolvrix::lib::grh::AttributeValue;
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Design;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::Port;
        using wolvrix::lib::grh::InoutPort;
        using wolvrix::lib::grh::SymbolId;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::attributeValueIsJsonSerializable;
        using wolvrix::lib::grh::parseOperationKind;
        using wolvrix::lib::grh::SrcLoc;

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

        enum class AttributeKind
        {
            Bool,
            Int,
            Double,
            String,
            BoolArray,
            IntArray,
            DoubleArray,
            StringArray
        };

        struct JsonValue
        {
            using Array = std::vector<JsonValue>;
            using Object = std::map<std::string, JsonValue, std::less<>>;

            std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value;

            bool isNull() const { return std::holds_alternative<std::nullptr_t>(value); }
            bool isBool() const { return std::holds_alternative<bool>(value); }
            bool isInt() const { return std::holds_alternative<int64_t>(value); }
            bool isDouble() const { return std::holds_alternative<double>(value); }
            bool isString() const { return std::holds_alternative<std::string>(value); }
            bool isArray() const { return std::holds_alternative<Array>(value); }
            bool isObject() const { return std::holds_alternative<Object>(value); }

            bool asBool(std::string_view ctx) const
            {
                if (!isBool())
                {
                    throw std::runtime_error(std::string(ctx) + ": expected bool");
                }
                return std::get<bool>(value);
            }

            int64_t asInt(std::string_view ctx) const
            {
                if (isInt())
                {
                    return std::get<int64_t>(value);
                }
                if (isDouble())
                {
                    double v = std::get<double>(value);
                    if (std::trunc(v) != v)
                    {
                        throw std::runtime_error(std::string(ctx) + ": expected integer number");
                    }
                    return static_cast<int64_t>(v);
                }
                throw std::runtime_error(std::string(ctx) + ": expected integer");
            }

            double asDouble(std::string_view ctx) const
            {
                if (isDouble())
                {
                    return std::get<double>(value);
                }
                if (isInt())
                {
                    return static_cast<double>(std::get<int64_t>(value));
                }
                throw std::runtime_error(std::string(ctx) + ": expected number");
            }

            const std::string &asString(std::string_view ctx) const
            {
                if (!isString())
                {
                    throw std::runtime_error(std::string(ctx) + ": expected string");
                }
                return std::get<std::string>(value);
            }

            const Array &asArray(std::string_view ctx) const
            {
                if (!isArray())
                {
                    throw std::runtime_error(std::string(ctx) + ": expected array");
                }
                return std::get<Array>(value);
            }

            const Object &asObject(std::string_view ctx) const
            {
                if (!isObject())
                {
                    throw std::runtime_error(std::string(ctx) + ": expected object");
                }
                return std::get<Object>(value);
            }
        };

        class JsonParser
        {
        public:
            explicit JsonParser(std::string_view text) : text_(text), current_(text_.data()), end_(text_.data() + text_.size()) {}

            JsonValue parse()
            {
                skipWhitespace();
                JsonValue value = parseValue();
                skipWhitespace();
                if (current_ != end_)
                {
                    throw std::runtime_error("Trailing characters after JSON document");
                }
                return value;
            }

        private:
            JsonValue parseValue()
            {
                if (current_ == end_)
                {
                    throw std::runtime_error("Unexpected end of JSON input");
                }

                switch (*current_)
                {
                case '{':
                    return parseObject();
                case '[':
                    return parseArray();
                case '"':
                    return JsonValue{parseString()};
                case 't':
                    return JsonValue{parseLiteral("true", true)};
                case 'f':
                    return JsonValue{parseLiteral("false", false)};
                case 'n':
                    parseLiteral("null");
                    return JsonValue{std::nullptr_t{}};
                default:
                    if (*current_ == '-' || std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        return parseNumber();
                    }
                    break;
                }

                throw std::runtime_error("Invalid JSON value");
            }

            JsonValue parseObject()
            {
                JsonValue::Object obj;
                expect('{');
                skipWhitespace();
                if (consume('}'))
                {
                    return JsonValue{std::move(obj)};
                }

                while (true)
                {
                    skipWhitespace();
                    std::string key = parseString();
                    skipWhitespace();
                    expect(':');
                    skipWhitespace();
                    obj.emplace(std::move(key), parseValue());
                    skipWhitespace();
                    if (consume('}'))
                    {
                        break;
                    }
                    expect(',');
                }

                return JsonValue{std::move(obj)};
            }

            JsonValue parseArray()
            {
                JsonValue::Array arr;
                expect('[');
                skipWhitespace();
                if (consume(']'))
                {
                    return JsonValue{std::move(arr)};
                }

                while (true)
                {
                    skipWhitespace();
                    arr.push_back(parseValue());
                    skipWhitespace();
                    if (consume(']'))
                    {
                        break;
                    }
                    expect(',');
                }

                return JsonValue{std::move(arr)};
            }

            JsonValue parseNumber()
            {
                const char *start = current_;
                if (*current_ == '-')
                {
                    ++current_;
                }
                if (current_ == end_)
                {
                    throw std::runtime_error("Invalid JSON number");
                }
                if (*current_ == '0')
                {
                    ++current_;
                }
                else
                {
                    if (!std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        throw std::runtime_error("Invalid JSON number");
                    }
                    while (current_ != end_ && std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        ++current_;
                    }
                }

                bool isFloat = false;
                if (current_ != end_ && *current_ == '.')
                {
                    isFloat = true;
                    ++current_;
                    if (current_ == end_ || !std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        throw std::runtime_error("Invalid JSON number");
                    }
                    while (current_ != end_ && std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        ++current_;
                    }
                }

                if (current_ != end_ && (*current_ == 'e' || *current_ == 'E'))
                {
                    isFloat = true;
                    ++current_;
                    if (current_ != end_ && (*current_ == '+' || *current_ == '-'))
                    {
                        ++current_;
                    }
                    if (current_ == end_ || !std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        throw std::runtime_error("Invalid JSON number");
                    }
                    while (current_ != end_ && std::isdigit(static_cast<unsigned char>(*current_)))
                    {
                        ++current_;
                    }
                }

                std::string_view numberView(start, static_cast<std::size_t>(current_ - start));
                if (isFloat)
                {
                    double value = 0.0;
                    auto [ptr, ec] = std::from_chars(numberView.data(), numberView.data() + numberView.size(), value);
                    if (ec != std::errc())
                    {
                        value = std::stod(std::string(numberView));
                    }
                    return JsonValue{value};
                }

                int64_t intValue = 0;
                auto [ptr, ec] = std::from_chars(numberView.data(), numberView.data() + numberView.size(), intValue);
                if (ec != std::errc())
                {
                    intValue = std::stoll(std::string(numberView));
                }
                return JsonValue{intValue};
            }

            std::string parseString()
            {
                expect('"');
                const char *segmentStart = current_;
                while (current_ != end_)
                {
                    const char ch = *current_;
                    if (ch == '"')
                    {
                        std::string result(segmentStart, static_cast<std::size_t>(current_ - segmentStart));
                        ++current_;
                        return result;
                    }
                    if (ch == '\\')
                    {
                        break;
                    }
                    if (static_cast<unsigned char>(ch) < 0x20)
                    {
                        throw std::runtime_error("Invalid control character in string");
                    }
                    ++current_;
                }

                std::string result;
                result.reserve(static_cast<std::size_t>(current_ - segmentStart) + 8);
                result.append(segmentStart, static_cast<std::size_t>(current_ - segmentStart));
                while (current_ != end_)
                {
                    char ch = *current_++;
                    if (ch == '"')
                    {
                        return result;
                    }
                    if (ch == '\\')
                    {
                        if (current_ == end_)
                        {
                            throw std::runtime_error("Invalid escape sequence");
                        }

                        char esc = *current_++;
                        switch (esc)
                        {
                        case '"':
                        case '\\':
                        case '/':
                            result.push_back(esc);
                            break;
                        case 'b':
                            result.push_back('\b');
                            break;
                        case 'f':
                            result.push_back('\f');
                            break;
                        case 'n':
                            result.push_back('\n');
                            break;
                        case 'r':
                            result.push_back('\r');
                            break;
                        case 't':
                            result.push_back('\t');
                            break;
                        case 'u':
                        {
                            if (end_ - current_ < 4)
                            {
                                throw std::runtime_error("Invalid unicode escape");
                            }
                            unsigned value = 0;
                            for (int i = 0; i < 4; ++i)
                            {
                                char hex = *current_++;
                                value <<= 4;
                                if (hex >= '0' && hex <= '9')
                                {
                                    value |= static_cast<unsigned>(hex - '0');
                                }
                                else if (hex >= 'a' && hex <= 'f')
                                {
                                    value |= static_cast<unsigned>(hex - 'a' + 10);
                                }
                                else if (hex >= 'A' && hex <= 'F')
                                {
                                    value |= static_cast<unsigned>(hex - 'A' + 10);
                                }
                                else
                                {
                                    throw std::runtime_error("Invalid unicode escape");
                                }
                            }
                            if (value <= 0x7F)
                            {
                                result.push_back(static_cast<char>(value));
                            }
                            else if (value <= 0x7FF)
                            {
                                result.push_back(static_cast<char>(0xC0 | ((value >> 6) & 0x1F)));
                                result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                            }
                            else
                            {
                                result.push_back(static_cast<char>(0xE0 | ((value >> 12) & 0x0F)));
                                result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                                result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                            }
                            break;
                        }
                        default:
                            throw std::runtime_error("Invalid escape sequence");
                        }
                    }
                    else
                    {
                        if (static_cast<unsigned char>(ch) < 0x20)
                        {
                            throw std::runtime_error("Invalid control character in string");
                        }
                        result.push_back(ch);
                    }
                }
                throw std::runtime_error("Unterminated JSON string");
            }

            JsonValue parseLiteral(std::string_view literal, bool value)
            {
                for (char expected : literal)
                {
                    if (current_ == end_ || *current_++ != expected)
                    {
                        throw std::runtime_error("Invalid JSON literal");
                    }
                }
                return JsonValue{value};
            }

            void parseLiteral(std::string_view literal)
            {
                for (char expected : literal)
                {
                    if (current_ == end_ || *current_++ != expected)
                    {
                        throw std::runtime_error("Invalid JSON literal");
                    }
                }
            }

            void skipWhitespace()
            {
                while (current_ != end_ && std::isspace(static_cast<unsigned char>(*current_)))
                {
                    ++current_;
                }
            }

            bool consume(char ch)
            {
                if (current_ != end_ && *current_ == ch)
                {
                    ++current_;
                    return true;
                }
                return false;
            }

            void expect(char ch)
            {
                if (current_ == end_ || *current_ != ch)
                {
                    throw std::runtime_error("Unexpected character in JSON stream");
                }
                ++current_;
            }

            std::string_view text_;
            const char *current_;
            const char *end_;
        };

        JsonValue parseJson(std::string_view json)
        {
            JsonParser parser(json);
            return parser.parse();
        }

        AttributeKind parseAttributeKind(std::string_view text)
        {
            if (text == "bool")
            {
                return AttributeKind::Bool;
            }
            if (text == "int")
            {
                return AttributeKind::Int;
            }
            if (text == "double")
            {
                return AttributeKind::Double;
            }
            if (text == "string" || text == "str")
            {
                return AttributeKind::String;
            }
            if (text == "bool_array" || text == "bool[]")
            {
                return AttributeKind::BoolArray;
            }
            if (text == "int_array" || text == "int[]")
            {
                return AttributeKind::IntArray;
            }
            if (text == "double_array" || text == "double[]")
            {
                return AttributeKind::DoubleArray;
            }
            if (text == "string_array" || text == "string[]")
            {
                return AttributeKind::StringArray;
            }
            throw std::runtime_error("Unknown attribute kind: " + std::string(text));
        }

        AttributeValue parseAttributeValue(const JsonValue &jsonAttr)
        {
            const auto &object = jsonAttr.asObject("attribute");
            auto kindIt = object.find("t");
            if (kindIt == object.end())
            {
                kindIt = object.find("k");
            }
            if (kindIt == object.end())
            {
                kindIt = object.find("kind");
            }
            if (kindIt == object.end())
            {
                throw std::runtime_error("Attribute object missing kind");
            }
            const char *kindContext = kindIt->first == "t"   ? "attribute.t"
                                     : kindIt->first == "k" ? "attribute.k"
                                                            : "attribute.kind";
            AttributeKind kind = parseAttributeKind(kindIt->second.asString(kindContext));

            auto valueIt = object.find("v");
            if (valueIt == object.end())
            {
                valueIt = object.find("value");
            }

            auto valuesIt = object.find("vs");
            if (valuesIt == object.end())
            {
                valuesIt = object.find("values");
            }

            auto validateAndReturn = [](AttributeValue attr) -> AttributeValue
            {
                if (!attributeValueIsJsonSerializable(attr))
                {
                    throw std::runtime_error("Attribute value is not JSON serializable");
                }
                return attr;
            };

            switch (kind)
            {
            case AttributeKind::Bool:
                if (valueIt == object.end())
                {
                    throw std::runtime_error("Bool attribute missing value");
                }
                return validateAndReturn(AttributeValue{valueIt->second.asBool("attribute.v")});
            case AttributeKind::Int:
                if (valueIt == object.end())
                {
                    throw std::runtime_error("Int attribute missing value");
                }
                return validateAndReturn(AttributeValue{valueIt->second.asInt("attribute.v")});
            case AttributeKind::Double:
                if (valueIt == object.end())
                {
                    throw std::runtime_error("Double attribute missing value");
                }
                return validateAndReturn(AttributeValue{valueIt->second.asDouble("attribute.v")});
            case AttributeKind::String:
                if (valueIt == object.end())
                {
                    throw std::runtime_error("String attribute missing value");
                }
                return validateAndReturn(AttributeValue{valueIt->second.asString("attribute.v")});
            case AttributeKind::BoolArray:
            {
                if (valuesIt == object.end())
                {
                    throw std::runtime_error("Bool array attribute missing values");
                }
                std::vector<bool> arr;
                for (const auto &entry : valuesIt->second.asArray("attribute.vs"))
                {
                    arr.push_back(entry.asBool("attribute.vs[]"));
                }
                return validateAndReturn(AttributeValue{arr});
            }
            case AttributeKind::IntArray:
            {
                if (valuesIt == object.end())
                {
                    throw std::runtime_error("Int array attribute missing values");
                }
                std::vector<int64_t> arr;
                for (const auto &entry : valuesIt->second.asArray("attribute.vs"))
                {
                    arr.push_back(entry.asInt("attribute.vs[]"));
                }
                return validateAndReturn(AttributeValue{arr});
            }
            case AttributeKind::DoubleArray:
            {
                if (valuesIt == object.end())
                {
                    throw std::runtime_error("Double array attribute missing values");
                }
                std::vector<double> arr;
                for (const auto &entry : valuesIt->second.asArray("attribute.vs"))
                {
                    arr.push_back(entry.asDouble("attribute.vs[]"));
                }
                return validateAndReturn(AttributeValue{arr});
            }
            case AttributeKind::StringArray:
            {
                if (valuesIt == object.end())
                {
                    throw std::runtime_error("String array attribute missing values");
                }
                std::vector<std::string> arr;
                for (const auto &entry : valuesIt->second.asArray("attribute.vs"))
                {
                    arr.push_back(entry.asString("attribute.vs[]"));
                }
                return validateAndReturn(AttributeValue{arr});
            }
            }
            throw std::runtime_error("Unhandled attribute kind");
        }

        std::optional<SrcLoc> parseSrcLoc(const JsonValue &json, std::string_view context)
        {
            const auto &object = json.asObject(context);
            SrcLoc info{};

            if (auto fileIt = object.find("file"); fileIt != object.end())
            {
                info.file = fileIt->second.asString(std::string(context) + ".file");
            }

            if (auto lineIt = object.find("line"); lineIt != object.end())
            {
                info.line = static_cast<uint32_t>(lineIt->second.asInt(std::string(context) + ".line"));
            }

            if (auto colIt = object.find("col"); colIt != object.end())
            {
                info.column = static_cast<uint32_t>(colIt->second.asInt(std::string(context) + ".col"));
            }

            if (auto endLineIt = object.find("endLine"); endLineIt != object.end())
            {
                info.endLine = static_cast<uint32_t>(endLineIt->second.asInt(std::string(context) + ".endLine"));
            }

            if (auto endColIt = object.find("endCol"); endColIt != object.end())
            {
                info.endColumn = static_cast<uint32_t>(endColIt->second.asInt(std::string(context) + ".endCol"));
            }

            if (auto originIt = object.find("origin"); originIt != object.end())
            {
                info.origin = originIt->second.asString(std::string(context) + ".origin");
            }

            if (auto passIt = object.find("pass"); passIt != object.end())
            {
                info.pass = passIt->second.asString(std::string(context) + ".pass");
            }

            if (auto noteIt = object.find("note"); noteIt != object.end())
            {
                info.note = noteIt->second.asString(std::string(context) + ".note");
            }

            if (info.file.empty() && info.line == 0 && info.column == 0 &&
                info.endLine == 0 && info.endColumn == 0 &&
                info.origin.empty() && info.pass.empty() && info.note.empty())
            {
                return std::nullopt;
            }
            return info;
        }

    } // namespace

    wolvrix::lib::grh::Design LoadJson::load(std::string_view json)
    {
        const bool timingEnabled = jsonTimingEnabled();
        const std::size_t progressStep = timingEnabled ? jsonTimingStep() : 0;
        TimingClock::time_point totalStart;
        if (timingEnabled)
        {
            totalStart = TimingClock::now();
        }

        TimingClock::time_point parseStart;
        if (timingEnabled)
        {
            parseStart = TimingClock::now();
        }
        JsonValue root = parseJson(json);
        if (timingEnabled)
        {
            const auto parseEnd = TimingClock::now();
            std::ostringstream details;
            details << "bytes=" << json.size();
            logJsonTiming("load_json.parse", toMillis(parseEnd - parseStart), details.str());
        }
        const auto &rootObj = root.asObject("design");

        Design design;
        auto graphsIt = rootObj.find("graphs");
        if (graphsIt == rootObj.end())
        {
            throw std::runtime_error("Design JSON missing graphs field");
        }
        const auto &graphsArray = graphsIt->second.asArray("graphs");
        std::size_t totalGraphs = 0;
        std::size_t totalDeclared = 0;
        std::size_t totalValues = 0;
        std::size_t totalOps = 0;
        std::size_t totalPortsIn = 0;
        std::size_t totalPortsOut = 0;
        std::size_t totalPortsInout = 0;
        TimingClock::time_point graphsStart;
        if (timingEnabled)
        {
            graphsStart = TimingClock::now();
        }
        for (const auto &graphValue : graphsArray)
        {
            TimingClock::time_point graphStart;
            TimingClock::time_point declaredEnd;
            TimingClock::time_point valuesEnd;
            TimingClock::time_point portsEnd;
            TimingClock::time_point opsEnd;
            if (timingEnabled)
            {
                graphStart = TimingClock::now();
            }
            const auto &graphObj = graphValue.asObject("graph");
            auto symbolIt = graphObj.find("symbol");
            if (symbolIt == graphObj.end())
            {
                throw std::runtime_error("Graph JSON missing symbol");
            }
            const std::string graphSymbol = symbolIt->second.asString("graph.symbol");
            if (graphSymbol.empty())
            {
                throw std::runtime_error("Graph JSON symbol is empty");
            }
            Graph &graph = design.createGraph(graphSymbol);
            ++totalGraphs;
            if (timingEnabled)
            {
                std::ostringstream details;
                details << "symbol=" << graphSymbol;
                logJsonTiming("load_json.graph.start", 0.0, details.str());
            }

            auto declaredIt = graphObj.find("declaredSymbols");
            if (declaredIt == graphObj.end())
            {
                throw std::runtime_error("Graph JSON missing declaredSymbols");
            }
            const auto &declaredArray = declaredIt->second.asArray("graph.declaredSymbols");

            const auto valuesIt = graphObj.find("vals");
            if (valuesIt == graphObj.end())
            {
                throw std::runtime_error("Graph JSON missing vals array");
            }
            const auto &valuesArray = valuesIt->second.asArray("graph.vals");

            const auto opsIt = graphObj.find("ops");
            const JsonValue::Array *opsArray = nullptr;
            if (opsIt != graphObj.end())
            {
                opsArray = &opsIt->second.asArray("graph.ops");
            }
            const std::size_t opReserve = opsArray ? opsArray->size() : 0;

            const std::size_t symbolReserve = declaredArray.size() + valuesArray.size() + opReserve + 64;
            graph.reserveSymbolCapacity(symbolReserve);
            graph.reserveDeclaredSymbolCapacity(declaredArray.size());
            graph.reserveValueCapacity(valuesArray.size());
            graph.reserveOperationCapacity(opReserve);

            std::size_t declaredCount = 0;
            for (const auto &entry : declaredArray)
            {
                const std::string name = entry.asString("graph.declaredSymbols[]");
                if (name.empty())
                {
                    throw std::runtime_error("Graph declared symbol is empty");
                }
                SymbolId sym = graph.lookupSymbol(name);
                if (!sym.valid())
                {
                    sym = graph.internSymbol(name);
                }
                if (!sym.valid())
                {
                    throw std::runtime_error("Declared symbol is already bound to value/operation: " + name);
                }
                graph.addDeclaredSymbol(sym);
                ++declaredCount;
            }
            totalDeclared += declaredCount;
            if (timingEnabled)
            {
                declaredEnd = TimingClock::now();
            }

            // generateGroups is an optional field: JSON written before its
            // introduction omits it entirely and loads as an empty set.
            if (auto groupsIt = graphObj.find("generateGroups"); groupsIt != graphObj.end())
            {
                auto symbolByText = [&](const std::string &text,
                                        std::string_view context) -> SymbolId
                {
                    if (text.empty())
                    {
                        throw std::runtime_error(std::string(context) + " symbol is empty");
                    }
                    SymbolId sym = graph.lookupSymbol(text);
                    if (!sym.valid())
                    {
                        sym = graph.internSymbol(text);
                    }
                    if (!sym.valid())
                    {
                        throw std::runtime_error(std::string(context) +
                                                 " symbol already bound to value/operation: " + text);
                    }
                    return sym;
                };
                const auto &groupsArray = groupsIt->second.asArray("graph.generateGroups");
                for (const auto &entry : groupsArray)
                {
                    const auto &groupObj = entry.asObject("graph.generateGroups[]");
                    const auto scopeIt = groupObj.find("scope");
                    const auto nameIt = groupObj.find("name");
                    const auto symbolsIt = groupObj.find("symbols");
                    if (scopeIt == groupObj.end() || nameIt == groupObj.end() ||
                        symbolsIt == groupObj.end())
                    {
                        throw std::runtime_error("Generate group entry missing required fields");
                    }
                    const SymbolId scope = symbolByText(
                        scopeIt->second.asString("generateGroups.scope"), "Generate group scope");
                    const SymbolId groupName = symbolByText(
                        nameIt->second.asString("generateGroups.name"), "Generate group name");
                    const std::size_t groupIndex = graph.addGenerateGroup(scope, groupName);
                    for (const auto &symEntry : symbolsIt->second.asArray("generateGroups.symbols"))
                    {
                        const SymbolId member = symbolByText(
                            symEntry.asString("generateGroups.symbols[]"),
                            "Generate group member");
                        graph.addGenerateGroupSymbol(groupIndex, member);
                    }
                }
            }

            std::unordered_map<std::string, ValueId> valueBySymbol;
            std::unordered_set<uint32_t> declaredInputs;
            std::unordered_set<uint32_t> declaredOutputs;
            std::unordered_set<uint32_t> declaredInouts;

            valueBySymbol.reserve(valuesArray.size());
            const std::size_t ioReserve = std::max<std::size_t>(32, valuesArray.size() / 32);
            declaredInputs.reserve(ioReserve);
            declaredOutputs.reserve(ioReserve);
            declaredInouts.reserve(ioReserve);

            std::size_t valuesCount = 0;
            TimingClock::time_point valuesStart;
            double valuesSymbolMs = 0.0;
            double valuesCreateMs = 0.0;
            double valuesLocMs = 0.0;
            std::size_t valuesLocCount = 0;
            if (timingEnabled)
            {
                valuesStart = TimingClock::now();
            }
            for (const auto &valueEntry : valuesArray)
            {
                const auto &valueObj = valueEntry.asObject("value");
                const auto symIt = valueObj.find("sym");
                const auto widthIt = valueObj.find("w");
                const auto signIt = valueObj.find("sgn");
                const auto inIt = valueObj.find("in");
                const auto outIt = valueObj.find("out");
                if (symIt == valueObj.end() || widthIt == valueObj.end() ||
                    signIt == valueObj.end() || inIt == valueObj.end() || outIt == valueObj.end())
                {
                    throw std::runtime_error("Value entry missing required fields");
                }

                const auto &symbol = symIt->second.asString("value.sym");
                if (symbol.empty())
                {
                    throw std::runtime_error("Value symbol is empty");
                }
                int64_t width = widthIt->second.asInt("value.w");
                bool isSigned = signIt->second.asBool("value.sgn");
                wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
                if (auto typeIt = valueObj.find("type"); typeIt != valueObj.end())
                {
                    const std::string &typeName = typeIt->second.asString("value.type");
                    if (auto parsed = wolvrix::lib::grh::parseValueType(typeName))
                    {
                        valueType = *parsed;
                    }
                    else
                    {
                        throw std::runtime_error("Unknown value type in JSON: " + typeName);
                    }
                }
                TimingClock::time_point valueSymbolStart;
                if (timingEnabled)
                {
                    valueSymbolStart = TimingClock::now();
                }
                SymbolId valueSym = graph.internSymbol(symbol);
                if (timingEnabled)
                {
                    const auto now = TimingClock::now();
                    valuesSymbolMs += toMillis(now - valueSymbolStart);
                }
                if (!valueSym.valid())
                {
                    throw std::runtime_error("Value symbol already bound to value/operation: " + symbol);
                }
                TimingClock::time_point valueCreateStart;
                if (timingEnabled)
                {
                    valueCreateStart = TimingClock::now();
                }
                ValueId valueId = graph.createValue(valueSym, static_cast<int32_t>(width), isSigned, valueType);
                if (timingEnabled)
                {
                    const auto now = TimingClock::now();
                    valuesCreateMs += toMillis(now - valueCreateStart);
                }
                valueBySymbol.emplace(symbol, valueId);

                bool isInput = inIt->second.asBool("value.in");
                bool isOutput = outIt->second.asBool("value.out");
                bool isInout = false;
                if (auto inoutIt = valueObj.find("inout"); inoutIt != valueObj.end())
                {
                    isInout = inoutIt->second.asBool("value.inout");
                }
                if (isInout && (isInput || isOutput))
                {
                    throw std::runtime_error("Value cannot be both inout and input/output in JSON");
                }

                if (isInput)
                {
                    declaredInputs.insert(valueSym.value);
                }
                if (isOutput)
                {
                    declaredOutputs.insert(valueSym.value);
                }
                if (isInout)
                {
                    declaredInouts.insert(valueSym.value);
                }

                if (auto dbgIt = valueObj.find("loc"); dbgIt != valueObj.end())
                {
                    TimingClock::time_point valueLocStart;
                    if (timingEnabled)
                    {
                        valueLocStart = TimingClock::now();
                    }
                    if (auto loc = parseSrcLoc(dbgIt->second, "value.loc"))
                    {
                        graph.setValueSrcLoc(valueId, std::move(*loc));
                    }
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        valuesLocMs += toMillis(now - valueLocStart);
                        ++valuesLocCount;
                    }
                }
                ++valuesCount;
                if (progressStep > 0 && (valuesCount % progressStep) == 0)
                {
                    const auto now = TimingClock::now();
                    std::ostringstream details;
                    details << "symbol=" << graphSymbol
                            << " count=" << valuesCount
                            << " elapsed=" << toMillis(now - valuesStart) << "ms";
                    logJsonTiming("load_json.values.progress", toMillis(now - valuesStart), details.str());
                }
            }
            totalValues += valuesCount;
            if (timingEnabled)
            {
                valuesEnd = TimingClock::now();
            }

            std::size_t portsInCount = 0;
            std::size_t portsOutCount = 0;
            std::size_t portsInoutCount = 0;
            double portsInParseMs = 0.0;
            double portsOutParseMs = 0.0;
            double portsInoutParseMs = 0.0;
            double portsInBindMs = 0.0;
            double portsOutBindMs = 0.0;
            double portsInoutBindMs = 0.0;
            if (auto portsIt = graphObj.find("ports"); portsIt != graphObj.end())
            {
                const auto &portsObj = portsIt->second.asObject("graph.ports");
                std::vector<Port> inputPorts;
                std::vector<Port> outputPorts;
                std::vector<InoutPort> inoutPorts;
                auto parsePortArray = [&](std::string_view key, bool isInput)
                {
                    auto arrayIt = portsObj.find(key);
                    if (arrayIt == portsObj.end())
                    {
                        return;
                    }
                    const std::string context = std::string("graph.ports.") + std::string(key);
                    const auto &portArray = arrayIt->second.asArray(context);
                    if (isInput)
                    {
                        inputPorts.reserve(inputPorts.size() + portArray.size());
                    }
                    else
                    {
                        outputPorts.reserve(outputPorts.size() + portArray.size());
                    }
                    for (const auto &entry : portArray)
                    {
                        const auto &portObj = entry.asObject("graph.port");
                        TimingClock::time_point portStart;
                        if (timingEnabled)
                        {
                            portStart = TimingClock::now();
                        }
                        auto nameField = portObj.find("name");
                        auto valField = portObj.find("val");
                        if (nameField == portObj.end() || valField == portObj.end())
                        {
                            throw std::runtime_error("Port entry missing name or val");
                        }
                        const std::string &portNameText = nameField->second.asString("graph.port.name");
                        if (portNameText.empty())
                        {
                            throw std::runtime_error("Port name is empty");
                        }
                        const std::string &valueName = valField->second.asString("graph.port.val");
                        if (valueName.empty())
                        {
                            throw std::runtime_error("Port value symbol is empty");
                        }
                        auto valueIt = valueBySymbol.find(valueName);
                        if (valueIt == valueBySymbol.end())
                        {
                            throw std::runtime_error("Port references unknown value: " + valueName);
                        }
                        if (isInput)
                        {
                            inputPorts.push_back(Port{portNameText, valueIt->second});
                        }
                        else
                        {
                            outputPorts.push_back(Port{portNameText, valueIt->second});
                        }
                        if (timingEnabled)
                        {
                            const auto now = TimingClock::now();
                            const double delta = toMillis(now - portStart);
                            if (isInput)
                            {
                                portsInParseMs += delta;
                            }
                            else
                            {
                                portsOutParseMs += delta;
                            }
                        }
                    }
                };
                parsePortArray("in", true);
                parsePortArray("out", false);

                auto inoutIt = portsObj.find("inout");
                if (inoutIt != portsObj.end())
                {
                    const auto &portArray = inoutIt->second.asArray("graph.ports.inout");
                    inoutPorts.reserve(inoutPorts.size() + portArray.size());
                    for (const auto &entry : portArray)
                    {
                        const auto &portObj = entry.asObject("graph.inout_port");
                        TimingClock::time_point portStart;
                        if (timingEnabled)
                        {
                            portStart = TimingClock::now();
                        }
                        auto nameField = portObj.find("name");
                        auto inField = portObj.find("in");
                        auto outField = portObj.find("out");
                        auto oeField = portObj.find("oe");
                        if (nameField == portObj.end() || inField == portObj.end() ||
                            outField == portObj.end() || oeField == portObj.end())
                        {
                            throw std::runtime_error("Inout port entry missing name/in/out/oe");
                        }
                        const std::string &portNameText = nameField->second.asString("graph.port.name");
                        if (portNameText.empty())
                        {
                            throw std::runtime_error("Inout port name is empty");
                        }
                        const std::string &inName = inField->second.asString("graph.port.in");
                        const std::string &outName = outField->second.asString("graph.port.out");
                        const std::string &oeName = oeField->second.asString("graph.port.oe");
                        if (inName.empty() || outName.empty() || oeName.empty())
                        {
                            throw std::runtime_error("Inout port value symbol is empty");
                        }
                        auto inIt = valueBySymbol.find(inName);
                        auto outIt = valueBySymbol.find(outName);
                        auto oeIt = valueBySymbol.find(oeName);
                        if (inIt == valueBySymbol.end() || outIt == valueBySymbol.end() ||
                            oeIt == valueBySymbol.end())
                        {
                            throw std::runtime_error("Inout port references unknown value");
                        }
                        inoutPorts.push_back(InoutPort{portNameText, inIt->second, outIt->second, oeIt->second});
                        if (timingEnabled)
                        {
                            const auto now = TimingClock::now();
                            portsInoutParseMs += toMillis(now - portStart);
                        }
                    }
                }

                portsInCount = inputPorts.size();
                portsOutCount = outputPorts.size();
                portsInoutCount = inoutPorts.size();

                if (!inputPorts.empty())
                {
                    TimingClock::time_point bindStart;
                    if (timingEnabled)
                    {
                        bindStart = TimingClock::now();
                    }
                    graph.bindInputPorts(inputPorts);
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        portsInBindMs = toMillis(now - bindStart);
                    }
                }
                if (!outputPorts.empty())
                {
                    TimingClock::time_point bindStart;
                    if (timingEnabled)
                    {
                        bindStart = TimingClock::now();
                    }
                    graph.bindOutputPorts(outputPorts);
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        portsOutBindMs = toMillis(now - bindStart);
                    }
                }
                if (!inoutPorts.empty())
                {
                    TimingClock::time_point bindStart;
                    if (timingEnabled)
                    {
                        bindStart = TimingClock::now();
                    }
                    graph.bindInoutPorts(inoutPorts);
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        portsInoutBindMs = toMillis(now - bindStart);
                    }
                }
            }
            else
            {
                throw std::runtime_error("Graph JSON missing ports object");
            }

            for (const auto &port : graph.inputPorts())
            {
                Value value = graph.getValue(port.value);
                if (!declaredInputs.contains(value.symbol().value))
                {
                    throw std::runtime_error("Input port missing isInput=true flag for value: " + std::string(graph.symbolText(value.symbol())));
                }
            }
            for (const auto &symbolValue : declaredInputs)
            {
                SymbolId sym{symbolValue};
                ValueId valueId = graph.findValue(sym);
                if (!valueId.valid())
                {
                    throw std::runtime_error("Value marked in=true but not bound to input port");
                }
                Value value = graph.getValue(valueId);
                if (!value.isInput())
                {
                    throw std::runtime_error("Value marked in=true but not bound to input port: " + std::string(graph.symbolText(sym)));
                }
            }

            for (const auto &port : graph.outputPorts())
            {
                Value value = graph.getValue(port.value);
                if (!declaredOutputs.contains(value.symbol().value))
                {
                    throw std::runtime_error("Output port missing isOutput=true flag for value: " + std::string(graph.symbolText(value.symbol())));
                }
            }
            for (const auto &symbolValue : declaredOutputs)
            {
                SymbolId sym{symbolValue};
                ValueId valueId = graph.findValue(sym);
                if (!valueId.valid())
                {
                    throw std::runtime_error("Value marked out=true but not bound to output port");
                }
                Value value = graph.getValue(valueId);
                if (!value.isOutput())
                {
                    throw std::runtime_error("Value marked out=true but not bound to output port: " + std::string(graph.symbolText(sym)));
                }
            }

            for (const auto &port : graph.inoutPorts())
            {
                Value inValue = graph.getValue(port.in);
                Value outValue = graph.getValue(port.out);
                Value oeValue = graph.getValue(port.oe);
                if (inValue.isInput() || inValue.isOutput() || outValue.isInput() ||
                    outValue.isOutput() || oeValue.isInput() || oeValue.isOutput())
                {
                    throw std::runtime_error("Inout port values must not be marked as input/output");
                }
                if (!inValue.isInout() || !outValue.isInout() || !oeValue.isInout())
                {
                    throw std::runtime_error("Inout port values must be marked as inout");
                }
            }

            for (const auto &symbolValue : declaredInouts)
            {
                SymbolId sym{symbolValue};
                ValueId valueId = graph.findValue(sym);
                if (!valueId.valid())
                {
                    throw std::runtime_error("Value marked inout=true not found in graph");
                }
                bool seen = false;
                for (const auto &port : graph.inoutPorts())
                {
                    if (port.in == valueId || port.out == valueId || port.oe == valueId)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    throw std::runtime_error("Value marked inout=true but not bound to inout port");
                }
            }
            totalPortsIn += portsInCount;
            totalPortsOut += portsOutCount;
            totalPortsInout += portsInoutCount;
            if (timingEnabled)
            {
                portsEnd = TimingClock::now();
            }

            std::size_t opsCount = 0;
            std::size_t opsInRefs = 0;
            std::size_t opsOutRefs = 0;
            std::size_t opsAttrCount = 0;
            double opsSymbolMs = 0.0;
            double opsCreateMs = 0.0;
            double opsInputsMs = 0.0;
            double opsOutputsMs = 0.0;
            double opsAttrsMs = 0.0;
            double opsLocMs = 0.0;
            TimingClock::time_point opsStart;
            if (timingEnabled)
            {
                opsStart = TimingClock::now();
            }
            if (opsArray)
            {
                for (const auto &opEntry : *opsArray)
                {
                    const auto &opObj = opEntry.asObject("operation");
                    auto kindIt = opObj.find("kind");
                    if (kindIt == opObj.end())
                    {
                        throw std::runtime_error("Operation missing kind");
                    }
                    auto kind = parseOperationKind(kindIt->second.asString("operation.kind"));
                    if (!kind)
                    {
                        throw std::runtime_error("Unknown operation kind: " + kindIt->second.asString("operation.kind"));
                    }

                    auto symIt = opObj.find("sym");
                    if (symIt == opObj.end())
                    {
                        throw std::runtime_error("Operation missing symbol");
                    }

                    const std::string &opSymbol = symIt->second.asString("operation.sym");
                    if (opSymbol.empty())
                    {
                        throw std::runtime_error("Operation symbol is empty");
                    }
                    TimingClock::time_point opSymbolStart;
                    if (timingEnabled)
                    {
                        opSymbolStart = TimingClock::now();
                    }
                    SymbolId opSym = graph.internSymbol(opSymbol);
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        opsSymbolMs += toMillis(now - opSymbolStart);
                    }
                    if (!opSym.valid())
                    {
                        throw std::runtime_error("Operation symbol already bound to value/operation: " + opSymbol);
                    }
                    TimingClock::time_point opCreateStart;
                    if (timingEnabled)
                    {
                        opCreateStart = TimingClock::now();
                    }
                    OperationId opId = graph.createOperation(*kind, opSym);
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        opsCreateMs += toMillis(now - opCreateStart);
                    }

                    const JsonValue::Array *inputsArray = nullptr;
                    if (auto inIt = opObj.find("in"); inIt != opObj.end())
                    {
                        inputsArray = &inIt->second.asArray("operation.in");
                        graph.reserveOpOperandCapacity(opId, inputsArray->size());
                    }

                    TimingClock::time_point opInputsStart;
                    if (timingEnabled)
                    {
                        opInputsStart = TimingClock::now();
                    }
                    if (inputsArray)
                    {
                        for (const auto &entry : *inputsArray)
                        {
                            const std::string &symbol = entry.asString("operation.in[]");
                            auto valueIt = valueBySymbol.find(symbol);
                            if (valueIt == valueBySymbol.end())
                            {
                                throw std::runtime_error("Operand references unknown value: " + symbol);
                            }
                            graph.addOperand(opId, valueIt->second);
                            ++opsInRefs;
                        }
                    }
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        opsInputsMs += toMillis(now - opInputsStart);
                    }

                    const JsonValue::Array *outputsArray = nullptr;
                    if (auto outIt = opObj.find("out"); outIt != opObj.end())
                    {
                        outputsArray = &outIt->second.asArray("operation.out");
                        graph.reserveOpResultCapacity(opId, outputsArray->size());
                    }

                    TimingClock::time_point opOutputsStart;
                    if (timingEnabled)
                    {
                        opOutputsStart = TimingClock::now();
                    }
                    if (outputsArray)
                    {
                        for (const auto &entry : *outputsArray)
                        {
                            const std::string &symbol = entry.asString("operation.out[]");
                            auto valueIt = valueBySymbol.find(symbol);
                            if (valueIt == valueBySymbol.end())
                            {
                                throw std::runtime_error("Result references unknown value: " + symbol);
                            }
                            graph.addResult(opId, valueIt->second);
                            ++opsOutRefs;
                        }
                    }
                    if (timingEnabled)
                    {
                        const auto now = TimingClock::now();
                        opsOutputsMs += toMillis(now - opOutputsStart);
                    }

                    if (auto attrsIt = opObj.find("attrs"); attrsIt != opObj.end())
                    {
                        TimingClock::time_point opAttrsStart;
                        if (timingEnabled)
                        {
                            opAttrsStart = TimingClock::now();
                        }
                        const auto &attrsObj = attrsIt->second.asObject("operation.attrs");
                        graph.reserveOpAttrCapacity(opId, attrsObj.size());
                        for (const auto &[attrName, attrValue] : attrsObj)
                        {
                            graph.setAttr(opId, attrName, parseAttributeValue(attrValue));
                            ++opsAttrCount;
                        }
                        if (timingEnabled)
                        {
                            const auto now = TimingClock::now();
                            opsAttrsMs += toMillis(now - opAttrsStart);
                        }
                    }

                    if (auto dbgIt = opObj.find("loc"); dbgIt != opObj.end())
                    {
                        TimingClock::time_point opLocStart;
                        if (timingEnabled)
                        {
                            opLocStart = TimingClock::now();
                        }
                        if (auto loc = parseSrcLoc(dbgIt->second, "operation.loc"))
                        {
                            graph.setOpSrcLoc(opId, std::move(*loc));
                        }
                        if (timingEnabled)
                        {
                            const auto now = TimingClock::now();
                            opsLocMs += toMillis(now - opLocStart);
                        }
                    }
                    ++opsCount;
                    if (progressStep > 0 && (opsCount % progressStep) == 0)
                    {
                        const auto now = TimingClock::now();
                        std::ostringstream details;
                        details << "symbol=" << graphSymbol
                                << " count=" << opsCount
                                << " elapsed=" << toMillis(now - opsStart) << "ms";
                        logJsonTiming("load_json.ops.progress", toMillis(now - opsStart), details.str());
                    }
                }
            }
            totalOps += opsCount;
            if (timingEnabled)
            {
                opsEnd = TimingClock::now();
                std::ostringstream details;
                details << "symbol=" << graphSymbol
                        << " declared=" << declaredCount
                        << " values=" << valuesCount
                        << " ops=" << opsCount
                        << " ports_in=" << portsInCount
                        << " ports_out=" << portsOutCount
                        << " ports_inout=" << portsInoutCount
                        << " phase_ms=decl:" << toMillis(declaredEnd - graphStart)
                        << ",vals:" << toMillis(valuesEnd - declaredEnd)
                        << ",ports:" << toMillis(portsEnd - valuesEnd)
                        << ",ops:" << toMillis(opsEnd - portsEnd)
                        << " values_ms=symbol:" << valuesSymbolMs
                        << ",create:" << valuesCreateMs
                        << ",loc:" << valuesLocMs
                        << " values_loc=" << valuesLocCount
                        << " ports_ms=parse_in:" << portsInParseMs
                        << ",parse_out:" << portsOutParseMs
                        << ",parse_inout:" << portsInoutParseMs
                        << ",bind_in:" << portsInBindMs
                        << ",bind_out:" << portsOutBindMs
                        << ",bind_inout:" << portsInoutBindMs
                        << " ops_ms=sym:" << opsSymbolMs
                        << ",create:" << opsCreateMs
                        << ",in:" << opsInputsMs
                        << ",out:" << opsOutputsMs
                        << ",attrs:" << opsAttrsMs
                        << ",loc:" << opsLocMs
                        << " ops_refs=in:" << opsInRefs
                        << ",out:" << opsOutRefs
                        << " ops_attrs=" << opsAttrCount;
                logJsonTiming("load_json.graph", toMillis(opsEnd - graphStart), details.str());
            }
        }
        if (timingEnabled)
        {
            const auto graphsEnd = TimingClock::now();
            std::ostringstream details;
            details << "graphs=" << totalGraphs
                    << " declared=" << totalDeclared
                    << " values=" << totalValues
                    << " ops=" << totalOps
                    << " ports_in=" << totalPortsIn
                    << " ports_out=" << totalPortsOut
                    << " ports_inout=" << totalPortsInout;
            logJsonTiming("load_json.graphs", toMillis(graphsEnd - graphsStart), details.str());
        }

        TimingClock::time_point designStart;
        if (timingEnabled)
        {
            designStart = TimingClock::now();
        }
        auto declaredIt = rootObj.find("declaredSymbols");
        if (declaredIt == rootObj.end())
        {
            throw std::runtime_error("Design JSON missing declaredSymbols");
        }
        std::size_t designDeclared = 0;
        for (const auto &entry : declaredIt->second.asArray("design.declaredSymbols"))
        {
            const std::string name = entry.asString("design.declaredSymbols[]");
            if (name.empty())
            {
                throw std::runtime_error("Design declared symbol is empty");
            }
            design.addDeclaredSymbol(design.internSymbol(name));
            ++designDeclared;
        }

        std::size_t designAliases = 0;
        if (auto aliasesIt = rootObj.find("aliases"); aliasesIt != rootObj.end())
        {
            for (const auto &[alias, value] : aliasesIt->second.asObject("design.aliases"))
            {
                if (alias.empty())
                {
                    throw std::runtime_error("Design alias is empty");
                }
                const std::string target = value.asString("design.aliases[]");
                if (target.empty())
                {
                    throw std::runtime_error("Design alias target is empty");
                }
                wolvrix::lib::grh::Graph *graph = design.findGraph(target);
                if (!graph)
                {
                    throw std::runtime_error("Design alias target graph not found: " + target);
                }
                design.registerGraphAlias(alias, *graph);
                ++designAliases;
            }
        }

        std::size_t designTops = 0;
        if (auto topIt = rootObj.find("tops"); topIt != rootObj.end())
        {
            for (const auto &entry : topIt->second.asArray("design.tops"))
            {
                design.markAsTop(entry.asString("design.tops[]"));
                ++designTops;
            }
        }

        if (timingEnabled)
        {
            const auto designEnd = TimingClock::now();
            std::ostringstream details;
            details << "declared=" << designDeclared
                    << " aliases=" << designAliases
                    << " tops=" << designTops;
            logJsonTiming("load_json.design", toMillis(designEnd - designStart), details.str());
            const auto totalEnd = TimingClock::now();
            logJsonTiming("load_json.total", toMillis(totalEnd - totalStart));
        }

        return design;
    }

} // namespace wolvrix::lib::load

namespace wolvrix::lib::grh
{

    Design Design::fromJsonString(std::string_view json)
    {
        wolvrix::lib::load::LoadJson loader;
        return loader.load(json);
    }

} // namespace wolvrix::lib::grh
