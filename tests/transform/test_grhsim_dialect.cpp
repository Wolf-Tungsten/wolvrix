#include "core/store.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include <iostream>
#include <stdexcept>

namespace grh = wolvrix::lib::grh;
namespace sim = wolvrix::lib::transform::grhsim;
namespace transform = wolvrix::lib::transform;

namespace {
void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

bool verifies(grh::Design& design)
{
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics diagnostics;
    return manager.run(design, diagnostics).success;
}

grh::ValueId constant(grh::Graph& graph, const grh::LogicalType& type, std::string literal)
{
    const auto value = graph.createValue(type);
    const auto op = graph.createOperation(grh::OperationKind::kConstant);
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", std::move(literal));
    return value;
}

grh::ValueId detector(grh::Graph& graph, grh::ValueId input)
{
    const auto value = graph.createValue({grh::ValueType::Logic, 1, false, grh::LogicDomain::TwoState, {}});
    const auto op = graph.createOperation(sim::kEdgeDetect);
    graph.addOperand(op, input);
    graph.addResult(op, value);
    graph.setAttr(op, "edge", std::string("posedge"));
    graph.setAttr(op, "prevInit", std::string("1'h0"));
    graph.setAttr(op, "sampleScope", std::string("event-round"));
    graph.setAttr(op, "historyUpdate", std::string("unconditional"));
    return value;
}

void testContractsAndCheckpoint()
{
    grh::Design design;
    auto& graph = design.createGraph("top");
    design.markAsTop("top");
    const grh::LogicalType bit{grh::ValueType::Logic, 1, false, grh::LogicDomain::TwoState, {}};
    const grh::LogicalType byte{grh::ValueType::Logic, 8, false, grh::LogicDomain::TwoState, {}};
    auto memory = byte;
    memory.width = 16;
    memory.shape = {2};
    const auto state = graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol("memory"));
    grh::setStateLogicalType(graph, state, memory);
    const grh::InitSpec init{{grh::InitKind::Fill, "8'h00"},
                            {grh::InitKind::Fill, "8'haa", {}, {}, 1, 1},
                            {grh::InitKind::Fill, "8'hff", {}, {}, 0, 0}};
    grh::setStateInitSpec(graph, state, init);
    require(grh::stateInitSpec(graph, state) == init, "typed InitSpec lost order/ranges");
    const auto clk = graph.createValue(graph.internSymbol("clk"), bit);
    graph.bindInputPort("clk", clk);
    const auto fire = detector(graph, clk);
    const auto one = constant(graph, bit, "1'h1");
    const auto data = constant(graph, byte, "8'h42");
    const auto index = constant(graph, bit, "1'h0");
    const auto write = graph.createOperation(sim::kWriteArray);
    for (const auto operand : {fire, one, index, data}) graph.addOperand(write, operand);
    graph.setAttr(write, "regSymbol", std::string("memory"));
    graph.setAttr(write, "writerOrder", int64_t{0});
    const auto task = graph.createOperation(sim::kSystemTask);
    for (const auto operand : {fire, one, data}) graph.addOperand(task, operand);
    graph.setAttr(task, "name", std::string("display"));
    graph.setAttr(task, "procKind", std::string("always_ff"));
    graph.setAttr(task, "hasTiming", true);
    graph.setAttr(task, "eventDriven", true);
    const auto function = graph.createOperation(sim::kSystemFunction);
    const auto random = graph.createValue({grh::ValueType::Logic, 32, true, grh::LogicDomain::TwoState, {}});
    graph.addResult(function, random);
    graph.bindOutputPort("random", random);
    graph.setAttr(function, "name", std::string("random"));
    graph.setAttr(function, "effect", std::string("sample"));
    graph.setAttr(function, "sampleId", std::string("sample/random"));
    graph.setAttr(function, "sampleScope", std::string("eval"));
    graph.setMetadata(sim::kStage, std::string("converted"));
    require(verifies(design), "legal dialect graph failed verification");
    require(!grh::canCseOperation(graph, function) && !grh::canCloneOperation(graph, function),
            "random treated as pure");
    require(!grh::canCseOperation(graph, graph.valueDef(fire)), "edge history treated as pure CSE");
    graph.replaceOperand(write, 0, clk);
    require(!verifies(design), "raw clock accepted in the fire position");
    graph.replaceOperand(write, 0, fire);
    graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
    require(!verifies(design), "raw event annotation accepted on a new writer");
    graph.eraseAttr(write, "eventEdge");
    graph.setAttr(write, sim::kPartition, std::vector<std::string>{"general"});
    require(!verifies(design), "partition accepted in converted graph");
    graph.eraseAttr(write, sim::kPartition);
    require(verifies(design), "failed to restore valid graph");

    for (const auto mode : {wolvrix::lib::store::JsonPrintMode::Compact,
                            wolvrix::lib::store::JsonPrintMode::PrettyCompact})
    {
        wolvrix::lib::store::StoreJson serializer;
        wolvrix::lib::store::StoreOptions options;
        options.jsonMode = mode;
        const auto json = serializer.storeToString(design, options);
        require(json.has_value(), "dialect checkpoint store failed");
        auto loaded = grh::Design::fromJsonString(*json);
        require(verifies(loaded), "dialect checkpoint verify failed");
        auto& restored = *loaded.findGraph("top");
        require(grh::stateInitSpec(restored, restored.findOperation("memory")) == init,
                "checkpoint lost typed initialization");
        require(restored.metadata(sim::kStage) == graph.metadata(sim::kStage), "checkpoint lost stage");
        auto invalid = *json;
        const auto kind = invalid.find("grhsim.writeArray");
        invalid.replace(kind, std::string("grhsim.writeArray").size(), "unregistered.write");
        bool rejected = false;
        try { grh::Design::fromJsonString(invalid); } catch (const std::exception&) { rejected = true; }
        require(rejected, "unknown dialect op accepted");
    }
    require(grh::parseOperationKind("grhsim.bitSelect") == sim::kBitSelect, "dialect registration missing");
    require(!grh::parseOperationKind("kBitSelect"), "bitSelect leaked into base op set");
}

void testDpiContract()
{
    grh::Design design;
    auto& graph = design.createGraph("top");
    const grh::LogicalType bit{grh::ValueType::Logic, 1, false, grh::LogicDomain::TwoState, {}};
    const grh::LogicalType byte{grh::ValueType::Logic, 8, false, grh::LogicDomain::TwoState, {}};
    const auto imported = graph.createOperation(grh::OperationKind::kDpicImport, graph.internSymbol("dpi"));
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input", "output", "inout"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{8, 8, 8});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false, false});
    graph.setAttr(imported, "argsName", std::vector<std::string>{"a", "b", "c"});
    graph.setAttr(imported, "hasReturn", true);
    graph.setAttr(imported, "returnWidth", int64_t{8});
    const auto one = constant(graph, bit, "1'h1");
    const auto data = constant(graph, byte, "8'h42");
    const auto call = graph.createOperation(sim::kDpiCall);
    graph.setAttr(call, "eventDriven", false);
    for (const auto value : {one, one, data, data}) graph.addOperand(call, value);
    for (int i = 0; i < 3; ++i) graph.addResult(call, graph.createValue(byte));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
    graph.setAttr(call, "procKind", std::string("always_comb"));
    graph.setAttr(call, "hasTiming", false);
    require(verifies(design), "DPI input/output/inout/return ABI rejected");
    graph.eraseResult(call, 2);
    require(!verifies(design), "missing DPI inout result accepted");
}

void testDpiTypeNames()
{
    for (const auto name : {"logic", "bit", "byte", "shortint", "int", "longint", "integer", "time"})
        require(sim::parseDpiValueType(name) == grh::ValueType::Logic, "SV integral DPI type rejected");
    for (const auto name : {"real", "shortreal", "realtime"})
        require(sim::parseDpiValueType(name) == grh::ValueType::Real, "SV real DPI type rejected");
    require(sim::parseDpiValueType("string") == grh::ValueType::String, "DPI string type rejected");
    require(!sim::parseDpiValueType("unknown") && !sim::parseDpiValueType("chandle"),
            "unsupported DPI type accepted");

    grh::Design design;
    auto& graph = design.createGraph("top");
    const grh::LogicalType bit{grh::ValueType::Logic, 1, false, grh::LogicDomain::TwoState, {}};
    const grh::LogicalType word{grh::ValueType::Logic, 64, true, grh::LogicDomain::TwoState, {}};
    const auto imported = graph.createOperation(grh::OperationKind::kDpicImport, graph.internSymbol("dpi"));
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(imported, "argsName", std::vector<std::string>{"a"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{64});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{true});
    graph.setAttr(imported, "argsType", std::vector<std::string>{"longint"});
    graph.setAttr(imported, "hasReturn", true);
    graph.setAttr(imported, "returnType", std::string("longint"));
    graph.setAttr(imported, "returnWidth", int64_t{64});
    graph.setAttr(imported, "returnSigned", true);
    const auto call = graph.createOperation(sim::kDpiCall);
    const auto one = constant(graph, bit, "1'h1");
    const auto argument = constant(graph, word, "64'hfedcba9876543210");
    for (const auto value : {one, one, argument}) graph.addOperand(call, value);
    graph.addResult(call, graph.createValue(word));
    graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
    graph.setAttr(call, "eventDriven", false);
    graph.setAttr(call, "procKind", std::string("always_comb"));
    graph.setAttr(call, "hasTiming", false);
    require(verifies(design), "longint argument/return rejected");
    graph.setAttr(imported, "argsType", std::vector<std::string>{"unknown"});
    require(!verifies(design), "unknown DPI metadata type accepted");
    graph.setAttr(imported, "argsType", std::vector<std::string>{"longint"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{32});
    require(!verifies(design), "DPI alias bypassed width checking");
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{64});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false});
    require(!verifies(design), "DPI alias bypassed signedness checking");
    graph.setAttr(imported, "argsSigned", std::vector<bool>{true});
    auto shaped = word; shaped.shape = {2};
    graph.setValueLogicalType(argument, shaped);
    require(!verifies(design), "DPI alias bypassed shape checking");
}
}

int main()
{
    try { testContractsAndCheckpoint(); testDpiContract(); testDpiTypeNames(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
