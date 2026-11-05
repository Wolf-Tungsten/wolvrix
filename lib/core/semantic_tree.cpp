#include "core/semantic_tree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

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

SemanticId SemanticTree::appendTree(const SemanticTree& source, SemanticId parent) {
    if (this == &source) throw std::invalid_argument("cannot append a SemanticTree to itself");
    if (!contains(parent)) throw std::invalid_argument("SemanticTree copy parent is invalid");
    if (source.size() > std::numeric_limits<SemanticId>::max() - nodes_.size())
        throw std::length_error("SemanticTree copy node capacity");
    if (source.names_.size() > std::numeric_limits<uint32_t>::max() - names_.size())
        throw std::length_error("SemanticTree copy name capacity");

    const auto copiedRoot = static_cast<SemanticId>(nodes_.size());
    const auto nameBase = static_cast<uint32_t>(names_.size());
    const auto nodeCapacity = nodes_.size() + source.size();
    const auto nameCapacity = names_.size() + source.names_.size();
    if (nodes_.capacity() < nodeCapacity)
        nodes_.reserve(std::max(nodeCapacity, nodes_.capacity() + nodes_.capacity() / 2));
    if (names_.capacity() < nameCapacity)
        names_.reserve(std::max(nameCapacity, names_.capacity() + names_.capacity() / 2));
    nodes_.insert(nodes_.end(), source.nodes_.begin() + 1, source.nodes_.end());
    names_.insert(names_.end(), source.names_.begin(), source.names_.end());
    for (std::size_t offset = 0; offset < source.size(); ++offset) {
        auto& copied = nodes_[copiedRoot + offset];
        copied.parent = offset == 0 ? parent : copiedRoot + copied.parent - 1;
        copied.nameOffset += nameBase;
    }
    return copiedRoot;
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
