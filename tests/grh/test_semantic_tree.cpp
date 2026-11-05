#include "core/grh.hpp"

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

void requirePath(const SemanticTree& tree, SemanticId id,
                 const std::vector<std::string>& expected) {
    require(tree.path(id) == expected, "unexpected semantic path");
}

} // namespace

int main() {
    try {
        static_assert(std::is_trivially_copyable_v<SemanticNode>);
        static_assert(sizeof(SemanticId) == sizeof(uint32_t));
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
        const auto core0 = top.appendSemanticTree(child, topTree.root(), "core0");
        const auto core1 = top.appendSemanticTree(child, topTree.root(), "core1");
        const auto core0Leaf = core0 + leaf - 1;
        const auto core1Leaf = core1 + leaf - 1;
        require(core0 != core1 && core0Leaf != core1Leaf,
                "copied trees share a semantic leaf");
        require(topTree.size() == 1 + childTree.size() * 2,
                "appendTree did not deep-copy every source node");
        requirePath(topTree, core0Leaf, {"top", "core0", "n", "bits", "data"});
        requirePath(topTree, core1Leaf, {"top", "core1", "n", "bits", "data"});
        bool emptyRootRejected = false;
        try {
            (void)top.appendSemanticTree(child, topTree.root(), "");
        } catch (const std::invalid_argument&) {
            emptyRootRejected = true;
        }
        require(emptyRootRejected, "empty instance root name was accepted");

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

        auto& outer = design.createGraph("outer");
        auto& outerTree = outer.getOrCreateSemanticTree("unit", "circuit", "outer");
        const auto copiedTop = outer.appendSemanticTree(top, outerTree.root(), "nested");
        const auto outerLeaf = copiedTop + core0Leaf - 1;
        requirePath(outerTree, outerLeaf, {"outer", "nested", "core0", "n", "bits", "data"});
        require(outerTree.size() == 1 + topTree.size(), "second copy did not copy the complete tree");

        require(design.deleteGraph("child"), "failed to delete source graph");
        requirePath(topTree, top.opSemanticIds(topOp)[1], {"top", "core0", "n", "bits", "data"});
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
        require(clone.semanticTree() != top.semanticTree(), "clone shares its SemanticTree");
        require(clone.opSemanticIds(clone.operations().front()).size() == 2 &&
                clone.valueSemanticIds(clone.values().front())[0] == core0Leaf,
                "clone lost semantic associations");
        requirePath(*clone.semanticTree(), clone.valueSemanticIds(clone.values().front())[0],
                    {"top", "core0", "n", "bits", "data"});

        auto& shared = design.createGraph("shared");
        auto& sharedTree = shared.getOrCreateSemanticTree("unit", "circuit", "shared");
        std::vector<SemanticId> wide;
        for (int index = 0; index < 300; ++index) {
            wide.push_back(sharedTree.addNode(sharedTree.root(), SemanticNodeKind::Declaration,
                                              "n" + std::to_string(index)));
        }
        const auto sharedFirst = shared.createTemporaryOperation(OperationKind::kNot);
        const auto sharedSecond = shared.createTemporaryOperation(OperationKind::kNot);
        shared.setOpSemanticIds(sharedFirst, wide);
        shared.setOpSemanticIds(sharedSecond, wide);
        require(shared.payloadStats().semanticIds == wide.size() &&
                shared.payloadStats().semanticLists == 1,
                "identical multi-ID lists were not interned");
        for (const auto op : shared.operations()) {
            const auto ids = shared.opSemanticIds(op);
            require(std::equal(ids.begin(), ids.end(), wide.begin(), wide.end()),
                    "interning changed an operation SemanticId list");
        }
        shared.compact();
        require(shared.payloadStats().semanticIds == wide.size() &&
                shared.payloadStats().semanticLists == 1,
                "compact discarded shared SemanticId storage");
        auto& sharedClone = design.cloneGraph("shared", "shared_clone");
        require(sharedClone.payloadStats().semanticIds == wide.size() &&
                sharedClone.payloadStats().semanticLists == 1,
                "clone discarded shared SemanticId storage");
        for (const auto op : sharedClone.operations()) {
            const auto ids = sharedClone.opSemanticIds(op);
            require(std::equal(ids.begin(), ids.end(), wide.begin(), wide.end()),
                    "clone changed a shared SemanticId list");
        }

        auto& aliasGraph = design.createGraph("alias");
        auto& aliasTree = aliasGraph.getOrCreateSemanticTree("unit", "circuit", "alias");
        const SemanticId aliasId = aliasTree.addNode(aliasTree.root(), SemanticNodeKind::Declaration, "n");
        const auto aliasOp = aliasGraph.createTemporaryOperation(OperationKind::kNot);
        aliasGraph.setOpSemanticIds(aliasOp, std::span<const SemanticId>(&aliasId, 1));
        for (int iteration = 0; iteration < 1024; ++iteration) {
            aliasGraph.setOpSemanticIds(aliasOp, aliasGraph.opSemanticIds(aliasOp));
            require(aliasGraph.opSemanticIds(aliasOp).size() == 1 &&
                    aliasGraph.opSemanticIds(aliasOp).front() == aliasId,
                    "aliased singleton SemanticId was corrupted during growth");
        }

        auto& bounded = design.createGraph("bounded");
        auto& boundedTree = bounded.getOrCreateSemanticTree("unit", "circuit", "bounded");
        std::vector<SemanticId> boundedIds;
        for (int index = 0; index < 5064; ++index) {
            boundedIds.push_back(boundedTree.addNode(boundedTree.root(), SemanticNodeKind::Declaration,
                                                     "n" + std::to_string(index)));
        }
        for (std::size_t index = 0; index + 64 < boundedIds.size(); ++index) {
            const std::vector<SemanticId> ids(boundedIds.begin() + index, boundedIds.begin() + index + 64);
            const auto firstOp = bounded.createTemporaryOperation(OperationKind::kNot);
            const auto secondOp = bounded.createTemporaryOperation(OperationKind::kNot);
            bounded.setOpSemanticIds(firstOp, ids);
            bounded.setOpSemanticIds(secondOp, ids);
        }
        require(bounded.payloadStats().semanticLists <= 4096,
                "SemanticId list cache exceeded its bound");
        require(bounded.payloadStats().semanticIds == 320000,
                "hot SemanticId list was not shared near cache eviction");
        const auto boundedOps = bounded.operations();
        for (std::size_t index = 0; index < 5000; ++index) {
            for (const auto op : {boundedOps[index * 2], boundedOps[index * 2 + 1]}) {
                const auto ids = bounded.opSemanticIds(op);
                require(std::equal(ids.begin(), ids.end(), boundedIds.begin() + index),
                        "cache eviction corrupted an older SemanticId list");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[grh-semantic-tree] " << error.what() << '\n';
        return 1;
    }
}
