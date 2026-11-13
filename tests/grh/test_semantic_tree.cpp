#include "core/grh.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace wolvrix::lib::grh;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void valid(const Graph& graph) {
    const auto errors = graph.validateSemanticTree();
    if (!errors.empty()) throw std::runtime_error(errors.front());
}
void attach(Graph& graph, OperationId op, ValueId value, SemanticId node) {
    graph.setOpSemanticIds(op, std::span<const SemanticId>(&node, 1));
    graph.setValueSemanticIds(value, std::span<const SemanticId>(&node, 1));
}

void testPathsAndFlatten() {
    SemanticTree tree("unit", "circuit", "Top");
    const auto instance = tree.addPath(tree.root(), "u.core");
    const auto bundle = tree.addPath(instance, "bundle");
    require(tree.addPath(instance, "bundle") == bundle, "same Path was duplicated");
    const auto node = tree.addNode(bundle, "data[0]\\x");
    const auto empty = tree.addNode(tree.root(), "input");
    const auto orphan = tree.addPath(tree.root(), "orphan");
    tree.addPath(orphan, "unused");
    require(tree.size() == 2 && tree.pathCount() == 5, "Path and Node counts are mixed");
    const std::vector<std::string> tokens{"Top", "u.core", "bundle", "data[0]\\x"};
    require(tree.path(node) == tokens, "leaf path differs");
    tree.pruneEmptyPaths();
    require(tree.size() == 2 && tree.contains(empty) && tree.pathCount() == 3,
            "path pruning removed an empty-op Node");
    const auto qualified = tree.qualifiedName(node);
    tree.flatten();
    require(tree.isFlat() && tree.namesFlattened() && tree.pathCount() == 1,
            "flatten retained internal Paths");
    require(tree.node(node).parent == tree.root() && tree.name(node) == qualified,
            "flatten changed Node identity or qualifier");
    require(tree.path(node) == tokens, "flatten name did not preserve reversible path tokens");
    tree.flatten();
    require(tree.name(node) == qualified, "flatten escaped an already flattened name twice");
    const std::vector<std::string> prefix{"outer.unit"};
    require(tree.qualifiedName(node, prefix) == "outer\\.unit." + qualified,
            "qualifiedName re-escaped an existing flattened name");
}

void testSparseImportAndAppend() {
    SemanticTree source("unit", "circuit", "Child");
    source.importPath(20, source.root(), "bundle");
    source.importNode(80, 20, "payload");
    source.importNode(3, source.root(), "input");
    require(source.nodeIds() == std::vector<SemanticId>({3, 80}), "sparse Node enumeration differs");
    SemanticTree target("unit", "circuit", "Top");
    const auto map = target.appendTree(source, target.root(), "u");
    require(map.nodeIds.size() == 81 && map.nodeIds[80] && map.nodeIds[3],
            "append lacks explicit sparse Node mapping");
    require(target.path(map.nodeIds[80]) == std::vector<std::string>({"Top", "u", "bundle", "payload"}),
            "appended Node path differs");
    require(target.operations(map.nodeIds[80]).empty(), "append copied graph-local member references");
    source.flatten();
    const auto flatMap = target.appendTree(source, target.root(), "v");
    target.flatten();
    require(target.name(flatMap.nodeIds[80]) == "v.bundle.payload", "copy of flattened source escaped qualifiers twice");
}

void testMembershipAndLifecycle() {
    Design design;
    auto& graph = design.createGraph("Top");
    auto& tree = graph.getOrCreateSemanticTree("unit", "circuit", "Top");
    const auto lhs = tree.addNode(tree.root(), "lhs");
    const auto rhs = tree.addNode(tree.root(), "rhs");
    const auto input = tree.addNode(tree.root(), "input");
    const auto deadValue = graph.createTemporaryValue(8, false);
    const auto deadOp = graph.createTemporaryOperation(OperationKind::kNot);
    const auto value = graph.createTemporaryValue(8, false);
    const auto op = graph.createTemporaryOperation(OperationKind::kNot);
    graph.addResult(op, value);
    const std::vector<SemanticId> owners{lhs, rhs, lhs};
    graph.setOpSemanticIds(op, owners);
    graph.setValueSemanticIds(value, owners);
    require(graph.opSemanticIds(op).size() == 2 && graph.valueSemanticIds(value).size() == 2,
            "Node membership is not a set");
    require(tree.operations(lhs).size() == 1 && tree.operations(rhs).size() == 1,
            "one operation cannot belong to multiple Nodes");
    require(graph.semanticNodeOperations(lhs) == std::vector<OperationId>{op} &&
            graph.semanticNodeAnchors(lhs) == std::vector<ValueId>{value}, "typed Node members differ");
    const auto port = graph.createTemporaryValue(8, false);
    graph.bindInputPort("input", port);
    graph.setValueSemanticIds(port, std::span<const SemanticId>(&input, 1));
    require(tree.operations(input).empty() && tree.anchors(input).size() == 1, "input Node needs artificial ops");
    graph.setOpSemanticIds(op, std::span<const SemanticId>(&rhs, 1));
    require(tree.operations(lhs).empty() && tree.operations(rhs).size() == 1, "replace membership left old owners");
    graph.addOpSemanticIds(op, owners);
    graph.addValueSemanticIds(value, graph.valueSemanticIds(value));
    valid(graph);
    auto& clone = design.cloneGraph("Top", "Clone");
    valid(clone);
    require(clone.semanticTree()->operations(lhs).size() == 1, "clone doubled memberships");
    graph.setOpSemanticIds(op, {});
    require(clone.semanticTree()->operations(lhs).size() == 1, "source edit mutated clone Node members");
    attach(graph, op, value, rhs);
    require(graph.eraseOp(deadOp) && graph.eraseValue(deadValue), "failed to create compaction holes");
    graph.compact();
    valid(graph);
    require(tree.contains(lhs) && tree.contains(rhs) && tree.contains(input), "compact renumbered Node IDs");
    const auto liveOp = graph.semanticNodeOperations(rhs).front();
    const auto liveValue = graph.semanticNodeAnchors(rhs).front();
    require(liveOp.index == 1 && liveValue.index == 1, "compact left stale member references");
    require(graph.eraseOp(liveOp), "failed to erase Node operation");
    require(tree.operations(rhs).empty(), "eraseOp left stale forward membership");
    require(graph.eraseValue(liveValue), "failed to erase Node anchor");
    require(tree.anchors(rhs).empty(), "eraseValue left stale forward membership");
    graph.pruneSemanticTree();
    require(tree.size() == 3 && tree.contains(lhs), "prune deleted a named empty Node");
    valid(graph);
}

void testRemapAndLargeSharedSet() {
    Design design;
    auto& source = design.createGraph("Source");
    auto& sourceTree = source.getOrCreateSemanticTree("unit", "circuit", "Source");
    std::vector<SemanticId> nodes;
    for (int i = 0; i < 4096; ++i) nodes.push_back(sourceTree.addNode(sourceTree.root(), "s" + std::to_string(i)));
    const auto value = source.createTemporaryValue(1, false);
    const auto op = source.createTemporaryOperation(OperationKind::kNot);
    source.addResult(op, value);
    source.setOpSemanticIds(op, nodes);
    source.setValueSemanticIds(value, nodes);
    valid(source);
    auto& target = design.createGraph("Target");
    auto& targetTree = target.getOrCreateSemanticTree("unit", "circuit", "Target");
    const auto map = target.appendSemanticTree(source, targetTree.root(), "u");
    const auto targetValue = target.createTemporaryValue(1, false);
    const auto targetOp = target.createTemporaryOperation(OperationKind::kNot);
    target.addResult(targetOp, targetValue);
    require(target.remapValueBlockSemantics(source, std::span<const ValueId>(&value, 1),
        std::span<const ValueId>(&targetValue, 1), map.nodeIds) == 1, "anchor remap count differs");
    require(target.remapOperationBlockSemantics(source, std::span<const OperationId>(&op, 1),
        std::span<const OperationId>(&targetOp, 1), map.nodeIds) == 1, "operation remap count differs");
    require(targetTree.operations(map.nodeIds[nodes.back()]).size() == 1, "block remap skipped forward members");
    valid(target);
    targetTree.flatten();
    valid(target);
}

void testValidationAndView() {
    Design design;
    auto& graph = design.createGraph("Top");
    auto& tree = graph.getOrCreateSemanticTree("unit", "circuit", "Top");
    const auto node = tree.addNode(tree.root(), "wire");
    const std::vector<int64_t> shape{2};
    const auto from = graph.createTemporaryValue(8, false, ValueType::Logic, shape);
    const auto to = graph.createTemporaryValue(8, false);
    graph.setValueSemanticIds(from, std::span<const SemanticId>(&node, 1));
    const auto view = graph.replaceAllUsesWithView(from, to);
    require(view != to && tree.operations(node).size() == 1, "new shape view has no Node operation membership");
    valid(graph);
    tree.addNode(tree.root(), "wire");
    require(!graph.validateSemanticTree().empty(), "duplicate named leaves passed validation");
}
} // namespace

int main() {
    try {
        static_assert(std::is_trivially_copyable_v<SemanticNode>);
        static_assert(sizeof(SemanticEntityRef) == 8);
        testPathsAndFlatten();
        testSparseImportAndAppend();
        testMembershipAndLifecycle();
        testRemapAndLargeSharedSet();
        testValidationAndView();
        std::cout << "semantic tree tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
