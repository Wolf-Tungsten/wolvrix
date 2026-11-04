#include "internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>

namespace wolvrix::lib::ingest::firrtl {
namespace detail {
namespace {
struct Nesting {
    std::size_t& depth;
    Nesting(std::size_t& depth, Range range) : depth(depth) {
        if (++depth > kMaxNesting) { --depth; throw Error(range, "FIRRTL syntax nesting limit exceeded"); }
    }
    ~Nesting() { --depth; }
};
struct LocalScope {
    std::vector<std::unordered_set<std::string_view>>& scopes;
    std::size_t previous;
    explicit LocalScope(std::vector<std::unordered_set<std::string_view>>& scopes)
        : scopes(scopes), previous(scopes.size()) { scopes.emplace_back(); }
    ~LocalScope() { scopes.resize(previous); }
};
std::string_view statementName(StmtKind kind) {
    static constexpr std::string_view names[] = {
        "node", "wire", "reg", "regreset", "inst", "object", "mem", "cmem", "smem", "mport",
        "connect", "invalidate", "attach", "define", "propassign", "when", "match", "match-case",
        "layerblock", "stop", "printf", "fprintf", "fflush", "assert", "assume", "cover", "intrinsic",
        "force", "force_initial", "release", "release_initial", "propassert", "skip"
    };
    return names[static_cast<unsigned>(kind)];
}

struct ModuleBytes {
    uint64_t end;
    uint64_t endLine;
    uint64_t interfaceEnd;
};

// Recognize only indentation and lexical boundaries. No body token, identifier,
// literal, or syntax node is constructed during the circuit index pass.
ModuleBytes moduleBytes(std::string_view text, Range start) {
    const uint64_t indentation = start.column - 1;
    uint64_t cursor = start.begin - indentation, line = start.line;
    uint64_t interfaceEnd = text.size();
    uint64_t groups = 0;
    bool first = true, interface = true;
    while (cursor < text.size()) {
        const auto lineBegin = cursor;
        auto content = cursor;
        while (content < text.size() && text[content] == ' ') ++content;
        const bool blank = content == text.size() || text[content] == '\n' ||
            text[content] == '\r' || text[content] == ';';
        if (!first && !groups && !blank) {
            if (content - lineBegin <= indentation) {
                if (interface) interfaceEnd = lineBegin;
                return {lineBegin, line, interfaceEnd};
            }
            if (interface) {
                auto wordEnd = content;
                while (wordEnd < text.size() && ((text[wordEnd] >= 'a' && text[wordEnd] <= 'z') || text[wordEnd] == '-')) ++wordEnd;
                const auto word = text.substr(content, wordEnd - content);
                if (word != "input" && word != "output" && word != "defname" && word != "intrinsic" && word != "parameter") {
                    interface = false; interfaceEnd = lineBegin;
                }
            }
        }
        char quote = 0;
        bool info = false, comment = false;
        while (cursor < text.size()) {
            const auto c = text[cursor++];
            if (c == '\n') { ++line; break; }
            if (comment) continue;
            if (quote || info) {
                if (c == '\\' && cursor < text.size() && text[cursor] != '\n') { ++cursor; continue; }
                if (quote && c == quote) quote = 0;
                else if (info && c == ']') info = false;
                continue;
            }
            if (c == ';') { comment = true; continue; }
            if (c == '\"' || c == '\'') { quote = c; continue; }
            if (c == '@' && cursor < text.size() && text[cursor] == '[') { ++cursor; info = true; continue; }
            if (c == '(' || c == '[' || c == '{' || c == '<') ++groups;
            else if (c == ')' || c == ']' || c == '}' || c == '>') {
                if (groups) --groups; // Full validation belongs to the one body parse.
            }
        }
        first = false;
    }
    return {cursor, line, interfaceEnd};
}

class Parser {
public:
    Parser(const Source& source, Arena& arena, std::vector<IngestDiagnostic>& diagnostics,
           uint64_t begin = 0, uint64_t end = UINT64_MAX, uint64_t line = 1, uint64_t indent = 0)
        : source_(source), arena_(arena), lexer_(source.text(), begin, end, line, indent), diagnostics_(diagnostics) {}

    void index(Source::Impl& output, const FirrtlOptions& options, bool byteOnly = true);
    ModuleBody module();

private:
    const Token& peek() { return lexer_.peek(); }
    Token take() { return lexer_.take(); }
    bool at(std::string_view text) { return peek().text == text && peek().kind != TokenKind::LiteralId; }
    bool at(TokenKind kind) { return peek().kind == kind; }
    bool accept(std::string_view text) { if (!at(text)) return false; take(); return true; }
    bool accept(TokenKind kind) { if (!at(kind)) return false; take(); return true; }
    Token expect(std::string_view text) {
        if (!at(text)) fail("expected '" + std::string(text) + "'");
        return take();
    }
    [[noreturn]] void fail(const std::string& message) {
        throw Error(peek().range, message + (peek().text.empty() ? " at end of line/block" : " before '" + std::string(peek().text) + "'"));
    }
    std::string_view name();
    std::string_view path();
    Token number(bool radix = false);
    Parameter parameter();
    TypeId type();
    ExprId expression(bool property = false);
    ExprId reference(bool dynamic = true);
    ExprId intrinsic();
    ExprId add(Expression expression) {
        if (arena_.expressions.size() >= std::numeric_limits<ExprId>::max()) fail("too many module expressions");
        const auto id = static_cast<ExprId>(arena_.expressions.size());
        arena_.expressions.push_back(std::move(expression)); return id;
    }
    StmtId add(Statement statement) {
        if (arena_.statements.size() >= std::numeric_limits<StmtId>::max()) fail("too many module statements");
        const auto id = static_cast<StmtId>(arena_.statements.size());
        arena_.statements.push_back(std::move(statement)); return id;
    }
    void line(Range& range);
    Module header(bool& indented);
    std::vector<StmtId> statements();
    std::vector<StmtId> suite(Range& range, bool allowEmpty = false);
    StmtId statement();
    Layer layer();
    FormalValue formalValue();
    void report(const Error& error);
    void recover(bool top = false);
    void declare(std::string_view name, Range range, std::optional<std::size_t> scope = {});
    void requireReference(std::string_view name, Range range);
    ExprId stringExpression();
    ExprId integerExpression();
    void command(Statement& statement, std::string_view word);

    const Source& source_;
    Arena& arena_;
    Lexer lexer_;
    std::vector<IngestDiagnostic>& diagnostics_;
    std::string_view module_;
    std::size_t depth_ = 0;
    bool indexing_ = false;
    std::unordered_set<std::string_view> allNames_;
    std::vector<std::unordered_set<std::string_view>> scopes_;
    std::unordered_map<std::string_view, std::size_t> memoryScopes_;
};

std::string_view Parser::name() {
    const auto token = take();
    auto text = token.text;
    if (token.kind == TokenKind::LiteralId) text = text.substr(1, text.size() - 2);
    if ((token.kind != TokenKind::Word && token.kind != TokenKind::LiteralId) ||
        !identifier(text, token.kind == TokenKind::LiteralId)) throw Error(token.range, "expected a FIRRTL identifier");
    return arena_.intern(text);
}
std::string_view Parser::path() {
    const auto begin = peek().range.begin;
    name();
    while (accept(".")) name();
    // Preserve separators/backticks in paths; individual names remain interned.
    const auto end = peek().range.begin;
    auto result = source_.text().substr(begin, end - begin);
    while (!result.empty() && result.back() == ' ') result.remove_suffix(1);
    return arena_.intern(result);
}
Token Parser::number(bool radix) {
    const auto token = take();
    if (token.kind != TokenKind::Number || !integer(token.text, radix))
        throw Error(token.range, radix ? "expected an integer literal" : "expected a decimal integer");
    return token;
}
Parameter Parser::parameter() {
    Parameter result;
    result.range = peek().range;
    if (at(TokenKind::String) || at(TokenKind::RawString)) {
        result.kind = at(TokenKind::String) ? LiteralKind::String : LiteralKind::RawString;
        result.value = take().text;
    } else {
        result.kind = LiteralKind::Integer;
        result.value = number().text;
    }
    return result;
}
void Parser::line(Range& range) {
    if (at(TokenKind::Info)) range.info = take().text;
    if (!at(TokenKind::Newline)) fail("expected a newline");
    range.end = take().range.begin;
}

TypeId Parser::type() {
    Nesting guard(depth_, peek().range);
    Type result;
    result.constant = accept("const");
    if (accept("{")) {
        result.kind = TypeKind::Bundle;
        std::unordered_set<std::string_view> fields;
        while (!at("}")) {
            Field field; field.flip = accept("flip"); field.name = name(); expect(":"); field.type = type();
            if (!fields.insert(field.name).second) fail("duplicate bundle field");
            result.fields.push_back(field);
            if (!accept(",") && !at("}")) fail("expected ',' between bundle fields");
        }
        expect("}");
    } else if (accept("{|")) {
        result.kind = TypeKind::Enum;
        std::unordered_set<std::string_view> fields;
        while (!at("|}")) {
            Field field; field.name = name();
            if (accept(":")) field.type = type();
            if (!fields.insert(field.name).second) fail("duplicate enum alternative");
            result.fields.push_back(field);
            accept(",");
        }
        expect("|}");
    } else {
        const auto location = peek().range;
        const auto word = name();
        if (word == "UInt") result.kind = TypeKind::UInt;
        else if (word == "SInt") result.kind = TypeKind::SInt;
        else if (word == "Clock") result.kind = TypeKind::Clock;
        else if (word == "Reset") result.kind = TypeKind::Reset;
        else if (word == "AsyncReset") result.kind = TypeKind::AsyncReset;
        else if (word == "Analog") result.kind = TypeKind::Analog;
        else if (word == "Probe" || word == "RWProbe") {
            result.kind = word == "Probe" ? TypeKind::Probe : TypeKind::RWProbe;
            expect("<"); result.element = type();
            if (accept(",")) result.layer = path();
            expect(">");
        } else if (word == "Integer") result.kind = TypeKind::Integer;
        else if (word == "String") result.kind = TypeKind::String;
        else if (word == "Bool") result.kind = TypeKind::Bool;
        else if (word == "Double") result.kind = TypeKind::Double;
        else if (word == "Path") result.kind = TypeKind::Path;
        else if (word == "AnyRef") result.kind = TypeKind::AnyRef;
        else if (word == "Inst") { result.kind = TypeKind::Inst; expect("<"); result.name = name(); expect(">"); }
        else if (word == "List") { result.kind = TypeKind::List; expect("<"); result.element = type(); expect(">"); }
        else {
            result.kind = TypeKind::Alias; result.name = word;
            if (!indexing_ && std::none_of(source_.aliases().begin(), source_.aliases().end(),
                                          [&](const auto& alias) { return alias.name == word; }))
                throw Error(location, "unknown FIRRTL type alias '" + std::string(word) + "'");
        }
        if (result.kind == TypeKind::UInt || result.kind == TypeKind::SInt || result.kind == TypeKind::Analog) {
            if (accept("<")) { result.width = integer64(number(), true); expect(">"); }
        }
    }
    auto id = arena_.intern(std::move(result));
    while (accept("[")) {
        Type vector; vector.kind = TypeKind::Vector; vector.element = id;
        vector.extent = integer64(number(), true); expect("]");
        id = arena_.intern(std::move(vector));
    }
    return id;
}

void Parser::declare(std::string_view name, Range range, std::optional<std::size_t> scope) {
    if (!allNames_.insert(name).second) throw Error(range, "duplicate declaration '" + std::string(name) + "' in module namespace");
    scopes_.at(scope.value_or(scopes_.size() - 1)).insert(name);
}
void Parser::requireReference(std::string_view name, Range range) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) if (scope->contains(name)) return;
    throw Error(range, "reference to undeclared or out-of-scope component '" + std::string(name) + "'");
}
ExprId Parser::integerExpression() {
    Expression result; result.kind = ExprKind::Integer; result.range = peek().range; result.literal = number().text;
    return add(std::move(result));
}
ExprId Parser::stringExpression() {
    if (!at(TokenKind::String)) fail("expected a double-quoted string");
    const auto token = take();
    Expression result; result.kind = ExprKind::String; result.range = token.range; result.literal = token.text;
    return add(std::move(result));
}
ExprId Parser::reference(bool dynamic) {
    Expression result; result.range = peek().range; result.name = name(); result.kind = ExprKind::Reference;
    requireReference(result.name, result.range);
    auto id = add(std::move(result));
    while (at(".") || at("[")) {
        Expression access; access.range = arena_.expressions[id].range; access.operands.push_back(id);
        if (accept(".")) { access.kind = ExprKind::Field; access.name = name(); }
        else {
            expect("["); access.kind = ExprKind::Index;
            if (at(TokenKind::Number)) access.operands.push_back(integerExpression());
            else if (dynamic) access.operands.push_back(expression());
            else fail("dynamic index is not allowed in a static reference");
            expect("]");
        }
        access.range.end = peek().range.begin; id = add(std::move(access));
    }
    return id;
}
ExprId Parser::intrinsic() {
    Expression result; result.kind = ExprKind::Intrinsic; result.range = expect("intrinsic").range;
    expect("("); result.name = name();
    if (accept("<")) {
        std::unordered_set<std::string_view> names;
        do {
            const auto parameterName = name(); expect("=");
            auto value = parameter(); value.name = parameterName;
            if (!names.insert(parameterName).second) fail("duplicate intrinsic parameter");
            result.parameters.push_back(value);
        } while (accept(","));
        expect(">");
    }
    if (accept(":")) result.type = type();
    while (accept(",")) result.operands.push_back(expression());
    result.range.end = expect(")").range.end;
    return add(std::move(result));
}

ExprId Parser::expression(bool property) {
    Nesting guard(depth_, peek().range);
    if (at("intrinsic")) return intrinsic();
    if (property && at(TokenKind::String)) return stringExpression();
    Expression result; result.range = peek().range;
    if (at("{|")) {
        result.kind = ExprKind::Enum; result.type = type(); expect("("); result.name = name();
        if (accept(",")) result.operands.push_back(expression());
        result.range.end = expect(")").range.end; return add(std::move(result));
    }
    const auto token = take();
    auto word = token.text;
    if (token.kind == TokenKind::LiteralId) word = word.substr(1, word.size() - 2);
    if ((token.kind != TokenKind::Word && token.kind != TokenKind::LiteralId) ||
        !identifier(word, token.kind == TokenKind::LiteralId)) throw Error(token.range, "expected a FIRRTL expression");
    word = arena_.intern(word); result.name = word;
    if (word == "UInt" || word == "SInt") {
        Type literalType; literalType.kind = word == "UInt" ? TypeKind::UInt : TypeKind::SInt;
        if (accept("<")) { literalType.width = integer64(number(), true); expect(">"); }
        result.type = arena_.intern(std::move(literalType));
        result.kind = word == "UInt" ? ExprKind::UInt : ExprKind::SInt;
        expect("("); result.literal = number(true).text; result.range.end = expect(")").range.end;
        return add(std::move(result));
    }
    if (!at("(") && !(property && word == "List" && at("<"))) {
        result.kind = ExprKind::Reference; requireReference(word, token.range);
        auto id = add(std::move(result));
        while (at(".") || at("[")) {
            Expression access; access.range = token.range; access.operands.push_back(id);
            if (accept(".")) { access.kind = ExprKind::Field; access.name = name(); }
            else {
                expect("["); access.kind = ExprKind::Index;
                access.operands.push_back(at(TokenKind::Number) ? integerExpression() : expression()); expect("]");
            }
            access.range.end = peek().range.begin; id = add(std::move(access));
        }
        return id;
    }
    if (property && word == "List") { expect("<"); result.type = type(); expect(">"); }
    expect("(");
    if (property && (word == "Integer" || word == "Bool" || word == "Double" || word == "path")) {
        result.kind = ExprKind::PropertyLiteral;
        if (word == "Integer") result.literal = number().text;
        else if (word == "Bool") {
            if (!at("true") && !at("false")) fail("expected true or false");
            result.literal = take().text;
        } else if (word == "path") {
            if (!at(TokenKind::String)) fail("expected a path string");
            result.literal = take().text;
        } else {
            const auto begin = peek().range.begin;
            if (!at(TokenKind::Number)) fail("expected a floating-point literal");
            auto end = take().range.end;
            if (accept(".")) { if (!at(TokenKind::Number)) fail("expected a floating-point fraction"); end = take().range.end; }
            result.literal = source_.text().substr(begin, end - begin);
            double value = 0;
            const auto [last, code] = std::from_chars(result.literal.data(), result.literal.data() + result.literal.size(), value);
            if (code != std::errc{} || last != result.literal.data() + result.literal.size() || !std::isfinite(value))
                fail("invalid finite floating-point literal");
        }
        result.range.end = expect(")").range.end; return add(std::move(result));
    }
    if (word == "probe" || word == "rwprobe") {
        result.kind = word == "probe" ? ExprKind::Probe : ExprKind::RWProbe;
        result.operands.push_back(reference(false));
    } else if (word == "read") {
        result.kind = ExprKind::Read; result.operands.push_back(expression());
    } else {
        int count = 0, integers = 0;
        result.kind = ExprKind::Primitive;
        if (word == "mux") { count = 3; result.kind = ExprKind::Mux; }
        else if (word == "add" || word == "sub" || word == "mul" || word == "div" || word == "rem" ||
                 word == "lt" || word == "leq" || word == "gt" || word == "geq" || word == "eq" || word == "neq" ||
                 word == "dshl" || word == "dshr" || word == "and" || word == "or" || word == "xor") count = 2;
        else if (word == "asUInt" || word == "asSInt" || word == "asClock" || word == "asAsyncReset" ||
                 word == "asReset" || word == "cvt" || word == "neg" || word == "not" ||
                 word == "andr" || word == "orr" || word == "xorr") count = 1;
        else if (word == "pad" || word == "shl" || word == "shr" || word == "head" || word == "tail") { count = 1; integers = 1; }
        else if (word == "bits") { count = 1; integers = 2; }
        else if (word == "cat") count = -1;
        else if (property && (word == "integer_add" || word == "integer_mul" || word == "integer_shr" ||
                 word == "integer_shl" || word == "prop_eq" || word == "bool_and" || word == "bool_or" || word == "bool_xor")) {
            count = 2; result.kind = ExprKind::PropertyOperation;
        } else if (property && (word == "List" || word == "list_concat" || word == "string_concat")) {
            count = -1; result.kind = ExprKind::PropertyOperation;
        } else {
            // A named enum alias is also an expression constructor.
            const auto aliases = source_.aliases();
            const auto alias = std::find_if(aliases.begin(), aliases.end(), [&](const auto& alias) { return alias.name == word; });
            if (alias == aliases.end()) throw Error(token.range, "unknown or unsupported FIRRTL expression '" + std::string(word) + "'");
            result.kind = ExprKind::Enum; result.literal = name();
            if (accept(",")) result.operands.push_back(expression());
            result.range.end = expect(")").range.end; return add(std::move(result));
        }
        if (count < 0) {
            if (!at(")")) do { result.operands.push_back(expression(property)); } while (accept(","));
        } else {
            for (int i = 0; i < count; ++i) { if (i) expect(","); result.operands.push_back(expression(property)); }
            for (int i = 0; i < integers; ++i) { expect(","); result.operands.push_back(integerExpression()); }
        }
    }
    result.range.end = expect(")").range.end;
    return add(std::move(result));
}

Module Parser::header(bool& indented) {
    Module result; result.range = peek().range; result.indentation = result.range.column - 1;
    result.isPublic = accept("public");
    const auto kind = take();
    if (kind.text == "module") result.kind = ModuleKind::Module;
    else if (kind.text == "extmodule") result.kind = ModuleKind::External;
    else if (kind.text == "intmodule") result.kind = ModuleKind::Intrinsic;
    else if (kind.text == "class") result.kind = ModuleKind::Class;
    else if (kind.text == "extclass") result.kind = ModuleKind::ExternalClass;
    else throw Error(kind.range, "expected a module or class declaration");
    if (result.isPublic && result.kind != ModuleKind::Module) throw Error(kind.range, "only modules may be public");
    result.name = name(); module_ = result.name; result.implementation = result.name;
    while (at("enablelayer") || at("knownlayer")) {
        const auto enabled = accept("enablelayer");
        if (!enabled) expect("knownlayer");
        auto& paths = enabled ? result.enabledLayers : result.knownLayers;
        paths.push_back(path());
        while (accept(",")) paths.push_back(path());
    }
    expect(":"); line(result.range);
    indented = accept(TokenKind::Indent);
    if (indented) {
        std::unordered_set<std::string_view> ports;
        while (at("input") || at("output")) {
            Port port; port.range = peek().range;
            port.direction = accept("input") ? LeafDirection::Input : (expect("output"), LeafDirection::Output);
            port.name = name(); expect(":"); port.type = type(); line(port.range);
            if (!ports.insert(port.name).second) throw Error(port.range, "duplicate port '" + std::string(port.name) + "'");
            result.ports.push_back(port);
        }
        if (result.kind == ModuleKind::External || result.kind == ModuleKind::Intrinsic || result.kind == ModuleKind::ExternalClass) {
            if (result.kind == ModuleKind::External && accept("defname")) {
                expect("="); result.implementation = name(); auto location = peek().range; line(location);
            }
            if (result.kind == ModuleKind::Intrinsic) {
                expect("intrinsic"); expect("="); result.implementation = name(); auto location = peek().range; line(location);
            }
            std::unordered_set<std::string_view> parameters;
            while (accept("parameter")) {
                const auto parameterName = name(); expect("=");
                auto value = parameter(); value.name = parameterName; line(value.range);
                if (!parameters.insert(parameterName).second) throw Error(value.range, "duplicate module parameter");
                result.parameters.push_back(value);
            }
            if (!at(TokenKind::Dedent) && !at(TokenKind::End)) fail("unexpected external module/class content");
        }
    }
    result.body = peek().range;
    return result;
}

void Parser::report(const Error& error) {
    if (diagnostics_.size() < kMaxErrors) diagnostics_.push_back(diagnostic(source_, error, module_));
}
void Parser::recover(bool top) {
    while (!at(TokenKind::End)) {
        if (at(TokenKind::Dedent)) { if (!top) return; take(); continue; }
        if (accept(TokenKind::Newline)) return;
        take();
    }
}

Layer Parser::layer() {
    Nesting guard(depth_, peek().range);
    Layer result; result.range = expect("layer").range; result.name = name(); expect(",");
    if (!at("bind") && !at("inline")) fail("expected layer convention bind or inline");
    result.convention = take().text;
    if (accept(",")) {
        if (!at(TokenKind::String)) fail("expected layer output directory string");
        result.directory = take().text;
    }
    expect(":"); line(result.range);
    if (accept(TokenKind::Indent)) {
        std::unordered_set<std::string_view> names;
        while (at("layer")) {
            auto child = layer();
            if (!names.insert(child.name).second) throw Error(child.range, "duplicate nested layer");
            result.children.push_back(std::move(child));
        }
        if (!accept(TokenKind::Dedent)) fail("expected end of layer declaration");
    }
    return result;
}

FormalValue Parser::formalValue() {
    Nesting guard(depth_, peek().range);
    FormalValue result;
    if (accept("[")) {
        result.kind = FormalValue::Kind::Array;
        if (!at("]")) do { result.children.push_back(formalValue()); } while (accept(","));
        expect("]");
    } else if (accept("{")) {
        result.kind = FormalValue::Kind::Object;
        if (!at("}")) do {
            const auto key = name(); expect("="); auto child = formalValue(); child.name = key;
            result.children.push_back(std::move(child));
        } while (accept(","));
        expect("}");
    } else {
        const auto value = parameter(); result.value = value.value;
        result.kind = value.kind == LiteralKind::Integer ? FormalValue::Kind::Integer :
                      value.kind == LiteralKind::String ? FormalValue::Kind::String : FormalValue::Kind::RawString;
    }
    return result;
}

void Parser::index(Source::Impl& output, const FirrtlOptions& options, bool byteOnly) {
    indexing_ = true;
    const auto start = expect("FIRRTL"); expect("version");
    const auto begin = peek().range.begin;
    integer64(number(), true); expect("."); integer64(number(), true); expect("."); const auto patch = number(); integer64(patch, true);
    output.version = source_.text().substr(begin, patch.range.end - begin);
    if (output.version != "6.0.0") throw Error(start.range, "unsupported FIRRTL version '" + std::string(output.version) + "' (supported: 6.0.0)");
    if (options.expectedVersion && *options.expectedVersion != output.version)
        throw Error(start.range, "FIRRTL version does not match expectedVersion='" + *options.expectedVersion + "'");
    auto versionRange = start.range; line(versionRange);
    const auto circuit = expect("circuit");
    if (circuit.range.column != 1) throw Error(circuit.range, "circuit declaration must have indentation zero");
    output.circuit = name(); expect(":");
    if (at(TokenKind::Annotation)) {
        const auto annotation = take(); output.annotations = annotation.text.substr(2, annotation.text.size() - 3);
    }
    auto circuitRange = circuit.range; line(circuitRange);
    if (!accept(TokenKind::Indent)) fail("circuit requires an indented declaration block");
    std::unordered_set<std::string_view> names, aliasNames;
    while (!at(TokenKind::Dedent) && !at(TokenKind::End) && diagnostics_.size() < kMaxErrors) {
        module_ = {};
        try {
            if (at("layer")) {
                auto declaration = layer();
                if (!names.insert(declaration.name).second) throw Error(declaration.range, "duplicate circuit declaration");
                output.layers.push_back(std::move(declaration));
            } else if (accept("type")) {
                TypeAlias alias; alias.range = peek().range; alias.name = name(); expect("="); alias.type = type(); line(alias.range);
                if (!aliasNames.insert(alias.name).second) throw Error(alias.range, "duplicate type alias");
                output.aliases.push_back(alias);
            } else if (at("formal")) {
                Formal formal; formal.range = take().range; formal.name = name(); expect("of"); formal.module = name(); expect(":"); line(formal.range);
                if (accept(TokenKind::Indent)) {
                    while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
                        const auto key = name(); expect("="); auto value = formalValue(); value.name = key;
                        auto range = peek().range; line(range); formal.parameters.push_back(std::move(value));
                    }
                    if (!accept(TokenKind::Dedent)) fail("truncated formal declaration");
                }
                if (!names.insert(formal.name).second) throw Error(formal.range, "duplicate circuit declaration");
                output.formals.push_back(std::move(formal));
            } else {
                bool indented = false;
                Module declaration;
                std::optional<ModuleBytes> bytes;
                if (byteOnly) {
                    const auto start = peek().range;
                    bytes = moduleBytes(source_.text(), start);
                    Parser interfaceParser(source_, arena_, diagnostics_, start.begin - start.column + 1,
                        bytes->interfaceEnd, start.line, start.column - 1);
                    interfaceParser.indexing_ = true;
                    declaration = interfaceParser.header(indented);
                } else declaration = header(indented);
                module_ = declaration.name;
                if (!names.insert(declaration.name).second) throw Error(declaration.range, "duplicate circuit declaration");
                if (bytes) declaration.range.end = bytes->end;
                else if (indented) {
                    unsigned depth = 1;
                    while (depth && !at(TokenKind::End)) {
                        const auto token = take();
                        if (token.kind == TokenKind::Indent) ++depth;
                        else if (token.kind == TokenKind::Dedent) --depth;
                        declaration.range.end = token.range.begin;
                    }
                    if (depth) fail("truncated module declaration");
                } else declaration.range.end = peek().range.begin;
                declaration.body.end = declaration.range.end;
                output.moduleNames.emplace(declaration.name, output.modules.size());
                output.modules.push_back(std::move(declaration));
                if (bytes) lexer_.skipTo(bytes->end, bytes->endLine);
            }
        } catch (const Error& error) { report(error); recover(true); }
    }
    accept(TokenKind::Dedent);
    if (!at(TokenKind::End)) fail("unexpected content after circuit");
    if (!output.moduleNames.contains(output.circuit)) throw Error(circuit.range, "circuit has no main module with the same name");
    const auto main = output.modules[output.moduleNames.at(output.circuit)].kind;
    if (main == ModuleKind::Class || main == ModuleKind::ExternalClass) throw Error(circuit.range, "a class cannot be the main module");
    for (const auto& formal : output.formals)
        if (!output.moduleNames.contains(formal.module)) throw Error(formal.range, "formal refers to an unknown module");
    std::unordered_map<std::string_view, TypeId> aliases;
    for (const auto& alias : output.aliases) aliases.emplace(alias.name, alias.type);
    std::vector<uint8_t> states(output.arena.types.size(), 0);
    std::function<void(TypeId, Range, std::size_t)> checkType = [&](TypeId id, Range location, std::size_t depth) {
        if (!id || states[id] == 2) return;
        if (depth > kMaxNesting) throw Error(location, "FIRRTL type alias nesting limit exceeded");
        if (states[id] == 1) throw Error(location, "cyclic FIRRTL type alias");
        states[id] = 1;
        const auto& type = output.arena.types[id];
        if (type.kind == TypeKind::Alias) {
            const auto found = aliases.find(type.name);
            if (found == aliases.end()) throw Error(location, "unknown FIRRTL type alias '" + std::string(type.name) + "'");
            checkType(found->second, location, depth + 1);
        }
        checkType(type.element, location, depth + 1);
        for (const auto& field : type.fields) checkType(field.type, location, depth + 1);
        states[id] = 2;
    };
    for (const auto& alias : output.aliases) checkType(alias.type, alias.range, 0);
    for (const auto& declaration : output.modules) for (const auto& port : declaration.ports) checkType(port.type, port.range, 0);
}

ModuleBody Parser::module() {
    ModuleBody result;
    bool indented = false; result.interface = header(indented);
    scopes_.emplace_back();
    for (const auto& port : result.interface.ports) declare(port.name, port.range);
    if (indented) {
        if (result.interface.kind == ModuleKind::Module || result.interface.kind == ModuleKind::Class)
            result.statements = statements();
        else if (!at(TokenKind::Dedent)) fail("unexpected external module content");
        if (!accept(TokenKind::Dedent)) fail("truncated module body");
    }
    if (!at(TokenKind::End)) fail("unexpected content after module body");
    return result;
}

std::vector<StmtId> Parser::statements() {
    std::vector<StmtId> result;
    while (!at(TokenKind::Dedent) && !at(TokenKind::End) && diagnostics_.size() < kMaxErrors) {
        try { result.push_back(statement()); }
        catch (const Error& error) { report(error); recover(); }
    }
    return result;
}
std::vector<StmtId> Parser::suite(Range& range, bool allowEmpty) {
    LocalScope scope(scopes_);
    std::vector<StmtId> result;
    if (at(TokenKind::Info) || at(TokenKind::Newline)) {
        line(range);
        if (accept(TokenKind::Indent)) { result = statements(); if (!accept(TokenKind::Dedent)) fail("truncated statement block"); }
        else if (!allowEmpty) fail("expected an indented statement block");
    } else result.push_back(statement());
    return result;
}

void Parser::command(Statement& result, std::string_view word) {
    expect("(");
    if (word == "force_initial" || word == "release_initial") {
        result.operands.push_back(expression());
        if (word == "force_initial") { expect(","); result.operands.push_back(expression()); }
    } else {
        result.operands.push_back(expression()); expect(","); result.operands.push_back(expression());
        if (word == "stop") { expect(","); result.operands.push_back(integerExpression()); }
        else if (word == "force" || word == "release") {
            expect(","); result.operands.push_back(expression());
            if (word == "force") { expect(","); result.operands.push_back(expression()); }
        } else {
            if (word == "assert" || word == "assume" || word == "cover") {
                expect(","); result.operands.push_back(expression());
            }
            if (word != "fflush" || at(",")) {
                expect(","); result.operands.push_back(stringExpression());
                unsigned strings = 1;
                while (word != "cover" && accept(",")) {
                    if (word == "fprintf" && at(TokenKind::String)) { result.operands.push_back(stringExpression()); ++strings; }
                    else result.operands.push_back(expression());
                }
                if (word == "fprintf" && strings != 2) fail("fprintf requires filename and format strings");
            }
        }
    }
    expect(")");
    if (accept(":")) result.name = name();
}

StmtId Parser::statement() {
    Nesting guard(depth_, peek().range);
    Statement result; result.range = peek().range;
    const auto word = take();
    if (word.kind != TokenKind::Word) throw Error(word.range, "expected a FIRRTL statement keyword");
    if (word.text == "wire" || word.text == "reg" || word.text == "regreset") {
        result.kind = word.text == "wire" ? StmtKind::Wire : word.text == "reg" ? StmtKind::Reg : StmtKind::RegReset;
        result.name = name(); expect(":"); result.type = type(); declare(result.name, result.range);
        if (word.text != "wire") { expect(","); result.operands.push_back(expression()); }
        if (word.text == "regreset") {
            expect(","); result.operands.push_back(expression()); expect(","); result.operands.push_back(expression());
        }
    } else if (word.text == "node") {
        result.kind = StmtKind::Node; result.name = name(); expect("="); result.operands.push_back(expression());
        declare(result.name, result.range);
    } else if (word.text == "inst" || word.text == "object") {
        result.kind = word.text == "inst" ? StmtKind::Instance : StmtKind::Object;
        result.name = name(); expect("of"); result.target = name();
        const auto target = source_.findModule(result.target);
        if (!target) throw Error(result.range, "instance refers to unknown module/class '" + std::string(result.target) + "'");
        const auto kind = source_.modules()[*target].kind;
        const bool isClass = kind == ModuleKind::Class || kind == ModuleKind::ExternalClass;
        if (isClass != (result.kind == StmtKind::Object)) throw Error(result.range, "instance/object target has the wrong declaration kind");
        declare(result.name, result.range);
    } else if (word.text == "connect" || word.text == "invalidate" || word.text == "define" || word.text == "propassign") {
        result.kind = word.text == "connect" ? StmtKind::Connect : word.text == "invalidate" ? StmtKind::Invalidate :
                      word.text == "define" ? StmtKind::Define : StmtKind::PropAssign;
        result.operands.push_back(reference(word.text == "connect" || word.text == "invalidate"));
        if (word.text != "invalidate") {
            expect(word.text == "define" ? "=" : ",");
            if (word.text == "define" && !at("probe") && !at("rwprobe")) result.operands.push_back(reference(false));
            else result.operands.push_back(expression(word.text == "propassign"));
        }
    } else if (word.text == "attach") {
        result.kind = StmtKind::Attach; expect("("); result.operands.push_back(reference());
        while (accept(",")) result.operands.push_back(reference()); expect(")");
    } else if (word.text == "when") {
        result.kind = StmtKind::When; result.operands.push_back(expression()); expect(":"); result.body = suite(result.range);
        if (accept("else")) {
            if (at("when")) { LocalScope scope(scopes_); result.otherwise.push_back(statement()); }
            else { expect(":"); auto location = peek().range; result.otherwise = suite(location); }
        }
        return add(std::move(result));
    } else if (word.text == "layerblock") {
        result.kind = StmtKind::LayerBlock; result.target = path(); expect(":"); result.body = suite(result.range);
        return add(std::move(result));
    } else if (word.text == "match") {
        result.kind = StmtKind::Match; result.operands.push_back(expression()); expect(":"); line(result.range);
        if (accept(TokenKind::Indent)) {
            std::unordered_set<std::string_view> tags;
            while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
                Statement branch; branch.kind = StmtKind::MatchCase; branch.range = peek().range; branch.target = name();
                if (!tags.insert(branch.target).second) fail("duplicate match branch");
                LocalScope scope(scopes_);
                if (accept("(")) { branch.name = name(); declare(branch.name, branch.range); expect(")"); }
                expect(":"); branch.body = suite(branch.range, true); result.body.push_back(add(std::move(branch)));
            }
            if (!accept(TokenKind::Dedent)) fail("truncated match");
        }
        return add(std::move(result));
    } else if (word.text == "cmem" || word.text == "smem") {
        result.kind = word.text == "cmem" ? StmtKind::CMem : StmtKind::SMem;
        result.name = name(); expect(":"); result.type = type();
        if (at("old") || at("new") || at("undefined")) result.target = take().text;
        declare(result.name, result.range);
        memoryScopes_.emplace(result.name, scopes_.size() - 1);
    } else if (word.text == "read" || word.text == "write" || word.text == "infer" || word.text == "rdwr") {
        result.kind = StmtKind::MPort; result.target = word.text; expect("mport"); result.name = name(); expect("=");
        const auto access = reference();
        const auto& indexed = arena_.expressions[access];
        if (indexed.kind != ExprKind::Index) throw Error(result.range, "mport requires an indexed CHIRRTL memory");
        const auto& base = arena_.expressions[indexed.operands[0]];
        const auto memory = memoryScopes_.find(base.name);
        if (base.kind != ExprKind::Reference || memory == memoryScopes_.end())
            throw Error(result.range, "mport target is not a CHIRRTL memory");
        const auto scope = memory->second;
        result.operands.push_back(access); expect(","); result.operands.push_back(expression());
        // CHIRRTL port data has the memory's scope. Keep this access statement
        // in its original block for enable/clock/address inference during F06.
        declare(result.name, result.range, scope);
    } else if (word.text == "mem") {
        result.kind = StmtKind::Memory; result.name = name(); declare(result.name, result.range); expect(":"); line(result.range);
        if (!accept(TokenKind::Indent)) fail("mem requires an indented property block");
        std::unordered_set<std::string_view> properties, ports;
        while (!at(TokenKind::Dedent) && !at(TokenKind::End)) {
            MemoryField field; field.range = peek().range;
            if (!at(TokenKind::Word)) fail("expected a memory property");
            field.name = take().text; expect("=>");
            if (field.name == "data-type") { field.type = type(); result.type = field.type; }
            else if (field.name == "depth" || field.name == "read-latency" || field.name == "write-latency") {
                const auto value = number(); integer64(value, true); field.value = value.text;
            } else if (field.name == "read-under-write") {
                if (!at("old") && !at("new") && !at("undefined")) fail("invalid read-under-write value");
                field.value = take().text;
            } else if (field.name == "reader" || field.name == "writer" || field.name == "readwriter") {
                field.value = name();
                if (!ports.insert(field.value).second) fail("duplicate memory port");
            } else throw Error(field.range, "unknown memory property '" + std::string(field.name) + "'");
            if (field.name != "reader" && field.name != "writer" && field.name != "readwriter" && !properties.insert(field.name).second)
                throw Error(field.range, "duplicate memory property");
            line(field.range); result.memory.push_back(field);
        }
        if (!accept(TokenKind::Dedent)) fail("truncated memory declaration");
        for (const auto required : {"data-type", "depth", "read-latency", "write-latency"})
            if (!properties.contains(required)) throw Error(result.range, "missing memory property '" + std::string(required) + "'");
        return add(std::move(result));
    } else if (word.text == "stop" || word.text == "printf" || word.text == "fprintf" || word.text == "fflush" ||
               word.text == "assert" || word.text == "assume" || word.text == "cover" || word.text == "force" ||
               word.text == "force_initial" || word.text == "release" || word.text == "release_initial") {
        if (word.text == "stop") result.kind = StmtKind::Stop;
        else if (word.text == "printf") result.kind = StmtKind::Printf;
        else if (word.text == "fprintf") result.kind = StmtKind::Fprintf;
        else if (word.text == "fflush") result.kind = StmtKind::Fflush;
        else if (word.text == "assert") result.kind = StmtKind::Assert;
        else if (word.text == "assume") result.kind = StmtKind::Assume;
        else if (word.text == "cover") result.kind = StmtKind::Cover;
        else if (word.text == "force") result.kind = StmtKind::Force;
        else if (word.text == "force_initial") result.kind = StmtKind::ForceInitial;
        else if (word.text == "release") result.kind = StmtKind::Release;
        else result.kind = StmtKind::ReleaseInitial;
        command(result, word.text);
        if (!result.name.empty() && !allNames_.insert(result.name).second)
            throw Error(result.range, "duplicate declaration for command label '" + std::string(result.name) + "'");
    } else if (word.text == "intrinsic") {
        // The statement keyword has already been consumed.
        Expression call; call.kind = ExprKind::Intrinsic; call.range = word.range; expect("("); call.name = name();
        if (accept("<")) {
            std::unordered_set<std::string_view> names;
            do {
                const auto key = name(); expect("="); auto value = parameter(); value.name = key;
                if (!names.insert(key).second) fail("duplicate intrinsic parameter");
                call.parameters.push_back(value);
            } while (accept(","));
            expect(">");
        }
        if (accept(":")) call.type = type();
        while (accept(",")) call.operands.push_back(expression());
        call.range.end = expect(")").range.end;
        result.kind = StmtKind::Intrinsic; result.operands.push_back(add(std::move(call)));
    } else if (word.text == "propassert") {
        result.kind = StmtKind::PropAssert; result.operands.push_back(expression(true)); expect(","); result.operands.push_back(stringExpression());
    } else if (word.text != "skip") throw Error(word.range, "unknown or unsupported FIRRTL statement '" + std::string(word.text) + "'");
    line(result.range);
    return add(std::move(result));
}

} // namespace

IndexResult index(std::unique_ptr<Source::Impl> impl, const FirrtlOptions& options) {
    IndexResult result;
    auto source = std::make_shared<Source>(std::move(impl));
    auto& output = Access::get(*source);
    const auto run = [&](bool byteOnly) {
        try {
            Parser parser(*source, output.arena, result.diagnostics); parser.index(output, options, byteOnly);
        } catch (const Error& error) {
            if (result.diagnostics.size() < kMaxErrors) result.diagnostics.push_back(diagnostic(*source, error));
        }
    };
    run(true);
    if (!result.diagnostics.empty()) {
        // Only malformed circuit/header input takes this compatibility path.
        // Legacy indexing may encounter an earlier body lexical failure; retain
        // that diagnostic selection/order without rescanning valid inputs.
        output.version = {}; output.circuit = {}; output.annotations = {};
        output.modules.clear(); output.layers.clear(); output.aliases.clear(); output.formals.clear();
        output.moduleNames.clear(); output.arena = Arena{};
        result.diagnostics.clear();
        run(false);
    }
    if (result.diagnostics.empty()) result.source = std::move(source);
    return result;
}
} // namespace detail

detail::SharedModuleResult detail::parseSharedModule(std::shared_ptr<const Source> source, std::size_t module) {
    SharedModuleResult result; result.source = std::move(source);
    if (!result.source || module >= result.source->modules().size()) {
        result.diagnostics.push_back({IngestSeverity::Error, "invalid FIRRTL module index", {}, "ingest-firrtl"}); return result;
    }
    std::shared_ptr<ParsedModuleCache> cached;
    {
        auto& implementation = Access::get(*result.source);
        std::lock_guard lock(implementation.parsedModulesMutex);
        if (implementation.parsedModules.size() < result.source->modules().size())
            implementation.parsedModules.resize(result.source->modules().size());
        auto& slot = implementation.parsedModules[module];
        if (!slot) slot = std::make_shared<ParsedModuleCache>();
        cached = slot;
    }
    std::lock_guard lock(cached->mutex);
    if (cached->parsed) {
        result.sharedBody = cached->body; result.diagnostics = cached->diagnostics;
        return result;
    }
    const auto& declaration = result.source->modules()[module];
    ModuleBody body;
    try {
        detail::Parser parser(*result.source, body.arena, result.diagnostics,
            declaration.range.begin - declaration.indentation, declaration.range.end,
            declaration.range.line, declaration.indentation);
        auto parsed = parser.module();
        body.interface = std::move(parsed.interface); body.statements = std::move(parsed.statements);
        result.sharedBody = std::make_shared<const ModuleBody>(std::move(body));
    } catch (const detail::Error& error) {
        if (result.diagnostics.size() < detail::kMaxErrors)
            result.diagnostics.push_back(detail::diagnostic(*result.source, error, declaration.name));
    }
    cached->body = result.sharedBody; cached->diagnostics = result.diagnostics; cached->parsed = true;
    return result;
}

ModuleResult parseModule(std::shared_ptr<const Source> source, std::size_t module) {
    auto shared = detail::parseSharedModule(std::move(source), module);
    ModuleResult result; result.source = std::move(shared.source);
    result.sharedBody = std::move(shared.sharedBody); result.diagnostics = std::move(shared.diagnostics);
    if (result.sharedBody) result.body = *result.sharedBody;
    return result;
}

ValidationResult validate(std::shared_ptr<const Source> source) {
    ValidationResult result;
    if (!source) { result.diagnostics.push_back({IngestSeverity::Error, "missing FIRRTL source", {}, "ingest-firrtl"}); return result; }
    for (std::size_t i = 0; i < source->modules().size() && result.diagnostics.size() < detail::kMaxErrors; ++i) {
        auto module = parseModule(source, i);
        const auto count = std::min(module.diagnostics.size(), detail::kMaxErrors - result.diagnostics.size());
        result.diagnostics.insert(result.diagnostics.end(), module.diagnostics.begin(), module.diagnostics.begin() + count);
        if (!module.success()) continue;
        ++result.modules;
        const auto& arena = module.body->arena;
        result.statements += arena.statements.size(); result.expressions += arena.expressions.size();
        result.maxModuleStatements = std::max<uint64_t>(result.maxModuleStatements, arena.statements.size());
        for (const auto& statement : arena.statements) {
            ++result.constructs["statement." + std::string(detail::statementName(statement.kind))];
            if (statement.kind == StmtKind::Node) ++result.constructs["node"];
            else if (statement.kind == StmtKind::LayerBlock) ++result.constructs["layerblock"];
            else if (statement.kind == StmtKind::Memory) ++result.constructs["mem"];
            else if (statement.kind == StmtKind::CMem) ++result.constructs["cmem"];
            else if (statement.kind == StmtKind::SMem) ++result.constructs["smem"];
            else if (statement.kind == StmtKind::MPort) ++result.constructs["mport"];
        }
        for (const auto& expression : arena.expressions) {
            if (expression.kind == ExprKind::Primitive || expression.kind == ExprKind::Intrinsic)
                ++result.constructs[std::string(expression.kind == ExprKind::Intrinsic ? "intrinsic." : "primitive.") + std::string(expression.name)];
        }
    }
    return result;
}
} // namespace wolvrix::lib::ingest::firrtl
