#ifndef WOLVRIX_EMIT_GRHSIM_JSON_WRITER_HPP
#define WOLVRIX_EMIT_GRHSIM_JSON_WRITER_HPP

#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string_view>
#include <vector>

namespace wolvrix::lib::emit::grhsim_detail {

// Writes compact JSON directly to the destination. Only the nesting stack is
// retained; strings and large arrays do not accumulate an intermediate buffer.
// Like slang::JsonWriter, callers must supply a valid object / array sequence.
class StreamingJsonWriter {
public:
    explicit StreamingJsonWriter(std::ostream& output) : output_(output) {}

    void startObject() {
        beforeValue();
        output_.put('{');
        stack_.push_back({Kind::Object});
    }

    void endObject() {
        assert(!stack_.empty() && stack_.back().kind == Kind::Object);
        assert(!stack_.back().propertyPending);
        output_.put('}');
        stack_.pop_back();
    }

    void startArray() {
        beforeValue();
        output_.put('[');
        stack_.push_back({Kind::Array});
    }

    void endArray() {
        assert(!stack_.empty() && stack_.back().kind == Kind::Array);
        output_.put(']');
        stack_.pop_back();
    }

    void writeProperty(std::string_view name) {
        assert(!stack_.empty() && stack_.back().kind == Kind::Object);
        auto& frame = stack_.back();
        assert(!frame.propertyPending);
        if (!frame.first) output_.put(',');
        frame.first = false;
        writeQuoted(name);
        output_.put(':');
        frame.propertyPending = true;
    }

    void writeValue(std::string_view value) {
        beforeValue();
        writeQuoted(value);
    }

    void writeValue(uint64_t value) { writeInteger(value); }
    void writeValue(int64_t value) { writeInteger(value); }

    void writeValue(bool value) {
        beforeValue();
        if (value) output_.write("true", 4);
        else output_.write("false", 5);
    }

    // The input must contain one complete, prevalidated JSON value. This
    // splices private spool contents without accumulating them in memory.
    void writeRawValue(std::istream& input) {
        beforeValue();
        output_ << input.rdbuf();
    }

    // Prevent a string literal from selecting the bool overload.
    void writeValue(const char*) = delete;

private:
    enum class Kind { Object, Array };
    struct Frame {
        Kind kind;
        bool first = true;
        bool propertyPending = false;
    };

    void beforeValue() {
        if (stack_.empty()) return;
        auto& frame = stack_.back();
        if (frame.kind == Kind::Object) {
            assert(frame.propertyPending);
            frame.propertyPending = false;
        } else {
            if (!frame.first) output_.put(',');
            frame.first = false;
        }
    }

    template <typename Integer>
    void writeInteger(Integer value) {
        beforeValue();
        // to_chars is locale-independent and its storage is bounded by the
        // largest decimal int64_t / uint64_t representation.
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        output_.write(buffer, static_cast<std::streamsize>(result.ptr - buffer));
    }

    void writeQuoted(std::string_view text) {
        output_.put('"');
        std::size_t begin = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte >= 0x20 && byte != '"' && byte != '\\') continue;
            if (i != begin)
                output_.write(text.data() + begin, static_cast<std::streamsize>(i - begin));
            switch (byte) {
                case '"': output_.write("\\\"", 2); break;
                case '\\': output_.write("\\\\", 2); break;
                case '\b': output_.write("\\b", 2); break;
                case '\f': output_.write("\\f", 2); break;
                case '\n': output_.write("\\n", 2); break;
                case '\r': output_.write("\\r", 2); break;
                case '\t': output_.write("\\t", 2); break;
                default: {
                    constexpr char hex[] = "0123456789abcdef";
                    const char escape[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 15]};
                    output_.write(escape, sizeof(escape));
                    break;
                }
            }
            begin = i + 1;
        }
        if (begin != text.size())
            output_.write(text.data() + begin, static_cast<std::streamsize>(text.size() - begin));
        output_.put('"');
    }

    std::ostream& output_;
    std::vector<Frame> stack_;
};

}
#endif
