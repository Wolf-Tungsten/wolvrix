#ifndef WOLVRIX_INGEST_FIRRTL_INTERNAL_HPP
#define WOLVRIX_INGEST_FIRRTL_INTERNAL_HPP

#include "ingest/firrtl_syntax.hpp"

#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace wolvrix::lib::ingest::firrtl {

namespace detail {
class InterfaceSummary;
struct ParsedModuleCache {
    std::mutex mutex;
    bool parsed = false;
    std::shared_ptr<const ModuleBody> body;
    std::vector<IngestDiagnostic> diagnostics;
    bool summarized = false;
    std::vector<std::string> summaryLayers;
    std::shared_ptr<const InterfaceSummary> summary;
    std::exception_ptr summaryFailure;
};
struct SharedModuleResult {
    std::shared_ptr<const Source> source;
    std::shared_ptr<const ModuleBody> sharedBody;
    std::vector<IngestDiagnostic> diagnostics;
    bool success() const noexcept;
};
// Production callers borrow the cached AST; the public parseModule API keeps
// returning an owning compatibility copy.
SharedModuleResult parseSharedModule(std::shared_ptr<const Source> source, std::size_t module);
// Called after a module has been lowered; outstanding shared readers stay valid.
void releaseParsedModule(const Source& source, std::size_t module);
}

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
    // Workers parse independent modules once, then reuse their inference
    // summaries and ASTs. Each slot has its own lock for lazy callers.
    mutable std::mutex parsedModulesMutex;
    mutable std::vector<std::shared_ptr<detail::ParsedModuleCache>> parsedModules;
    ~Impl();
};

namespace detail {
// Borrows range.info or the immutable Source text; callers own any cached copy.
std::string_view sourceInfoAt(const Source& source, Range range);

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
    // Resume the circuit lexer after a byte-indexed module, preserving its
    // enclosing indentation stack without lexing any module body tokens.
    void skipTo(uint64_t begin, uint64_t line) {
        cursor_ = begin; line_ = line; column_ = 1;
        lineStart_ = true; lineContent_ = false;
        groups_.clear(); pending_.clear(); lookahead_.reset();
    }
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
