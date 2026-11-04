#include "node_test_fixture.hpp"
#include "transform/redundant_elim.hpp"

#include <array>
#include <map>
#include <set>

using namespace node_test;

namespace {
// Preserve the former inspection algorithm as an independent validation oracle.
node::Inspection originalInspection(const grh::Graph& graph) {
    node::Inspection result;
    std::map<std::string, node::NodeRecord> records;
    const auto schema = graph.metadata(node::kSchema);
    if (schema && (!std::holds_alternative<int64_t>(*schema) || std::get<int64_t>(*schema) != node::kSchemaRevision))
        result.errors.push_back("unsupported firrtl.nodeSchema; re-ingest for soft-node-v2");
    for (const auto op : graph.operations()) {
        if (!node::annotated(graph, op)) continue;
        try {
            if (!schema) throw std::invalid_argument("node attributes require firrtl.nodeSchema=2");
            bool hasRoots = false, hasIndices = false;
            for (const auto& entry : graph.opAttrRange(op)) if (node::isAttribute(entry.key())) {
                if (entry.key() != node::kMembers && entry.key() != node::kRoots && entry.key() != node::kRootResults && entry.key() != node::kOrigins)
                    throw std::invalid_argument("unknown node attribute: " + std::string(entry.key()));
                hasRoots |= entry.key() == node::kRoots; hasIndices |= entry.key() == node::kRootResults;
            }
            if (hasRoots != hasIndices) throw std::invalid_argument("node roots/rootResults must be paired");
            const auto attributes = node::read(graph, op);
            if (node::normalize(attributes) != attributes) throw std::invalid_argument("node attributes are not sorted/unique");
            if (attributes.members.empty() && graph.opKind(op) != OperationKind::kConstant) result.isolatedFragments.push_back(op);
            for (const auto& member : attributes.members) {
                auto& record = records[member]; record.key = member; record.members.push_back(op);
            }
            for (const auto& root : attributes.roots) {
                const auto outputs = graph.opResults(op);
                if (root.result < 0 || static_cast<uint64_t>(root.result) >= outputs.size())
                    throw std::invalid_argument("node root result index out of range");
                auto& record = records[root.key]; record.key = root.key;
                record.roots.push_back({op, root.result, outputs[root.result]});
            }
        } catch (const std::exception& error) {
            result.errors.push_back(std::string(graph.getOperation(op).symbolText()) + ": " + error.what());
        }
    }
    for (auto& [key, record] : records) result.nodes.push_back(std::move(record));
    return result;
}

void sameValidation(grh::Graph& graph) {
    const auto expected = originalInspection(graph);
    require(node::scanAttributeErrors(graph) == expected.errors,
        "validation-only scan changed the complete diagnostic list or order");
    require(node::inspect(graph).report(graph) == expected.report(graph), "borrowed validation changed inspection or diagnostics");
    std::string message;
    try { node::requireValid(graph); }
    catch (const std::invalid_argument& error) { message = error.what(); }
    require(message == (expected.errors.empty() ? "" : "FIRRTL node attributes: " + expected.errors.front()),
        "validation-only scan changed the first error or prefix");
    node::ValidationCache cache;
    require(node::scanAttributeErrors(graph, cache) == expected.errors && cache.misses == 1 && cache.hits == 0,
        "revision validation cache did not populate deterministically");
    require(node::scanAttributeErrors(graph, cache) == expected.errors && cache.hits == 1,
        "revision validation cache did not reuse a stable graph result");
    graph.setMetadata("firrtl.validationProbe", true);
    require(node::scanAttributeErrors(graph, cache) == expected.errors && cache.misses == 2,
        "revision validation cache reused a result after graph mutation");
    node::establishPolicy(graph);
    std::vector<std::string> keys;
    for (const auto& record : expected.nodes) keys.push_back(record.key);
    require(graph.metadata(node::kInventory) == grh::AttributeValue(keys), "establishPolicy changed partial-record behavior");
    if (!expected.errors.empty()) {
        message.clear();
        try { node::recordRetention(graph); }
        catch (const std::invalid_argument& error) { message = error.what(); }
        require(message == expected.errors.front(), "retention changed validation failure priority");
    }
}

void copyQualifiedAcrossGraphs() {
    Fixture source;
    const auto sourceInput = source.input("a");
    const auto sourceOp = source.graph.createOperation(OperationKind::kNot);
    const auto sourceValue0 = source.graph.createValue(Fixture::type());
    const auto sourceValue1 = source.graph.createValue(Fixture::type());
    source.graph.addOperand(sourceOp, sourceInput);
    source.graph.addResult(sourceOp, sourceValue0);
    source.graph.addResult(sourceOp, sourceValue1);
    const auto first = key("qualified-first", {"field", "x/y"});
    const auto second = key("qualified-second", {"field", "z"});
    node::write(source.graph, sourceOp,
        {{second, first, second}, {{second, 1}, {first, 0}}, {first, second}});
    source.graph.setAttr(sourceOp, node::kMembers, std::vector<std::string>{second, first, second});
    source.graph.setAttr(sourceOp, node::kOrigins, std::vector<std::string>{second, first, first});
    source.graph.setAttr(sourceOp, node::kRoots, std::vector<std::string>{second, first, second});
    source.graph.setAttr(sourceOp, node::kRootResults, std::vector<int64_t>{1, 0, 1});

    Fixture expected;
    const auto expectedInput = expected.input("a");
    const auto expectedOp = expected.graph.createOperation(OperationKind::kNot);
    const auto expectedValue0 = expected.graph.createValue(Fixture::type());
    const auto expectedValue1 = expected.graph.createValue(Fixture::type());
    expected.graph.addOperand(expectedOp, expectedInput);
    expected.graph.addResult(expectedOp, expectedValue0);
    expected.graph.addResult(expectedOp, expectedValue1);
    node::write(expected.graph, expectedOp, node::read(source.graph, sourceOp));

    Fixture actual;
    const auto actualInput = actual.input("a");
    const auto actualOp = actual.graph.createOperation(OperationKind::kNot);
    const auto actualValue0 = actual.graph.createValue(Fixture::type());
    const auto actualValue1 = actual.graph.createValue(Fixture::type());
    actual.graph.addOperand(actualOp, actualInput);
    actual.graph.addResult(actualOp, actualValue0);
    actual.graph.addResult(actualOp, actualValue1);

    const auto sourceAttributes = node::read(source.graph, sourceOp);
    source.graph.freeze();
    const std::array<std::vector<std::string>, 4> paths{{{"left"}, {"right"},
                                                       {"outer", "inner/a"}, {"outer/inner", "a"}}};
    std::set<std::string> identities;
    for (const auto& path : paths) {
        auto qualified = sourceAttributes;
        const auto prefix = [&](std::string& encoded) {
            auto identity = node::parseKey(encoded);
            identity.instancePath.insert(identity.instancePath.begin(), path.begin(), path.end());
            encoded = node::key(identity);
        };
        for (auto& member : qualified.members) prefix(member);
        for (auto& origin : qualified.origins) prefix(origin);
        for (auto& root : qualified.roots) prefix(root.key);
        node::write(expected.graph, expectedOp, std::move(qualified));
        node::QualificationContext context(path);
        node::copyQualified(source.graph, sourceOp, actual.graph, actualOp, context);
        const auto attributes = node::read(actual.graph, actualOp);
        require(attributes == node::read(expected.graph, expectedOp),
            "copyQualified changed qualified members, origins, normalization, or root result positions");
        require(identities.insert(attributes.roots.front().key).second,
            "copyQualified collided across sibling or nested instance paths");
        require(source.graph.frozen() && node::read(source.graph, sourceOp) == sourceAttributes,
            "copyQualified mutated the frozen source associations");
    }
    source.design.deleteGraph("top");
    actual.output("first", actualValue0); actual.output("second", actualValue1);
    auto restored = roundTrip(actual.design);
    const auto& restoredGraph = *restored.findGraph("top");
    require(node::read(restoredGraph, restoredGraph.valueDef(restoredGraph.outputPortValue("second"))) ==
            node::read(actual.graph, actualOp), "qualified attributes borrowed storage from a deleted source");
}

void keyInterner() {
    node::NodeKeyInterner interner;
    const auto first = interner.intern("canonical-node-key");
    const auto duplicate = interner.intern("canonical-node-key");
    const auto second = interner.intern("another-node-key");
    require(first == duplicate && first != second && interner.size() == 2,
        "node-key interner did not reuse canonical IDs");
    require(interner.value(first) == "canonical-node-key" &&
            interner.bytes() == std::string_view("canonical-node-key").size() +
                std::string_view("another-node-key").size(),
        "node-key interner lost canonical storage accounting");
    auto copy = interner;
    require(copy.value(first) == interner.value(first) && copy.size() == interner.size(),
        "node-key interner copy rebuilt dangling IDs");
    auto moved = std::move(copy);
    require(moved.value(second) == "another-node-key" && moved.size() == interner.size(),
        "node-key interner move rebuilt dangling IDs");
}

// Reference census uses full inspection records and owning sets, as before.
std::vector<std::string> originalCensus(const grh::Graph& graph) {
    const auto inspection = originalInspection(graph);
    require(inspection.errors.empty(), "invalid reference census fixture");
    std::set<std::string> keys, available, constants, aliases, shared, fragments;
    std::size_t references = 0, bytes = 0, roots = 0;
    for (const auto& record : inspection.nodes) {
        if (!record.roots.empty()) available.insert(record.key);
        else fragments.insert(record.key);
        roots += record.roots.size();
        for (const auto& root : record.roots) {
            auto operation = root.operation;
            const auto args = graph.opOperands(operation);
            if (graph.opKind(operation) == OperationKind::kAssign && args.size() == 1) {
                aliases.insert(record.key); operation = graph.valueDef(args[0]);
            }
            if (operation && graph.opKind(operation) == OperationKind::kConstant) constants.insert(record.key);
            if (node::read(graph, root.operation).roots.size() > 1) shared.insert(record.key);
        }
    }
    for (const auto op : graph.operations()) {
        const auto attributes = node::read(graph, op);
        const auto add = [&](const std::string& key) { keys.insert(key); ++references; bytes += key.size(); };
        for (const auto& key : attributes.members) add(key);
        for (const auto& key : attributes.origins) add(key);
        for (const auto& root : attributes.roots) add(root.key);
    }
    const auto metadata = [&](std::string_view name) {
        const auto value = graph.metadata(name);
        return value ? std::get<std::vector<std::string>>(*value) : std::vector<std::string>{};
    };
    const auto eliminated = metadata(node::kEliminated), inventory = metadata(node::kInventory);
    const std::set<std::string> dead(eliminated.begin(), eliminated.end());
    std::size_t missing = 0, removed = 0, uniqueBytes = 0;
    for (const auto& key : keys) uniqueBytes += key.size();
    for (const auto& key : inventory) if (!available.contains(key)) {
        if (dead.contains(key)) ++removed;
        else ++missing;
    }
    return {"v2", "source-nodes=" + std::to_string(inventory.size()), "available=" + std::to_string(available.size()),
        "roots=" + std::to_string(roots), "dead=" + std::to_string(removed),
        "constant=" + std::to_string(constants.size()), "alias=" + std::to_string(aliases.size()),
        "shared=" + std::to_string(shared.size()), "fragments=" + std::to_string(fragments.size()),
        "isolated=" + std::to_string(inspection.isolatedFragments.size()), "missing=" + std::to_string(missing),
        "key-references=" + std::to_string(references), "key-bytes=" + std::to_string(bytes),
        "unique-key-bytes=" + std::to_string(uniqueBytes)};
}

void sameCensus(grh::Graph& graph) {
    const auto expected = originalCensus(graph);
    node::recordRetention(graph);
    require(graph.metadata("firrtl.nodeRetention") == grh::AttributeValue(expected), "retention differs from former full-inspection census");
}

void canonicalValidationEquivalence() {
    const auto originalNormalize = [](node::Attributes attributes) {
        const auto unique = [](auto& values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        };
        unique(attributes.members); unique(attributes.origins); unique(attributes.roots);
        for (const auto& member : attributes.members) (void)node::parseKey(member);
        for (const auto& origin : attributes.origins) (void)node::parseKey(origin);
        for (const auto& root : attributes.roots) (void)node::parseKey(root.key);
        return attributes;
    };
    const auto compare = [&](const node::Attributes& input) {
        node::Attributes expected, actual;
        std::string expectedError, actualError;
        try { expected = originalNormalize(input); }
        catch (const std::invalid_argument& error) { expectedError = error.what(); }
        try { actual = node::normalize(input); }
        catch (const std::invalid_argument& error) { actualError = error.what(); }
        require(actualError == expectedError, "canonical validation changed rejection or diagnostic priority");
        if (expectedError.empty()) require(actual == expected, "canonical validation changed normalized attributes");
    };
    const auto candidate = [&](const std::string& encoded) {
        // Each payload is checked in every attribute category, independently
        // of the first failing category and the deduplication traversal.
        compare({{encoded}, {}, {}});
        compare({{}, {}, {encoded}});
        compare({{}, {{encoded, -1}}, {}});
    };

    Fixture f;
    const auto input = f.input("a"), output = f.compute(OperationKind::kNot, {input}, key("stable"), true);
    f.output("y", output);
    const auto stamp = tx::DesignStamp::capture(f.graph);
    for (unsigned byte = 0; byte < 256; ++byte) {
        const std::string suffix(1, static_cast<char>(byte));
        const node::Identity identity{"s" + suffix, "c" + suffix, "m" + suffix,
            {"i" + suffix}, "n" + suffix, {"f" + suffix}};
        const auto encoded = node::key(identity);
        require(node::parseKey(encoded) == identity, "byte fixture did not preserve identity");
        candidate(encoded);
    }

    const std::string simple = R"(["s","c","m",["i"],"n",["f"]])";
    for (std::size_t offset = 0; offset < simple.size(); ++offset) {
        candidate(simple.substr(0, offset));
        auto erased = simple; erased.erase(offset, 1); candidate(erased);
        for (unsigned byte = 0; byte < 256; ++byte) {
            auto replaced = simple; replaced[offset] = static_cast<char>(byte); candidate(replaced);
            auto inserted = simple; inserted.insert(offset, 1, static_cast<char>(byte)); candidate(inserted);
        }
    }
    candidate(simple); candidate(simple + '\0'); candidate(simple + " ");
    for (const std::string encoded : {
        R"(["","c","m",[],"n",[]])", R"(["s","","m",[],"n",[]])",
        R"(["s","c","",[],"n",[]])", R"(["s","c","m",[],"",[]])",
        R"(["s","c","m",[""],"n",[]])", R"(["s","c","m",[],"n",[""]])",
        R"(["\u0000","c","m",[],"n",[]])", R"(["\u001f","c","m",[],"n",[]])",
        R"(["\u000A","c","m",[],"n",[]])", R"(["\u0020","c","m",[],"n",[]])",
        R"(["\u0022","c","m",[],"n",[]])", R"(["\u005c","c","m",[],"n",[]])",
        R"(["\u00ff","c","m",[],"n",[]])", R"(["\n","c","m",[],"n",[]])",
        R"(["\/","c","m",[],"n",[]])", R"(["\\","c","m",[],"n",[]])",
        R"(["\"","c","m",[],"n",[]])", R"(["\u001","c","m",[],"n",[]])"}) candidate(encoded);

    node::Identity longIdentity{std::string(8192, 's'), "circuit", "module", {}, std::string(4096, 'n'), {"field"}};
    for (unsigned i = 0; i < 512; ++i) longIdentity.instancePath.push_back("instance/" + std::to_string(i));
    const auto longKey = node::key(longIdentity);
    candidate(longKey);
    const auto first = key("first"), last = key("last");
    compare({{last, first, first}, {{last, 2}, {first, -1}, {first, 0}, {last, 2}}, {last, first, last}});
    compare({{"invalid member", first}, {{"invalid root", 0}}, {"invalid origin"}});
    compare({{first}, {{"invalid root", 0}}, {"invalid origin"}});
    compare({{first}, {{"invalid root", 0}}, {last}});
    require(stamp.matches(f.graph), "canonical normalization mutated graph state");
    const auto op = f.graph.valueDef(output);
    const auto original = node::read(f.graph, op);
    rejects([&] { node::write(f.graph, op, {{longKey, "invalid"}, {}, {}}); }, "invalid canonical key was written");
    require(stamp.matches(f.graph) && node::read(f.graph, op) == original,
        "invalid canonical write mutated graph state");
}

void identityAndPersistence() {
    const node::Identity identity{"source\"\\\n", "Circuit", "top", {"a/b", "c"}, "n", {"x__DOT__y", "z"}};
    const auto encoded = node::key(identity);
    require(node::parseKey(encoded) == identity, "escaped node key did not round-trip");
    auto different = identity; different.instancePath = {"a", "b/c"};
    require(node::key(different) != encoded, "instance path components collided");
    different = identity; different.fieldPath = {"x", "y__DOT__z"};
    require(node::key(different) != encoded, "field path components collided");
    rejects([&] { node::parseKey(encoded + " "); }, "noncanonical key was accepted");
    rejects([&] { node::key({}); }, "empty identity was accepted");

    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto temporary = f.compute(OperationKind::kAnd, {a, b});
    const auto dead = f.graph.valueDef(temporary);
    const auto value = f.compute(OperationKind::kXor, {a, b}, encoded, true);
    require(f.graph.eraseOp(dead) && f.graph.eraseValue(temporary), "could not create sparse IDs");
    f.output("y", value);
    auto clone = f.design.clone();
    auto& left = *clone.findGraph("top");
    auto& right = clone.cloneGraph("top", "other");
    const std::array<std::string, 2> leftPath{"u0", "inner"}, rightPath{"u1", "inner"};
    for (const auto op : left.operations()) node::qualify(left, op, leftPath);
    for (const auto op : right.operations()) node::qualify(right, op, rightPath);
    require(node::inspect(left).nodes[0].key != node::inspect(right).nodes[0].key,
            "same-module instances received the same key");
    const auto record = node::inspect(left).nodes.at(0);
    const auto root = record.roots.at(0);
    require(root.value.index != value.index, "clone fixture did not actually reorder value IDs");
    require(root.operation.index != f.graph.valueDef(value).index, "clone fixture did not actually reorder operation IDs");
    left.setOpSymbol(root.operation, left.internSymbol("renamed_node_op"));
    left.setValueSymbol(root.value, left.internSymbol("renamed_node_value"));
    const auto renamed = node::inspect(left);
    require(renamed.nodes.at(0).key == record.key && renamed.nodes.at(0).roots.at(0).value == root.value,
            "renaming an operation/value changed source identity or result mapping");
    require(renamed.report(left).find("root=renamed_node_op:0 value=renamed_node_value") != std::string::npos,
            "inspection did not resolve renamed operation/value symbols");
    const auto before = node::inspect(left).report(left);
    left.freeze(); right.freeze();
    valid(clone);
    auto loaded = roundTrip(clone);
    valid(loaded);
    require(node::inspect(*loaded.findGraph("top")).report(*loaded.findGraph("top")) == before,
            "root symbols/result mapping changed across clone/freeze/JSON");
    run(loaded, std::make_unique<tx::GrhVerifyPass>());
}

void mergeAndRemap() {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto one = f.compute(OperationKind::kAnd, {a, b}, key("one"), true);
    const auto two = f.compute(OperationKind::kAnd, {a, b}, key("two"), true);
    const auto op1 = f.graph.valueDef(one), op2 = f.graph.valueDef(two);
    node::copyMembers(f.graph, op1, op1);
    node::mergeCseOperations(f.graph, op1, op1);
    node::mergeEquivalentOperations(f.graph, op1, op1, std::array<int64_t, 1>{0});
    require(node::read(f.graph, op1).roots.size() == 1, "self copy/merge lost the result root");
    node::mergeCseOperations(f.graph, op1, op2);
    require(node::unite(node::read(f.graph, op1), node::read(f.graph, op2),
                        node::MergeReason::SameComputation).members.size() == 2, "CSE associations lost");
    const auto remapped = node::remapResults(node::Attributes{{key("two")}, {{key("two"), 0}}, {}}, std::array<int64_t, 1>{1});
    require(remapped.roots[0].result == 1, "result remap lost a nonzero result index");
    require(node::remapResults(remapped, std::array<int64_t, 1>{0}).roots.empty(), "unmapped optional root was retained");
    // Whole-node equivalence is proven here directly by the identical operations.
    node::mergeEquivalentOperations(f.graph, op1, op2, std::array<int64_t, 1>{0});
    require(f.graph.eraseOp(op2), "could not erase the equivalent node");
    f.output("y", one);
    const auto attrs = node::read(f.graph, op1);
    require(attrs.members.size() == 2 && attrs.roots.size() == 2 && attrs.origins.size() == 2,
            "whole-node merge did not keep both identities");
    valid(f.design);
    auto loaded = roundTrip(f.design);
    valid(loaded);
    require(node::inspect(*loaded.findGraph("top")).nodes.size() == 2, "merged origins did not round-trip");
}

void invalidMetadata() {
    Fixture f;
    const auto a = f.input("a"), value = f.compute(OperationKind::kNot, {a}, key("n"), true);
    f.output("y", value);
    const auto op = f.graph.valueDef(value);
    const auto check = [&](const std::function<void(grh::Graph&, grh::OperationId)>& corrupt) {
        auto copy = f.design.clone();
        auto& graph = *copy.findGraph("top");
        corrupt(graph, graph.findOperation(f.graph.symbolText(f.graph.operationSymbol(op))));
        sameValidation(graph);
        require(!node::inspect(graph).errors.empty(), "corrupt node attrs were accepted");
        require(!run(copy, std::make_unique<tx::GrhVerifyPass>(), false), "GRH verifier accepted corrupt node attrs");
    };
    check([](auto& graph, auto op) { graph.eraseMetadata(node::kSchema); });
    check([](auto& graph, auto op) { graph.setMetadata(node::kSchema, int64_t{1}); });
    check([](auto& graph, auto op) { graph.setMetadata(node::kSchema, std::string("1")); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, int64_t{1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kOrigins, int64_t{1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRoots, int64_t{1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::string("0")); });
    check([](auto& graph, auto op) { graph.eraseAttr(op, node::kRootResults); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{-1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, "firrtl.node.unknown", true); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, std::vector<std::string>{"invalid"}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kOrigins, std::vector<std::string>{key("n") + " "}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{0, 0}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, std::vector<std::string>{key("n"), key("n")}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kOrigins, std::vector<std::string>{key("z"), key("a")}); });
    check([](auto& graph, auto op) {
        graph.setAttr(op, node::kRoots, std::vector<std::string>{key("n"), key("n")});
        graph.setAttr(op, node::kRootResults, std::vector<int64_t>{0, 0});
    });
    check([](auto& graph, auto op) {
        graph.setAttr(op, node::kRoots, std::vector<std::string>{key("z"), key("a")});
        graph.setAttr(op, node::kRootResults, std::vector<int64_t>{0, 0});
    });
    check([](auto& graph, auto op) {
        graph.setAttr(op, node::kMembers, std::vector<std::string>{key("z"), key("a")});
        graph.setAttr(op, node::kOrigins, std::vector<std::string>{"invalid"});
    });
    const auto second = f.compute(OperationKind::kNot, {a}, key("second"), true);
    f.graph.setMetadata(node::kSchema, int64_t{1});
    f.graph.setAttr(op, node::kMembers, int64_t{1});
    f.graph.setAttr(f.graph.valueDef(second), node::kRootResults, std::vector<int64_t>{5});
    const auto expected = originalInspection(f.graph).errors;
    require(expected.size() == 3, "multi-diagnostic fixture did not produce all expected errors");
    sameValidation(f.graph);
    tx::PassManager manager; manager.addPass(std::make_unique<tx::RedundantElimPass>());
    tx::PassDiagnostics diagnostics;
    const auto result = manager.run(f.design, diagnostics);
    require(!result.success && diagnostics.messages().size() == expected.size(),
        "redundant-elim did not preserve every node diagnostic");
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(diagnostics.messages()[i].message == expected[i],
            "redundant-elim changed node diagnostic text or order");
}

void retentionEquivalence() {
    Fixture empty;
    empty.compute(OperationKind::kNot, {empty.input("a")});
    node::requireValid(empty.graph); node::recordRetention(empty.graph);
    require(!empty.graph.metadata("firrtl.nodeRetention"), "retention added a policy to a non-FIRRTL graph");
    node::establishPolicy(empty.graph); sameCensus(empty.graph);
    empty.graph.setMetadata(node::kSchema, std::string("invalid")); sameValidation(empty.graph);

    Fixture f;
    const auto input = f.input("a"), constant = f.constant("8'd3", key("constant"));
    f.compute(OperationKind::kAssign, {constant}, key("alias"), true);
    const auto shared = f.compute(OperationKind::kAssign, {input});
    node::write(f.graph, f.graph.valueDef(shared), {{key("shared-a"), key("shared-b")},
        {{key("shared-a"), 0}, {key("shared-b"), 0}}, {key("shared-a"), key("shared-b")}});
    f.compute(OperationKind::kNot, {input}, key("shared-a"), true);
    f.compute(OperationKind::kNot, {input}, key("fragment"), false);
    const auto origin = f.compute(OperationKind::kNot, {input});
    node::write(f.graph, f.graph.valueDef(origin), {{}, {}, {key("origin-only")}});
    const auto root = f.compute(OperationKind::kNot, {input});
    node::write(f.graph, f.graph.valueDef(root), {{}, {{key("root-only"), 0}}, {}});
    const auto literal = f.constant("8'd4");
    node::write(f.graph, f.graph.valueDef(literal), {{}, {}, {key("literal-origin")}});
    const auto isolated = f.compute(OperationKind::kNot, {input});
    f.graph.setAttr(f.graph.valueDef(isolated), node::kMembers, std::vector<std::string>{});
    node::establishPolicy(f.graph);
    auto inventory = std::get<std::vector<std::string>>(*f.graph.metadata(node::kInventory));
    inventory.insert(inventory.end(), {key("dead"), key("dead"), key("missing"), key("missing")});
    f.graph.setMetadata(node::kInventory, inventory);
    f.graph.setMetadata(node::kEliminated, std::vector<std::string>{key("dead"), key("dead"), key("not-in-inventory")});
    sameCensus(f.graph);
    const auto expected = originalCensus(f.graph);
    for (const auto field : {"available=5", "roots=6", "dead=2", "constant=2", "alias=3", "shared=2", "fragments=1", "isolated=3", "missing=3"})
        require(std::find(expected.begin(), expected.end(), field) != expected.end(), "reference fixture missed a census category");
    auto restored = roundTrip(f.design); sameCensus(*restored.findGraph("top"));
    require(restored.findGraph("top")->metadata("firrtl.nodeRetention") == f.graph.metadata("firrtl.nodeRetention"),
        "borrowed retention changed across JSON restoration");
    for (const auto metadata : {node::kInventory, node::kEliminated}) {
        auto copy = f.design.clone(); auto& graph = *copy.findGraph("top");
        graph.setMetadata(metadata, int64_t{1});
        rejects([&] { node::recordRetention(graph); }, "wrong inventory/tombstone type was accepted");
    }
}

void executionClones() {
    Fixture f;
    const auto a = f.input("a");
    const auto original = f.compute(OperationKind::kNot, {a}, key("phase"), true);
    const auto clone = f.compute(OperationKind::kNot, {a}, key("phase"), true);
    const auto op = f.graph.valueDef(original), copy = f.graph.valueDef(clone);
    f.graph.setAttr(op, "grhsim.partition", std::vector<std::string>{"general"});
    f.graph.setAttr(copy, "grhsim.partition", std::vector<std::string>{"output"});
    require(node::inspect(f.graph).errors.empty(), "optional phase roots were rejected");
    f.graph.setAttr(copy, "grhsim.nodeClone", true);
    f.output("y", clone); valid(f.design);
    auto loaded = roundTrip(f.design); valid(loaded);
    require(node::inspect(*loaded.findGraph("top")).nodes.at(0).roots.size() == 2, "phase clone root lost across JSON");
    f.graph.setAttr(copy, "grhsim.partition", std::vector<std::string>{"general"});
    require(node::inspect(f.graph).errors.empty(), "optional duplicate source roots were rejected");
}

void orphanLiteralProvenance() {
    Fixture f;
    const auto value = f.constant("8'd3");
    const auto literal = f.graph.valueDef(value);
    const auto dead = key("dead-node"), live = key("live-node");
    node::write(f.graph, literal, {{dead, live}, {}, {dead, live}});
    const auto root = f.compute(OperationKind::kAssign, {value}, live, true);
    f.output("y", root);
    valid(f.design); valid(roundTrip(f.design));
    const auto computation = f.compute(OperationKind::kNot, {value});
    const auto operation = f.graph.valueDef(computation);
    f.tag(operation, dead, false); f.output("escaped", computation);
    require(node::inspect(f.graph).errors.empty(), "optional rootless association was rejected");
}

void multipleResults() {
    Fixture f;
    auto& pair = f.design.createGraph("Pair");
    const auto input = pair.createValue(pair.internSymbol("in"), Fixture::type());
    pair.bindInputPort("in", input); pair.bindOutputPort("x", input); pair.bindOutputPort("y", input);
    const auto a = f.input("a");
    const auto makeInstance = [&](std::string name, bool reversed) {
        const auto op = f.graph.createOperation(OperationKind::kInstance);
        f.graph.addOperand(op, a);
        for (unsigned i = 0; i < 2; ++i) f.graph.addResult(op, f.graph.createValue(Fixture::type()));
        f.graph.setAttr(op, "moduleName", std::string("Pair"));
        f.graph.setAttr(op, "instanceName", name);
        f.graph.setAttr(op, "inputPortName", std::vector<std::string>{"in"});
        f.graph.setAttr(op, "outputPortName", reversed ? std::vector<std::string>{"y", "x"}
                                                       : std::vector<std::string>{"x", "y"});
        node::Attributes attrs;
        const auto x = key(name, {"x"}), y = key(name, {"y"});
        attrs.members = {x, y}; attrs.roots = {{x, reversed ? 1 : 0}, {y, reversed ? 0 : 1}};
        node::write(f.graph, op, std::move(attrs));
        return op;
    };
    const auto source = makeInstance("source", false), target = makeInstance("target", true);
    run(f.design, std::make_unique<tx::GrhVerifyPass>());
    node::mergeEquivalentOperations(f.graph, target, source, std::array<int64_t, 2>{1, 0});
    require(f.graph.eraseOp(source), "could not remove remapped source instance");
    const auto values = f.graph.opResults(target);
    f.output("y", values[0]); f.output("x", values[1]);
    const auto check = [](const grh::Graph& graph) {
        const auto inspection = node::inspect(graph);
        require(inspection.errors.empty() && inspection.nodes.size() == 4, "multi-result roots were lost");
        for (const auto& record : inspection.nodes) {
            const auto identity = node::parseKey(record.key);
            const auto expected = identity.fieldPath[0] == "x" ? 1 : 0;
            require(record.roots.size() == 1 && record.roots[0].result == expected,
                    "root result permutation was not preserved");
            const auto port = std::find_if(graph.outputPorts().begin(), graph.outputPorts().end(),
                                           [&](const auto& port) { return port.name == identity.fieldPath[0]; });
            require(port != graph.outputPorts().end() && record.roots[0].value == port->value,
                    "inspection mapped a node leaf to the wrong output value");
        }
    };
    check(f.graph);
    auto loaded = roundTrip(f.design);
    check(*loaded.findGraph("top"));
    run(loaded, std::make_unique<tx::GrhVerifyPass>());
    // Both Pair outputs alias the same input, so an intra-op root redirection is valid.
    const auto replacement = node::redirectRoots(f.graph, values[0], values[1]);
    require(replacement == values[1], "same-operation root redirection created a carrier");
    for (const auto& record : node::inspect(f.graph).nodes)
        require(record.roots[0].result == 1, "same-operation root redirection lost a root");
}

void boundedHints() {
    Fixture f;
    const auto input = f.input("a"), value = f.compute(OperationKind::kNot, {input});
    const auto op = f.graph.valueDef(value); f.output("y", value);
    node::Attributes attrs;
    for (unsigned i = 0; i <= node::kMaxAssociations; ++i) attrs.members.push_back(key("node-" + std::to_string(i)));
    const auto operations = f.graph.operations().size();
    node::write(f.graph, op, std::move(attrs));
    require(!node::annotated(f.graph, op), "over-budget hint was retained");
    require(f.graph.metadata("firrtl.nodeHintDegraded") == grh::AttributeValue(int64_t{1}), "hint degradation not diagnosed");
    require(f.graph.operations().size() == operations, "hint degradation created scaffolding");
    node::establishPolicy(f.graph);
    f.graph.setMetadata(node::kScopePolicy, std::string("source-node-v1"));
    rejects([&] { node::requirePolicy(f.graph); }, "legacy strong node policy was silently accepted");
}

void deadInventory() {
    Fixture f;
    const auto input = f.input("a"), other = f.input("b");
    const auto old = f.compute(OperationKind::kAnd, {input, other}, key("old"), true);
    const auto first = f.compute(OperationKind::kXor, {input, other}, key("new"), true);
    const auto duplicate = f.compute(OperationKind::kXor, {input, other}, key("new"), true);
    const auto live = f.compute(OperationKind::kNot, {input}, key("live"), true);
    f.output("y", live); node::establishPolicy(f.graph);
    const auto original = f.design.clone();
    const auto erase = [&](auto value) {
        require(f.graph.eraseOp(f.graph.valueDef(value)) && f.graph.eraseValue(value), "could not erase dead fixture root");
    };
    node::recordDead(f.graph, f.graph.valueDef(old)); erase(old);
    node::DeadRootBatch batch(f.graph);
    for (const auto value : {first, duplicate}) { batch.record(f.graph.valueDef(value)); erase(value); }
    batch.flush(); batch.flush();
    auto expected = std::vector<std::string>{key("old"), key("new")};
    std::sort(expected.begin(), expected.end());
    require(f.graph.metadata(node::kEliminated) == grh::AttributeValue(expected),
        "batched deletion lost existing tombstones or retained duplicate roots");
    node::recordRetention(f.graph);
    const auto census = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeRetention"));
    require(std::find(census.begin(), census.end(), "dead=2") != census.end() &&
            std::find(census.begin(), census.end(), "missing=0") != census.end(),
        "deleted roots were reported as lost hints instead of eliminated nodes");
    auto loaded = roundTrip(f.design); valid(loaded);
    auto& restored = *loaded.findGraph("top"); node::recordRetention(restored);
    require(restored.metadata(node::kEliminated) == f.graph.metadata(node::kEliminated) &&
            restored.metadata("firrtl.nodeRetention") == f.graph.metadata("firrtl.nodeRetention"),
        "batched tombstones or retention census changed across JSON");
    const auto pass = [](std::string_view name) {
        std::string error;
        auto implementation = tx::makePass(name, {}, error);
        require(bool(implementation), error); return implementation;
    };
    for (const auto name : {"dead-code-elim", "mux-tree-fold", "grhsim-simplify"}) {
        auto candidate = original.clone();
        if (std::string_view(name) == "grhsim-simplify") run(candidate, pass("grh-to-grhsim"));
        run(candidate, pass(name));
        auto& graph = *candidate.findGraph("top"); node::recordRetention(graph);
        const auto stats = std::get<std::vector<std::string>>(*graph.metadata("firrtl.nodeRetention"));
        require(graph.metadata(node::kEliminated) == grh::AttributeValue(expected) &&
                std::find(stats.begin(), stats.end(), "dead=2") != stats.end() &&
                std::find(stats.begin(), stats.end(), "missing=0") != stats.end(),
            std::string(name) + " did not publish dead node roots before returning");
        run(candidate, std::make_unique<tx::GrhVerifyPass>());
    }
}

std::string qualifiedKey(std::string_view encoded, std::span<const std::string> path) {
    auto identity = node::parseKey(encoded);
    identity.instancePath.insert(identity.instancePath.begin(), path.begin(), path.end());
    return node::key(identity);
}

void qualificationContext() {
    node::NodeKeyInterner interner;
    const auto interned = node::key({"fixture", "Circuit", "child", {}, "shared", {}});
    const auto firstId = interner.intern(interned);
    require(firstId == interner.intern(interned) && interner.size() == 1 &&
            interner.bytes() == interned.size() && interner.value(firstId) == interned,
        "node-key interner failed to reuse canonical storage");
    Fixture fixture;
    const auto value = fixture.compute(OperationKind::kNot, {fixture.input("a")});
    fixture.output("y", value);
    auto reference = fixture.design.clone(), candidate = fixture.design.clone();
    auto& expected = *reference.findGraph("top");
    auto& actual = *candidate.findGraph("top");
    const auto referenceOp = expected.valueDef(expected.outputPorts()[0].value);
    const auto candidateOp = actual.valueDef(actual.outputPorts()[0].value);
    const std::vector<std::string> frozenPath{"outer/part", "child\"\\\n"};
    auto mutablePath = frozenPath;
    node::QualificationContext context(mutablePath);
    mutablePath.assign({"different", "sibling"});
    node::Attributes original;
    // Reuse the first key after growing the cache and replacing op attributes.
    for (unsigned i = 0; i <= 96; ++i) {
        const auto encoded = node::key({"fixture\"\\\n", "Circuit", "child", {"local/part"},
            "node" + std::to_string(i == 96 ? 0 : i), {"field", "x/y"}});
        original = {{encoded}, {{encoded, 0}}, {encoded}};
        node::write(expected, referenceOp, original); node::write(actual, candidateOp, original);
        node::qualify(expected, referenceOp, frozenPath); node::qualify(actual, candidateOp, context);
        require(node::read(actual, candidateOp) == node::read(expected, referenceOp),
            "qualification cache changed an escaped key, result association, or owned path");
    }
    const auto parentAttributes = node::read(actual, candidateOp);
    {
        node::QualificationContext sibling(mutablePath);
        node::write(expected, referenceOp, original); node::write(actual, candidateOp, original);
        node::qualify(expected, referenceOp, mutablePath); node::qualify(actual, candidateOp, sibling);
        require(node::read(actual, candidateOp) == node::read(expected, referenceOp) &&
                node::read(actual, candidateOp) != parentAttributes,
            "qualification cache leaked one instance path into another");
    }
    node::write(actual, candidateOp, original); node::qualify(actual, candidateOp, context);
    require(node::read(actual, candidateOp) == parentAttributes, "nested context changed its parent's cache");
    require(context.stats().lookups > context.stats().hits && context.stats().hits > 0 &&
            context.stats().misses > 0 && context.stats().inputBytes > 0 && context.stats().outputBytes > 0,
        "qualification context did not report key interning reuse");

    const auto failure = [](const auto& action) {
        try { action(); }
        catch (const std::invalid_argument& error) { return std::string(error.what()); }
        return std::string{};
    };
    const auto sameRejection = [&](const auto& corrupt) {
        expected.setMetadata(node::kSchema, node::kSchemaRevision);
        actual.setMetadata(node::kSchema, node::kSchemaRevision);
        node::write(expected, referenceOp, original); node::write(actual, candidateOp, original);
        corrupt(expected, referenceOp); corrupt(actual, candidateOp);
        const auto legacy = failure([&] { node::qualify(expected, referenceOp, frozenPath); });
        const auto cached = failure([&] { node::qualify(actual, candidateOp, context); });
        require(!legacy.empty() && cached == legacy, "cached qualification changed validation diagnostics");
    };
    sameRejection([](auto& graph, auto) { graph.setMetadata(node::kSchema, int64_t{1}); });
    sameRejection([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{-1}); });
    sameRejection([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{1}); });
    sameRejection([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{}); });
    sameRejection([](auto& graph, auto op) { graph.setAttr(op, node::kOrigins, int64_t{1}); });
    sameRejection([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, std::vector<std::string>{"invalid"}); });

    std::vector<std::string> overCap;
    for (unsigned i = 0; i <= node::kMaxAssociations; ++i) overCap.push_back(key("over-cap" + std::to_string(i)));
    expected.setAttr(referenceOp, node::kMembers, overCap); actual.setAttr(candidateOp, node::kMembers, overCap);
    node::qualify(expected, referenceOp, frozenPath); node::qualify(actual, candidateOp, context);
    require(node::read(actual, candidateOp) == node::Attributes{} &&
            node::read(actual, candidateOp) == node::read(expected, referenceOp) &&
            actual.metadata("firrtl.nodeHintDegraded") == expected.metadata("firrtl.nodeHintDegraded"),
        "qualification cache changed association-cap degradation");
}

void sameInventory(const grh::Graph& actual, const grh::Graph& expected) {
    for (const auto name : {node::kScopePolicy, node::kInventory, node::kEliminated})
        require(actual.metadata(name) == expected.metadata(name), "batched import differs from sequential import");
}

void inventoryImportBatch() {
    Fixture source;
    const auto input = source.input("a");
    const auto liveKey = node::key({"fixture", "Circuit", "child", {"inside", "leaf/a"}, "live", {"data", "x/y"}});
    const auto deadKey = node::key({"fixture", "Circuit", "child", {"inside", "leaf/a"}, "dead", {}});
    const auto live = source.compute(OperationKind::kNot, {input}, liveKey, true);
    const auto dead = source.compute(OperationKind::kNot, {input}, deadKey, true);
    source.output("y", live); node::establishPolicy(source.graph);
    node::recordDead(source.graph, source.graph.valueDef(dead));
    require(source.graph.eraseOp(source.graph.valueDef(dead)) && source.graph.eraseValue(dead),
        "could not erase imported dead root");

    Fixture initial;
    const auto a = initial.input("a");
    const auto existing = initial.compute(OperationKind::kNot, {a}, key("existing"), true);
    const auto removed = initial.compute(OperationKind::kNot, {a}, key("removed"), true);
    initial.output("y", existing); node::establishPolicy(initial.graph);
    node::recordDead(initial.graph, initial.graph.valueDef(removed));
    require(initial.graph.eraseOp(initial.graph.valueDef(removed)) && initial.graph.eraseValue(removed),
        "could not erase existing dead root");
    auto sequential = initial.design.clone(), batched = initial.design.clone();
    auto& expected = *sequential.findGraph("top");
    auto& actual = *batched.findGraph("top");
    const std::array<std::vector<std::string>, 4> paths{{{"left", "child"}, {"right", "child"},
                                                       {"a/b", "c"}, {"a", "b/c"}}};
    const auto append = [&](grh::Graph& graph, std::span<const std::string> path, unsigned ordinal) {
        const auto op = graph.createOperation(OperationKind::kNot);
        const auto value = graph.createValue(Fixture::type());
        graph.addOperand(op, graph.inputPorts()[0].value); graph.addResult(op, value);
        node::write(graph, op, node::read(source.graph, source.graph.valueDef(live)));
        node::qualify(graph, op, path);
        graph.bindOutputPort("copy" + std::to_string(ordinal), value);
    };
    auto inventory = std::vector<std::string>{key("existing"), key("removed")};
    auto eliminated = std::vector<std::string>{key("removed")};
    const auto beforeInventory = actual.metadata(node::kInventory), beforeDead = actual.metadata(node::kEliminated);
    node::InventoryImportBatch batch(actual);
    for (unsigned i = 0; i < paths.size(); ++i) {
        append(expected, paths[i], i); append(actual, paths[i], i);
        node::importInventory(source.graph, expected, paths[i]);
        batch.record(source.graph, paths[i]);
        inventory.push_back(qualifiedKey(liveKey, paths[i]));
        inventory.push_back(qualifiedKey(deadKey, paths[i]));
        eliminated.push_back(qualifiedKey(deadKey, paths[i]));
        if (i == 0) {
            require(actual.metadata(node::kInventory) == beforeInventory && actual.metadata(node::kEliminated) == beforeDead,
                "record published pending inventory before flush");
            node::requirePolicy(actual);
        }
        if (i == 1) { batch.flush(); batch.flush(); sameInventory(actual, expected); }
    }
    // Repeated imports of the same instance must not duplicate its census entry.
    node::importInventory(source.graph, expected, paths[0]); batch.record(source.graph, paths[0]);
    const auto operationCount = actual.operations().size(), valueCount = actual.values().size();
    batch.flush(); batch.flush();
    sameInventory(actual, expected);
    std::sort(inventory.begin(), inventory.end()); std::sort(eliminated.begin(), eliminated.end());
    require(actual.metadata(node::kInventory) == grh::AttributeValue(inventory) &&
            actual.metadata(node::kEliminated) == grh::AttributeValue(eliminated),
        "batch lost existing entries, nested path components, or distinct escaped paths");
    require(actual.operations().size() == operationCount && actual.values().size() == valueCount,
        "inventory flush created optimization scaffolding");
    require(node::inspect(actual).report(actual) == node::inspect(expected).report(expected),
        "inventory batching changed operation hints");
    for (std::size_t i = 0; i < actual.operations().size(); ++i)
        require(node::read(actual, actual.operations()[i]) == node::read(expected, expected.operations()[i]),
            "inventory batching changed node members, origins, or result associations");
    for (const auto op : actual.operations()) {
        require(node::cseScope(actual, op).empty(), "imported soft hints created a CSE scope");
        require(node::sameScope(actual, op, actual.valueDef(actual.outputPorts()[0].value)),
            "imported soft hints separated equivalent computation scopes");
    }
    node::recordRetention(actual); node::recordRetention(expected);
    require(actual.metadata("firrtl.nodeRetention") == expected.metadata("firrtl.nodeRetention"),
        "batched import changed the retention census");
    const auto census = std::get<std::vector<std::string>>(*actual.metadata("firrtl.nodeRetention"));
    for (const auto item : {"source-nodes=10", "available=5", "dead=5", "missing=0"})
        require(std::find(census.begin(), census.end(), item) != census.end(), "imported inventory census is incomplete");
    valid(batched); auto loaded = roundTrip(batched); valid(loaded);
    auto& restored = *loaded.findGraph("top"); node::recordRetention(restored);
    sameInventory(restored, actual);
    require(restored.metadata("firrtl.nodeRetention") == actual.metadata("firrtl.nodeRetention") &&
            node::inspect(restored).report(restored) == node::inspect(actual).report(actual),
        "imported identities or census changed across JSON");
    run(loaded, std::make_unique<tx::GrhVerifyPass>());

    // Ordinary CSE preserves output values; keep the duplicate computations
    // internal so this checks hint transparency rather than that port rule.
    Fixture sharing;
    const auto sharedInput = sharing.input("a");
    const auto first = sharing.compute(OperationKind::kNot, {sharedInput});
    const auto second = sharing.compute(OperationKind::kNot, {sharedInput});
    node::InventoryImportBatch imported(sharing.graph);
    const std::array<grh::ValueId, 2> duplicates{first, second};
    for (std::size_t i = 0; i < duplicates.size(); ++i) {
        const auto op = sharing.graph.valueDef(duplicates[i]);
        node::write(sharing.graph, op, node::read(source.graph, source.graph.valueDef(live)));
        node::qualify(sharing.graph, op, paths[i]); imported.record(source.graph, paths[i]);
    }
    imported.flush();
    sharing.output("y", sharing.compute(OperationKind::kXor, {first, second}));
    auto mergedKeys = std::vector<std::string>{qualifiedKey(liveKey, paths[0]), qualifiedKey(liveKey, paths[1])};
    std::sort(mergedKeys.begin(), mergedKeys.end());
    const node::Attributes mergedAttributes{mergedKeys, {{mergedKeys[0], 0}, {mergedKeys[1], 0}}, mergedKeys};
    require(count(sharing.graph, OperationKind::kNot) == 2, "CSE fixture did not contain duplicate internal operations");
    run(sharing.design, std::make_unique<tx::RedundantElimPass>());
    require(count(sharing.graph, OperationKind::kNot) == 1, "imported source-node hints blocked ordinary CSE");
    const auto retained = std::find_if(sharing.graph.operations().begin(), sharing.graph.operations().end(),
        [&](auto op) { return sharing.graph.opKind(op) == OperationKind::kNot; });
    require(retained != sharing.graph.operations().end() && node::read(sharing.graph, *retained) == mergedAttributes,
        "ordinary CSE lost qualified node members, roots, or origins");
    run(sharing.design, std::make_unique<tx::GrhVerifyPass>());
}

void inventoryImportBoundaries() {
    Fixture plain, target;
    const auto a = plain.input("a");
    plain.output("y", plain.compute(OperationKind::kNot, {a}, key("untracked"), true));
    // Loaded hints without a source policy must not be silently upgraded.
    plain.graph.setMetadata(node::kInventory, std::vector<std::string>{key("untracked")});
    const auto before = wolvrix::lib::store::StoreJson{}.storeToString(target.design);
    node::InventoryImportBatch ignored(target.graph);
    ignored.record(plain.graph, std::array<std::string, 1>{"ignored"}); ignored.flush();
    require(wolvrix::lib::store::StoreJson{}.storeToString(target.design) == before &&
            !plain.graph.metadata(node::kScopePolicy), "untracked source import established a policy");
    Fixture empty;
    ignored.record(empty.graph, {}); ignored.flush();
    require(wolvrix::lib::store::StoreJson{}.storeToString(target.design) == before,
        "empty untracked source changed the target");
    node::establishPolicy(empty.graph);
    auto sequential = target.design.clone();
    node::importInventory(empty.graph, *sequential.findGraph("top"), {});
    ignored.record(empty.graph, {}); node::requirePolicy(target.graph); ignored.flush();
    sameInventory(target.graph, *sequential.findGraph("top"));
    require(target.graph.metadata(node::kInventory) == grh::AttributeValue(std::vector<std::string>{}) &&
            !target.graph.metadata(node::kEliminated), "empty tracked source lost its valid empty policy");

    Fixture tombstones, firstImport, afterEmpty;
    node::establishPolicy(tombstones.graph);
    tombstones.graph.setMetadata(node::kInventory, std::vector<std::string>{key("already-dead")});
    tombstones.graph.setMetadata(node::kEliminated, std::vector<std::string>{key("already-dead")});
    const auto checkOrder = [](const grh::Graph& graph, const std::array<std::string_view, 3>& expected) {
        const auto metadata = graph.metadata();
        require(metadata.size() == expected.size(), "inventory import added unexpected metadata fields");
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(metadata[i].key == expected[i], "inventory import changed historical metadata field order");
    };
    node::InventoryImportBatch first(firstImport.graph);
    first.record(tombstones.graph, {}); first.flush();
    checkOrder(firstImport.graph, {node::kInventory, node::kEliminated, node::kScopePolicy});
    node::InventoryImportBatch delayed(afterEmpty.graph);
    delayed.record(empty.graph, {}); delayed.flush();
    delayed.record(tombstones.graph, {}); delayed.flush();
    checkOrder(afterEmpty.graph, {node::kInventory, node::kScopePolicy, node::kEliminated});

    Fixture lifetime;
    node::InventoryImportBatch pending(lifetime.graph);
    std::string expected;
    {
        Fixture temporary;
        const auto in = temporary.input("a");
        temporary.output("y", temporary.compute(OperationKind::kNot, {in}, key("owned"), true));
        node::establishPolicy(temporary.graph);
        std::vector<std::string> path{"outer", "inner"};
        expected = qualifiedKey(key("owned"), path);
        pending.record(temporary.graph, path);
        path.assign({"changed"});
    }
    node::requirePolicy(lifetime.graph);
    require(lifetime.graph.metadata(node::kInventory) == grh::AttributeValue(std::vector<std::string>{}),
        "first SV-top import published pending keys before flush");
    pending.flush(); pending.flush();
    require(lifetime.graph.metadata(node::kInventory) == grh::AttributeValue(std::vector<std::string>{expected}),
        "batch retained references to the source graph or instance path");
    Fixture explicitFlush;
    {
        node::InventoryImportBatch abandoned(explicitFlush.graph);
        abandoned.record(lifetime.graph, {});
    }
    require(explicitFlush.graph.metadata(node::kInventory) == grh::AttributeValue(std::vector<std::string>{}),
        "inventory batch published implicitly from its destructor");

    const auto invalidMetadata = [](std::string_view name, grh::AttributeValue value) {
        for (const bool corruptSource : {true, false}) {
            Fixture source, destination; node::establishPolicy(source.graph); node::establishPolicy(destination.graph);
            (corruptSource ? source.graph : destination.graph).setMetadata(name, value);
            rejects([&] {
                node::InventoryImportBatch batch(destination.graph); batch.record(source.graph, {}); batch.flush();
            }, "inventory batch accepted an incompatible policy or metadata type");
        }
    };
    invalidMetadata(node::kScopePolicy, std::string("source-node-v1"));
    invalidMetadata(node::kScopePolicy, int64_t{2});
    invalidMetadata(node::kInventory, int64_t{1});
    invalidMetadata(node::kEliminated, int64_t{1});
    for (const bool corruptSource : {true, false}) {
        Fixture source, destination; node::establishPolicy(source.graph); node::establishPolicy(destination.graph);
        (corruptSource ? source.graph : destination.graph).eraseMetadata(node::kInventory);
        rejects([&] {
            node::InventoryImportBatch batch(destination.graph); batch.record(source.graph, {}); batch.flush();
        }, "inventory batch upgraded a policy missing its inventory");
    }
}

grh::ValueId inventoryInstance(grh::Graph& graph, grh::ValueId input,
                               std::string module, std::string instance) {
    const auto op = graph.createOperation(OperationKind::kInstance);
    const auto value = graph.createValue(Fixture::type());
    graph.addOperand(op, input); graph.addResult(op, value);
    graph.setAttr(op, "moduleName", std::move(module)); graph.setAttr(op, "instanceName", std::move(instance));
    graph.setAttr(op, "inputPortName", std::vector<std::string>{"a"});
    graph.setAttr(op, "outputPortName", std::vector<std::string>{"y"});
    return value;
}

void flattenedInventory(bool fail) {
    Fixture f;
    auto& child = f.design.createGraph("child");
    const auto in = child.createValue(child.internSymbol("a"), Fixture::type()); child.bindInputPort("a", in);
    const auto op = child.createOperation(OperationKind::kNot);
    const auto value = child.createValue(Fixture::type()); child.addOperand(op, in); child.addResult(op, value);
    const auto live = node::key({"fixture", "Circuit", "child", {"local"}, "live", {"field"}});
    const auto dead = node::key({"fixture", "Circuit", "child", {"local"}, "dead", {}});
    node::write(child, op, {{live}, {{live, 0}}, {live}}); child.bindOutputPort("y", value);
    node::establishPolicy(child);
    auto sourceInventory = std::vector<std::string>{live, dead}; std::sort(sourceInventory.begin(), sourceInventory.end());
    child.setMetadata(node::kInventory, sourceInventory); child.setMetadata(node::kEliminated, std::vector<std::string>{dead});

    auto& middle = f.design.createGraph("middle");
    const auto midIn = middle.createValue(middle.internSymbol("a"), Fixture::type()); middle.bindInputPort("a", midIn);
    const auto left = inventoryInstance(middle, midIn, "child", "left");
    const auto right = inventoryInstance(middle, midIn, "child", "right");
    const auto combine = middle.createOperation(OperationKind::kXor);
    const auto midOut = middle.createValue(Fixture::type());
    middle.addOperand(combine, left); middle.addOperand(combine, right); middle.addResult(combine, midOut);
    middle.bindOutputPort("y", midOut);
    const auto a = f.input("a");
    f.output("nested", inventoryInstance(f.graph, a, "middle", "outer"));
    if (fail) inventoryInstance(f.graph, a, "missing", "bad");
    else f.output("peer", inventoryInstance(f.graph, a, "child", "peer"));
    require(!f.graph.metadata(node::kScopePolicy), "hierarchy top fixture already has a FIRRTL policy");

    Fixture expected;
    const std::array<std::string, 2> leftPath{"outer", "left"}, rightPath{"outer", "right"};
    const std::array<std::string, 1> peerPath{"peer"};
    node::importInventory(child, expected.graph, leftPath); node::importInventory(child, expected.graph, rightPath);
    if (!fail) node::importInventory(child, expected.graph, peerPath);
    std::string error;
    auto pass = tx::makePass("hier-flatten", {}, error); require(bool(pass), error);
    require(run(f.design, std::move(pass), !fail) == !fail, "hierarchy failure fixture did not fail");
    sameInventory(f.graph, expected.graph); node::requirePolicy(f.graph);
    const auto inspection = node::inspect(f.graph);
    require(inspection.errors.empty() && inspection.nodes.size() == (fail ? 2 : 3),
        "flatten did not publish qualified inventory at its return boundary");
    for (const auto& record : inspection.nodes) {
        const auto identity = node::parseKey(record.key);
        require(record.roots.size() == 1 && identity.instancePath.back() == "local" &&
                identity.fieldPath == std::vector<std::string>{"field"},
            "flatten changed the source-local path, field, or root mapping");
    }
    node::recordRetention(f.graph);
    const auto census = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeRetention"));
    require(std::find(census.begin(), census.end(), "missing=0") != census.end() &&
            std::find(census.begin(), census.end(), fail ? "dead=2" : "dead=3") != census.end(),
        "flatten returned before publishing imported tombstones");
    if (!fail) {
        require(f.design.graphs().size() == 1, "flatten kept source graphs instead of testing batch ownership");
        valid(f.design); auto loaded = roundTrip(f.design); valid(loaded);
        auto& restored = *loaded.findGraph("top"); node::recordRetention(restored);
        sameInventory(restored, f.graph);
        require(restored.metadata("firrtl.nodeRetention") == f.graph.metadata("firrtl.nodeRetention"),
            "flattened inventory census changed across JSON");
        run(loaded, std::make_unique<tx::GrhVerifyPass>());
    }
}
}

int main() {
    try { canonicalValidationEquivalence(); identityAndPersistence(); mergeAndRemap(); invalidMetadata(); retentionEquivalence(); executionClones(); orphanLiteralProvenance(); multipleResults(); boundedHints(); deadInventory(); qualificationContext(); keyInterner(); copyQualifiedAcrossGraphs(); inventoryImportBatch(); inventoryImportBoundaries(); flattenedInventory(false); flattenedInventory(true); }
    catch (const std::exception& error) { std::cerr << "[firrtl-node-attributes] " << error.what() << '\n'; return 1; }
    return 0;
}
