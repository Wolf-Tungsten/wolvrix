#include "core/grh.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void requirePath(const SemanticTree& tree, SemanticId id,
                 const std::vector<std::string>& expected) {
    require(tree.path(id) == expected, "unexpected semantic path");
}

} // namespace

int main() {
    try {
        Design design;
        auto& child = design.createGraph("child");
        auto& childTree = child.getOrCreateSemanticTree("unit", "circuit", "child");
        const auto declaration = childTree.addNode(childTree.root(), SemanticNodeKind::Declaration, "n");
        const std::vector<std::string> fields{"bits", "data"};
        const auto leaf = childTree.addFieldPath(declaration, fields);
        const auto childValue = child.createTemporaryValue(8, false);
        const auto childOp = child.createTemporaryOperation(OperationKind::kNot);
        child.addResult(childOp, childValue);
        child.setValueSemanticIds(childValue, std::span<const SemanticId>(&leaf, 1));
        const std::vector<SemanticId> childOpSources{declaration, leaf};
        child.setOpSemanticIds(childOp, childOpSources);

        auto& top = design.createGraph("top");
        auto& topTree = top.getOrCreateSemanticTree("unit", "circuit", "top");
        const auto core0 = top.appendSemanticInstance(child, topTree.root(), "core0");
        const auto core1 = top.appendSemanticInstance(child, topTree.root(), "core1");
        const auto core0Leaf = qualifiedSemantic(core0, leaf);
        const auto core1Leaf = qualifiedSemantic(core1, leaf);
        require(core0 != core1 && core0Leaf != core1Leaf,
                "instances share a fully-qualified semantic leaf");
        requirePath(topTree, core0Leaf, {"top", "core0", "child", "n", "bits", "data"});
        requirePath(topTree, core1Leaf, {"top", "core1", "child", "n", "bits", "data"});

        const auto topValue = top.createTemporaryValue(8, false);
        const auto topOp = top.createTemporaryOperation(OperationKind::kNot);
        top.addResult(topOp, topValue);
        require(top.remapOperationBlockSemantics(child, std::span<const OperationId>(&childOp, 1),
                                                 std::span<const OperationId>(&topOp, 1), core0) == 1 &&
                top.remapValueBlockSemantics(child, std::span<const ValueId>(&childValue, 1),
                                             std::span<const ValueId>(&topValue, 1), core0) == 1,
                "block semantic remap failed");
        const auto alias = top.createTemporaryValue(8, false);
        top.setValueSemanticIds(alias, std::span<const SemanticId>(&core1Leaf, 1));
        top.replaceAllUses(topValue, alias);
        require(top.valueSemanticIds(alias).size() == 2,
                "replaceAllUses did not merge direct source associations");
        const auto replacement = top.createTemporaryValue(8, false);
        top.replaceResult(topOp, 0, replacement);
        require(top.valueSemanticIds(replacement).size() == 1 &&
                top.valueSemanticIds(replacement)[0] == core0Leaf,
                "replaceResult lost the previous result source");

        require(design.deleteGraph("child"), "failed to delete source graph");
        requirePath(topTree, top.opSemanticIds(topOp)[1], {"top", "core0", "child", "n", "bits", "data"});
        top.compact();
        const auto compactedOp = top.operations().front();
        const auto compactedValue = top.values().front();
        require(top.opSemanticIds(compactedOp).size() == 2 &&
                top.opSemanticIds(compactedOp)[1] == core0Leaf &&
                top.valueSemanticIds(compactedValue)[0] == core0Leaf,
                "compact changed semantic associations");

        auto& clone = design.cloneGraph("top", "clone");
        require(clone.semanticTree() && clone.semanticTree()->size() == topTree.size(),
                "clone lost SemanticTree nodes");
        require(clone.opSemanticIds(clone.operations().front()).size() == 2 &&
                clone.valueSemanticIds(clone.values().front())[0] == core0Leaf,
                "clone lost semantic associations");
        requirePath(*clone.semanticTree(), clone.valueSemanticIds(clone.values().front())[0],
                    {"top", "core0", "child", "n", "bits", "data"});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[grh-semantic-tree] " << error.what() << '\n';
        return 1;
    }
}
