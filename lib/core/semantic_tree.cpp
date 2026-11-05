#include "core/semantic_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wolvrix::lib::grh {

SemanticTree::SemanticTree(std::string sourceUnit, std::string circuit, std::string module)
    : sourceUnit_(std::move(sourceUnit)), circuit_(std::move(circuit)), nodes_(2) {
    if (module.empty()) throw std::invalid_argument("SemanticTree module name is empty");
    nodes_[1].kind = SemanticNodeKind::Module;
    nodes_[1].name = std::move(module);
}

bool SemanticTree::contains(SemanticId id) const noexcept {
    if (!id) return false;
    if (const auto instance = semanticInstance(id)) {
        const auto found = instances_.find(instance);
        return found != instances_.end() && found->second->contains(semanticLocalId(id));
    }
    return id < nodes_.size();
}

const SemanticNode& SemanticTree::node(SemanticId id) const {
    if (semanticInstance(id) || id >= nodes_.size()) throw std::out_of_range("invalid local SemanticId");
    return nodes_[id];
}

SemanticId SemanticTree::addNode(SemanticId parent, SemanticNodeKind kind,
                                 std::string_view name, int64_t index) {
    if (semanticInstance(parent) || parent >= nodes_.size())
        throw std::invalid_argument("SemanticTree parent is invalid");
    if (kind != SemanticNodeKind::Element && name.empty())
        throw std::invalid_argument("SemanticTree node name is empty");
    if (kind == SemanticNodeKind::Element && index < 0)
        throw std::invalid_argument("SemanticTree element index is negative");
    if (nodes_.size() >= UINT32_MAX) throw std::length_error("too many SemanticTree nodes");
    nodes_.push_back({parent, kind, std::string(name), index});
    return static_cast<SemanticId>(nodes_.size() - 1);
}

SemanticId SemanticTree::addFieldPath(SemanticId parent, std::span<const std::string> fields) {
    SemanticId current = parent;
    for (const auto& field : fields) current = addNode(current, SemanticNodeKind::Field, field);
    return current;
}

SemanticId SemanticTree::appendInstance(const SemanticTree& source, SemanticId parent,
                                        std::string_view instanceName) {
    if (semanticInstance(parent) || parent >= nodes_.size())
        throw std::invalid_argument("SemanticTree instance parent is invalid");
    if (instanceName.empty()) throw std::invalid_argument("SemanticTree instance name is empty");
    const SemanticId instance = addNode(parent, SemanticNodeKind::Instance, instanceName);
    instances_.emplace(instance, source.shared_from_this());
    return instance;
}

std::vector<std::string> SemanticTree::path(SemanticId id) const {
    if (!contains(id)) throw std::out_of_range("invalid SemanticId");
    if (const auto instance = semanticInstance(id)) {
        auto result = path(instance);
        const auto tail = instances_.at(instance)->path(semanticLocalId(id));
        result.insert(result.end(), tail.begin(), tail.end());
        return result;
    }
    std::vector<std::string> result;
    for (SemanticId current = id; current; current = node(current).parent) {
        const auto& item = node(current);
        std::string component = item.name;
        if (item.kind == SemanticNodeKind::Element)
            component = "[" + std::to_string(item.index) + "]";
        result.push_back(std::move(component));
    }
    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace wolvrix::lib::grh
