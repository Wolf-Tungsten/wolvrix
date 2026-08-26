#include "emit/verilator_repcut_package.hpp"
#include "core/grh.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[emit_verilator_repcut_package] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    bool contains(std::string_view text, std::string_view needle)
    {
        return text.find(needle) != std::string_view::npos;
    }

    std::size_t countOccurrences(std::string_view text, std::string_view needle)
    {
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = text.find(needle, position)) != std::string_view::npos)
        {
            ++count;
            position += needle.size();
        }
        return count;
    }

    std::string diagnosticsSummary(const EmitDiagnostics &diagnostics)
    {
        std::string summary;
        for (const auto &diag : diagnostics.messages())
        {
            if (!summary.empty())
            {
                summary += " | ";
            }
            summary += diag.message;
            if (!diag.context.empty())
            {
                summary += " [";
                summary += diag.context;
                summary += "]";
            }
        }
        return summary;
    }

    Graph &buildUnitGraph(Design &design,
                          std::string_view name,
                          std::vector<std::pair<std::string, int32_t>> inputs,
                          std::vector<std::pair<std::string, int32_t>> outputs)
    {
        Graph &graph = design.createGraph(std::string(name));
        for (const auto &[portName, width] : inputs)
        {
            const auto value = graph.createValue(graph.internSymbol(portName), width, false);
            graph.bindInputPort(portName, value);
        }
        for (const auto &[portName, width] : outputs)
        {
            const auto value = graph.createValue(graph.internSymbol(portName), width, false);
            graph.bindOutputPort(portName, value);
        }
        return graph;
    }

    void addInstance(Graph &graph,
                     std::string_view instanceName,
                     std::string_view moduleName,
                     std::vector<ValueId> operands,
                     std::vector<ValueId> results,
                     std::vector<std::string> inputPortNames,
                     std::vector<std::string> outputPortNames)
    {
        const auto op = graph.createOperation(OperationKind::kInstance, graph.internSymbol(std::string(instanceName)));
        graph.setAttr(op, "instanceName", std::string(instanceName));
        graph.setAttr(op, "moduleName", std::string(moduleName));
        graph.setAttr(op, "inputPortName", std::move(inputPortNames));
        graph.setAttr(op, "outputPortName", std::move(outputPortNames));
        graph.setAttr(op, "inoutPortName", std::vector<std::string>{});
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        for (const auto result : results)
        {
            graph.addResult(op, result);
        }
    }

    Design buildDesign()
    {
        Design design;
        Graph &effect = design.createGraph("SimTop_effect_part");
        const auto effectClock = effect.createValue(effect.internSymbol("clock"), 1, false);
        const auto effectReset = effect.createValue(effect.internSymbol("reset"), 1, false);
        const auto effectInput = effect.createValue(effect.internSymbol("in__data"), 8, false);
        const auto effectOutput = effect.createValue(effect.internSymbol("effect__out"), 8, false);
        const auto effectDpiResult = effect.createValue(effect.internSymbol("effect_dpi_result"), 8, false);
        const auto stateOutput = effect.createValue(effect.internSymbol("state__out"), 8, false);
        const auto effectCond = effect.createValue(effect.internSymbol("effect_cond"), 1, false);
        effect.bindInputPort("clock", effectClock);
        effect.bindInputPort("reset", effectReset);
        effect.bindInputPort("in__data", effectInput);
        effect.bindOutputPort("effect__out", effectOutput);
        effect.bindOutputPort("state__out", stateOutput);
        const auto condConst =
            effect.createOperation(OperationKind::kConstant, effect.internSymbol("const_true"));
        effect.setAttr(condConst, "constValue", std::string("1'b1"));
        effect.addResult(condConst, effectCond);
        const auto dpiImport =
            effect.createOperation(OperationKind::kDpicImport, effect.internSymbol("dpi_func"));
        effect.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "output"});
        effect.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8});
        effect.setAttr(dpiImport, "argsName", std::vector<std::string>{"in_val", "out_val"});
        effect.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
        effect.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic"});
        effect.setAttr(dpiImport, "hasReturn", false);
        (void)dpiImport;
        const auto dpiCall =
            effect.createOperation(OperationKind::kDpicCall, effect.internSymbol("dpi_call"));
        effect.addOperand(dpiCall, effectCond);
        effect.addOperand(dpiCall, effectInput);
        effect.addOperand(dpiCall, effectClock);
        effect.addResult(dpiCall, effectDpiResult);
        effect.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_func"));
        effect.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
        effect.setAttr(dpiCall, "inArgName", std::vector<std::string>{"in_val"});
        effect.setAttr(dpiCall, "outArgName", std::vector<std::string>{"out_val"});
        effect.setAttr(dpiCall, "hasReturn", false);
        const auto effectAssign = effect.createOperation(OperationKind::kAssign,
                                                         effect.internSymbol("effect_result_assign"));
        effect.addOperand(effectAssign, effectDpiResult);
        effect.addResult(effectAssign, effectOutput);

        const auto dpiImportVoid =
            effect.createOperation(OperationKind::kDpicImport, effect.internSymbol("dpi_void_func"));
        effect.setAttr(dpiImportVoid, "argsDirection", std::vector<std::string>{"input"});
        effect.setAttr(dpiImportVoid, "argsWidth", std::vector<int64_t>{8});
        effect.setAttr(dpiImportVoid, "argsName", std::vector<std::string>{"in_val"});
        effect.setAttr(dpiImportVoid, "argsSigned", std::vector<bool>{false});
        effect.setAttr(dpiImportVoid, "argsType", std::vector<std::string>{"logic"});
        effect.setAttr(dpiImportVoid, "hasReturn", false);
        const auto dpiCallVoid =
            effect.createOperation(OperationKind::kDpicCall, effect.internSymbol("dpi_call_void"));
        effect.addOperand(dpiCallVoid, effectCond);
        effect.addOperand(dpiCallVoid, effectInput);
        effect.addOperand(dpiCallVoid, effectClock);
        effect.setAttr(dpiCallVoid, "targetImportSymbol", std::string("dpi_void_func"));
        effect.setAttr(dpiCallVoid, "eventEdge", std::vector<std::string>{"posedge"});
        effect.setAttr(dpiCallVoid, "inArgName", std::vector<std::string>{"in_val"});
        effect.setAttr(dpiCallVoid, "outArgName", std::vector<std::string>{});
        effect.setAttr(dpiCallVoid, "hasReturn", false);

        const auto stateReg = effect.createOperation(OperationKind::kRegister,
                                                     effect.internSymbol("state_reg"));
        effect.setAttr(stateReg, "width", static_cast<int64_t>(8));
        effect.setAttr(stateReg, "isSigned", false);
        const auto stateRead = effect.createOperation(OperationKind::kRegisterReadPort,
                                                      effect.internSymbol("state_read"));
        effect.addResult(stateRead, stateOutput);
        effect.setAttr(stateRead, "regSymbol", std::string("state_reg"));
        Graph &part0 = buildUnitGraph(design,
                                      "SimTop_logic_part_repcut_part0",
                                      {{"clock", 1}, {"reset", 1}, {"effect__out", 8}, {"state__out", 8}},
                                      {{"mid__val", 8}});
        const auto part0Cond = part0.createValue(part0.internSymbol("part0_cond"), 1, false);
        const auto part0CondConst =
            part0.createOperation(OperationKind::kConstant, part0.internSymbol("part0_const_true"));
        part0.setAttr(part0CondConst, "constValue", std::string("1'b1"));
        part0.addResult(part0CondConst, part0Cond);
        const auto part0DpiImport =
            part0.createOperation(OperationKind::kDpicImport, part0.internSymbol("part0_dpi_void_func"));
        part0.setAttr(part0DpiImport, "argsDirection", std::vector<std::string>{"input"});
        part0.setAttr(part0DpiImport, "argsWidth", std::vector<int64_t>{8});
        part0.setAttr(part0DpiImport, "argsName", std::vector<std::string>{"in_val"});
        part0.setAttr(part0DpiImport, "argsSigned", std::vector<bool>{false});
        part0.setAttr(part0DpiImport, "argsType", std::vector<std::string>{"logic"});
        part0.setAttr(part0DpiImport, "hasReturn", false);
        const auto part0DpiCall =
            part0.createOperation(OperationKind::kDpicCall, part0.internSymbol("part0_dpi_call_void"));
        part0.addOperand(part0DpiCall, part0Cond);
        part0.addOperand(part0DpiCall, part0.inputPorts()[2].value);
        part0.addOperand(part0DpiCall, part0.inputPorts()[0].value);
        part0.setAttr(part0DpiCall, "targetImportSymbol", std::string("part0_dpi_void_func"));
        part0.setAttr(part0DpiCall, "eventEdge", std::vector<std::string>{"posedge"});
        part0.setAttr(part0DpiCall, "inArgName", std::vector<std::string>{"in_val"});
        part0.setAttr(part0DpiCall, "outArgName", std::vector<std::string>{});
        part0.setAttr(part0DpiCall, "hasReturn", false);
        buildUnitGraph(design,
                       "SimTop_logic_part_repcut_part1",
                       {{"clock", 1}, {"reset", 1}, {"mid__val", 8}, {"sel", 1}},
                       {{"out__data", 8}});

        Graph &top = design.createGraph("SimTop");
        const auto clock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto linkClock = top.createValue(top.internSymbol("link_clock"), 1, false);
        const auto reset = top.createValue(top.internSymbol("reset"), 1, false);
        const auto inData = top.createValue(top.internSymbol("in_data"), 8, false);
        const auto effectOut = top.createValue(top.internSymbol("effect__out"), 8, false);
        const auto stateOut = top.createValue(top.internSymbol("state__out"), 8, false);
        const auto mid = top.createValue(top.internSymbol("mid"), 8, false);
        const auto outData = top.createValue(top.internSymbol("out_data"), 8, false);
        const auto outAlias = top.createValue(top.internSymbol("out_alias"), 8, false);
        const auto selConst = top.createValue(top.internSymbol("sel_const"), 1, false);

        top.bindInputPort("clock", clock);
        top.bindInputPort("reset", reset);
        top.bindInputPort("in_data", inData);
        top.bindOutputPort("out", outAlias);
        top.bindOutputPort("effect_tap", effectOut);

        const auto clockAssign = top.createOperation(OperationKind::kAssign, top.internSymbol("assign_clock_alias"));
        top.addOperand(clockAssign, clock);
        top.addResult(clockAssign, linkClock);

        const auto constOp = top.createOperation(OperationKind::kConstant, top.internSymbol("const_sel"));
        top.setAttr(constOp, "constValue", std::string("1'b1"));
        top.addResult(constOp, selConst);

        const auto outAssign = top.createOperation(OperationKind::kAssign, top.internSymbol("assign_out_alias"));
        top.addOperand(outAssign, outData);
        top.addResult(outAssign, outAlias);

        addInstance(top,
                    "effect_part",
                    "SimTop_effect_part",
                    {linkClock, reset, inData},
                    {effectOut, stateOut},
                    {"clock", "reset", "in__data"},
                    {"effect__out", "state__out"});
        addInstance(top,
                    "part_0",
                    "SimTop_logic_part_repcut_part0",
                    {linkClock, reset, effectOut, stateOut},
                    {mid},
                    {"clock", "reset", "effect__out", "state__out"},
                    {"mid__val"});
        addInstance(top,
                    "part_1",
                    "SimTop_logic_part_repcut_part1",
                    {linkClock, reset, mid, selConst},
                    {outData},
                    {"clock", "reset", "mid__val", "sel"},
                    {"out__data"});

        design.markAsTop("SimTop");
        return design;
    }

    struct DpiResult
    {
        ValueId condition;
        ValueId result;
    };

    DpiResult addDpiResult(Graph &graph,
                           ValueId clock,
                           ValueId input,
                           std::string_view prefix)
    {
        const std::string stem(prefix);
        const auto condition = graph.createValue(graph.internSymbol(stem + "_condition"), 1, false);
        const auto result = graph.createValue(graph.internSymbol(stem + "_result"), 8, false);
        const auto conditionOp = graph.createOperation(OperationKind::kConstant,
                                                       graph.internSymbol(stem + "_condition_op"));
        graph.setAttr(conditionOp, "constValue", std::string("1'b1"));
        graph.addResult(conditionOp, condition);

        const auto dpiImport = graph.createOperation(OperationKind::kDpicImport,
                                                     graph.internSymbol(stem + "_import"));
        graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "output"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"in_val", "out_val"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        const auto dpiCall = graph.createOperation(OperationKind::kDpicCall,
                                                   graph.internSymbol(stem + "_call"));
        graph.addOperand(dpiCall, condition);
        graph.addOperand(dpiCall, input);
        graph.addOperand(dpiCall, clock);
        graph.addResult(dpiCall, result);
        graph.setAttr(dpiCall, "targetImportSymbol", stem + "_import");
        graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"in_val"});
        graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"out_val"});
        graph.setAttr(dpiCall, "hasReturn", false);
        return {condition, result};
    }

    ValueId addRegisterRead(Graph &graph, std::string_view prefix)
    {
        const std::string stem(prefix);
        const auto reg = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol(stem + "_reg"));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", false);
        const auto value = graph.createValue(graph.internSymbol(stem + "_value"), 8, false);
        const auto read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                graph.internSymbol(stem + "_read"));
        graph.addResult(read, value);
        graph.setAttr(read, "regSymbol", stem + "_reg");
        return value;
    }

    Design buildNoEarlyEffectDesign()
    {
        Design design;
        Graph &topOnly = design.createGraph("TopOnlyEffectUnit");
        const auto topOnlyClock = topOnly.createValue(topOnly.internSymbol("clock"), 1, false);
        const auto topOnlyInput = topOnly.createValue(topOnly.internSymbol("input"), 8, false);
        topOnly.bindInputPort("clock", topOnlyClock);
        topOnly.bindInputPort("input", topOnlyInput);
        const auto topOnlyEffect = addDpiResult(topOnly, topOnlyClock, topOnlyInput, "top_only");
        topOnly.bindOutputPort("effect", topOnlyEffect.result);

        Graph &local = design.createGraph("LocalEffectUnit");
        const auto localClock = local.createValue(local.internSymbol("clock"), 1, false);
        const auto localInput = local.createValue(local.internSymbol("input"), 8, false);
        local.bindInputPort("clock", localClock);
        local.bindInputPort("input", localInput);
        const auto localEffect = addDpiResult(local, localClock, localInput, "local");
        const auto localState = addRegisterRead(local, "local_state");
        local.bindOutputPort("state", localState);
        const auto mask = local.createValue(local.internSymbol("local_mask"), 8, false);
        const auto maskOp = local.createOperation(OperationKind::kConstant,
                                                  local.internSymbol("local_mask_op"));
        local.setAttr(maskOp, "constValue", std::string("8'hff"));
        local.addResult(maskOp, mask);
        const auto write = local.createOperation(OperationKind::kRegisterWritePort,
                                                 local.internSymbol("local_state_write"));
        local.addOperand(write, localEffect.condition);
        local.addOperand(write, localEffect.result);
        local.addOperand(write, mask);
        local.addOperand(write, localClock);
        local.setAttr(write, "regSymbol", std::string("local_state_reg"));
        local.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        Graph &top = design.createGraph("NoEarlyTop");
        const auto clock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto input = top.createValue(top.internSymbol("input"), 8, false);
        const auto effect = top.createValue(top.internSymbol("effect"), 8, false);
        const auto state = top.createValue(top.internSymbol("state"), 8, false);
        top.bindInputPort("clock", clock);
        top.bindInputPort("input", input);
        top.bindOutputPort("effect", effect);
        top.bindOutputPort("state", state);
        addInstance(top, "top_only", "TopOnlyEffectUnit", {clock, input}, {effect},
                    {"clock", "input"}, {"effect"});
        addInstance(top, "local", "LocalEffectUnit", {clock, input}, {state},
                    {"clock", "input"}, {"state"});
        design.markAsTop("NoEarlyTop");
        return design;
    }

    Design buildMixedCrossUnitDesign()
    {
        Design design;
        Graph &mixed = design.createGraph("MixedUnit");
        const auto clock = mixed.createValue(mixed.internSymbol("clock"), 1, false);
        const auto input = mixed.createValue(mixed.internSymbol("input"), 8, false);
        mixed.bindInputPort("clock", clock);
        mixed.bindInputPort("input", input);
        const auto effect = addDpiResult(mixed, clock, input, "mixed");
        const auto state = addRegisterRead(mixed, "mixed_state");
        const auto output = mixed.createValue(mixed.internSymbol("mixed_output"), 16, false);
        const auto concat = mixed.createOperation(OperationKind::kConcat,
                                                  mixed.internSymbol("mixed_concat"));
        mixed.addOperand(concat, effect.result);
        mixed.addOperand(concat, state);
        mixed.addResult(concat, output);
        mixed.bindOutputPort("mixed", output);

        Graph &sink = buildUnitGraph(design, "MixedSink", {{"input", 16}}, {{"output", 16}});
        const auto sinkAssign = sink.createOperation(OperationKind::kAssign,
                                                     sink.internSymbol("sink_assign"));
        sink.addOperand(sinkAssign, sink.inputPorts()[0].value);
        sink.addResult(sinkAssign, sink.outputPorts()[0].value);

        Graph &top = design.createGraph("MixedTop");
        const auto topClock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto topInput = top.createValue(top.internSymbol("input"), 8, false);
        const auto mixedValue = top.createValue(top.internSymbol("mixed"), 16, false);
        const auto outputValue = top.createValue(top.internSymbol("output"), 16, false);
        top.bindInputPort("clock", topClock);
        top.bindInputPort("input", topInput);
        top.bindOutputPort("output", outputValue);
        addInstance(top, "mixed_unit", "MixedUnit", {topClock, topInput}, {mixedValue},
                    {"clock", "input"}, {"mixed"});
        addInstance(top, "sink_unit", "MixedSink", {mixedValue}, {outputValue}, {"input"}, {"output"});
        design.markAsTop("MixedTop");
        return design;
    }

    Design buildEarlyChainDesign()
    {
        Design design;
        auto buildEffectUnit = [&](std::string_view name, std::string_view prefix) -> Graph & {
            Graph &graph = design.createGraph(std::string(name));
            const auto clock = graph.createValue(graph.internSymbol("clock"), 1, false);
            const auto input = graph.createValue(graph.internSymbol("input"), 8, false);
            graph.bindInputPort("clock", clock);
            graph.bindInputPort("input", input);
            const auto effect = addDpiResult(graph, clock, input, prefix);
            graph.bindOutputPort("output", effect.result);
            return graph;
        };
        buildEffectUnit("EarlyA", "early_a");
        buildEffectUnit("EarlyB", "early_b");
        Graph &sink = buildUnitGraph(design, "EarlySink", {{"input", 8}}, {{"output", 8}});
        const auto sinkAssign = sink.createOperation(OperationKind::kAssign,
                                                     sink.internSymbol("sink_assign"));
        sink.addOperand(sinkAssign, sink.inputPorts()[0].value);
        sink.addResult(sinkAssign, sink.outputPorts()[0].value);

        Graph &top = design.createGraph("EarlyChainTop");
        const auto clock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto input = top.createValue(top.internSymbol("input"), 8, false);
        const auto a = top.createValue(top.internSymbol("a"), 8, false);
        const auto b = top.createValue(top.internSymbol("b"), 8, false);
        const auto output = top.createValue(top.internSymbol("output"), 8, false);
        top.bindInputPort("clock", clock);
        top.bindInputPort("input", input);
        top.bindOutputPort("output", output);
        addInstance(top, "early_a", "EarlyA", {clock, input}, {a},
                    {"clock", "input"}, {"output"});
        addInstance(top, "early_b", "EarlyB", {clock, a}, {b},
                    {"clock", "input"}, {"output"});
        addInstance(top, "sink", "EarlySink", {b}, {output}, {"input"}, {"output"});
        design.markAsTop("EarlyChainTop");
        return design;
    }

    Design buildNormalToEarlyDesign()
    {
        Design design;
        Graph &normal = buildUnitGraph(design,
                                       "NormalProducer",
                                       {{"clock", 1}, {"input", 8}},
                                       {{"output", 8}});
        const auto normalAssign = normal.createOperation(OperationKind::kAssign,
                                                         normal.internSymbol("normal_assign"));
        normal.addOperand(normalAssign, normal.inputPorts()[1].value);
        normal.addResult(normalAssign, normal.outputPorts()[0].value);

        Graph &early = design.createGraph("EarlyConsumer");
        const auto earlyClock = early.createValue(early.internSymbol("clock"), 1, false);
        const auto normalInput = early.createValue(early.internSymbol("normal_input"), 8, false);
        early.bindInputPort("clock", earlyClock);
        early.bindInputPort("normal_input", normalInput);
        const auto effect = addDpiResult(early, earlyClock, normalInput, "normal_to_early");
        early.bindOutputPort("output", effect.result);

        Graph &sink = buildUnitGraph(design, "EarlySinkNormal", {{"input", 8}}, {{"output", 8}});
        const auto sinkAssign = sink.createOperation(OperationKind::kAssign,
                                                     sink.internSymbol("sink_assign"));
        sink.addOperand(sinkAssign, sink.inputPorts()[0].value);
        sink.addResult(sinkAssign, sink.outputPorts()[0].value);

        Graph &top = design.createGraph("NormalToEarlyTop");
        const auto clock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto input = top.createValue(top.internSymbol("input"), 8, false);
        const auto normalValue = top.createValue(top.internSymbol("normal_value"), 8, false);
        const auto earlyValue = top.createValue(top.internSymbol("early_value"), 8, false);
        const auto output = top.createValue(top.internSymbol("output"), 8, false);
        top.bindInputPort("clock", clock);
        top.bindInputPort("input", input);
        top.bindOutputPort("output", output);
        addInstance(top, "normal", "NormalProducer", {clock, input}, {normalValue},
                    {"clock", "input"}, {"output"});
        addInstance(top, "early", "EarlyConsumer", {clock, normalValue}, {earlyValue},
                    {"clock", "normal_input"}, {"output"});
        addInstance(top, "sink", "EarlySinkNormal", {earlyValue}, {output}, {"input"}, {"output"});
        design.markAsTop("NormalToEarlyTop");
        return design;
    }

    Design buildSystemFunctionEffectPathDesign(bool hasSideEffects)
    {
        Design design;
        const std::string namePrefix = hasSideEffects ? "Unsupported" : "Pure";
        Graph &producer = design.createGraph(namePrefix + "Producer");
        const auto clock = producer.createValue(producer.internSymbol("clock"), 1, false);
        const auto input = producer.createValue(producer.internSymbol("input"), 8, false);
        producer.bindInputPort("clock", clock);
        producer.bindInputPort("input", input);
        const auto effect = addDpiResult(producer, clock, input, "unsupported");
        const auto output = producer.createValue(producer.internSymbol("output"), 8, false);
        const auto unknown = producer.createOperation(OperationKind::kSystemFunction,
                                                      producer.internSymbol("unsupported_function"));
        producer.addOperand(unknown, effect.result);
        producer.addResult(unknown, output);
        producer.setAttr(unknown, "name", std::string("unsigned"));
        if (hasSideEffects)
        {
            producer.setAttr(unknown, "hasSideEffects", true);
        }
        producer.bindOutputPort("output", output);

        Graph &sink = buildUnitGraph(design, namePrefix + "Sink", {{"input", 8}}, {{"output", 8}});
        const auto sinkAssign = sink.createOperation(OperationKind::kAssign,
                                                     sink.internSymbol("sink_assign"));
        sink.addOperand(sinkAssign, sink.inputPorts()[0].value);
        sink.addResult(sinkAssign, sink.outputPorts()[0].value);

        Graph &top = design.createGraph(namePrefix + "Top");
        const auto topClock = top.createValue(top.internSymbol("clock"), 1, false);
        const auto topInput = top.createValue(top.internSymbol("input"), 8, false);
        const auto intermediate = top.createValue(top.internSymbol("intermediate"), 8, false);
        const auto topOutput = top.createValue(top.internSymbol("output"), 8, false);
        top.bindInputPort("clock", topClock);
        top.bindInputPort("input", topInput);
        top.bindOutputPort("output", topOutput);
        addInstance(top, "producer", namePrefix + "Producer", {topClock, topInput}, {intermediate},
                    {"clock", "input"}, {"output"});
        addInstance(top, "sink", namePrefix + "Sink", {intermediate}, {topOutput}, {"input"}, {"output"});
        design.markAsTop(namePrefix + "Top");
        return design;
    }

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    const Design design = buildDesign();
    const std::filesystem::path artifactRoot = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "verilator_repcut_package";

    std::error_code ec;
    std::filesystem::remove_all(artifactRoot, ec);

    EmitDiagnostics diagnostics;
    EmitVerilatorRepCutPackage emitter(&diagnostics);
    EmitOptions options;
    options.outputDir = artifactRoot.string();
    options.topOverrides = {"SimTop"};

    const EmitResult result = emitter.emit(design, options);
    if (!result.success)
    {
        return fail("package emit reported failure: " + diagnosticsSummary(diagnostics));
    }
    if (diagnostics.hasError())
    {
        return fail("package emit reported diagnostics errors: " + diagnosticsSummary(diagnostics));
    }

    const std::filesystem::path effectSvPath = artifactRoot / "sv" / "SimTop_effect_part.sv";
    const std::filesystem::path part0SvPath = artifactRoot / "sv" / "SimTop_logic_part_repcut_part0.sv";
    const std::filesystem::path part1SvPath = artifactRoot / "sv" / "SimTop_logic_part_repcut_part1.sv";
    const std::filesystem::path topSvPath = artifactRoot / "sv" / "SimTop.sv";
    const std::filesystem::path effectFileList = artifactRoot / "verilate" / "effect_part.f";
    const std::filesystem::path part0FileList = artifactRoot / "verilate" / "part_0.f";
    const std::filesystem::path part1FileList = artifactRoot / "verilate" / "part_1.f";
    const std::filesystem::path wrapperHeaderPath = artifactRoot / "wolvi_repcut_verilator_sim.h";
    const std::filesystem::path smokeMainPath = artifactRoot / "partitioned_smoke_main.cpp";
    const std::filesystem::path unitsMkPath = artifactRoot / "units.mk";
    const std::filesystem::path makefilePath = artifactRoot / "Makefile";
    std::vector<std::filesystem::path> wrapperSourcePaths;
    for (const auto &entry : std::filesystem::directory_iterator(artifactRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (entry.path().extension() == ".cpp" && filename.rfind("wolvi_repcut_verilator_sim", 0) == 0)
        {
            wrapperSourcePaths.push_back(entry.path());
        }
    }
    std::sort(wrapperSourcePaths.begin(), wrapperSourcePaths.end());
    std::unordered_map<std::string, std::string> wrapperSourceByName;

    for (const auto &path : {effectSvPath, part0SvPath, part1SvPath, topSvPath,
                             effectFileList, part0FileList, part1FileList, wrapperHeaderPath,
                             smokeMainPath, unitsMkPath, makefilePath})
    {
        if (!std::filesystem::exists(path))
        {
            return fail("expected package artifact is missing: " + path.string());
        }
    }
    if (wrapperSourcePaths.empty())
    {
        return fail("expected split wrapper sources are missing");
    }

    if (readFile(effectFileList) !=
        (artifactRoot / "sv" / "WolviRepCutUnit_effect_part.sv").generic_string() + "\n" +
            (artifactRoot / "sv" / "SimTop_effect_part.sv").generic_string() + "\n")
    {
        return fail("unexpected effect_part file list content");
    }
    if (readFile(part0FileList) !=
        (artifactRoot / "sv" / "WolviRepCutUnit_part_0.sv").generic_string() + "\n" +
            (artifactRoot / "sv" / "SimTop_logic_part_repcut_part0.sv").generic_string() + "\n")
    {
        return fail("unexpected part_0 file list content");
    }
    if (readFile(part1FileList) !=
        (artifactRoot / "sv" / "WolviRepCutUnit_part_1.sv").generic_string() + "\n" +
            (artifactRoot / "sv" / "SimTop_logic_part_repcut_part1.sv").generic_string() + "\n")
    {
        return fail("unexpected part_1 file list content");
    }

    const std::string wrapperHeader = readFile(wrapperHeaderPath);
    const std::string smokeMain = readFile(smokeMainPath);
    const std::string unitsMk = readFile(unitsMkPath);
    const std::string makefile = readFile(makefilePath);
    std::string wrapperSource;
    for (const auto &path : wrapperSourcePaths)
    {
        const std::string text = readFile(path);
        wrapperSourceByName.emplace(path.filename().string(), text);
        wrapperSource += text;
        wrapperSource += "\n";
    }
    if (wrapperHeader.empty() || wrapperSource.empty() || smokeMain.empty() || unitsMk.empty() || makefile.empty())
    {
        return fail("failed to read generated package build files");
    }
    if (!contains(wrapperHeader, "class VWolviRepCutUnit_effect_part;") ||
        !contains(wrapperHeader, "class VWolviRepCutUnit_part_0;") ||
        !contains(wrapperHeader, "class VWolviRepCutUnit_part_1;"))
    {
        return fail("wrapper header missing expected model forward declarations");
    }
    if (!contains(wrapperHeader, "std::unique_ptr<VWolviRepCutUnit_effect_part> unit_effect_part_;") ||
        !contains(wrapperHeader, "std::unique_ptr<VWolviRepCutUnit_part_0> unit_part_0_;") ||
        !contains(wrapperHeader, "std::unique_ptr<VWolviRepCutUnit_part_1> unit_part_1_;"))
    {
        return fail("wrapper header missing expected unit members");
    }
    if (!contains(wrapperHeader, "using StepFn = void (WolviRepCutVerilatorSim::*)(std::size_t);") ||
        !contains(wrapperHeader, "std::vector<StepFn> load_step_fns_;") ||
        !contains(wrapperHeader, "std::vector<StepFn> early_eval_step_fns_;") ||
        !contains(wrapperHeader, "std::vector<StepFn> early_update_step_fns_;") ||
        !contains(wrapperHeader, "std::vector<StepFn> normal_eval_step_fns_;") ||
        !contains(wrapperHeader, "std::vector<StepFn> normal_update_step_fns_;") ||
        !contains(wrapperHeader, "void run_host_phase_(const std::vector<StepFn>& phaseFns);") ||
        !contains(wrapperHeader, "void run_phase_workers_(const std::vector<StepFn>& phaseFns);"))
    {
        return fail("wrapper header missing expected phase scheduling declarations");
    }
    if (!contains(wrapperHeader, "struct alignas(64) PartTimingStats {") ||
        !contains(wrapperHeader, "std::uint64_t input_apply_ns{};") ||
        !contains(wrapperHeader, "std::uint64_t eval_ns{};") ||
        !contains(wrapperHeader, "std::uint64_t update_push_ns{};") ||
        !contains(wrapperHeader, "struct StepTimingStats {") ||
        !contains(wrapperHeader, "std::uint64_t input_load_ns{};") ||
        !contains(wrapperHeader, "std::uint64_t global_update_ns{};") ||
        !contains(wrapperHeader, "PartTimingStats collect_part_timing_stats_(std::size_t partIndex) const;") ||
        !contains(wrapperHeader, "void report_step_timing_() const;") ||
        !contains(wrapperHeader, "void report_part_timing_() const;") ||
        !contains(wrapperHeader, "void dump_timing_jsonl_() const;") ||
        !contains(wrapperHeader, "std::uint64_t step_count_{};") ||
        !contains(wrapperHeader, "StepTimingStats step_timing_{};") ||
        !contains(wrapperHeader, "part_timing_stats_{};") ||
        !contains(wrapperHeader, "part_timing_worker_stats_{};"))
    {
        return fail("wrapper header missing expected timing declarations");
    }
    if (contains(wrapperHeader, "snapshot_") ||
        contains(wrapperHeader, "writeback_"))
    {
        return fail("wrapper header should not contain snapshot/writeback cache members");
    }
    if (!contains(wrapperHeader, "void set_clock(CData value) { top_in_clock_ = value; }") ||
        !contains(wrapperHeader, "void set_in_data(CData value) { top_in_in_data_ = value; }") ||
        !contains(wrapperHeader, "CData get_out() const { return top_out_out_; }"))
    {
        return fail("wrapper header missing expected top port accessors");
    }
    if (!contains(wrapperSource, "#include \"VWolviRepCutUnit_effect_part.h\"") ||
        !contains(wrapperSource, "#include \"VWolviRepCutUnit_part_0.h\"") ||
        !contains(wrapperSource, "#include \"VWolviRepCutUnit_part_1.h\""))
    {
        return fail("wrapper sources missing expected model includes");
    }
    if (!contains(wrapperSource, "unit_effect_part_->in_2 = top_in_in_data_;") ||
        !contains(wrapperSource, "unit_part_0_->in_2 = unit_effect_part_->out_0;") ||
        !contains(wrapperSource, "unit_part_0_->in_3 = unit_effect_part_->out_1;") ||
        !contains(wrapperSource, "top_out_effect_tap_ = unit_effect_part_->out_0;") ||
        !contains(wrapperSource, "unit_part_1_->in_2 = unit_part_0_->out_0;") ||
        !contains(wrapperSource, "unit_part_1_->in_3 = const_sel_const_;") ||
        !contains(wrapperSource, "top_out_out_ = unit_part_1_->out_0;"))
    {
        return fail("wrapper source missing expected load/update wiring");
    }
    if (!contains(wrapperSource, "load_step_fns_.push_back(&WolviRepCutVerilatorSim::") ||
        !contains(wrapperSource, "early_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_effect_part_);") ||
        !contains(wrapperSource, "early_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_early_effect_part_);") ||
        !contains(wrapperSource, "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_part_0_);") ||
        !contains(wrapperSource, "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_part_1_);") ||
        !contains(wrapperSource, "normal_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_effect_part_);") ||
        !contains(wrapperSource, "normal_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_part_0_);") ||
        !contains(wrapperSource, "normal_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_part_1_);") ||
        contains(wrapperSource, "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_effect_part_);") ||
        contains(wrapperSource, "early_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_effect_part_);"))
    {
        return fail("wrapper source missing expected early/normal task registration");
    }
    if (!contains(wrapperSource, "unit_effect_part_->eval();") ||
        !contains(wrapperSource, "// eval part_0") ||
        !contains(wrapperSource, "// eval part_1") ||
        !contains(wrapperSource, "run_phase_workers_(early_eval_step_fns_);") ||
        !contains(wrapperSource, "run_phase_workers_(normal_eval_step_fns_);"))
    {
        return fail("wrapper source missing expected eval calls");
    }
    if (contains(wrapperSource, "snapshot_") ||
        contains(wrapperSource, "commit_writeback_()"))
    {
        return fail("wrapper source should not contain snapshot/writeback publish paths");
    }
    if (!contains(wrapperSource, "std::getenv(\"XS_EMU_THREADS\")") ||
        !contains(wrapperSource, "const std::size_t maxParallelFns = std::max({early_eval_step_fns_.size(), early_update_step_fns_.size(), normal_eval_step_fns_.size(), normal_update_step_fns_.size()});") ||
        !contains(wrapperSource, "[[noreturn]] inline void wolvi_repcut_thread_config_error") ||
        !contains(wrapperSource, "[WOLVI][thread-config] error=%s") ||
        !contains(wrapperSource, "std::abort();") ||
        !contains(wrapperSource, "if (*cursor < '0' || *cursor > '9')") ||
        !contains(wrapperSource, "errno == ERANGE") ||
        !contains(wrapperSource, "std::numeric_limits<std::size_t>::max()") ||
        !contains(wrapperSource, "if (requestedWorkers > 3)") ||
        !contains(wrapperSource, "if (requestedWorkers != 0 && requestedWorkers > maxParallelFns)") ||
        !contains(wrapperSource, "if (requestedWorkers > availableCpuCount)") ||
        !contains(wrapperSource, "std::size_t startedWorkerCount = 0;") ||
        !contains(wrapperSource, "} catch (...) {") ||
        !contains(wrapperSource, "worker.stop = true;") ||
        !contains(wrapperSource, "worker.cv.notify_one();") ||
        !contains(wrapperSource, "return worker.stop || worker.hasWork;") ||
        !contains(wrapperSource, "phase_workers_[workerIndex].thread.join();") ||
        !contains(wrapperSource, "failed to create phase worker thread") ||
        !contains(wrapperSource, "phase_worker_count_ = requestedWorkers;") ||
        !contains(wrapperSource, "[WOLVI][thread-config] requested=%zu effective=%zu max_parallel=%zu available_cpus=%zu") ||
        !contains(wrapperSource, "#if defined(__linux__)"))
    {
        return fail("wrapper source missing expected runtime thread-pool guards");
    }
    const auto commonSourceIt = wrapperSourceByName.find("wolvi_repcut_verilator_sim_common.cpp");
    if (commonSourceIt == wrapperSourceByName.end())
    {
        return fail("missing common wrapper source");
    }
    const std::string &commonSource = commonSourceIt->second;
    if (!contains(wrapperHeader, "using WolviClock = std::chrono::steady_clock;") ||
        !contains(wrapperSource, "void WolviRepCutVerilatorSim::report_step_timing_() const {") ||
        !contains(wrapperSource, "[WOLVI][step-timing] steps=%llu total=%.3f ms avg=%.3f us\\n") ||
        !contains(wrapperSource, "printPhase(\"input_load\", step_timing_.input_load_ns);") ||
        !contains(wrapperSource, "printPhase(\"part_eval\", step_timing_.part_eval_ns);") ||
        !contains(wrapperSource, "printPhase(\"global_update\", step_timing_.global_update_ns);") ||
        !contains(wrapperSource, "WolviRepCutVerilatorSim::PartTimingStats WolviRepCutVerilatorSim::collect_part_timing_stats_(std::size_t partIndex) const {") ||
        !contains(wrapperSource, "void WolviRepCutVerilatorSim::report_part_timing_() const {") ||
        !contains(wrapperSource, "void WolviRepCutVerilatorSim::dump_timing_jsonl_() const {") ||
        !contains(wrapperSource, "std::getenv(\"WOLVI_REPCUT_TIMING_JSONL\")") ||
        !contains(wrapperSource, "\"{\\\"record_type\\\":\\\"part_timing\\\"") ||
        !contains(wrapperSource, "\\\"input_apply_total_ms\\\":%.3f") ||
        !contains(wrapperSource, "\\\"eval_total_ms\\\":%.3f") ||
        !contains(wrapperSource, "\\\"update_push_total_ms\\\":%.3f") ||
        !contains(wrapperSource, "[WOLVI][part-timing] part=%s steps=%llu total=%.3f ms avg=%.3f us input_apply=%.3f us eval=%.3f us update_push=%.3f us\\n") ||
        !contains(wrapperSource, "++step_timing_.steps;") ||
        !contains(wrapperSource, "step_timing_.input_load_ns +=") ||
        !contains(wrapperSource, "step_timing_.part_eval_ns +=") ||
        !contains(wrapperSource, "step_timing_.global_update_ns +=") ||
        !contains(wrapperSource, "step_timing_.total_ns +=") ||
        !contains(wrapperSource, "partTimingStats->input_apply_ns +=") ||
        !contains(wrapperSource, "partTimingStats->eval_ns +=") ||
        !contains(wrapperSource, "partTimingStats->update_push_ns +=") ||
        !contains(wrapperSource, "partTimingStats->total_ns +=") ||
        !contains(wrapperSource, "partTimingStats = &part_timing_worker_stats_[workerIndex][") ||
        !contains(wrapperSource, "PartTimingStats* partTimingStats = &part_timing_stats_[0];") ||
        !contains(wrapperSource, "PartTimingStats* partTimingStats = &part_timing_stats_[1];") ||
        !contains(wrapperSource, "PartTimingStats* partTimingStats = &part_timing_stats_[2];") ||
        contains(wrapperSource, "\\\"scatter_total_ms\\\":%.3f") ||
        contains(wrapperSource, "\\\"gather_total_ms\\\":%.3f"))
    {
        return fail("wrapper source missing expected timing instrumentation");
    }
    if (!contains(wrapperSource, "run_phase_workers_(early_eval_step_fns_);") ||
        !contains(wrapperSource, "run_phase_workers_(early_update_step_fns_);") ||
        !contains(wrapperSource, "run_phase_workers_(normal_eval_step_fns_);") ||
        !contains(wrapperSource, "run_phase_workers_(normal_update_step_fns_);"))
    {
        return fail("wrapper source missing early/normal scheduling phases");
    }
    if (contains(wrapperSource, "XS_REPCUT_STEP_TIMING") ||
        contains(wrapperSource, "step_timing_enabled_") ||
        contains(wrapperSource, "wolvi_env_flag_enabled("))
    {
        return fail("wrapper source should not contain legacy step timing instrumentation");
    }
    const std::size_t dtorPos = commonSource.find("WolviRepCutVerilatorSim::~WolviRepCutVerilatorSim()");
    const std::size_t reportStepPos = commonSource.find("report_step_timing_();", dtorPos);
    const std::size_t reportPartPos = commonSource.find("report_part_timing_();", dtorPos);
    const std::size_t dumpJsonPos = commonSource.find("dump_timing_jsonl_();", dtorPos);
    const std::size_t shutdownPos = commonSource.find("shutdown_phase_workers_();", dtorPos);
    if (dtorPos == std::string::npos || reportStepPos == std::string::npos || reportPartPos == std::string::npos ||
        dumpJsonPos == std::string::npos || shutdownPos == std::string::npos)
    {
        return fail("wrapper source missing destructor timing/shutdown sequence");
    }
    if (!(reportStepPos < shutdownPos && reportPartPos < shutdownPos && dumpJsonPos < shutdownPos))
    {
        return fail("wrapper destructor should report and dump timing before clearing worker timing state");
    }
    std::string loadChunkSource;
    std::string evalChunkSource;
    std::string updateChunkSource;
    for (const auto &[name, text] : wrapperSourceByName)
    {
        if (name.rfind("wolvi_repcut_verilator_sim_load_", 0) == 0 &&
            text.find("unit_effect_part_->in_2 = top_in_in_data_;") != std::string::npos)
        {
            loadChunkSource = text;
        }
        if (name.rfind("wolvi_repcut_verilator_sim_eval_", 0) == 0 &&
            text.find("unit_effect_part_->eval();") != std::string::npos)
        {
            evalChunkSource = text;
        }
        if (name.rfind("wolvi_repcut_verilator_sim_update_", 0) == 0 &&
            text.find("unit_part_0_->in_2 = unit_effect_part_->out_0;") != std::string::npos)
        {
            updateChunkSource = text;
        }
    }
    if (loadChunkSource.empty() || evalChunkSource.empty() || updateChunkSource.empty())
    {
        return fail("wrapper source missing expected load/eval/update chunks");
    }
    const std::size_t commonLoadPos = commonSource.find("run_host_phase_(load_step_fns_);");
    const std::size_t commonEarlyEvalPos = commonSource.find("run_phase_workers_(early_eval_step_fns_);");
    const std::size_t commonEarlyUpdatePos = commonSource.find("run_phase_workers_(early_update_step_fns_);");
    const std::size_t commonNormalEvalPos = commonSource.find("run_phase_workers_(normal_eval_step_fns_);");
    const std::size_t commonFinalUpdatePos = commonSource.find("run_phase_workers_(normal_update_step_fns_);");
    const std::size_t loadInputPos = loadChunkSource.find("unit_effect_part_->in_2 = top_in_in_data_;");
    const std::size_t evalEvalPos = evalChunkSource.find("unit_effect_part_->eval();");
    const std::size_t updatePushPos = updateChunkSource.find("unit_part_0_->in_2 = unit_effect_part_->out_0;");
    if (commonLoadPos == std::string::npos || commonEarlyEvalPos == std::string::npos ||
        commonEarlyUpdatePos == std::string::npos || commonNormalEvalPos == std::string::npos ||
        commonFinalUpdatePos == std::string::npos || loadInputPos == std::string::npos ||
        evalEvalPos == std::string::npos || updatePushPos == std::string::npos)
    {
        return fail("wrapper source missing phase-order markers");
    }
    if (!(commonLoadPos < commonEarlyEvalPos &&
          commonEarlyEvalPos < commonEarlyUpdatePos &&
          commonEarlyUpdatePos < commonNormalEvalPos &&
          commonNormalEvalPos < commonFinalUpdatePos))
    {
        return fail("wrapper source should run load, early eval/update, then normal eval/final update");
    }
    if (countOccurrences(commonSource, "run_phase_workers_(early_update_step_fns_);") != 1 ||
        countOccurrences(commonSource, "run_phase_workers_(normal_update_step_fns_);") != 1)
    {
        return fail("wrapper source should publish each phase exactly once per step");
    }
    const std::size_t earlyEffectMethodPos = wrapperSource.find(
        "void WolviRepCutVerilatorSim::run_update_early_effect_part_");
    const std::size_t finalEffectMethodPos = wrapperSource.find(
        "void WolviRepCutVerilatorSim::run_update_final_effect_part_");
    const std::size_t earlyEffectEdgePos = wrapperSource.find(
        "unit_part_0_->in_2 = unit_effect_part_->out_0;");
    const std::size_t earlyEffectTopPos = wrapperSource.find(
        "top_out_effect_tap_ = unit_effect_part_->out_0;");
    const std::size_t finalStateEdgePos = wrapperSource.find(
        "unit_part_0_->in_3 = unit_effect_part_->out_1;");
    if (earlyEffectMethodPos == std::string::npos || finalEffectMethodPos == std::string::npos ||
        earlyEffectEdgePos == std::string::npos || earlyEffectTopPos == std::string::npos ||
        finalStateEdgePos == std::string::npos ||
        !(earlyEffectMethodPos < earlyEffectEdgePos && earlyEffectMethodPos < earlyEffectTopPos &&
          finalEffectMethodPos < finalStateEdgePos) ||
        countOccurrences(wrapperSource, "unit_part_0_->in_2 = unit_effect_part_->out_0;") != 1 ||
        countOccurrences(wrapperSource, "top_out_effect_tap_ = unit_effect_part_->out_0;") != 1 ||
        countOccurrences(wrapperSource, "unit_part_0_->in_3 = unit_effect_part_->out_1;") != 1)
    {
        return fail("effect and state edges should be published by their respective update methods exactly once");
    }
    if (!contains(wrapperSource, "const CData WolviRepCutVerilatorSim::const_sel_const_ = static_cast<CData>(0x1ULL);"))
    {
        return fail("wrapper source missing expected constant definition");
    }
    if (!contains(smokeMain, "WolviRepCutVerilatorSim sim;") ||
        !contains(smokeMain, "sim.step();"))
    {
        return fail("smoke main missing expected simulation bootstrap");
    }
    if (!contains(unitsMk, "PARTITIONED_UNITS := effect_part part_0 part_1") ||
        !contains(unitsMk, "PARTITIONED_UNIT_MAKE_J ?= $(if $(strip $(VM_BUILD_JOBS)),-j $(VM_BUILD_JOBS),)") ||
        !contains(unitsMk, "PARTITIONED_VM_PARALLEL_BUILDS ?= $(VM_PARALLEL_BUILDS)") ||
        !contains(unitsMk, "UNIT_effect_part_MODULE := WolviRepCutUnit_effect_part") ||
        !contains(unitsMk, "UNIT_part_0_MODULE := WolviRepCutUnit_part_0") ||
        !contains(unitsMk, "UNIT_part_1_MODULE := WolviRepCutUnit_part_1") ||
        !contains(unitsMk, "$(VERILATOR) --cc -f $(UNIT_effect_part_FILELIST)") ||
        !contains(unitsMk, "$(MAKE) $(PARTITIONED_UNIT_MAKE_J) -C $(UNIT_part_1_MDIR) VM_PARALLEL_BUILDS=$(PARTITIONED_VM_PARALLEL_BUILDS) OBJCACHE= -f VWolviRepCutUnit_part_1.mk VWolviRepCutUnit_part_1__ALL.a"))
    {
        return fail("units.mk missing expected unit build rules");
    }
    if (!contains(makefile, "include units.mk") ||
        !contains(makefile, ".DEFAULT_GOAL := all") ||
        !contains(makefile, "PARTITIONED_VERILATOR_FLAGS ?= --no-timing -Wno-STMTDLY -Wno-WIDTH -Wno-WIDTHTRUNC --output-split 30000 --output-split-cfuncs 30000") ||
        !contains(makefile, "VM_BUILD_JOBS ?=") ||
        !contains(makefile, "VM_PARALLEL_BUILDS ?= 1") ||
        !contains(makefile, "verilate-units: $(UNIT_ARCHIVES)") ||
        !contains(makefile, "WRAPPER_SRC_NAMES :=") ||
        !contains(makefile, "WRAPPER_SRCS := $(addprefix $(PACKAGE_ROOT)/,$(WRAPPER_SRC_NAMES))") ||
        !contains(makefile, "WRAPPER_OBJS := $(addprefix $(BUILD_DIR)/,$(WRAPPER_SRC_NAMES:.cpp=.o))") ||
        !contains(makefile, "VERILATED_DPI_OBJ := $(BUILD_DIR)/verilated_dpi.o") ||
        !contains(makefile, "VERILATED_THREADS_OBJ := $(BUILD_DIR)/verilated_threads.o") ||
        !contains(makefile, "$(BUILD_DIR)/%.o: $(PACKAGE_ROOT)/%.cpp $(PACKAGE_ROOT)/wolvi_repcut_verilator_sim.h $(UNIT_ARCHIVES) | $(BUILD_DIR)") ||
        !contains(makefile, "$(TARGET): $(VERILATED_OBJ) $(VERILATED_DPI_OBJ) $(VERILATED_THREADS_OBJ) $(WRAPPER_OBJS) $(SMOKE_MAIN_OBJ) $(UNIT_ARCHIVES)") ||
        !contains(makefile, "$(CXX) $(LDFLAGS) -o $@ $(VERILATED_OBJ) $(VERILATED_DPI_OBJ) $(VERILATED_THREADS_OBJ) $(WRAPPER_OBJS) $(SMOKE_MAIN_OBJ) $(UNIT_ARCHIVES) $(LDLIBS) -ldl -pthread") ||
        !contains(makefile, "run: $(TARGET)"))
    {
        return fail("Makefile missing expected top-level build rules");
    }

    {
        const std::filesystem::path noEarlyRoot = artifactRoot / "no_early_effect";
        EmitDiagnostics noEarlyDiagnostics;
        EmitVerilatorRepCutPackage noEarlyEmitter(&noEarlyDiagnostics);
        EmitOptions noEarlyOptions;
        noEarlyOptions.outputDir = noEarlyRoot.string();
        noEarlyOptions.topOverrides = {"NoEarlyTop"};
        const EmitResult noEarlyResult = noEarlyEmitter.emit(buildNoEarlyEffectDesign(), noEarlyOptions);
        if (!noEarlyResult.success || noEarlyDiagnostics.hasError())
        {
            return fail("top-only/local effect package emit failed: " + diagnosticsSummary(noEarlyDiagnostics));
        }
        std::string noEarlySource;
        for (const auto &entry : std::filesystem::directory_iterator(noEarlyRoot))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp" &&
                entry.path().filename().string().rfind("wolvi_repcut_verilator_sim", 0) == 0)
            {
                noEarlySource += readFile(entry.path());
            }
        }
        if (!contains(noEarlySource,
                      "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_top_only_);") ||
            !contains(noEarlySource,
                      "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_local_);") ||
            !contains(noEarlySource,
                      "normal_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_top_only_);") ||
            !contains(noEarlySource,
                      "normal_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_final_local_);") ||
            contains(noEarlySource,
                     "early_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_top_only_);") ||
            contains(noEarlySource,
                     "early_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_local_);"))
        {
            return fail("top-only and locally consumed effects should remain in the normal/final phases");
        }
    }

    {
        const std::filesystem::path mixedRoot = artifactRoot / "mixed_cross_unit";
        EmitDiagnostics mixedDiagnostics;
        EmitVerilatorRepCutPackage mixedEmitter(&mixedDiagnostics);
        EmitOptions mixedOptions;
        mixedOptions.outputDir = mixedRoot.string();
        mixedOptions.topOverrides = {"MixedTop"};
        const EmitResult mixedResult = mixedEmitter.emit(buildMixedCrossUnitDesign(), mixedOptions);
        if (mixedResult.success ||
            !contains(diagnosticsSummary(mixedDiagnostics),
                      "Cross-unit output mixes result-producing effect and state provenance"))
        {
            return fail("mixed effect/state cross-unit output should fail fast");
        }
    }

    {
        const std::filesystem::path earlyChainRoot = artifactRoot / "early_chain";
        EmitDiagnostics earlyChainDiagnostics;
        EmitVerilatorRepCutPackage earlyChainEmitter(&earlyChainDiagnostics);
        EmitOptions earlyChainOptions;
        earlyChainOptions.outputDir = earlyChainRoot.string();
        earlyChainOptions.topOverrides = {"EarlyChainTop"};
        const EmitResult earlyChainResult = earlyChainEmitter.emit(buildEarlyChainDesign(), earlyChainOptions);
        if (earlyChainResult.success ||
            !contains(diagnosticsSummary(earlyChainDiagnostics),
                      "Early effect edge targets another early unit"))
        {
            return fail("early effect edge targeting another early unit should fail fast");
        }

        const std::filesystem::path normalToEarlyRoot = artifactRoot / "normal_to_early";
        EmitDiagnostics normalToEarlyDiagnostics;
        EmitVerilatorRepCutPackage normalToEarlyEmitter(&normalToEarlyDiagnostics);
        EmitOptions normalToEarlyOptions;
        normalToEarlyOptions.outputDir = normalToEarlyRoot.string();
        normalToEarlyOptions.topOverrides = {"NormalToEarlyTop"};
        const EmitResult normalToEarlyResult =
            normalToEarlyEmitter.emit(buildNormalToEarlyDesign(), normalToEarlyOptions);
        if (normalToEarlyResult.success ||
            !contains(diagnosticsSummary(normalToEarlyDiagnostics),
                      "Normal unit output feeds an early unit"))
        {
            return fail("normal-to-early dependency should fail fast");
        }
    }

    {
        const std::filesystem::path pureRoot = artifactRoot / "pure_system_function_effect_path";
        EmitDiagnostics pureDiagnostics;
        EmitVerilatorRepCutPackage pureEmitter(&pureDiagnostics);
        EmitOptions pureOptions;
        pureOptions.outputDir = pureRoot.string();
        pureOptions.topOverrides = {"PureTop"};
        const EmitResult pureResult =
            pureEmitter.emit(buildSystemFunctionEffectPathDesign(false), pureOptions);
        if (!pureResult.success || pureDiagnostics.hasError())
        {
            return fail("pure system function should propagate effect provenance: " +
                        diagnosticsSummary(pureDiagnostics));
        }
        std::string pureSource;
        for (const auto &entry : std::filesystem::directory_iterator(pureRoot))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp" &&
                entry.path().filename().string().rfind("wolvi_repcut_verilator_sim", 0) == 0)
            {
                pureSource += readFile(entry.path());
            }
        }
        if (!contains(pureSource,
                      "early_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_producer_);") ||
            !contains(pureSource,
                      "early_update_step_fns_.push_back(&WolviRepCutVerilatorSim::run_update_early_producer_);") ||
            contains(pureSource,
                     "normal_eval_step_fns_.push_back(&WolviRepCutVerilatorSim::run_eval_producer_);"))
        {
            return fail("pure system function effect path was not scheduled in the early phase");
        }
    }

    {
        const std::filesystem::path unsupportedRoot = artifactRoot / "unsupported_effect_path";
        EmitDiagnostics unsupportedDiagnostics;
        EmitVerilatorRepCutPackage unsupportedEmitter(&unsupportedDiagnostics);
        EmitOptions unsupportedOptions;
        unsupportedOptions.outputDir = unsupportedRoot.string();
        unsupportedOptions.topOverrides = {"UnsupportedTop"};
        const EmitResult unsupportedResult =
            unsupportedEmitter.emit(buildSystemFunctionEffectPathDesign(true), unsupportedOptions);
        if (unsupportedResult.success ||
            !contains(diagnosticsSummary(unsupportedDiagnostics),
                      "effect operations that cannot be ordered atomically"))
        {
            return fail("side-effecting system function consuming a DPI result should fail fast");
        }
    }

    return 0;
}
