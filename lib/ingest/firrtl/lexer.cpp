#include "internal.hpp"

#include <algorithm>
#include <charconv>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
}
bool identifier(std::string_view text, bool literal) {
    return !text.empty() && (alpha(text.front()) || (literal && digit(text.front()))) &&
        std::all_of(text.begin(), text.end(), [](char c) { return alpha(c) || digit(c); });
}
bool integer(std::string_view text, bool radix) {
    if (!text.empty() && text.front() == '-') text.remove_prefix(1);
    unsigned base = 10;
    if (radix && text.size() >= 2 && text[0] == '0') {
        switch (text[1]) {
        case 'b': base = 2; break; case 'o': base = 8; break;
        case 'd': base = 10; break; case 'h': base = 16; break;
        default: break;
        }
        if (text[1] == 'b' || text[1] == 'o' || text[1] == 'd' || text[1] == 'h') text.remove_prefix(2);
    }
    return !text.empty() && std::all_of(text.begin(), text.end(), [base](char c) {
        const unsigned value = digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                               c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        return value < base;
    });
}
int64_t integer64(const Token& token, bool nonnegative) {
    int64_t result = 0;
    const auto [end, code] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), result);
    if (token.kind != TokenKind::Number || !integer(token.text) || code != std::errc{} ||
        end != token.text.data() + token.text.size() || (nonnegative && result < 0))
        throw Error(token.range, nonnegative ? "expected a nonnegative 64-bit decimal integer" : "expected a 64-bit decimal integer");
    return result;
}

Lexer::Lexer(std::string_view text, uint64_t begin, uint64_t end, uint64_t line, uint64_t baseIndent)
    : text_(text), cursor_(begin), end_(std::min<uint64_t>(end, text.size())), line_(line), indents_{baseIndent} {}
void Lexer::advance() {
    if (cursor_ < end_) {
        if (text_[cursor_++] == '\n') { ++line_; column_ = 1; lineStart_ = true; }
        else ++column_;
    }
}
Token Lexer::token(TokenKind kind, uint64_t start, uint64_t line, uint64_t column) const {
    return {kind, text_.substr(start, cursor_ - start), {start, cursor_, line, column, {}}};
}
[[noreturn]] void Lexer::error(const std::string& message) {
    const Range range{cursor_, cursor_ + (cursor_ < end_), line_, column_, {}};
    if (cursor_ < end_) advance();
    throw Error(range, message);
}
const Token& Lexer::peek() {
    if (!lookahead_) lookahead_ = next();
    return *lookahead_;
}
Token Lexer::take() { auto result = peek(); lookahead_.reset(); return result; }

void Lexer::jsonSpace() {
    while (cursor_ < end_ && (text_[cursor_] == ' ' || text_[cursor_] == '\t' ||
           text_[cursor_] == '\n' || text_[cursor_] == '\r')) advance();
}
void Lexer::jsonString() {
    if (cursor_ == end_ || text_[cursor_] != '"') error("expected a JSON string in FIRRTL annotations");
    advance();
    while (cursor_ < end_) {
        const auto c = text_[cursor_]; advance();
        if (c == '"') return;
        if (static_cast<unsigned char>(c) < 0x20) error("control character in annotation string");
        if (c == '\\') {
            if (cursor_ == end_) break;
            const auto escape = text_[cursor_]; advance();
            if (escape == 'u') {
                for (unsigned i = 0; i < 4; ++i) {
                    if (cursor_ == end_ || !hex(text_[cursor_])) error("invalid annotation Unicode escape");
                    advance();
                }
            } else if (std::string_view("\"\\/bfnrt").find(escape) == std::string_view::npos)
                error("invalid annotation string escape");
        }
    }
    error("unterminated annotation string");
}
void Lexer::jsonValue(std::size_t depth) {
    if (depth > kMaxNesting) error("annotation nesting limit exceeded");
    jsonSpace();
    if (cursor_ == end_) error("truncated annotation JSON");
    const char c = text_[cursor_];
    if (c == '"') { jsonString(); return; }
    if (c == '[' || c == '{') {
        const char close = c == '[' ? ']' : '}';
        advance(); jsonSpace();
        if (cursor_ < end_ && text_[cursor_] == close) { advance(); return; }
        while (cursor_ < end_) {
            if (c == '{') {
                jsonString(); jsonSpace();
                if (cursor_ == end_ || text_[cursor_] != ':') error("expected ':' in annotation object");
                advance();
            }
            jsonValue(depth + 1); jsonSpace();
            if (cursor_ < end_ && text_[cursor_] == close) { advance(); return; }
            if (cursor_ == end_ || text_[cursor_] != ',') error("expected ',' or closing delimiter in annotations");
            advance(); jsonSpace();
        }
        error("truncated annotation JSON");
    }
    for (const auto word : {std::string_view("true"), std::string_view("false"), std::string_view("null")})
        if (text_.substr(cursor_, word.size()) == word) {
            for (std::size_t i = 0; i < word.size(); ++i) advance();
            return;
        }
    if (c == '-') advance();
    if (cursor_ == end_ || !digit(text_[cursor_])) error("invalid annotation JSON value");
    if (text_[cursor_] == '0') advance();
    else while (cursor_ < end_ && digit(text_[cursor_])) advance();
    if (cursor_ < end_ && text_[cursor_] == '.') {
        advance();
        if (cursor_ == end_ || !digit(text_[cursor_])) error("invalid JSON fraction");
        while (cursor_ < end_ && digit(text_[cursor_])) advance();
    }
    if (cursor_ < end_ && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
        advance();
        if (cursor_ < end_ && (text_[cursor_] == '-' || text_[cursor_] == '+')) advance();
        if (cursor_ == end_ || !digit(text_[cursor_])) error("invalid JSON exponent");
        while (cursor_ < end_ && digit(text_[cursor_])) advance();
    }
}

Token Lexer::next() {
    if (!pending_.empty()) { auto result = pending_.front(); pending_.pop_front(); return result; }
    while (cursor_ < end_) {
        if (lineStart_ && groups_.empty()) {
            uint64_t indent = 0;
            while (cursor_ < end_ && text_[cursor_] == ' ') { ++indent; advance(); }
            if (cursor_ == end_) break;
            const auto c = text_[cursor_];
            if (c == '\t') error("tabs are illegal in FIRRTL indentation");
            if (c == ';' || c == '\n' || c == '\r') {
                while (cursor_ < end_ && text_[cursor_] != '\n') advance();
                if (cursor_ < end_) advance();
                lineContent_ = false;
                continue;
            }
            lineStart_ = false;
            const auto location = Range{cursor_, cursor_, line_, column_, {}};
            if (indent > indents_.back()) {
                if (indents_.size() >= kMaxNesting) error("indentation nesting limit exceeded");
                indents_.push_back(indent);
                return {TokenKind::Indent, {}, location};
            }
            while (indent < indents_.back() && indents_.size() > 1) {
                indents_.pop_back(); pending_.push_back({TokenKind::Dedent, {}, location});
            }
            if (indent != indents_.back()) { pending_.clear(); error("dedent does not match an enclosing indent level"); }
            if (!pending_.empty()) { auto result = pending_.front(); pending_.pop_front(); return result; }
        }
        const char c = text_[cursor_];
        if (c == ' ') { advance(); continue; }
        if (c == '\t') error("tabs are illegal outside FIRRTL strings");
        if (c == '\r' && cursor_ + 1 < end_ && text_[cursor_ + 1] == '\n') { advance(); continue; }
        if (c == ';') { while (cursor_ < end_ && text_[cursor_] != '\n') advance(); continue; }
        if (c == '\n') {
            const auto start = cursor_, line = line_, column = column_;
            advance();
            if (!groups_.empty()) continue;
            lineContent_ = false;
            return token(TokenKind::Newline, start, line, column);
        }
        const auto start = cursor_, line = line_, column = column_;
        lineContent_ = true; lineStart_ = false;
        if (c == '@' && cursor_ + 1 < end_ && text_[cursor_ + 1] == '[') {
            advance(); advance();
            while (cursor_ < end_ && text_[cursor_] != '\n') {
                const auto part = text_[cursor_]; advance();
                if (part == ']') return token(TokenKind::Info, start, line, column);
                if (part == '\\') {
                    if (cursor_ == end_ || std::string_view("nt]\\").find(text_[cursor_]) == std::string_view::npos)
                        error("invalid source info escape");
                    advance();
                }
            }
            throw Error({start, cursor_, line, column, {}}, "unterminated FIRRTL source info");
        }
        if (c == '%' && cursor_ + 2 < end_ && text_.substr(cursor_, 3) == "%[[") {
            advance(); advance(); jsonValue(0); jsonSpace();
            if (cursor_ == end_ || text_[cursor_] != ']') error("missing annotation closing ']'");
            advance(); lineStart_ = false;
            return token(TokenKind::Annotation, start, line, column);
        }
        if (c == '"' || c == '\'') {
            advance();
            while (cursor_ < end_ && text_[cursor_] != '\n') {
                const auto part = text_[cursor_]; advance();
                if (part == c) {
                    const auto result = token(c == '"' ? TokenKind::String : TokenKind::RawString, start, line, column);
                    try { (void)decodeString(result.text); }
                    catch (const std::exception& error) { throw Error(result.range, error.what()); }
                    return result;
                }
                if (part == '\\' && cursor_ < end_ && text_[cursor_] != '\n') advance();
            }
            throw Error({start, cursor_, line, column, {}}, "unterminated FIRRTL string");
        }
        if (c == '`') {
            advance(); const auto nameStart = cursor_;
            while (cursor_ < end_ && (alpha(text_[cursor_]) || digit(text_[cursor_]))) advance();
            if (cursor_ == end_ || text_[cursor_] != '`' || cursor_ == nameStart) error("invalid literal identifier");
            advance();
            return token(TokenKind::LiteralId, start, line, column);
        }
        if (alpha(c)) {
            advance();
            while (cursor_ < end_ && (alpha(text_[cursor_]) || digit(text_[cursor_]) || text_[cursor_] == '-')) advance();
            return token(TokenKind::Word, start, line, column);
        }
        if (digit(c) || (c == '-' && cursor_ + 1 < end_ && digit(text_[cursor_ + 1]))) {
            advance();
            while (cursor_ < end_) {
                const auto part = text_[cursor_];
                if (alpha(part) || digit(part) || ((part == '-' || part == '+') && cursor_ > start &&
                    (text_[cursor_ - 1] == 'e' || text_[cursor_ - 1] == 'E'))) advance();
                else break;
            }
            return token(TokenKind::Number, start, line, column);
        }
        if (c == '=' && cursor_ + 1 < end_ && text_[cursor_ + 1] == '>') {
            advance(); advance(); return token(TokenKind::Symbol, start, line, column);
        }
        if (c == '{' && cursor_ + 1 < end_ && text_[cursor_ + 1] == '|') {
            advance(); advance(); groups_.push_back('{');
            return token(TokenKind::Symbol, start, line, column);
        }
        if (c == '|' && cursor_ + 1 < end_ && text_[cursor_ + 1] == '}') {
            if (groups_.empty() || groups_.back() != '{') error("unmatched enum delimiter");
            groups_.pop_back(); advance(); advance(); return token(TokenKind::Symbol, start, line, column);
        }
        if (std::string_view("()[]{}<>").find(c) != std::string_view::npos) {
            if (c == '(' || c == '[' || c == '{' || c == '<') {
                if (groups_.size() >= kMaxNesting) error("delimiter nesting limit exceeded");
                groups_.push_back(c);
            } else {
                const auto expected = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : '<';
                if (groups_.empty() || groups_.back() != expected) error("unmatched FIRRTL delimiter");
                groups_.pop_back();
            }
            advance(); return token(TokenKind::Symbol, start, line, column);
        }
        if (std::string_view(":,.=").find(c) != std::string_view::npos) {
            advance(); return token(TokenKind::Symbol, start, line, column);
        }
        error("unexpected character in FIRRTL source");
    }
    const auto location = Range{cursor_, cursor_, line_, column_, {}};
    if (!groups_.empty()) { groups_.clear(); throw Error(location, "truncated FIRRTL delimiter group"); }
    if (lineContent_) { lineContent_ = false; return {TokenKind::Newline, {}, location}; }
    if (indents_.size() > 1) { indents_.pop_back(); return {TokenKind::Dedent, {}, location}; }
    return {TokenKind::End, {}, location};
}
} // namespace wolvrix::lib::ingest::firrtl::detail
