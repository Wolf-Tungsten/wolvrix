#include "grhsim/io/json.hpp"

#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        class StreamWriter
        {
        public:
            StreamWriter(std::ostream &output, bool pretty) : output_(output), pretty_(pretty) {}

            void startObject() { beforeValue(); output_ << '{'; stack_.push_back({'o', true, false}); }
            void endObject()
            {
                if (stack_.empty() || stack_.back().kind != 'o' || stack_.back().expectValue)
                    throw std::logic_error("invalid JSON object writer state");
                const bool empty = stack_.back().first;
                stack_.pop_back();
                if (pretty_ && !empty) newline();
                output_ << '}';
            }
            void startArray() { beforeValue(); output_ << '['; stack_.push_back({'a', true, false}); }
            void endArray()
            {
                if (stack_.empty() || stack_.back().kind != 'a')
                    throw std::logic_error("invalid JSON array writer state");
                const bool empty = stack_.back().first;
                stack_.pop_back();
                if (pretty_ && !empty) newline();
                output_ << ']';
            }
            void key(std::string_view name)
            {
                if (stack_.empty() || stack_.back().kind != 'o' || stack_.back().expectValue)
                    throw std::logic_error("invalid JSON property writer state");
                Context &context = stack_.back();
                if (!context.first) output_ << ',';
                context.first = false;
                if (pretty_) newline();
                quote(name);
                output_ << (pretty_ ? ": " : ":");
                context.expectValue = true;
            }
            void value(std::string_view value) { beforeValue(); quote(value); }
            void value(const char *value) { this->value(std::string_view(value)); }
            void value(bool value) { beforeValue(); output_ << (value ? "true" : "false"); }
            void value(int64_t value) { beforeValue(); output_ << value; }
            void value(uint64_t value) { beforeValue(); output_ << value; }
            void value(double value)
            {
                if (!std::isfinite(value)) throw std::runtime_error("cannot serialize non-finite number");
                beforeValue();
                output_ << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
            }

        private:
            struct Context { char kind; bool first; bool expectValue; };

            void beforeValue()
            {
                if (stack_.empty())
                {
                    if (hasRoot_) throw std::logic_error("multiple JSON root values");
                    hasRoot_ = true;
                    return;
                }
                Context &context = stack_.back();
                if (context.kind == 'o')
                {
                    if (!context.expectValue) throw std::logic_error("JSON object value without property");
                    context.expectValue = false;
                    return;
                }
                if (!context.first) output_ << ',';
                context.first = false;
                if (pretty_) newline();
            }
            void newline()
            {
                output_ << '\n';
                for (std::size_t i = 0; i < stack_.size(); ++i) output_ << "  ";
            }
            void quote(std::string_view text)
            {
                output_ << '"';
                for (unsigned char ch : text)
                {
                    switch (ch)
                    {
                    case '"': output_ << "\\\""; break;
                    case '\\': output_ << "\\\\"; break;
                    case '\b': output_ << "\\b"; break;
                    case '\f': output_ << "\\f"; break;
                    case '\n': output_ << "\\n"; break;
                    case '\r': output_ << "\\r"; break;
                    case '\t': output_ << "\\t"; break;
                    default:
                        if (ch < 0x20)
                        {
                            char buffer[7];
                            std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                            output_ << buffer;
                        }
                        else output_ << static_cast<char>(ch);
                    }
                }
                output_ << '"';
            }

            std::ostream &output_;
            bool pretty_ = false;
            bool hasRoot_ = false;
            std::vector<Context> stack_;
        };

        class StreamReader
        {
        public:
            explicit StreamReader(std::istream &input) : input_(input) {}

            void startObject() { expect('{'); }
            void endObject() { expect('}'); }
            void startArray() { expect('['); }
            void endArray() { expect(']'); }
            bool comma() { return consume(','); }
            // Peeks whether the next value opens an array (no consume).
            bool arrayStarts()
            {
                skipWhitespace();
                return input_.peek() == '[';
            }
            bool nextArray(bool &first)
            {
                skipWhitespace();
                if (first)
                {
                    first = false;
                    if (consume(']')) return false;
                    return true;
                }
                if (consume(']')) return false;
                expect(',');
                return true;
            }
            void key(std::string_view expected, bool first)
            {
                if (!first) expect(',');
                const std::string actual = string();
                if (actual != expected)
                    fail("expected property '" + std::string(expected) + "', found '" + actual + "'");
                expect(':');
            }
            std::string string()
            {
                skipWhitespace();
                if (input_.get() != '"') fail("expected JSON string");
                std::string result;
                while (true)
                {
                    const int next = input_.get();
                    if (next == EOF) fail("unexpected end of JSON string");
                    const unsigned char ch = static_cast<unsigned char>(next);
                    if (ch == '"') return result;
                    if (ch < 0x20) fail("unescaped control character in JSON string");
                    if (ch != '\\')
                    {
                        result.push_back(static_cast<char>(ch));
                        continue;
                    }
                    const int escape = input_.get();
                    if (escape == EOF) fail("unexpected end of JSON escape");
                    switch (escape)
                    {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u': appendUnicodeEscape(result); break;
                    default: fail("invalid JSON escape sequence");
                    }
                }
            }
            bool boolean()
            {
                const std::string token = literalToken();
                if (token == "true") return true;
                if (token == "false") return false;
                fail("expected JSON boolean");
            }
            int64_t integer()
            {
                const std::string token = numberToken();
                int64_t result = 0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
                if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size())
                    fail("expected signed integer");
                return result;
            }
            uint64_t unsignedInteger()
            {
                const std::string token = numberToken();
                uint64_t result = 0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
                if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size())
                    fail("expected unsigned integer");
                return result;
            }
            double number()
            {
                const std::string token = numberToken();
                double result = 0.0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), result);
                if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size() ||
                    !std::isfinite(result))
                    fail("expected finite JSON number");
                return result;
            }
            uint32_t index(std::string_view context, bool optional = false)
            {
                const uint64_t value = unsignedInteger();
                if ((!optional && value == 0) || value > std::numeric_limits<uint32_t>::max())
                    fail(std::string(context) + " is outside the valid ID range");
                return static_cast<uint32_t>(value);
            }
            void finish()
            {
                skipWhitespace();
                if (input_.peek() != EOF) fail("trailing characters after JSON document");
            }

            void expect(char expected)
            {
                skipWhitespace();
                const int actual = input_.get();
                ++offset_;
                if (actual != expected)
                    fail(std::string("expected '") + expected + "'");
            }

        private:
            [[noreturn]] void fail(const std::string &message) const
            {
                throw std::runtime_error(message + " at byte " + std::to_string(offset_));
            }
            void skipWhitespace()
            {
                while (true)
                {
                    const int ch = input_.peek();
                    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return;
                    input_.get();
                    ++offset_;
                }
            }
            bool consume(char expected)
            {
                skipWhitespace();
                if (input_.peek() != expected) return false;
                input_.get();
                ++offset_;
                return true;
            }
            std::string literalToken()
            {
                skipWhitespace();
                std::string result;
                while (true)
                {
                    const int ch = input_.peek();
                    if (ch == EOF || ch == ',' || ch == ']' || ch == '}' ||
                        ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                        break;
                    result.push_back(static_cast<char>(input_.get()));
                    ++offset_;
                }
                return result;
            }
            std::string numberToken()
            {
                const std::string token = literalToken();
                if (token.empty()) fail("expected JSON number");
                return token;
            }
            uint32_t hex4()
            {
                uint32_t value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const int ch = input_.get();
                    ++offset_;
                    if (ch == EOF) fail("unexpected end of Unicode escape");
                    value <<= 4;
                    if (ch >= '0' && ch <= '9') value |= static_cast<uint32_t>(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint32_t>(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint32_t>(ch - 'A' + 10);
                    else fail("invalid Unicode escape");
                }
                return value;
            }
            static void appendUtf8(std::string &out, uint32_t codepoint)
            {
                if (codepoint <= 0x7f) out.push_back(static_cast<char>(codepoint));
                else if (codepoint <= 0x7ff)
                {
                    out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                }
                else if (codepoint <= 0xffff)
                {
                    out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
                }
            }
            void appendUnicodeEscape(std::string &out)
            {
                uint32_t codepoint = hex4();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff)
                {
                    if (input_.get() != '\\' || input_.get() != 'u')
                        fail("high surrogate is not followed by a low surrogate");
                    offset_ += 2;
                    const uint32_t low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail("invalid low surrogate");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                }
                else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
                    fail("unpaired low surrogate");
                appendUtf8(out, codepoint);
            }

            std::istream &input_;
            std::size_t offset_ = 0;
        };

        template <typename IdType>
        void writeId(StreamWriter &writer, IdType id)
        {
            writer.value(static_cast<uint64_t>(id.index));
        }

        template <typename IdType>
        IdType readId(StreamReader &reader, std::string_view context, bool optional = false)
        {
            return IdType{reader.index(context, optional), 0};
        }

        void writeParameterValue(StreamWriter &writer, const ParameterValue &value)
        {
            std::visit([&](const auto &entry) {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    writer.value("bool"); writer.value(entry);
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    writer.value("int"); writer.value(entry);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    writer.value("double"); writer.value(entry);
                }
                else if constexpr (std::is_same_v<T, std::string>)
                {
                    writer.value("string"); writer.value(entry);
                }
                else
                {
                    if constexpr (std::is_same_v<T, std::vector<bool>>) writer.value("bools");
                    else if constexpr (std::is_same_v<T, std::vector<int64_t>>) writer.value("ints");
                    else if constexpr (std::is_same_v<T, std::vector<double>>) writer.value("doubles");
                    else writer.value("strings");
                    writer.startArray();
                    for (const auto &item : entry)
                    {
                        if constexpr (std::is_same_v<T, std::vector<std::string>>) writer.value(item);
                        else writer.value(item);
                    }
                    writer.endArray();
                }
            }, value);
        }

        void writeParameter(StreamWriter &writer, const Parameter &parameter)
        {
            writer.startArray();
            writeId(writer, parameter.name);
            writeParameterValue(writer, parameter.value);
            writer.endArray();
        }

        ParameterValue readParameterValue(StreamReader &reader, std::string tag)
        {
            if (tag == "bool") return reader.boolean();
            if (tag == "int") return reader.integer();
            if (tag == "double") return reader.number();
            if (tag == "string") return reader.string();
            bool first = true;
            reader.startArray();
            if (tag == "bools")
            {
                std::vector<bool> values;
                while (reader.nextArray(first)) values.push_back(reader.boolean());
                return values;
            }
            if (tag == "ints")
            {
                std::vector<int64_t> values;
                while (reader.nextArray(first)) values.push_back(reader.integer());
                return values;
            }
            if (tag == "doubles")
            {
                std::vector<double> values;
                while (reader.nextArray(first)) values.push_back(reader.number());
                return values;
            }
            if (tag == "strings")
            {
                std::vector<std::string> values;
                while (reader.nextArray(first)) values.push_back(reader.string());
                return values;
            }
            throw std::runtime_error("unknown parameter value tag: " + tag);
        }

        Parameter readParameter(StreamReader &reader)
        {
            reader.startArray();
            StringId name = readId<StringId>(reader, "parameter name");
            reader.expect(',');
            const std::string tag = reader.string();
            reader.expect(',');
            ParameterValue value = readParameterValue(reader, tag);
            reader.endArray();
            return Parameter{name, std::move(value)};
        }

        void writeParameterArray(StreamWriter &writer, std::span<const Parameter> parameters)
        {
            writer.startArray();
            for (const Parameter &parameter : parameters) writeParameter(writer, parameter);
            writer.endArray();
        }

        std::vector<Parameter> readParameterArray(StreamReader &reader)
        {
            std::vector<Parameter> parameters;
            bool first = true;
            reader.startArray();
            while (reader.nextArray(first)) parameters.push_back(readParameter(reader));
            return parameters;
        }

        template <typename IdType>
        void writeIdArray(StreamWriter &writer, std::span<const IdType> ids)
        {
            writer.startArray();
            for (IdType id : ids) writeId(writer, id);
            writer.endArray();
        }

        template <typename IdType>
        std::vector<IdType> readIdArray(StreamReader &reader, std::string_view context)
        {
            std::vector<IdType> ids;
            bool first = true;
            reader.startArray();
            while (reader.nextArray(first)) ids.push_back(readId<IdType>(reader, context));
            return ids;
        }

        void writeObjectRefs(StreamWriter &writer, std::span<const ObjectRef> refs)
        {
            writer.startArray();
            for (ObjectRef ref : refs)
            {
                writer.startArray();
                writer.value(toString(ref.kind));
                writer.value(static_cast<uint64_t>(ref.index));
                writer.endArray();
            }
            writer.endArray();
        }

        std::vector<ObjectRef> readObjectRefs(StreamReader &reader)
        {
            std::vector<ObjectRef> refs;
            bool first = true;
            reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray();
                const auto kind = parseObjectKind(reader.string());
                if (!kind) throw std::runtime_error("unknown object reference kind");
                reader.expect(',');
                const uint32_t index = reader.index("object reference");
                reader.endArray();
                refs.push_back(ObjectRef{*kind, index, 0});
            }
            return refs;
        }

        void expectComma(StreamReader &reader) { reader.expect(','); }

        void writeCpuLayout(StreamWriter &writer, const CpuDataLayout &layout)
        {
            // M5d-6 shape: [pointerBytes, [types...], [namedStores...]?]. The
            // legacy object/value/frame/runtime arenas are gone.
            writer.startArray(); writer.value(static_cast<uint64_t>(layout.pointerBytes));
            writer.startArray();
            for (const auto &type : layout.types)
            {
                writer.startArray(); writeId(writer, type.id);
                writer.value(static_cast<uint64_t>(type.kind)); writer.value(static_cast<uint64_t>(type.width));
                writeId(writer, type.elementType); writer.value(type.count); writer.value(type.size);
                writer.value(static_cast<uint64_t>(type.alignment)); writer.endArray();
            }
            writer.endArray();
            if (layout.namedStores)
            {
                writer.startArray();
                for (const auto &store : *layout.namedStores)
                {
                    writer.startArray(); writer.value(toString(store.kind)); writer.startArray();
                    for (const auto &field : store.fields)
                    {
                        writer.startArray(); writeId(writer, field.name); writeId(writer, field.type);
                        writer.value(field.offset); writeId(writer, field.state);
                        writeId(writer, field.value); writer.value(static_cast<uint64_t>(field.aux));
                        writer.endArray();
                    }
                    writer.endArray(); writer.value(store.sizeBytes); writer.endArray();
                }
                writer.endArray();
            }
            writer.endArray();
        }

        template <typename SourceId>
        void writeCpuFanout(StreamWriter &writer, const std::vector<CpuFanoutEntry<SourceId>> &rows)
        {
            writer.startArray();
            for (const auto &row : rows)
            {
                writer.startArray(); writeId(writer, row.source);
                writeIdArray<PartitionId>(writer, row.targets.activate); writer.endArray();
            }
            writer.endArray();
        }

        void writeCpuSchedule(StreamWriter &writer, const CpuSchedulePlan &schedule)
        {
            // M5d-6 shape: [numaNodes, inputFanout, supernodeFanout,
            // stateFanout, eventActivation?, memWritePlan?, timeslotTriggers?] —
            // the last three stay positional: a present later field forces the
            // earlier ones to serialize (possibly as empty arrays). The legacy
            // round-seed/input-shadow/quiescence/demonitor/fold-residue
            // payloads are gone. (The fourth slot held the pre-v2 event
            // bitmaps; V2-M2 repurposed it for the non-sink activation map —
            // same wire shape, new semantics, and pre-v2 checkpoints are
            // rejected upstream by the V2-M1 category checks anyway.)
            writer.startArray(); writer.startArray();
            for (const auto &node : schedule.numaNodes)
            {
                writer.startArray(); writer.value(static_cast<uint64_t>(node.numaNode)); writer.startArray();
                for (const auto &core : node.cores)
                {
                    writer.startArray(); writer.value(static_cast<uint64_t>(core.core)); writer.startArray();
                    for (const auto &task : core.tasks)
                    {
                        writer.startArray(); writeId(writer, task.id); writeId(writer, task.partition);
                        writeIdArray<CpuTaskId>(writer, task.waitsFor);
                        writer.value(static_cast<uint64_t>(task.execution)); writer.endArray();
                    }
                    writer.endArray(); writer.endArray();
                }
                writer.endArray(); writer.endArray();
            }
            writer.endArray();
            writeCpuFanout(writer, schedule.inputFanout); writeCpuFanout(writer, schedule.computeSupernodeFanout);
            writeCpuFanout(writer, schedule.commitStateFanout);
            if (schedule.eventActivation || schedule.memWritePlan || schedule.timeslotTriggers)
            {
                writer.startArray();
                if (schedule.eventActivation)
                {
                    for (const auto &entry : *schedule.eventActivation)
                    {
                        writer.startArray(); writer.value(static_cast<uint64_t>(entry.act));
                        writer.startArray();
                        for (const auto word : entry.supernodeWords) writer.value(word);
                        writer.endArray(); writer.endArray();
                    }
                }
                writer.endArray();
            }
            if (schedule.memWritePlan || schedule.timeslotTriggers)
            {
                writer.startArray();
                if (schedule.memWritePlan)
                {
                    for (const auto &entry : *schedule.memWritePlan)
                    {
                        writer.startArray(); writeId(writer, entry.writeOp);
                        writer.value(static_cast<uint64_t>(entry.priority)); writer.value(entry.eventFree);
                        writer.startArray();
                        for (const auto &memReader : entry.readers)
                        {
                            writer.startArray(); writeId(writer, memReader.owner); writer.startArray();
                            if (memReader.staticRow) writer.value(*memReader.staticRow);
                            writer.endArray(); writer.endArray();
                        }
                        writer.endArray(); writer.endArray();
                    }
                }
                writer.endArray();
            }
            if (schedule.timeslotTriggers)
            {
                writer.startArray();
                for (const auto &trigger : *schedule.timeslotTriggers)
                {
                    writer.startArray(); writer.value(static_cast<uint64_t>(trigger.act));
                    writer.value(static_cast<uint64_t>(trigger.flag)); writer.endArray();
                }
                writer.endArray();
            }
            writer.endArray();
        }

        void writeCpuTranslationUnits(StreamWriter &writer, const CpuTranslationUnitPlan &plan)
        {
            // M5d-7 shape: [chunkCap, unitCap, [units...]]; a unit is
            // [name, estimatedLines, [[kind, offset, count, estimatedLines]...]].
            writer.startArray();
            writer.value(plan.chunkMaxEstimatedLines); writer.value(plan.unitMaxEstimatedLines);
            writer.startArray();
            for (const auto &unit : plan.units)
            {
                writer.startArray(); writer.value(unit.name); writer.value(unit.estimatedLines);
                writer.startArray();
                for (const auto &chunk : unit.chunks)
                {
                    writer.startArray();
                    writer.value(static_cast<uint64_t>(chunk.kind));
                    writer.value(static_cast<uint64_t>(chunk.offset));
                    writer.value(static_cast<uint64_t>(chunk.count));
                    writer.value(chunk.estimatedLines);
                    writer.endArray();
                }
                writer.endArray(); writer.endArray();
            }
            writer.endArray(); writer.endArray();
        }

        void writeCpuMapping(StreamWriter &writer, const CpuBackendMapping &cpu)
        {
            writer.startArray();
            writer.value(static_cast<uint64_t>(cpu.stage));
            writeId(writer, cpu.partitionTree.root);
            writer.startArray();
            for (const auto &partition : cpu.partitionTree.partitions)
            {
                writer.startArray();
                writeId(writer, partition.id); writeId(writer, partition.parent);
                writer.value(static_cast<uint64_t>(partition.attrs.kind));
                writer.value(static_cast<uint64_t>(partition.attrs.phase));
                writeIdArray<PartitionId>(writer, partition.children);
                writeIdArray<OpId>(writer, partition.ops);
                // Attr tail (V2-M1 shape): [helperChunks...], then an optional
                // [eventActs...], then either an optional [supernodeRange
                // offset,count] (emit functions) or a scalar supernode
                // category (supernodes) — the two never co-occur. A present
                // later field forces the earlier ones to serialize (possibly
                // as empty arrays).
                if (!partition.attrs.helperChunks.empty() || partition.attrs.eventActs ||
                    partition.attrs.supernodeRange || partition.attrs.supernodeCategory)
                {
                    writer.startArray();
                    for (auto chunk : partition.attrs.helperChunks)
                    {
                        writer.startArray(); writer.value(static_cast<uint64_t>(chunk.offset));
                        writer.value(static_cast<uint64_t>(chunk.count)); writer.endArray();
                    }
                    writer.endArray();
                    if (partition.attrs.eventActs || partition.attrs.supernodeRange ||
                        partition.attrs.supernodeCategory)
                    {
                        writer.startArray();
                        if (partition.attrs.eventActs)
                            for (const auto act : *partition.attrs.eventActs) writer.value(act);
                        writer.endArray();
                    }
                    if (partition.attrs.supernodeRange)
                    {
                        writer.startArray();
                        writer.value(static_cast<uint64_t>(partition.attrs.supernodeRange->offset));
                        writer.value(static_cast<uint64_t>(partition.attrs.supernodeRange->count));
                        writer.endArray();
                    }
                    else if (partition.attrs.supernodeCategory)
                        writer.value(static_cast<uint64_t>(*partition.attrs.supernodeCategory));
                }
                writer.endArray();
            }
            writer.endArray();
            if (cpu.dataLayout) writeCpuLayout(writer, *cpu.dataLayout);
            if (cpu.schedule) writeCpuSchedule(writer, *cpu.schedule);
            if (cpu.translationUnits) writeCpuTranslationUnits(writer, *cpu.translationUnits);
            writer.endArray();
        }

        template <typename Enum>
        Enum readCpuEnum(StreamReader &reader, Enum last)
        {
            const auto value = reader.unsignedInteger();
            if (value > static_cast<uint64_t>(last))
                throw std::runtime_error("unknown CPU mapping enum value");
            return static_cast<Enum>(value);
        }

        CpuDataLayout readCpuLayout(StreamReader &reader)
        {
            // M5d-6 shape: [pointerBytes, [types...], [namedStores...]?].
            CpuDataLayout layout;
            reader.startArray(); layout.pointerBytes = reader.index("pointer bytes");
            expectComma(reader); reader.startArray(); bool first = true;
            while (reader.nextArray(first))
            {
                CpuType type;
                reader.startArray(); type.id = readId<CpuTypeId>(reader, "CPU type ID");
                expectComma(reader); type.kind = readCpuEnum(reader, CpuTypeKind::Array);
                expectComma(reader); type.width = reader.index("CPU width", true);
                expectComma(reader); type.elementType = readId<CpuTypeId>(reader, "CPU element type", true);
                expectComma(reader); type.count = reader.unsignedInteger();
                expectComma(reader); type.size = reader.unsignedInteger();
                expectComma(reader); type.alignment = reader.index("CPU alignment"); reader.endArray();
                layout.types.push_back(type);
            }
            if (reader.comma())
            {
                layout.namedStores.emplace(); reader.startArray(); first = true;
                while (reader.nextArray(first))
                {
                    CpuNamedStore store;
                    reader.startArray();
                    const auto kind = parseCpuNamedStoreKind(reader.string());
                    if (!kind) throw std::runtime_error("unknown CPU named store kind");
                    store.kind = *kind;
                    expectComma(reader); reader.startArray(); bool fieldFirst = true;
                    while (reader.nextArray(fieldFirst))
                    {
                        CpuStoreField field;
                        reader.startArray(); field.name = readId<StringId>(reader, "store field name");
                        expectComma(reader); field.type = readId<CpuTypeId>(reader, "store field type");
                        expectComma(reader); field.offset = reader.unsignedInteger();
                        expectComma(reader); field.state = readId<StateId>(reader, "store field state", true);
                        expectComma(reader); field.value = readId<ValueId>(reader, "store field value", true);
                        expectComma(reader); field.aux = reader.index("store field aux", true);
                        reader.endArray(); store.fields.push_back(field);
                    }
                    expectComma(reader); store.sizeBytes = reader.unsignedInteger(); reader.endArray();
                    layout.namedStores->push_back(std::move(store));
                }
            }
            reader.endArray();
            return layout;
        }

        template <typename SourceId>
        std::vector<CpuFanoutEntry<SourceId>> readCpuFanout(StreamReader &reader)
        {
            std::vector<CpuFanoutEntry<SourceId>> rows;
            reader.startArray(); bool first = true;
            while (reader.nextArray(first))
            {
                CpuFanoutEntry<SourceId> row;
                reader.startArray(); row.source = readId<SourceId>(reader, "fanout source");
                expectComma(reader); row.targets.activate = readIdArray<PartitionId>(reader, "activation target");
                reader.endArray(); rows.push_back(std::move(row));
            }
            return rows;
        }

        CpuSchedulePlan readCpuSchedule(StreamReader &reader)
        {
            CpuSchedulePlan schedule;
            reader.startArray(); reader.startArray(); bool first = true;
            while (reader.nextArray(first))
            {
                CpuNumaSchedule node;
                reader.startArray(); node.numaNode = reader.index("NUMA node", true);
                expectComma(reader); reader.startArray(); bool coreFirst = true;
                while (reader.nextArray(coreFirst))
                {
                    CpuCoreSchedule core;
                    reader.startArray(); core.core = reader.index("CPU core", true);
                    expectComma(reader); reader.startArray(); bool taskFirst = true;
                    while (reader.nextArray(taskFirst))
                    {
                        CpuScheduledTask task;
                        reader.startArray(); task.id = readId<CpuTaskId>(reader, "task ID");
                        expectComma(reader); task.partition = readId<PartitionId>(reader, "task partition");
                        expectComma(reader); task.waitsFor = readIdArray<CpuTaskId>(reader, "task dependency");
                        expectComma(reader); task.execution = readCpuEnum(reader, CpuExecution::EvalEnd);
                        reader.endArray(); core.tasks.push_back(std::move(task));
                    }
                    reader.endArray(); node.cores.push_back(std::move(core));
                }
                reader.endArray(); schedule.numaNodes.push_back(std::move(node));
            }
            expectComma(reader); schedule.inputFanout = readCpuFanout<ValueId>(reader);
            expectComma(reader); schedule.computeSupernodeFanout = readCpuFanout<ValueId>(reader);
            expectComma(reader); schedule.commitStateFanout = readCpuFanout<StateId>(reader);
            // M5d-6 shape: the legacy round-seed/input-shadow/quiescence/
            // demonitor/fold-residue fields are gone; eventActivation (the
            // pre-v2 event bitmaps' slot, V2-M2), memWritePlan and
            // timeslotTriggers remain positional tails.
            if (reader.comma())
            {
                schedule.eventActivation.emplace(); reader.startArray(); first = true;
                while (reader.nextArray(first))
                {
                    CpuEventActivation entry;
                    reader.startArray(); entry.act = reader.index("event activation act", true);
                    expectComma(reader); reader.startArray(); bool wordFirst = true;
                    while (reader.nextArray(wordFirst))
                        entry.supernodeWords.push_back(reader.unsignedInteger());
                    reader.endArray();
                    schedule.eventActivation->push_back(std::move(entry));
                }
            }
            if (reader.comma())
            {
                schedule.memWritePlan.emplace(); reader.startArray(); first = true;
                while (reader.nextArray(first))
                {
                    CpuMemWritePlanEntry entry;
                    reader.startArray(); entry.writeOp = readId<OpId>(reader, "mem write plan op");
                    expectComma(reader); entry.priority = reader.index("mem write plan priority", true);
                    expectComma(reader); entry.eventFree = reader.boolean();
                    expectComma(reader); reader.startArray(); bool readerFirst = true;
                    while (reader.nextArray(readerFirst))
                    {
                        CpuMemReader memReader;
                        reader.startArray(); memReader.owner = readId<PartitionId>(reader, "mem reader owner");
                        expectComma(reader); reader.startArray(); bool rowFirst = true;
                        if (reader.nextArray(rowFirst))
                        {
                            memReader.staticRow = reader.unsignedInteger();
                            reader.endArray();
                        }
                        reader.endArray(); entry.readers.push_back(memReader);
                    }
                    reader.endArray();
                    schedule.memWritePlan->push_back(std::move(entry));
                }
            }
            if (reader.comma())
            {
                schedule.timeslotTriggers.emplace(); reader.startArray(); first = true;
                while (reader.nextArray(first))
                {
                    CpuTimeslotTrigger trigger;
                    reader.startArray(); trigger.act = reader.index("timeslot trigger act", true);
                    expectComma(reader); trigger.flag = reader.index("timeslot trigger flag", true);
                    reader.endArray(); schedule.timeslotTriggers->push_back(trigger);
                }
            }
            reader.endArray();
            return schedule;
        }

        CpuTranslationUnitPlan readCpuTranslationUnits(StreamReader &reader)
        {
            // M5d-7 shape: [chunkCap, unitCap, [[name, estimatedLines,
            // [[kind, offset, count, estimatedLines]...]]...]].
            CpuTranslationUnitPlan plan;
            reader.startArray();
            plan.chunkMaxEstimatedLines = reader.unsignedInteger();
            expectComma(reader); plan.unitMaxEstimatedLines = reader.unsignedInteger();
            expectComma(reader); reader.startArray(); bool first = true;
            while (reader.nextArray(first))
            {
                CpuTranslationUnit unit;
                reader.startArray(); unit.name = reader.string();
                expectComma(reader); unit.estimatedLines = reader.unsignedInteger();
                expectComma(reader); reader.startArray(); bool chunkFirst = true;
                while (reader.nextArray(chunkFirst))
                {
                    CpuEmitChunk chunk;
                    reader.startArray();
                    chunk.kind = readCpuEnum(reader, CpuEmitChunkKind::SupernodePart);
                    expectComma(reader); chunk.offset = reader.index("TU chunk offset", true);
                    expectComma(reader); chunk.count = reader.index("TU chunk count", true);
                    expectComma(reader); chunk.estimatedLines = reader.unsignedInteger();
                    reader.endArray(); unit.chunks.push_back(chunk);
                }
                reader.endArray(); plan.units.push_back(std::move(unit));
            }
            reader.endArray();
            return plan;
        }

        CpuBackendMapping readCpuMapping(StreamReader &reader)
        {
            CpuBackendMapping cpu;
            reader.startArray(); cpu.stage = readCpuEnum(reader, CpuMappingStage::TranslationUnits);
            expectComma(reader); cpu.partitionTree.root = readId<PartitionId>(reader, "partition root");
            expectComma(reader); reader.startArray();
            bool first = true;
            while (reader.nextArray(first))
            {
                CpuPartition partition;
                reader.startArray(); partition.id = readId<PartitionId>(reader, "partition ID");
                expectComma(reader); partition.parent = readId<PartitionId>(reader, "partition parent", true);
                expectComma(reader); partition.attrs.kind = readCpuEnum(reader, CpuPartitionKind::EmitFunction);
                expectComma(reader); partition.attrs.phase = readCpuEnum(reader, CpuPhase::Output);
                expectComma(reader); partition.children = readIdArray<PartitionId>(reader, "partition child");
                expectComma(reader); partition.ops = readIdArray<OpId>(reader, "partition op");
                // Attr tail (V2-M1 shape): optional [helperChunks...], then
                // optional [eventActs...], then either an optional
                // [supernodeRange offset,count] array (emit functions) or a
                // scalar supernode category (supernodes) — positional
                // elements of the partition array.
                bool tailFirst = false;
                if (reader.nextArray(tailFirst))
                {
                    reader.startArray();
                    bool first = true;
                    while (reader.nextArray(first))
                    {
                        reader.startArray(); const auto offset = reader.index("helper offset", true);
                        expectComma(reader); const auto count = reader.index("helper count"); reader.endArray();
                        partition.attrs.helperChunks.push_back({offset, count});
                    }
                    if (reader.comma())
                    {
                        std::vector<int64_t> acts;
                        reader.startArray(); bool actFirst = true;
                        while (reader.nextArray(actFirst)) acts.push_back(reader.integer());
                        partition.attrs.eventActs = std::move(acts);
                    }
                    if (reader.comma())
                    {
                        if (reader.arrayStarts())
                        {
                            reader.startArray();
                            const auto offset = reader.index("supernode range offset", true);
                            expectComma(reader); const auto count = reader.index("supernode range count", true);
                            reader.endArray();
                            partition.attrs.supernodeRange = Range{offset, count};
                            // The empty event-acts element written before a range
                            // is a positional placeholder, not an engaged empty
                            // annotation (that combination never occurs on a valid
                            // mapping: acts annotate supernodes, ranges annotate
                            // emit functions).
                            if (partition.attrs.eventActs && partition.attrs.eventActs->empty())
                                partition.attrs.eventActs.reset();
                        }
                        else
                            partition.attrs.supernodeCategory =
                                readCpuEnum(reader, CpuSupernodeCategory::SinkEvent);
                    }
                    reader.endArray();
                }
                cpu.partitionTree.partitions.push_back(std::move(partition));
            }
            bool tailFirst = false;
            if (reader.nextArray(tailFirst))
            {
                cpu.dataLayout = readCpuLayout(reader);
                if (reader.nextArray(tailFirst))
                {
                    cpu.schedule = readCpuSchedule(reader);
                    if (reader.nextArray(tailFirst))
                    {
                        cpu.translationUnits = readCpuTranslationUnits(reader);
                        reader.endArray();
                    }
                }
            }
            return cpu;
        }

        void writeCounts(StreamWriter &writer, const GrhSimModel &model)
        {
            // replaceOperation leaves orphaned ranges in the shared pools, so pool
            // capacity can exceed what the payload carries (each operation's own
            // spans); the counts must match the serialized spans for load-side
            // verification.
            uint64_t operandCount = 0, resultCount = 0, objectRefCount = 0, parameterCount = 0;
            for (const auto &op : model.operations())
            {
                operandCount += model.operands(op).size();
                resultCount += model.results(op).size();
                objectRefCount += model.objectRefs(op).size();
                parameterCount += model.parameters(op).size();
            }
            writer.startObject();
            writer.key("strings"); writer.value(static_cast<uint64_t>(model.strings().size()));
            writer.key("dialects"); writer.value(static_cast<uint64_t>(model.dialects().size()));
            writer.key("types"); writer.value(static_cast<uint64_t>(model.types().size()));
            writer.key("origins"); writer.value(static_cast<uint64_t>(model.origins().size()));
            writer.key("inputs"); writer.value(static_cast<uint64_t>(model.inputs().size()));
            writer.key("outputs"); writer.value(static_cast<uint64_t>(model.outputs().size()));
            writer.key("states"); writer.value(static_cast<uint64_t>(model.states().size()));
            writer.key("functions"); writer.value(static_cast<uint64_t>(model.functions().size()));
            writer.key("function_arguments");
            writer.value(static_cast<uint64_t>(model.functionArguments().size()));
            writer.key("interface_ports");
            writer.value(static_cast<uint64_t>(model.interfacePorts().size()));
            writer.key("values"); writer.value(static_cast<uint64_t>(model.values().size()));
            writer.key("operations"); writer.value(static_cast<uint64_t>(model.operations().size()));
            writer.key("operands"); writer.value(operandCount);
            writer.key("results"); writer.value(resultCount);
            writer.key("object_refs"); writer.value(objectRefCount);
            writer.key("parameters"); writer.value(parameterCount);
            writer.key("init_records"); writer.value(static_cast<uint64_t>(model.initRecords().size()));
            writer.key("init_steps"); writer.value(static_cast<uint64_t>(model.initSteps().size()));
            writer.key("init_parameters");
            writer.value(static_cast<uint64_t>(model.initParameterPool().size()));
            writer.key("mappings"); writer.value(static_cast<uint64_t>(model.mappings().size()));
            writer.key("mapping_parameters");
            writer.value(static_cast<uint64_t>(model.mappingParameterPool().size()));
            writer.endObject();
        }

        ModelReserve readCounts(StreamReader &reader)
        {
            ModelReserve counts;
            reader.startObject();
            reader.key("strings", true); counts.strings = reader.index("string count", true);
            reader.key("dialects", false); counts.dialects = reader.index("dialect count", true);
            reader.key("types", false); counts.types = reader.index("type count", true);
            reader.key("origins", false); counts.origins = reader.index("origin count", true);
            reader.key("inputs", false); counts.inputs = reader.index("input count", true);
            reader.key("outputs", false); counts.outputs = reader.index("output count", true);
            reader.key("states", false); counts.states = reader.index("state count", true);
            reader.key("functions", false); counts.functions = reader.index("function count", true);
            reader.key("function_arguments", false);
            counts.functionArguments = reader.index("function argument count", true);
            reader.key("interface_ports", false);
            counts.interfacePorts = reader.index("interface port count", true);
            reader.key("values", false); counts.values = reader.index("value count", true);
            reader.key("operations", false); counts.operations = reader.index("operation count", true);
            reader.key("operands", false); counts.operands = reader.index("operand count", true);
            reader.key("results", false); counts.results = reader.index("result count", true);
            reader.key("object_refs", false); counts.objectRefs = reader.index("object reference count", true);
            reader.key("parameters", false); counts.parameters = reader.index("parameter count", true);
            reader.key("init_records", false); counts.initRecords = reader.index("init record count", true);
            reader.key("init_steps", false); counts.initSteps = reader.index("init step count", true);
            reader.key("init_parameters", false);
            counts.initParameters = reader.index("init parameter count", true);
            reader.key("mappings", false); counts.mappings = reader.index("mapping count", true);
            reader.key("mapping_parameters", false);
            counts.mappingParameters = reader.index("mapping parameter count", true);
            reader.endObject();
            return counts;
        }

        void verifyCount(std::size_t actual, std::size_t expected, std::string_view name)
        {
            if (actual != expected)
            {
                throw std::runtime_error("serialized " + std::string(name) + " count is " +
                                         std::to_string(expected) + ", payload contains " +
                                         std::to_string(actual));
            }
        }

        void verifyCounts(const GrhSimModel &model, const ModelReserve &counts)
        {
            verifyCount(model.strings().size(), counts.strings, "string");
            verifyCount(model.dialects().size(), counts.dialects, "dialect");
            verifyCount(model.types().size(), counts.types, "type");
            verifyCount(model.origins().size(), counts.origins, "origin");
            verifyCount(model.inputs().size(), counts.inputs, "input");
            verifyCount(model.outputs().size(), counts.outputs, "output");
            verifyCount(model.states().size(), counts.states, "state");
            verifyCount(model.functions().size(), counts.functions, "function");
            verifyCount(model.functionArguments().size(), counts.functionArguments,
                        "function argument");
            verifyCount(model.interfacePorts().size(), counts.interfacePorts, "interface port");
            verifyCount(model.values().size(), counts.values, "value");
            verifyCount(model.operations().size(), counts.operations, "operation");
            verifyCount(model.operandPool().size(), counts.operands, "operand");
            verifyCount(model.resultPool().size(), counts.results, "result");
            verifyCount(model.objectRefPool().size(), counts.objectRefs, "object reference");
            verifyCount(model.parameterPool().size(), counts.parameters, "parameter");
            verifyCount(model.initRecords().size(), counts.initRecords, "init record");
            verifyCount(model.initSteps().size(), counts.initSteps, "init step");
            verifyCount(model.initParameterPool().size(), counts.initParameters, "init parameter");
            verifyCount(model.mappings().size(), counts.mappings, "mapping");
            verifyCount(model.mappingParameterPool().size(), counts.mappingParameters,
                        "mapping parameter");
        }

        void writeModel(StreamWriter &writer, const GrhSimModel &model)
        {
            writer.startObject();
            writer.key("format"); writer.value(kGrhSimJsonFormat);
            writer.key("counts"); writeCounts(writer, model);
            writer.key("strings"); writer.startArray();
            for (const std::string &value : model.strings().strings()) writer.value(value);
            writer.endArray();
            writer.key("name"); writeId(writer, model.name());

            writer.key("dialects"); writer.startArray();
            for (const DialectUse &dialect : model.dialects())
            {
                writer.startArray(); writeId(writer, dialect.name); writeId(writer, dialect.version);
                writeId(writer, dialect.schemaFingerprint); writer.endArray();
            }
            writer.endArray();

            writer.key("types"); writer.startArray();
            for (const Type &type : model.types())
            {
                writer.startArray(); writeId(writer, type.id); writeId(writer, type.typeRef);
                writer.value(toString(type.kind)); writer.value(static_cast<uint64_t>(type.width));
                writer.value(type.isSigned); writer.value(toString(type.domain));
                writeId(writer, type.elementType); writer.value(type.count); writer.endArray();
            }
            writer.endArray();

            writer.key("origins"); writer.startArray();
            for (const Origin &origin : model.origins())
            {
                writer.startArray(); writeId(writer, origin.id); writeId(writer, origin.sourceKind);
                writeId(writer, origin.symbol); writer.value(static_cast<uint64_t>(origin.sourceIndex));
                writeId(writer, origin.file); writer.value(static_cast<uint64_t>(origin.line));
                writer.value(static_cast<uint64_t>(origin.column)); writer.value(static_cast<uint64_t>(origin.endLine));
                writer.value(static_cast<uint64_t>(origin.endColumn)); writeId(writer, origin.pass);
                writeId(writer, origin.note); writer.endArray();
            }
            writer.endArray();

            writer.key("inputs"); writer.startArray();
            for (const InputObject &object : model.inputs())
            {
                writer.startArray(); writeId(writer, object.id); writeId(writer, object.name);
                writeId(writer, object.type); writeId(writer, object.origin); writer.endArray();
            }
            writer.endArray();
            writer.key("outputs"); writer.startArray();
            for (const OutputObject &object : model.outputs())
            {
                writer.startArray(); writeId(writer, object.id); writeId(writer, object.name);
                writeId(writer, object.type); writeId(writer, object.origin); writer.endArray();
            }
            writer.endArray();
            writer.key("states"); writer.startArray();
            for (const StateObject &object : model.states())
            {
                writer.startArray(); writeId(writer, object.id); writeId(writer, object.name);
                writeId(writer, object.type); writeId(writer, object.origin);
                // Optional fifth element (M5d-4): the state store class,
                // written only once the model is classified, so unclassified
                // checkpoints stay byte-compatible with the older schema.
                if (object.storeClass != StateStoreClass::None)
                    writer.value(toString(object.storeClass));
                writer.endArray();
            }
            writer.endArray();

            writer.key("functions"); writer.startArray();
            for (const ExternFunction &function : model.functions())
            {
                writer.startArray(); writeId(writer, function.id); writeId(writer, function.name);
                writeId(writer, function.declRef); writeId(writer, function.symbol);
                writeId(writer, function.returnType); writeId(writer, function.origin);
                writer.startArray();
                for (const DpiArgument &argument : model.arguments(function))
                {
                    writer.startArray(); writeId(writer, argument.name); writer.value(toString(argument.direction));
                    writeId(writer, argument.type); writer.endArray();
                }
                writer.endArray(); writer.endArray();
            }
            writer.endArray();

            writer.key("interface"); writer.startArray();
            for (const InterfacePort &port : model.interfacePorts())
            {
                writer.startArray(); writeId(writer, port.name); writer.value(toString(port.direction));
                writeId(writer, port.input); writeId(writer, port.output); writeId(writer, port.outputEnable);
                writer.endArray();
            }
            writer.endArray();

            writer.key("values"); writer.startArray();
            for (const SimValue &value : model.values())
            {
                writer.startArray(); writeId(writer, value.id); writeId(writer, value.type);
                writeId(writer, value.name); writeId(writer, value.origin); writer.endArray();
            }
            writer.endArray();

            writer.key("operations"); writer.startArray();
            for (const SimOp &op : model.operations())
            {
                writer.startArray(); writeId(writer, op.id); writeId(writer, op.opType);
                writeId(writer, op.name); writeId(writer, op.origin);
                writeIdArray(writer, model.operands(op)); writeIdArray(writer, model.results(op));
                writeObjectRefs(writer, model.objectRefs(op)); writeParameterArray(writer, model.parameters(op));
                writer.value(toString(op.phase)); writer.endArray();
            }
            writer.endArray();

            writer.key("init"); writer.startArray();
            for (const InitRecord &record : model.initRecords())
            {
                writer.startArray(); writeId(writer, record.state); writer.startArray();
                for (const InitStep &step : model.steps(record))
                {
                    writer.startArray(); writeId(writer, step.kind);
                    writeParameterArray(writer, model.parameters(step)); writer.endArray();
                }
                writer.endArray(); writer.endArray();
            }
            writer.endArray();

            writer.key("mappings"); writer.startArray();
            for (const BackendMapping &mapping : model.mappings())
            {
                writer.startArray(); writeId(writer, mapping.backend); writeId(writer, mapping.schema);
                writer.value(mapping.complete); writeParameterArray(writer, model.parameters(mapping));
                if (mapping.cpu) writeCpuMapping(writer, *mapping.cpu);
                writer.endArray();
            }
            writer.endArray();
            // Optional trailing keys, positional: declaredSymbols, then
            // generateGroups, then declProvenance. Only written when non-empty
            // (a later key drags the earlier ones out as possibly empty
            // arrays), so metadata-free checkpoints stay byte-compatible with
            // the pre-metadata schema.
            const bool hasProvenance = !model.declProvenances().empty();
            if (!model.declaredSymbols().empty() || !model.generateGroups().empty() || hasProvenance)
            {
                writer.key("declaredSymbols");
                writeIdArray<StringId>(writer, model.declaredSymbols());
                if (!model.generateGroups().empty() || hasProvenance)
                {
                    writer.key("generateGroups");
                    writer.startArray();
                    for (const GenerateGroup &group : model.generateGroups())
                    {
                        writer.startArray();
                        writeId(writer, group.scope); writeId(writer, group.name);
                        writeIdArray<StringId>(writer, group.symbols);
                        writer.endArray();
                    }
                    writer.endArray();
                }
                if (hasProvenance)
                {
                    writer.key("declProvenance");
                    writer.startArray();
                    for (const DeclProvenance &record : model.declProvenances())
                    {
                        writer.startArray();
                        writeId(writer, record.symbol); writeId(writer, record.origin);
                        writer.value(record.width);
                        writer.startArray();
                        for (const uint64_t dim : record.shape) writer.value(dim);
                        writer.endArray();
                        writer.startArray();
                        for (const DeclProvenanceSlice &slice : record.slices)
                        {
                            writer.startArray();
                            writer.value(toString(slice.kind));
                            writer.value(toString(slice.target));
                            writer.value(static_cast<uint64_t>(slice.targetIndex));
                            writer.value(slice.targetOffset);
                            writer.value(slice.declOffset);
                            writer.value(slice.width);
                            writer.endArray();
                        }
                        writer.endArray();
                        writer.endArray();
                    }
                    writer.endArray();
                }
            }
            writer.endObject();
        }

        std::unique_ptr<GrhSimModel> readModel(StreamReader &reader)
        {
            reader.startObject();
            reader.key("format", true);
            const std::string format = reader.string();
            if (format != kGrhSimJsonFormat)
                throw std::runtime_error("unsupported GrhSIM JSON format: " + format);

            reader.key("counts", false);
            const ModelReserve counts = readCounts(reader);
            auto model = std::make_unique<GrhSimModel>();
            model->reserve(counts);

            reader.key("strings", false);
            bool first = true;
            reader.startArray();
            std::size_t stringIndex = 0;
            while (reader.nextArray(first))
            {
                const StringId id = model->intern(reader.string());
                ++stringIndex;
                if (id.index != stringIndex)
                    throw std::runtime_error("string table contains an empty or duplicate entry");
            }

            reader.key("name", false);
            const StringId modelName = readId<StringId>(reader, "model name");
            model->setName(model->text(modelName));

            reader.key("dialects", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const StringId name = readId<StringId>(reader, "dialect name");
                expectComma(reader); const StringId version = readId<StringId>(reader, "dialect version");
                expectComma(reader); const StringId fingerprint = readId<StringId>(reader, "dialect fingerprint", true);
                reader.endArray();
                model->addDialect(model->text(name), model->text(version), model->text(fingerprint));
            }

            reader.key("types", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const TypeId expected = readId<TypeId>(reader, "type ID");
                expectComma(reader); const StringId typeRef = readId<StringId>(reader, "type ref");
                expectComma(reader); const auto kind = parseTypeKind(reader.string());
                if (!kind) throw std::runtime_error("unknown GrhSIM type kind");
                expectComma(reader); const uint32_t width = reader.index("type width", true);
                expectComma(reader); const bool isSigned = reader.boolean();
                expectComma(reader); const auto domain = parseLogicDomain(reader.string());
                if (!domain) throw std::runtime_error("unknown logic domain");
                expectComma(reader); const TypeId element = readId<TypeId>(reader, "element type", true);
                expectComma(reader); const uint64_t count = reader.unsignedInteger(); reader.endArray();
                TypeId actual;
                if (*kind == TypeKind::Logic) actual = model->logicType(width, isSigned, *domain);
                else if (*kind == TypeKind::Real) actual = model->realType();
                else if (*kind == TypeKind::String) actual = model->stringType();
                else actual = model->arrayType(element, count);
                if (actual != expected || model->text(model->types()[actual.index - 1].typeRef) != model->text(typeRef))
                    throw std::runtime_error("type table is not canonical or has mismatched IDs");
            }

            reader.key("origins", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const OriginId expected = readId<OriginId>(reader, "origin ID");
                Origin origin; expectComma(reader); origin.sourceKind = readId<StringId>(reader, "origin kind", true);
                expectComma(reader); origin.symbol = readId<StringId>(reader, "origin symbol", true);
                expectComma(reader); origin.sourceIndex = reader.index("origin source index", true);
                expectComma(reader); origin.file = readId<StringId>(reader, "origin file", true);
                expectComma(reader); origin.line = reader.index("origin line", true);
                expectComma(reader); origin.column = reader.index("origin column", true);
                expectComma(reader); origin.endLine = reader.index("origin end line", true);
                expectComma(reader); origin.endColumn = reader.index("origin end column", true);
                expectComma(reader); origin.pass = readId<StringId>(reader, "origin pass", true);
                expectComma(reader); origin.note = readId<StringId>(reader, "origin note", true);
                reader.endArray();
                if (model->addOrigin(origin) != expected) throw std::runtime_error("origin IDs are not sequential");
            }

            reader.key("inputs", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const InputId expected = readId<InputId>(reader, "input ID");
                expectComma(reader); const StringId name = readId<StringId>(reader, "input name");
                expectComma(reader); const TypeId type = readId<TypeId>(reader, "input type");
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "input origin", true);
                reader.endArray();
                if (model->addInput(model->text(name), type, origin) != expected)
                    throw std::runtime_error("input IDs are not sequential");
            }
            reader.key("outputs", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const OutputId expected = readId<OutputId>(reader, "output ID");
                expectComma(reader); const StringId name = readId<StringId>(reader, "output name");
                expectComma(reader); const TypeId type = readId<TypeId>(reader, "output type");
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "output origin", true);
                reader.endArray();
                if (model->addOutput(model->text(name), type, origin) != expected)
                    throw std::runtime_error("output IDs are not sequential");
            }
            reader.key("states", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const StateId expected = readId<StateId>(reader, "state ID");
                expectComma(reader); const StringId name = readId<StringId>(reader, "state name");
                expectComma(reader); const TypeId type = readId<TypeId>(reader, "state type");
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "state origin", true);
                // Optional fifth element (M5d-4): the state store class.
                StateStoreClass storeClass = StateStoreClass::None;
                bool tailFirst = false;
                if (reader.nextArray(tailFirst))
                {
                    const auto parsed = parseStateStoreClass(reader.string());
                    if (!parsed || *parsed == StateStoreClass::None)
                        throw std::runtime_error("unknown state store class");
                    storeClass = *parsed;
                    reader.endArray();
                }
                if (model->addState(model->text(name), type, origin) != expected)
                    throw std::runtime_error("state IDs are not sequential");
                if (storeClass != StateStoreClass::None)
                    model->setStateStoreClass(expected, storeClass);
            }

            reader.key("functions", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const FuncId expected = readId<FuncId>(reader, "function ID");
                expectComma(reader); const StringId name = readId<StringId>(reader, "function name");
                expectComma(reader); const StringId decl = readId<StringId>(reader, "function decl");
                expectComma(reader); const StringId symbol = readId<StringId>(reader, "function symbol");
                expectComma(reader); const TypeId returnType = readId<TypeId>(reader, "return type", true);
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "function origin", true);
                expectComma(reader); std::vector<DpiArgument> arguments; bool argFirst = true; reader.startArray();
                while (reader.nextArray(argFirst))
                {
                    reader.startArray(); const StringId argName = readId<StringId>(reader, "argument name");
                    expectComma(reader); const auto direction = parseDpiDirection(reader.string());
                    if (!direction) throw std::runtime_error("unknown DPI direction");
                    expectComma(reader); const TypeId type = readId<TypeId>(reader, "argument type");
                    reader.endArray(); arguments.push_back(DpiArgument{argName, *direction, type});
                }
                reader.endArray();
                if (model->addExternFunction(model->text(name), model->text(decl), model->text(symbol),
                                             arguments, returnType, origin) != expected)
                    throw std::runtime_error("function IDs are not sequential");
            }

            reader.key("interface", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const StringId name = readId<StringId>(reader, "interface name");
                expectComma(reader); const auto direction = parseInterfaceDirection(reader.string());
                if (!direction) throw std::runtime_error("unknown interface direction");
                expectComma(reader); const InputId input = readId<InputId>(reader, "interface input", true);
                expectComma(reader); const OutputId output = readId<OutputId>(reader, "interface output", true);
                expectComma(reader); const OutputId oe = readId<OutputId>(reader, "interface output enable", true);
                reader.endArray(); model->addInterfacePort(InterfacePort{name, *direction, input, output, oe});
            }

            reader.key("values", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const ValueId expected = readId<ValueId>(reader, "value ID");
                expectComma(reader); const TypeId type = readId<TypeId>(reader, "value type");
                expectComma(reader); const StringId name = readId<StringId>(reader, "value name", true);
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "value origin", true);
                reader.endArray();
                if (model->addValue(type, model->text(name), origin) != expected)
                    throw std::runtime_error("value IDs are not sequential");
            }

            reader.key("operations", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const OpId expected = readId<OpId>(reader, "operation ID");
                expectComma(reader); const StringId opType = readId<StringId>(reader, "operation type");
                expectComma(reader); const StringId name = readId<StringId>(reader, "operation name", true);
                expectComma(reader); const OriginId origin = readId<OriginId>(reader, "operation origin", true);
                expectComma(reader); auto operands = readIdArray<ValueId>(reader, "operand");
                expectComma(reader); auto results = readIdArray<ValueId>(reader, "result");
                expectComma(reader); auto refs = readObjectRefs(reader);
                expectComma(reader); auto parameters = readParameterArray(reader);
                expectComma(reader); const auto phase = parseSimPhase(reader.string());
                if (!phase) throw std::runtime_error("unknown operation phase");
                reader.endArray();
                if (model->addOperation(model->text(opType), operands, results, refs, parameters,
                                        model->text(name), origin) != expected)
                    throw std::runtime_error("operation IDs are not sequential");
                model->setOperationPhase(expected, *phase);
            }

            reader.key("init", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const StateId state = readId<StateId>(reader, "init state");
                expectComma(reader); std::vector<InitStep> steps; std::vector<Parameter> allParameters;
                bool stepFirst = true; reader.startArray();
                while (reader.nextArray(stepFirst))
                {
                    reader.startArray(); const StringId kind = readId<StringId>(reader, "init step kind");
                    expectComma(reader); auto parameters = readParameterArray(reader); reader.endArray();
                    const uint32_t offset = static_cast<uint32_t>(allParameters.size());
                    allParameters.insert(allParameters.end(), parameters.begin(), parameters.end());
                    steps.push_back(InitStep{kind, Range{offset, static_cast<uint32_t>(parameters.size())}});
                }
                reader.endArray(); model->addInit(state, steps, allParameters);
            }

            reader.key("mappings", false); first = true; reader.startArray();
            while (reader.nextArray(first))
            {
                reader.startArray(); const StringId backend = readId<StringId>(reader, "mapping backend");
                expectComma(reader); const StringId schema = readId<StringId>(reader, "mapping schema");
                expectComma(reader); const bool complete = reader.boolean();
                expectComma(reader); auto parameters = readParameterArray(reader);
                model->addMapping(model->text(backend), model->text(schema), complete, parameters);
                bool tailFirst = false;
                if (reader.nextArray(tailFirst))
                {
                    if (model->text(backend) != "cpu" || model->text(schema) != "cpu.st.v1")
                        throw std::runtime_error("unexpected CPU mapping payload or completion flag");
                    auto cpu = readCpuMapping(reader);
                    // The six-phase pipeline completes at PhaseSchedule and
                    // stays complete through TranslationUnits (M5d-7); the
                    // legacy Schedule terminal was removed in M5d-6.
                    const bool terminal = cpu.stage == CpuMappingStage::PhaseSchedule ||
                                          cpu.stage == CpuMappingStage::TranslationUnits;
                    if (complete != terminal)
                        throw std::runtime_error("CPU mapping completion disagrees with stage");
                    model->setCpuMapping(std::move(cpu));
                    reader.endArray();
                }
            }

            // Optional trailing keys (absent in old checkpoints): a
            // "declaredSymbols" string-id array, then a "generateGroups" array
            // of [scope, name, [symbol ids]] entries, then a "declProvenance"
            // array of [symbol, origin, width, [shape], [[kind, target kind,
            // target index, target offset, decl offset, width]]] records.
            if (reader.comma())
            {
                if (reader.string() != "declaredSymbols")
                    throw std::runtime_error("expected property 'declaredSymbols'");
                reader.expect(':');
                for (const StringId symbol : readIdArray<StringId>(reader, "declared symbol"))
                    model->addDeclaredSymbol(symbol);
                if (reader.comma())
                {
                    if (reader.string() != "generateGroups")
                        throw std::runtime_error("expected property 'generateGroups'");
                    reader.expect(':');
                    bool groupFirst = true;
                    reader.startArray();
                    while (reader.nextArray(groupFirst))
                    {
                        reader.startArray();
                        const StringId scope = readId<StringId>(reader, "generate group scope");
                        expectComma(reader);
                        const StringId name = readId<StringId>(reader, "generate group name");
                        expectComma(reader);
                        auto symbols = readIdArray<StringId>(reader, "generate group symbol");
                        reader.endArray();
                        const std::size_t group = model->addGenerateGroup(scope, name);
                        for (const StringId symbol : symbols)
                            model->addGenerateGroupSymbol(group, symbol);
                    }
                    if (reader.comma())
                    {
                        if (reader.string() != "declProvenance")
                            throw std::runtime_error("expected property 'declProvenance'");
                        reader.expect(':');
                        bool recordFirst = true;
                        reader.startArray();
                        while (reader.nextArray(recordFirst))
                        {
                            reader.startArray();
                            DeclProvenance record;
                            record.symbol = readId<StringId>(reader, "provenance symbol");
                            expectComma(reader);
                            record.origin = readId<OriginId>(reader, "provenance origin", true);
                            expectComma(reader);
                            record.width = reader.unsignedInteger();
                            expectComma(reader);
                            bool dimFirst = true;
                            reader.startArray();
                            while (reader.nextArray(dimFirst))
                                record.shape.push_back(reader.unsignedInteger());
                            expectComma(reader);
                            bool sliceFirst = true;
                            reader.startArray();
                            while (reader.nextArray(sliceFirst))
                            {
                                reader.startArray();
                                DeclProvenanceSlice slice;
                                const auto kind = parseDeclProvenanceKind(reader.string());
                                if (!kind)
                                    throw std::runtime_error("unknown provenance slice kind");
                                slice.kind = *kind;
                                expectComma(reader);
                                const auto target = parseDeclProvenanceTarget(reader.string());
                                if (!target)
                                    throw std::runtime_error("unknown provenance slice target");
                                slice.target = *target;
                                expectComma(reader);
                                slice.targetIndex = reader.index("provenance slice target");
                                expectComma(reader);
                                slice.targetOffset = reader.unsignedInteger();
                                expectComma(reader);
                                slice.declOffset = reader.unsignedInteger();
                                expectComma(reader);
                                slice.width = reader.unsignedInteger();
                                reader.endArray();
                                record.slices.push_back(slice);
                            }
                            reader.endArray();
                            model->upsertDeclProvenance(std::move(record));
                        }
                    }
                }
            }
            reader.endObject(); reader.finish();
            verifyCounts(*model, counts);
            return model;
        }
    } // namespace

    bool writeGrhSimJson(const GrhSimModel &model, std::ostream &output,
                         const DialectRegistry &registry,
                         diag::Diagnostics &diagnostics, bool pretty)
    {
        if (!verifyGrhSimModel(model, registry, diagnostics)) return false;
        try
        {
            StreamWriter writer(output, pretty);
            writeModel(writer, model);
            if (pretty) output << '\n';
            if (!output) throw std::runtime_error("failed while writing GrhSIM JSON stream");
            return true;
        }
        catch (const std::exception &ex)
        {
            diagnostics.error(ex.what(), "store_grhsim");
            return false;
        }
    }

    std::unique_ptr<GrhSimModel> readGrhSimJson(std::istream &input,
                                                const DialectRegistry &registry,
                                                diag::Diagnostics &diagnostics)
    {
        try
        {
            StreamReader reader(input);
            auto model = readModel(reader);
            if (!verifyGrhSimModel(*model, registry, diagnostics)) return nullptr;
            return model;
        }
        catch (const std::exception &ex)
        {
            diagnostics.error(ex.what(), "load_grhsim");
            return nullptr;
        }
    }

    bool storeGrhSimModel(const GrhSimModel &model, const std::filesystem::path &path,
                          const DialectRegistry &registry,
                          diag::Diagnostics &diagnostics, bool pretty)
    {
        if (path.empty() || path.filename().empty())
        {
            diagnostics.error("output path must name a file", "store_grhsim");
            return false;
        }
        std::error_code ec;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
            {
                diagnostics.error("failed to create output directory: " + ec.message(),
                                  path.string());
                return false;
            }
        }
        static std::atomic<uint64_t> temporarySequence{1};
        const std::filesystem::path temporary =
            path.string() + ".tmp." +
            std::to_string(temporarySequence.fetch_add(1, std::memory_order_relaxed));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                diagnostics.error("failed to open temporary output file", temporary.string());
                return false;
            }
            if (!writeGrhSimJson(model, output, registry, diagnostics, pretty))
            {
                output.close();
                std::filesystem::remove(temporary, ec);
                return false;
            }
            output.flush();
            if (!output)
            {
                diagnostics.error("failed to flush temporary output file", temporary.string());
                output.close();
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }
        std::filesystem::rename(temporary, path, ec);
        if (ec)
        {
            diagnostics.error("failed to atomically publish GrhSIM JSON: " + ec.message(),
                              path.string());
            std::filesystem::remove(temporary, ec);
            return false;
        }
        return true;
    }

    std::unique_ptr<GrhSimModel> loadGrhSimModel(const std::filesystem::path &path,
                                                 const DialectRegistry &registry,
                                                 diag::Diagnostics &diagnostics)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            diagnostics.error("failed to open GrhSIM JSON file", path.string());
            return nullptr;
        }
        return readGrhSimJson(input, registry, diagnostics);
    }

} // namespace wolvrix::lib::grhsim
