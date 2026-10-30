#ifndef WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP
#define WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP

#include "core/grh.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::ingest::node {

inline constexpr std::string_view kSchema = "firrtl.nodeSchema";
inline constexpr std::string_view kMembers = "firrtl.node.members";
inline constexpr std::string_view kRoots = "firrtl.node.roots";
inline constexpr std::string_view kRootResults = "firrtl.node.rootResults";
inline constexpr std::string_view kOrigins = "firrtl.node.origins";

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

Attributes read(const grh::Graph& graph, grh::OperationId op);
Attributes normalize(Attributes attributes);
void write(grh::Graph& graph, grh::OperationId op, Attributes attributes);
void copyMembers(grh::Graph& graph, grh::OperationId from, grh::OperationId to);
void qualify(grh::Graph& graph, grh::OperationId op, std::span<const std::string> instancePath);
Attributes remapResults(Attributes attributes, std::span<const int64_t> oldToNew);

enum class MergeReason { SameComputation, WholeNode, ExecutionLeaf };
Attributes unite(Attributes target, const Attributes& source, MergeReason reason);
// The caller must prove whole-node equivalence; this function only moves metadata.
void mergeEquivalentOperations(grh::Graph& graph, grh::OperationId target,
                              grh::OperationId source, std::span<const int64_t> resultMap);
// Functional equivalence is the caller's proof; this checks the sharing scope.
void mergeCseOperations(grh::Graph& graph, grh::OperationId target, grh::OperationId source);

// Functional hashes exclude node attrs. Add this key to the existing execution
// owner scope; sameScope checks both. Keys are transient and never serialized.
std::string cseScope(const grh::Graph& graph, grh::OperationId op);
bool sameScope(const grh::Graph& graph, grh::OperationId left, grh::OperationId right);
bool mayInline(const grh::Graph& graph, grh::OperationId consumer, grh::OperationId producer);
bool rootCarrier(const grh::Graph& graph, grh::OperationId op);

// Returns a transparent carrier when a root would otherwise move onto an unrelated
// producer/input. Call before replacing uses and erasing the original operation.
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

Inspection inspect(const grh::Graph& graph);
void requireValid(const grh::Graph& graph);

} // namespace wolvrix::lib::ingest::node

#endif // WOLVRIX_INGEST_NODE_ATTRIBUTES_HPP
