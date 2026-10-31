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
        schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != 1))
        throw std::invalid_argument("unsupported firrtl.nodeSchema (expected int64 1)");
    attributes = normalize(std::move(attributes));
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
        graph.setMetadata(kSchema, int64_t{1});
}

void copyMembers(grh::Graph& graph, grh::OperationId from, grh::OperationId to) {
    if (from == to || !annotated(graph, from)) return;
    auto attributes = read(graph, from);
    attributes.roots.clear();
    write(graph, to, std::move(attributes));
    if (const auto clone = graph.getOperation(from).attr("grhsim.nodeClone")) graph.setAttr(to, "grhsim.nodeClone", *clone);
}

Attributes remapResults(Attributes attributes, std::span<const int64_t> oldToNew) {
    for (auto& root : attributes.roots) {
        if (root.result < 0 || static_cast<uint64_t>(root.result) >= oldToNew.size() || oldToNew[root.result] < 0)
            throw std::invalid_argument("node result remap would lose a live root");
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

Attributes unite(Attributes target, const Attributes& source, MergeReason reason) {
    if (reason == MergeReason::SameComputation && normalize(target).members != normalize(source).members)
        throw std::invalid_argument("cross-node attribute union requires a whole-node or execution-leaf merge");
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
        throw std::invalid_argument("primitive CSE would cross a FIRRTL node or execution owner");
    if (graph.opResults(target).size() != graph.opResults(source).size())
        throw std::invalid_argument("CSE result count mismatch");
    const auto reason = finalLeaf(owner(graph, target)) || (literal(graph, target) && literal(graph, source))
        ? MergeReason::ExecutionLeaf : MergeReason::SameComputation;
    write(graph, target, unite(read(graph, target), read(graph, source), reason));
    auto remaining = read(graph, source);
    remaining.roots.clear();
    write(graph, source, std::move(remaining));
}

std::string cseScope(const grh::Graph& graph, grh::OperationId op) {
    if (!op) return {};
    const auto partition = owner(graph, op);
    if (literal(graph, op) || finalLeaf(partition) || !annotated(graph, op)) return {};
    for (const auto& entry : graph.opAttrs(op)) if (entry.key == kMembers) {
        const auto* members = std::get_if<std::vector<std::string>>(&entry.value);
        if (!members) throw std::invalid_argument("wrong node members type");
        if (!members->empty()) {
            std::string result = "node:";
            array(result, *members);
            return result;
        }
    }
    return "orphan:" + std::to_string(op.index) + ":" + std::to_string(op.generation);
}

bool sameScope(const grh::Graph& graph, grh::OperationId left, grh::OperationId right) {
    return left && right && sameOwner(graph, left, right) && cseScope(graph, left) == cseScope(graph, right);
}

bool mayInline(const grh::Graph& graph, grh::OperationId consumer, grh::OperationId producer) {
    if (!producer) return false;
    if (literal(graph, producer) || sameScope(graph, consumer, producer)) return true;
    const auto args = graph.opOperands(producer), results = graph.opResults(producer);
    return consumer && sameOwner(graph, consumer, producer) &&
           graph.opKind(producer) == grh::OperationKind::kAssign && args.size() == 1 && results.size() == 1 &&
           graph.sameValueType(args[0], results[0]);
}

bool rootCarrier(const grh::Graph& graph, grh::OperationId op) {
    if (graph.opKind(op) != grh::OperationKind::kAssign || read(graph, op).roots.empty()) return false;
    const auto args = graph.opOperands(op);
    return args.size() == 1 && !literal(graph, graph.valueDef(args[0])) &&
           !sameScope(graph, op, graph.valueDef(args[0]));
}

grh::ValueId redirectRoots(grh::Graph& graph, grh::ValueId from, grh::ValueId to) {
    const auto source = graph.valueDef(from);
    if (!source || from == to || !annotated(graph, source)) return to;
    auto original = read(graph, source);
    Attributes moved;
    const std::vector<grh::ValueId> results(graph.opResults(source).begin(), graph.opResults(source).end());
    for (const auto& root : original.roots)
        if (root.result >= 0 && static_cast<uint64_t>(root.result) < results.size() && results[root.result] == from)
            moved.roots.push_back({root.key, 0});
    if (moved.roots.empty()) return to;
    auto target = graph.valueDef(to);
    if (target == source && graph.sameValueType(from, to)) {
        const auto index = std::find(results.begin(), results.end(), to) - results.begin();
        for (auto& root : original.roots)
            if (root.result >= 0 && static_cast<uint64_t>(root.result) < results.size() && results[root.result] == from)
                root.result = index;
        write(graph, source, std::move(original));
        return to;
    }
    if (!target || !graph.sameValueType(from, to) ||
        (!literal(graph, target) && !sameScope(graph, source, target))) {
        target = graph.createOperation(grh::OperationKind::kAssign);
        const auto carrier = graph.createValue(graph.valueLogicalType(from));
        graph.addOperand(target, to); graph.addResult(target, carrier);
        moved.members = original.members;
        moved.origins = original.origins;
        write(graph, target, moved);
        for (const auto name : {std::string_view("grhsim.partition"), std::string_view("grhsim.nodeClone"), std::string_view("originModules")})
            for (const auto& entry : graph.opAttrs(source))
                if (entry.key == name) { graph.setAttr(target, name, entry.value); break; }
        if (const auto location = graph.opSrcLoc(source)) graph.setOpSrcLoc(target, *location);
        to = carrier;
    } else {
        const auto targetResults = graph.opResults(target);
        const auto index = std::find(targetResults.begin(), targetResults.end(), to) - targetResults.begin();
        for (auto& root : moved.roots) root.result = index;
        auto destination = read(graph, target);
        destination.roots.insert(destination.roots.end(), moved.roots.begin(), moved.roots.end());
        destination.origins.insert(destination.origins.end(), original.origins.begin(), original.origins.end());
        write(graph, target, std::move(destination));
    }
    std::erase_if(original.roots, [&](const auto& root) {
        return root.result >= 0 && static_cast<uint64_t>(root.result) < results.size() && results[root.result] == from;
    });
    write(graph, source, std::move(original));
    return to;
}

Inspection inspect(const grh::Graph& graph) {
    Inspection result;
    std::map<std::string, NodeRecord> records;
    const auto schema = graph.metadata(kSchema);
    if (schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != 1))
        result.errors.push_back("unsupported firrtl.nodeSchema (expected int64 1)");
    for (const auto op : graph.operations()) {
        if (!annotated(graph, op)) continue;
        try {
            if (!schema) throw std::invalid_argument("node attributes require firrtl.nodeSchema=1");
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
        if (record.roots.size() > 1) {
            // Explicit phase/leaf materializations preserve source identity.
            // Only the owning transform's marked clones may add a root, and
            // each root must live in a different execution owner.
            std::set<std::vector<std::string>> owners;
            unsigned originals = 0; bool valid = true;
            for (const auto& root : record.roots) {
                const auto path = owner(graph, root.operation);
                valid &= !path.empty() && owners.emplace(path.begin(), path.end()).second;
                const auto clone = graph.getOperation(root.operation).attr("grhsim.nodeClone");
                valid &= !clone || std::holds_alternative<bool>(*clone);
                originals += !(clone && std::holds_alternative<bool>(*clone) && std::get<bool>(*clone));
            }
            if (!valid || originals > 1) result.errors.push_back("duplicate live roots for node " + identity);
        }
        if (record.roots.empty()) {
            bool escapes = false;
            const std::unordered_set<grh::OperationId, grh::OperationIdHash> members(record.members.begin(), record.members.end());
            for (const auto op : record.members) for (const auto value : graph.opResults(op)) {
                escapes |= graph.valueIsOutput(value) || graph.valueIsInout(value);
                for (const auto& use : graph.valueUsers(value))
                    escapes |= !members.contains(use.operation);
            }
            if (escapes) result.errors.push_back("live members without a result root for node " + identity);
        }
        result.nodes.push_back(std::move(record));
    }
    return result;
}

void requireValid(const grh::Graph& graph) {
    const auto result = inspect(graph);
    if (!result.errors.empty()) throw std::invalid_argument("FIRRTL node attributes: " + result.errors.front());
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
