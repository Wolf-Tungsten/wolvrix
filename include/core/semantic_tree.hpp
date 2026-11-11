#ifndef WOLVRIX_SEMANTIC_TREE_HPP
#define WOLVRIX_SEMANTIC_TREE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::grh {

using SemanticId = uint32_t;

enum class SemanticNodeKind : uint8_t {
    Module,
    Declaration,
    Expression,
    Field,
    Element,
};

std::string_view toString(SemanticNodeKind kind) noexcept;
std::optional<SemanticNodeKind> parseSemanticNodeKind(std::string_view text) noexcept;

struct SemanticNode {
    SemanticId parent = 0;
    SemanticNodeKind kind = SemanticNodeKind::Declaration;
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    int64_t index = -1;
};

struct SemanticTreePruneStats {
    std::size_t removedNodes = 0;
    std::size_t removedNameBytes = 0;
    std::size_t retainedNodes = 0;
    std::size_t retainedIndirectNodes = 0;
    std::size_t semanticIds = 0;
};

struct SemanticTreePruneResult : SemanticTreePruneStats {
    std::vector<SemanticId> oldToNew;
};

class SemanticTree {
public:
    SemanticTree(std::string sourceUnit, std::string circuit, std::string module);

    const std::string& sourceUnit() const noexcept { return sourceUnit_; }
    const std::string& circuit() const noexcept { return circuit_; }
    SemanticId root() const noexcept { return 1; }
    std::size_t size() const noexcept { return nodes_.size() - 1; }
    std::size_t nameBytes() const noexcept { return names_.size(); }
    const SemanticNode& node(SemanticId id) const;
    std::string_view name(SemanticId id) const;
    bool contains(SemanticId id) const noexcept { return id != 0 && id < nodes_.size(); }

    SemanticId addNode(SemanticId parent, SemanticNodeKind kind,
                       std::string_view name, int64_t index = -1);
    SemanticId addFieldPath(SemanticId parent, std::span<const std::string> fields);

    // Copy the complete source tree, replacing its Module root name with the
    // instance name. Every source ID moves by returnedId - 1.
    SemanticId appendTree(const SemanticTree& source, SemanticId parent,
                          std::string_view rootName);
    std::unique_ptr<SemanticTree> buildPruned(std::span<const uint8_t> referenced,
                                              SemanticTreePruneResult& result) const;

    std::vector<std::string> path(SemanticId id) const;

private:
    std::string sourceUnit_;
    std::string circuit_;
    std::vector<SemanticNode> nodes_;
    std::vector<char> names_;
};

} // namespace wolvrix::lib::grh

#endif // WOLVRIX_SEMANTIC_TREE_HPP
