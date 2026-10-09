#include "emit/system_verilog.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

    const auto latch = graph.createOperation(OperationKind::kLatch,
                                             graph.internSymbol("lat_a"));
    graph.setAttr(latch, "width", static_cast<int64_t>(4));
    graph.setAttr(latch, "isSigned", false);

    const auto orderedMemory = graph.createOperation(OperationKind::kRegister,
                                                     graph.internSymbol("ordered_mem"));
    graph.setAttr(orderedMemory, "width", static_cast<int64_t>(32));
    graph.setAttr(orderedMemory, "isSigned", false);
    graph.setAttr(orderedMemory, "shape", std::vector<int64_t>{4});

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
    graph.addOperand(regWrite, clk);
    graph.setAttr(regWrite, "regSymbol", std::string("reg_full"));
    graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});

    const auto regMaskWrite = graph.createOperation(OperationKind::kRegisterWriteMaskPort,
                                                    graph.internSymbol("_op_emit_reg_mask_write"));
    graph.addOperand(regMaskWrite, condAlways);
    graph.addOperand(regMaskWrite, dataMask);
    graph.addOperand(regMaskWrite, halfMask);
    graph.addOperand(regMaskWrite, clk);
    graph.setAttr(regMaskWrite, "regSymbol", std::string("reg_mask"));
    graph.setAttr(regMaskWrite, "eventEdge", std::vector<std::string>{"posedge"});

    // stage-03: write ports of one register apply in declaration order and
    // the later write wins, so the low-priority data is declared first.
    const auto orderedLowWrite = graph.createOperation(OperationKind::kRegisterWriteArrayMaskPort,
                                                       graph.internSymbol("_op_ordered_low_write"));
    graph.addOperand(orderedLowWrite, en);
    graph.addOperand(orderedLowWrite, orderedAddr);
    graph.addOperand(orderedLowWrite, orderedLowData);
    graph.addOperand(orderedLowWrite, fullMask);
    graph.addOperand(orderedLowWrite, clk);
    graph.setAttr(orderedLowWrite, "regSymbol", std::string("ordered_mem"));
    graph.setAttr(orderedLowWrite, "eventEdge", std::vector<std::string>{"posedge"});

    const auto orderedHighWrite = graph.createOperation(OperationKind::kRegisterWriteArrayMaskPort,
                                                        graph.internSymbol("_op_ordered_high_write"));
    graph.addOperand(orderedHighWrite, en);
    graph.addOperand(orderedHighWrite, orderedAddr);
    graph.addOperand(orderedHighWrite, orderedHighData);
    graph.addOperand(orderedHighWrite, fullMask);
    graph.addOperand(orderedHighWrite, clk);
    graph.setAttr(orderedHighWrite, "regSymbol", std::string("ordered_mem"));
    graph.setAttr(orderedHighWrite, "eventEdge", std::vector<std::string>{"posedge"});

    const auto latchWrite = graph.createOperation(OperationKind::kLatchWritePort,
                                                  graph.internSymbol("_op_emit_latch_write"));
    graph.addOperand(latchWrite, latchEn);
    graph.addOperand(latchWrite, latchData);
    graph.addOperand(latchWrite, latchMask);
    graph.setAttr(latchWrite, "latchSymbol", std::string("lat_a"));

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
        return fail("Array writes were not emitted in declaration order (later write must win)");
    }

    return 0;
}
