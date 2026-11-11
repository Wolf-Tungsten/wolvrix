#include "core/semantic_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace wolvrix::lib::grh {

std::string_view toString(SemanticNodeKind kind) noexcept {
    switch (kind) {
    case SemanticNodeKind::Module: return "module";
    case SemanticNodeKind::Declaration: return "declaration";
    case SemanticNodeKind::Expression: return "expression";
    case SemanticNodeKind::Field: return "field";
    case SemanticNodeKind::Element: return "element";
    }
    return "unknown";
}

std::optional<SemanticNodeKind> parseSemanticNodeKind(std::string_view text) noexcept {
    if (text == "module") return SemanticNodeKind::Module;
    if (text == "declaration") return SemanticNodeKind::Declaration;
    if (text == "expression") return SemanticNodeKind::Expression;
    if (text == "field") return SemanticNodeKind::Field;
    if (text == "element") return SemanticNodeKind::Element;
    return std::nullopt;
}

SemanticTree::SemanticTree(std::string sourceUnit, std::string circuit, std::string module)
    : sourceUnit_(std::move(sourceUnit)), circuit_(std::move(circuit)), nodes_(2) {
    if (module.empty()) throw std::invalid_argument("SemanticTree module name is empty");
    if (module.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("SemanticTree module name is too large");
    names_.assign(module.begin(), module.end());
    nodes_[1].kind = SemanticNodeKind::Module;
    nodes_[1].nameSize = static_cast<uint32_t>(names_.size());
}

const SemanticNode& SemanticTree::node(SemanticId id) const {
    if (!contains(id)) throw std::out_of_range("invalid SemanticId");
    return nodes_[id];
}

std::string_view SemanticTree::name(SemanticId id) const {
    const auto& item = node(id);
    if (item.nameSize == 0) return {};
    return std::string_view(names_.data() + item.nameOffset, item.nameSize);
}

SemanticId SemanticTree::addNode(SemanticId parent, SemanticNodeKind kind,
                                 std::string_view name, int64_t index) {
    if (!contains(parent)) throw std::invalid_argument("SemanticTree parent is invalid");
    if (kind != SemanticNodeKind::Element && name.empty())
        throw std::invalid_argument("SemanticTree node name is empty");
    if (kind == SemanticNodeKind::Element && index < 0)
        throw std::invalid_argument("SemanticTree element index is negative");
    if (kind != SemanticNodeKind::Element && index != -1)
        throw std::invalid_argument("SemanticTree non-element index must be omitted");
    if (nodes_.size() >= std::numeric_limits<SemanticId>::max())
        throw std::length_error("too many SemanticTree nodes");
    if (name.size() > std::numeric_limits<uint32_t>::max() - names_.size())
        throw std::length_error("SemanticTree name storage capacity");
    const auto offset = static_cast<uint32_t>(names_.size());
    names_.insert(names_.end(), name.begin(), name.end());
    nodes_.push_back({parent, kind, offset, static_cast<uint32_t>(name.size()), index});
    return static_cast<SemanticId>(nodes_.size() - 1);
}

SemanticId SemanticTree::addFieldPath(SemanticId parent, std::span<const std::string> fields) {
    SemanticId current = parent;
    for (const auto& field : fields) current = addNode(current, SemanticNodeKind::Field, field);
    return current;
}

SemanticId SemanticTree::appendTree(const SemanticTree& source, SemanticId parent,
                                    std::string_view rootName) {
    if (rootName.empty()) throw std::invalid_argument("SemanticTree copy root name is empty");
    if (this == &source) throw std::invalid_argument("cannot append a SemanticTree to itself");
    if (!contains(parent)) throw std::invalid_argument("SemanticTree copy parent is invalid");
    const auto& sourceRoot = source.nodes_[source.root()];
    if (sourceRoot.nameOffset != 0 || sourceRoot.nameSize > source.names_.size())
        throw std::invalid_argument("SemanticTree source root name must be first");
    if (source.size() > std::numeric_limits<SemanticId>::max() - nodes_.size())
        throw std::length_error("SemanticTree copy node capacity");
    const std::size_t suffixBytes = source.names_.size() - sourceRoot.nameSize;
    if (rootName.size() > std::numeric_limits<uint32_t>::max() - names_.size() ||
        suffixBytes > std::numeric_limits<uint32_t>::max() - names_.size() - rootName.size())
        throw std::length_error("SemanticTree copy name capacity");

    const auto copiedRoot = static_cast<SemanticId>(nodes_.size());
    const auto nameBase = static_cast<uint32_t>(names_.size());
    const auto nodeCapacity = nodes_.size() + source.size();
    const auto nameCapacity = names_.size() + rootName.size() + suffixBytes;
    if (nodes_.capacity() < nodeCapacity)
        nodes_.reserve(std::max(nodeCapacity, nodes_.capacity() + nodes_.capacity() / 2));
    if (names_.capacity() < nameCapacity)
        names_.reserve(std::max(nameCapacity, names_.capacity() + names_.capacity() / 2));
    nodes_.insert(nodes_.end(), source.nodes_.begin() + 1, source.nodes_.end());
    names_.insert(names_.end(), rootName.begin(), rootName.end());
    names_.insert(names_.end(), source.names_.begin() + sourceRoot.nameSize, source.names_.end());
    for (std::size_t offset = 0; offset < source.size(); ++offset) {
        auto& copied = nodes_[copiedRoot + offset];
        copied.parent = offset == 0 ? parent : copiedRoot + copied.parent - 1;
        if (offset == 0) {
            copied.nameOffset = nameBase;
            copied.nameSize = static_cast<uint32_t>(rootName.size());
        } else {
            copied.nameOffset = nameBase + static_cast<uint32_t>(rootName.size()) +
                copied.nameOffset - sourceRoot.nameSize;
        }
    }
    return copiedRoot;
}

std::unique_ptr<SemanticTree> SemanticTree::buildPruned(
    std::span<const uint8_t> referenced, SemanticTreePruneResult& result) const {
    result = {};
    if (referenced.size() != nodes_.size())
        throw std::invalid_argument("SemanticTree reference bitmap size mismatch");

    std::vector<uint8_t> keep(referenced.begin(), referenced.end());
    keep[0] = 0;
    keep[root()] = 1;
    for (SemanticId id = static_cast<SemanticId>(nodes_.size() - 1); id > root(); --id) {
        if (keep[id]) keep[nodes_[id].parent] = 1;
    }

    struct SiblingKey {
        SemanticId parent;
        std::string_view name;
        bool operator==(const SiblingKey&) const = default;
    };
    struct SiblingHash {
        std::size_t operator()(const SiblingKey& key) const noexcept {
            std::size_t hash = std::hash<SemanticId>{}(key.parent);
            hash ^= std::hash<std::string_view>{}(key.name) + 0x9e3779b9u +
                (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    std::unordered_map<SiblingKey, SemanticId, SiblingHash> modules;
    for (SemanticId id = root() + 1; id < nodes_.size(); ++id) {
        if (nodes_[id].kind == SemanticNodeKind::Module)
            modules.emplace(SiblingKey{nodes_[id].parent, name(id)}, id);
    }
    std::vector<std::pair<SemanticId, SemanticId>> instanceLinks;
    instanceLinks.reserve(modules.size());
    for (SemanticId id = root() + 1; id < nodes_.size(); ++id) {
        const auto& item = nodes_[id];
        if (item.kind != SemanticNodeKind::Declaration ||
            nodes_[item.parent].kind != SemanticNodeKind::Module)
            continue;
        const auto module = modules.find(SiblingKey{item.parent, name(id)});
        if (module != modules.end()) instanceLinks.emplace_back(module->second, id);
    }
    for (const auto& [module, declaration] : instanceLinks) {
        if (keep[module] || keep[declaration]) {
            keep[module] = 1;
            keep[declaration] = 1;
        }
    }
    for (SemanticId id = static_cast<SemanticId>(nodes_.size() - 1); id > root(); --id) {
        if (keep[id]) keep[nodes_[id].parent] = 1;
    }

    std::size_t retainedNameBytes = 0;
    for (SemanticId id = root(); id < nodes_.size(); ++id) {
        if (!keep[id]) continue;
        ++result.retainedNodes;
        retainedNameBytes += nodes_[id].nameSize;
        if (id != root() && !referenced[id]) ++result.retainedIndirectNodes;
    }
    result.removedNodes = size() - result.retainedNodes;
    result.removedNameBytes = names_.size() - retainedNameBytes;
    if (result.removedNodes == 0) return nullptr;

    result.oldToNew.assign(nodes_.size(), 0);
    auto compacted = std::make_unique<SemanticTree>(sourceUnit_, circuit_, std::string(name(root())));
    auto& compactedNodes = compacted->nodes_;
    auto& compactedNames = compacted->names_;
    compactedNodes.clear();
    compactedNodes.resize(1);
    compactedNames.clear();
    compactedNodes.reserve(result.retainedNodes + 1);
    compactedNames.reserve(retainedNameBytes);
    for (SemanticId id = root(); id < nodes_.size(); ++id) {
        if (!keep[id]) continue;
        const auto& source = nodes_[id];
        SemanticNode target = source;
        if (id == root()) {
            target.parent = 0;
        } else {
            target.parent = result.oldToNew[source.parent];
            if (target.parent == 0) throw std::logic_error("SemanticTree retained parent was removed");
        }
        if (source.nameOffset > names_.size() ||
            source.nameSize > names_.size() - source.nameOffset)
            throw std::logic_error("SemanticTree name range is invalid");
        result.oldToNew[id] = static_cast<SemanticId>(compactedNodes.size());
        target.nameOffset = static_cast<uint32_t>(compactedNames.size());
        compactedNames.insert(compactedNames.end(), names_.begin() + source.nameOffset,
                              names_.begin() + source.nameOffset + source.nameSize);
        compactedNodes.push_back(target);
    }
    if (compactedNodes.size() != result.retainedNodes + 1 ||
        compactedNames.size() != retainedNameBytes)
        throw std::logic_error("SemanticTree compaction count mismatch");
    return compacted;
}

std::vector<std::string> SemanticTree::path(SemanticId id) const {
    if (!contains(id)) throw std::out_of_range("invalid SemanticId");
    std::vector<std::string> result;
    for (SemanticId current = id; current; current = node(current).parent) {
        const auto& item = node(current);
        std::string component(name(current));
        if (item.kind == SemanticNodeKind::Element)
            component = "[" + std::to_string(item.index) + "]";
        result.push_back(std::move(component));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace wolvrix::lib::grh
