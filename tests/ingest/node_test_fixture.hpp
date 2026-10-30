#ifndef WOLVRIX_TEST_NODE_FIXTURE_HPP
#define WOLVRIX_TEST_NODE_FIXTURE_HPP

#include "core/grh.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "ingest/node_attributes.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace node_test {
namespace grh = wolvrix::lib::grh;
namespace tx = wolvrix::lib::transform;
namespace node = wolvrix::lib::ingest::node;
using grh::OperationKind;

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
inline void rejects(const std::function<void()>& action, std::string_view message) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, message);
}
inline std::string key(std::string name, std::vector<std::string> field = {}) {
    return node::key({"fixture", "Circuit", "top", {}, std::move(name), std::move(field)});
}
inline bool run(grh::Design& design, std::unique_ptr<tx::Pass> pass, bool expected = true) {
    tx::PassManager manager; manager.addPass(std::move(pass));
    tx::PassDiagnostics diagnostics;
    const auto result = manager.run(design, diagnostics);
    if (expected && !result.success) {
        std::string text;
        for (const auto& diagnostic : diagnostics.messages()) text += diagnostic.message + '\n';
        throw std::runtime_error(text);
    }
    return result.success;
}
inline grh::Design roundTrip(const grh::Design& design) {
    wolvrix::lib::store::StoreJson store;
    const auto text = store.storeToString(design);
    require(text.has_value(), "node checkpoint store failed");
    return grh::Design::fromJsonString(*text);
}
inline void valid(const grh::Design& design) {
    for (const auto& [name, graph] : design.graphs()) {
        const auto inspection = node::inspect(*graph);
        require(inspection.errors.empty(), inspection.report(*graph));
    }
}
inline std::size_t count(const grh::Graph& graph, OperationKind kind) {
    return std::count_if(graph.operations().begin(), graph.operations().end(),
                         [&](auto op) { return graph.opKind(op) == kind; });
}
struct Fixture {
    grh::Design design;
    grh::Graph& graph = design.createGraph("top");
    Fixture() { design.markAsTop("top"); }
    static grh::LogicalType type(int32_t width = 8, std::vector<int64_t> shape = {}) {
        return {grh::ValueType::Logic, width, false, grh::LogicDomain::TwoState, std::move(shape)};
    }
    grh::ValueId input(std::string name, int32_t width = 8) {
        const auto value = graph.createValue(graph.internSymbol(name), type(width));
        graph.bindInputPort(name, value); return value;
    }
    grh::ValueId compute(OperationKind kind, std::initializer_list<grh::ValueId> args,
                         std::string identity = {}, bool root = false, int32_t width = 8) {
        const auto op = graph.createOperation(kind);
        const auto value = graph.createValue(type(width));
        for (const auto input : args) graph.addOperand(op, input);
        graph.addResult(op, value);
        if (!identity.empty()) tag(op, std::move(identity), root);
        return value;
    }
    grh::ValueId constant(std::string literal, std::string identity = {}, int32_t width = 8) {
        const auto value = compute(OperationKind::kConstant, {}, identity, !identity.empty(), width);
        graph.setAttr(graph.valueDef(value), "constValue", std::move(literal)); return value;
    }
    void tag(grh::OperationId op, std::string identity, bool root) {
        node::Attributes attrs;
        attrs.members = {identity}; attrs.origins = {identity};
        if (root) attrs.roots.push_back({identity, 0});
        node::write(graph, op, std::move(attrs));
    }
    void output(std::string name, grh::ValueId value) { graph.bindOutputPort(name, value); }
};
}
#endif
