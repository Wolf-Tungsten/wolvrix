#ifndef WOLVRIX_SEMANTIC_TREE_HPP
#define WOLVRIX_SEMANTIC_TREE_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::grh {

using SemanticId = uint64_t;

constexpr SemanticId semanticScope(SemanticId instance) noexcept { return instance << 32; }
constexpr SemanticId qualifiedSemantic(SemanticId scope, SemanticId id) noexcept { return scope | id; }
constexpr SemanticId semanticInstance(SemanticId id) noexcept { return id >> 32; }
constexpr SemanticId semanticLocalId(SemanticId id) noexcept { return id & SemanticId{0xffffffff}; }

enum class SemanticNodeKind : uint8_t {
    Module,
    Instance,
    Declaration,
    Expression,
    Field,
    Element,
};

struct SemanticNode {
    SemanticId parent = 0;
    SemanticNodeKind kind = SemanticNodeKind::Declaration;
    std::string name;
    int64_t index = -1;
};

class SemanticTree : public std::enable_shared_from_this<SemanticTree> {
public:
    SemanticTree(std::string sourceUnit, std::string circuit, std::string module);

    const std::string& sourceUnit() const noexcept { return sourceUnit_; }
    const std::string& circuit() const noexcept { return circuit_; }
    SemanticId root() const noexcept { return 1; }
    std::size_t size() const noexcept { return nodes_.size() - 1; }
    const SemanticNode& node(SemanticId id) const;
    bool contains(SemanticId id) const noexcept;

    SemanticId addNode(SemanticId parent, SemanticNodeKind kind,
                       std::string_view name, int64_t index = -1);
    SemanticId addFieldPath(SemanticId parent, std::span<const std::string> fields);

    // Share the definition tree. Only the instance node is allocated; qualified
    // IDs are formed as semanticScope(instance) | definitionLocalId.
    SemanticId appendInstance(const SemanticTree& source, SemanticId parent,
                              std::string_view instanceName);

    std::vector<std::string> path(SemanticId id) const;

private:
    std::string sourceUnit_;
    std::string circuit_;
    std::vector<SemanticNode> nodes_;
    std::unordered_map<SemanticId, std::shared_ptr<const SemanticTree>> instances_;
};

} // namespace wolvrix::lib::grh

#endif // WOLVRIX_SEMANTIC_TREE_HPP
