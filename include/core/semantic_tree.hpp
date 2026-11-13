#ifndef WOLVRIX_SEMANTIC_TREE_HPP
#define WOLVRIX_SEMANTIC_TREE_HPP

#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::grh {

// Node and Path IDs are separate domains. Only leaf Node IDs annotate GRH IR.
using SemanticId = uint32_t;
using SemanticPathId = uint32_t;

struct SemanticEntityRef {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool operator==(const SemanticEntityRef&) const = default;
};

struct SemanticPath {
    SemanticPathId parent = 0;
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t hashNext = 0;
};

struct SemanticNode {
    SemanticPathId parent = 0;
    uint32_t nameOffset = 0;
    uint32_t nameSize = 0;
    uint32_t operationHead = 0;
    uint32_t operationCount = 0;
    uint32_t anchorHead = 0;
    uint32_t anchorCount = 0;
};

struct SemanticTreePruneStats {
    std::size_t removedNodes = 0;
    std::size_t removedNameBytes = 0;
    std::size_t retainedNodes = 0;
    std::size_t retainedIndirectNodes = 0;
    std::size_t semanticIds = 0;
};

struct SemanticTreeAppendResult {
    SemanticPathId rootPath = 0;
    std::vector<SemanticId> nodeIds;
    std::vector<SemanticPathId> pathIds;
};

class SemanticTree {
    struct Member {
        SemanticEntityRef entity;
        uint32_t next = 0;
    };
public:
    class MemberRange {
    public:
        class iterator {
        public:
            using value_type = SemanticEntityRef;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::forward_iterator_tag;
            using pointer = const SemanticEntityRef*;
            using reference = const SemanticEntityRef&;
            iterator() = default;
            const SemanticEntityRef& operator*() const { return (*pool_)[current_].entity; }
            const SemanticEntityRef* operator->() const { return &operator*(); }
            iterator& operator++() { current_ = (*pool_)[current_].next; return *this; }
            iterator operator++(int) { auto old = *this; ++*this; return old; }
            bool operator==(const iterator&) const = default;
        private:
            friend class MemberRange;
            iterator(const std::vector<Member>* pool, uint32_t current) : pool_(pool), current_(current) {}
            const std::vector<Member>* pool_ = nullptr;
            uint32_t current_ = 0;
        };
        iterator begin() const { return iterator(pool_, head_); }
        iterator end() const { return iterator(pool_, 0); }
        std::size_t size() const noexcept { return count_; }
        bool empty() const noexcept { return count_ == 0; }
    private:
        friend class SemanticTree;
        MemberRange(const std::vector<Member>* pool, uint32_t head, uint32_t count)
            : pool_(pool), head_(head), count_(count) {}
        const std::vector<Member>* pool_;
        uint32_t head_, count_;
    };

    SemanticTree(std::string sourceUnit, std::string circuit, std::string module);
    const std::string& sourceUnit() const noexcept { return sourceUnit_; }
    const std::string& circuit() const noexcept { return circuit_; }
    SemanticPathId root() const noexcept { return 1; }
    std::size_t size() const noexcept { return nodeCount_; }
    std::size_t pathCount() const noexcept { return pathCount_; }
    std::size_t nodeCapacity() const noexcept { return nodes_.size(); }
    std::size_t pathCapacity() const noexcept { return paths_.size(); }
    std::size_t nameBytes() const noexcept { return names_.size(); }
    bool contains(SemanticId id) const noexcept { return id && id < nodes_.size() && nodes_[id].nameSize; }
    bool pathContains(SemanticPathId id) const noexcept { return id && id < paths_.size() && paths_[id].nameSize; }
    const SemanticNode& node(SemanticId id) const;
    const SemanticPath& pathEntry(SemanticPathId id) const;
    std::string_view name(SemanticId id) const;
    std::string_view pathName(SemanticPathId id) const;
    std::vector<SemanticId> nodeIds() const;
    std::vector<SemanticPathId> pathIds() const;
    SemanticPathId addPath(SemanticPathId parent, std::string_view name);
    SemanticId addNode(SemanticPathId parent, std::string_view name);
    SemanticPathId importPath(SemanticPathId id, SemanticPathId parent, std::string_view name);
    SemanticId importNode(SemanticId id, SemanticPathId parent, std::string_view name);
    SemanticPathId addFieldPath(SemanticPathId parent, std::span<const std::string> fields);
    SemanticTreeAppendResult appendTree(const SemanticTree& source, SemanticPathId parent,
                                       std::string_view rootName);
    std::vector<std::string> path(SemanticId id) const;
    std::vector<std::string> pathComponents(SemanticPathId id) const;
    // Escape '.', '\\', '[' and ']' so a flattened name is reversible.
    static std::string escapeName(std::string_view name);
    static std::vector<std::string> splitQualifiedName(std::string_view name);
    static bool validQualifiedName(std::string_view name) noexcept;
    static std::string qualifiedName(std::span<const std::string> components);
    std::string qualifiedName(SemanticId id, std::span<const std::string> prefix = {}) const;
    bool namesFlattened() const noexcept { return flattened_; }
    void setNamesFlattened(bool flattened);
    void flatten();
    SemanticTreePruneStats pruneEmptyPaths();
    bool isFlat() const noexcept;
    MemberRange operations(SemanticId id) const;
    MemberRange anchors(SemanticId id) const;
    void clearMembers();
    void reserveMembers(std::size_t additional);
    // Graph owns synchronization with the reverse IR annotations.
    void addOperation(SemanticId id, SemanticEntityRef operation);
    void removeOperation(SemanticId id, SemanticEntityRef operation);
    void addAnchor(SemanticId id, SemanticEntityRef value);
    void removeAnchor(SemanticId id, SemanticEntityRef value);

private:
    uint32_t storeName(std::string_view name);
    static std::size_t pathHash(SemanticPathId parent, std::string_view name);
    void rebuildPathIndex();
    void appendMember(uint32_t& head, uint32_t& count, SemanticEntityRef entity);
    void removeMember(uint32_t& head, uint32_t& count, SemanticEntityRef entity);
    std::string sourceUnit_, circuit_;
    std::vector<SemanticNode> nodes_{1};
    std::vector<SemanticPath> paths_{1};
    std::vector<char> names_;
    std::unordered_map<std::size_t, SemanticPathId> pathIndex_;
    std::vector<Member> members_{1};
    uint32_t freeMember_ = 0;
    std::size_t nodeCount_ = 0, pathCount_ = 0;
    bool flattened_ = false;
};

} // namespace wolvrix::lib::grh
#endif // WOLVRIX_SEMANTIC_TREE_HPP
