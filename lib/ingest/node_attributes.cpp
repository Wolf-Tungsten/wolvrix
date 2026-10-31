#include "ingest/node_attributes.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
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
    for (const auto& member : attributes.members) (void)parseKey(member);
    for (const auto& origin : attributes.origins) (void)parseKey(origin);
    for (const auto& root : attributes.roots) (void)parseKey(root.key);
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

Inspection inspect(const grh::Graph& graph) {
    Inspection result;
    std::map<std::string, NodeRecord> records;
    const auto schema = graph.metadata(kSchema);
    if (schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != kSchemaRevision))
        result.errors.push_back("unsupported firrtl.nodeSchema; re-ingest for soft-node-v2");
    for (const auto op : graph.operations()) {
        if (!annotated(graph, op)) continue;
        try {
            if (!schema) throw std::invalid_argument("node attributes require firrtl.nodeSchema=2");
            bool hasRoots = false, hasIndices = false;
            for (const auto& entry : graph.opAttrs(op)) if (isAttribute(entry.key)) {
                if (entry.key != kMembers && entry.key != kRoots && entry.key != kRootResults && entry.key != kOrigins)
                    throw std::invalid_argument("unknown node attribute: " + entry.key);
                hasRoots |= entry.key == kRoots; hasIndices |= entry.key == kRootResults;
            }
            if (hasRoots != hasIndices) throw std::invalid_argument("node roots/rootResults must be paired");
            const auto attributes = read(graph, op);
            if (normalize(attributes) != attributes) throw std::invalid_argument("node attributes are not sorted/unique");
            if (attributes.members.empty() && !literal(graph, op)) result.isolatedFragments.push_back(op);
            for (const auto& member : attributes.members) {
                auto& record = records[member]; record.key = member; record.members.push_back(op);
            }
            for (const auto& root : attributes.roots) {
                const auto outputs = graph.opResults(op);
                if (root.result < 0 || static_cast<uint64_t>(root.result) >= outputs.size())
                    throw std::invalid_argument("node root result index out of range");
                auto& record = records[root.key]; record.key = root.key;
                record.roots.push_back({op, root.result, outputs[root.result]});
            }
        } catch (const std::exception& error) {
            result.errors.push_back(std::string(graph.getOperation(op).symbolText()) + ": " + error.what());
        }
    }
    for (auto& [identity, record] : records) {
        // Optional associations may overlap, lose roots, or have cloned roots.
        // Only the representation above is a validity constraint.
        result.nodes.push_back(std::move(record));
    }
    return result;
}

void requireValid(const grh::Graph& graph) {
    const auto result = inspect(graph);
    if (!result.errors.empty()) throw std::invalid_argument("FIRRTL node attributes: " + result.errors.front());
}

namespace {
std::vector<std::string> metadataKeys(const grh::Graph& graph, std::string_view name) {
    const auto value = graph.metadata(name);
    if (!value) return {};
    const auto* keys = std::get_if<std::vector<std::string>>(&*value);
    if (!keys) throw std::invalid_argument("wrong FIRRTL inventory metadata type");
    return *keys;
}
}

void establishPolicy(grh::Graph& graph) {
    std::vector<std::string> keys;
    for (const auto& record : inspect(graph).nodes) keys.push_back(record.key);
    graph.setMetadata(kScopePolicy, std::string(kScopeRevision));
    graph.setMetadata(kInventory, std::move(keys));
}

void requirePolicy(const grh::Graph& graph) {
    if (!graph.metadata(kSchema) && !graph.metadata(kScopePolicy)) return;
    if (graph.metadata(kScopePolicy) != std::optional<grh::AttributeValue>(std::string(kScopeRevision)) ||
        !graph.metadata(kInventory))
        throw std::invalid_argument("FIRRTL soft-node-v2 policy is missing/incompatible; re-ingest the source");
}

void recordDead(grh::Graph& graph, grh::OperationId op) {
    if (!graph.metadata(kScopePolicy) || !annotated(graph, op)) return;
    const auto attributes = read(graph, op);
    if (attributes.roots.empty()) return;
    auto dead = metadataKeys(graph, kEliminated);
    for (const auto& root : attributes.roots) dead.push_back(root.key);
    unique(dead); graph.setMetadata(kEliminated, std::move(dead));
}

void importInventory(const grh::Graph& source, grh::Graph& target,
                     std::span<const std::string> instancePath) {
    if (!source.metadata(kScopePolicy)) return;
    requirePolicy(source);
    if (target.metadata(kScopePolicy)) requirePolicy(target);
    for (const auto name : {kInventory, kEliminated}) {
        auto keys = metadataKeys(target, name);
        for (const auto& encoded : metadataKeys(source, name)) {
            auto identity = parseKey(encoded);
            identity.instancePath.insert(identity.instancePath.begin(), instancePath.begin(), instancePath.end());
            keys.push_back(key(identity));
        }
        unique(keys);
        if (!keys.empty() || name == kInventory) target.setMetadata(name, std::move(keys));
    }
    target.setMetadata(kScopePolicy, std::string(kScopeRevision));
}

void recordRetention(grh::Graph& graph) {
    if (!graph.metadata(kScopePolicy)) return;
    requirePolicy(graph);
    const auto inspection = inspect(graph);
    if (!inspection.errors.empty()) throw std::invalid_argument(inspection.errors.front());
    std::set<std::string> keys, available, constants, aliases, shared, fragments;
    std::size_t references = 0, bytes = 0, roots = 0;
    for (const auto& record : inspection.nodes) {
        if (!record.roots.empty()) available.insert(record.key);
        else fragments.insert(record.key);
        roots += record.roots.size();
        for (const auto& root : record.roots) {
            auto operation = root.operation;
            const auto args = graph.opOperands(operation);
            if (graph.opKind(operation) == grh::OperationKind::kAssign && args.size() == 1) {
                aliases.insert(record.key);
                operation = graph.valueDef(args[0]);
            }
            if (operation && literal(graph, operation)) constants.insert(record.key);
            if (read(graph, root.operation).roots.size() > 1) shared.insert(record.key);
        }
    }
    for (const auto op : graph.operations()) {
        const auto attributes = read(graph, op);
        const auto add = [&](const std::string& key) { keys.insert(key); ++references; bytes += key.size(); };
        for (const auto& key : attributes.members) add(key);
        for (const auto& key : attributes.origins) add(key);
        for (const auto& root : attributes.roots) add(root.key);
    }
    const auto eliminated = metadataKeys(graph, kEliminated);
    const std::set<std::string> dead(eliminated.begin(), eliminated.end());
    const auto inventory = metadataKeys(graph, kInventory);
    std::size_t missing = 0, removed = 0, uniqueBytes = 0;
    for (const auto& key : keys) uniqueBytes += key.size();
    for (const auto& key : inventory) if (!available.contains(key)) {
        if (dead.contains(key)) ++removed;
        else ++missing;
    }
    grh::AttributeValue census = std::vector<std::string>{"v2",
        "source-nodes=" + std::to_string(inventory.size()), "available=" + std::to_string(available.size()),
        "roots=" + std::to_string(roots), "dead=" + std::to_string(removed),
        "constant=" + std::to_string(constants.size()), "alias=" + std::to_string(aliases.size()),
        "shared=" + std::to_string(shared.size()), "fragments=" + std::to_string(fragments.size()),
        "isolated=" + std::to_string(inspection.isolatedFragments.size()), "missing=" + std::to_string(missing),
        "key-references=" + std::to_string(references), "key-bytes=" + std::to_string(bytes),
        "unique-key-bytes=" + std::to_string(uniqueBytes)};
    if (graph.metadata("firrtl.nodeRetention") != std::optional<grh::AttributeValue>(census))
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
