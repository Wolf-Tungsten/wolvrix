#include "node_test_fixture.hpp"

#include <array>

using namespace node_test;

namespace {
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
    rejects([&] { node::mergeCseOperations(f.graph, op1, op2); }, "primitive CSE crossed node scope");
    rejects([&] { node::unite(node::read(f.graph, op1), node::read(f.graph, op2),
                             node::MergeReason::SameComputation); }, "unproven member union was allowed");
    const auto remapped = node::remapResults(node::read(f.graph, op2), std::array<int64_t, 1>{1});
    require(remapped.roots[0].result == 1, "result remap lost a nonzero result index");
    rejects([&] { node::remapResults(remapped, std::array<int64_t, 1>{0}); }, "unmapped root was dropped");
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
        require(!node::inspect(graph).errors.empty(), "corrupt node attrs were accepted");
        require(!run(copy, std::make_unique<tx::GrhVerifyPass>(), false), "GRH verifier accepted corrupt node attrs");
    };
    check([](auto& graph, auto op) { graph.eraseMetadata(node::kSchema); });
    check([](auto& graph, auto op) { graph.setMetadata(node::kSchema, int64_t{2}); });
    check([](auto& graph, auto op) { graph.setMetadata(node::kSchema, std::string("1")); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, int64_t{1}); });
    check([](auto& graph, auto op) { graph.eraseAttr(op, node::kRootResults); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{-1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kRootResults, std::vector<int64_t>{1}); });
    check([](auto& graph, auto op) { graph.setAttr(op, "firrtl.node.unknown", true); });
    check([](auto& graph, auto op) { graph.setAttr(op, node::kMembers, std::vector<std::string>{"invalid"}); });
    check([](auto& graph, auto op) {
        graph.eraseAttr(op, node::kRoots); graph.eraseAttr(op, node::kRootResults);
    });
    check([](auto& graph, auto op) {
        auto attrs = node::read(graph, op);
        const auto duplicate = graph.createOperation(OperationKind::kNot);
        const auto input = graph.opOperands(op)[0];
        const auto output = graph.createValue(Fixture::type());
        graph.addOperand(duplicate, input); graph.addResult(duplicate, output);
        node::write(graph, duplicate, std::move(attrs));
    });
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
}

int main() {
    try { identityAndPersistence(); mergeAndRemap(); invalidMetadata(); multipleResults(); }
    catch (const std::exception& error) { std::cerr << "[firrtl-node-attributes] " << error.what() << '\n'; return 1; }
    return 0;
}
