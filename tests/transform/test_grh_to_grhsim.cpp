#include "core/grh.hpp"
#include "core/store.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace grh = wolvrix::lib::grh;
namespace transform = wolvrix::lib::transform;
namespace sim = transform::grhsim;

namespace {
using grh::OperationKind;

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

grh::LogicalType logic(int32_t width, std::vector<int64_t> shape = {})
{
    return {grh::ValueType::Logic, width, false, grh::LogicDomain::FourState, std::move(shape)};
}

grh::ValueId input(grh::Graph& graph, std::string_view name, grh::LogicalType type)
{
    const auto value = graph.createValue(graph.internSymbol(name), type);
    graph.bindInputPort(name, value);
    return value;
}

grh::ValueId constant(grh::Graph& graph, grh::LogicalType type, std::string literal)
{
    const auto value = graph.createValue(type);
    const auto op = graph.createOperation(OperationKind::kConstant);
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", std::move(literal));
    return value;
}

grh::OperationId state(grh::Graph& graph, std::string_view name, grh::LogicalType type, bool latch = false)
{
    const auto op = graph.createOperation(latch ? OperationKind::kLatch : OperationKind::kRegister,
                                           graph.internSymbol(name));
    graph.setAttr(op, "width", int64_t{type.width});
    graph.setAttr(op, "isSigned", type.isSigned);
    if (!type.shape.empty()) graph.setAttr(op, "shape", type.shape);
    return op;
}

grh::OperationId operation(grh::Graph& graph, OperationKind kind, std::string_view name,
                           std::initializer_list<grh::ValueId> operands,
                           std::initializer_list<grh::ValueId> results = {})
{
    const auto op = graph.createOperation(kind, graph.internSymbol(name));
    for (const auto value : operands) graph.addOperand(op, value);
    for (const auto value : results) graph.addResult(op, value);
    return op;
}

bool convert(grh::Design& design, sim::GrhToGrhsimOptions options = {}, bool* changed = nullptr)
{
    transform::PassManager manager;
    manager.addPass(std::make_unique<sim::GrhToGrhsimPass>(std::move(options)));
    transform::PassDiagnostics diagnostics;
    const auto result = manager.run(design, diagnostics);
    if (changed) *changed = result.changed;
    if (!result.success)
        for (const auto& entry : diagnostics.messages()) std::cerr << entry.message << '\n';
    return result.success;
}

bool verifies(grh::Design& design)
{
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics diagnostics;
    return manager.run(design, diagnostics).success;
}

std::size_t count(const grh::Graph& graph, OperationKind kind)
{
    std::size_t result = 0;
    for (const auto op : graph.operations()) result += graph.opKind(op) == kind;
    return result;
}

std::string checkpoint(const grh::Design& design)
{
    wolvrix::lib::store::StoreJson serializer;
    const auto json = serializer.storeToString(design);
    require(json.has_value(), "store failed");
    return *json;
}

void testCompleteConversion()
{
    grh::Design design;
    auto& graph = design.createGraph("top");
    design.markAsTop("top");
    const auto clk = input(graph, "clk", logic(1));
    const auto reset = input(graph, "reset", logic(1));
    const auto data = input(graph, "data", logic(8));
    const auto one = constant(graph, logic(1), "1'b1");
    const auto index = constant(graph, logic(2), "2'd1");
    const auto mask8 = constant(graph, logic(8), "8'hff");
    const auto mask16 = constant(graph, logic(16), "16'hffff");
    const auto row = constant(graph, logic(16, {2}), "16'hbbaa");
    const auto unknown = constant(graph, logic(8), "8'b1x0z0011");
    graph.bindOutputPort("projected", unknown);
    const auto q = state(graph, "q", logic(8));
    graph.setAttr(q, "initValue", std::string("8'h01"));
    const auto memory = state(graph, "memory", logic(32, {2, 2}));
    graph.setAttr(memory, "initKind", std::vector<std::string>{"literal", "literal"});
    graph.setAttr(memory, "initValue", std::vector<std::string>{"8'h12", "8'h34"});
    graph.setAttr(memory, "initStart", std::vector<int64_t>{0, 1});
    graph.setAttr(memory, "initLen", std::vector<int64_t>{4, 1});
    const auto latch = state(graph, "latch", logic(8), true);
    const auto readValue = graph.createValue(graph.internSymbol("q_read"), logic(8));
    const auto read = operation(graph, OperationKind::kRegisterReadPort, "read_q", {}, {readValue});
    graph.setAttr(read, "regSymbol", std::string("q"));
    graph.bindOutputPort("q", readValue);
    graph.addDeclaredSymbol(graph.getOperation(q).symbol());
    graph.addDeclaredSymbol(graph.getValue(readValue).symbol());
    const auto group = graph.addGenerateGroup(graph.internSymbol("gen"), graph.lookupSymbol("q"));
    graph.addGenerateGroupSymbol(group, graph.getOperation(q).symbol());

    const std::array kinds{OperationKind::kRegisterWritePort, OperationKind::kRegisterWriteMaskPort,
        OperationKind::kRegisterWriteArrayPort, OperationKind::kRegisterWriteArrayMaskPort,
        OperationKind::kRegisterFillPort, OperationKind::kLatchWritePort};
    const std::array expected{sim::kWrite, sim::kWriteMask, sim::kWriteArray, sim::kWriteArrayMask, sim::kFill, sim::kLatchWrite};
    std::array<grh::OperationId, 6> writers;
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        writers[i] = graph.createOperation(kinds[i], graph.internSymbol("writer" + std::to_string(i)));
        const bool array = i == 2 || i == 3;
        graph.addOperand(writers[i], one);
        if (array) graph.addOperand(writers[i], index);
        graph.addOperand(writers[i], array ? row : data);
        if (i == 1 || i == 3 || i == 5) graph.addOperand(writers[i], array ? mask16 : mask8);
        if (i != 5)
        {
            graph.addOperand(writers[i], clk);
            graph.addOperand(writers[i], reset);
            graph.setAttr(writers[i], "eventEdge", std::vector<std::string>{"posedge", "negedge"});
        }
        graph.setAttr(writers[i], i == 5 ? "latchSymbol" : "regSymbol",
                      std::string(i == 5 ? "latch" : i >= 2 ? "memory" : "q"));
        graph.setAttr(writers[i], "originModules", std::vector<std::string>{"top$registers"});
    }
    const auto taskCondition = constant(graph, logic(32), "32'h2");
    const auto task = operation(graph, OperationKind::kSystemTask, "monitor", {taskCondition, readValue, clk});
    graph.setAttr(task, "name", std::string("monitor"));
    graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(task, "clkPolarity", std::string("posedge"));
    graph.setAttr(task, "procKind", std::string("always_ff"));
    graph.setAttr(task, "hasTiming", true);
    const auto randomValue = graph.createValue(logic(32));
    const auto random = operation(graph, OperationKind::kSystemFunction, "random", {}, {randomValue});
    graph.setAttr(random, "name", std::string("random"));
    graph.setAttr(random, "sampleId", std::string("original-random"));
    const auto random2Value = graph.createValue(logic(32));
    const auto random2 = operation(graph, OperationKind::kSystemFunction, "random2", {}, {random2Value});
    graph.setAttr(random2, "name", std::string("random"));
    const auto pureValue = graph.createValue(logic(8));
    const auto pure = operation(graph, OperationKind::kSystemFunction, "clog2", {data}, {pureValue});
    graph.setAttr(pure, "name", std::string("clog2"));

    const auto imported = operation(graph, OperationKind::kDpicImport, "dpi", {});
    graph.setAttr(imported, "argsName", std::vector<std::string>{"io", "out", "in"});
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"inout", "output", "input"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{16, 32, 8});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false, true});
    graph.setAttr(imported, "argsType", std::vector<std::string>{"shortint", "int", "byte"});
    graph.setAttr(imported, "hasReturn", true);
    graph.setAttr(imported, "returnWidth", int64_t{8});
    graph.setAttr(imported, "returnSigned", false);
    graph.setAttr(imported, "returnType", std::string("byte"));
    const auto returned = graph.createValue(logic(8));
    const auto out = graph.createValue(logic(32));
    const auto inout = graph.createValue(logic(16));
    const auto call = operation(graph, OperationKind::kDpicCall, "call", {one, data, row, clk}, {returned, out, inout});
    // The ABI rejects shaped DPI args. An explicit equal-width view supplies
    // the same bits while leaving the array producer's complete type intact.
    const auto flat = graph.createValue(logic(16));
    const auto view = operation(graph, OperationKind::kAssign, "dpi_view", {row}, {flat});
    graph.replaceOperand(call, 2, flat);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "clkPolarity", std::string("posedge"));

    auto conflicting = design.clone();
    auto& conflictingGraph = *conflicting.findGraph("top");
    conflictingGraph.setAttr(conflictingGraph.findOperation("monitor"), "clkPolarity", std::string("negedge"));
    const auto conflictingStamp = transform::DesignStamp::capture(conflictingGraph);
    require(!convert(conflicting) && conflictingStamp.matches(conflictingGraph),
            "conflicting frontend clock annotation was accepted or partly converted");

    const auto sourceJson = checkpoint(design);
    const auto originalIdentity = design.instanceId();
    const auto oldStateCount = count(graph, OperationKind::kRegister) + count(graph, OperationKind::kLatch);
    require(convert(design), "complete conversion failed");
    require(design.instanceId() == originalIdentity && design.findGraph("top") == &graph,
            "conversion replaced the original graph infrastructure");
    require(graph.opKind(read) == OperationKind::kRegisterReadPort && graph.opResults(read)[0] == readValue,
            "conversion copied a common read/value");
    require(graph.opKind(view) == OperationKind::kAssign && graph.valueShape(row).size() == 1,
            "conversion discarded the array/packed view");
    for (std::size_t i = 0; i < writers.size(); ++i)
    {
        require(graph.opKind(writers[i]) == expected[i] && count(graph, expected[i]) == 1,
                "writer was omitted, copied or converted to the wrong op");
        require(sim::attribute<int64_t>(graph, writers[i], "writerOrder") == int64_t(i), "writer priority changed");
        require(!graph.getOperation(writers[i]).attr("eventEdge"), "writer retained raw events");
        if (i != 5) require(sim::isFire(graph, graph.opOperands(writers[i])[0]), "writer did not get an explicit fire");
    }
    require(count(graph, OperationKind::kRegister) + count(graph, OperationKind::kLatch) == oldStateCount,
            "conversion synthesized monitor/event history declarations");
    require(graph.opKind(task) == sim::kSystemTask && graph.opKind(call) == sim::kDpiCall &&
            graph.opKind(random) == sim::kSystemFunction && graph.opKind(pure) == sim::kSystemFunction,
            "system/DPI operations were not converted");
    const auto normalizedCondition = graph.opOperands(task)[1];
    require(graph.valueWidth(normalizedCondition) == 1 &&
            graph.opKind(graph.valueDef(normalizedCondition)) == OperationKind::kReduceOr &&
            graph.opOperands(graph.valueDef(normalizedCondition))[0] == taskCondition,
            "wide call condition was truncated instead of testing nonzero");
    const auto normalizedInput = graph.opOperands(call)[2];
    require(graph.valueSigned(normalizedInput) && graph.valueWidth(normalizedInput) == 8 &&
            graph.opKind(graph.valueDef(normalizedInput)) == OperationKind::kAssign &&
            graph.opOperands(graph.valueDef(normalizedInput))[0] == data && !graph.valueSigned(data),
            "DPI signedness view changed the original producer or was omitted");
    require(!graph.getOperation(task).attr("clkPolarity") && !graph.getOperation(call).attr("clkPolarity"),
            "converted calls retained redundant raw clock annotations");
    require(sim::attribute<std::string>(graph, random, "sampleId") == "original-random" &&
            sim::attribute<std::string>(graph, random2, "sampleId") != sim::attribute<std::string>(graph, random, "sampleId"),
            "sample identity changed or independent samples were merged");
    require(!grh::canCseOperation(graph, random) && grh::canCseOperation(graph, pure), "function effects were lost");
    for (const auto op : graph.operations())
        require(!graph.getOperation(op).attr(sim::kPartition), "conversion created virtual partitions");
    for (const auto value : graph.values())
        require(graph.valueType(value) != grh::ValueType::Logic || graph.valueDomain(value) == grh::LogicDomain::TwoState,
                "conversion only changed an option instead of projecting the actual types");
    const auto projected = slang::SVInt::fromString(sim::attribute<std::string>(graph, graph.valueDef(unknown), "constValue").value());
    require(!projected.hasUnknown() && projected.getRawPtr()[0] == 0x83, "X/Z projection changed known bits");
    const auto init = grh::stateInitSpec(graph, memory);
    require(init.size() == 3 && init[1].start == 0 && init[1].count == 4 && init[2].start == 1 && init[2].count == 1,
            "initialization override order/ranges were lost");
    require(graph.validateDeclaredSymbols().empty() && graph.generateGroups()[0].symbols[0] == graph.getOperation(q).symbol(),
            "declaration/generate provenance was lost");
    const auto stamp = transform::DesignStamp::capture(graph);
    bool changed = true;
    require(convert(design, {}, &changed) && !changed && stamp.matches(graph), "matching conversion rerun was not a no-op");
    auto restored = grh::Design::fromJsonString(checkpoint(design));
    require(verifies(restored) && convert(restored), "converted checkpoint did not round-trip/restart");
    auto source = grh::Design::fromJsonString(sourceJson);
    auto dryrun = source.clone();
    const auto sourceStamp = transform::DesignStamp::capture(*source.findGraph("top"));
    require(convert(dryrun) && sourceStamp.matches(*source.findGraph("top")) && checkpoint(source) == sourceJson,
            "isolated conversion changed the source design");
    require(count(*source.findGraph("top"), OperationKind::kSystemTask) == 1,
            "base call contract was modified through the clone");
}

void testHistoryInitialization()
{
    grh::Design design;
    auto& graph = design.createGraph("events");
    design.markAsTop("events");
    const auto clk = input(graph, "clk", logic(1));
    const auto one = constant(graph, logic(1), "1'b1");
    const auto q = state(graph, "q", logic(1));
    graph.setAttr(q, "initValue", std::string("1'h1"));
    const auto readValue = graph.createValue(logic(1));
    const auto read = operation(graph, OperationKind::kRegisterReadPort, "read", {}, {readValue});
    graph.setAttr(read, "regSymbol", std::string("q"));
    const auto inverted = graph.createValue(logic(1));
    operation(graph, OperationKind::kNot, "not", {clk}, {inverted});
    const auto unsupported = graph.createValue(logic(1));
    operation(graph, OperationKind::kAdd, "add", {readValue, one}, {unsupported});
    const auto mismatch = state(graph, "mismatch", logic(1));
    graph.setAttr(mismatch, "initValue", std::string("8'h1"));
    const auto mismatchValue = graph.createValue(logic(1));
    const auto mismatchRead = operation(graph, OperationKind::kRegisterReadPort, "mismatch_read", {}, {mismatchValue});
    graph.setAttr(mismatchRead, "regSymbol", std::string("mismatch"));
    const std::array events{clk, inverted, readValue, unsupported, mismatchValue};
    std::array<grh::OperationId, events.size()> tasks;
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        tasks[i] = operation(graph, OperationKind::kSystemTask, "t" + std::to_string(i), {one, events[i]});
        graph.setAttr(tasks[i], "name", std::string("display"));
        graph.setAttr(tasks[i], "eventEdge", std::vector<std::string>{"both"});
    }
    require(convert(design), "history conversion failed");
    const std::array previous{"1'h0", "1'h1", "1'h1", "1'h0", "1'h0"};
    for (std::size_t i = 0; i < tasks.size(); ++i)
    {
        const auto detector = graph.valueDef(graph.opOperands(tasks[i])[0]);
        require(graph.opKind(detector) == sim::kEdgeDetect &&
                sim::attribute<std::string>(graph, detector, "prevInit") == previous[i], "history initialization differs from legacy");
        require(sim::attribute<bool>(graph, detector, "prevInitFallback").value_or(false) == (i >= 3),
                "history fallback policy changed");
    }
}

void testFourStateAndUndriven()
{
    grh::Design design;
    auto& graph = design.createGraph("four");
    design.markAsTop("four");
    const auto value = constant(graph, logic(8, {2}), "8'b1x0z0011");
    graph.bindOutputPort("array", value);
    require(convert(design, {"", grh::LogicDomain::FourState}), "FourState conversion failed");
    const auto bits = slang::SVInt::fromString(sim::attribute<std::string>(graph, graph.valueDef(value), "constValue").value());
    require(bits.hasUnknown() && graph.valueDomain(value) == grh::LogicDomain::FourState,
            "FourState projection silently erased X/Z");
    require(!convert(design), "different conversion policy silently reused a checkpoint");
    grh::Design undriven;
    auto& zero = undriven.createGraph("zero");
    undriven.markAsTop("zero");
    const auto output = zero.createValue(zero.internSymbol("zero_output"), logic(8));
    zero.bindOutputPort("output", output);
    require(convert(undriven) && zero.opKind(zero.valueDef(output)) == OperationKind::kConstant,
            "legacy undriven TwoState zero was not materialized");
}

void testFailuresAndFactory()
{
    for (const auto kind : {OperationKind::kInstance, OperationKind::kBlackbox, OperationKind::kXMRRead})
    {
        grh::Design design;
        auto& graph = design.createGraph("bad");
        design.markAsTop("bad");
        operation(graph, kind, "hierarchy", {});
        const auto stamp = transform::DesignStamp::capture(graph);
        require(!convert(design) && stamp.matches(graph) && !graph.metadata(sim::kStage),
                "unsupported hierarchy was partly converted");
    }
    grh::Design design;
    auto& graph = design.createGraph("bad_event");
    design.markAsTop("bad_event");
    const auto clock = input(graph, "clk", logic(2));
    const auto one = constant(graph, logic(1), "1'h1");
    const auto task = operation(graph, OperationKind::kSystemTask, "task", {one, clock});
    graph.setAttr(task, "name", std::string("display"));
    graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
    const auto stamp = transform::DesignStamp::capture(graph);
    require(!convert(design) && stamp.matches(graph), "malformed event was partly converted");
    std::string error;
    const std::array<std::string_view, 2> args{"-top=bad_event", "-logic-domain=4state"};
    require(transform::makePass("grh-to-grhsim", args, error) != nullptr, "factory registration missing");
    const std::array<std::string_view, 1> bad{"-logic-domain=invalid"};
    require(!transform::makePass("grh-to-grhsim", bad, error) && !error.empty(), "unknown option accepted");
}

void testCallPreflightAndNonLogicState()
{
    for (const auto& [key, value] : std::vector<std::pair<std::string, grh::AttributeValue>>{
             {"procKind", int64_t{2}}, {"procKind", std::string{}}, {"hasTiming", std::string("yes")},
             {"TriggerSpec", std::string("clock")}, {"clkPolarity", std::string("posedge")}})
    {
        grh::Design design;
        auto& graph = design.createGraph("top");
        design.markAsTop("top");
        const auto one = constant(graph, logic(1), "1'h1");
        const auto task = operation(graph, OperationKind::kSystemTask, "task", {one});
        graph.setAttr(task, "name", std::string("display"));
        graph.setAttr(task, key, value);
        const auto stamp = transform::DesignStamp::capture(graph);
        require(!convert(design) && stamp.matches(graph), "bad call attribute was rejected after mutation");
    }
    for (const auto& [key, value] : std::vector<std::pair<std::string, grh::AttributeValue>>{
             {"sampleId", int64_t{2}}, {"sampleScope", std::string{}}, {"hasSideEffects", std::string("yes")}})
    {
        grh::Design design;
        auto& graph = design.createGraph("top");
        design.markAsTop("top");
        const auto result = graph.createValue(logic(32));
        const auto function = operation(graph, OperationKind::kSystemFunction, "random", {}, {result});
        graph.setAttr(function, "name", std::string("random"));
        graph.setAttr(function, key, value);
        const auto stamp = transform::DesignStamp::capture(graph);
        require(!convert(design) && stamp.matches(graph), "bad sample attribute was rejected after mutation");
    }
    grh::Design design;
    auto& graph = design.createGraph("top");
    design.markAsTop("top");
    for (const auto kind : {grh::ValueType::Real, grh::ValueType::String})
    {
        const auto type = grh::LogicalType{kind, 1, false, grh::LogicDomain::FourState, {}};
        const auto name = std::string(grh::toString(kind));
        const auto declaration = state(graph, name, type);
        const auto value = graph.createValue(type);
        const auto read = operation(graph, OperationKind::kRegisterReadPort, name + "_read", {}, {value});
        graph.setAttr(read, "regSymbol", name);
        graph.bindOutputPort(name, value);
        require(grh::stateLogicalType(graph, declaration).kind == grh::ValueType::Logic,
                "fixture should exercise legacy inferred state kind");
    }
    const auto data = constant(graph, logic(8), "8'h2");
    const auto result = graph.createValue(logic(8));
    const auto function = operation(graph, OperationKind::kSystemFunction, "external_clog2", {data}, {result});
    graph.setAttr(function, "name", std::string("clog2"));
    graph.setAttr(function, "hasSideEffects", true);
    require(convert(design) && verifies(design), "legacy non-logic state conversion failed");
    require(!grh::canCseOperation(graph, function) && sim::attribute<std::string>(graph, function, "sampleId"),
            "explicit side effect was discarded by the function whitelist");
    for (const auto kind : {grh::ValueType::Real, grh::ValueType::String})
    {
        const auto declaration = graph.findOperation(grh::toString(kind));
        require(grh::stateLogicalType(graph, declaration).kind == kind, "read-inferred state kind lost");
        require(grh::stateInitSpec(graph, declaration)[0].value == (kind == grh::ValueType::Real ? "0" : ""),
                "non-logic default initialization lost");
    }
}
} // namespace

int main()
{
    try
    {
        testCompleteConversion();
        testHistoryInitialization();
        testFourStateAndUndriven();
        testFailuresAndFactory();
        testCallPreflightAndNonLogicState();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
