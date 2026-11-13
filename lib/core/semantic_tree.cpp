#include "core/semantic_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wolvrix::lib::grh {

SemanticTree::SemanticTree(std::string sourceUnit, std::string circuit, std::string module)
    : sourceUnit_(std::move(sourceUnit)), circuit_(std::move(circuit)) {
    importPath(root(), 0, module);
}

uint32_t SemanticTree::storeName(std::string_view name) {
    if (name.empty()) throw std::invalid_argument("SemanticTree name is empty");
    if (name.size() > std::numeric_limits<uint32_t>::max() - names_.size())
        throw std::length_error("SemanticTree name storage capacity");
    const auto offset = static_cast<uint32_t>(names_.size());
    const std::less<const char*> before;
    if (!names_.empty() && !before(name.data(), names_.data()) &&
        before(name.data(), names_.data() + names_.size())) {
        const std::string copy(name);
        names_.insert(names_.end(), copy.begin(), copy.end());
    } else {
        names_.insert(names_.end(), name.begin(), name.end());
    }
    return offset;
}

const SemanticNode& SemanticTree::node(SemanticId id) const {
    if (!contains(id)) throw std::out_of_range("invalid Semantic Node ID");
    return nodes_[id];
}
const SemanticPath& SemanticTree::pathEntry(SemanticPathId id) const {
    if (!pathContains(id)) throw std::out_of_range("invalid Semantic Path ID");
    return paths_[id];
}
std::string_view SemanticTree::name(SemanticId id) const {
    const auto& item = node(id);
    return {names_.data() + item.nameOffset, item.nameSize};
}
std::string_view SemanticTree::pathName(SemanticPathId id) const {
    const auto& item = pathEntry(id);
    return {names_.data() + item.nameOffset, item.nameSize};
}
std::vector<SemanticId> SemanticTree::nodeIds() const {
    std::vector<SemanticId> result;
    result.reserve(nodeCount_);
    for (SemanticId id = 1; id < nodes_.size(); ++id) if (contains(id)) result.push_back(id);
    return result;
}
std::vector<SemanticPathId> SemanticTree::pathIds() const {
    std::vector<SemanticPathId> result;
    result.reserve(pathCount_);
    for (SemanticPathId id = 1; id < paths_.size(); ++id) if (pathContains(id)) result.push_back(id);
    return result;
}
SemanticPathId SemanticTree::importPath(SemanticPathId id, SemanticPathId parent, std::string_view name) {
    if (!id || pathContains(id)) throw std::invalid_argument("duplicate or invalid Semantic Path ID");
    if (flattened_) throw std::invalid_argument("cannot add a Path to a flattened SemanticTree");
    if (id == root() ? parent != 0 : !pathContains(parent))
        throw std::invalid_argument("Semantic Path parent is invalid");
    const auto hash = pathHash(parent, name);
    const auto offset = storeName(name);
    if (id >= paths_.size()) paths_.resize(static_cast<std::size_t>(id) + 1);
    auto& first = pathIndex_[hash];
    paths_[id] = {parent, offset, static_cast<uint32_t>(name.size()), first};
    first = id;
    ++pathCount_;
    flattened_ = false;
    return id;
}
SemanticId SemanticTree::importNode(SemanticId id, SemanticPathId parent, std::string_view name) {
    if (!id || contains(id)) throw std::invalid_argument("duplicate or invalid Semantic Node ID");
    if (!pathContains(parent)) throw std::invalid_argument("Semantic Node parent is invalid");
    const auto offset = storeName(name);
    if (id >= nodes_.size()) nodes_.resize(static_cast<std::size_t>(id) + 1);
    nodes_[id] = {parent, offset, static_cast<uint32_t>(name.size())};
    ++nodeCount_;
    return id;
}
SemanticPathId SemanticTree::addPath(SemanticPathId parent, std::string_view name) {
    if (!pathContains(parent)) throw std::invalid_argument("Semantic Path parent is invalid");
    const auto found = pathIndex_.find(pathHash(parent, name));
    if (found != pathIndex_.end())
        for (auto id = found->second; id; id = paths_[id].hashNext)
            if (paths_[id].parent == parent && pathName(id) == name) return id;
    if (paths_.size() >= std::numeric_limits<SemanticPathId>::max())
        throw std::length_error("too many Semantic Paths");
    return importPath(static_cast<SemanticPathId>(paths_.size()), parent, name);
}
SemanticId SemanticTree::addNode(SemanticPathId parent, std::string_view name) {
    if (nodes_.size() >= std::numeric_limits<SemanticId>::max())
        throw std::length_error("too many Semantic Nodes");
    return importNode(static_cast<SemanticId>(nodes_.size()), parent, name);
}
SemanticPathId SemanticTree::addFieldPath(SemanticPathId parent, std::span<const std::string> fields) {
    for (const auto& field : fields) parent = addPath(parent, field);
    return parent;
}
SemanticTreeAppendResult SemanticTree::appendTree(const SemanticTree& source, SemanticPathId parent,
                                                 std::string_view rootName) {
    if (this == &source) throw std::invalid_argument("cannot append a SemanticTree to itself");
    SemanticTreeAppendResult result;
    result.pathIds.resize(source.pathCapacity());
    result.nodeIds.resize(source.nodeCapacity());
    result.rootPath = addPath(parent, rootName);
    result.pathIds[source.root()] = result.rootPath;
    auto pending = source.pathIds();
    std::size_t remaining = pending.size() - 1;
    while (remaining) {
        bool progress = false;
        for (const auto id : pending) {
            if (result.pathIds[id]) continue;
            const auto mappedParent = result.pathIds[source.pathEntry(id).parent];
            if (!mappedParent) continue;
            result.pathIds[id] = addPath(mappedParent, source.pathName(id));
            --remaining;
            progress = true;
        }
        if (!progress) throw std::invalid_argument("SemanticTree contains a Path cycle");
    }
    for (const auto id : source.nodeIds()) {
        auto copiedParent = result.pathIds[source.node(id).parent];
        if (source.namesFlattened()) {
            auto components = splitQualifiedName(source.name(id));
            for (std::size_t i = 0; i + 1 < components.size(); ++i)
                copiedParent = addPath(copiedParent, components[i]);
            result.nodeIds[id] = addNode(copiedParent, components.back());
        } else {
            result.nodeIds[id] = addNode(copiedParent, source.name(id));
        }
    }
    return result;
}
std::vector<std::string> SemanticTree::pathComponents(SemanticPathId id) const {
    std::vector<std::string> result;
    for (std::size_t depth = 0; id; ++depth) {
        if (depth > pathCount_) throw std::logic_error("SemanticTree Path cycle");
        result.emplace_back(pathName(id));
        id = pathEntry(id).parent;
    }
    std::reverse(result.begin(), result.end());
    return result;
}
std::vector<std::string> SemanticTree::path(SemanticId id) const {
    auto result = pathComponents(node(id).parent);
    if (flattened_) {
        const auto components = splitQualifiedName(name(id));
        result.insert(result.end(), components.begin(), components.end());
    } else {
        result.emplace_back(name(id));
    }
    return result;
}
std::string SemanticTree::escapeName(std::string_view name) {
    std::string result;
    for (const char ch : name) {
        if (ch == '.' || ch == '\\' || ch == '[' || ch == ']') result += '\\';
        result += ch;
    }
    return result;
}
std::vector<std::string> SemanticTree::splitQualifiedName(std::string_view name) {
    std::vector<std::string> result(1);
    bool escaped = false;
    for (const char ch : name) {
        if (escaped) {
            if (ch != '.' && ch != '\\' && ch != '[' && ch != ']')
                throw std::invalid_argument("invalid flattened Semantic Node escape");
            result.back() += ch;
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '.') {
            if (result.back().empty()) throw std::invalid_argument("empty flattened Semantic Node path token");
            result.emplace_back();
        } else if (ch == '[' || ch == ']') {
            throw std::invalid_argument("unescaped flattened Semantic Node name token");
        } else {
            result.back() += ch;
        }
    }
    if (escaped || result.back().empty()) throw std::invalid_argument("invalid flattened Semantic Node name");
    return result;
}
bool SemanticTree::validQualifiedName(std::string_view name) noexcept {
    bool escaped = false, token = false;
    for (const auto ch : name) {
        if (escaped) {
            if (ch != '.' && ch != '\\' && ch != '[' && ch != ']') return false;
            token = true;
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '.') {
            if (!token) return false;
            token = false;
        } else if (ch == '[' || ch == ']') {
            return false;
        } else {
            token = true;
        }
    }
    return token && !escaped;
}
std::string SemanticTree::qualifiedName(std::span<const std::string> components) {
    std::string result;
    for (const auto& component : components) {
        if (!result.empty()) result += '.';
        result += escapeName(component);
    }
    return result;
}
std::string SemanticTree::qualifiedName(SemanticId id, std::span<const std::string> prefix) const {
    std::string result = qualifiedName(prefix);
    if (!result.empty()) result += '.';
    if (flattened_) return result + std::string(name(id));
    const auto components = path(id);
    return result + qualifiedName(std::span<const std::string>(components).subspan(1));
}
void SemanticTree::setNamesFlattened(bool flattened) {
    if (flattened && !isFlat()) throw std::invalid_argument("flattened SemanticTree has internal Paths");
    if (flattened) for (const auto id : nodeIds())
        if (!validQualifiedName(name(id))) throw std::invalid_argument("invalid flattened Semantic Node name");
    flattened_ = flattened;
}
void SemanticTree::flatten() {
    if (flattened_) return;
    std::vector<char> nextNames;
    const std::string rootName(pathName(root()));
    nextNames.insert(nextNames.end(), rootName.begin(), rootName.end());
    for (const auto id : nodeIds()) {
        const auto components = path(id);
        std::string qualified;
        for (std::size_t i = 1; i < components.size(); ++i) {
            if (i != 1) qualified += '.';
            qualified += escapeName(components[i]);
        }
        auto& item = nodes_[id];
        if (qualified.size() > std::numeric_limits<uint32_t>::max() - nextNames.size())
            throw std::length_error("flattened SemanticTree name storage capacity");
        item.parent = root();
        item.nameOffset = static_cast<uint32_t>(nextNames.size());
        item.nameSize = static_cast<uint32_t>(qualified.size());
        nextNames.insert(nextNames.end(), qualified.begin(), qualified.end());
    }
    names_.swap(nextNames);
    paths_.assign(2, {});
    paths_[root()] = {0, 0, static_cast<uint32_t>(rootName.size())};
    pathCount_ = 1;
    rebuildPathIndex();
    flattened_ = true;
}
SemanticTreePruneStats SemanticTree::pruneEmptyPaths() {
    SemanticTreePruneStats result;
    result.retainedNodes = nodeCount_;
    std::vector<uint8_t> keep(paths_.size());
    keep[root()] = 1;
    for (const auto id : nodeIds()) {
        auto current = nodes_[id].parent;
        while (current && !keep[current]) { keep[current] = 1; current = pathEntry(current).parent; }
    }
    for (SemanticPathId id = 1; id < paths_.size(); ++id) {
        if (!pathContains(id)) continue;
        if (keep[id]) { ++result.retainedIndirectNodes; continue; }
        paths_[id] = {};
        --pathCount_;
    }
    rebuildPathIndex();
    return result;
}
std::size_t SemanticTree::pathHash(SemanticPathId parent, std::string_view name) {
    return std::hash<std::string_view>{}(name) ^ (std::size_t(parent) * 0x9e3779b9u);
}
void SemanticTree::rebuildPathIndex() {
    pathIndex_.clear();
    for (SemanticPathId id = 1; id < paths_.size(); ++id) {
        if (!pathContains(id)) continue;
        auto& first = pathIndex_[pathHash(paths_[id].parent, pathName(id))];
        paths_[id].hashNext = first;
        first = id;
    }
}
bool SemanticTree::isFlat() const noexcept {
    if (pathCount_ != 1) return false;
    for (const auto& item : nodes_) if (item.nameSize && item.parent != root()) return false;
    return true;
}
SemanticTree::MemberRange SemanticTree::operations(SemanticId id) const {
    const auto& item = node(id);
    return {&members_, item.operationHead, item.operationCount};
}
SemanticTree::MemberRange SemanticTree::anchors(SemanticId id) const {
    const auto& item = node(id);
    return {&members_, item.anchorHead, item.anchorCount};
}
void SemanticTree::clearMembers() {
    members_.assign(1, {});
    freeMember_ = 0;
    for (auto& item : nodes_) item.operationHead = item.operationCount = item.anchorHead = item.anchorCount = 0;
}
void SemanticTree::reserveMembers(std::size_t additional) {
    if (additional > std::numeric_limits<uint32_t>::max() - members_.size())
        throw std::length_error("too many Semantic Node memberships");
    const auto required = members_.size() + additional;
    if (required <= members_.capacity()) return;
    const auto grown = members_.capacity() + members_.capacity() / 2 + 16;
    members_.reserve(std::max(required, std::min<std::size_t>(grown, std::numeric_limits<uint32_t>::max())));
}
void SemanticTree::appendMember(uint32_t& head, uint32_t& count, SemanticEntityRef entity) {
    if (!entity.index) throw std::invalid_argument("Semantic Node member is invalid");
    uint32_t slot = freeMember_;
    if (slot) freeMember_ = members_[slot].next;
    else {
        if (members_.size() >= std::numeric_limits<uint32_t>::max())
            throw std::length_error("too many Semantic Node memberships");
        slot = static_cast<uint32_t>(members_.size());
        members_.push_back({});
    }
    members_[slot] = {entity, head};
    head = slot;
    ++count;
}
void SemanticTree::removeMember(uint32_t& head, uint32_t& count, SemanticEntityRef entity) {
    uint32_t* link = &head;
    while (*link) {
        const auto slot = *link;
        if (members_[slot].entity == entity) {
            *link = members_[slot].next;
            members_[slot].next = freeMember_;
            freeMember_ = slot;
            --count;
            return;
        }
        link = &members_[slot].next;
    }
    throw std::logic_error("Semantic Node reverse membership is missing");
}
void SemanticTree::addOperation(SemanticId id, SemanticEntityRef entity) {
    node(id); auto& item = nodes_[id]; appendMember(item.operationHead, item.operationCount, entity);
}
void SemanticTree::removeOperation(SemanticId id, SemanticEntityRef entity) {
    node(id); auto& item = nodes_[id]; removeMember(item.operationHead, item.operationCount, entity);
}
void SemanticTree::addAnchor(SemanticId id, SemanticEntityRef entity) {
    node(id); auto& item = nodes_[id]; appendMember(item.anchorHead, item.anchorCount, entity);
}
void SemanticTree::removeAnchor(SemanticId id, SemanticEntityRef entity) {
    node(id); auto& item = nodes_[id]; removeMember(item.anchorHead, item.anchorCount, entity);
}

} // namespace wolvrix::lib::grh
