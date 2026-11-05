#include "core/grh.hpp"
#include "core/load.hpp"
#include "core/store.hpp"

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

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

grh::Design fixture() {
    grh::Design design;
    auto& child = design.createGraph("child");
    auto& childTree = child.getOrCreateSemanticTree("unit", "circuit", "child");
    const auto declaration = childTree.addNode(childTree.root(), grh::SemanticNodeKind::Declaration, "n");
    const auto field = childTree.addNode(declaration, grh::SemanticNodeKind::Field, "bits");
    const auto element = childTree.addNode(field, grh::SemanticNodeKind::Element, "", 3);
    childTree.addNode(element, grh::SemanticNodeKind::Expression, "add");

    auto& top = design.createGraph("top");
    auto& topTree = top.getOrCreateSemanticTree("unit", "circuit", "top");
    const auto first = top.appendSemanticTree(child, topTree.root(), "first");
    const auto second = top.appendSemanticTree(child, topTree.root(), "second");
    const auto firstLeaf = first + element - 1;
    const auto secondLeaf = second + element - 1;
    require(topTree.path(firstLeaf) == std::vector<std::string>{"top", "first", "n", "bits", "[3]"} &&
            topTree.path(secondLeaf) == std::vector<std::string>{"top", "second", "n", "bits", "[3]"},
            "instance root replacement corrupted semantic paths");
    const auto value = top.createTemporaryValue(8, false);
    std::vector<grh::SemanticId> valueSources;
    for (int index = 0; index < 64; ++index)
        valueSources.push_back(index % 2 ? secondLeaf : firstLeaf);
    top.setValueSemanticIds(value, valueSources);
    const auto op = top.createTemporaryOperation(grh::OperationKind::kNot);
    top.addResult(op, value);
    const auto firstDeclaration = first + declaration - 1;
    const auto secondDeclaration = second + declaration - 1;
    std::vector<grh::SemanticId> opSources;
    for (int index = 0; index < 64; ++index)
        opSources.push_back(index % 2 ? secondDeclaration : firstDeclaration);
    top.setOpSemanticIds(op, opSources);
    const auto sharedValue = top.createTemporaryValue(8, false);
    const auto sharedOp = top.createTemporaryOperation(grh::OperationKind::kNot);
    top.addResult(sharedOp, sharedValue);
    top.setOpSemanticIds(sharedOp, opSources);
    require(top.payloadStats().semanticIds == 128 && top.payloadStats().semanticLists == 2,
            "multi-ID lists were not shared before JSON storage");

    auto& plain = design.createGraph("plain");
    plain.createTemporaryValue(1, false);
    design.markAsTop("top");
    require(design.deleteGraph("child"), "failed to remove source graph");
    return design;
}

std::string stored(const grh::Design& design, store::JsonPrintMode mode,
                   bool emitDefUsers = false, bool emitSourceLocations = false) {
    store::StoreOptions options;
    options.jsonMode = mode;
    options.emitDefUsers = emitDefUsers;
    options.emitSourceLocations = emitSourceLocations;
    const auto json = store::StoreJson().storeToString(design, options);
    require(json.has_value(), "storeToString failed");
    return *json;
}

void compareTrees(const grh::SemanticTree& expected, const grh::SemanticTree& actual) {
    require(expected.sourceUnit() == actual.sourceUnit() && expected.circuit() == actual.circuit(),
            "tree identity changed");
    require(expected.size() == actual.size() && expected.nameBytes() == actual.nameBytes(),
            "tree storage size changed");
    for (grh::SemanticId id = 1; id <= expected.size(); ++id) {
        const auto& a = expected.node(id);
        const auto& b = actual.node(id);
        require(a.parent == b.parent && a.kind == b.kind && a.index == b.index,
                "tree node changed at ID " + std::to_string(id));
        require(expected.name(id) == actual.name(id), "tree node name changed");
    }
}

void compareDesign(const grh::Design& expected, const grh::Design& actual) {
    const auto& before = *expected.findGraph("top");
    const auto& after = *actual.findGraph("top");
    require(before.semanticTree() && after.semanticTree(), "top tree was not restored");
    compareTrees(*before.semanticTree(), *after.semanticTree());
    require(after.values().size() == before.values().size() && after.operations().size() == before.operations().size(),
            "top entity counts changed");
    for (std::size_t i = 0; i < before.values().size(); ++i) {
        const auto a = before.valueSemanticIds(before.values()[i]);
        const auto b = after.valueSemanticIds(after.values()[i]);
        require(std::equal(a.begin(), a.end(), b.begin(), b.end()), "value SemanticId list changed");
    }
    for (std::size_t i = 0; i < before.operations().size(); ++i) {
        const auto a = before.opSemanticIds(before.operations()[i]);
        const auto b = after.opSemanticIds(after.operations()[i]);
        require(std::equal(a.begin(), a.end(), b.begin(), b.end()), "operation SemanticId list changed");
    }
    require(after.payloadStats().semanticIds == before.payloadStats().semanticIds &&
            after.payloadStats().semanticLists == before.payloadStats().semanticLists,
            "JSON load changed shared SemanticId storage");
    require(!actual.findGraph("plain")->semanticTree(), "plain graph acquired a SemanticTree");
}

std::string replaceOnce(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    require(position != std::string::npos, "failed to build malformed JSON fixture");
    text.replace(position, from.size(), to);
    return text;
}

void expectReject(const std::string& json, const std::string& label) {
    try {
        auto ignored = load::LoadJson().load(json);
        (void)ignored;
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid JSON was accepted: " + label);
}

} // namespace

int main() {
    try {
        const auto design = fixture();
        for (const auto mode : {store::JsonPrintMode::PrettyCompact, store::JsonPrintMode::Compact,
                                store::JsonPrintMode::Pretty}) {
            const auto json = stored(design, mode);
            auto restored = load::LoadJson().load(json);
            compareDesign(design, restored);
            require(stored(restored, mode) == json, "store/load/store changed v4 bytes");

            const auto directory = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR);
            const auto path = directory / ("semantic-tree-v4-" + std::to_string(static_cast<int>(mode)) + ".json");
            {
                std::ofstream output(path);
                output << json;
                require(output.good(), "failed to write JSON fixture");
            }
            auto fromFile = load::LoadJson().loadFile(path);
            compareDesign(design, fromFile);
            require(stored(fromFile, mode) == json, "text/file loaders produced different v4 bytes");

            const auto slim = stored(design, mode, false, false);
            require(slim.find("\"def\"") == std::string::npos && slim.find("\"users\"") == std::string::npos &&
                    slim.find("\"loc\"") == std::string::npos, "slim JSON retained def/users/loc");
            auto slimRestored = load::LoadJson().load(slim);
            compareDesign(design, slimRestored);
            require(stored(slimRestored, mode, false, false) == slim,
                    "slim store/load/store changed v4 bytes");
            require(json == slim, "default JSON did not use slim def/users/loc settings");
        }

        const auto json = stored(design, store::JsonPrintMode::PrettyCompact);
        expectReject(replaceOnce(json, "\"formatVersion\": 4", "\"formatVersion\": 3"), "v3 compatibility");
        expectReject(replaceOnce(json, "\"parent\": 2", "\"parent\": 99"), "invalid parent");
        expectReject(replaceOnce(json, "\"kind\": \"declaration\"", "\"kind\": \"instance\""), "instance node");
        expectReject(replaceOnce(json, "\"index\": 3", "\"index\": -1"), "negative element index");
        expectReject(replaceOnce(json, "\"semanticIds\": [5, 10, 5, 10, 5, 10, 5, 10",
                                 "\"semanticIds\": [999999"), "unknown SemanticId");
        expectReject(R"json({
          "formatVersion":4,
          "graphs":[{
            "symbol":"plain", "dialects":{"grh":2}, "metadata":{}, "declaredSymbols":[],
            "vals":[{"id":1,"w":1,"sgn":false,"type":"logic","domain":"4state",
                     "in":false,"out":false,"inout":false,"semanticIds":[1],"users":[]}],
            "ports":{"in":[],"out":[],"inout":[]},"ops":[]
          }],
          "aliases":{},"declaredSymbols":[],"tops":["plain"]
        })json", "SemanticId without a tree");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[store-semantic-tree-json] " << error.what() << '\n';
        return 1;
    }
}
