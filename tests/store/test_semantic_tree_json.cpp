#include "core/grh.hpp"
#include "core/load.hpp"
#include "core/store.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

namespace {

using namespace wolvrix::lib;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

grh::Design fixture() {
    grh::Design design;
    auto &top = design.createGraph("top");
    auto &tree = top.getOrCreateSemanticTree("unit", "circuit", "top");
    tree.importPath(7, tree.root(), "first");
    tree.importPath(11, 7, "bundle");
    tree.importPath(23, tree.root(), "second");
    tree.importNode(2, tree.root(), "input");
    tree.importNode(5, 11, "bits");
    tree.importNode(13, 23, "out");
    tree.importNode(29, tree.root(), "alias");
    tree.importNode(41, tree.root(), "zero.width");

    const auto input = top.createTemporaryValue(8, false);
    const auto discardedValue = top.createTemporaryValue(8, false);
    const auto intermediate = top.createTemporaryValue(8, false);
    const auto output = top.createTemporaryValue(8, false);
    const auto first = top.createTemporaryOperation(grh::OperationKind::kNot);
    const auto discardedOp = top.createTemporaryOperation(grh::OperationKind::kNot);
    const auto second = top.createTemporaryOperation(grh::OperationKind::kNot);
    top.addOperand(first, input);
    top.addResult(first, intermediate);
    top.addOperand(second, intermediate);
    top.addResult(second, output);
    const std::vector<grh::SemanticId> shared{5, 13};
    const std::vector<grh::SemanticId> secondOwner{13};
    const std::vector<grh::SemanticId> inputAliases{2, 29};
    top.setOpSemanticIds(first, shared);
    top.setOpSemanticIds(second, secondOwner);
    top.setValueSemanticIds(input, inputAliases);
    top.setValueSemanticIds(intermediate, shared);
    top.setValueSemanticIds(output, secondOwner);
    require(top.eraseOp(discardedOp) && top.eraseValue(discardedValue), "failed to make entity ID holes");
    require(top.validateSemanticTree().empty(), "fixture violates SemanticTree invariants");

    design.createGraph("plain").createTemporaryValue(1, false);
    design.markAsTop("top");
    return design;
}

std::string stored(const grh::Design &design, store::JsonPrintMode mode,
                   bool emitDefUsers = false, bool emitSourceLocations = false) {
    store::StoreOptions options;
    options.jsonMode = mode;
    options.emitDefUsers = emitDefUsers;
    options.emitSourceLocations = emitSourceLocations;
    const auto json = store::StoreJson().storeToString(design, options);
    require(json.has_value(), "storeToString failed");
    return *json;
}

void compareTrees(const grh::SemanticTree &expected, const grh::SemanticTree &actual) {
    require(expected.sourceUnit() == actual.sourceUnit() && expected.circuit() == actual.circuit() &&
            expected.root() == actual.root() && expected.namesFlattened() == actual.namesFlattened(),
            "tree identity or name encoding changed");
    require(expected.nodeIds() == actual.nodeIds() && expected.pathIds() == actual.pathIds(),
            "explicit Path/Node identities changed");
    for (const auto id : expected.pathIds()) {
        require(expected.pathEntry(id).parent == actual.pathEntry(id).parent &&
                expected.pathName(id) == actual.pathName(id), "Path changed");
    }
    for (const auto id : expected.nodeIds()) {
        require(expected.node(id).parent == actual.node(id).parent && expected.name(id) == actual.name(id),
                "Node name or parent changed");
    }
}

template<typename Range>
std::vector<grh::SemanticId> sortedIds(const Range &ids) {
    std::vector<grh::SemanticId> result(ids.begin(), ids.end());
    std::sort(result.begin(), result.end());
    return result;
}

void compareDesign(const grh::Design &expected, const grh::Design &actual) {
    const auto &before = *expected.findGraph("top");
    const auto &after = *actual.findGraph("top");
    require(before.semanticTree() && after.semanticTree(), "top tree was not restored");
    compareTrees(*before.semanticTree(), *after.semanticTree());
    require(before.values().size() == after.values().size() && before.operations().size() == after.operations().size(),
            "IR entity counts changed");
    for (std::size_t i = 0; i < before.values().size(); ++i)
        require(sortedIds(before.valueSemanticIds(before.values()[i])) == sortedIds(after.valueSemanticIds(after.values()[i])),
                "anchor boundary changed");
    for (std::size_t i = 0; i < before.operations().size(); ++i)
        require(sortedIds(before.opSemanticIds(before.operations()[i])) == sortedIds(after.opSemanticIds(after.operations()[i])),
                "actual operation membership changed");
    require(after.validateSemanticTree().empty(), "loaded memberships are not bidirectionally consistent");
    require(after.semanticNodeOperations(5).size() == 1 && after.semanticNodeOperations(13).size() == 2 &&
            after.semanticNodeOperations(2).empty() && after.semanticNodeAnchors(2).size() == 1,
            "shared implementation or empty input Node was lost");
    require(!actual.findGraph("plain")->semanticTree(), "plain graph acquired a SemanticTree");
}

std::string replaceOnce(std::string text, const std::string &from, const std::string &to) {
    const auto position = text.find(from);
    require(position != std::string::npos, "failed to build malformed JSON fixture: " + from);
    text.replace(position, from.size(), to);
    return text;
}

void expectReject(const std::string &json, const std::string &label) {
    try {
        auto ignored = load::LoadJson().load(json);
        (void)ignored;
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("invalid JSON was accepted: " + label);
}

void testSharedFanoutRoundtrip() {
    constexpr uint32_t nodeCount = 8192;
    grh::Design design;
    auto &graph = design.createGraph("shared");
    auto &tree = graph.getOrCreateSemanticTree("shared.fir", "Shared", "shared");
    std::vector<grh::SemanticId> nodes;
    nodes.reserve(nodeCount);
    for (uint32_t i = 0; i < nodeCount; ++i)
        nodes.push_back(tree.addNode(tree.root(), "consumer" + std::to_string(i)));
    const auto sharedValue = graph.createTemporaryValue(1, false);
    const auto sharedOp = graph.createTemporaryOperation(grh::OperationKind::kConstant);
    graph.addResult(sharedOp, sharedValue);
    graph.setAttr(sharedOp, "constValue", std::string("1'b0"));
    graph.setOpSemanticIds(sharedOp, nodes);
    graph.setValueSemanticIds(sharedValue, nodes);
    design.markAsTop("shared");
    const auto mode = store::JsonPrintMode::PrettyCompact;
    const auto json = stored(design, mode);
    auto restored = load::LoadJson().load(json);
    const auto &after = *restored.findGraph("shared");
    require(after.operations().size() == 1 && after.values().size() == 1 &&
            sortedIds(after.opSemanticIds(after.operations()[0])) == nodes &&
            sortedIds(after.valueSemanticIds(after.values()[0])) == nodes,
            "large shared implementation changed its unique IR objects or Node owners");
    for (const auto id : nodes)
        require(after.semanticNodeOperations(id).size() == 1 && after.semanticNodeAnchors(id).size() == 1,
                "large shared Node lost a forward membership");
    require(after.validateSemanticTree().empty() && stored(restored, mode) == json,
            "large shared roundtrip changed canonical membership facts");
    // A checkpoint with one shared object must not retain every growing
    // prefix of its owner list while loading the Node records.
    require(after.payloadStats().semanticIds <= uint64_t{nodeCount} * 2,
            "shared checkpoint load amplified membership storage quadratically");
}

void testSemanticPortMaps() {
    grh::Design design;
    auto &child = design.createGraph("child");
    auto &childTree = child.getOrCreateSemanticTree("ports.fir", "Ports", "child");
    const auto childData = childTree.addNode(childTree.root(), "data");
    const auto childZero = childTree.addNode(childTree.root(), "zero");
    child.setMetadata("firrtl.semanticPortNames", std::vector<std::string>{"data", "zero"});
    child.setMetadata("firrtl.semanticPortNodes", std::vector<int64_t>{childData, childZero});
    auto &top = design.createGraph("top");
    auto &tree = top.getOrCreateSemanticTree("ports.fir", "Ports", "top");
    const auto path = tree.addPath(tree.root(), "c");
    const auto data = tree.addNode(path, "data");
    const auto zero = tree.addNode(path, "zero");
    const auto instance = top.createTemporaryOperation(grh::OperationKind::kInstance);
    top.setOpSemanticIds(instance, std::vector<grh::SemanticId>{data, zero});
    top.setAttr(instance, "moduleName", std::string("child"));
    top.setAttr(instance, "instanceName", std::string("c"));
    top.setAttr(instance, "semanticPortNames", std::vector<std::string>{"data", "zero"});
    top.setAttr(instance, "semanticPortNodes", std::vector<int64_t>{data, zero});
    design.markAsTop("top");
    const auto mode = store::JsonPrintMode::PrettyCompact;
    const auto json = stored(design, mode);
    auto restored = load::LoadJson().load(json);
    require(stored(restored, mode) == json && restored.findGraph("child")->semanticTree()->contains(childZero),
            "formal/actual port maps or elided empty-op endpoints changed on reload");
    const auto blackbox = replaceOnce(json, "\"kind\": \"kInstance\"", "\"kind\": \"kBlackbox\"");
    auto blackboxRestored = load::LoadJson().load(blackbox);
    require(stored(blackboxRestored, mode) == blackbox, "blackbox semantic port boundaries changed on reload");

    const std::string formal = "\"firrtl.semanticPortNodes\": {\"t\": \"int[]\", \"vs\": [1, 2]}";
    const std::string actual = "\"semanticPortNodes\": {\"t\": \"int[]\", \"vs\": [1, 2]}";
    for (const auto &prefix : {formal, actual}) {
        for (const auto *invalid : {"[0, 2]", "[-4294967295, 2]", "[4294967297, 2]", "[99, 2]", "[1]"})
            expectReject(replaceOnce(json, prefix, replaceOnce(prefix, "[1, 2]", invalid)),
                         "invalid formal/actual semantic port Node reference");
        expectReject(replaceOnce(json, prefix, replaceOnce(prefix, "\"int[]\"", "\"string[]\"")),
                     "invalid semantic port Node array type");
    }
    for (const auto *key : {"firrtl.semanticPortNames", "semanticPortNames"}) {
        const std::string field = std::string("\"") + key + "\": {\"t\": \"string[]\", \"vs\": [\"data\", \"zero\"]}";
        expectReject(replaceOnce(json, field, replaceOnce(field, "[\"data\", \"zero\"]", "[\"data\", \"data\"]")),
                     "duplicate semantic port name");
        expectReject(replaceOnce(json, field, replaceOnce(field, key, std::string(key) + "Absent")),
                     "unpaired semantic port map");
    }
    expectReject(replaceOnce(json, "\"name\": \"zero\", \"ops\": [1]", "\"name\": \"zero\", \"ops\": []"),
                 "actual port Node is not an instance member");
    expectReject(replaceOnce(json, "\"kind\": \"kInstance\"", "\"kind\": \"kNot\""),
                 "actual semantic port map on non-instance");
    grh::Design noTree;
    auto &plain = noTree.createGraph("plain");
    noTree.markAsTop("plain");
    plain.setMetadata("firrtl.semanticPortNames", std::vector<std::string>{"data"});
    plain.setMetadata("firrtl.semanticPortNodes", std::vector<int64_t>{1});
    expectReject(stored(noTree, mode), "formal semantic port map without a tree");
}

} // namespace

int main() {
    try {
        testSharedFanoutRoundtrip();
        testSemanticPortMaps();
        const auto design = fixture();
        auto cloned = design.clone();
        compareDesign(design, cloned);
        cloned.findGraph("top")->compact();
        compareDesign(design, cloned);
        for (const auto mode : {store::JsonPrintMode::PrettyCompact, store::JsonPrintMode::Compact,
                                store::JsonPrintMode::Pretty}) {
            const auto json = stored(design, mode);
            require(json.find("\"semanticIds\"") == std::string::npos && json.find("\"semanticBindings\"") == std::string::npos,
                    "v5 wrote duplicate or legacy membership facts");
            auto restored = load::LoadJson().load(json);
            compareDesign(design, restored);
            require(stored(restored, mode) == json, "store/load/store changed v5 bytes");
            require(stored(cloned, mode) == json, "clone/compact changed local member references");

            const auto path = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                ("semantic-tree-v5-" + std::to_string(static_cast<int>(mode)) + ".json");
            {
                std::ofstream output(path);
                output << json;
                require(output.good(), "failed to write JSON fixture");
            }
            auto fromFile = load::LoadJson().loadFile(path);
            compareDesign(design, fromFile);
            require(stored(fromFile, mode) == json, "text/file loaders produced different v5 bytes");
            require(json.find("\"def\"") == std::string::npos && json.find("\"users\"") == std::string::npos &&
                    json.find("\"loc\"") == std::string::npos, "default JSON retained def/users/loc");
            auto full = load::LoadJson().load(stored(design, mode, true, true));
            compareDesign(design, full);
        }

        auto flattened = design.clone();
        flattened.findGraph("top")->semanticTree()->flatten();
        const auto flatJson = stored(flattened, store::JsonPrintMode::PrettyCompact);
        auto flatRestored = load::LoadJson().load(flatJson);
        compareDesign(flattened, flatRestored);
        flatRestored.findGraph("top")->semanticTree()->flatten();
        require(stored(flatRestored, store::JsonPrintMode::PrettyCompact) == flatJson,
                "reload/reflatten changed escaped names or stable IDs");

        const auto json = stored(design, store::JsonPrintMode::PrettyCompact);
        expectReject(replaceOnce(json, "\"formatVersion\": 5", "\"formatVersion\": 4"), "v4 requires reingest");
        expectReject(replaceOnce(json, "\"rootPath\": 1", "\"rootPath\": 7"), "invalid root");
        expectReject(replaceOnce(json, "\"namesFlattened\": false", "\"namesFlattened\": true"), "flattened encoding with internal Paths");
        expectReject(replaceOnce(json, "\"id\": 11, \"parent\": 7", "\"id\": 11, \"parent\": 99"), "unknown Path parent");
        expectReject(replaceOnce(json, "\"id\": 11, \"parent\": 7", "\"id\": 7, \"parent\": 1"), "duplicate Path ID");
        expectReject(replaceOnce(json, "\"id\": 7, \"parent\": 1", "\"id\": 7, \"parent\": 11"), "Path cycle");
        expectReject(replaceOnce(json, "\"id\": 5, \"parent\": 11", "\"id\": 5, \"parent\": 5"), "Node cannot parent a Node");
        expectReject(replaceOnce(json, "\"id\": 29, \"parent\": 1", "\"id\": 2, \"parent\": 1"), "duplicate Node ID");
        expectReject(replaceOnce(json, "\"name\": \"alias\"", "\"name\": \"input\""), "duplicate leaf name");
        expectReject(replaceOnce(json, "\"ops\": [1, 2]", "\"ops\": [1, 99]"), "unknown actual operation");
        expectReject(replaceOnce(json, "\"ops\": [1, 2]", "\"ops\": [1, 1]"), "duplicate operation member");
        expectReject(replaceOnce(json, "\"anchors\": [1]", "\"anchors\": [99]"), "unknown actual anchor");
        expectReject(replaceOnce(json, "\"anchors\": [1]", "\"anchors\": [1, 1]"), "duplicate anchor");
        expectReject(replaceOnce(json, "\"name\": \"input\"", "\"name\": \"input\", \"kind\": \"declaration\""), "legacy leaf classification");
        expectReject(replaceOnce(json, "\"metadata\": {}", "\"semanticBindings\": [], \"metadata\": {}"), "legacy endpoint bindings");
        expectReject(replaceOnce(json, "\"w\": 1", "\"semanticIds\": [2], \"w\": 1"), "legacy reverse membership");

        // Parent references and explicit identities, rather than array order, define the tree.
        const std::string parentRecord = "{\"id\": 7, \"parent\": 1, \"name\": \"first\"}";
        const std::string childRecord = "{\"id\": 11, \"parent\": 7, \"name\": \"bundle\"}";
        auto reordered = replaceOnce(json, parentRecord, "__parent_record__");
        reordered = replaceOnce(std::move(reordered), childRecord, parentRecord);
        reordered = replaceOnce(std::move(reordered), "__parent_record__", childRecord);
        auto reorderedDesign = load::LoadJson().load(reordered);
        compareDesign(design, reorderedDesign);
        require(stored(reorderedDesign, store::JsonPrintMode::PrettyCompact) == json,
                "unordered Path records changed canonical output");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[store-semantic-tree-json] " << error.what() << '\n';
        return 1;
    }
}
