#include "core/state.hpp"
#include "core/store.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/sink-schedule.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
namespace grh = wolvrix::lib::grh;
namespace tx = wolvrix::lib::transform;
namespace store = wolvrix::lib::store;

void require(bool success, std::string_view message) {
    if (!success) throw std::runtime_error(std::string(message));
}

void contains(const std::string& text, std::string_view expected) {
    require(text.find(expected) != std::string::npos, "Missing readable text: " + std::string(expected));
}

void run(grh::Design& design, tx::SessionStore& session, std::string_view name,
         std::initializer_list<std::string_view> arguments = {}) {
    tx::PassManagerOptions options;
    options.session = &session;
    tx::PassManager manager(options);
    std::string error;
    auto pass = tx::makePass(name, std::span(arguments.begin(), arguments.size()), error);
    require(bool(pass), error);
    manager.addPass(std::move(pass));
    tx::PassDiagnostics diagnostics;
    const auto result = manager.run(design, diagnostics);
    if (!result.success) {
        std::string messages;
        for (const auto& item : diagnostics.messages()) messages += item.message + '\n';
        throw std::runtime_error(messages);
    }
}

void testReadableSnapshot() {
    grh::Design design;
    auto& graph = design.createGraph("CoreTop");
    design.markAsTop("CoreTop");
    const auto logical = [](int width, std::vector<int64_t> shape = {}) {
        return grh::LogicalType{grh::ValueType::Logic, width, false, grh::LogicDomain::TwoState, std::move(shape)};
    };
    const auto input = [&](std::string_view name, int width) {
        const auto value = graph.createValue(graph.internSymbol(name), logical(width));
        graph.bindInputPort(name, value);
        return value;
    };
    const auto clock = input("core$clock", 1), enable = input("core$enable", 1);
    const auto data = input("core$alu$operand", 8);
    const auto state = [&](std::string_view name, grh::LogicalType type) {
        const auto op = graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol(name));
        grh::setStateLogicalType(graph, op, type);
        grh::setStateInitSpec(graph, op, {{grh::InitKind::Constant, "0"}});
        graph.setAttr(op, "originModules", std::vector<std::string>{"CoreTop$core$alu"});
        return op;
    };
    const auto q = state("core$alu$accumulator", logical(8));
    const auto peer = state("core$alu$peer", logical(8));
    const auto memory = state("core$memory$words", logical(32, {4}));
    const auto readOp = graph.createOperation(grh::OperationKind::kRegisterReadPort);
    graph.setAttr(readOp, "regSymbol", std::string("core$alu$accumulator"));
    const auto current = graph.createValue(graph.internSymbol("core$alu$current"), logical(8));
    graph.addResult(readOp, current);
    const auto add = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("core$alu$sum"));
    graph.addOperand(add, current); graph.addOperand(add, data);
    const auto result = graph.createValue(graph.internSymbol("core$alu$next"), logical(8));
    graph.addResult(add, result);
    graph.setAttr(add, "originModules", std::vector<std::string>{"CoreTop$core$alu", "CoreTop$core$shared"});
    graph.setOpSrcLoc(add, {"rtl/alu.sv", 42, 7, 42, 25, "ingest", {}, "accumulate\nnext"});
    graph.setValueSrcLoc(result, {"rtl/alu.sv", 42, 7});
    const auto task = graph.createOperation(grh::OperationKind::kSystemTask);
    graph.setAttr(task, "name", std::string("display"));
    graph.setAttr(task, "eventEdge", std::vector<std::string>{});
    graph.addOperand(task, enable); graph.addOperand(task, result);
    const auto writer = graph.createOperation(grh::OperationKind::kRegisterWritePort);
    graph.setAttr(writer, "regSymbol", std::string("core$alu$accumulator"));
    graph.setAttr(writer, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(writer, enable); graph.addOperand(writer, result); graph.addOperand(writer, clock);
    const auto peerWriter = graph.createOperation(grh::OperationKind::kRegisterWritePort);
    graph.setAttr(peerWriter, "regSymbol", std::string("core$alu$peer"));
    graph.setAttr(peerWriter, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(peerWriter, enable); graph.addOperand(peerWriter, result); graph.addOperand(peerWriter, clock);
    const auto fill = graph.createOperation(grh::OperationKind::kRegisterFillPort);
    graph.setAttr(fill, "regSymbol", std::string("core$memory$words"));
    graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});
    graph.addOperand(fill, enable); graph.addOperand(fill, data); graph.addOperand(fill, clock);
    graph.bindOutputPort("accumulator", current);

    store::StoreText early;
    const auto initial = early.storeToString(design);
    require(initial.has_value(), "initial GRH export failed");
    contains(*initial, "six-phase-model:");
    contains(*initial, "shape=[4] leaf-width=8");
    contains(*initial, "rtl/alu.sv\":42:7");
    contains(*initial, "accumulate\\nnext");
    contains(*initial, "source: unavailable");

    tx::SessionStore session;
    run(design, session, "grh-to-grhsim");
    run(design, session, "grhsim-split-event-and-output");
    run(design, session, "grhsim-split-nonsink-and-sink", {"-mem-min-bytes=4"});
    run(design, session, "grhsim-partition-nonsink", {"-max-node-work=1", "-max-supernode-work=4", "-clone-shared=0"});
    run(design, session, "grhsim-partition-sink", {"-enable-guard-min-size=2"});
    store::StoreText readable(nullptr, &session);
    const auto snapshot = readable.storeToString(design);
    require(snapshot.has_value(), "partitioned export failed");
    contains(*snapshot, "status: validated complete=true");
    contains(*snapshot, "logical-partition-tree:");
    contains(*snapshot, "execution-owners:");
    contains(*snapshot, "requires-logical-leaf");
    contains(*snapshot, "hoisted-enable");
    contains(*snapshot, "logical-owner=[\"general\", \"nonsink\",");
    contains(*snapshot, "origin-set[");
    contains(*snapshot, "CoreTop$core$shared");
    contains(*snapshot, "kind=grhsim.write");
    contains(*snapshot, "kind=grhsim.fill");
    contains(*snapshot, "operand[0] fire = v");
    contains(*snapshot, "operand[1] enable = v");
    contains(*snapshot, "operand[2] data = v");
    contains(*snapshot, "target-state: op");
    contains(*snapshot, "writerOrder=");
    contains(*snapshot, "target-write-rank=");
    contains(*snapshot, "large-commit-runs");
    require(snapshot == readable.storeToString(design), "repeated export changed a snapshot");

    store::StoreOptions options;
    options.outputDir = WOLF_SV_EMIT_ARTIFACT_DIR;
    options.outputFilename = "grhsim-readable.txt";
    const auto saved = readable.store(design, options);
    require(saved.success && saved.artifacts.size() == 1, "readable file export failed");
    std::ifstream file(saved.artifacts.front());
    const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    require(content == *snapshot, "streaming and string exports disagree");
    const auto noSession = early.storeToString(design);
    require(noSession.has_value(), "missing session should not block inspection");
    contains(*noSession, "status: unavailable");
    graph.setMetadata("test.invalidate", true);
    const auto stale = readable.storeToString(design);
    require(stale.has_value(), "stale session should not block graph inspection");
    contains(*stale, "Missing or stale sink schedule");
    require(stale->find("status: validated") == std::string::npos, "stale schedule exported as valid");

    grh::Design noTop;
    noTop.createGraph("unmarked");
    store::StoreDiagnostics diagnostics;
    store::StoreText invalid(&diagnostics);
    require(!invalid.storeToString(noTop) && diagnostics.hasError(), "missing top should fail");
    (void)q; (void)peer; (void)memory;
}
}

int main() {
    try { testReadableSnapshot(); }
    catch (const std::exception& error) { std::cerr << "[store-text] " << error.what() << '\n'; return 1; }
    return 0;
}
