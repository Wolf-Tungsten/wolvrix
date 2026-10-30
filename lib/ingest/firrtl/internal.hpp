#ifndef WOLVRIX_INGEST_FIRRTL_INTERNAL_HPP
#define WOLVRIX_INGEST_FIRRTL_INTERNAL_HPP

#include "ingest/firrtl_syntax.hpp"

#include <deque>
#include <stdexcept>

namespace wolvrix::lib::ingest::firrtl {

struct Source::Impl {
    std::filesystem::path path;
    std::string unit;
    std::string storage;
    const char* mapped = nullptr;
    uint64_t mappedSize = 0;
    std::string_view text;
    std::string_view version;
    std::string_view circuit;
    std::string_view annotations;
    Arena arena;
    std::vector<Module> modules;
    std::vector<Layer> layers;
    std::vector<TypeAlias> aliases;
    std::vector<Formal> formals;
    std::unordered_map<std::string_view, std::size_t> moduleNames;
    ~Impl();
};

namespace detail {
inline constexpr std::size_t kMaxErrors = 32;
inline constexpr std::size_t kMaxNesting = 256;

struct Access {
    static Source::Impl& get(Source& source) { return *source.impl_; }
    static const Source::Impl& get(const Source& source) { return *source.impl_; }
};

enum class TokenKind { Word, LiteralId, Number, String, RawString, Info, Annotation,
                       Symbol, Newline, Indent, Dedent, End };
struct Token {
    TokenKind kind = TokenKind::End;
    std::string_view text;
    Range range;
};
struct Error : std::runtime_error {
    Range range;
    Error(Range range, const std::string& message) : std::runtime_error(message), range(range) {}
};

class Lexer {
public:
    explicit Lexer(std::string_view text, uint64_t begin = 0, uint64_t end = UINT64_MAX,
                   uint64_t line = 1, uint64_t baseIndent = 0);
    const Token& peek();
    Token take();
private:
    Token next();
    Token token(TokenKind kind, uint64_t start, uint64_t line, uint64_t column) const;
    void advance();
    void jsonValue(std::size_t depth);
    void jsonSpace();
    void jsonString();
    [[noreturn]] void error(const std::string& message);
    std::string_view text_;
    uint64_t cursor_, end_, line_, column_ = 1;
    bool lineStart_ = true;
    bool lineContent_ = false;
    std::vector<uint64_t> indents_;
    std::vector<char> groups_;
    std::deque<Token> pending_;
    std::optional<Token> lookahead_;
};

bool identifier(std::string_view text, bool literal = false);
bool integer(std::string_view text, bool radix = false);
int64_t integer64(const Token& token, bool nonnegative = false);
IngestDiagnostic diagnostic(const Source& source, const Error& error, std::string_view module = {});
IndexResult index(std::unique_ptr<Source::Impl> impl, const FirrtlOptions& options);

} // namespace detail
} // namespace wolvrix::lib::ingest::firrtl
#endif
