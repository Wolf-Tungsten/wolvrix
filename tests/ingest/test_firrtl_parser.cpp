#include "ingest/firrtl.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace fi = wolvrix::lib::ingest::firrtl;
namespace in = wolvrix::lib::ingest;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void print(const std::vector<in::IngestDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.location) std::cerr << diagnostic.location->line << ':' << diagnostic.location->column << ' ';
        std::cerr << diagnostic.context << ' ' << diagnostic.message << '\n';
    }
}
std::string design(std::string_view body) {
    return "FIRRTL version 6.0.0\ncircuit T :\n  module T :\n    input a : UInt<8>\n    output y : UInt<8>\n" + std::string(body);
}
void sourceInfoScanning() {
    for (const auto& [line, expected] : std::vector<std::pair<std::string, std::string>>{
             {"    skip ; @[comment.scala 1:1]", ""},
             {"    printf(clk, en, \"@[quoted.scala 1:1]\") @[real.scala 2:3]", "@[real.scala 2:3]"},
             {"    printf(clk, en, \"escaped \\\" @[quoted]\") ; @[comment]", ""},
             {"    skip @[long/path/with/escaped\\]bracket.scala 4:5]", "@[long/path/with/escaped\\]bracket.scala 4:5]"},
             {"    skip", ""}}) {
        const auto text = design(line); // Also exercise the final line without a newline.
        auto indexed = fi::indexText(text, "source-info.fir", "source-info");
        require(indexed.success(), "source-info fixture indexing failed");
        const auto begin = text.size() - line.size();
        auto position = indexed.source->position({begin, text.size(), 6, 1, {}});
        indexed = {};
        require(position.sourceLine == line && position.sourceInfo == expected,
                "source info scanner changed quoting, comments, escapes or ownership");
    }
}
void validFixture() {
    auto index = fi::indexFile(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "parser.fir", "fixture");
    if (!index.success()) print(index.diagnostics);
    require(index.success(), "fixture indexing failed");
    require(index.source->version() == "6.0.0" && index.source->circuit() == "Top", "preamble identity lost");
    require(index.source->modules().size() == 2 && index.source->layers().size() == 1, "top-level index lost declarations");
    require(index.source->annotations().find("a ]] ; b") != std::string_view::npos, "annotation/string delimiter damaged");
    auto module = fi::parseModule(index.source, 0);
    if (!module.success()) print(module.diagnostics);
    require(module.success(), "module parser rejected supported fixture");
    const auto& arena = module.body->arena;
    require(std::count_if(arena.statements.begin(), arena.statements.end(), [](const auto& statement) {
        return statement.kind == fi::StmtKind::Node;
    }) == 7, "source node declarations were dropped");
    const auto root = std::find_if(arena.statements.begin(), arena.statements.end(), [](const auto& statement) { return statement.name == "nested"; });
    require(root != arena.statements.end(), "nested node missing");
    const auto& expression = arena.expressions.at(root->operands.at(0));
    require(expression.name == "xor" && expression.operands.size() == 2 &&
            arena.expressions.at(expression.operands[0]).name == "and" &&
            arena.expressions.at(expression.operands[1]).name == "not", "expression structure flattened or changed");
    const auto validated = fi::validate(index.source);
    if (!validated.success()) print(validated.diagnostics);
    require(validated.success() && validated.modules == 2 && validated.constructs.at("node") == 7, "full syntax validation failed");
    require(index.source->arena().expressions.empty() && index.source->arena().statements.empty(),
            "full validation retained module ASTs in the source/interface arena");
    index = {};
    require(module.source->circuit() == "Top" && expression.name == "xor", "module body did not keep source text alive");

    in::IngestRequest request;
    request.inputRoot = WOLVRIX_FIRRTL_TEST_DATA_DIR;
    request.sources.push_back({in::SourceLanguage::Firrtl, "parser.fir", "fixture"});
    const auto scanned = in::scanInterfaces(request);
    if (!scanned.success) print(scanned.diagnostics);
    require(scanned.success && scanned.modules.size() == 2 && scanned.sourceOwners.size() == 1, "public FIRRTL scan failed");
    const auto& ports = scanned.modules[0].ports;
    const auto packet = std::find_if(ports.begin(), ports.end(), [](const auto& leaf) { return leaf.name == "packet__DOT__data"; });
    require(packet != ports.end() && packet->shape == std::vector<int64_t>{8, 4} && packet->elementWidth == 32,
            "Bundle leaf/Vector shape signature is wrong");
    const auto& parameters = scanned.modules[1].parameters;
    require(parameters.size() == 4 && std::get<in::IntegerParameter>(parameters[0].value).width == 129,
            "large typed integer parameter was truncated");
    require(std::get<in::IntegerParameter>(parameters[0].value).bits == "0" + std::string(128, '1') &&
            std::get<in::IntegerParameter>(parameters[1].value).bits == std::string(32, '1') &&
            std::get<std::string>(parameters[2].value) == "hello\"world", "typed parameter value or escaping changed");
    require(std::holds_alternative<in::RawStringParameter>(parameters[3].value), "raw parameter became a quoted string");
    const auto probe = std::find_if(ports.begin(), ports.end(), [](const auto& leaf) { return leaf.name == "xp"; });
    require(probe != ports.end() && probe->requiresTypeResolution && probe->sourceType == "Probe<UInt>",
            "Probe interface was silently treated as ordinary Logic");
    const auto read = in::readFirrtl(request);
    require(!read.success() && !read.design && read.diagnostics.back().message.find("annotation") != std::string::npos,
            "syntax-only annotation fixture became executable without an annotation policy");
}

void bad(std::string text, std::string_view expected) {
    const auto lastLine = 1 + std::count(text.begin(), text.end(), '\n');
    auto indexed = fi::indexText(std::move(text), "bad.fir", "bad");
    auto diagnostics = indexed.diagnostics;
    if (indexed.success()) diagnostics = fi::validate(indexed.source).diagnostics;
    const auto found = std::find_if(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.message.find(expected) != std::string::npos;
    });
    if (found == diagnostics.end()) print(diagnostics);
    require(found != diagnostics.end(), "invalid input did not produce expected diagnostic");
    require(found->location && found->location->file == "bad.fir" && found->location->line > 0 &&
            found->location->column > 0 &&
            (!found->location->sourceLine.empty() || found->location->line == static_cast<uint64_t>(lastLine)),
            "diagnostic source position missing");
}
void errors() {
    bad("FIRRTL version 5.0.0\ncircuit T:\n  module T:\n    skip\n", "unsupported FIRRTL version");
    bad("circuit T:\n  module T:\n    skip\n", "FIRRTL");
    bad(design("\tconnect y, a\n"), "tabs");
    bad(design("    when a:\n      connect y, a\n     skip\n"), "dedent");
    bad(design("    when a:\n"), "indented statement block");
    bad(design("    node n = add(a, UInt<8>(0h1)\n"), "truncated");
    bad(design("    node n = unknown(a)\n"), "unknown or unsupported FIRRTL expression");
    bad(design("    mystery a\n"), "unknown or unsupported FIRRTL statement");
    bad(design("    node n = later\n    node later = a\n"), "undeclared");
    bad(design("    when a:\n      node local = a\n    connect y, local\n"), "out-of-scope");
    bad(design("    when a:\n      wire q : UInt<8>\n    else:\n      wire q : UInt<8>\n"), "duplicate declaration");
    bad(design("    inst missing of Missing\n"), "unknown module");
    bad(design("    printf(a, a, \"unterminated)\n"), "unterminated");
    bad(design("    skip @[source.scala 1:1\n"), "unterminated");
    bad("FIRRTL version 6.0.0\ncircuit T :%[[{\"value\":1,}]]\n  module T:\n    skip\n", "JSON string");
    bad(design("    node n = UInt<8>(0b102)\n"), "integer literal");
    bad(design("    node n = add(a, a, a)\n"), "expected ')'");
    bad(design("    cover(a, a, a, \"message\", a)\n"), "expected ')'");
    bad(design("    printf(a, a, \"text\") : label\n    wire label : UInt<1>\n"), "duplicate declaration");
    bad(design("    printf(a, a, \"text\") : label\n    node n = label\n"), "undeclared");
    auto wrongVersion = fi::indexText(design("    skip\n"), "version.fir", "version", in::FirrtlOptions{"4.0.0"});
    require(!wrongVersion.success(), "expectedVersion mismatch was ignored");
    auto recover = fi::indexText(design("    node x = add(a,) @[original.scala 4:5]\n    mystery y\n    connect y, a\n"), "recover.fir", "recover");
    require(recover.success(), "body syntax was prematurely accepted/rejected during interface-only indexing");
    auto parsed = fi::parseModule(recover.source, 0);
    require(!parsed.success() && parsed.diagnostics.size() >= 2, "parser did not recover across independent invalid statements");
    require(parsed.diagnostics[0].context == "T" && parsed.diagnostics[0].location->sourceInfo == "@[original.scala 4:5]",
            "original source info/current module not included in error");
    require(parsed.diagnostics[0].location->line == 6 && parsed.diagnostics[0].location->column == 20,
            "body diagnostic line/column did not identify the invalid operand");
    auto owned = parsed.diagnostics; parsed = {}; recover = {};
    require(owned[0].location->sourceLine.find("add(a,)") != std::string::npos, "diagnostics did not own source snippets");
}
void propertiesAndInterning() {
    const auto index = fi::indexFile(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "properties.fir", "properties");
    if (!index.success()) print(index.diagnostics);
    require(index.success() && index.source->modules().size() == 4 && index.source->formals().size() == 1,
            "property/class/enum/formal interface indexing failed");
    const auto validation = fi::validate(index.source);
    if (!validation.success()) print(validation.diagnostics);
    require(validation.success() && validation.modules == 4, "typed property/enum/command syntax failed");
    auto body = fi::parseModule(index.source, 0);
    const auto& arena = body.body->arena;
    const auto constructor = std::find_if(arena.expressions.begin(), arena.expressions.end(), [](const auto& expression) {
        return expression.kind == fi::ExprKind::Enum;
    });
    require(constructor != arena.expressions.end() && constructor->literal == "Ready", "enum tag/payload lost");
    require(index.source->modules()[3].knownLayers.size() == 2, "external known layers lost");
    require(index.source->formals()[0].parameters[1].children[1].children.size() == 3, "nested formal values dropped");
    auto interned = fi::indexText(design("    wire b : UInt<8>\n    node n = add(a, a)\n"), "intern.fir", "intern");
    require(interned.success(), "interning fixture indexing failed");
    auto parsed = fi::parseModule(interned.source, 0);
    require(parsed.success(), "interning fixture body parsing failed");
    require(parsed.body->interface.ports[0].type == parsed.body->arena.statements[0].type,
            "identical types were not interned");
    const auto& add = parsed.body->arena.expressions.back();
    const auto left = parsed.body->arena.expressions[add.operands[0]].name;
    const auto right = parsed.body->arena.expressions[add.operands[1]].name;
    require(left.data() == right.data() && left.data() >= parsed.source->text().data() &&
            left.data() < parsed.source->text().data() + parsed.source->text().size(), "names were copied or not interned");
    bad("FIRRTL version 6.0.0\ncircuit T:\n  type A = B\n  type B = A\n  module T:\n    skip\n", "cyclic FIRRTL type alias");
    bad(design("    wire x : MissingType\n"), "unknown FIRRTL type alias");
}
void memoryPortScope() {
    auto indexed = fi::indexText(design(
        "    smem m : UInt<8>[8]\n"
        "    when a :\n"
        "      rdwr mport p = m[1], a\n"
        "      connect p, a\n"
        "    connect y, p\n"), "mport.fir", "mport");
    require(indexed.success(), "CHIRRTL fixture index failed");
    const auto module = fi::parseModule(indexed.source, 0);
    if (!module.success()) print(module.diagnostics);
    require(module.success(), "CHIRRTL mport data did not inherit the memory scope");
    const auto& statements = module.body->arena.statements;
    const auto when = std::find_if(statements.begin(), statements.end(), [](const auto& statement) { return statement.kind == fi::StmtKind::When; });
    require(when != statements.end() && statements[when->body[0]].kind == fi::StmtKind::MPort,
            "CHIRRTL access was hoisted out of its enable condition");
    bad(design("    when a :\n      smem m : UInt<8>[8]\n      read mport p = m[1], a\n    connect y, p\n"), "out-of-scope");
    bad(design("    read mport p = a[1], a\n"), "not a CHIRRTL memory");
}
void signaturesAndRequests() {
    auto index = fi::indexText("FIRRTL version 6.0.0\ncircuit T:\n  module T:\n"
        "    input p : { flip a : UInt<8>[2], b : UInt }[3]\n    input reset : Reset\n"
        "    output z : const UInt<0>[1]\n    skip\n", "signatures.fir", "signature");
    require(index.success(), "signature fixture index failed");
    const auto signature = in::describeFirrtlModule(*index.source, index.source->modules()[0]);
    require(signature.ports.size() == 4 && signature.ports[0].direction == in::LeafDirection::Output &&
            signature.ports[0].shape == std::vector<int64_t>{3, 2}, "nested flow or Vector geometry changed");
    require(signature.ports[1].elementWidth == -1 && signature.ports[1].requiresTypeResolution &&
            signature.ports[2].sourceType == "Reset" && signature.ports[2].requiresTypeResolution &&
            signature.ports[3].isConst && signature.ports[3].elementWidth == 0,
            "inferred/reset/const/zero-width source distinctions were lost");
    auto collision = fi::indexText(design("    output p : { a : UInt<8> }\n    output p__DOT__a : UInt<8>\n    skip\n"),
                                    "collision.fir", "collision");
    require(collision.success(), "valid source names rejected before flattening");
    bool rejected = false;
    try { in::describeFirrtlModule(*collision.source, collision.source->modules()[0]); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "flattened name collision was accepted");
    in::IngestRequest request; request.inputRoot = WOLVRIX_FIRRTL_TEST_DATA_DIR;
    request.sources.push_back({in::SourceLanguage::Firrtl, "parser.fir", "duplicate"});
    request.sources.push_back(request.sources[0]);
    require(!in::scanInterfaces(request).success, "duplicate logical source units were accepted");
    request.sources.resize(1); request.sources[0].path = "missing.fir";
    const auto missing = in::readFirrtl(request);
    require(!missing.success() && !missing.design && missing.diagnostics[0].location &&
            missing.diagnostics[0].location->file.filename() == "missing.fir", "missing source diagnostics lost filename");
    require(fi::decodeString("\"x\\\";]\\t\\n\\\\\"") == "x\";]\t\n\\", "string decoding changed escaped content");
    require(fi::decodeString("\"\\{{SimulationTime}}\"") == "{{SimulationTime}}", "escaped format substitution rejected");
}

void resourceAndLineBoundaries() {
    const std::string literal = "0h" + std::string(256, 'f');
    auto text = design("    node wide = UInt<1024>(" + literal + ") @[wide.scala 7:9]\n");
    text.pop_back(); // EOF may terminate the final statement without a physical newline.
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    auto indexed = fi::indexText(std::move(crlf), "wide.fir", "wide");
    require(indexed.success(), "CRLF input without a final newline failed indexing");
    auto parsed = fi::parseModule(indexed.source, 0);
    require(parsed.success(), "CRLF input without a final newline failed body validation");
    const auto& statement = parsed.body->arena.statements.at(parsed.body->statements.at(0));
    const auto& expression = parsed.body->arena.expressions.at(statement.operands.at(0));
    require(expression.literal == literal && parsed.body->arena.types.at(expression.type).width == 1024,
            "wide literal spelling or explicit type width was truncated");
    require(statement.range.info == "@[wide.scala 7:9]", "EOF statement lost its original source info");

    std::string invalid;
    for (int i = 0; i < 100; ++i) invalid += "    mystery bad\n";
    indexed = fi::indexText(design(invalid), "many-errors.fir", "many-errors");
    require(indexed.success(), "interface index unexpectedly retained body errors");
    parsed = fi::parseModule(indexed.source, 0);
    require(!parsed.success() && parsed.diagnostics.size() == 32,
            "independent body errors must recover and stop at the documented diagnostic limit");

    std::string nested;
    for (int i = 0; i < 300; ++i) nested += "not(";
    nested += "a";
    nested.append(300, ')');
    bad(design("    node n = " + nested + "\n"), "nesting limit exceeded");
}

void byteIndexedModuleBoundaries() {
    const std::string text =
        "FIRRTL version 6.0.0\ncircuit T :\n  module T :\n"
        "    input a : UInt<8>\n    output y : UInt<9>\n"
        "    node n = add(\nUInt<8>(1),\nUInt<8>(2))\n"
        "    connect y, n\n"
        "    printf(a, a, \"brackets () <> [] ; module Phantom :\") @[escaped\\]source 1:2]\n"
        "  ; blank and comment lines do not terminate the preceding module\n"
        "  module Spare :\n    skip";
    std::string crlf;
    for (const auto c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
    const auto indexed = fi::indexText(std::move(crlf), "byte-boundaries.fir", "bytes");
    if (!indexed.success()) print(indexed.diagnostics);
    require(indexed.success() && indexed.source->modules().size() == 2,
            "byte index mistook grouped continuation, string, or comment for a module boundary");
    require(indexed.source->modules()[1].range.line == 12,
            "byte index lost physical CRLF line numbers across a module boundary");
    const auto validated = fi::validate(indexed.source);
    if (!validated.success()) print(validated.diagnostics);
    require(validated.success() && validated.modules == 2,
            "byte-indexed modules did not preserve grouped body parsing or the final line");

    const auto invalid = fi::indexText(design("    @\n"), "body-token.fir", "body-token");
    require(invalid.success(), "interface index lexed an invalid body token");
    const auto parsed = fi::parseModule(invalid.source, 0);
    require(!parsed.success() && !parsed.diagnostics.empty() &&
            parsed.diagnostics.front().message.find("unexpected character") != std::string::npos &&
            parsed.diagnostics.front().location->line == 6,
            "the one body parse lost its lexical error or source line");

    const auto doubleError = fi::indexText(
        "FIRRTL version 6.0.0\ncircuit T :\n  module T :\n    output y : UInt<1>\n"
        "    @\n  module Later\n    skip\n", "ordered-errors.fir", "ordered-errors");
    require(!doubleError.success() && !doubleError.diagnostics.empty() &&
            doubleError.diagnostics.front().message.find("unexpected character") != std::string::npos &&
            doubleError.diagnostics.front().location->line == 5,
            "header-error fallback lost the earlier legacy body lexical diagnostic");
}
}
int main() {
    try { sourceInfoScanning(); validFixture(); errors(); propertiesAndInterning(); memoryPortScope(); signaturesAndRequests(); resourceAndLineBoundaries(); byteIndexedModuleBoundaries(); }
    catch (const std::exception& error) { std::cerr << "[firrtl-parser] " << error.what() << '\n'; return 1; }
    return 0;
}
