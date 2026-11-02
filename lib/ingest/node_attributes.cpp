#include "ingest/node_attributes.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::ingest::node {
namespace {
void quote(std::string& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += ch;
    }
    out += '"';
}

void array(std::string& out, std::span<const std::string> values) {
    out += '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ',';
        quote(out, values[i]);
    }
    out += ']';
}

class KeyParser {
public:
    explicit KeyParser(std::string_view text) : text(text) {}
    void take(char ch) {
        if (cursor >= text.size() || text[cursor++] != ch) fail();
    }
    std::string string() {
        take('"');
        std::string result;
        while (cursor < text.size()) {
            const unsigned char ch = text[cursor++];
            if (ch == '"') return result;
            if (ch < 32) fail();
            if (ch != '\\') { result += ch; continue; }
            if (cursor >= text.size()) fail();
            switch (text[cursor++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case 'u': {
                if (cursor + 4 > text.size() || text.substr(cursor, 2) != "00") fail();
                cursor += 2;
                const auto digit = [&]() {
                    const char value = text[cursor++];
                    if (value >= '0' && value <= '9') return value - '0';
                    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                    fail();
                };
                const int high = digit(), low = digit();
                result += static_cast<char>(high * 16 + low);
                break;
            }
            default: fail();
            }
        }
        fail();
    }
    std::vector<std::string> strings() {
        take('[');
        std::vector<std::string> result;
        if (cursor < text.size() && text[cursor] == ']') { ++cursor; return result; }
        for (;;) {
            result.push_back(string());
            if (cursor < text.size() && text[cursor] == ']') { ++cursor; return result; }
            take(',');
        }
    }
    bool done() const { return cursor == text.size(); }
private:
    [[noreturn]] static void fail() { throw std::invalid_argument("invalid canonical FIRRTL node key"); }
    std::string_view text;
    std::size_t cursor = 0;
};

// Validate the canonical representation without allocating an Identity and
// serializing it again. Invalid input still uses KeyParser for its diagnostic.
class CanonicalKeyValidator {
public:
    explicit CanonicalKeyValidator(std::string_view text) : text_(text) {}
    bool valid() {
        return take('[') && string() && take(',') && string() && take(',') && string() && take(',') &&
            strings() && take(',') && string() && take(',') && strings() && take(']') && cursor_ == text_.size();
    }
private:
    bool take(char ch) {
        if (cursor_ == text_.size() || text_[cursor_] != ch) return false;
        ++cursor_;
        return true;
    }
    bool string() {
        if (!take('"')) return false;
        bool nonempty = false;
        while (cursor_ < text_.size()) {
            const unsigned char ch = text_[cursor_++];
            if (ch == '"') return nonempty;
            if (ch < 32) return false;
            nonempty = true;
            if (ch != '\\') continue;
            if (cursor_ == text_.size()) return false;
            const char escaped = text_[cursor_++];
            if (escaped == '"' || escaped == '\\') continue;
            if (escaped != 'u' || text_.size() - cursor_ < 4 || text_.substr(cursor_, 2) != "00") return false;
            cursor_ += 2;
            const auto digit = [](char value) {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                return -1;
            };
            const int high = digit(text_[cursor_++]), low = digit(text_[cursor_++]);
            // quote() uses lowercase \\u00hh only for bytes below 32.
            if (high < 0 || low < 0 || high * 16 + low >= 32) return false;
        }
        return false;
    }
    bool strings() {
        if (!take('[')) return false;
        if (take(']')) return true;
        for (;;) {
            if (!string()) return false;
            if (take(']')) return true;
            if (!take(',')) return false;
        }
    }
    std::string_view text_;
    std::size_t cursor_ = 0;
};

void validateKey(std::string_view encoded) {
    if (!CanonicalKeyValidator(encoded).valid()) (void)parseKey(encoded);
}

template <typename T>
T attr(const grh::Graph& graph, grh::OperationId op, std::string_view name) {
    for (const auto& entry : graph.opAttrs(op)) if (entry.key == name) {
        if (const auto* value = std::get_if<T>(&entry.value)) return *value;
        throw std::invalid_argument("wrong node attribute type: " + std::string(name));
    }
    return {};
}

template <typename T> void unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool literal(const grh::Graph& graph, grh::OperationId op) {
    return op && graph.opKind(op) == grh::OperationKind::kConstant;
}

std::span<const std::string> owner(const grh::Graph& graph, grh::OperationId op) {
    if (!op) return {};
    for (const auto& entry : graph.opAttrs(op)) if (entry.key == "grhsim.partition") {
        if (const auto* value = std::get_if<std::vector<std::string>>(&entry.value)) return *value;
        throw std::invalid_argument("wrong grhsim.partition type");
    }
    return {};
}

bool sameOwner(const grh::Graph& graph, grh::OperationId left, grh::OperationId right) {
    const auto a = owner(graph, left), b = owner(graph, right);
    return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

bool finalLeaf(std::span<const std::string> path) {
    return (path.size() == 4 && path[0] == "general" && path[1] == "nonsink") ||
           (path.size() == 3 && path[0] == "general" && (path[1] == "small-sink" || path[1] == "large-sink"));
}

} // namespace

std::string key(const Identity& identity) {
    if (identity.sourceUnit.empty() || identity.circuit.empty() || identity.module.empty() || identity.name.empty())
        throw std::invalid_argument("FIRRTL node identity requires source unit, circuit, module and name");
    for (const auto* path : {&identity.instancePath, &identity.fieldPath})
        if (std::any_of(path->begin(), path->end(), [](const auto& part) { return part.empty(); }))
            throw std::invalid_argument("FIRRTL node identity has an empty path component");
    std::string result = "[";
    quote(result, identity.sourceUnit); result += ',';
    quote(result, identity.circuit); result += ',';
    quote(result, identity.module); result += ',';
    array(result, identity.instancePath); result += ',';
    quote(result, identity.name); result += ',';
    array(result, identity.fieldPath); result += ']';
    return result;
}

Identity parseKey(std::string_view encoded) {
    KeyParser parser(encoded);
    Identity identity;
    parser.take('[');
    identity.sourceUnit = parser.string(); parser.take(',');
    identity.circuit = parser.string(); parser.take(',');
    identity.module = parser.string(); parser.take(',');
    identity.instancePath = parser.strings(); parser.take(',');
    identity.name = parser.string(); parser.take(',');
    identity.fieldPath = parser.strings(); parser.take(']');
    if (!parser.done() || key(identity) != encoded)
        throw std::invalid_argument("noncanonical FIRRTL node key");
    return identity;
}

bool isAttribute(std::string_view name) noexcept { return name.starts_with("firrtl.node."); }

bool annotated(const grh::Graph& graph, grh::OperationId op) {
    return op && std::any_of(graph.opAttrs(op).begin(), graph.opAttrs(op).end(),
                            [](const auto& entry) { return isAttribute(entry.key); });
}

Attributes read(const grh::Graph& graph, grh::OperationId op) {
    Attributes result;
    result.members = attr<std::vector<std::string>>(graph, op, kMembers);
    result.origins = attr<std::vector<std::string>>(graph, op, kOrigins);
    const auto roots = attr<std::vector<std::string>>(graph, op, kRoots);
    const auto indices = attr<std::vector<int64_t>>(graph, op, kRootResults);
    if (roots.size() != indices.size()) throw std::invalid_argument("node roots/rootResults length mismatch");
    for (std::size_t i = 0; i < roots.size(); ++i) result.roots.push_back({roots[i], indices[i]});
    return result;
}

Attributes normalize(Attributes attributes) {
    unique(attributes.members); unique(attributes.origins); unique(attributes.roots);
    for (const auto& member : attributes.members) validateKey(member);
    for (const auto& origin : attributes.origins) validateKey(origin);
    for (const auto& root : attributes.roots) validateKey(root.key);
    return attributes;
}

void write(grh::Graph& graph, grh::OperationId op, Attributes attributes) {
    if (const auto schema = graph.metadata(kSchema);
        schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != kSchemaRevision))
        throw std::invalid_argument("unsupported firrtl.nodeSchema; re-ingest for soft-node-v2");
    attributes = normalize(std::move(attributes));
    if (attributes.members.size() > kMaxAssociations || attributes.roots.size() > kMaxAssociations ||
        attributes.origins.size() > kMaxAssociations) {
        // Dropping a hint is safe; truncating it into an apparently complete
        // expression or refusing the functional rewrite is not.
        attributes = {};
        const auto old = graph.metadata("firrtl.nodeHintDegraded");
        const auto count = old && std::holds_alternative<int64_t>(*old) ? std::get<int64_t>(*old) : 0;
        graph.setMetadata("firrtl.nodeHintDegraded", count + 1);
    }
    std::vector<std::string> roots;
    std::vector<int64_t> indices;
    for (const auto& root : attributes.roots) {
        if (root.result < 0 || static_cast<uint64_t>(root.result) >= graph.opResults(op).size())
            throw std::invalid_argument("node root result index out of range");
        roots.push_back(root.key); indices.push_back(root.result);
    }
    const auto put = [&](std::string_view name, const auto& value) {
        if (value.empty()) graph.eraseAttr(op, name);
        else graph.setAttr(op, name, value);
    };
    put(kMembers, attributes.members); put(kOrigins, attributes.origins);
    put(kRoots, roots); put(kRootResults, indices);
    if (!attributes.members.empty() || !attributes.roots.empty() || !attributes.origins.empty())
        graph.setMetadata(kSchema, kSchemaRevision);
}

namespace {
void writeSingleCanonicalImpl(grh::Graph& graph, grh::OperationId op,
                              std::string_view canonicalKey, bool root,
                              int64_t rootResult) {
    if (canonicalKey.empty()) throw std::invalid_argument("FIRRTL node identity requires a canonical key");

    if (const auto schema = graph.metadata(kSchema);
        schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != kSchemaRevision))
        throw std::invalid_argument("unsupported firrtl.nodeSchema; re-ingest for soft-node-v2");
    if (root && (rootResult < 0 || static_cast<uint64_t>(rootResult) >= graph.opResults(op).size()))
        throw std::invalid_argument("node root result index out of range");
    graph.setCanonicalNodeAttrs(op, canonicalKey, root, rootResult);

    graph.setMetadata(kSchema, kSchemaRevision);
}
} // namespace

void writeSingleCanonical(grh::Graph& graph, grh::OperationId op,
                          std::string_view canonicalKey, bool root,
                          int64_t rootResult) {
    writeSingleCanonicalImpl(graph, op, canonicalKey, root, rootResult);
}

void writeSingleCanonicalReady(grh::Graph& graph, grh::OperationId op,
                               std::string_view canonicalKey, bool root,
                               int64_t rootResult) {
    if (canonicalKey.empty()) throw std::invalid_argument("FIRRTL node identity requires a canonical key");
    if (root && (rootResult < 0 || static_cast<uint64_t>(rootResult) >= graph.opResults(op).size()))
        throw std::invalid_argument("node root result index out of range");
    graph.setCanonicalNodeAttrs(op, canonicalKey, root, rootResult);
}

void copyMembers(grh::Graph& graph, grh::OperationId from, grh::OperationId to) {
    if (from == to || !annotated(graph, from)) return;
    auto attributes = read(graph, from);
    attributes.roots.clear();
    write(graph, to, std::move(attributes));
    if (const auto clone = graph.getOperation(from).attr("grhsim.nodeClone")) graph.setAttr(to, "grhsim.nodeClone", *clone);
}

Attributes remapResults(Attributes attributes, std::span<const int64_t> oldToNew) {
    std::erase_if(attributes.roots, [&](const Root& root) {
        return root.result < 0 || static_cast<uint64_t>(root.result) >= oldToNew.size() || oldToNew[root.result] < 0;
    });
    for (auto& root : attributes.roots) {
        root.result = oldToNew[root.result];
    }
    return normalize(std::move(attributes));
}

QualificationContext::QualificationContext(std::span<const std::string> instancePath)
    : instancePath_(instancePath.begin(), instancePath.end()) {}

void qualify(grh::Graph& graph, grh::OperationId op, std::span<const std::string> instancePath) {
    if (!annotated(graph, op)) return;
    auto attributes = read(graph, op);
    const auto qualifyKey = [&](std::string& encoded) {
        auto identity = parseKey(encoded);
        identity.instancePath.insert(identity.instancePath.begin(), instancePath.begin(), instancePath.end());
        encoded = key(identity);
    };
    for (auto& member : attributes.members) qualifyKey(member);
    for (auto& origin : attributes.origins) qualifyKey(origin);
    for (auto& root : attributes.roots) qualifyKey(root.key);
    write(graph, op, std::move(attributes));
}

void qualify(grh::Graph& graph, grh::OperationId op, QualificationContext& context) {
    if (!annotated(graph, op)) return;
    auto attributes = read(graph, op);
    const auto qualifyKey = [&](std::string& encoded) {
        auto found = context.qualified_.find(encoded);
        if (found == context.qualified_.end()) {
            auto identity = parseKey(encoded);
            identity.instancePath.insert(identity.instancePath.begin(),
                context.instancePath_.begin(), context.instancePath_.end());
            found = context.qualified_.emplace(encoded, key(identity)).first;
        }
        encoded = found->second;
    };
    for (auto& member : attributes.members) qualifyKey(member);
    for (auto& origin : attributes.origins) qualifyKey(origin);
    for (auto& root : attributes.roots) qualifyKey(root.key);
    write(graph, op, std::move(attributes));
}

Attributes unite(Attributes target, const Attributes& source, MergeReason) {
    target.members.insert(target.members.end(), source.members.begin(), source.members.end());
    target.roots.insert(target.roots.end(), source.roots.begin(), source.roots.end());
    target.origins.insert(target.origins.end(), source.origins.begin(), source.origins.end());
    return normalize(std::move(target));
}

void mergeEquivalentOperations(grh::Graph& graph, grh::OperationId target,
                              grh::OperationId source, std::span<const int64_t> resultMap) {
    if (target == source) return;
    const auto mapped = remapResults(read(graph, source), resultMap);
    write(graph, target, unite(read(graph, target), mapped, MergeReason::WholeNode));
    auto remaining = read(graph, source);
    remaining.roots.clear();
    write(graph, source, std::move(remaining));
}

void mergeCseOperations(grh::Graph& graph, grh::OperationId target, grh::OperationId source) {
    if (target == source) return;
    if (!annotated(graph, source) && !annotated(graph, target)) return;
    if (!sameScope(graph, target, source))
        throw std::invalid_argument("primitive CSE would cross an execution owner");
    if (graph.opResults(target).size() != graph.opResults(source).size())
        throw std::invalid_argument("CSE result count mismatch");
    const auto reason = finalLeaf(owner(graph, target)) || (literal(graph, target) && literal(graph, source))
        ? MergeReason::ExecutionLeaf : MergeReason::SameComputation;
    write(graph, target, unite(read(graph, target), read(graph, source), reason));
    auto remaining = read(graph, source);
    remaining.roots.clear();
    write(graph, source, std::move(remaining));
}

std::string cseScope(const grh::Graph&, grh::OperationId) { return {}; }

bool sameScope(const grh::Graph& graph, grh::OperationId left, grh::OperationId right) {
    return left && right && sameOwner(graph, left, right);
}

bool mayInline(const grh::Graph& graph, grh::OperationId consumer, grh::OperationId producer) {
    return producer && (literal(graph, producer) || sameScope(graph, consumer, producer));
}

bool rootCarrier(const grh::Graph&, grh::OperationId) { return false; }

grh::ValueId redirectRoots(grh::Graph& graph, grh::ValueId from, grh::ValueId to) {
    const auto source = graph.valueDef(from);
    if (!source || from == to || !annotated(graph, source)) return to;
    auto original = read(graph, source);
    const auto results = graph.opResults(source);
    const auto matches = [&](const Root& root) {
        return root.result >= 0 && static_cast<uint64_t>(root.result) < results.size() && results[root.result] == from;
    };
    const auto target = graph.valueDef(to);
    if (target == source) {
        const auto outputs = graph.opResults(target);
        const auto index = std::find(outputs.begin(), outputs.end(), to) - outputs.begin();
        for (auto& root : original.roots) if (matches(root)) root.result = index;
    } else {
        if (target && graph.sameValueType(from, to)) {
            auto destination = read(graph, target);
            const auto outputs = graph.opResults(target);
            const auto index = std::find(outputs.begin(), outputs.end(), to) - outputs.begin();
            for (const auto& root : original.roots) if (matches(root)) destination.roots.push_back({root.key, index});
            destination.members.insert(destination.members.end(), original.members.begin(), original.members.end());
            destination.origins.insert(destination.origins.end(), original.origins.begin(), original.origins.end());
            write(graph, target, std::move(destination));
        }
        std::erase_if(original.roots, matches);
    }
    write(graph, source, std::move(original));
    return to;
}

namespace {
struct AttributeView {
    std::span<const std::string> members, origins, roots;
    std::span<const int64_t> indices;
};

template <typename T>
std::span<const T> arrayView(const grh::AttributeValue* attribute, std::string_view name) {
    if (!attribute) return {};
    if (const auto* values = std::get_if<std::vector<T>>(attribute)) return *values;
    throw std::invalid_argument("wrong node attribute type: " + std::string(name));
}

std::optional<AttributeView> attributeView(const grh::Graph& graph, grh::OperationId op, bool schema) {
    const grh::AttributeValue *members = nullptr, *origins = nullptr, *roots = nullptr, *indices = nullptr;
    std::string_view unknown;
    bool annotated = false;
    for (const auto& entry : graph.opAttrs(op)) if (isAttribute(entry.key)) {
        annotated = true;
        if (entry.key == kMembers) members = &entry.value;
        else if (entry.key == kOrigins) origins = &entry.value;
        else if (entry.key == kRoots) roots = &entry.value;
        else if (entry.key == kRootResults) indices = &entry.value;
        else if (unknown.empty()) unknown = entry.key;
    }
    if (!annotated) return std::nullopt;
    if (!schema) throw std::invalid_argument("node attributes require firrtl.nodeSchema=2");
    if (!unknown.empty()) throw std::invalid_argument("unknown node attribute: " + std::string(unknown));
    if (bool(roots) != bool(indices)) throw std::invalid_argument("node roots/rootResults must be paired");
    AttributeView result;
    result.members = arrayView<std::string>(members, kMembers);
    result.origins = arrayView<std::string>(origins, kOrigins);
    result.roots = arrayView<std::string>(roots, kRoots);
    result.indices = arrayView<int64_t>(indices, kRootResults);
    if (result.roots.size() != result.indices.size())
        throw std::invalid_argument("node roots/rootResults length mismatch");
    return result;
}

class AttributeValidation {
public:
    void validate(const AttributeView& attributes) {
        bool normalized = keys(attributes.members);
        normalized = keys(attributes.origins) && normalized;
        const auto root = [&](std::size_t i) { return std::pair(std::string_view(attributes.roots[i]), attributes.indices[i]); };
        bool rootsNormalized = true;
        for (std::size_t i = 1; i < attributes.roots.size(); ++i)
            rootsNormalized &= root(i - 1) < root(i);
        if (rootsNormalized) {
            for (const auto& encoded : attributes.roots) canonical(encoded);
        } else {
            std::vector<std::pair<std::string_view, int64_t>> roots;
            for (std::size_t i = 0; i < attributes.roots.size(); ++i) roots.push_back(root(i));
            unique(roots);
            for (const auto& [encoded, index] : roots) canonical(encoded);
        }
        if (!normalized || !rootsNormalized) throw std::invalid_argument("node attributes are not sorted/unique");
    }
private:
    void canonical(std::string_view encoded) {
        if (canonical_.contains(encoded)) return;
        validateKey(encoded);
        canonical_.insert(encoded);
    }
    bool keys(std::span<const std::string> keys) {
        const bool normalized = std::adjacent_find(keys.begin(), keys.end(),
            [](const auto& left, const auto& right) { return left >= right; }) == keys.end();
        if (normalized) {
            for (const auto& encoded : keys) canonical(encoded);
        } else {
            std::vector<std::string_view> sorted(keys.begin(), keys.end());
            unique(sorted);
            for (const auto& encoded : sorted) canonical(encoded);
        }
        return normalized;
    }
    std::unordered_set<std::string_view> canonical_;
};

template <typename Attributes, typename Root>
std::vector<std::string> scanAttributes(const grh::Graph& graph, Attributes attributes, Root root) {
    std::vector<std::string> errors;
    const auto schema = graph.metadata(kSchema);
    if (schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != kSchemaRevision))
        errors.push_back("unsupported firrtl.nodeSchema; re-ingest for soft-node-v2");
    AttributeValidation validation;
    for (const auto op : graph.operations()) {
        try {
            const auto view = attributeView(graph, op, schema.has_value());
            if (!view) continue;
            validation.validate(*view);
            attributes(op, *view);
            const auto outputs = graph.opResults(op);
            for (std::size_t i = 0; i < view->roots.size(); ++i) {
                const auto index = view->indices[i];
                if (index < 0 || static_cast<uint64_t>(index) >= outputs.size())
                    throw std::invalid_argument("node root result index out of range");
                root(op, view->roots[i], index, outputs[index]);
            }
        } catch (const std::exception& error) {
            errors.push_back(std::string(graph.getOperation(op).symbolText()) + ": " + error.what());
        }
    }
    return errors;
}
}

Inspection inspect(const grh::Graph& graph) {
    Inspection result;
    std::map<std::string, NodeRecord> records;
    result.errors = scanAttributes(graph, [&](grh::OperationId op, const AttributeView& attributes) {
        if (attributes.members.empty() && !literal(graph, op)) result.isolatedFragments.push_back(op);
        for (const auto& member : attributes.members) {
            auto& record = records[member]; record.key = member; record.members.push_back(op);
        }
    }, [&](grh::OperationId op, const std::string& key, int64_t index, grh::ValueId value) {
        auto& record = records[key]; record.key = key;
        record.roots.push_back({op, index, value});
    });
    for (auto& [identity, record] : records) {
        // Optional associations may overlap, lose roots, or have cloned roots.
        // Only the representation above is a validity constraint.
        result.nodes.push_back(std::move(record));
    }
    return result;
}

void requireValid(const grh::Graph& graph) {
    const auto errors = scanAttributes(graph, [](grh::OperationId, const AttributeView&) {},
        [](grh::OperationId, const std::string&, int64_t, grh::ValueId) {});
    if (!errors.empty()) throw std::invalid_argument("FIRRTL node attributes: " + errors.front());
}

namespace {
const grh::AttributeValue* metadataValue(const grh::Graph& graph, std::string_view name) {
    for (const auto& entry : graph.metadata()) if (entry.key == name) return &entry.value;
    return nullptr;
}
std::span<const std::string> metadataKeyView(const grh::Graph& graph, std::string_view name) {
    const auto* value = metadataValue(graph, name);
    if (!value) return {};
    const auto* keys = std::get_if<std::vector<std::string>>(value);
    if (!keys) throw std::invalid_argument("wrong FIRRTL inventory metadata type");
    return *keys;
}
std::vector<std::string> metadataKeys(const grh::Graph& graph, std::string_view name) {
    const auto keys = metadataKeyView(graph, name);
    return {keys.begin(), keys.end()};
}
}

void establishPolicy(grh::Graph& graph) {
    std::vector<std::string> keys;
    for (const auto& record : inspect(graph).nodes) keys.push_back(record.key);
    graph.setMetadata(kScopePolicy, std::string(kScopeRevision));
    graph.setMetadata(kInventory, std::move(keys));
}

void requirePolicy(const grh::Graph& graph) {
    const auto* policy = metadataValue(graph, kScopePolicy);
    if (!metadataValue(graph, kSchema) && !policy) return;
    const auto* revision = policy ? std::get_if<std::string>(policy) : nullptr;
    if (!revision || *revision != kScopeRevision || !metadataValue(graph, kInventory))
        throw std::invalid_argument("FIRRTL soft-node-v2 policy is missing/incompatible; re-ingest the source");
}

DeadRootBatch::DeadRootBatch(grh::Graph& graph)
    : graph_(graph), enabled_(graph.metadata(kScopePolicy).has_value()) {}

void DeadRootBatch::record(grh::OperationId op) {
    if (!enabled_) return;
    // Dead-root recording is a hot path during broad cone sweeps. Borrow the
    // roots attribute directly instead of materializing members/origins and
    // parsing every root into NodeRecord objects. Keep attr<T>'s first-match
    // and wrong-type behavior: only the first roots attribute is observed.
    for (const auto& entry : graph_.opAttrs(op)) {
        if (!isAttribute(entry.key)) continue;
        if (entry.key != kRoots) continue;
        const auto* roots = std::get_if<std::vector<std::string>>(&entry.value);
        if (!roots) throw std::invalid_argument("wrong node attribute type: firrtl.node.roots");
        for (const auto& root : *roots) pending_.push_back(root);
        return;
    }
}

void DeadRootBatch::flush() {
    if (pending_.empty()) return;
    auto dead = metadataKeys(graph_, kEliminated);
    for (auto& key : pending_) dead.push_back(std::move(key));
    unique(dead); graph_.setMetadata(kEliminated, std::move(dead));
    pending_.clear();
}

void recordDead(grh::Graph& graph, grh::OperationId op) {
    DeadRootBatch batch(graph); batch.record(op); batch.flush();
}

InventoryImportBatch::InventoryImportBatch(grh::Graph& target) : target_(target) {}

void InventoryImportBatch::record(const grh::Graph& source,
                                  std::span<const std::string> instancePath) {
    if (!metadataValue(source, kScopePolicy)) return;
    requirePolicy(source);
    if (metadataValue(target_, kScopePolicy)) requirePolicy(target_);
    const auto append = [&](std::string_view name, std::vector<std::string>& pending) {
        (void)metadataKeyView(target_, name);
        for (const auto& encoded : metadataKeyView(source, name)) {
            auto identity = parseKey(encoded);
            identity.instancePath.insert(identity.instancePath.begin(), instancePath.begin(), instancePath.end());
            pending.push_back(key(identity));
        }
    };
    append(kInventory, inventory_); append(kEliminated, eliminated_);
    // Keep the policy structurally valid while imports remain private. No
    // metadata is added when every source lacks the FIRRTL policy.
    if (!metadataValue(target_, kInventory)) target_.setMetadata(kInventory, std::vector<std::string>{});
    if (!eliminated_.empty() && !metadataValue(target_, kEliminated))
        target_.setMetadata(kEliminated, std::vector<std::string>{});
    if (!metadataValue(target_, kScopePolicy)) target_.setMetadata(kScopePolicy, std::string(kScopeRevision));
    pendingImport_ = true;
}

void InventoryImportBatch::flush() {
    if (!pendingImport_) return;
    const auto merge = [&](std::string_view name, std::vector<std::string>& pending) {
        const auto existing = metadataKeyView(target_, name);
        pending.reserve(pending.size() + existing.size());
        pending.insert(pending.end(), existing.begin(), existing.end());
        unique(pending);
        if (!pending.empty() || name == kInventory) target_.setMetadata(name, std::move(pending));
        pending.clear();
    };
    merge(kInventory, inventory_); merge(kEliminated, eliminated_);
    pendingImport_ = false;
}

void importInventory(const grh::Graph& source, grh::Graph& target,
                     std::span<const std::string> instancePath) {
    InventoryImportBatch batch(target); batch.record(source, instancePath); batch.flush();
}

void recordRetention(grh::Graph& graph) {
    if (!graph.metadata(kScopePolicy)) return;
    requirePolicy(graph);
    enum Flags : unsigned { Member = 1, Root = 2, Constant = 4, Alias = 8, Shared = 16 };
    std::unordered_map<std::string_view, unsigned> keys;
    std::size_t references = 0, bytes = 0, roots = 0, isolated = 0;
    const auto errors = scanAttributes(graph, [&](grh::OperationId op, const AttributeView& attributes) {
        if (attributes.members.empty() && !literal(graph, op)) ++isolated;
        const auto add = [&](const std::string& key) -> unsigned& {
            ++references; bytes += key.size(); return keys[key];
        };
        for (const auto& key : attributes.members) add(key) |= Member;
        for (const auto& key : attributes.origins) add(key);
        const auto args = graph.opOperands(op);
        const bool alias = graph.opKind(op) == grh::OperationKind::kAssign && args.size() == 1;
        const bool constant = literal(graph, alias ? graph.valueDef(args[0]) : op);
        for (const auto& key : attributes.roots)
            add(key) |= Root | (alias ? Alias : 0) | (constant ? Constant : 0) |
                (attributes.roots.size() > 1 ? Shared : 0);
        roots += attributes.roots.size();
    }, [](grh::OperationId, const std::string&, int64_t, grh::ValueId) {});
    if (!errors.empty()) throw std::invalid_argument(errors.front());
    const auto eliminated = metadataKeyView(graph, kEliminated);
    const std::unordered_set<std::string_view> dead(eliminated.begin(), eliminated.end());
    const auto inventory = metadataKeyView(graph, kInventory);
    std::size_t missing = 0, removed = 0, uniqueBytes = 0, available = 0, constants = 0, aliases = 0, shared = 0, fragments = 0;
    for (const auto& [key, flags] : keys) {
        uniqueBytes += key.size();
        available += bool(flags & Root); constants += bool(flags & Constant);
        aliases += bool(flags & Alias); shared += bool(flags & Shared);
        fragments += bool(flags & Member) && !(flags & Root);
    }
    for (const auto& key : inventory) if (const auto found = keys.find(key); found == keys.end() || !(found->second & Root)) {
        if (dead.contains(key)) ++removed;
        else ++missing;
    }
    grh::AttributeValue census = std::vector<std::string>{"v2",
        "source-nodes=" + std::to_string(inventory.size()), "available=" + std::to_string(available),
        "roots=" + std::to_string(roots), "dead=" + std::to_string(removed),
        "constant=" + std::to_string(constants), "alias=" + std::to_string(aliases),
        "shared=" + std::to_string(shared), "fragments=" + std::to_string(fragments),
        "isolated=" + std::to_string(isolated), "missing=" + std::to_string(missing),
        "key-references=" + std::to_string(references), "key-bytes=" + std::to_string(bytes),
        "unique-key-bytes=" + std::to_string(uniqueBytes)};
    const auto* previous = metadataValue(graph, "firrtl.nodeRetention");
    if (!previous || *previous != census)
        graph.setMetadata("firrtl.nodeRetention", std::move(census));
}

std::string Inspection::report(const grh::Graph& graph) const {
    std::ostringstream out;
    out << "nodes=" << nodes.size() << " isolated_fragments=" << isolatedFragments.size()
        << " errors=" << errors.size() << '\n';
    for (const auto& record : nodes) {
        out << record.key << " members=" << record.members.size();
        for (const auto& root : record.roots)
            out << " root=" << graph.getOperation(root.operation).symbolText() << ':' << root.result
                << " value=" << graph.symbolText(graph.valueSymbol(root.value));
        out << '\n';
    }
    for (const auto op : isolatedFragments) out << "isolated=" << graph.getOperation(op).symbolText() << '\n';
    for (const auto& error : errors) out << "error: " << error << '\n';
    return out.str();
}

} // namespace wolvrix::lib::ingest::node
