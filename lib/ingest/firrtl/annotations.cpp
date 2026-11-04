#include "annotations.hpp"

#include <algorithm>
#include <cctype>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
// The lexer validates JSON. Interpret one annotation at a time without a
// generic tree for the large circuit-level payload.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}
    explicit Reader(const Source& source) : text_(source.annotations()) {
        base_ = static_cast<uint64_t>(text_.data() - source.text().data());
        for (uint64_t i = 0; i < base_; ++i) {
            if (source.text()[i] == '\n') { ++line_; column_ = 1; } else ++column_;
        }
    }
    AnnotationIndex parse() {
        AnnotationIndex result; expect('[');
        if (accept(']')) return result;
        do {
            Annotation record; space(); const auto begin = cursor_; record.range = range(); expect('{');
            if (!accept('}')) {
                do {
                    auto key = string(); expect(':');
                    if (!record.fields.emplace(std::move(key), string()).second) fail("duplicate annotation field");
                } while (accept(','));
                expect('}');
            }
            record.raw = std::string(text_.substr(begin, cursor_ - begin));
            record.range.end = base_ + cursor_;
            if (!record.fields.contains("class") || !record.fields.contains("target")) fail("annotation requires class and target");
            record.kind = record.fields.at("class"); record.target = record.fields.at("target");
            const auto separator = record.target.find('|');
            if (record.target.empty() || record.target.front() != '~' || separator == std::string::npos ||
                record.target.find('|', separator + 1) != std::string::npos) fail("unsupported annotation target");
            const auto end = record.target.find_first_of("/>", separator + 1);
            record.module = record.target.substr(separator + 1, end - separator - 1);
            if (record.module.empty()) fail("annotation target requires a module");
            ++result.counts[record.kind];
            result.byModule[record.module].push_back(result.records.size());
            result.records.push_back(std::move(record));
        } while (accept(','));
        expect(']'); space(); if (cursor_ != text_.size()) fail("trailing annotation content");
        return result;
    }
private:
    std::string_view text_;
    std::size_t cursor_ = 0;
    std::size_t located_ = 0;
    uint64_t base_ = 0, line_ = 1, column_ = 1;
    Range range() {
        while (located_ < cursor_) {
            if (text_[located_++] == '\n') { ++line_; column_ = 1; } else ++column_;
        }
        return {base_ + cursor_, base_ + cursor_, line_, column_, {}};
    }
    [[noreturn]] void fail(const char* message) { throw Error(range(), message); }
    void space() { while (cursor_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[cursor_]))) ++cursor_; }
    bool accept(char c) { space(); if (cursor_ == text_.size() || text_[cursor_] != c) return false; ++cursor_; return true; }
    void expect(char c) { if (!accept(c)) fail("unsupported annotation JSON schema"); }
    uint32_t hex4() {
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            if (cursor_ == text_.size()) fail("truncated annotation Unicode escape");
            const auto c = text_[cursor_++]; unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else fail("invalid annotation Unicode escape");
            result = result * 16 + digit;
        }
        return result;
    }
    std::string string() {
        expect('"'); std::string result;
        while (cursor_ < text_.size()) {
            auto c = text_[cursor_++];
            if (c == '"') return result;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in annotation string");
            if (c != '\\') { result += c; continue; }
            if (cursor_ == text_.size()) fail("truncated annotation string");
            c = text_[cursor_++];
            switch (c) {
            case '"': case '\\': case '/': result += c; break;
            case 'b': result += '\b'; break; case 'f': result += '\f'; break;
            case 'n': result += '\n'; break; case 'r': result += '\r'; break; case 't': result += '\t'; break;
            case 'u': {
                auto point = hex4();
                if (point >= 0xd800 && point <= 0xdbff) {
                    if (cursor_ + 2 > text_.size() || text_[cursor_++] != '\\' || text_[cursor_++] != 'u') fail("missing annotation low surrogate");
                    const auto low = hex4(); if (low < 0xdc00 || low > 0xdfff) fail("invalid annotation low surrogate");
                    point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
                } else if (point >= 0xdc00 && point <= 0xdfff) fail("unpaired annotation low surrogate");
                if (point < 0x80) result += static_cast<char>(point);
                else {
                    if (point < 0x800) result += static_cast<char>(0xc0 | (point >> 6));
                    else {
                        if (point < 0x10000) result += static_cast<char>(0xe0 | (point >> 12));
                        else { result += static_cast<char>(0xf0 | (point >> 18)); result += static_cast<char>(0x80 | ((point >> 12) & 63)); }
                        result += static_cast<char>(0x80 | ((point >> 6) & 63));
                    }
                    result += static_cast<char>(0x80 | (point & 63));
                }
                break;
            }
            default: fail("invalid annotation escape");
            }
        }
        fail("unterminated annotation string");
    }
};
}

AnnotationIndex readAnnotations(const Source& source) {
    if (source.annotations().empty()) return {};
    auto result = Reader(source).parse();
    validateAnnotations(source, result);
    return result;
}
AnnotationIndex parseAnnotations(std::string_view text) { return Reader(text).parse(); }
void validateAnnotations(const Source& source, const AnnotationIndex& result) {
    const std::map<std::string, std::vector<std::string>> schemas{
        {"firrtl.transforms.DedupGroupAnnotation", {"class", "target", "group"}},
        {"firrtl.transforms.DontTouchAnnotation", {"class", "target"}},
        {"firrtl.passes.InlineAnnotation", {"class", "target"}},
        {"chisel3.util.experimental.decode.DecodeTableAnnotation", {"class", "target", "truthTable", "minimizedTable"}},
        {"firrtl.transforms.BlackBoxInlineAnno", {"class", "target", "name", "text"}},
        {"firrtl.transforms.BlackBoxPathAnno", {"class", "target", "path"}},
        {"firrtl.transforms.BlackBoxResourceAnno", {"class", "target", "resourceId"}}
    };
    for (const auto& record : result.records) {
        const auto schema = schemas.find(record.kind);
        if (schema == schemas.end()) throw Error(record.range, "unsupported FIRRTL annotation class '" + record.kind + "'");
        if (record.fields.size() != schema->second.size() || std::any_of(schema->second.begin(), schema->second.end(), [&](const auto& key) { return !record.fields.contains(key); }))
            throw Error(record.range, "unsupported annotation fields for '" + record.kind + "'");
        const auto module = source.findModule(record.module);
        if (!module) throw Error(record.range, "annotation targets an unknown module '" + record.module + "'");
        const auto circuit = record.target.substr(1, record.target.find('|') - 1);
        if (!circuit.empty() && circuit != source.circuit()) throw Error(record.range, "annotation targets another circuit");
        if (record.kind.starts_with("firrtl.transforms.BlackBox") && source.modules()[*module].kind != ModuleKind::External)
            throw Error(record.range, "blackbox source annotation requires an external module");
        const auto component = record.target.find('>');
        if (component != std::string::npos && (component + 1 == record.target.size() || record.target.find('>', component + 1) != std::string::npos))
            throw Error(record.range, "malformed annotation component target");
    }
}
void applyAnnotations(const Source&, const Module& module, const AnnotationIndex& annotations, grh::Graph& graph) {
    std::vector<std::string> counts, records;
    for (const auto& [kind, count] : annotations.counts) counts.push_back(kind + "=" + std::to_string(count));
    std::vector<const Annotation*> targets;
    const auto grouped = annotations.byModule.find(std::string(module.name));
    if (grouped != annotations.byModule.end()) for (const auto index : grouped->second) {
        const auto& record = annotations.records[index];
        records.emplace_back(record.raw);
        if (record.kind == "firrtl.transforms.DontTouchAnnotation" && record.target.find('>') != std::string::npos)
            targets.push_back(&record);
    }
    std::map<std::string_view, std::vector<grh::SymbolId>, std::less<>> anchors;
    if (!targets.empty()) {
        for (const auto value : graph.values()) {
            const auto symbol = graph.valueSymbol(value);
            anchors[graph.symbolText(symbol)].push_back(symbol);
        }
        for (const auto op : graph.operations()) {
            if (graph.opKind(op) != grh::OperationKind::kRegister) continue;
            const auto symbol = graph.operationSymbol(op);
            anchors[graph.symbolText(symbol)].push_back(symbol);
            for (const auto& attribute : graph.opAttrRange(op)) {
                if (attribute.key() != "firrtl.memory") continue;
                if (const auto name = attribute.value().get<std::string>()) anchors[*name].push_back(symbol);
            }
        }
    }
    // Empty anchor entries resolve erased zero-width leaves without inventing
    // executable carriers. Keep their owning metadata copy alive with the index.
    const auto elided = targets.empty() ? std::nullopt : graph.metadata("firrtl.elidedLeaves");
    if (elided)
        for (const auto& name : std::get<std::vector<std::string>>(*elided)) anchors.try_emplace(name);
    for (const auto* target : targets) {
        const auto& record = *target;
        const auto at = record.target.find('>');
        if (record.target.find('/') < at) throw Error(module.range, "hierarchical DontTouch target requires F07");
        std::string symbol = record.target.substr(at + 1);
        const auto index = symbol.find('['); if (index != std::string::npos) symbol.resize(index);
        for (auto dot = symbol.find('.'); dot != std::string::npos; dot = symbol.find('.', dot + 7)) symbol.replace(dot, 1, "__DOT__");
        bool found = false;
        const auto anchor = [&](const auto& entry) {
            for (const auto symbol : entry.second) graph.addDeclaredSymbol(symbol);
            found = true;
        };
        if (const auto exact = anchors.find(symbol); exact != anchors.end()) anchor(*exact);
        const auto prefix = symbol + "__DOT__";
        for (auto field = anchors.lower_bound(prefix); field != anchors.end() && field->first.starts_with(prefix); ++field) {
            anchor(*field);
        }
        if (!found) throw Error(module.range, "DontTouch target cannot be resolved: " + record.target);
    }
    graph.setMetadata("firrtl.annotationCounts", counts);
    graph.setMetadata("firrtl.annotations", records);
}
} // namespace wolvrix::lib::ingest::firrtl::detail
