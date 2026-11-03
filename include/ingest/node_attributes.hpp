#ifndef WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP
#define WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP

#include "core/grh.hpp"

#include <span>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::ingest::node {

inline constexpr std::string_view kSchema = "firrtl.nodeSchema";
inline constexpr std::string_view kMembers = "firrtl.node.members";
inline constexpr std::string_view kRoots = "firrtl.node.roots";
inline constexpr std::string_view kRootResults = "firrtl.node.rootResults";
inline constexpr std::string_view kOrigins = "firrtl.node.origins";
inline constexpr std::string_view kScopePolicy = "firrtl.nodeScopePolicy";
inline constexpr std::string_view kScopeRevision = "soft-node-v2";
inline constexpr int64_t kSchemaRevision = 2;
inline constexpr std::size_t kMaxAssociations = 64;
inline constexpr std::string_view kInventory = "firrtl.nodeInventory";
inline constexpr std::string_view kEliminated = "firrtl.nodeEliminated";

struct Identity {
    std::string sourceUnit;
    std::string circuit;
    std::string module;
    std::vector<std::string> instancePath;
    std::string name;
    std::vector<std::string> fieldPath;
    bool operator==(const Identity&) const = default;
};

std::string key(const Identity& identity);
Identity parseKey(std::string_view encoded);
bool isAttribute(std::string_view name) noexcept;
bool annotated(const grh::Graph& graph, grh::OperationId op);

struct Root {
    std::string key;
    int64_t result = 0;
    auto operator<=>(const Root&) const = default;
};

struct Attributes {
    std::vector<std::string> members;
    std::vector<Root> roots;
    std::vector<std::string> origins;
    bool operator==(const Attributes&) const = default;
};

using NodeKeyId = uint32_t;

// Transient canonical key storage used by flattening and other hot paths.
// Persistence still uses the existing schema-v2 strings; IDs are valid only
// for the lifetime of this interner.
class NodeKeyInterner {
public:
    NodeKeyInterner() = default;
    NodeKeyInterner(const NodeKeyInterner& other);
    NodeKeyInterner& operator=(const NodeKeyInterner& other);
    NodeKeyInterner(NodeKeyInterner&& other);
    NodeKeyInterner& operator=(NodeKeyInterner&& other);

    NodeKeyId intern(std::string_view key);
    std::string_view value(NodeKeyId id) const;
    std::size_t size() const noexcept { return values_.size(); }
    std::size_t bytes() const noexcept { return bytes_; }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
        std::size_t operator()(const std::string& value) const noexcept { return (*this)(std::string_view(value)); }
    };
    struct Equal {
        using is_transparent = void;
        bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
    };
    // deque keeps the character storage stable while ids_ borrows it. This
    // avoids retaining a second owned copy of every canonical key.
    std::unordered_map<std::string_view, NodeKeyId, Hash, Equal> ids_;
    std::deque<std::string> values_;
    std::size_t bytes_ = 0;

    void rebuildIndex();
};

Attributes read(const grh::Graph& graph, grh::OperationId op);
Attributes normalize(Attributes attributes);
void write(grh::Graph& graph, grh::OperationId op, Attributes attributes);
// Fast path for a freshly-created operation with one already-canonical node
// key. The caller owns the proof that key() produced the canonical encoding;
// this function preserves the same schema, member, origin and optional root
// attributes without re-normalizing or reparsing the key.
void writeSingleCanonical(grh::Graph& graph, grh::OperationId op,
                          std::string_view key, bool root = false,
                          int64_t rootResult = 0);
// Lowering fast path. The caller must have initialized firrtl.nodeSchema to
// kSchemaRevision and must pass a key produced by key().
void writeSingleCanonicalReady(grh::Graph& graph, grh::OperationId op,
                               std::string_view key, bool root = false,
                               int64_t rootResult = 0);
void copyMembers(grh::Graph& graph, grh::OperationId from, grh::OperationId to);
// One immutable instance path per context. Cache strings own their storage;
// graph attribute edits and recursive path changes cannot invalidate them.
class QualificationContext {
public:
    struct Stats {
        std::size_t lookups = 0;
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t inputBytes = 0;
        std::size_t outputBytes = 0;
    };

    explicit QualificationContext(std::span<const std::string> instancePath);
    const Stats& stats() const noexcept { return stats_; }
private:
    friend void copyQualified(const grh::Graph&, grh::OperationId,
                              grh::Graph&, grh::OperationId, QualificationContext&);
    const std::vector<std::string> instancePath_;
    NodeKeyInterner sourceKeys_;
    NodeKeyInterner qualifiedKeys_;
    std::unordered_map<NodeKeyId, NodeKeyId> qualified_;
    Stats stats_;
};
void qualify(grh::Graph& graph, grh::OperationId op, std::span<const std::string> instancePath);
void qualify(grh::Graph& graph, grh::OperationId op, QualificationContext& context);
// Borrow source associations until the final qualified payload is built, then
// move it into the target. Result positions must be preserved by the clone.
void copyQualified(const grh::Graph& source, grh::OperationId from,
                   grh::Graph& target, grh::OperationId to, QualificationContext& context);
Attributes remapResults(Attributes attributes, std::span<const int64_t> oldToNew);

enum class MergeReason { SameComputation, WholeNode, ExecutionLeaf };
Attributes unite(Attributes target, const Attributes& source, MergeReason reason);
// The caller must prove whole-node equivalence; this function only moves metadata.
void mergeEquivalentOperations(grh::Graph& graph, grh::OperationId target,
                              grh::OperationId source, std::span<const int64_t> resultMap);
// Functional equivalence is the caller's proof; this checks the sharing scope.
void mergeCseOperations(grh::Graph& graph, grh::OperationId target, grh::OperationId source);

// Hints never add an optimization scope. Only actual execution owners constrain
// sharing; the empty key keeps functional signatures annotation-transparent.
std::string cseScope(const grh::Graph& graph, grh::OperationId op);
bool sameScope(const grh::Graph& graph, grh::OperationId left, grh::OperationId right);
bool mayInline(const grh::Graph& graph, grh::OperationId consumer, grh::OperationId producer);
bool rootCarrier(const grh::Graph& graph, grh::OperationId op);

// Update optional result associations without creating operations or values.
grh::ValueId redirectRoots(grh::Graph& graph, grh::ValueId from, grh::ValueId to);

struct RootLocation {
    grh::OperationId operation;
    int64_t result = 0;
    grh::ValueId value;
};

struct NodeRecord {
    std::string key;
    std::vector<grh::OperationId> members;
    std::vector<RootLocation> roots;
};

struct Inspection {
    std::vector<std::string> errors;
    std::vector<NodeRecord> nodes;
    std::vector<grh::OperationId> isolatedFragments;
    std::string report(const grh::Graph& graph) const;
};

// A short-lived validation result cache for transformation pipelines. Graph
// mutations advance the owning design revision, so a cached result is only
// reusable while both the graph identity and revision remain unchanged.
struct ValidationCache {
    struct Entry {
        uint64_t instance = 0;
        uint64_t revision = 0;
        grh::GraphId graph;
        std::vector<std::string> errors;
    };
    bool populated = false;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::unordered_map<uint64_t, Entry> entries;

    void clear() {
        populated = false;
        hits = 0;
        misses = 0;
        entries.clear();
    }
};

Inspection inspect(const grh::Graph& graph);
// Validate node attributes while collecting every diagnostic, without
// materializing the inspection census (node records, roots, or fragments).
// This is the appropriate entry point for passes that only need to reject
// malformed annotations.
std::vector<std::string> scanAttributeErrors(const grh::Graph& graph);
std::vector<std::string> scanAttributeErrors(const grh::Graph& graph, ValidationCache& cache);
void requireValid(const grh::Graph& graph);
void requireValid(const grh::Graph& graph, ValidationCache& cache);

// Establish only at source construction, never to upgrade a loaded checkpoint.
void establishPolicy(grh::Graph& graph);
void requirePolicy(const grh::Graph& graph);
// Record only proven-dead results, before erasing their operations. Explicitly
// flush once at the graph/pass boundary, before verification or persistence.
class DeadRootBatch {
public:
    explicit DeadRootBatch(grh::Graph& graph);
    void record(grh::OperationId op);
    void flush();
private:
    grh::Graph& graph_;
    bool enabled_;
    std::vector<std::string> pending_;
};
// Single-operation convenience; repeated deletions should share a batch.
void recordDead(grh::Graph& graph, grh::OperationId op);
// Own qualified imports until an explicit graph/pass boundary. Source graphs
// may be deleted before flush; no verification/persistence may precede it.
// Flush on success and explicit failure returns; destruction never publishes.
class InventoryImportBatch {
public:
    explicit InventoryImportBatch(grh::Graph& target);
    void record(const grh::Graph& source, std::span<const std::string> instancePath);
    void flush();
private:
    grh::Graph& target_;
    bool pendingImport_ = false;
    std::vector<std::string> inventory_;
    std::vector<std::string> eliminated_;
};
// Copy the source inventory/tombstones with the same qualification as the ops.
void importInventory(const grh::Graph& source, grh::Graph& target,
                     std::span<const std::string> instancePath);
// Diagnostic census. Optimizations may legitimately lose optional associations.
void recordRetention(grh::Graph& graph);

} // namespace wolvrix::lib::ingest::node

#endif // WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP
