#include "emit/system_verilog.hpp"
#include "slang/numeric/SVInt.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::emit;

int main()
{
    using slang::SVInt;
    // Check the expression identity with four-state arithmetic, including indices
    // whose unknown upper bits must not be truncated before the range check.
    for (const int width : {65, 128, 256})
    {
        for (const std::string pattern : {"0", "1", "x", "z", "10xz"})
        {
            std::string bits;
            for (int bit = 0; bit < width; ++bit) bits += pattern[bit % pattern.size()];
            const auto data = SVInt::fromString(std::to_string(width) + "'b" + bits);
            for (const int indexWidth : {7, 13, 33, 65, 128})
            {
                auto check = [&](const SVInt &index)
                {
                    const auto original = data.lshr(index).slice(0, 0);
                    const auto candidate = SVInt::conditional(SVInt(index >= SVInt(32, width, false)),
                                                               SVInt(1, 0, false), SVInt(data[index]));
                    return exactlyEqual(original, candidate);
                };
                for (int index = 0; index <= width + 1; ++index)
                {
                    if (!check(SVInt(indexWidth, index, false))) return 1;
                }
                for (const char unknown : {'x', 'z'})
                {
                    for (const int position : {0, indexWidth - 1})
                    {
                        std::string indexBits(indexWidth, '0');
                        indexBits[position] = unknown;
                        if (!check(SVInt::fromString(std::to_string(indexWidth) + "'b" + indexBits))) return 1;
                    }
                }
                if (!check(SVInt(indexWidth, UINT64_C(0x100000001), false))) return 1;
            }
        }
    }
    // A fixed logical left shift maps result bit b to source bit b - amount;
    // bits below the shift amount are zero, including for four-state data.
    for (const int width : {65, 128, 256})
    {
        const std::string bits = "10xz";
        std::string dataBits;
        for (int bit = 0; bit < width; ++bit) dataBits += bits[bit % bits.size()];
        const auto data = SVInt::fromString(std::to_string(width) + "'b" + dataBits);
        for (const int amount : {0, 1, 7, width - 1, width, width + 1})
        {
            const auto shifted = data.shl(static_cast<uint64_t>(amount));
            for (const int bit : {0, 1, 7, width - 1})
            {
                if (bit >= width) continue;
                const auto original = shifted.slice(bit, bit);
                const auto candidate = bit < amount
                                           ? SVInt(1, 0, false)
                                           : SVInt(data[bit - amount]);
                if (!exactlyEqual(original, candidate)) return 1;
            }
        }
    }
    // Masked memory writes first widen a narrow lane to the full row width.
    // The mapping must also preserve zero extension above that lane.
    for (const auto [sourceWidth, resultWidth] :
         {std::pair{7, 70}, std::pair{7, 112}, std::pair{30, 300}, std::pair{28, 448}})
    {
        for (const std::string pattern : {"0", "1", "x", "z", "10xz"})
        {
            std::string bits;
            for (int bit = 0; bit < sourceWidth; ++bit) bits += pattern[bit % pattern.size()];
            const auto data = SVInt::fromString(std::to_string(sourceWidth) + "'b" + bits);
            for (const int amount : {0, 1, 7, sourceWidth, resultWidth - 1, resultWidth, resultWidth + 1})
            {
                const auto shifted = data.extend(resultWidth, false).shl(static_cast<uint64_t>(amount));
                for (int bit = 0; bit < resultWidth; ++bit)
                {
                    const int sourceBit = bit - amount;
                    const auto candidate = sourceBit < 0 || sourceBit >= sourceWidth
                                               ? SVInt(1, 0, false)
                                               : SVInt(data[sourceBit]);
                    if (!exactlyEqual(shifted.slice(bit, bit), candidate))
                    {
                        std::cerr << "widened constant shift identity failed at " << sourceWidth << " -> "
                                  << resultWidth << ", amount " << amount << ", bit " << bit << '\n';
                        return 1;
                    }
                }
            }
        }
    }
    Design design;
    auto &graph = design.createGraph("shift_low_bit");
    design.markAsTop(graph.symbol());
    auto make = [&](const std::string &name, int width, bool signedData, bool signedIndex,
                    OperationKind kind, int bit, int shiftedWidth, bool exposeShift)
    {
        const auto data = graph.createValue(graph.internSymbol(name + "_data"), width, signedData);
        const auto index = graph.createValue(graph.internSymbol(name + "_index"), 13, signedIndex);
        const auto shifted = graph.createValue(graph.internSymbol(name + "_shifted"), shiftedWidth, false);
        const auto result = graph.createValue(graph.internSymbol(name + "_result"), 1, false);
        graph.bindInputPort(name + "_data", data);
        graph.bindInputPort(name + "_index", index);
        graph.bindOutputPort(name + "_result", result);
        if (exposeShift) graph.bindOutputPort(name + "_shifted", shifted);
        const auto shift = graph.createOperation(kind, graph.internSymbol(name + "_shift_op"));
        graph.addOperand(shift, data);
        graph.addOperand(shift, index);
        graph.addResult(shift, shifted);
        const auto slice = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol(name + "_slice_op"));
        graph.addOperand(slice, shifted);
        graph.addResult(slice, result);
        graph.setAttr(slice, "sliceStart", static_cast<int64_t>(bit));
        graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(bit));
    };
    auto makeConstant = [&](const std::string &name, int constantWidth, std::string literal)
    {
        const auto value = graph.createValue(graph.internSymbol(name + "_amount"), constantWidth, false);
        const auto op = graph.createOperation(OperationKind::kConstant,
                                              graph.internSymbol(name + "_const_op"));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    };
    auto makeConstShl = [&](const std::string &name, int width, int amount, int bit,
                            int shiftedWidth = -1, bool signedData = false,
                            std::string literal = {}, int amountWidth = 32)
    {
        const auto data = graph.createValue(graph.internSymbol(name + "_data"), width, signedData);
        const auto shiftAmount = makeConstant(name, amountWidth,
                                              literal.empty() ? std::to_string(amountWidth) + "'d" +
                                                                    std::to_string(amount)
                                                              : std::move(literal));
        const auto shifted = graph.createValue(graph.internSymbol(name + "_shifted"),
                                               shiftedWidth < 0 ? width : shiftedWidth, false);
        const auto result = graph.createValue(graph.internSymbol(name + "_result"), 1, false);
        graph.bindInputPort(name + "_data", data);
        graph.bindOutputPort(name + "_result", result);
        const auto shift = graph.createOperation(OperationKind::kShl, graph.internSymbol(name + "_shift_op"));
        graph.addOperand(shift, data);
        graph.addOperand(shift, shiftAmount);
        graph.addResult(shift, shifted);
        const auto slice = graph.createOperation(OperationKind::kSliceStatic,
                                                 graph.internSymbol(name + "_slice_op"));
        graph.addOperand(slice, shifted);
        graph.addResult(slice, result);
        graph.setAttr(slice, "sliceStart", static_cast<int64_t>(bit));
        graph.setAttr(slice, "sliceEnd", static_cast<int64_t>(bit));
    };
    make("wide", 128, false, false, OperationKind::kLShr, 0, 128, false);
    make("odd", 65, false, false, OperationKind::kLShr, 0, 65, false);
    make("shared", 128, false, false, OperationKind::kLShr, 0, 128, true);
    make("signed_data", 128, true, false, OperationKind::kLShr, 0, 128, false);
    make("signed_index", 128, false, true, OperationKind::kLShr, 0, 128, false);
    make("arithmetic", 128, false, false, OperationKind::kAShr, 0, 128, false);
    make("other_bit", 128, false, false, OperationKind::kLShr, 1, 128, false);
    make("extended", 128, false, false, OperationKind::kLShr, 0, 256, false);
    make("truncated", 128, false, false, OperationKind::kLShr, 0, 64, false);
    make("scalar", 1, false, false, OperationKind::kLShr, 0, 1, false);
    make("native64", 64, false, false, OperationKind::kLShr, 0, 64, false);
    makeConstShl("const_bit", 128, 7, 7);
    makeConstShl("const_bit_hi", 300, 30, 31);
    makeConstShl("const_zero", 128, 7, 0);
    makeConstShl("const_overshift", 128, 200, 127);
    makeConstShl("const_signed_data", 128, 7, 7, -1, true);
    makeConstShl("const_hex", 128, 7, 7, -1, false, "32'h7");
    makeConstShl("const_truncated", 128, 8, 7, -1, false, "3'd8", 3);
    makeConstShl("const_unknown", 128, 7, 7, -1, false, "32'bx");
    makeConstShl("const_oob", 128, 7, 128);
    make("const_dynamic", 128, false, false, OperationKind::kShl, 7, 128, false);
    makeConstShl("const_extended", 128, 7, 7, 256);

    struct MemoryCase
    {
        std::string name;
        std::string memory;
        int sourceWidth;
        int resultWidth;
        int amount;
        std::vector<int> maskBits;
        bool optimize;
    };
    std::vector<MemoryCase> memoryCases;
    const auto memoryClock = graph.createValue(graph.internSymbol("memory_clk"), 1, false);
    graph.bindInputPort("memory_clk", memoryClock);
    auto makeMemoryShift = [&](const std::string &name, int sourceWidth, int resultWidth, int amount,
                               std::vector<int> maskBits, bool optimize = true,
                               bool signedData = false, bool dynamicAmount = false,
                               std::string amountLiteral = {}, std::string memory = {},
                               int64_t priority = -1)
    {
        if (memory.empty()) memory = name + "_mem";
        if (!graph.findOperation(memory).valid())
        {
            const auto mem = graph.createOperation(OperationKind::kMemory, graph.internSymbol(memory));
            graph.setAttr(mem, "width", static_cast<int64_t>(resultWidth));
            graph.setAttr(mem, "row", int64_t{4});
            graph.setAttr(mem, "isSigned", false);
        }
        const auto data = graph.createValue(graph.internSymbol(name + "_data"), sourceWidth, signedData);
        const auto enable = graph.createValue(graph.internSymbol(name + "_en"), 1, false);
        const auto addr = graph.createValue(graph.internSymbol(name + "_addr"), 2, false);
        graph.bindInputPort(name + "_data", data);
        graph.bindInputPort(name + "_en", enable);
        graph.bindInputPort(name + "_addr", addr);
        ValueId index;
        if (dynamicAmount)
        {
            index = graph.createValue(graph.internSymbol(name + "_amount"), 32, false);
            graph.bindInputPort(name + "_amount", index);
        }
        else
        {
            index = makeConstant(name, 32, amountLiteral.empty() ? "32'd" + std::to_string(amount)
                                                                : std::move(amountLiteral));
        }
        const auto shifted = graph.createValue(graph.internSymbol(name + "_shifted"), resultWidth, false);
        const auto shift = graph.createOperation(OperationKind::kShl, graph.internSymbol(name + "_shift_op"));
        graph.addOperand(shift, data);
        graph.addOperand(shift, index);
        graph.addResult(shift, shifted);
        std::string maskLiteral(resultWidth, '0');
        for (const int bit : maskBits) maskLiteral[resultWidth - 1 - bit] = '1';
        const auto mask = makeConstant(name + "_mask", resultWidth,
                                       std::to_string(resultWidth) + "'b" + maskLiteral);
        const auto write = graph.createOperation(OperationKind::kMemoryWritePort,
                                                 graph.internSymbol(name + "_write"));
        graph.addOperand(write, enable);
        graph.addOperand(write, addr);
        graph.addOperand(write, shifted);
        graph.addOperand(write, mask);
        graph.addOperand(write, memoryClock);
        graph.setAttr(write, "memSymbol", memory);
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        if (priority >= 0)
        {
            graph.setAttr(write, kMemoryWritePriorityGroupAttr, memory + "_writes");
            graph.setAttr(write, kMemoryWritePriorityAttr, priority);
        }
        memoryCases.push_back({name, memory, sourceWidth, resultWidth, amount, std::move(maskBits), optimize});
    };
    for (const auto [sourceWidth, resultWidth] :
         {std::pair{7, 70}, std::pair{7, 112}, std::pair{30, 300}, std::pair{28, 448}})
    {
        std::vector<int> maskBits;
        for (int bit = sourceWidth; bit < sourceWidth * 2; ++bit) maskBits.push_back(bit);
        makeMemoryShift("memory_" + std::to_string(sourceWidth) + "_to_" + std::to_string(resultWidth),
                        sourceWidth, resultWidth, sourceWidth, std::move(maskBits));
    }
    makeMemoryShift("memory_boundaries", 7, 70, 7, {0, 6, 7, 13, 14, 69});
    makeMemoryShift("memory_overshift", 7, 70, 70, {0, 69});
    makeMemoryShift("memory_dynamic", 7, 70, 7, {7, 13}, false, false, true);
    makeMemoryShift("memory_signed", 7, 70, 7, {7, 13}, false, true);
    makeMemoryShift("memory_unknown", 7, 70, 7, {7, 13}, false, false, false, "32'bx");
    makeMemoryShift("memory_narrowed", 128, 70, 7, {7, 13}, false);
    // The emitter must preserve masked writes and their NBA priority. Create the
    // higher priority writer first so textual order cannot pass accidentally.
    makeMemoryShift("memory_priority_high", 7, 70, 7, {7, 8}, true, false, false, {}, "priority_mem", 0);
    makeMemoryShift("memory_priority_low", 7, 70, 7, {7, 8}, true, false, false, {}, "priority_mem", 1);

    EmitOptions options;
    options.outputDir = WOLF_SV_EMIT_ARTIFACT_DIR;
    options.outputFilename = "shift_low_bit.sv";
    EmitDiagnostics diagnostics;
    EmitSystemVerilog emitter(&diagnostics);
    if (!emitter.emit(design, options).success || diagnostics.hasError()) return 1;
    std::ifstream stream(std::filesystem::path(*options.outputDir) / *options.outputFilename);
    const std::string output{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    for (const std::string name : {"wide", "odd", "shared"})
    {
        const int width = name == "odd" ? 65 : 128;
        const std::string expected = "assign " + name + "_result = ($unsigned(" + name +
            "_index) >= 32'd" + std::to_string(width) + ") ? 1'b0 : " + name +
            "_data[$unsigned(" + name + "_index)];";
        if (output.find(expected) == std::string::npos)
        {
            std::cerr << "missing guarded selection: " << expected << '\n';
            return 1;
        }
    }
    for (const std::string name : {"signed_data", "signed_index", "arithmetic", "extended", "truncated", "scalar", "native64"})
    {
        const auto expected = "assign " + name + "_result = " + name + "_shifted" + (name == "scalar" ? ";" : "[0];");
        if (output.find(expected) == std::string::npos)
        {
            std::cerr << "unexpected rewrite of " << name << '\n';
            return 1;
        }
    }
    if (output.find("assign other_bit_result = other_bit_shifted[1];") == std::string::npos ||
        output.find("assign shared_shifted = shared_data >> shared_index;") == std::string::npos) return 1;
    for (const std::string expected : {
             "assign const_bit_result = const_bit_data[0];",
             "assign const_bit_hi_result = const_bit_hi_data[1];",
             "assign const_zero_result = 1'b0;",
             "assign const_overshift_result = 1'b0;"})
    {
        if (output.find(expected) == std::string::npos)
        {
            std::cerr << "missing constant shift rewrite: " << expected << '\n';
            return 1;
        }
    }
    for (const std::string name : {"const_signed_data", "const_unknown", "const_dynamic", "const_extended"})
    {
        const auto expected = "assign " + name + "_result = " + name + "_shifted[7];";
        if (output.find(expected) == std::string::npos)
        {
            std::cerr << "unexpected constant shift rewrite of " << name << '\n';
            return 1;
        }
    }
    if (output.find("assign const_oob_result = const_oob_shifted[128];") == std::string::npos)
    {
        std::cerr << "out-of-range static slice was unexpectedly rewritten\n";
        return 1;
    }
    if (output.find("assign const_hex_result = const_hex_data[0];") == std::string::npos ||
        output.find("assign const_truncated_result = const_truncated_data[7];") == std::string::npos)
    {
        std::cerr << "constant base or width truncation was not modeled\n";
        return 1;
    }
    const auto clockPosition = output.find("always @(posedge memory_clk) begin");
    if (clockPosition == std::string::npos)
    {
        std::cerr << "masked memory writes lost their event clock\n";
        return 1;
    }
    for (const auto &test : memoryCases)
    {
        std::string expected = "    if (" + test.name + "_en) begin\n";
        for (std::size_t index = 0; index < test.maskBits.size();)
        {
            const int runStart = test.maskBits[index];
            int runEnd = runStart;
            while (index + 1 < test.maskBits.size() && test.maskBits[index + 1] == runEnd + 1)
            {
                ++index;
                runEnd = test.maskBits[index];
            }
            const int firstSourceBit = runStart - test.amount;
            const int lastSourceBit = runEnd - test.amount;
            const bool canDirectPartSelect = test.optimize && runEnd > runStart &&
                                             firstSourceBit >= 0 && lastSourceBit < test.sourceWidth;
            const bool canShiftedPartSelect = runEnd > runStart && runEnd < test.resultWidth;
            if (canDirectPartSelect)
            {
                expected += "      " + test.memory + "[" + test.name + "_addr][" +
                            std::to_string(runEnd) + ":" + std::to_string(runStart) + "] <= " +
                            test.name + "_data[" + std::to_string(lastSourceBit) + ":" +
                            std::to_string(firstSourceBit) + "];\n";
            }
            else if (canShiftedPartSelect)
            {
                expected += "      " + test.memory + "[" + test.name + "_addr][" +
                            std::to_string(runEnd) + ":" + std::to_string(runStart) + "] <= " +
                            test.name + "_shifted[" + std::to_string(runEnd) + ":" +
                            std::to_string(runStart) + "];\n";
            }
            else
            {
                for (std::size_t selected = index - (runEnd - runStart); selected <= index; ++selected)
                {
                    const int bit = test.maskBits[selected];
                    const int sourceBit = bit - test.amount;
                    const std::string rhs = !test.optimize ? test.name + "_shifted[" + std::to_string(bit) + "]"
                                              : sourceBit < 0 || sourceBit >= test.sourceWidth ? "1'b0"
                                              : test.name + "_data[" + std::to_string(sourceBit) + "]";
                    expected += "      " + test.memory + "[" + test.name + "_addr][" + std::to_string(bit) +
                                "] <= " + rhs + ";\n";
                }
            }
            ++index;
        }
        expected += "    end";
        const auto position = output.find(expected);
        if (position == std::string::npos || position < clockPosition)
        {
            std::cerr << "unexpected masked memory shift emission for " << test.name << ":\n" << expected << '\n';
            return 1;
        }
    }
    const auto lowPriority = output.find("if (memory_priority_low_en) begin");
    const auto highPriority = output.find("if (memory_priority_high_en) begin");
    if (lowPriority == std::string::npos || highPriority == std::string::npos || lowPriority >= highPriority)
    {
        std::cerr << "constant shift rewrite changed memory write priority\n";
        return 1;
    }
    std::cout << "guarded low-bit shift emission passed\n";
}
