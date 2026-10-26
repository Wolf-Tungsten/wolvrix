#ifndef WOLVRIX_TEST_GRHSIM_PARTITION_FIXTURE_HPP
#define WOLVRIX_TEST_GRHSIM_PARTITION_FIXTURE_HPP

#include "core/store.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/partition-nonsink.hpp"
#include "transform/grhsim/partition-sink.hpp"
#include "transform/grhsim/simplify.hpp"
#include "transform/grhsim/split-event-and-output.hpp"
#include "transform/grhsim/split-nonsink-and-sink.hpp"
#include "transform/verify.hpp"
#include "grhsim_oracle.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>

namespace partition_test {
namespace grh = wolvrix::lib::grh;
namespace tx = wolvrix::lib::transform;
namespace sim = tx::grhsim;
using grh::OperationKind;
using Owner = std::vector<std::string>;

inline void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
inline void rejects(const std::function<void()>& action, std::string_view message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
inline std::string json(const grh::Design& design) {
    wolvrix::lib::store::StoreJson store;
    const auto result = store.storeToString(design);
    require(result.has_value(), "Could not store checkpoint"); return *result;
}
inline std::filesystem::path artifact(std::string_view name) {
    const std::filesystem::path root(WOLVRIX_GRHSIM_PARTITION_ARTIFACT_DIR);
    std::filesystem::create_directories(root);
    return root / name;
}
inline void checkpoint(const grh::Design& design, std::string_view name) {
    std::ofstream output(artifact(std::string(name) + ".json"));
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << json(design);
}
inline Owner owner(const grh::Graph& graph, grh::OperationId op) {
    return sim::attribute<Owner>(graph, op, sim::kPartition).value_or(Owner{});
}
inline bool run(grh::Design& design, std::string_view name, tx::SessionStore& session,
                 std::initializer_list<std::string_view> args = {}, bool expected = true) {
    tx::PassManagerOptions options; options.session = &session;
    tx::PassManager manager(options); std::string error;
    auto pass = tx::makePass(name, std::span(args.begin(), args.size()), error);
    require(bool(pass), "Factory rejected " + std::string(name) + ": " + error);
    manager.addPass(std::move(pass)); manager.addPass(std::make_unique<tx::GrhVerifyPass>());
    tx::PassDiagnostics diagnostics;
    const auto status = manager.run(design, diagnostics);
    if (expected && !status.success) {
        std::string text;
        for (const auto& entry : diagnostics.messages()) text += entry.message + "\n";
        throw std::runtime_error(std::string(name) + ": " + text);
    }
    return status.success;
}

struct Fixture {
    grh::Design design;
    grh::Graph& graph;
    tx::SessionStore session;
    Fixture() : graph(design.createGraph("top")) { design.markAsTop("top"); }
    static grh::LogicalType type(int32_t width, std::vector<int64_t> shape = {}) {
        return {grh::ValueType::Logic, width, false, grh::LogicDomain::TwoState, std::move(shape)};
    }
    grh::ValueId input(std::string_view name, int32_t width = 1) {
        const auto value = graph.createValue(graph.internSymbol(name), type(width)); graph.bindInputPort(name, value); return value;
    }
    grh::ValueId constant(int32_t width, std::string text, std::vector<int64_t> shape = {}) {
        const auto value = compute(OperationKind::kConstant, type(width, std::move(shape)), {});
        graph.setAttr(graph.valueDef(value), "constValue", std::move(text)); return value;
    }
    grh::ValueId compute(OperationKind kind, const grh::LogicalType& type, std::initializer_list<grh::ValueId> args) {
        const auto op = graph.createOperation(kind);
        const auto value = graph.createValue(type);
        for (const auto input : args) graph.addOperand(op, input);
        graph.addResult(op, value); return value;
    }
    grh::OperationId state(std::string_view name, int32_t width, std::vector<int64_t> shape = {}) {
        const auto op = graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
        grh::setStateLogicalType(graph, op, type(width, std::move(shape)));
        grh::setStateInitSpec(graph, op, {{grh::InitKind::Constant, "0"}});
        graph.addDeclaredSymbol(graph.operationSymbol(op));
        graph.setAttr(op, "originModules", std::vector<std::string>{"top$state"}); return op;
    }
    grh::ValueId read(grh::OperationId state, std::initializer_list<grh::ValueId> indices = {}) {
        auto logical = grh::stateLogicalType(graph, state);
        const bool latch = graph.opKind(state) == OperationKind::kLatch;
        const auto op = graph.createOperation(latch ? OperationKind::kLatchReadPort :
            indices.size() ? OperationKind::kRegisterReadArrayPort : OperationKind::kRegisterReadPort);
        for (const auto value : indices) graph.addOperand(op, value);
        for (std::size_t i = 0; i < indices.size(); ++i) logical.width /= logical.shape[i];
        logical.shape.erase(logical.shape.begin(), logical.shape.begin() + indices.size());
        const auto value = graph.createValue(logical); graph.addResult(op, value);
        graph.setAttr(op, latch ? "latchSymbol" : "regSymbol", std::string(graph.symbolText(graph.operationSymbol(state))));
        graph.setAttr(op, "originModules", std::vector<std::string>{"top$read"}); return value;
    }
    grh::OperationId write(OperationKind kind, grh::OperationId state, std::initializer_list<grh::ValueId> args,
                            std::vector<std::string> edges = {"posedge"}) {
        const auto op = graph.createOperation(kind);
        for (const auto value : args) graph.addOperand(op, value);
        const bool latch = kind == OperationKind::kLatchWritePort;
        graph.setAttr(op, latch ? "latchSymbol" : "regSymbol", std::string(graph.symbolText(graph.operationSymbol(state))));
        if (!latch) graph.setAttr(op, "eventEdge", std::move(edges));
        return op;
    }
    grh::OperationId task(std::string name, std::initializer_list<grh::ValueId> args,
                           std::vector<std::string> edges = {}) {
        const auto op = graph.createOperation(OperationKind::kSystemTask);
        for (const auto value : args) graph.addOperand(op, value);
        graph.setAttr(op, "name", std::move(name)); graph.setAttr(op, "eventEdge", std::move(edges)); return op;
    }
    void convert() { run(design, "grh-to-grhsim", session); run(design, "grhsim-simplify", session); }
    void split() { run(design, "grhsim-split-event-and-output", session); run(design, "grhsim-split-nonsink-and-sink", session); }
    void partition() { run(design, "grhsim-partition-nonsink", session); run(design, "grhsim-partition-sink", session); }
};

inline void compare(const grh::Design& reference, const grh::Design& candidate,
                    const std::vector<std::string>& probes, const std::vector<std::map<std::string, uint64_t>>& inputs,
                    std::string_view label, const sim::SinkSchedule* schedule = nullptr) {
    grhsim_test::Oracle before(*reference.findGraph("top"), probes);
    grhsim_test::Oracle after(*candidate.findGraph("top"), probes, schedule);
    checkpoint(reference, std::string(label) + "-reference");
    checkpoint(candidate, std::string(label) + "-candidate");
    std::ofstream observations(artifact(std::string(label) + ".log"));
    observations.exceptions(std::ios::badbit | std::ios::failbit);
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        before.init(); after.init();
        for (std::size_t step = 0; step < inputs.size(); ++step) {
            const auto expected = before.eval(inputs[step]), actual = after.eval(inputs[step]);
            observations << "epoch=" << epoch << " step=" << step;
            for (const auto& [name, value] : inputs[step]) observations << " input/" << name << '=' << value;
            observations << '\n';
            const auto record = [&](std::string_view side, const auto& observation) {
                for (const auto& [name, value] : observation.outputs) observations << side << " output/" << name << '=' << value << '\n';
                for (const auto& [name, value] : observation.states) observations << side << " state/" << name << '=' << value << '\n';
                for (const auto& round : observation.rounds) {
                    observations << side << " round";
                    for (const auto& [name, value] : round) observations << ' ' << name << '=' << value;
                    observations << '\n';
                }
                for (const auto& effect : observation.effects) observations << side << " effect " << effect << '\n';
            };
            record("reference", expected); record("candidate", actual);
            if (!(expected == actual)) {
                std::string detail = std::string(label) + " epoch=" + std::to_string(epoch) + " step=" + std::to_string(step);
                for (const auto& [name, value] : expected.outputs) detail += "\noutput " + name + " expected=" + value + " actual=" + actual.outputs.at(name);
                for (const auto& [name, value] : expected.states) detail += "\nstate " + name + " expected=" + value + " actual=" + actual.states.at(name);
                detail += "\neffects expected=";
                for (const auto& effect : expected.effects) detail += effect + ";";
                detail += " actual=";
                for (const auto& effect : actual.effects) detail += effect + ";";
                throw std::runtime_error(detail);
            }
        }
    }
}

inline std::size_t count(const grh::Graph& graph, OperationKind kind, const Owner& path = {}) {
    std::size_t count = 0;
    for (const auto op : graph.operations()) if (graph.opKind(op) == kind && (path.empty() || owner(graph, op) == path)) ++count;
    return count;
}
}
#endif
