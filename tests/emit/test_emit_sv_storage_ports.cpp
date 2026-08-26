#include "emit/system_verilog.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[emit_sv_storage_ports] " << message << '\n';
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

bool appearsInOrderWithinClockBlock(const std::string &output,
                                    std::string_view before,
                                    std::string_view after)
{
    const std::size_t beforePos = output.find(before);
    const std::size_t afterPos = output.find(after);
    if (beforePos == std::string::npos || afterPos == std::string::npos || beforePos >= afterPos)
    {
        return false;
    }
    const std::size_t blockStart = output.rfind("  always @(posedge clk) begin\n", beforePos);
    const std::size_t blockEnd = output.find("\n  end\n", beforePos);
    return blockStart != std::string::npos && blockEnd != std::string::npos && afterPos < blockEnd;
}

std::size_t countOccurrences(const std::string &text, std::string_view needle)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}

bool diagnosticsContain(const EmitDiagnostics &diagnostics, std::string_view needle)
{
    for (const auto &message : diagnostics.messages())
    {
        if (message.message.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

ValueId addConstant(Graph &graph, std::string_view name, int32_t width, std::string literal)
{
    const auto value =
        graph.createValue(graph.internSymbol(std::string(name)), width, false);
    const auto op =
        graph.createOperation(OperationKind::kConstant,
                              graph.internSymbol(std::string("_op_emit_const_") + std::string(name)));
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", std::move(literal));
    return value;
}

Design buildDesign()
{
    Design design;
    Graph &graph = design.createGraph("storage_ports");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);
    const auto dataMask = graph.createValue(graph.internSymbol("data_mask"), 8, false);
    const auto latchEn = graph.createValue(graph.internSymbol("latch_en"), 1, false);
    const auto latchData = graph.createValue(graph.internSymbol("latch_data"), 4, false);
    const auto wideData = graph.createValue(graph.internSymbol("wide_data"), 32, false);
    const auto narrowMask = graph.createValue(graph.internSymbol("narrow_mask"), 3, false);
    const auto orderedAddr = graph.createValue(graph.internSymbol("ordered_addr"), 2, false);
    const auto orderedHighData = graph.createValue(graph.internSymbol("ordered_high_data"), 8, false);
    const auto orderedLowData = graph.createValue(graph.internSymbol("ordered_low_data"), 8, false);

    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    graph.bindInputPort("data", data);
    graph.bindInputPort("data_mask", dataMask);
    graph.bindInputPort("latch_en", latchEn);
    graph.bindInputPort("latch_data", latchData);
    graph.bindInputPort("wide_data", wideData);
    graph.bindInputPort("narrow_mask", narrowMask);
    graph.bindInputPort("ordered_addr", orderedAddr);
    graph.bindInputPort("ordered_high_data", orderedHighData);
    graph.bindInputPort("ordered_low_data", orderedLowData);

    const auto fullMask = addConstant(graph, "mask_full", 8, "8'hFF");
    const auto halfMask = addConstant(graph, "mask_half", 8, "8'h0F");
    const auto condAlways = addConstant(graph, "cond_always", 1, "1'b1");
    const auto latchMask = addConstant(graph, "latch_mask", 4, "4'hF");

    const auto regFull = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol("reg_full"));
    graph.setAttr(regFull, "width", static_cast<int64_t>(8));
    graph.setAttr(regFull, "isSigned", false);

    const auto regMask = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol("reg_mask"));
    graph.setAttr(regMask, "width", static_cast<int64_t>(8));
    graph.setAttr(regMask, "isSigned", false);

    auto addDpiRegister = [&](std::string_view name)
    {
        const auto reg = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol(std::string(name)));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", false);
    };
    addDpiRegister("dpi_output_a_reg");
    addDpiRegister("dpi_output_b_reg");
    addDpiRegister("dpi_inout_a_reg");
    addDpiRegister("dpi_inout_b_reg");
    addDpiRegister("dpi_mixed_return_reg");
    addDpiRegister("dpi_mixed_output_reg");
    addDpiRegister("dpi_return_only_reg");

    const auto latch = graph.createOperation(OperationKind::kLatch,
                                             graph.internSymbol("lat_a"));
    graph.setAttr(latch, "width", static_cast<int64_t>(4));
    graph.setAttr(latch, "isSigned", false);

    const auto orderedMemory = graph.createOperation(OperationKind::kMemory,
                                                     graph.internSymbol("ordered_mem"));
    graph.setAttr(orderedMemory, "width", static_cast<int64_t>(8));
    graph.setAttr(orderedMemory, "row", static_cast<int64_t>(4));
    graph.setAttr(orderedMemory, "isSigned", false);

    const auto regRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                               graph.internSymbol("_op_emit_reg_read"));
    const auto regReadValue =
        graph.createValue(graph.internSymbol("reg_full_q"), 8, false);
    graph.addResult(regRead, regReadValue);
    graph.setAttr(regRead, "regSymbol", std::string("reg_full"));

    const auto latchRead = graph.createOperation(OperationKind::kLatchReadPort,
                                                 graph.internSymbol("_op_emit_latch_read"));
    const auto latchReadValue =
        graph.createValue(graph.internSymbol("lat_q"), 4, false);
    graph.addResult(latchRead, latchReadValue);
    graph.setAttr(latchRead, "latchSymbol", std::string("lat_a"));

    const auto maskedNarrowOp = graph.createOperation(OperationKind::kAnd,
                                                      graph.internSymbol("_op_emit_masked_narrow"));
    const auto maskedNarrow =
        graph.createValue(graph.internSymbol("masked_narrow"), 3, false);
    graph.addOperand(maskedNarrowOp, wideData);
    graph.addOperand(maskedNarrowOp, narrowMask);
    graph.addResult(maskedNarrowOp, maskedNarrow);
    graph.bindOutputPort("masked_narrow", maskedNarrow);

    const auto regWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                graph.internSymbol("_op_emit_reg_write"));
    graph.addOperand(regWrite, en);
    graph.addOperand(regWrite, data);
    graph.addOperand(regWrite, fullMask);
    graph.addOperand(regWrite, clk);
    graph.setAttr(regWrite, "regSymbol", std::string("reg_full"));
    graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});

    const auto regMaskWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                    graph.internSymbol("_op_emit_reg_mask_write"));
    graph.addOperand(regMaskWrite, condAlways);
    graph.addOperand(regMaskWrite, dataMask);
    graph.addOperand(regMaskWrite, halfMask);
    graph.addOperand(regMaskWrite, clk);
    graph.setAttr(regMaskWrite, "regSymbol", std::string("reg_mask"));
    graph.setAttr(regMaskWrite, "eventEdge", std::vector<std::string>{"posedge"});

    const auto orderedHighWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                        graph.internSymbol("_op_ordered_high_write"));
    graph.addOperand(orderedHighWrite, en);
    graph.addOperand(orderedHighWrite, orderedAddr);
    graph.addOperand(orderedHighWrite, orderedHighData);
    graph.addOperand(orderedHighWrite, fullMask);
    graph.addOperand(orderedHighWrite, clk);
    graph.setAttr(orderedHighWrite, "memSymbol", std::string("ordered_mem"));
    graph.setAttr(orderedHighWrite, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(orderedHighWrite, kMemoryWritePriorityGroupAttr, std::string("ordered_mem_writes"));
    graph.setAttr(orderedHighWrite, kMemoryWritePriorityAttr, int64_t{0});

    const auto orderedLowWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                       graph.internSymbol("_op_ordered_low_write"));
    graph.addOperand(orderedLowWrite, en);
    graph.addOperand(orderedLowWrite, orderedAddr);
    graph.addOperand(orderedLowWrite, orderedLowData);
    graph.addOperand(orderedLowWrite, fullMask);
    graph.addOperand(orderedLowWrite, clk);
    graph.setAttr(orderedLowWrite, "memSymbol", std::string("ordered_mem"));
    graph.setAttr(orderedLowWrite, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(orderedLowWrite, kMemoryWritePriorityGroupAttr, std::string("ordered_mem_writes"));
    graph.setAttr(orderedLowWrite, kMemoryWritePriorityAttr, int64_t{1});

    const auto dpiOutputA = graph.createValue(graph.internSymbol("dpi_output_a_value"), 8, false);
    const auto dpiOutputB = graph.createValue(graph.internSymbol("dpi_output_b_value"), 8, false);
    const auto dpiOutputBNext = graph.createValue(graph.internSymbol("dpi_output_b_next"), 8, false);
    const auto dpiInoutA = graph.createValue(graph.internSymbol("dpi_inout_a_value"), 8, false);
    const auto dpiInoutB = graph.createValue(graph.internSymbol("dpi_inout_b_value"), 8, false);
    const auto dpiMixedReturn = graph.createValue(graph.internSymbol("dpi_mixed_return_value"), 8, false);
    const auto dpiMixedOutput = graph.createValue(graph.internSymbol("dpi_mixed_output_value"), 8, false);
    const auto dpiReturnOnly = graph.createValue(graph.internSymbol("dpi_return_only_value"), 8, false);
    const auto dpiReturnOnlyTwice = graph.createValue(graph.internSymbol("dpi_return_only_twice"), 8, false);

    const auto dpiReturnOnlyAdd = graph.createOperation(OperationKind::kAdd,
                                                        graph.internSymbol("_op_dpi_return_only_add"));
    graph.addOperand(dpiReturnOnlyAdd, dpiReturnOnly);
    graph.addOperand(dpiReturnOnlyAdd, dpiReturnOnly);
    graph.addResult(dpiReturnOnlyAdd, dpiReturnOnlyTwice);
    graph.bindOutputPort("dpi_return_only_observed", dpiReturnOnly);

    auto addDpiSink = [&](std::string_view opName,
                          std::string_view regName,
                          ValueId nextValue)
    {
        const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                                graph.internSymbol(std::string(opName)));
        graph.addOperand(sink, en);
        graph.addOperand(sink, nextValue);
        graph.addOperand(sink, fullMask);
        graph.addOperand(sink, clk);
        graph.setAttr(sink, "regSymbol", std::string(regName));
        graph.setAttr(sink, "eventEdge", std::vector<std::string>{"posedge"});
    };

    // Create consumers first to prove DPI/state ordering does not depend on graph traversal order.
    const auto dpiOutputBAdd = graph.createOperation(OperationKind::kAdd,
                                                     graph.internSymbol("_op_dpi_output_b_add"));
    graph.addOperand(dpiOutputBAdd, dpiOutputB);
    graph.addOperand(dpiOutputBAdd, data);
    graph.addResult(dpiOutputBAdd, dpiOutputBNext);
    addDpiSink("_op_dpi_output_a_sink", "dpi_output_a_reg", dpiOutputA);
    addDpiSink("_op_dpi_output_b_sink", "dpi_output_b_reg", dpiOutputBNext);
    addDpiSink("_op_dpi_inout_a_sink", "dpi_inout_a_reg", dpiInoutA);
    addDpiSink("_op_dpi_inout_b_sink", "dpi_inout_b_reg", dpiInoutB);
    addDpiSink("_op_dpi_mixed_return_sink", "dpi_mixed_return_reg", dpiMixedReturn);
    addDpiSink("_op_dpi_mixed_output_sink", "dpi_mixed_output_reg", dpiMixedOutput);
    addDpiSink("_op_dpi_return_only_sink", "dpi_return_only_reg", dpiReturnOnlyTwice);

    const auto latchWrite = graph.createOperation(OperationKind::kLatchWritePort,
                                                  graph.internSymbol("_op_emit_latch_write"));
    graph.addOperand(latchWrite, latchEn);
    graph.addOperand(latchWrite, latchData);
    graph.addOperand(latchWrite, latchMask);
    graph.setAttr(latchWrite, "latchSymbol", std::string("lat_a"));

    auto addDpiImport = [&](std::string_view name,
                            std::vector<std::string> directions,
                            std::vector<std::string> names,
                            bool hasReturn)
    {
        const auto import = graph.createOperation(OperationKind::kDpicImport,
                                                  graph.internSymbol(std::string(name)));
        graph.setAttr(import, "argsDirection", directions);
        graph.setAttr(import, "argsWidth", std::vector<int64_t>(directions.size(), 8));
        graph.setAttr(import, "argsName", std::move(names));
        graph.setAttr(import, "argsSigned", std::vector<bool>(directions.size(), false));
        graph.setAttr(import, "argsType", std::vector<std::string>(directions.size(), "logic"));
        graph.setAttr(import, "hasReturn", hasReturn);
        if (hasReturn)
        {
            graph.setAttr(import, "returnWidth", static_cast<int64_t>(8));
            graph.setAttr(import, "returnSigned", false);
            graph.setAttr(import, "returnType", std::string("bit"));
        }
    };
    auto setDpiCallAttrs = [&](OperationId call,
                               std::string target,
                               std::vector<std::string> inputs,
                               std::vector<std::string> outputs,
                               std::vector<std::string> inouts,
                               bool hasReturn)
    {
        graph.setAttr(call, "targetImportSymbol", std::move(target));
        graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(call, "inArgName", std::move(inputs));
        graph.setAttr(call, "outArgName", std::move(outputs));
        graph.setAttr(call, "inoutArgName", std::move(inouts));
        graph.setAttr(call, "hasReturn", hasReturn);
    };

    addDpiImport("dpi_output_atomic",
                 {"input", "output", "output"},
                 {"src", "dst_a", "dst_b"}, false);
    const auto dpiOutputCall = graph.createOperation(OperationKind::kDpicCall,
                                                     graph.internSymbol("_op_dpi_output_atomic"));
    graph.addOperand(dpiOutputCall, en);
    graph.addOperand(dpiOutputCall, data);
    graph.addOperand(dpiOutputCall, clk);
    graph.addResult(dpiOutputCall, dpiOutputA);
    graph.addResult(dpiOutputCall, dpiOutputB);
    setDpiCallAttrs(dpiOutputCall, "dpi_output_atomic", {"src"}, {"dst_a", "dst_b"}, {}, false);

    addDpiImport("dpi_inout_atomic",
                 {"inout", "inout"},
                 {"state_a", "state_b"}, false);
    const auto dpiInoutCall = graph.createOperation(OperationKind::kDpicCall,
                                                    graph.internSymbol("_op_dpi_inout_atomic"));
    graph.addOperand(dpiInoutCall, en);
    graph.addOperand(dpiInoutCall, data);
    graph.addOperand(dpiInoutCall, dataMask);
    graph.addOperand(dpiInoutCall, clk);
    graph.addResult(dpiInoutCall, dpiInoutA);
    graph.addResult(dpiInoutCall, dpiInoutB);
    setDpiCallAttrs(dpiInoutCall, "dpi_inout_atomic", {}, {}, {"state_a", "state_b"}, false);

    addDpiImport("dpi_mixed_atomic",
                 {"input", "output"},
                 {"src", "dst"}, true);
    const auto dpiMixedCall = graph.createOperation(OperationKind::kDpicCall,
                                                    graph.internSymbol("_op_dpi_mixed_atomic"));
    graph.addOperand(dpiMixedCall, en);
    graph.addOperand(dpiMixedCall, data);
    graph.addOperand(dpiMixedCall, clk);
    graph.addResult(dpiMixedCall, dpiMixedReturn);
    graph.addResult(dpiMixedCall, dpiMixedOutput);
    setDpiCallAttrs(dpiMixedCall, "dpi_mixed_atomic", {"src"}, {"dst"}, {}, true);

    addDpiImport("dpi_return_only_atomic", {"input"}, {"src"}, true);
    const auto dpiReturnOnlyCall = graph.createOperation(OperationKind::kDpicCall,
                                                         graph.internSymbol("_op_dpi_return_only_atomic"));
    graph.addOperand(dpiReturnOnlyCall, en);
    graph.addOperand(dpiReturnOnlyCall, data);
    graph.addOperand(dpiReturnOnlyCall, clk);
    graph.addResult(dpiReturnOnlyCall, dpiReturnOnly);
    setDpiCallAttrs(dpiReturnOnlyCall, "dpi_return_only_atomic", {"src"}, {}, {}, true);

    return design;
}

Design buildUnsupportedDpiStateDesign()
{
    Design design;
    Graph &graph = design.createGraph("unsupported_dpi_state_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    const auto fullMask = addConstant(graph, "mask_full", 8, "8'hFF");

    const auto reg = graph.createOperation(OperationKind::kRegister,
                                           graph.internSymbol("dpi_unsupported_reg"));
    graph.setAttr(reg, "width", static_cast<int64_t>(8));
    graph.setAttr(reg, "isSigned", false);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_unsupported_output"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"data"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto dpiResult = graph.createValue(graph.internSymbol("dpi_unsupported_value"), 8, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_unsupported_output"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiResult);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_unsupported_output"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"data"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    const auto converted = graph.createValue(graph.internSymbol("dpi_unsupported_converted"), 8, false);
    const auto systemFunction = graph.createOperation(OperationKind::kSystemFunction,
                                                      graph.internSymbol("_op_dpi_unsupported_system_function"));
    graph.addOperand(systemFunction, dpiResult);
    graph.addResult(systemFunction, converted);
    graph.setAttr(systemFunction, "name", std::string("unsigned"));
    graph.setAttr(systemFunction, "hasSideEffects", false);

    const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                            graph.internSymbol("_op_dpi_unsupported_sink"));
    graph.addOperand(sink, en);
    graph.addOperand(sink, converted);
    graph.addOperand(sink, fullMask);
    graph.addOperand(sink, clk);
    graph.setAttr(sink, "regSymbol", std::string("dpi_unsupported_reg"));
    graph.setAttr(sink, "eventEdge", std::vector<std::string>{"posedge"});

    return design;
}

Design buildDpiMergedGuardDesign(bool mismatchedEvent = false)
{
    Design design;
    Graph &graph = design.createGraph("dpi_merged_guard_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    const auto reset = graph.createValue(graph.internSymbol("reset"), 1, false);
    const auto data = graph.createValue(graph.internSymbol("data"), 8, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    graph.bindInputPort("reset", reset);
    graph.bindInputPort("data", data);

    const auto fullMask = addConstant(graph, "merged_guard_mask", 8, "8'hFF");
    const auto resetValue = addConstant(graph, "merged_guard_reset_value", 8, "8'h00");
    const auto sinkCond = graph.createValue(graph.internSymbol("dpi_merged_guard_sink_cond"), 1, false);
    const auto dpiResult = graph.createValue(graph.internSymbol("dpi_merged_guard_value"), 8, false);
    const auto nextValue = graph.createValue(graph.internSymbol("dpi_merged_guard_next"), 8, false);

    const auto reg = graph.createOperation(OperationKind::kRegister,
                                           graph.internSymbol("dpi_merged_guard_reg"));
    graph.setAttr(reg, "width", static_cast<int64_t>(8));
    graph.setAttr(reg, "isSigned", false);

    // Build the consumer before the producer to make ordering independent of traversal order.
    const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                            graph.internSymbol("_op_dpi_merged_guard_sink"));
    graph.addOperand(sink, sinkCond);
    graph.addOperand(sink, nextValue);
    graph.addOperand(sink, fullMask);
    graph.addOperand(sink, clk);
    graph.setAttr(sink, "regSymbol", std::string("dpi_merged_guard_reg"));
    graph.setAttr(sink, "eventEdge",
                  std::vector<std::string>{mismatchedEvent ? "negedge" : "posedge"});

    const auto select = graph.createOperation(OperationKind::kMux,
                                              graph.internSymbol("_op_dpi_merged_guard_select"));
    graph.addOperand(select, en);
    graph.addOperand(select, dpiResult);
    graph.addOperand(select, resetValue);
    graph.addResult(select, nextValue);

    const auto mergeGuard = graph.createOperation(OperationKind::kLogicOr,
                                                  graph.internSymbol("_op_dpi_merged_guard_or"));
    graph.addOperand(mergeGuard, reset);
    graph.addOperand(mergeGuard, en);
    graph.addResult(mergeGuard, sinkCond);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_merged_guard"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"input", "output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8, 8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"src", "dst"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic", "logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_merged_guard_call"));
    graph.addOperand(call, en);
    graph.addOperand(call, data);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiResult);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_merged_guard"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{"src"});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"dst"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    return design;
}

Design buildDpiDynamicSliceDesign()
{
    Design design;
    Graph &graph = design.createGraph("dpi_dynamic_slice_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    const auto fullMask = addConstant(graph, "dpi_slice_mask", 1, "1'b1");
    const auto outOfRangeIndex = addConstant(graph, "dpi_const_slice_index", 8, "8'd4");

    for (const std::string &name : {"dpi_slice_reg", "dpi_const_slice_reg"})
    {
        const auto reg = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol(name));
        graph.setAttr(reg, "width", static_cast<int64_t>(1));
        graph.setAttr(reg, "isSigned", false);
    }

    const auto dpiBase = graph.createValue(graph.internSymbol("dpi_slice_base"), 4, false);
    const auto dpiIndex = graph.createValue(graph.internSymbol("dpi_slice_index"), 8, false);
    const auto dpiConstBase = graph.createValue(graph.internSymbol("dpi_const_slice_base"), 4, false);
    const auto dpiBit = graph.createValue(graph.internSymbol("dpi_slice_bit"), 1, false);
    const auto dpiConstBit = graph.createValue(graph.internSymbol("dpi_const_slice_bit"), 1, false);

    const auto slice = graph.createOperation(OperationKind::kSliceDynamic,
                                             graph.internSymbol("_op_dpi_dynamic_slice"));
    graph.addOperand(slice, dpiBase);
    graph.addOperand(slice, dpiIndex);
    graph.addResult(slice, dpiBit);
    graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(1));
    const auto constSlice = graph.createOperation(OperationKind::kSliceDynamic,
                                                  graph.internSymbol("_op_dpi_const_dynamic_slice"));
    graph.addOperand(constSlice, dpiConstBase);
    graph.addOperand(constSlice, outOfRangeIndex);
    graph.addResult(constSlice, dpiConstBit);
    graph.setAttr(constSlice, "sliceWidth", static_cast<int64_t>(1));

    auto addSink = [&](std::string_view opName, std::string_view regName, ValueId value)
    {
        const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                                graph.internSymbol(std::string(opName)));
        graph.addOperand(sink, en);
        graph.addOperand(sink, value);
        graph.addOperand(sink, fullMask);
        graph.addOperand(sink, clk);
        graph.setAttr(sink, "regSymbol", std::string(regName));
        graph.setAttr(sink, "eventEdge", std::vector<std::string>{"posedge"});
    };
    addSink("_op_dpi_slice_sink", "dpi_slice_reg", dpiBit);
    addSink("_op_dpi_const_slice_sink", "dpi_const_slice_reg", dpiConstBit);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_slice_atomic"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output", "output", "output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{4, 8, 4});
    graph.setAttr(import, "argsName", std::vector<std::string>{"base", "index", "const_base"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false, false, false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic", "logic", "logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_slice_atomic"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiBase);
    graph.addResult(call, dpiIndex);
    graph.addResult(call, dpiConstBase);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_slice_atomic"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"base", "index", "const_base"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    return design;
}

Design buildDpiNarrowAggregateDesign()
{
    Design design;
    Graph &graph = design.createGraph("dpi_narrow_aggregate_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    const auto fullMask = addConstant(graph, "dpi_narrow_mask", 1, "1'b1");
    const auto prefix = addConstant(graph, "dpi_narrow_prefix", 8, "8'h01");
    const auto zero = addConstant(graph, "dpi_narrow_zero", 8, "8'h00");

    for (const std::string &name : {"dpi_concat_reg", "dpi_replicate_reg"})
    {
        const auto reg = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol(name));
        graph.setAttr(reg, "width", static_cast<int64_t>(1));
        graph.setAttr(reg, "isSigned", false);
    }

    const auto concatInput = graph.createValue(graph.internSymbol("dpi_narrow_concat"), 8, false);
    const auto replicateInput = graph.createValue(graph.internSymbol("dpi_narrow_replicate"), 8, false);
    const auto concatValue = graph.createValue(graph.internSymbol("dpi_narrow_concat_value"), 8, false);
    const auto replicateValue = graph.createValue(graph.internSymbol("dpi_narrow_replicate_value"), 8, false);
    const auto concatEq = graph.createValue(graph.internSymbol("dpi_narrow_concat_eq"), 1, false);
    const auto replicateEq = graph.createValue(graph.internSymbol("dpi_narrow_replicate_eq"), 1, false);

    const auto concat = graph.createOperation(OperationKind::kConcat,
                                              graph.internSymbol("_op_dpi_narrow_concat"));
    graph.addOperand(concat, prefix);
    graph.addOperand(concat, concatInput);
    graph.addResult(concat, concatValue);
    const auto replicate = graph.createOperation(OperationKind::kReplicate,
                                                 graph.internSymbol("_op_dpi_narrow_replicate"));
    graph.addOperand(replicate, replicateInput);
    graph.addResult(replicate, replicateValue);
    graph.setAttr(replicate, "rep", static_cast<int64_t>(2));

    const auto concatCompare = graph.createOperation(OperationKind::kEq,
                                                     graph.internSymbol("_op_dpi_concat_compare"));
    graph.addOperand(concatCompare, concatValue);
    graph.addOperand(concatCompare, zero);
    graph.addResult(concatCompare, concatEq);
    const auto replicateCompare = graph.createOperation(OperationKind::kEq,
                                                        graph.internSymbol("_op_dpi_replicate_compare"));
    graph.addOperand(replicateCompare, replicateValue);
    graph.addOperand(replicateCompare, zero);
    graph.addResult(replicateCompare, replicateEq);

    auto addSink = [&](std::string_view opName, std::string_view regName, ValueId value)
    {
        const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                                graph.internSymbol(std::string(opName)));
        graph.addOperand(sink, en);
        graph.addOperand(sink, value);
        graph.addOperand(sink, fullMask);
        graph.addOperand(sink, clk);
        graph.setAttr(sink, "regSymbol", std::string(regName));
        graph.setAttr(sink, "eventEdge", std::vector<std::string>{"posedge"});
    };
    addSink("_op_dpi_concat_sink", "dpi_concat_reg", concatEq);
    addSink("_op_dpi_replicate_sink", "dpi_replicate_reg", replicateEq);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_narrow_atomic"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output", "output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8, 8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"concat_data", "replicate_data"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic", "logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_narrow_atomic"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, concatInput);
    graph.addResult(call, replicateInput);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_narrow_atomic"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"concat_data", "replicate_data"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    return design;
}

Design buildDpiSignedAggregateDesign()
{
    Design design;
    Graph &graph = design.createGraph("dpi_signed_aggregate_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);
    const auto fullMask = addConstant(graph, "dpi_signed_mask", 8, "8'hFF");
    const auto shiftAmount = addConstant(graph, "dpi_signed_shift", 8, "8'd1");

    for (const auto &[name, isSigned] :
         std::vector<std::pair<std::string, bool>>{{"dpi_signed_concat_reg", true},
                                                   {"dpi_signed_replicate_reg", true},
                                                   {"dpi_unsigned_boundary_reg", false}})
    {
        const auto reg = graph.createOperation(OperationKind::kRegister,
                                               graph.internSymbol(name));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", isSigned);
    }

    const auto concatInput = graph.createValue(graph.internSymbol("dpi_signed_concat"), 8, true);
    const auto replicateInput = graph.createValue(graph.internSymbol("dpi_signed_replicate"), 8, true);
    const auto unsignedBoundaryInput =
        graph.createValue(graph.internSymbol("dpi_unsigned_boundary"), 8, true);
    const auto concatValue = graph.createValue(graph.internSymbol("dpi_signed_concat_value"), 8, true);
    const auto replicateValue = graph.createValue(graph.internSymbol("dpi_signed_replicate_value"), 8, true);
    const auto concatShifted = graph.createValue(graph.internSymbol("dpi_signed_concat_shifted"), 8, true);
    const auto replicateShifted = graph.createValue(graph.internSymbol("dpi_signed_replicate_shifted"), 8, true);
    const auto unsignedAssigned = graph.createValue(graph.internSymbol("dpi_unsigned_assigned"), 8, false);
    const auto unsignedShifted = graph.createValue(graph.internSymbol("dpi_unsigned_shifted"), 8, false);

    const auto concat = graph.createOperation(OperationKind::kConcat,
                                              graph.internSymbol("_op_dpi_signed_concat"));
    graph.addOperand(concat, concatInput);
    graph.addResult(concat, concatValue);
    const auto replicate = graph.createOperation(OperationKind::kReplicate,
                                                 graph.internSymbol("_op_dpi_signed_replicate"));
    graph.addOperand(replicate, replicateInput);
    graph.addResult(replicate, replicateValue);
    graph.setAttr(replicate, "rep", static_cast<int64_t>(1));

    const auto concatShift = graph.createOperation(OperationKind::kAShr,
                                                   graph.internSymbol("_op_dpi_signed_concat_shift"));
    graph.addOperand(concatShift, concatValue);
    graph.addOperand(concatShift, shiftAmount);
    graph.addResult(concatShift, concatShifted);
    const auto replicateShift = graph.createOperation(OperationKind::kAShr,
                                                      graph.internSymbol("_op_dpi_signed_replicate_shift"));
    graph.addOperand(replicateShift, replicateValue);
    graph.addOperand(replicateShift, shiftAmount);
    graph.addResult(replicateShift, replicateShifted);
    const auto unsignedAssign = graph.createOperation(OperationKind::kAssign,
                                                      graph.internSymbol("_op_dpi_unsigned_assign"));
    graph.addOperand(unsignedAssign, unsignedBoundaryInput);
    graph.addResult(unsignedAssign, unsignedAssigned);
    const auto unsignedShift = graph.createOperation(OperationKind::kAShr,
                                                     graph.internSymbol("_op_dpi_unsigned_shift"));
    graph.addOperand(unsignedShift, unsignedAssigned);
    graph.addOperand(unsignedShift, shiftAmount);
    graph.addResult(unsignedShift, unsignedShifted);

    auto addSink = [&](std::string_view opName, std::string_view regName, ValueId value)
    {
        const auto sink = graph.createOperation(OperationKind::kRegisterWritePort,
                                                graph.internSymbol(std::string(opName)));
        graph.addOperand(sink, en);
        graph.addOperand(sink, value);
        graph.addOperand(sink, fullMask);
        graph.addOperand(sink, clk);
        graph.setAttr(sink, "regSymbol", std::string(regName));
        graph.setAttr(sink, "eventEdge", std::vector<std::string>{"posedge"});
    };
    addSink("_op_dpi_signed_concat_sink", "dpi_signed_concat_reg", concatShifted);
    addSink("_op_dpi_signed_replicate_sink", "dpi_signed_replicate_reg", replicateShifted);
    addSink("_op_dpi_unsigned_boundary_sink", "dpi_unsigned_boundary_reg", unsignedShifted);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_signed_atomic"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output", "output", "output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8, 8, 8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"concat_data", "replicate_data", "unsigned_data"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{true, true, true});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic", "logic", "logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_signed_atomic"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, concatInput);
    graph.addResult(call, replicateInput);
    graph.addResult(call, unsignedBoundaryInput);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_signed_atomic"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName",
                  std::vector<std::string>{"concat_data", "replicate_data", "unsigned_data"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    return design;
}

Design buildDpiMemoryFillDesign()
{
    Design design;
    Graph &graph = design.createGraph("dpi_memory_fill_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);

    const auto memory = graph.createOperation(OperationKind::kMemory,
                                              graph.internSymbol("dpi_fill_memory"));
    graph.setAttr(memory, "width", static_cast<int64_t>(8));
    graph.setAttr(memory, "row", static_cast<int64_t>(4));
    graph.setAttr(memory, "isSigned", false);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_fill_output"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"data"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto dpiResult = graph.createValue(graph.internSymbol("dpi_fill_value"), 8, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_fill_output"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiResult);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_fill_output"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"data"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    const auto fill = graph.createOperation(OperationKind::kMemoryFillPort,
                                            graph.internSymbol("_op_dpi_memory_fill"));
    graph.addOperand(fill, en);
    graph.addOperand(fill, dpiResult);
    graph.addOperand(fill, clk);
    graph.setAttr(fill, "memSymbol", std::string("dpi_fill_memory"));
    graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});

    return design;
}

Design buildDpiEffectConsumerDesign()
{
    Design design;
    Graph &graph = design.createGraph("dpi_effect_consumer_path");
    design.markAsTop(graph.symbol());

    const auto clk = graph.createValue(graph.internSymbol("clk"), 1, false);
    const auto en = graph.createValue(graph.internSymbol("en"), 1, false);
    graph.bindInputPort("clk", clk);
    graph.bindInputPort("en", en);

    const auto import = graph.createOperation(OperationKind::kDpicImport,
                                              graph.internSymbol("dpi_effect_output"));
    graph.setAttr(import, "argsDirection", std::vector<std::string>{"output"});
    graph.setAttr(import, "argsWidth", std::vector<int64_t>{8});
    graph.setAttr(import, "argsName", std::vector<std::string>{"data"});
    graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    graph.setAttr(import, "argsType", std::vector<std::string>{"logic"});
    graph.setAttr(import, "hasReturn", false);

    const auto dpiResult = graph.createValue(graph.internSymbol("dpi_effect_value"), 8, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall,
                                            graph.internSymbol("_op_dpi_effect_output"));
    graph.addOperand(call, en);
    graph.addOperand(call, clk);
    graph.addResult(call, dpiResult);
    graph.setAttr(call, "targetImportSymbol", std::string("dpi_effect_output"));
    graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    graph.setAttr(call, "inArgName", std::vector<std::string>{});
    graph.setAttr(call, "outArgName", std::vector<std::string>{"data"});
    graph.setAttr(call, "inoutArgName", std::vector<std::string>{});
    graph.setAttr(call, "hasReturn", false);

    const auto effectArg = graph.createValue(graph.internSymbol("dpi_effect_arg"), 8, false);
    const auto add = graph.createOperation(OperationKind::kAdd,
                                           graph.internSymbol("_op_dpi_effect_add"));
    graph.addOperand(add, dpiResult);
    graph.addOperand(add, dpiResult);
    graph.addResult(add, effectArg);

    const auto task = graph.createOperation(OperationKind::kSystemTask,
                                            graph.internSymbol("_op_dpi_effect_task"));
    graph.addOperand(task, en);
    graph.addOperand(task, effectArg);
    graph.addOperand(task, clk);
    graph.setAttr(task, "name", std::string("display"));
    graph.setAttr(task, "procKind", std::string("always_ff"));
    graph.setAttr(task, "hasTiming", false);
    graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});

    return design;
}

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    Design design = buildDesign();

    EmitDiagnostics diag;
    EmitSystemVerilog emitter(&diag);

    EmitOptions options;
    options.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    options.outputFilename = std::string("emit_storage_ports.sv");

    EmitResult result = emitter.emit(design, options);
    if (!result.success)
    {
        return fail("EmitSystemVerilog failed");
    }
    if (diag.hasError())
    {
        return fail("EmitSystemVerilog reported diagnostics errors");
    }
    if (result.artifacts.empty())
    {
        return fail("EmitSystemVerilog did not report artifacts");
    }

    const std::filesystem::path outputPath = result.artifacts.front();
    const std::string output = readFile(outputPath);
    if (output.empty())
    {
        return fail("Failed to read emitted SystemVerilog file");
    }

    if (output.find("reg [7:0] reg_full;") == std::string::npos)
    {
        return fail("Missing reg_full declaration");
    }
    if (output.find("reg [7:0] reg_mask;") == std::string::npos)
    {
        return fail("Missing reg_mask declaration");
    }
    if (output.find("reg [3:0] lat_a;") == std::string::npos)
    {
        return fail("Missing lat_a declaration");
    }
    if (output.find("assign reg_full_q = reg_full;") == std::string::npos)
    {
        return fail("Missing reg_full read port assign");
    }
    if (output.find("assign lat_q = lat_a;") == std::string::npos)
    {
        return fail("Missing latch read port assign");
    }
    if (output.find("always @(posedge clk)") == std::string::npos)
    {
        return fail("Missing sequential always block");
    }
    if (output.find("reg_full <= data;") == std::string::npos)
    {
        return fail("Missing reg_full write");
    }
    if (output.find("reg_mask[0] <= data_mask[0];") == std::string::npos)
    {
        return fail("Missing masked reg_mask write");
    }
    if (output.find("always_latch begin") == std::string::npos)
    {
        return fail("Missing always_latch block");
    }
    if (output.find("lat_a = latch_data;") == std::string::npos)
    {
        return fail("Missing latch write");
    }
    if (output.find("assign masked_narrow = wide_data[2:0] & narrow_mask;") == std::string::npos)
    {
        return fail("Missing width-trimmed bitwise and emit");
    }
    const std::size_t lowWrite = output.find("ordered_mem[ordered_addr] <= ordered_low_data;");
    const std::size_t highWrite = output.find("ordered_mem[ordered_addr] <= ordered_high_data;");
    if (lowWrite == std::string::npos || highWrite == std::string::npos || lowWrite >= highWrite)
    {
        return fail("Ordered memory writes were not emitted from low to high priority");
    }

    const std::string outputCall =
        "dpi_output_atomic(data, dpi_output_a_value_intm, dpi_output_b_value_intm);";
    if (!appearsInOrderWithinClockBlock(output, outputCall,
                                        "dpi_output_a_reg <= dpi_output_a_value_intm;") ||
        !appearsInOrderWithinClockBlock(output, outputCall,
                                        "dpi_output_b_reg <= $unsigned(dpi_output_b_value_intm + data);"))
    {
        return fail("Void DPI output call was not ordered before all dependent state sinks");
    }
    if (output.find("dpi_output_a_reg <= dpi_output_a_value;") != std::string::npos ||
        output.find("dpi_output_b_reg <= dpi_output_b_next;") != std::string::npos)
    {
        return fail("Void DPI output state sink still reads the continuous-assign result");
    }

    const std::string inoutCall =
        "dpi_inout_atomic(dpi_inout_a_value_intm, dpi_inout_b_value_intm);";
    if (!appearsInOrderWithinClockBlock(output, "dpi_inout_a_value_intm = data;", inoutCall) ||
        !appearsInOrderWithinClockBlock(output, "dpi_inout_b_value_intm = data_mask;", inoutCall) ||
        !appearsInOrderWithinClockBlock(output, inoutCall,
                                        "dpi_inout_a_reg <= dpi_inout_a_value_intm;") ||
        !appearsInOrderWithinClockBlock(output, inoutCall,
                                        "dpi_inout_b_reg <= dpi_inout_b_value_intm;"))
    {
        return fail("Void DPI inout preload/call/state order is not atomic");
    }

    const std::string mixedCall =
        "dpi_mixed_return_value_intm = dpi_mixed_atomic(data, dpi_mixed_output_value_intm);";
    if (!appearsInOrderWithinClockBlock(output, mixedCall,
                                        "dpi_mixed_return_reg <= dpi_mixed_return_value_intm;") ||
        !appearsInOrderWithinClockBlock(output, mixedCall,
                                        "dpi_mixed_output_reg <= dpi_mixed_output_value_intm;"))
    {
        return fail("DPI return/output call was not ordered before all dependent state sinks");
    }

    const std::string returnOnlyCall =
        "dpi_return_only_value_intm = dpi_return_only_atomic(data);";
    if (!appearsInOrderWithinClockBlock(output, returnOnlyCall,
                                        "dpi_return_only_reg <= $unsigned(dpi_return_only_value_intm + "
                                        "dpi_return_only_value_intm);") ||
        countOccurrences(output, "dpi_return_only_atomic(data)") != 1 ||
        output.find("assign dpi_return_only_value = dpi_return_only_value_intm;") == std::string::npos)
    {
        return fail("Return-only DPI call was not emitted once before all state/non-state consumers");
    }

    Design mergedGuardDesign = buildDpiMergedGuardDesign();
    EmitDiagnostics mergedGuardDiagnostics;
    EmitSystemVerilog mergedGuardEmitter(&mergedGuardDiagnostics);
    EmitOptions mergedGuardOptions;
    mergedGuardOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    mergedGuardOptions.outputFilename = std::string("emit_dpi_merged_guard_path.sv");
    const EmitResult mergedGuardResult =
        mergedGuardEmitter.emit(mergedGuardDesign, mergedGuardOptions);
    if (!mergedGuardResult.success || mergedGuardDiagnostics.hasError())
    {
        return fail("DPI result with a broader merged state guard failed to emit");
    }
    const std::string mergedGuardOutput =
        readFile(std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                 "emit_dpi_merged_guard_path.sv");
    const std::string mergedGuardCall =
        "dpi_merged_guard(data, dpi_merged_guard_value_intm);";
    if (!appearsInOrderWithinClockBlock(mergedGuardOutput,
                                        mergedGuardCall,
                                        "dpi_merged_guard_reg <= ") ||
        countOccurrences(mergedGuardOutput, mergedGuardCall) != 1 ||
        mergedGuardOutput.find("reset || en") == std::string::npos ||
        mergedGuardOutput.find("en ? dpi_merged_guard_value_intm") == std::string::npos)
    {
        return fail("Merged-guard DPI call was not emitted once before its selected state update");
    }

    Design mismatchedEventDesign = buildDpiMergedGuardDesign(true);
    EmitDiagnostics mismatchedEventDiagnostics;
    EmitSystemVerilog mismatchedEventEmitter(&mismatchedEventDiagnostics);
    EmitOptions mismatchedEventOptions;
    mismatchedEventOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    mismatchedEventOptions.outputFilename = std::string("emit_dpi_mismatched_event_path.sv");
    const EmitResult mismatchedEventResult =
        mismatchedEventEmitter.emit(mismatchedEventDesign, mismatchedEventOptions);
    if (mismatchedEventResult.success || !mismatchedEventDiagnostics.hasError() ||
        !diagnosticsContain(mismatchedEventDiagnostics,
                            "eventEdge does not match sink eventEdge"))
    {
        return fail("DPI/state event mismatch did not fail explicitly");
    }

    Design unsupportedDesign = buildUnsupportedDpiStateDesign();
    EmitDiagnostics unsupportedDiagnostics;
    EmitSystemVerilog unsupportedEmitter(&unsupportedDiagnostics);
    EmitOptions unsupportedOptions;
    unsupportedOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    unsupportedOptions.outputFilename = std::string("emit_unsupported_dpi_state_path.sv");
    const EmitResult unsupportedResult = unsupportedEmitter.emit(unsupportedDesign, unsupportedOptions);
    if (unsupportedResult.success || !unsupportedDiagnostics.hasError() ||
        !diagnosticsContain(unsupportedDiagnostics,
                            "cannot be inlined atomically"))
    {
        return fail("Unsupported DPI-to-state combinational path did not fail explicitly");
    }

    Design dynamicSliceDesign = buildDpiDynamicSliceDesign();
    EmitDiagnostics dynamicSliceDiagnostics;
    EmitSystemVerilog dynamicSliceEmitter(&dynamicSliceDiagnostics);
    EmitOptions dynamicSliceOptions;
    dynamicSliceOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    dynamicSliceOptions.outputFilename = std::string("emit_dpi_dynamic_slice_path.sv");
    const EmitResult dynamicSliceResult =
        dynamicSliceEmitter.emit(dynamicSliceDesign, dynamicSliceOptions);
    if (!dynamicSliceResult.success || dynamicSliceDiagnostics.hasError())
    {
        return fail("DPI dynamic-slice state path failed to emit");
    }
    const std::string dynamicSliceOutput =
        readFile(std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                 "emit_dpi_dynamic_slice_path.sv");
    const std::string dynamicSliceCall =
        "dpi_slice_atomic(dpi_slice_base_intm, dpi_slice_index_intm, dpi_const_slice_base_intm);";
    const std::string dynamicSliceCommit =
        "dpi_slice_reg <= $unsigned(dpi_slice_base_intm[2'(dpi_slice_index_intm)]);";
    if (!appearsInOrderWithinClockBlock(dynamicSliceOutput,
                                        dynamicSliceCall,
                                        dynamicSliceCommit) ||
        !appearsInOrderWithinClockBlock(dynamicSliceOutput,
                                        dynamicSliceCall,
                                        "dpi_const_slice_reg <= $unsigned(dpi_const_slice_base_intm[8'd4]);"))
    {
        return fail("DPI dynamic-slice index was not clamped and committed atomically");
    }

    Design narrowAggregateDesign = buildDpiNarrowAggregateDesign();
    EmitDiagnostics narrowAggregateDiagnostics;
    EmitSystemVerilog narrowAggregateEmitter(&narrowAggregateDiagnostics);
    EmitOptions narrowAggregateOptions;
    narrowAggregateOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    narrowAggregateOptions.outputFilename = std::string("emit_dpi_narrow_aggregate_path.sv");
    const EmitResult narrowAggregateResult =
        narrowAggregateEmitter.emit(narrowAggregateDesign, narrowAggregateOptions);
    if (!narrowAggregateResult.success || narrowAggregateDiagnostics.hasError())
    {
        return fail("DPI narrow aggregate state paths failed to emit");
    }
    const std::string narrowAggregateOutput =
        readFile(std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                 "emit_dpi_narrow_aggregate_path.sv");
    const std::string narrowAggregateCall =
        "dpi_narrow_atomic(dpi_narrow_concat_intm, dpi_narrow_replicate_intm);";
    if (!appearsInOrderWithinClockBlock(narrowAggregateOutput,
                                        narrowAggregateCall,
                                        "8'({dpi_narrow_prefix, dpi_narrow_concat_intm})") ||
        !appearsInOrderWithinClockBlock(narrowAggregateOutput,
                                        narrowAggregateCall,
                                        "8'({2{dpi_narrow_replicate_intm}})"))
    {
        return fail("DPI concat/replicate expressions were not narrowed before atomic state use");
    }

    Design signedAggregateDesign = buildDpiSignedAggregateDesign();
    EmitDiagnostics signedAggregateDiagnostics;
    EmitSystemVerilog signedAggregateEmitter(&signedAggregateDiagnostics);
    EmitOptions signedAggregateOptions;
    signedAggregateOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    signedAggregateOptions.outputFilename = std::string("emit_dpi_signed_aggregate_path.sv");
    const EmitResult signedAggregateResult =
        signedAggregateEmitter.emit(signedAggregateDesign, signedAggregateOptions);
    if (!signedAggregateResult.success || signedAggregateDiagnostics.hasError())
    {
        return fail("DPI signed aggregate state paths failed to emit");
    }
    const std::string signedAggregateOutput =
        readFile(std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                 "emit_dpi_signed_aggregate_path.sv");
    const std::string signedAggregateCall =
        "dpi_signed_atomic(dpi_signed_concat_intm, dpi_signed_replicate_intm, "
        "dpi_unsigned_boundary_intm);";
    if (!appearsInOrderWithinClockBlock(signedAggregateOutput,
                                        signedAggregateCall,
                                        "$signed(dpi_signed_concat_intm) >>>") ||
        !appearsInOrderWithinClockBlock(signedAggregateOutput,
                                        signedAggregateCall,
                                        "$signed({1{dpi_signed_replicate_intm}}) >>>") ||
        !appearsInOrderWithinClockBlock(signedAggregateOutput,
                                        signedAggregateCall,
                                        "$unsigned(dpi_unsigned_boundary_intm) >>>"))
    {
        return fail("DPI signed concat/replicate values lost signedness before arithmetic shift");
    }

    Design memoryFillDesign = buildDpiMemoryFillDesign();
    EmitDiagnostics memoryFillDiagnostics;
    EmitSystemVerilog memoryFillEmitter(&memoryFillDiagnostics);
    EmitOptions memoryFillOptions;
    memoryFillOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    memoryFillOptions.outputFilename = std::string("emit_dpi_memory_fill_path.sv");
    const EmitResult memoryFillResult = memoryFillEmitter.emit(memoryFillDesign, memoryFillOptions);
    if (memoryFillResult.success || !memoryFillDiagnostics.hasError() ||
        !diagnosticsContain(memoryFillDiagnostics, "supported atomic state sink"))
    {
        return fail("DPI-to-memory-fill path did not fail explicitly");
    }

    Design effectConsumerDesign = buildDpiEffectConsumerDesign();
    EmitDiagnostics effectConsumerDiagnostics;
    EmitSystemVerilog effectConsumerEmitter(&effectConsumerDiagnostics);
    EmitOptions effectConsumerOptions;
    effectConsumerOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    effectConsumerOptions.outputFilename = std::string("emit_dpi_effect_consumer_path.sv");
    const EmitResult effectConsumerResult =
        effectConsumerEmitter.emit(effectConsumerDesign, effectConsumerOptions);
    if (effectConsumerResult.success || !effectConsumerDiagnostics.hasError() ||
        !diagnosticsContain(effectConsumerDiagnostics,
                            "effect operations that cannot be ordered atomically"))
    {
        return fail("DPI result flowing into another effect operation did not fail explicitly");
    }

    return 0;
}
