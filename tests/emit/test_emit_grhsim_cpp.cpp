#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/reg_to_mem.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    constexpr std::string_view kHarnessCompileFlags = "-std=c++20 -O0";

    int fail(const std::string &message)
    {
        std::cerr << "[emit_grhsim_cpp] " << message << '\n';
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

    std::string readFiles(const std::vector<std::filesystem::path> &paths)
    {
        std::string out;
        for (const auto &path : paths)
        {
            out += readFile(path);
        }
        return out;
    }

    template <typename T>
    const T *sessionValue(const SessionStore &session, std::string_view key)
    {
        const auto it = session.find(std::string(key));
        if (it == session.end())
        {
            return nullptr;
        }
        const auto *typed = dynamic_cast<const SessionSlotValue<T> *>(it->second.get());
        return typed == nullptr ? nullptr : &typed->value;
    }

    std::map<std::string, std::string> extractValueSlotMapping(std::string_view text)
    {
        std::map<std::string, std::string> mapping;
        std::istringstream lines{std::string(text)};
        std::string line;
        while (std::getline(lines, line))
        {
            const std::size_t valueIdMarker = line.find(" [value_id=");
            if (valueIdMarker == std::string::npos)
            {
                continue;
            }
            const std::size_t arrow = line.rfind(" -> ", valueIdMarker);
            const std::size_t valueIdBegin = valueIdMarker + std::string_view(" [value_id=").size();
            const std::size_t valueIdEnd = line.find(']', valueIdBegin);
            if (arrow == std::string::npos || valueIdEnd == std::string::npos)
            {
                continue;
            }
            const std::string slot = line.substr(arrow + 4, valueIdMarker - arrow - 4);
            if (!slot.starts_with("value_") || slot.find("_slots_[") == std::string::npos)
            {
                continue;
            }
            mapping.insert_or_assign(line.substr(valueIdBegin, valueIdEnd - valueIdBegin), slot);
        }
        return mapping;
    }

    std::vector<std::string> extractValueSlotDeclarations(std::string_view header)
    {
        std::vector<std::string> declarations;
        std::istringstream lines{std::string(header)};
        std::string line;
        while (std::getline(lines, line))
        {
            if (line.find(" value_") != std::string::npos &&
                line.find("_slots_{};") != std::string::npos)
            {
                declarations.push_back(line);
            }
        }
        return declarations;
    }

    std::map<std::string, std::string> collectGeneratedSourceFiles(
        const std::filesystem::path &dir,
        std::string_view prefix)
    {
        std::map<std::string, std::string> files;
        for (const auto &entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (!name.starts_with(prefix) ||
                (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp"))
            {
                continue;
            }
            files.emplace(name, readFile(entry.path()));
        }
        return files;
    }

    std::size_t countSubstring(std::string_view text, std::string_view needle)
    {
        if (needle.empty())
        {
            return 0;
        }
        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string_view::npos)
        {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    std::optional<std::uint64_t> jsonUnsignedField(std::string_view json, std::string_view name)
    {
        const std::string key = "\"" + std::string(name) + "\"";
        const std::size_t keyPos = json.find(key);
        if (keyPos == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::size_t colonPos = json.find(':', keyPos + key.size());
        if (colonPos == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::size_t valuePos = json.find_first_not_of(" \t\r\n", colonPos + 1u);
        if (valuePos == std::string_view::npos || json[valuePos] < '0' || json[valuePos] > '9')
        {
            return std::nullopt;
        }
        const std::size_t valueEnd = json.find_first_not_of("0123456789", valuePos);
        try
        {
            return std::stoull(std::string(json.substr(valuePos, valueEnd - valuePos)));
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    bool diagnosticsContain(const EmitDiagnostics &diagnostics, std::string_view needle)
    {
        return std::any_of(diagnostics.messages().begin(),
                           diagnostics.messages().end(),
                           [&](const EmitDiagnostic &message) {
                               return message.message.find(needle) != std::string::npos;
                           });
    }

    std::size_t findMatchingBrace(std::string_view text, std::size_t openBrace)
    {
        if (openBrace == std::string_view::npos || openBrace >= text.size() || text[openBrace] != '{')
        {
            return std::string_view::npos;
        }
        std::size_t depth = 0;
        for (std::size_t pos = openBrace; pos < text.size(); ++pos)
        {
            if (text[pos] == '{')
            {
                ++depth;
            }
            else if (text[pos] == '}')
            {
                --depth;
                if (depth == 0)
                {
                    return pos;
                }
            }
        }
        return std::string_view::npos;
    }

    std::vector<std::string_view> splitTabs(std::string_view line)
    {
        std::vector<std::string_view> fields;
        std::size_t pos = 0;
        while (pos <= line.size())
        {
            const std::size_t next = line.find('\t', pos);
            if (next == std::string_view::npos)
            {
                fields.push_back(line.substr(pos));
                break;
            }
            fields.push_back(line.substr(pos, next - pos));
            pos = next + 1;
        }
        return fields;
    }

    std::vector<std::filesystem::path> collectSchedFiles(const std::filesystem::path &dir, std::string_view prefix)
    {
        std::vector<std::filesystem::path> files;
        if (!std::filesystem::exists(dir))
        {
            return files;
        }
        for (const auto &entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name.rfind(std::string(prefix), 0) == 0 && name.ends_with(".cpp"))
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    ValueId makeLogicValue(Graph &graph, std::string_view name, int32_t width, bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), width, isSigned, ValueType::Logic);
    }

    ValueId makeStringValue(Graph &graph, std::string_view name)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), 0, false, ValueType::String);
    }

    ValueId makeRealValue(Graph &graph, std::string_view name)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), 0, false, ValueType::Real);
    }

    ValueId addConstant(Graph &graph,
                        std::string_view opName,
                        std::string_view valueName,
                        int32_t width,
                        std::string literal,
                        ValueType type = ValueType::Logic,
                        bool isSigned = false)
    {
        ValueId value = graph.createValue(graph.internSymbol(std::string(valueName)), width, isSigned, type);
        OperationId op = graph.createOperation(OperationKind::kConstant,
                                               graph.internSymbol(std::string(opName)));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    void setRegToMemIntentShape(Graph &graph,
                                OperationId opId,
                                const std::string &group,
                                const std::string &role,
                                int64_t elementWidth,
                                int64_t elementCount)
    {
        graph.setAttr(opId, "regToMem.intent.version", int64_t{1});
        graph.setAttr(opId, "regToMem.intent.group", group);
        graph.setAttr(opId, "regToMem.intent.role", role);
        graph.setAttr(opId, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(opId, "regToMem.intent.elementWidth", elementWidth);
        graph.setAttr(opId, "regToMem.intent.elementCount", elementCount);
    }

    std::string allOnesLiteral(int32_t width)
    {
        return std::to_string(width) + "'b" + std::string(static_cast<std::size_t>(width), '1');
    }

    Design buildDesign(const std::string &wideMemInitFile)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId en = makeLogicValue(graph, "en", 1);
        ValueId a = makeLogicValue(graph, "a", 8);
        ValueId comb = makeLogicValue(graph, "comb", 8);
        ValueId b = makeLogicValue(graph, "b", 8);
        ValueId sh = makeLogicValue(graph, "sh", 3);
        ValueId rep2 = makeLogicValue(graph, "rep2", 2);
        ValueId sa = makeLogicValue(graph, "sa", 8, true);
        ValueId ss4 = makeLogicValue(graph, "ss4", 4, true);
        ValueId midIn = makeLogicValue(graph, "mid_in", 96);
        ValueId wideIn = makeLogicValue(graph, "wide_in", 130);
        ValueId wideMaskDyn = makeLogicValue(graph, "wide_mask_dyn", 130);
        ValueId wideAddr = makeLogicValue(graph, "wide_addr", 2);
        ValueId wideMemIdx = makeLogicValue(graph, "wide_mem_idx", 65);
        ValueId wideSignedIn = makeLogicValue(graph, "wide_signed_in", 65, true);
        ValueId padIn = makeLogicValue(graph, "pad_in", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("a", a);
        graph.bindInputPort("comb", comb);
        graph.bindInputPort("b", b);
        graph.bindInputPort("sh", sh);
        graph.bindInputPort("rep2", rep2);
        graph.bindInputPort("sa", sa);
        graph.bindInputPort("ss4", ss4);
        graph.bindInputPort("mid_in", midIn);
        graph.bindInputPort("wide_in", wideIn);
        graph.bindInputPort("wide_mask_dyn", wideMaskDyn);
        graph.bindInputPort("wide_addr", wideAddr);
        graph.bindInputPort("wide_mem_idx", wideMemIdx);
        graph.bindInputPort("wide_signed_in", wideSignedIn);

        OperationId reg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("reg_q"));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h2"));

        OperationId randReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("rand_reg_q"));
        graph.setAttr(randReg, "width", static_cast<int64_t>(32));
        graph.setAttr(randReg, "isSigned", false);
        graph.setAttr(randReg, "initValue", std::string("$random"));

        ValueId randRegQ = makeLogicValue(graph, "rand_reg_q_read", 32);
        OperationId randRegRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                        graph.internSymbol("rand_reg_q_read_op"));
        graph.addResult(randRegRead, randRegQ);
        graph.setAttr(randRegRead, "regSymbol", std::string("rand_reg_q"));
        graph.bindOutputPort("rand_y", randRegQ);

        ValueId regQ = makeLogicValue(graph, "reg_q_read", 8);
        OperationId regRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                    graph.internSymbol("reg_q_read_op"));
        graph.addResult(regRead, regQ);
        graph.setAttr(regRead, "regSymbol", std::string("reg_q"));

        ValueId mask = addConstant(graph, "const_mask", "mask", 8, "8'hFF");
        ValueId wideZero = addConstant(graph, "const_wide_zero", "wide_zero", 130, "130'h0");
        ValueId wideOne = addConstant(graph, "const_wide_one", "wide_one", 130, "130'h1");
        ValueId wideTwo = addConstant(graph, "const_wide_two", "wide_two", 130, "130'h2");
        ValueId wideGeneralDivisor = addConstant(graph, "const_wide_general_divisor", "wide_general_divisor", 130,
                                                 "130'h080000000000000000000000000000003");
        ValueId midWideConst = addConstant(graph, "const_mid_wide", "mid_wide_const", 96, "96'h000000010000000000000003");
        ValueId smallTwo = addConstant(graph, "const_small_two", "small_two", 2, "2'd2");
        ValueId sh65 = addConstant(graph, "const_sh65", "sh65", 7, "7'd65");
        ValueId wideMask = addConstant(graph, "const_wide_mask", "wide_mask", 130, allOnesLiteral(130));
        ValueId idxWriteData = addConstant(graph, "const_idx_write_data", "idx_write_data", 8, "8'h44");
        ValueId fmt = addConstant(graph, "const_fmt", "fmt", 0, "\"q=%0d\"", ValueType::String);
        ValueId signedOne8 = addConstant(graph, "const_signed_one8", "signed_one8", 8, "8'sd1", ValueType::Logic, true);
        ValueId signedTwo8 = addConstant(graph, "const_signed_two8", "signed_two8", 8, "8'sd2", ValueType::Logic, true);
        ValueId signedThree8 = addConstant(graph, "const_signed_three8", "signed_three8", 8, "8'sd3", ValueType::Logic, true);
        ValueId wideSignedOne = addConstant(graph, "const_wide_signed_one", "wide_signed_one", 130, "130'sd1", ValueType::Logic, true);
        ValueId wideSignedTwo = addConstant(graph, "const_wide_signed_two", "wide_signed_two", 130, "130'sd2", ValueType::Logic, true);

        ValueId sum = makeLogicValue(graph, "sum", 8);
        OperationId add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum_add"));
        graph.addOperand(add, regQ);
        graph.addOperand(add, a);
        graph.addResult(add, sum);
        graph.bindOutputPort("y", sum);

        ValueId smallConcatLoopY = makeLogicValue(graph, "small_concat_loop_y", 8);
        OperationId smallConcatLoop =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("small_concat_loop_op"));
        graph.addOperand(smallConcatLoop, en);
        graph.addOperand(smallConcatLoop, clk);
        graph.addOperand(smallConcatLoop, en);
        graph.addOperand(smallConcatLoop, clk);
        graph.addOperand(smallConcatLoop, en);
        graph.addOperand(smallConcatLoop, clk);
        graph.addOperand(smallConcatLoop, en);
        graph.addOperand(smallConcatLoop, clk);
        graph.addResult(smallConcatLoop, smallConcatLoopY);
        graph.bindOutputPort("small_concat_loop_y", smallConcatLoopY);

        ValueId wideConcatLoopY = makeLogicValue(graph, "wide_concat_loop_y", 72);
        OperationId wideConcatLoop =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("wide_concat_loop_op"));
        for (int i = 0; i < 9; ++i)
        {
            graph.addOperand(wideConcatLoop, sum);
        }
        graph.addResult(wideConcatLoop, wideConcatLoopY);
        graph.bindOutputPort("wide_concat_loop_y", wideConcatLoopY);

        ValueId padSeenY = makeLogicValue(graph, "pad_seen_y", 8);
        OperationId padSeenAdd = graph.createOperation(OperationKind::kAdd, graph.internSymbol("pad_seen_add"));
        graph.addOperand(padSeenAdd, padIn);
        graph.addOperand(padSeenAdd, a);
        graph.addResult(padSeenAdd, padSeenY);
        graph.bindOutputPort("pad_seen_y", padSeenY);

        ValueId padOut = makeLogicValue(graph, "pad_out", 8);
        OperationId padOutXor = graph.createOperation(OperationKind::kXor, graph.internSymbol("pad_out_xor"));
        graph.addOperand(padOutXor, comb);
        graph.addOperand(padOutXor, a);
        graph.addResult(padOutXor, padOut);

        ValueId padOe = makeLogicValue(graph, "pad_oe", 1);
        OperationId padOeAssign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("pad_oe_assign"));
        graph.addOperand(padOeAssign, en);
        graph.addResult(padOeAssign, padOe);

        graph.bindInoutPort("pad", padIn, padOut, padOe);

        ValueId mulY = makeLogicValue(graph, "mul_y", 8);
        OperationId mul = graph.createOperation(OperationKind::kMul, graph.internSymbol("mul_op"));
        graph.addOperand(mul, comb);
        graph.addOperand(mul, b);
        graph.addResult(mul, mulY);
        graph.bindOutputPort("mul_y", mulY);

        ValueId divY = makeLogicValue(graph, "div_y", 8);
        OperationId div = graph.createOperation(OperationKind::kDiv, graph.internSymbol("div_op"));
        graph.addOperand(div, comb);
        graph.addOperand(div, b);
        graph.addResult(div, divY);
        graph.bindOutputPort("div_y", divY);

        ValueId modY = makeLogicValue(graph, "mod_y", 8);
        OperationId mod = graph.createOperation(OperationKind::kMod, graph.internSymbol("mod_op"));
        graph.addOperand(mod, comb);
        graph.addOperand(mod, b);
        graph.addResult(mod, modY);
        graph.bindOutputPort("mod_y", modY);

        ValueId shlY = makeLogicValue(graph, "shl_y", 8);
        OperationId shlOp = graph.createOperation(OperationKind::kShl, graph.internSymbol("shl_op"));
        graph.addOperand(shlOp, comb);
        graph.addOperand(shlOp, sh);
        graph.addResult(shlOp, shlY);
        graph.bindOutputPort("shl_y", shlY);

        ValueId lshrY = makeLogicValue(graph, "lshr_y", 8);
        OperationId lshrOp = graph.createOperation(OperationKind::kLShr, graph.internSymbol("lshr_op"));
        graph.addOperand(lshrOp, comb);
        graph.addOperand(lshrOp, sh);
        graph.addResult(lshrOp, lshrY);
        graph.bindOutputPort("lshr_y", lshrY);

        ValueId ashrY = makeLogicValue(graph, "ashr_y", 8, true);
        OperationId ashrOp = graph.createOperation(OperationKind::kAShr, graph.internSymbol("ashr_op"));
        graph.addOperand(ashrOp, sa);
        graph.addOperand(ashrOp, sh);
        graph.addResult(ashrOp, ashrY);
        graph.bindOutputPort("ashr_y", ashrY);

        ValueId redOrY = makeLogicValue(graph, "red_or_y", 1);
        OperationId redOrOp = graph.createOperation(OperationKind::kReduceOr, graph.internSymbol("red_or_op"));
        graph.addOperand(redOrOp, comb);
        graph.addResult(redOrOp, redOrY);
        graph.bindOutputPort("red_or_y", redOrY);

        ValueId redXorY = makeLogicValue(graph, "red_xor_y", 1);
        OperationId redXorOp = graph.createOperation(OperationKind::kReduceXor, graph.internSymbol("red_xor_op"));
        graph.addOperand(redXorOp, comb);
        graph.addResult(redXorOp, redXorY);
        graph.bindOutputPort("red_xor_y", redXorY);

        ValueId sliceY = makeLogicValue(graph, "slice_y", 3);
        OperationId sliceOp = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("slice_dyn_op"));
        graph.addOperand(sliceOp, comb);
        graph.addOperand(sliceOp, sh);
        graph.addResult(sliceOp, sliceY);
        graph.setAttr(sliceOp, "sliceWidth", static_cast<int64_t>(3));
        graph.bindOutputPort("slice_y", sliceY);

        ValueId repY = makeLogicValue(graph, "rep_y", 8);
        OperationId repOp = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("rep_op"));
        graph.addOperand(repOp, rep2);
        graph.addResult(repOp, repY);
        graph.setAttr(repOp, "rep", static_cast<int64_t>(4));
        graph.bindOutputPort("rep_y", repY);

        ValueId scalarMuxY = makeLogicValue(graph, "scalar_mux_y", 8);
        OperationId scalarMux = graph.createOperation(OperationKind::kMux, graph.internSymbol("scalar_mux_op"));
        graph.addOperand(scalarMux, en);
        graph.addOperand(scalarMux, comb);
        graph.addOperand(scalarMux, a);
        graph.addResult(scalarMux, scalarMuxY);
        graph.bindOutputPort("scalar_mux_y", scalarMuxY);

        ValueId caseEqY = makeLogicValue(graph, "case_eq_y", 1);
        OperationId caseEq = graph.createOperation(OperationKind::kCaseEq, graph.internSymbol("case_eq_op"));
        graph.addOperand(caseEq, comb);
        graph.addOperand(caseEq, a);
        graph.addResult(caseEq, caseEqY);
        graph.bindOutputPort("case_eq_y", caseEqY);

        ValueId caseNeY = makeLogicValue(graph, "case_ne_y", 1);
        OperationId caseNe = graph.createOperation(OperationKind::kCaseNe, graph.internSymbol("case_ne_op"));
        graph.addOperand(caseNe, comb);
        graph.addOperand(caseNe, a);
        graph.addResult(caseNe, caseNeY);
        graph.bindOutputPort("case_ne_y", caseNeY);

        ValueId wildcardEqY = makeLogicValue(graph, "wildcard_eq_y", 1);
        OperationId wildcardEq = graph.createOperation(OperationKind::kWildcardEq, graph.internSymbol("wildcard_eq_op"));
        graph.addOperand(wildcardEq, a);
        graph.addOperand(wildcardEq, b);
        graph.addResult(wildcardEq, wildcardEqY);
        graph.bindOutputPort("wildcard_eq_y", wildcardEqY);

        ValueId wildcardNeY = makeLogicValue(graph, "wildcard_ne_y", 1);
        OperationId wildcardNe = graph.createOperation(OperationKind::kWildcardNe, graph.internSymbol("wildcard_ne_op"));
        graph.addOperand(wildcardNe, comb);
        graph.addOperand(wildcardNe, b);
        graph.addResult(wildcardNe, wildcardNeY);
        graph.bindOutputPort("wildcard_ne_y", wildcardNeY);

        ValueId sliceArrayIn = makeLogicValue(graph, "slice_array_in", 72);
        OperationId sliceArrayConcat = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("slice_array_concat_op"));
        graph.addOperand(sliceArrayConcat, comb);
        graph.addOperand(sliceArrayConcat, b);
        graph.addOperand(sliceArrayConcat, a);
        graph.addOperand(sliceArrayConcat, comb);
        graph.addOperand(sliceArrayConcat, b);
        graph.addOperand(sliceArrayConcat, a);
        graph.addOperand(sliceArrayConcat, comb);
        graph.addOperand(sliceArrayConcat, b);
        graph.addOperand(sliceArrayConcat, a);
        graph.addResult(sliceArrayConcat, sliceArrayIn);
        graph.setAttr(sliceArrayConcat, "svPackedArray.version", static_cast<int64_t>(1));
        graph.setAttr(sliceArrayConcat, "svPackedArray.elementWidth", static_cast<int64_t>(8));
        graph.setAttr(sliceArrayConcat, "svPackedArray.elementCount", static_cast<int64_t>(9));
        graph.setAttr(sliceArrayConcat, "svPackedArray.indexLow", static_cast<int64_t>(0));
        graph.setAttr(sliceArrayConcat, "svPackedArray.indexHigh", static_cast<int64_t>(8));
        graph.setAttr(sliceArrayConcat, "svPackedArray.indexDirection", std::string("downto"));
        graph.setAttr(sliceArrayConcat, "svPackedArray.laneOrder", std::string("lsb_index_low"));
        graph.setAttr(sliceArrayConcat, "svPackedArray.concat.operand0Index", static_cast<int64_t>(8));
        graph.setAttr(sliceArrayConcat, "svPackedArray.concat.operandStride", static_cast<int64_t>(-1));

        ValueId sliceArrayY = makeLogicValue(graph, "slice_array_y", 8);
        OperationId sliceArray = graph.createOperation(OperationKind::kSliceArray,
                                                       graph.internSymbol("slice_array_op"));
        graph.addOperand(sliceArray, sliceArrayIn);
        graph.addOperand(sliceArray, rep2);
        graph.addResult(sliceArray, sliceArrayY);
        graph.setAttr(sliceArray, "sliceWidth", static_cast<int64_t>(8));
        graph.bindOutputPort("slice_array_y", sliceArrayY);

        ValueId clog2Y = makeLogicValue(graph, "clog2_y", 32);
        OperationId clog2Op = graph.createOperation(OperationKind::kSystemFunction,
                                                    graph.internSymbol("clog2_op"));
        graph.addOperand(clog2Op, comb);
        graph.addResult(clog2Op, clog2Y);
        graph.setAttr(clog2Op, "name", std::string("clog2"));
        graph.bindOutputPort("clog2_y", clog2Y);

        ValueId signedAssignY = makeLogicValue(graph, "signed_assign_y", 8, true);
        OperationId signedAssign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("signed_assign_op"));
        graph.addOperand(signedAssign, ss4);
        graph.addResult(signedAssign, signedAssignY);
        graph.bindOutputPort("signed_assign_y", signedAssignY);

        ValueId signedAddY = makeLogicValue(graph, "signed_add_y", 8, true);
        OperationId signedAdd = graph.createOperation(OperationKind::kAdd, graph.internSymbol("signed_add_op"));
        graph.addOperand(signedAdd, ss4);
        graph.addOperand(signedAdd, sa);
        graph.addResult(signedAdd, signedAddY);
        graph.bindOutputPort("signed_add_y", signedAddY);

        ValueId mixedAddY = makeLogicValue(graph, "mixed_add_y", 8);
        OperationId mixedAdd = graph.createOperation(OperationKind::kAdd, graph.internSymbol("mixed_add_op"));
        graph.addOperand(mixedAdd, ss4);
        graph.addOperand(mixedAdd, b);
        graph.addResult(mixedAdd, mixedAddY);
        graph.bindOutputPort("mixed_add_y", mixedAddY);

        ValueId signedDivY = makeLogicValue(graph, "signed_div_y", 8, true);
        OperationId signedDiv = graph.createOperation(OperationKind::kDiv, graph.internSymbol("signed_div_op"));
        graph.addOperand(signedDiv, ss4);
        graph.addOperand(signedDiv, signedTwo8);
        graph.addResult(signedDiv, signedDivY);
        graph.bindOutputPort("signed_div_y", signedDivY);

        ValueId signedModY = makeLogicValue(graph, "signed_mod_y", 8, true);
        OperationId signedMod = graph.createOperation(OperationKind::kMod, graph.internSymbol("signed_mod_op"));
        graph.addOperand(signedMod, ss4);
        graph.addOperand(signedMod, signedThree8);
        graph.addResult(signedMod, signedModY);
        graph.bindOutputPort("signed_mod_y", signedModY);

        ValueId signedLtY = makeLogicValue(graph, "signed_lt_y", 1);
        OperationId signedLt = graph.createOperation(OperationKind::kLt, graph.internSymbol("signed_lt_op"));
        graph.addOperand(signedLt, ss4);
        graph.addOperand(signedLt, signedOne8);
        graph.addResult(signedLt, signedLtY);
        graph.bindOutputPort("signed_lt_y", signedLtY);

        ValueId mixedLtY = makeLogicValue(graph, "mixed_lt_y", 1);
        OperationId mixedLt = graph.createOperation(OperationKind::kLt, graph.internSymbol("mixed_lt_op"));
        graph.addOperand(mixedLt, ss4);
        graph.addOperand(mixedLt, b);
        graph.addResult(mixedLt, mixedLtY);
        graph.bindOutputPort("mixed_lt_y", mixedLtY);

        ValueId wideSignedAssignY = makeLogicValue(graph, "wide_signed_assign_y", 130, true);
        OperationId wideSignedAssign = graph.createOperation(OperationKind::kAssign,
                                                             graph.internSymbol("wide_signed_assign_op"));
        graph.addOperand(wideSignedAssign, wideSignedIn);
        graph.addResult(wideSignedAssign, wideSignedAssignY);
        graph.bindOutputPort("wide_signed_assign_y", wideSignedAssignY);

        ValueId wideSignedDivY = makeLogicValue(graph, "wide_signed_div_y", 130, true);
        OperationId wideSignedDiv = graph.createOperation(OperationKind::kDiv,
                                                          graph.internSymbol("wide_signed_div_op"));
        graph.addOperand(wideSignedDiv, wideSignedIn);
        graph.addOperand(wideSignedDiv, wideSignedTwo);
        graph.addResult(wideSignedDiv, wideSignedDivY);
        graph.bindOutputPort("wide_signed_div_y", wideSignedDivY);

        ValueId wideSignedLtY = makeLogicValue(graph, "wide_signed_lt_y", 1);
        OperationId wideSignedLt = graph.createOperation(OperationKind::kLt,
                                                         graph.internSymbol("wide_signed_lt_op"));
        graph.addOperand(wideSignedLt, wideSignedIn);
        graph.addOperand(wideSignedLt, wideSignedOne);
        graph.addResult(wideSignedLt, wideSignedLtY);
        graph.bindOutputPort("wide_signed_lt_y", wideSignedLtY);

        ValueId wideMixedLtY = makeLogicValue(graph, "wide_mixed_lt_y", 1);
        OperationId wideMixedLt = graph.createOperation(OperationKind::kLt,
                                                        graph.internSymbol("wide_mixed_lt_op"));
        graph.addOperand(wideMixedLt, wideSignedIn);
        graph.addOperand(wideMixedLt, wideTwo);
        graph.addResult(wideMixedLt, wideMixedLtY);
        graph.bindOutputPort("wide_mixed_lt_y", wideMixedLtY);

        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("reg_q_write"));
        graph.addOperand(write, en);
        graph.addOperand(write, sum);
        graph.addOperand(write, mask);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("reg_q"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId maskedReg = graph.createOperation(OperationKind::kRegister,
                                                      graph.internSymbol("masked_reg_q"));
        graph.setAttr(maskedReg, "width", static_cast<int64_t>(8));
        graph.setAttr(maskedReg, "isSigned", false);
        graph.setAttr(maskedReg, "initValue", std::string("8'h0"));

        ValueId maskedRegQ = makeLogicValue(graph, "masked_reg_q_read", 8);
        OperationId maskedRegRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                          graph.internSymbol("masked_reg_q_read_op"));
        graph.addResult(maskedRegRead, maskedRegQ);
        graph.setAttr(maskedRegRead, "regSymbol", std::string("masked_reg_q"));
        graph.bindOutputPort("masked_y", maskedRegQ);

        OperationId maskedWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                        graph.internSymbol("masked_reg_q_write"));
        graph.addOperand(maskedWrite, en);
        graph.addOperand(maskedWrite, a);
        graph.addOperand(maskedWrite, b);
        graph.addOperand(maskedWrite, clk);
        graph.setAttr(maskedWrite, "regSymbol", std::string("masked_reg_q"));
        graph.setAttr(maskedWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId wideReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("wide_reg_q"));
        graph.setAttr(wideReg, "width", static_cast<int64_t>(130));
        graph.setAttr(wideReg, "isSigned", false);
        graph.setAttr(wideReg, "initValue", std::string("130'h2"));

        OperationId wideRandReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("wide_rand_reg_q"));
        graph.setAttr(wideRandReg, "width", static_cast<int64_t>(130));
        graph.setAttr(wideRandReg, "isSigned", false);
        graph.setAttr(wideRandReg, "initValue", std::string("$random"));

        ValueId wideRandRegQ = makeLogicValue(graph, "wide_rand_reg_q_read", 130);
        OperationId wideRandRegRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                            graph.internSymbol("wide_rand_reg_q_read_op"));
        graph.addResult(wideRandRegRead, wideRandRegQ);
        graph.setAttr(wideRandRegRead, "regSymbol", std::string("wide_rand_reg_q"));
        graph.bindOutputPort("rand_wide_y", wideRandRegQ);

        ValueId wideRegQ = makeLogicValue(graph, "wide_reg_q_read", 130);
        OperationId wideRegRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                        graph.internSymbol("wide_reg_q_read_op"));
        graph.addResult(wideRegRead, wideRegQ);
        graph.setAttr(wideRegRead, "regSymbol", std::string("wide_reg_q"));
        graph.bindOutputPort("wide_y", wideRegQ);

        OperationId wideRegWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                         graph.internSymbol("wide_reg_q_write"));
        graph.addOperand(wideRegWrite, en);
        graph.addOperand(wideRegWrite, wideIn);
        graph.addOperand(wideRegWrite, wideMask);
        graph.addOperand(wideRegWrite, clk);
        graph.setAttr(wideRegWrite, "regSymbol", std::string("wide_reg_q"));
        graph.setAttr(wideRegWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId wideMaskedReg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol("wide_masked_reg_q"));
        graph.setAttr(wideMaskedReg, "width", static_cast<int64_t>(130));
        graph.setAttr(wideMaskedReg, "isSigned", false);
        graph.setAttr(wideMaskedReg, "initValue", std::string("130'h0"));

        ValueId wideMaskedRegQ = makeLogicValue(graph, "wide_masked_reg_q_read", 130);
        OperationId wideMaskedRegRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                              graph.internSymbol("wide_masked_reg_q_read_op"));
        graph.addResult(wideMaskedRegRead, wideMaskedRegQ);
        graph.setAttr(wideMaskedRegRead, "regSymbol", std::string("wide_masked_reg_q"));
        graph.bindOutputPort("wide_masked_reg_y", wideMaskedRegQ);

        OperationId wideMaskedRegWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                               graph.internSymbol("wide_masked_reg_q_write"));
        graph.addOperand(wideMaskedRegWrite, en);
        graph.addOperand(wideMaskedRegWrite, wideIn);
        graph.addOperand(wideMaskedRegWrite, wideMaskDyn);
        graph.addOperand(wideMaskedRegWrite, clk);
        graph.setAttr(wideMaskedRegWrite, "regSymbol", std::string("wide_masked_reg_q"));
        graph.setAttr(wideMaskedRegWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId wideMem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("wide_mem"));
        graph.setAttr(wideMem, "width", static_cast<int64_t>(130));
        graph.setAttr(wideMem, "row", static_cast<int64_t>(4));
        graph.setAttr(wideMem, "isSigned", false);
        graph.setAttr(wideMem, "initKind", std::vector<std::string>{"readmemh"});
        graph.setAttr(wideMem, "initFile", std::vector<std::string>{wideMemInitFile});
        graph.setAttr(wideMem, "initValue", std::vector<std::string>{""});
        graph.setAttr(wideMem, "initStart", std::vector<int64_t>{0});
        graph.setAttr(wideMem, "initLen", std::vector<int64_t>{0});

        ValueId wideMemQ = makeLogicValue(graph, "wide_mem_q", 130);
        OperationId wideMemRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                        graph.internSymbol("wide_mem_read"));
        graph.addOperand(wideMemRead, wideAddr);
        graph.addResult(wideMemRead, wideMemQ);
        graph.setAttr(wideMemRead, "memSymbol", std::string("wide_mem"));
        graph.bindOutputPort("wide_mem_y", wideMemQ);

        OperationId wideMemWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                         graph.internSymbol("wide_mem_write"));
        graph.addOperand(wideMemWrite, en);
        graph.addOperand(wideMemWrite, wideAddr);
        graph.addOperand(wideMemWrite, wideIn);
        graph.addOperand(wideMemWrite, wideMask);
        graph.addOperand(wideMemWrite, clk);
        graph.setAttr(wideMemWrite, "memSymbol", std::string("wide_mem"));
        graph.setAttr(wideMemWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId wideMaskedMem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("wide_masked_mem"));
        graph.setAttr(wideMaskedMem, "width", static_cast<int64_t>(130));
        graph.setAttr(wideMaskedMem, "row", static_cast<int64_t>(4));
        graph.setAttr(wideMaskedMem, "isSigned", false);
        graph.setAttr(wideMaskedMem, "initKind", std::vector<std::string>{});
        graph.setAttr(wideMaskedMem, "initFile", std::vector<std::string>{});
        graph.setAttr(wideMaskedMem, "initValue", std::vector<std::string>{});
        graph.setAttr(wideMaskedMem, "initStart", std::vector<int64_t>{});
        graph.setAttr(wideMaskedMem, "initLen", std::vector<int64_t>{});

        ValueId wideMaskedMemQ = makeLogicValue(graph, "wide_masked_mem_q", 130);
        OperationId wideMaskedMemRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                              graph.internSymbol("wide_masked_mem_read"));
        graph.addOperand(wideMaskedMemRead, wideAddr);
        graph.addResult(wideMaskedMemRead, wideMaskedMemQ);
        graph.setAttr(wideMaskedMemRead, "memSymbol", std::string("wide_masked_mem"));
        graph.bindOutputPort("wide_masked_mem_y", wideMaskedMemQ);

        OperationId wideMaskedMemWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                               graph.internSymbol("wide_masked_mem_write"));
        graph.addOperand(wideMaskedMemWrite, en);
        graph.addOperand(wideMaskedMemWrite, wideAddr);
        graph.addOperand(wideMaskedMemWrite, wideIn);
        graph.addOperand(wideMaskedMemWrite, wideMaskDyn);
        graph.addOperand(wideMaskedMemWrite, clk);
        graph.setAttr(wideMaskedMemWrite, "memSymbol", std::string("wide_masked_mem"));
        graph.setAttr(wideMaskedMemWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId idxMem = graph.createOperation(OperationKind::kMemory, graph.internSymbol("idx_mem"));
        graph.setAttr(idxMem, "width", static_cast<int64_t>(8));
        graph.setAttr(idxMem, "row", static_cast<int64_t>(3));
        graph.setAttr(idxMem, "isSigned", false);
        graph.setAttr(idxMem, "initKind", std::vector<std::string>{"literal", "literal", "literal"});
        graph.setAttr(idxMem, "initFile", std::vector<std::string>{"", "", ""});
        graph.setAttr(idxMem, "initValue", std::vector<std::string>{"8'h11", "8'h22", "8'h33"});
        graph.setAttr(idxMem, "initStart", std::vector<int64_t>{0, 1, 2});
        graph.setAttr(idxMem, "initLen", std::vector<int64_t>{1, 1, 1});

        ValueId idxMemQ = makeLogicValue(graph, "idx_mem_q", 8);
        OperationId idxMemRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                       graph.internSymbol("idx_mem_read"));
        graph.addOperand(idxMemRead, wideMemIdx);
        graph.addResult(idxMemRead, idxMemQ);
        graph.setAttr(idxMemRead, "memSymbol", std::string("idx_mem"));
        graph.bindOutputPort("idx_mem_y", idxMemQ);

        OperationId idxMemWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                        graph.internSymbol("idx_mem_write"));
        graph.addOperand(idxMemWrite, en);
        graph.addOperand(idxMemWrite, wideMemIdx);
        graph.addOperand(idxMemWrite, idxWriteData);
        graph.addOperand(idxMemWrite, mask);
        graph.addOperand(idxMemWrite, clk);
        graph.setAttr(idxMemWrite, "memSymbol", std::string("idx_mem"));
        graph.setAttr(idxMemWrite, "eventEdge", std::vector<std::string>{"posedge"});

        ValueId wideAddY = makeLogicValue(graph, "wide_add_y", 130);
        OperationId wideAdd = graph.createOperation(OperationKind::kAdd, graph.internSymbol("wide_add_op"));
        graph.addOperand(wideAdd, wideIn);
        graph.addOperand(wideAdd, wideOne);
        graph.addResult(wideAdd, wideAddY);
        graph.bindOutputPort("wide_add_y", wideAddY);

        ValueId wideSubY = makeLogicValue(graph, "wide_sub_y", 130);
        OperationId wideSub = graph.createOperation(OperationKind::kSub, graph.internSymbol("wide_sub_op"));
        graph.addOperand(wideSub, wideIn);
        graph.addOperand(wideSub, wideOne);
        graph.addResult(wideSub, wideSubY);
        graph.bindOutputPort("wide_sub_y", wideSubY);

        ValueId wideMulY = makeLogicValue(graph, "wide_mul_y", 132);
        OperationId wideMul = graph.createOperation(OperationKind::kMul, graph.internSymbol("wide_mul_op"));
        graph.addOperand(wideMul, wideIn);
        graph.addOperand(wideMul, smallTwo);
        graph.addResult(wideMul, wideMulY);
        graph.bindOutputPort("wide_mul_y", wideMulY);

        ValueId wideDivY = makeLogicValue(graph, "wide_div_y", 130);
        OperationId wideDiv = graph.createOperation(OperationKind::kDiv, graph.internSymbol("wide_div_op"));
        graph.addOperand(wideDiv, wideIn);
        graph.addOperand(wideDiv, wideTwo);
        graph.addResult(wideDiv, wideDivY);
        graph.bindOutputPort("wide_div_y", wideDivY);

        ValueId wideModY = makeLogicValue(graph, "wide_mod_y", 130);
        OperationId wideMod = graph.createOperation(OperationKind::kMod, graph.internSymbol("wide_mod_op"));
        graph.addOperand(wideMod, wideIn);
        graph.addOperand(wideMod, wideTwo);
        graph.addResult(wideMod, wideModY);
        graph.bindOutputPort("wide_mod_y", wideModY);

        ValueId widePow65 = makeLogicValue(graph, "wide_pow65", 130);
        OperationId widePow65Op = graph.createOperation(OperationKind::kShl, graph.internSymbol("wide_pow65_op"));
        graph.addOperand(widePow65Op, wideOne);
        graph.addOperand(widePow65Op, sh65);
        graph.addResult(widePow65Op, widePow65);

        ValueId wideMulPowY = makeLogicValue(graph, "wide_mul_pow_y", 130);
        OperationId wideMulPow = graph.createOperation(OperationKind::kMul, graph.internSymbol("wide_mul_pow_op"));
        graph.addOperand(wideMulPow, wideIn);
        graph.addOperand(wideMulPow, widePow65);
        graph.addResult(wideMulPow, wideMulPowY);
        graph.bindOutputPort("wide_mul_pow_y", wideMulPowY);

        ValueId wideDivPowY = makeLogicValue(graph, "wide_div_pow_y", 130);
        OperationId wideDivPow = graph.createOperation(OperationKind::kDiv, graph.internSymbol("wide_div_pow_op"));
        graph.addOperand(wideDivPow, wideIn);
        graph.addOperand(wideDivPow, widePow65);
        graph.addResult(wideDivPow, wideDivPowY);
        graph.bindOutputPort("wide_div_pow_y", wideDivPowY);

        ValueId wideModPowY = makeLogicValue(graph, "wide_mod_pow_y", 130);
        OperationId wideModPow = graph.createOperation(OperationKind::kMod, graph.internSymbol("wide_mod_pow_op"));
        graph.addOperand(wideModPow, wideIn);
        graph.addOperand(wideModPow, widePow65);
        graph.addResult(wideModPow, wideModPowY);
        graph.bindOutputPort("wide_mod_pow_y", wideModPowY);

        ValueId wideDivGeneralY = makeLogicValue(graph, "wide_div_general_y", 130);
        OperationId wideDivGeneral = graph.createOperation(OperationKind::kDiv, graph.internSymbol("wide_div_general_op"));
        graph.addOperand(wideDivGeneral, wideIn);
        graph.addOperand(wideDivGeneral, wideGeneralDivisor);
        graph.addResult(wideDivGeneral, wideDivGeneralY);
        graph.bindOutputPort("wide_div_general_y", wideDivGeneralY);

        ValueId wideModGeneralY = makeLogicValue(graph, "wide_mod_general_y", 130);
        OperationId wideModGeneral = graph.createOperation(OperationKind::kMod, graph.internSymbol("wide_mod_general_op"));
        graph.addOperand(wideModGeneral, wideIn);
        graph.addOperand(wideModGeneral, wideGeneralDivisor);
        graph.addResult(wideModGeneral, wideModGeneralY);
        graph.bindOutputPort("wide_mod_general_y", wideModGeneralY);

        ValueId midMulY = makeLogicValue(graph, "mid_mul_y", 96);
        OperationId midMul = graph.createOperation(OperationKind::kMul, graph.internSymbol("mid_mul_op"));
        graph.addOperand(midMul, midIn);
        graph.addOperand(midMul, midWideConst);
        graph.addResult(midMul, midMulY);
        graph.bindOutputPort("mid_mul_y", midMulY);

        ValueId midDivY = makeLogicValue(graph, "mid_div_y", 96);
        OperationId midDiv = graph.createOperation(OperationKind::kDiv, graph.internSymbol("mid_div_op"));
        graph.addOperand(midDiv, midIn);
        graph.addOperand(midDiv, midWideConst);
        graph.addResult(midDiv, midDivY);
        graph.bindOutputPort("mid_div_y", midDivY);

        ValueId midModY = makeLogicValue(graph, "mid_mod_y", 96);
        OperationId midMod = graph.createOperation(OperationKind::kMod, graph.internSymbol("mid_mod_op"));
        graph.addOperand(midMod, midIn);
        graph.addOperand(midMod, midWideConst);
        graph.addResult(midMod, midModY);
        graph.bindOutputPort("mid_mod_y", midModY);

        ValueId midAddY = makeLogicValue(graph, "mid_add_y", 96);
        OperationId midAdd = graph.createOperation(OperationKind::kAdd, graph.internSymbol("mid_add_op"));
        graph.addOperand(midAdd, midIn);
        graph.addOperand(midAdd, midWideConst);
        graph.addResult(midAdd, midAddY);
        graph.bindOutputPort("mid_add_y", midAddY);

        ValueId midSubY = makeLogicValue(graph, "mid_sub_y", 96);
        OperationId midSub = graph.createOperation(OperationKind::kSub, graph.internSymbol("mid_sub_op"));
        graph.addOperand(midSub, midIn);
        graph.addOperand(midSub, midWideConst);
        graph.addResult(midSub, midSubY);
        graph.bindOutputPort("mid_sub_y", midSubY);

        ValueId midEqY = makeLogicValue(graph, "mid_eq_y", 1);
        OperationId midEq = graph.createOperation(OperationKind::kEq, graph.internSymbol("mid_eq_op"));
        graph.addOperand(midEq, midIn);
        graph.addOperand(midEq, midWideConst);
        graph.addResult(midEq, midEqY);
        graph.bindOutputPort("mid_eq_y", midEqY);

        ValueId midLtY = makeLogicValue(graph, "mid_lt_y", 1);
        OperationId midLt = graph.createOperation(OperationKind::kLt, graph.internSymbol("mid_lt_op"));
        graph.addOperand(midLt, midIn);
        graph.addOperand(midLt, midWideConst);
        graph.addResult(midLt, midLtY);
        graph.bindOutputPort("mid_lt_y", midLtY);

        ValueId wideAndY = makeLogicValue(graph, "wide_and_y", 130);
        OperationId wideAnd = graph.createOperation(OperationKind::kAnd, graph.internSymbol("wide_and_op"));
        graph.addOperand(wideAnd, wideIn);
        graph.addOperand(wideAnd, wideMask);
        graph.addResult(wideAnd, wideAndY);
        graph.bindOutputPort("wide_and_y", wideAndY);

        ValueId wideOrY = makeLogicValue(graph, "wide_or_y", 130);
        OperationId wideOr = graph.createOperation(OperationKind::kOr, graph.internSymbol("wide_or_op"));
        graph.addOperand(wideOr, wideIn);
        graph.addOperand(wideOr, wideZero);
        graph.addResult(wideOr, wideOrY);
        graph.bindOutputPort("wide_or_y", wideOrY);

        ValueId wideXorY = makeLogicValue(graph, "wide_xor_y", 130);
        OperationId wideXor = graph.createOperation(OperationKind::kXor, graph.internSymbol("wide_xor_op"));
        graph.addOperand(wideXor, wideIn);
        graph.addOperand(wideXor, wideZero);
        graph.addResult(wideXor, wideXorY);
        graph.bindOutputPort("wide_xor_y", wideXorY);

        ValueId wideXnorY = makeLogicValue(graph, "wide_xnor_y", 130);
        OperationId wideXnor = graph.createOperation(OperationKind::kXnor, graph.internSymbol("wide_xnor_op"));
        graph.addOperand(wideXnor, wideIn);
        graph.addOperand(wideXnor, wideMask);
        graph.addResult(wideXnor, wideXnorY);
        graph.bindOutputPort("wide_xnor_y", wideXnorY);

        ValueId wideNotY = makeLogicValue(graph, "wide_not_y", 130);
        OperationId wideNot = graph.createOperation(OperationKind::kNot, graph.internSymbol("wide_not_op"));
        graph.addOperand(wideNot, wideIn);
        graph.addResult(wideNot, wideNotY);
        graph.bindOutputPort("wide_not_y", wideNotY);

        ValueId wideEqY = makeLogicValue(graph, "wide_eq_y", 1);
        OperationId wideEq = graph.createOperation(OperationKind::kEq, graph.internSymbol("wide_eq_op"));
        graph.addOperand(wideEq, wideIn);
        graph.addOperand(wideEq, wideIn);
        graph.addResult(wideEq, wideEqY);
        graph.bindOutputPort("wide_eq_y", wideEqY);

        ValueId wideLtY = makeLogicValue(graph, "wide_lt_y", 1);
        OperationId wideLt = graph.createOperation(OperationKind::kLt, graph.internSymbol("wide_lt_op"));
        graph.addOperand(wideLt, wideOne);
        graph.addOperand(wideLt, wideIn);
        graph.addResult(wideLt, wideLtY);
        graph.bindOutputPort("wide_lt_y", wideLtY);

        ValueId wideLogicAndY = makeLogicValue(graph, "wide_logic_and_y", 1);
        OperationId wideLogicAnd = graph.createOperation(OperationKind::kLogicAnd, graph.internSymbol("wide_logic_and_op"));
        graph.addOperand(wideLogicAnd, wideIn);
        graph.addOperand(wideLogicAnd, wideOne);
        graph.addResult(wideLogicAnd, wideLogicAndY);
        graph.bindOutputPort("wide_logic_and_y", wideLogicAndY);

        ValueId wideReduceOrY = makeLogicValue(graph, "wide_reduce_or_y", 1);
        OperationId wideReduceOr = graph.createOperation(OperationKind::kReduceOr, graph.internSymbol("wide_reduce_or_op"));
        graph.addOperand(wideReduceOr, wideIn);
        graph.addResult(wideReduceOr, wideReduceOrY);
        graph.bindOutputPort("wide_reduce_or_y", wideReduceOrY);

        ValueId wideShlY = makeLogicValue(graph, "wide_shl_y", 130);
        OperationId wideShl = graph.createOperation(OperationKind::kShl, graph.internSymbol("wide_shl_op"));
        graph.addOperand(wideShl, wideIn);
        graph.addOperand(wideShl, wideAddr);
        graph.addResult(wideShl, wideShlY);
        graph.bindOutputPort("wide_shl_y", wideShlY);

        ValueId wideLshrY = makeLogicValue(graph, "wide_lshr_y", 130);
        OperationId wideLshr = graph.createOperation(OperationKind::kLShr, graph.internSymbol("wide_lshr_op"));
        graph.addOperand(wideLshr, wideIn);
        graph.addOperand(wideLshr, wideAddr);
        graph.addResult(wideLshr, wideLshrY);
        graph.bindOutputPort("wide_lshr_y", wideLshrY);

        ValueId wideAshrY = makeLogicValue(graph, "wide_ashr_y", 130);
        OperationId wideAshr = graph.createOperation(OperationKind::kAShr, graph.internSymbol("wide_ashr_op"));
        graph.addOperand(wideAshr, wideMask);
        graph.addOperand(wideAshr, wideAddr);
        graph.addResult(wideAshr, wideAshrY);
        graph.bindOutputPort("wide_ashr_y", wideAshrY);

        ValueId wideMuxY = makeLogicValue(graph, "wide_mux_y", 130);
        OperationId wideMux = graph.createOperation(OperationKind::kMux, graph.internSymbol("wide_mux_op"));
        graph.addOperand(wideMux, en);
        graph.addOperand(wideMux, wideIn);
        graph.addOperand(wideMux, wideZero);
        graph.addResult(wideMux, wideMuxY);
        graph.bindOutputPort("wide_mux_y", wideMuxY);

        ValueId wideConcatY = makeLogicValue(graph, "wide_concat_y", 132);
        OperationId wideConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("wide_concat_op"));
        graph.addOperand(wideConcat, wideAddr);
        graph.addOperand(wideConcat, wideIn);
        graph.addResult(wideConcat, wideConcatY);
        graph.bindOutputPort("wide_concat_y", wideConcatY);

        ValueId wideRepY = makeLogicValue(graph, "wide_rep_y", 260);
        OperationId wideRep = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("wide_rep_op"));
        graph.addOperand(wideRep, wideIn);
        graph.addResult(wideRep, wideRepY);
        graph.setAttr(wideRep, "rep", static_cast<int64_t>(2));
        graph.bindOutputPort("wide_rep_y", wideRepY);

        ValueId wideSliceStaticY = makeLogicValue(graph, "wide_slice_static_y", 65);
        OperationId wideSliceStatic = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("wide_slice_static_op"));
        graph.addOperand(wideSliceStatic, wideIn);
        graph.addResult(wideSliceStatic, wideSliceStaticY);
        graph.setAttr(wideSliceStatic, "sliceStart", static_cast<int64_t>(5));
        graph.setAttr(wideSliceStatic, "sliceEnd", static_cast<int64_t>(69));
        graph.bindOutputPort("wide_slice_static_y", wideSliceStaticY);

        ValueId wideSliceDynY = makeLogicValue(graph, "wide_slice_dyn_y", 65);
        OperationId wideSliceDyn = graph.createOperation(OperationKind::kSliceDynamic, graph.internSymbol("wide_slice_dyn_op"));
        graph.addOperand(wideSliceDyn, wideIn);
        graph.addOperand(wideSliceDyn, wideAddr);
        graph.addResult(wideSliceDyn, wideSliceDynY);
        graph.setAttr(wideSliceDyn, "sliceWidth", static_cast<int64_t>(65));
        graph.bindOutputPort("wide_slice_dyn_y", wideSliceDynY);

        OperationId display = graph.createOperation(OperationKind::kSystemTask,
                                                    graph.internSymbol("display_task"));
        graph.addOperand(display, en);
        graph.addOperand(display, fmt);
        graph.addOperand(display, sum);
        graph.addOperand(display, clk);
        graph.setAttr(display, "name", std::string("display"));
        graph.setAttr(display, "procKind", std::string("always_ff"));
        graph.setAttr(display, "hasTiming", false);
        graph.setAttr(display, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId dpiImport = graph.createOperation(OperationKind::kDpicImport,
                                                      graph.internSymbol("trace_sum"));
        graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"value"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        OperationId dpi = graph.createOperation(OperationKind::kDpicCall,
                                                graph.internSymbol("trace_dpi_call"));
        graph.addOperand(dpi, en);
        graph.addOperand(dpi, sum);
        graph.addOperand(dpi, clk);
        graph.setAttr(dpi, "targetImportSymbol", std::string("trace_sum"));
        graph.setAttr(dpi, "inArgName", std::vector<std::string>{"value"});
        graph.setAttr(dpi, "outArgName", std::vector<std::string>{});
        graph.setAttr(dpi, "hasReturn", false);
        graph.setAttr(dpi, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    bool runActivitySchedule(Design &design, SessionStore &session, ActivityScheduleOptions options = {})
    {
        if (options.path.empty())
        {
            options.path = "top";
        }
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(std::move(options)));
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            for (const auto &diag : diags.messages())
            {
                std::cerr << "[emit_grhsim_cpp] activity-schedule diagnostic: "
                          << diag.message << '\n';
            }
        }
        return result.success && !diags.hasError();
    }

    bool emitWithActivitySchedule(Design &design,
                                  const std::filesystem::path &outDir,
                                  EmitDiagnostics &diag,
                                  EmitResult &result,
                                  ActivityScheduleOptions scheduleOptions = {},
                                  bool posedgeFullpassSpecialization = false,
                                  bool perf = false,
                                  bool stateReadLocalityStats = false,
                                  std::optional<bool> directSingleWriterStateReads = false,
                                  bool materializedScalarReadLocalityStats = false,
                                  bool fullActiveWordConsume = false,
                                  std::optional<bool> pureEventComputeWordBypass = false,
                                  std::optional<bool> pureEventComputeWordProfile = std::nullopt,
                                  std::size_t schedBatchMaxOps = 8u,
                                  std::size_t schedBatchMaxEstimatedLines = 96u,
                                  std::optional<std::string_view> pureEventWordPackPolicy = std::nullopt,
                                  std::optional<std::size_t> pureEventWordPackMaxMovedSupernodePpm = std::nullopt,
                                  std::optional<std::size_t> pureEventWordPackMaxChangedWordPpm = std::nullopt,
                                  bool removeActivityScheduleDag = false,
                                  std::optional<std::string_view> pureEventWordPackMaxMovedSupernodePpmRaw = std::nullopt,
                                  std::optional<std::string_view> pureEventWordPackMaxChangedWordPpmRaw = std::nullopt)
    {
        SessionStore session;
        if (scheduleOptions.path.empty())
        {
            scheduleOptions.path = "top";
        }
        if (!runActivitySchedule(design, session, std::move(scheduleOptions)))
        {
            return false;
        }
        if (removeActivityScheduleDag)
        {
            session.erase("top.activity_schedule.dag");
        }
        std::filesystem::create_directories(outDir);
        EmitOptions options;
        options.outputDir = outDir.string();
        options.session = &session;
        options.sessionPathPrefix = std::string("top");
        options.attributes["sched_batch_max_ops"] = std::to_string(schedBatchMaxOps);
        options.attributes["sched_batch_max_estimated_lines"] = std::to_string(schedBatchMaxEstimatedLines);
        options.attributes["emit_parallelism"] = "2";
        if (posedgeFullpassSpecialization)
        {
            options.attributes["posedge_fullpass_specialization"] = "1";
        }
        if (perf)
        {
            options.attributes["perf"] = "eval";
        }
        if (stateReadLocalityStats)
        {
            options.attributes["state_read_locality_stats"] = "1";
        }
        if (directSingleWriterStateReads.has_value())
        {
            options.attributes["direct_single_writer_state_reads"] =
                *directSingleWriterStateReads ? "1" : "0";
        }
        if (materializedScalarReadLocalityStats)
        {
            options.attributes["materialized_scalar_read_locality_stats"] = "1";
        }
        if (fullActiveWordConsume)
        {
            options.attributes["full_active_word_consume"] = "1";
        }
        if (pureEventComputeWordBypass)
        {
            options.attributes["pure_event_compute_word_bypass"] = *pureEventComputeWordBypass ? "1" : "0";
        }
        if (pureEventComputeWordProfile)
        {
            options.attributes["pure_event_compute_word_profile"] = *pureEventComputeWordProfile ? "1" : "0";
        }
        if (pureEventWordPackPolicy)
        {
            options.attributes["pure_event_word_pack_policy"] = std::string(*pureEventWordPackPolicy);
        }
        if (pureEventWordPackMaxMovedSupernodePpm)
        {
            options.attributes["pure_event_word_pack_max_moved_supernode_ppm"] =
                std::to_string(*pureEventWordPackMaxMovedSupernodePpm);
        }
        if (pureEventWordPackMaxChangedWordPpm)
        {
            options.attributes["pure_event_word_pack_max_changed_word_ppm"] =
                std::to_string(*pureEventWordPackMaxChangedWordPpm);
        }
        if (pureEventWordPackMaxMovedSupernodePpmRaw)
        {
            options.attributes["pure_event_word_pack_max_moved_supernode_ppm"] =
                std::string(*pureEventWordPackMaxMovedSupernodePpmRaw);
        }
        if (pureEventWordPackMaxChangedWordPpmRaw)
        {
            options.attributes["pure_event_word_pack_max_changed_word_ppm"] =
                std::string(*pureEventWordPackMaxChangedWordPpmRaw);
        }

        EmitGrhSimCpp emitter(&diag);
        result = emitter.emit(design, options);
        return true;
    }

    bool emitCommitLocalityCase(
        Design &design,
        const std::filesystem::path &outDir,
        ActivityScheduleOptions scheduleOptions,
        bool removeMetadata,
        const ActivityScheduleCommitLocalityGroupByOp *metadataOverride,
        const ActivityScheduleCommitLocalityGroupOrder *orderOverride,
        EmitDiagnostics &diag,
        EmitResult &result)
    {
        SessionStore session;
        if (!runActivitySchedule(design, session, std::move(scheduleOptions)))
        {
            return false;
        }
        if (metadataOverride != nullptr)
        {
            session.insert_or_assign(
                "top.activity_schedule.commit_locality_group_by_op",
                std::make_unique<SessionSlotValue<ActivityScheduleCommitLocalityGroupByOp>>(
                    *metadataOverride,
                    "activity-schedule.commit-locality-group-by-op"));
        }
        if (orderOverride != nullptr)
        {
            session.insert_or_assign(
                "top.activity_schedule.commit_locality_group_order",
                std::make_unique<SessionSlotValue<ActivityScheduleCommitLocalityGroupOrder>>(
                    *orderOverride,
                    "activity-schedule.commit-locality-group-order"));
        }
        if (removeMetadata)
        {
            session.erase("top.activity_schedule.commit_locality_group_by_op");
            session.erase("top.activity_schedule.commit_locality_group_order");
        }

        std::filesystem::create_directories(outDir);
        EmitOptions options;
        options.outputDir = outDir.string();
        options.session = &session;
        options.sessionPathPrefix = std::string("top");
        options.attributes["sched_batch_max_ops"] = "8";
        options.attributes["sched_batch_max_estimated_lines"] = "96";
        options.attributes["emit_parallelism"] = "2";
        EmitGrhSimCpp emitter(&diag);
        result = emitter.emit(design, options);
        return true;
    }

    Design buildWideConcatFastPathDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId a = makeLogicValue(graph, "a", 8);
        ValueId b = makeLogicValue(graph, "b", 8);
        ValueId c = makeLogicValue(graph, "c", 8);
        ValueId d = makeLogicValue(graph, "d", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("d", d);

        ValueId wideConcatMid = makeLogicValue(graph, "wide_concat_fast_mid", 96);
        OperationId wideConcat =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("wide_concat_fast_op"));
        for (int i = 0; i < 3; ++i)
        {
            graph.addOperand(wideConcat, a);
            graph.addOperand(wideConcat, b);
            graph.addOperand(wideConcat, c);
            graph.addOperand(wideConcat, d);
        }
        graph.addResult(wideConcat, wideConcatMid);
        graph.bindOutputPort("wide_concat_fast_mid", wideConcatMid);

        ValueId wideConcatSlice = makeLogicValue(graph, "wide_concat_fast_slice_y", 32);
        OperationId sliceOp =
            graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol("wide_concat_fast_slice_op"));
        graph.addOperand(sliceOp, wideConcatMid);
        graph.addResult(sliceOp, wideConcatSlice);
        graph.setAttr(sliceOp, "sliceStart", static_cast<int64_t>(32));
        graph.setAttr(sliceOp, "sliceEnd", static_cast<int64_t>(63));
        graph.bindOutputPort("wide_concat_fast_slice_y", wideConcatSlice);

        return design;
    }

    Design buildTwoWordHelperDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId a64 = makeLogicValue(graph, "a64", 64);
        ValueId b64 = makeLogicValue(graph, "b64", 64);
        ValueId wide96 = makeLogicValue(graph, "wide96", 96);
        ValueId tag8 = makeLogicValue(graph, "tag8", 8);
        ValueId broadcastBit = makeLogicValue(graph, "broadcast_bit", 1);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("a64", a64);
        graph.bindInputPort("b64", b64);
        graph.bindInputPort("wide96", wide96);
        graph.bindInputPort("tag8", tag8);
        graph.bindInputPort("broadcast_bit", broadcastBit);

        ValueId concat128 = makeLogicValue(graph, "concat128", 128);
        OperationId concat11 =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("two_word_concat_1_1"));
        graph.addOperand(concat11, a64);
        graph.addOperand(concat11, b64);
        graph.addResult(concat11, concat128);
        graph.bindOutputPort("concat128", concat128);

        ValueId concat104 = makeLogicValue(graph, "concat104", 104);
        OperationId concat12 =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("two_word_concat_1_2"));
        graph.addOperand(concat12, tag8);
        graph.addOperand(concat12, wide96);
        graph.addResult(concat12, concat104);
        graph.bindOutputPort("concat104", concat104);

        ValueId rep96 = makeLogicValue(graph, "rep96", 96);
        OperationId rep = graph.createOperation(OperationKind::kReplicate, graph.internSymbol("two_word_rep"));
        graph.addOperand(rep, tag8);
        graph.addResult(rep, rep96);
        graph.setAttr(rep, "rep", static_cast<int64_t>(12));
        graph.bindOutputPort("rep96", rep96);

        ValueId rep65 = makeLogicValue(graph, "rep65", 65);
        OperationId rep65Op =
            graph.createOperation(OperationKind::kReplicate, graph.internSymbol("replicate_bit_65"));
        graph.addOperand(rep65Op, broadcastBit);
        graph.addResult(rep65Op, rep65);
        graph.setAttr(rep65Op, "rep", static_cast<int64_t>(65));
        graph.bindOutputPort("rep65", rep65);

        ValueId rep256 = makeLogicValue(graph, "rep256", 256);
        OperationId rep256Op =
            graph.createOperation(OperationKind::kReplicate, graph.internSymbol("replicate_bit_256"));
        graph.addOperand(rep256Op, broadcastBit);
        graph.addResult(rep256Op, rep256);
        graph.setAttr(rep256Op, "rep", static_cast<int64_t>(256));
        graph.bindOutputPort("rep256", rep256);

        OperationId reg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("two_word_reg"));
        graph.setAttr(reg, "width", static_cast<int64_t>(96));
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("96'h0"));

        ValueId regQ = makeLogicValue(graph, "reg_q", 96);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("two_word_reg_read"));
        graph.addResult(read, regQ);
        graph.setAttr(read, "regSymbol", std::string("two_word_reg"));
        graph.bindOutputPort("reg_q", regQ);

        ValueId one = addConstant(graph, "two_word_one", "two_word_one_value", 1, "1'b1");
        ValueId mask = addConstant(graph, "two_word_mask", "two_word_mask_value", 96, allOnesLiteral(96));
        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("two_word_reg_write"));
        graph.addOperand(write, one);
        graph.addOperand(write, wide96);
        graph.addOperand(write, mask);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("two_word_reg"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildPackedActivationDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId trigger = makeLogicValue(graph, "trigger", 8);
        graph.bindInputPort("trigger", trigger);
        ValueId one = addConstant(graph, "packed_activation_one", "packed_activation_one_value", 8, "8'h1");

        for (std::size_t i = 0; i < 16u; ++i)
        {
            const std::string index = std::to_string(i);
            ValueId out = makeLogicValue(graph, "packed_activation_out_" + index, 8);
            OperationId add = graph.createOperation(OperationKind::kAdd,
                                                    graph.internSymbol("packed_activation_add_" + index));
            graph.addOperand(add, trigger);
            graph.addOperand(add, one);
            graph.addResult(add, out);
            graph.bindOutputPort("packed_activation_out_" + index, out);
        }

        return design;
    }

    Design buildFullActiveWordConsumeDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId current = makeLogicValue(graph, "chain_in", 8);
        graph.bindInputPort("chain_in", current);
        ValueId one = addConstant(graph, "chain_one", "chain_one_value", 8, "8'h1");
        for (std::size_t i = 0; i < 9u; ++i)
        {
            const std::string index = std::to_string(i);
            ValueId next = makeLogicValue(graph, "chain_value_" + index, 8);
            OperationId add = graph.createOperation(OperationKind::kAdd,
                                                    graph.internSymbol("chain_add_" + index));
            graph.addOperand(add, current);
            graph.addOperand(add, one);
            graph.addResult(add, next);
            current = next;
        }
        graph.bindOutputPort("chain_out", current);
        return design;
    }

    Design buildOverlappingActivationDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId a = makeLogicValue(graph, "a", 8);
        ValueId b = makeLogicValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        ValueId producer0 = makeLogicValue(graph, "overlap_producer0", 8);
        ValueId producer1 = makeLogicValue(graph, "overlap_producer1", 8);
        graph.bindOutputPort("overlap_producer1", producer1);

        OperationId dpiImport = graph.createOperation(OperationKind::kDpicImport,
                                                      graph.internSymbol("overlap_pair"));
        graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "input", "output", "output"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8, 8, 8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"a", "b", "out0", "out1"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false, false, false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic", "logic", "logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        OperationId dpiCall = graph.createOperation(OperationKind::kDpicCall,
                                                    graph.internSymbol("overlap_pair_call"));
        graph.addOperand(dpiCall, a);
        graph.addOperand(dpiCall, a);
        graph.addOperand(dpiCall, b);
        graph.addResult(dpiCall, producer0);
        graph.addResult(dpiCall, producer1);
        graph.setAttr(dpiCall, "targetImportSymbol", std::string("overlap_pair"));
        graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"a", "b"});
        graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{"out0", "out1"});
        graph.setAttr(dpiCall, "hasReturn", false);

        ValueId sharedConsumer = makeLogicValue(graph, "overlap_shared_consumer", 8);
        OperationId sharedConsumerOp = graph.createOperation(OperationKind::kAdd,
                                                            graph.internSymbol("overlap_shared_consumer_add"));
        graph.addOperand(sharedConsumerOp, producer0);
        graph.addOperand(sharedConsumerOp, producer1);
        graph.addResult(sharedConsumerOp, sharedConsumer);
        graph.bindOutputPort("overlap_shared_consumer", sharedConsumer);

        return design;
    }

    Design buildRegisterWriteInteractionDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId rstN = makeLogicValue(graph, "rst_n", 1);
        ValueId seqD = makeLogicValue(graph, "seq_d", 8);
        ValueId rstValue = makeLogicValue(graph, "rst_value", 8);
        ValueId writeA = makeLogicValue(graph, "write_a", 8);
        ValueId writeB = makeLogicValue(graph, "write_b", 8);
        ValueId fireA = makeLogicValue(graph, "fire_a", 1);
        ValueId fireB = makeLogicValue(graph, "fire_b", 1);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("rst_n", rstN);
        graph.bindInputPort("seq_d", seqD);
        graph.bindInputPort("rst_value", rstValue);
        graph.bindInputPort("write_a", writeA);
        graph.bindInputPort("write_b", writeB);
        graph.bindInputPort("fire_a", fireA);
        graph.bindInputPort("fire_b", fireB);

        ValueId one = addConstant(graph, "const_one", "one", 1, "1'b1");
        ValueId mask = addConstant(graph, "const_mask_ff", "mask_ff", 8, "8'hFF");

        OperationId seqReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("seq_reg"));
        graph.setAttr(seqReg, "width", static_cast<int64_t>(8));
        graph.setAttr(seqReg, "isSigned", false);
        graph.setAttr(seqReg, "initValue", std::string("8'h00"));

        ValueId seqQ = makeLogicValue(graph, "seq_q", 8);
        OperationId seqRead = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("seq_read"));
        graph.addResult(seqRead, seqQ);
        graph.setAttr(seqRead, "regSymbol", std::string("seq_reg"));
        graph.bindOutputPort("seq_q", seqQ);

        OperationId seqClkWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                        graph.internSymbol("seq_clk_write"));
        graph.addOperand(seqClkWrite, one);
        graph.addOperand(seqClkWrite, seqD);
        graph.addOperand(seqClkWrite, mask);
        graph.addOperand(seqClkWrite, clk);
        graph.setAttr(seqClkWrite, "regSymbol", std::string("seq_reg"));
        graph.setAttr(seqClkWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId seqRstWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                        graph.internSymbol("seq_rst_write"));
        graph.addOperand(seqRstWrite, one);
        graph.addOperand(seqRstWrite, rstValue);
        graph.addOperand(seqRstWrite, mask);
        graph.addOperand(seqRstWrite, rstN);
        graph.setAttr(seqRstWrite, "regSymbol", std::string("seq_reg"));
        graph.setAttr(seqRstWrite, "eventEdge", std::vector<std::string>{"negedge"});

        OperationId conflictReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("conflict_reg"));
        graph.setAttr(conflictReg, "width", static_cast<int64_t>(8));
        graph.setAttr(conflictReg, "isSigned", false);
        graph.setAttr(conflictReg, "initValue", std::string("8'h00"));

        ValueId conflictQ = makeLogicValue(graph, "conflict_q", 8);
        OperationId conflictRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                         graph.internSymbol("conflict_read"));
        graph.addResult(conflictRead, conflictQ);
        graph.setAttr(conflictRead, "regSymbol", std::string("conflict_reg"));
        graph.bindOutputPort("conflict_q", conflictQ);

        OperationId conflictWriteA = graph.createOperation(OperationKind::kRegisterWritePort,
                                                           graph.internSymbol("conflict_write_a"));
        graph.addOperand(conflictWriteA, fireA);
        graph.addOperand(conflictWriteA, writeA);
        graph.addOperand(conflictWriteA, mask);
        graph.addOperand(conflictWriteA, clk);
        graph.setAttr(conflictWriteA, "regSymbol", std::string("conflict_reg"));
        graph.setAttr(conflictWriteA, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId conflictWriteB = graph.createOperation(OperationKind::kRegisterWritePort,
                                                           graph.internSymbol("conflict_write_b"));
        graph.addOperand(conflictWriteB, fireB);
        graph.addOperand(conflictWriteB, writeB);
        graph.addOperand(conflictWriteB, mask);
        graph.addOperand(conflictWriteB, clk);
        graph.setAttr(conflictWriteB, "regSymbol", std::string("conflict_reg"));
        graph.setAttr(conflictWriteB, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildLocalTempDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId a = makeLogicValue(graph, "a", 8);
        ValueId b = makeLogicValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        ValueId sumTmp = makeLogicValue(graph, "sum_tmp", 8);
        OperationId add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum_tmp_add"));
        graph.addOperand(add, a);
        graph.addOperand(add, b);
        graph.addResult(add, sumTmp);

        ValueId y = makeLogicValue(graph, "y", 8);
        OperationId xorr = graph.createOperation(OperationKind::kXor, graph.internSymbol("y_xor"));
        graph.addOperand(xorr, sumTmp);
        graph.addOperand(xorr, b);
        graph.addResult(xorr, y);
        graph.bindOutputPort("y", y);

        return design;
    }

    Design buildCommitCondBatchDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId fire0 = makeLogicValue(graph, "fire0", 8);
        ValueId fire1 = makeLogicValue(graph, "fire1", 8);
        ValueId fire2 = makeLogicValue(graph, "fire2", 8);
        ValueId fire3 = makeLogicValue(graph, "fire3", 8);
        ValueId d0 = makeLogicValue(graph, "d0", 8);
        ValueId d1 = makeLogicValue(graph, "d1", 8);
        ValueId d2 = makeLogicValue(graph, "d2", 8);
        ValueId d3 = makeLogicValue(graph, "d3", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("fire0", fire0);
        graph.bindInputPort("fire1", fire1);
        graph.bindInputPort("fire2", fire2);
        graph.bindInputPort("fire3", fire3);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        graph.bindInputPort("d2", d2);
        graph.bindInputPort("d3", d3);

        ValueId mask = addConstant(graph, "const_mask_commit_batch", "mask_commit_batch", 8, "8'hFF");
        ValueId one = addConstant(graph, "const_one_commit_batch", "one_commit_batch", 8, "8'h01");
        ValueId zero = addConstant(graph, "const_zero_commit_batch", "zero_commit_batch", 8, "8'h00");

        auto addRegister = [&](std::string_view regName,
                               std::string_view readName,
                               std::string_view valueName) -> ValueId
        {
            OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                    graph.internSymbol(std::string(regName)));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", std::string("8'h00"));

            ValueId q = makeLogicValue(graph, valueName, 8);
            OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                     graph.internSymbol(std::string(readName)));
            graph.addResult(read, q);
            graph.setAttr(read, "regSymbol", std::string(regName));
            return q;
        };

        ValueId q0 = addRegister("batch_reg0", "batch_reg0_read", "batch_q0");
        ValueId q1 = addRegister("batch_reg1", "batch_reg1_read", "batch_q1");
        ValueId q2 = addRegister("batch_reg2", "batch_reg2_read", "batch_q2");
        ValueId q3 = addRegister("batch_reg3", "batch_reg3_read", "batch_q3");

        auto addFireCond = [&](std::string_view opName,
                               std::string_view valueName,
                               std::string_view outputName,
                               ValueId fire) -> ValueId
        {
            ValueId cond = makeLogicValue(graph, valueName, 1);
            OperationId eq = graph.createOperation(OperationKind::kEq, graph.internSymbol(std::string(opName)));
            graph.addOperand(eq, fire);
            graph.addOperand(eq, one);
            graph.addResult(eq, cond);
            graph.bindOutputPort(std::string(outputName), cond);
            return cond;
        };

        ValueId fireCond0 = addFireCond("batch_fire0_eq", "batch_fire_cond0", "fire_cond0", fire0);
        ValueId fireCond1 = addFireCond("batch_fire1_eq", "batch_fire_cond1", "fire_cond1", fire1);
        ValueId fireCond2 = addFireCond("batch_fire2_eq", "batch_fire_cond2", "fire_cond2", fire2);
        ValueId fireCond3 = addFireCond("batch_fire3_eq", "batch_fire_cond3", "fire_cond3", fire3);

        auto addNextData = [&](std::string_view opName,
                               std::string_view valueName,
                               std::string_view outputName,
                               ValueId data) -> ValueId
        {
            ValueId nextData = makeLogicValue(graph, valueName, 8);
            OperationId add = graph.createOperation(OperationKind::kAdd, graph.internSymbol(std::string(opName)));
            graph.addOperand(add, data);
            graph.addOperand(add, zero);
            graph.addResult(add, nextData);
            graph.bindOutputPort(std::string(outputName), nextData);
            return nextData;
        };

        ValueId nextData0 = addNextData("batch_next0_add", "batch_next_data0", "next_data0", d0);
        ValueId nextData1 = addNextData("batch_next1_add", "batch_next_data1", "next_data1", d1);
        ValueId nextData2 = addNextData("batch_next2_add", "batch_next_data2", "next_data2", d2);
        ValueId nextData3 = addNextData("batch_next3_add", "batch_next_data3", "next_data3", d3);

        auto addWrite = [&](std::string_view opName,
                            std::string_view regName,
                            ValueId fire,
                            ValueId data)
        {
            OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                      graph.internSymbol(std::string(opName)));
            graph.addOperand(write, fire);
            graph.addOperand(write, data);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "regSymbol", std::string(regName));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        };

        addWrite("batch_reg0_write", "batch_reg0", fireCond0, nextData0);
        addWrite("batch_reg1_write", "batch_reg1", fireCond1, nextData1);
        addWrite("batch_reg2_write", "batch_reg2", fireCond0, nextData2);
        addWrite("batch_reg3_write", "batch_reg3", fireCond3, nextData3);

        ValueId sum01 = makeLogicValue(graph, "sum01", 8);
        OperationId add01 = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum01_add"));
        graph.addOperand(add01, q0);
        graph.addOperand(add01, q1);
        graph.addResult(add01, sum01);

        ValueId sum23 = makeLogicValue(graph, "sum23", 8);
        OperationId add23 = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum23_add"));
        graph.addOperand(add23, q2);
        graph.addOperand(add23, q3);
        graph.addResult(add23, sum23);

        ValueId y = makeLogicValue(graph, "y", 8);
        OperationId addY = graph.createOperation(OperationKind::kAdd, graph.internSymbol("sum_y_add"));
        graph.addOperand(addY, sum01);
        graph.addOperand(addY, sum23);
        graph.addResult(addY, y);
        graph.bindOutputPort("y", y);

        return design;
    }

    Design buildCommitLocalityPartitionDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        const ValueId clk = makeLogicValue(graph, "locality_clk", 1);
        const ValueId guardLow = makeLogicValue(graph, "locality_guard_low", 1);
        const ValueId guardHigh = makeLogicValue(graph, "locality_guard_high", 1);
        const ValueId a = makeLogicValue(graph, "locality_a", 8);
        const ValueId b = makeLogicValue(graph, "locality_b", 8);
        const ValueId c = makeLogicValue(graph, "locality_c", 8);
        const ValueId d = makeLogicValue(graph, "locality_d", 8);
        graph.bindInputPort("locality_clk", clk);
        graph.bindInputPort("locality_guard_low", guardLow);
        graph.bindInputPort("locality_guard_high", guardHigh);
        graph.bindInputPort("locality_a", a);
        graph.bindInputPort("locality_b", b);
        graph.bindInputPort("locality_c", c);
        graph.bindInputPort("locality_d", d);

        const ValueId mask =
            addConstant(graph, "locality_mask_op", "locality_mask", 8, "8'hff");
        const ValueId dataLow = makeLogicValue(graph, "locality_data_low", 8);
        const OperationId dataLowOp =
            graph.createOperation(OperationKind::kXor, graph.internSymbol("locality_data_low_op"));
        graph.addOperand(dataLowOp, a);
        graph.addOperand(dataLowOp, b);
        graph.addResult(dataLowOp, dataLow);
        graph.bindOutputPort("locality_data_low", dataLow);

        const ValueId dataHigh = makeLogicValue(graph, "locality_data_high", 8);
        const OperationId dataHighOp =
            graph.createOperation(OperationKind::kAdd, graph.internSymbol("locality_data_high_op"));
        graph.addOperand(dataHighOp, c);
        graph.addOperand(dataHighOp, d);
        graph.addResult(dataHighOp, dataHigh);
        graph.bindOutputPort("locality_data_high", dataHigh);

        const auto addRegister = [&](std::string_view name)
        {
            const OperationId reg =
                graph.createOperation(OperationKind::kRegister,
                                      graph.internSymbol(std::string(name)));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", std::string("8'h00"));
        };
        addRegister("locality_reg_low");
        addRegister("locality_reg_high");

        const auto addWrite = [&](std::string_view name,
                                  std::string_view reg,
                                  ValueId guard,
                                  ValueId data)
        {
            const OperationId write =
                graph.createOperation(OperationKind::kRegisterWritePort,
                                      graph.internSymbol(std::string(name)));
            graph.addOperand(write, guard);
            graph.addOperand(write, data);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "regSymbol", std::string(reg));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        };
        addWrite("locality_write_low", "locality_reg_low", guardLow, dataLow);
        addWrite("locality_write_high", "locality_reg_high", guardHigh, dataHigh);
        return design;
    }

    Design buildInvalidRegisterWriteDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("data", data);

        ValueId one = addConstant(graph, "const_one", "one", 1, "1'b1");
        ValueId badMask = addConstant(graph, "const_bad_mask", "bad_mask", 4, "4'hF");

        OperationId reg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("bad_reg"));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h00"));

        ValueId q = makeLogicValue(graph, "q", 8);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("bad_reg_read"));
        graph.addResult(read, q);
        graph.setAttr(read, "regSymbol", std::string("bad_reg"));
        graph.bindOutputPort("q", q);

        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("bad_reg_write"));
        graph.addOperand(write, one);
        graph.addOperand(write, data);
        graph.addOperand(write, badMask);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("bad_reg"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildOneBitBitwiseDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId a = makeLogicValue(graph, "a", 1);
        ValueId b = makeLogicValue(graph, "b", 1);
        ValueId c = makeLogicValue(graph, "c", 1);
        ValueId d = makeLogicValue(graph, "d", 1);
        ValueId e = makeLogicValue(graph, "e", 1);
        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId wideA = makeLogicValue(graph, "wide_a", 8);
        ValueId wideB = makeLogicValue(graph, "wide_b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("d", d);
        graph.bindInputPort("e", e);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("wide_a", wideA);
        graph.bindInputPort("wide_b", wideB);

        const auto addBinary = [&](OperationKind kind,
                                   std::string_view opName,
                                   std::string_view valueName,
                                   ValueId lhs,
                                   ValueId rhs,
                                   int32_t width) {
            ValueId result = makeLogicValue(graph, valueName, width);
            OperationId op = graph.createOperation(kind, graph.internSymbol(opName));
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, result);
            return result;
        };
        const auto addUnary = [&](OperationKind kind,
                                  std::string_view opName,
                                  std::string_view valueName,
                                  ValueId operand) {
            ValueId result = makeLogicValue(graph, valueName, 1);
            OperationId op = graph.createOperation(kind, graph.internSymbol(opName));
            graph.addOperand(op, operand);
            graph.addResult(op, result);
            return result;
        };

        ValueId andY = addBinary(OperationKind::kAnd, "and_op", "and_y", a, b, 1);
        ValueId orY = addBinary(OperationKind::kOr, "or_op", "or_y", c, d, 1);
        ValueId xorY = addBinary(OperationKind::kXor, "xor_op", "xor_y", a, c, 1);
        ValueId xnorY = addBinary(OperationKind::kXnor, "xnor_op", "xnor_y", b, d, 1);
        ValueId notY = addUnary(OperationKind::kNot, "not_op", "not_y", e);
        ValueId chainY = addBinary(OperationKind::kAnd, "chain_and_op", "chain_y", orY, e, 1);
        ValueId logicAndY = addBinary(OperationKind::kLogicAnd, "logic_and_op", "logic_and_y", a, d, 1);
        ValueId wideAndY = addBinary(OperationKind::kAnd, "wide_and_op", "wide_and_y", wideA, wideB, 8);

        OperationId bitReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("bit_reg"));
        graph.setAttr(bitReg, "width", static_cast<int64_t>(1));
        graph.setAttr(bitReg, "isSigned", false);
        graph.setAttr(bitReg, "initValue", std::string("1'b0"));
        ValueId bitQ = makeLogicValue(graph, "bit_q", 1);
        OperationId bitRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                    graph.internSymbol("bit_read"));
        graph.addResult(bitRead, bitQ);
        graph.setAttr(bitRead, "regSymbol", std::string("bit_reg"));
        ValueId one = addConstant(graph, "one_op", "one", 1, "1'b1");
        OperationId bitWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                     graph.internSymbol("bit_write"));
        graph.addOperand(bitWrite, one);
        graph.addOperand(bitWrite, a);
        graph.addOperand(bitWrite, one);
        graph.addOperand(bitWrite, clk);
        graph.setAttr(bitWrite, "regSymbol", std::string("bit_reg"));
        graph.setAttr(bitWrite, "eventEdge", std::vector<std::string>{"posedge"});
        ValueId stateAndY = addBinary(OperationKind::kAnd, "state_and_op", "state_and_y", bitQ, b, 1);

        graph.bindOutputPort("and_y", andY);
        graph.bindOutputPort("or_y", orY);
        graph.bindOutputPort("xor_y", xorY);
        graph.bindOutputPort("xnor_y", xnorY);
        graph.bindOutputPort("not_y", notY);
        graph.bindOutputPort("chain_y", chainY);
        graph.bindOutputPort("logic_and_y", logicAndY);
        graph.bindOutputPort("wide_and_y", wideAndY);
        graph.bindOutputPort("state_and_y", stateAndY);
        return design;
    }

    Design buildGatedClockDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId auxClk = makeLogicValue(graph, "aux_clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        ValueId gateIn = makeLogicValue(graph, "gate_in", 130);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("aux_clk", auxClk);
        graph.bindInputPort("data", data);
        graph.bindInputPort("gate_in", gateIn);

        ValueId one = addConstant(graph, "const_one_gc", "one_gc", 1, "1'b1");
        ValueId mask8 = addConstant(graph, "const_mask8_gc", "mask8_gc", 8, "8'hFF");
        ValueId mask130 = addConstant(graph, "const_mask130_gc", "mask130_gc", 130, allOnesLiteral(130));
        ValueId gateMagic = addConstant(graph, "const_gate_magic", "gate_magic", 130,
                                        "130'h200000000000000000000000000000001");

        OperationId gateReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("gate_reg"));
        graph.setAttr(gateReg, "width", static_cast<int64_t>(130));
        graph.setAttr(gateReg, "isSigned", false);
        graph.setAttr(gateReg, "initValue", std::string("130'h0"));

        ValueId gateQ = makeLogicValue(graph, "gate_q", 130);
        OperationId gateRead = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("gate_read"));
        graph.addResult(gateRead, gateQ);
        graph.setAttr(gateRead, "regSymbol", std::string("gate_reg"));

        OperationId gateWrite = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("gate_write"));
        graph.addOperand(gateWrite, one);
        graph.addOperand(gateWrite, gateIn);
        graph.addOperand(gateWrite, mask130);
        graph.addOperand(gateWrite, clk);
        graph.setAttr(gateWrite, "regSymbol", std::string("gate_reg"));
        graph.setAttr(gateWrite, "eventEdge", std::vector<std::string>{"posedge"});

        ValueId gateMatch = makeLogicValue(graph, "gate_match", 1);
        OperationId gateEq = graph.createOperation(OperationKind::kEq, graph.internSymbol("gate_eq"));
        graph.addOperand(gateEq, gateQ);
        graph.addOperand(gateEq, gateMagic);
        graph.addResult(gateEq, gateMatch);
        graph.bindOutputPort("gate_match", gateMatch);

        ValueId gatedClk = makeLogicValue(graph, "gated_clk", 1);
        OperationId gatedClkOp = graph.createOperation(OperationKind::kAnd, graph.internSymbol("gated_clk_and"));
        graph.addOperand(gatedClkOp, clk);
        graph.addOperand(gatedClkOp, gateMatch);
        graph.addResult(gatedClkOp, gatedClk);

        ValueId gatedAuxClk = makeLogicValue(graph, "gated_aux_clk", 1);
        OperationId gatedAuxClkOp = graph.createOperation(OperationKind::kAnd, graph.internSymbol("gated_aux_clk_and"));
        graph.addOperand(gatedAuxClkOp, auxClk);
        graph.addOperand(gatedAuxClkOp, gateMatch);
        graph.addResult(gatedAuxClkOp, gatedAuxClk);

        OperationId gatedReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("gated_reg"));
        graph.setAttr(gatedReg, "width", static_cast<int64_t>(8));
        graph.setAttr(gatedReg, "isSigned", false);
        graph.setAttr(gatedReg, "initValue", std::string("8'h00"));

        ValueId gatedQ = makeLogicValue(graph, "gated_q", 8);
        OperationId gatedRead = graph.createOperation(OperationKind::kRegisterReadPort, graph.internSymbol("gated_read"));
        graph.addResult(gatedRead, gatedQ);
        graph.setAttr(gatedRead, "regSymbol", std::string("gated_reg"));
        graph.bindOutputPort("gated_q", gatedQ);

        OperationId gatedWrite = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("gated_write"));
        graph.addOperand(gatedWrite, one);
        graph.addOperand(gatedWrite, data);
        graph.addOperand(gatedWrite, mask8);
        graph.addOperand(gatedWrite, gatedClk);
        graph.setAttr(gatedWrite, "regSymbol", std::string("gated_reg"));
        graph.setAttr(gatedWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId gatedAuxReg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("gated_aux_reg"));
        graph.setAttr(gatedAuxReg, "width", static_cast<int64_t>(8));
        graph.setAttr(gatedAuxReg, "isSigned", false);
        graph.setAttr(gatedAuxReg, "initValue", std::string("8'h00"));

        ValueId gatedAuxQ = makeLogicValue(graph, "gated_aux_q", 8);
        OperationId gatedAuxRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                         graph.internSymbol("gated_aux_read"));
        graph.addResult(gatedAuxRead, gatedAuxQ);
        graph.setAttr(gatedAuxRead, "regSymbol", std::string("gated_aux_reg"));
        graph.bindOutputPort("gated_aux_q", gatedAuxQ);

        OperationId gatedAuxWrite =
            graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("gated_aux_write"));
        graph.addOperand(gatedAuxWrite, one);
        graph.addOperand(gatedAuxWrite, data);
        graph.addOperand(gatedAuxWrite, mask8);
        graph.addOperand(gatedAuxWrite, gatedAuxClk);
        graph.setAttr(gatedAuxWrite, "regSymbol", std::string("gated_aux_reg"));
        graph.setAttr(gatedAuxWrite, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildSystemTaskDesign(std::string_view filePath)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        ValueId cond8 = makeLogicValue(graph, "cond8", 8);
        ValueId handle = makeLogicValue(graph, "file_handle", 32);
        ValueId fileError = makeLogicValue(graph, "file_error", 32);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("data", data);
        graph.bindInputPort("cond8", cond8);
        graph.bindOutputPort("data_out", data);
        graph.bindOutputPort("file_error", fileError);

        ValueId one = addConstant(graph, "const_one_sys", "one_sys", 1, "1'b1");
        ValueId fmtInit = addConstant(graph, "const_fmt_init", "fmt_init", 0, "\"init-once\"", ValueType::String);
        ValueId fmtInitEdge = addConstant(graph, "const_fmt_init_edge", "fmt_init_edge", 0,
                                          "\"init-edge=%0d\"", ValueType::String);
        ValueId fmtDisplay = addConstant(graph, "const_fmt_display", "fmt_display", 0,
                                         "\"d=%0d h=%0h b=%b s=%s r=%0.2f\"", ValueType::String);
        ValueId fmtInfo = addConstant(graph, "const_fmt_info", "fmt_info", 0, "\"info=%0d\"", ValueType::String);
        ValueId fmtWarn = addConstant(graph, "const_fmt_warn", "fmt_warn", 0, "\"warn=%0d\"", ValueType::String);
        ValueId fmtErr = addConstant(graph, "const_fmt_err", "fmt_err", 0, "\"err=%0d\"", ValueType::String);
        ValueId fmtWrite = addConstant(graph, "const_fmt_write", "fmt_write", 0, "\"fw=%0d\"", ValueType::String);
        ValueId fmtFdisplay = addConstant(graph, "const_fmt_fdisplay", "fmt_fdisplay", 0, "\"|fd=%0d\"", ValueType::String);
        ValueId fmtFinal = addConstant(graph, "const_fmt_final", "fmt_final", 0, "\"final=%0d\"", ValueType::String);
        ValueId fmtCond = addConstant(graph, "const_fmt_cond", "fmt_cond", 0, "\"cond=%0d\"", ValueType::String);
        ValueId strArg = addConstant(graph, "const_str_arg", "str_arg", 0, "\"ok\"", ValueType::String);
        ValueId realArg = addConstant(graph, "const_real_arg", "real_arg", 0, "3.25", ValueType::Real);
        ValueId dumpfileName = addConstant(graph, "const_dumpfile_name", "dumpfile_name", 0, "\"waves.out\"",
                                           ValueType::String);
        ValueId fopenPath = addConstant(graph,
                                        "const_fopen_path",
                                        "fopen_path",
                                        0,
                                        "\"" + std::string(filePath) + "\"",
                                        ValueType::String);
        ValueId fopenMode = addConstant(graph, "const_fopen_mode", "fopen_mode", 0, "\"w\"", ValueType::String);

        OperationId fopenOp = graph.createOperation(OperationKind::kSystemFunction,
                                                    graph.internSymbol("fopen_op"));
        graph.addOperand(fopenOp, fopenPath);
        graph.addOperand(fopenOp, fopenMode);
        graph.addResult(fopenOp, handle);
        graph.setAttr(fopenOp, "name", std::string("fopen"));
        graph.setAttr(fopenOp, "hasSideEffects", true);
        graph.setAttr(fopenOp, "procKind", std::string("initial"));
        graph.setAttr(fopenOp, "hasTiming", false);

        OperationId ferrorOp = graph.createOperation(OperationKind::kSystemFunction,
                                                     graph.internSymbol("ferror_op"));
        graph.addOperand(ferrorOp, handle);
        graph.addResult(ferrorOp, fileError);
        graph.setAttr(ferrorOp, "name", std::string("ferror"));
        graph.setAttr(ferrorOp, "hasSideEffects", true);
        graph.setAttr(ferrorOp, "procKind", std::string("always_comb"));
        graph.setAttr(ferrorOp, "hasTiming", false);

        auto addTask = [&](std::string_view symbolName,
                           std::string_view taskName,
                           const std::vector<ValueId> &args,
                           std::string_view procKind,
                           bool hasTiming,
                           const std::vector<ValueId> &events = {},
                           const std::vector<std::string> &eventEdges = {},
                           ValueId callCond = ValueId{})
        {
            OperationId op = graph.createOperation(OperationKind::kSystemTask,
                                                   graph.internSymbol(std::string(symbolName)));
            graph.addOperand(op, callCond.valid() ? callCond : one);
            for (ValueId arg : args)
            {
                graph.addOperand(op, arg);
            }
            for (ValueId evt : events)
            {
                graph.addOperand(op, evt);
            }
            graph.setAttr(op, "name", std::string(taskName));
            graph.setAttr(op, "procKind", std::string(procKind));
            graph.setAttr(op, "hasTiming", hasTiming);
            if (!eventEdges.empty())
            {
                graph.setAttr(op, "eventEdge", eventEdges);
            }
        };

        addTask("task_init_once", "display", {fmtInit}, "initial", false);
        addTask("task_init_edge", "display", {fmtInitEdge, data}, "initial", true, {clk}, {"posedge"});
        addTask("task_display", "display", {fmtDisplay, data, data, data, strArg, realArg},
                "always_ff", false, {clk}, {"posedge"});
        addTask("task_info", "info", {fmtInfo, data}, "initial", false);
        addTask("task_warning", "warning", {fmtWarn, data}, "initial", false);
        addTask("task_error", "error", {fmtErr, data}, "initial", false);
        addTask("task_dumpfile", "dumpfile", {dumpfileName}, "initial", false);
        addTask("task_dumpvars", "dumpvars", {}, "initial", false);
        addTask("task_fwrite", "fwrite", {handle, fmtWrite, data}, "initial", false);
        addTask("task_conditional_display", "display", {fmtCond, data}, "always_ff", false, {clk}, {"posedge"}, cond8);
        addTask("task_final", "display", {fmtFinal, data}, "final", false);
        addTask("task_final_fdisplay", "fdisplay", {handle, fmtFdisplay, data}, "final", false);

        return design;
    }

    enum class PureEventWordFixtureMode
    {
        kHomogeneous,
        kOnceOnly,
        kMultiEvent,
        kAlternatingEvents,
    };

    Design buildPureEventWordBypassDesign(PureEventWordFixtureMode mode,
                                          std::size_t taskCount = 16u)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId auxClk = makeLogicValue(graph, "aux_clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("aux_clk", auxClk);
        graph.bindInputPort("data", data);
        graph.bindOutputPort("data_out", data);

        ValueId one = addConstant(graph, "pure_event_one_op", "pure_event_one", 1, "1'b1");
        ValueId format = addConstant(graph,
                                     "pure_event_format_op",
                                     "pure_event_format",
                                     0,
                                     "\"pure-event=%0d\"",
                                     ValueType::String);
        for (std::size_t index = 0; index < taskCount; ++index)
        {
            OperationId task = graph.createOperation(
                OperationKind::kSystemTask,
                graph.internSymbol("pure_event_task_" + std::to_string(index)));
            graph.addOperand(task, one);
            graph.addOperand(task, format);
            graph.addOperand(task, data);
            graph.addOperand(task,
                             mode == PureEventWordFixtureMode::kAlternatingEvents && (index % 2u) != 0u
                                 ? auxClk
                                 : clk);
            std::vector<std::string> edges{"posedge"};
            if (mode == PureEventWordFixtureMode::kMultiEvent)
            {
                graph.addOperand(task, auxClk);
                edges.push_back("negedge");
            }
            graph.setAttr(task, "name", std::string("display"));
            graph.setAttr(task,
                          "procKind",
                          std::string(mode == PureEventWordFixtureMode::kOnceOnly ? "initial" : "always_ff"));
            graph.setAttr(task, "hasTiming", mode == PureEventWordFixtureMode::kOnceOnly);
            graph.setAttr(task, "hasSideEffects", true);
            graph.setAttr(task, "eventEdge", std::move(edges));
        }
        return design;
    }

    Design buildPureEventWordPackEstimatedLineDriftDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId auxClk = makeLogicValue(graph, "aux_clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("aux_clk", auxClk);
        graph.bindInputPort("data", data);
        graph.bindOutputPort("data_out", data);

        OperationId clkReg = graph.createOperation(OperationKind::kRegister,
                                                   graph.internSymbol("pack_estimate_clk_reg"));
        graph.setAttr(clkReg, "width", static_cast<int64_t>(8));
        graph.setAttr(clkReg, "isSigned", false);
        graph.setAttr(clkReg, "initValue", std::string("8'h03"));
        OperationId auxReg = graph.createOperation(OperationKind::kRegister,
                                                   graph.internSymbol("pack_estimate_aux_reg"));
        graph.setAttr(auxReg, "width", static_cast<int64_t>(8));
        graph.setAttr(auxReg, "isSigned", false);
        graph.setAttr(auxReg, "initValue", std::string("8'h05"));
        ValueId one = addConstant(graph, "pack_estimate_one_op", "pack_estimate_one", 1, "1'b1");
        ValueId mask = addConstant(graph, "pack_estimate_mask_op", "pack_estimate_mask", 8, "8'hff");
        ValueId format = addConstant(graph,
                                     "pack_estimate_format_op",
                                     "pack_estimate_format",
                                     0,
                                     "\"pack-estimate=%0d\"",
                                     ValueType::String);

        constexpr std::size_t taskCount = 31u;
        for (std::size_t index = 0; index < taskCount; ++index)
        {
            const bool useAux = (index % 2u) != 0u || index + 1u == taskCount;
            const std::string suffix = std::to_string(index);
            ValueId q = makeLogicValue(graph, "pack_estimate_q_" + suffix, 8);
            OperationId read = graph.createOperation(
                OperationKind::kRegisterReadPort,
                graph.internSymbol("pack_estimate_read_" + suffix));
            graph.addResult(read, q);
            graph.setAttr(read,
                          "regSymbol",
                          std::string(useAux ? "pack_estimate_aux_reg" : "pack_estimate_clk_reg"));

            OperationId task = graph.createOperation(
                OperationKind::kSystemTask,
                graph.internSymbol("pack_estimate_task_" + suffix));
            graph.addOperand(task, one);
            graph.addOperand(task, format);
            graph.addOperand(task, q);
            graph.addOperand(task, useAux ? auxClk : clk);
            graph.setAttr(task, "name", std::string("display"));
            graph.setAttr(task, "procKind", std::string("always_ff"));
            graph.setAttr(task, "hasTiming", false);
            graph.setAttr(task, "hasSideEffects", true);
            graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
        }

        OperationId clkWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                      graph.internSymbol("pack_estimate_clk_write"));
        graph.addOperand(clkWrite, one);
        graph.addOperand(clkWrite, data);
        graph.addOperand(clkWrite, mask);
        graph.addOperand(clkWrite, clk);
        graph.setAttr(clkWrite, "regSymbol", std::string("pack_estimate_clk_reg"));
        graph.setAttr(clkWrite, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId auxWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                      graph.internSymbol("pack_estimate_aux_write"));
        graph.addOperand(auxWrite, one);
        graph.addOperand(auxWrite, data);
        graph.addOperand(auxWrite, mask);
        graph.addOperand(auxWrite, auxClk);
        graph.setAttr(auxWrite, "regSymbol", std::string("pack_estimate_aux_reg"));
        graph.setAttr(auxWrite, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildRepeatedScalarStateReadDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("data", data);

        ValueId one = addConstant(graph, "repeated_const_one", "repeated_one", 1, "1'b1");
        ValueId mask = addConstant(graph, "repeated_const_mask", "repeated_mask", 8, "8'hff");
        std::string format = "\"";
        constexpr std::size_t repeatCount = 16;
        for (std::size_t index = 0; index < repeatCount; ++index)
        {
            if (index != 0)
            {
                format += ' ';
            }
            format += "%0d";
        }
        format += "\"";
        ValueId fmt = addConstant(graph,
                                  "repeated_const_fmt",
                                  "repeated_fmt",
                                  0,
                                  std::move(format),
                                  ValueType::String);

        OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                graph.internSymbol("repeated_q"));
        graph.setAttr(reg, "width", static_cast<int64_t>(8));
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h03"));

        ValueId q = makeLogicValue(graph, "repeated_q_read", 8);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("repeated_q_read_op"));
        graph.addResult(read, q);
        graph.setAttr(read, "regSymbol", std::string("repeated_q"));
        graph.bindOutputPort("q_out", q);

        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("repeated_q_write"));
        graph.addOperand(write, one);
        graph.addOperand(write, data);
        graph.addOperand(write, mask);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("repeated_q"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId display = graph.createOperation(OperationKind::kSystemTask,
                                                    graph.internSymbol("repeated_q_display"));
        graph.addOperand(display, one);
        graph.addOperand(display, fmt);
        for (std::size_t index = 0; index < repeatCount; ++index)
        {
            graph.addOperand(display, q);
        }
        graph.addOperand(display, clk);
        graph.setAttr(display, "name", std::string("display"));
        graph.setAttr(display, "procKind", std::string("always_ff"));
        graph.setAttr(display, "hasTiming", false);
        graph.setAttr(display, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildMaterializedScalarReadLocalityDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        const ValueId a = makeLogicValue(graph, "locality_a", 8);
        const ValueId b = makeLogicValue(graph, "locality_b", 8);
        graph.bindInputPort("locality_a", a);
        graph.bindInputPort("locality_b", b);

        const ValueId repeatedSource = makeLogicValue(graph, "locality_repeated_source", 8);
        const OperationId repeatedSourceOp =
            graph.createOperation(OperationKind::kAdd, graph.internSymbol("locality_repeated_source_op"));
        graph.addOperand(repeatedSourceOp, a);
        graph.addOperand(repeatedSourceOp, b);
        graph.addResult(repeatedSourceOp, repeatedSource);
        graph.bindOutputPort("locality_repeated_source_out", repeatedSource);

        const ValueId repeatedResult = makeLogicValue(graph, "locality_repeated_result", 16);
        const OperationId repeatedResultOp =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("locality_repeated_result_op"));
        graph.addOperand(repeatedResultOp, repeatedSource);
        graph.addOperand(repeatedResultOp, repeatedSource);
        graph.addResult(repeatedResultOp, repeatedResult);
        graph.bindOutputPort("locality_repeated_result_out", repeatedResult);

        const ValueId singleSource = makeLogicValue(graph, "locality_single_source", 8);
        const OperationId singleSourceOp =
            graph.createOperation(OperationKind::kXor, graph.internSymbol("locality_single_source_op"));
        graph.addOperand(singleSourceOp, a);
        graph.addOperand(singleSourceOp, b);
        graph.addResult(singleSourceOp, singleSource);
        graph.bindOutputPort("locality_single_source_out", singleSource);

        const ValueId singleResult = makeLogicValue(graph, "locality_single_result", 8);
        const OperationId singleResultOp =
            graph.createOperation(OperationKind::kAdd, graph.internSymbol("locality_single_result_op"));
        graph.addOperand(singleResultOp, singleSource);
        graph.addOperand(singleResultOp, b);
        graph.addResult(singleResultOp, singleResult);
        graph.bindOutputPort("locality_single_result_out", singleResult);

        const ValueId wideSource = makeLogicValue(graph, "locality_wide_source", 72);
        const OperationId wideSourceOp =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("locality_wide_source_op"));
        for (std::size_t index = 0; index < 9; ++index)
        {
            graph.addOperand(wideSourceOp, a);
        }
        graph.addResult(wideSourceOp, wideSource);
        graph.bindOutputPort("locality_wide_source_out", wideSource);

        const ValueId wideResult = makeLogicValue(graph, "locality_wide_result", 144);
        const OperationId wideResultOp =
            graph.createOperation(OperationKind::kConcat, graph.internSymbol("locality_wide_result_op"));
        graph.addOperand(wideResultOp, wideSource);
        graph.addOperand(wideResultOp, wideSource);
        graph.addResult(wideResultOp, wideResult);
        graph.bindOutputPort("locality_wide_result_out", wideResult);

        return design;
    }

    Design buildDirectStateReadForwardDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId directData = makeLogicValue(graph, "direct_data", 8);
        const ValueId protectedData = makeLogicValue(graph, "protected_data", 8);
        const ValueId multiWriteA = makeLogicValue(graph, "multi_write_a", 1);
        const ValueId multiWriteB = makeLogicValue(graph, "multi_write_b", 1);
        const ValueId multiDataA = makeLogicValue(graph, "multi_data_a", 8);
        const ValueId multiDataB = makeLogicValue(graph, "multi_data_b", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("direct_data", directData);
        graph.bindInputPort("protected_data", protectedData);
        graph.bindInputPort("multi_write_a", multiWriteA);
        graph.bindInputPort("multi_write_b", multiWriteB);
        graph.bindInputPort("multi_data_a", multiDataA);
        graph.bindInputPort("multi_data_b", multiDataB);

        const ValueId one = addConstant(graph, "direct_const_one", "direct_one", 1, "1'b1");
        const ValueId increment = addConstant(graph, "direct_const_increment", "direct_increment", 8, "8'h01");
        const ValueId mask = addConstant(graph, "direct_const_mask", "direct_mask", 8, "8'hff");

        auto addRegister = [&](std::string_view symbol, std::string_view initValue) {
            const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol(std::string(symbol)));
            graph.setAttr(reg, "width", int64_t{8});
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", std::string(initValue));
        };
        auto addRead = [&](std::string_view stateSymbol, std::string_view valueSymbol) {
            const ValueId value = makeLogicValue(graph, std::string(valueSymbol), 8);
            const OperationId read = graph.createOperation(
                OperationKind::kRegisterReadPort,
                graph.internSymbol(std::string(valueSymbol) + "_op"));
            graph.addResult(read, value);
            graph.setAttr(read, "regSymbol", std::string(stateSymbol));
            return value;
        };
        auto addIncrement = [&](ValueId value, std::string_view symbol) {
            const ValueId result = makeLogicValue(graph, std::string(symbol), 8);
            const OperationId add = graph.createOperation(OperationKind::kAdd,
                                                          graph.internSymbol(std::string(symbol) + "_op"));
            graph.addOperand(add, value);
            graph.addOperand(add, increment);
            graph.addResult(add, result);
            return result;
        };
        auto addWrite = [&](std::string_view opSymbol,
                            std::string_view stateSymbol,
                            ValueId cond,
                            ValueId data) {
            const OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                            graph.internSymbol(std::string(opSymbol)));
            graph.addOperand(write, cond);
            graph.addOperand(write, data);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "regSymbol", std::string(stateSymbol));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        };

        addRegister("direct_q", "8'h03");
        const ValueId directRead = addRead("direct_q", "direct_q_read");
        graph.bindOutputPort("direct_plus_one", addIncrement(directRead, "direct_plus_one_value"));
        addWrite("direct_q_write", "direct_q", one, directData);

        addRegister("protected_q", "8'h07");
        const ValueId protectedRead = addRead("protected_q", "protected_q_read");
        graph.bindOutputPort("protected_q_out", protectedRead);
        addWrite("protected_q_write", "protected_q", one, protectedData);

        addRegister("multi_q", "8'h05");
        const ValueId multiRead = addRead("multi_q", "multi_q_read");
        graph.bindOutputPort("multi_plus_one", addIncrement(multiRead, "multi_plus_one_value"));
        addWrite("multi_q_write_a", "multi_q", multiWriteA, multiDataA);
        addWrite("multi_q_write_b", "multi_q", multiWriteB, multiDataB);

        return design;
    }

    Design buildTerminatingSystemTaskDesign(std::string_view taskName,
                                            int exitCode,
                                            std::string_view prefix)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("data", data);
        graph.bindOutputPort("data_out", data);

        ValueId one = addConstant(graph,
                                  std::string(prefix) + "_const_one",
                                  std::string(prefix) + "_one",
                                  1,
                                  "1'b1");
        ValueId code = addConstant(graph,
                                   std::string(prefix) + "_const_code",
                                   std::string(prefix) + "_code",
                                   32,
                                   "32'd" + std::to_string(exitCode));
        ValueId fmt = addConstant(graph,
                                  std::string(prefix) + "_const_fmt",
                                  std::string(prefix) + "_fmt",
                                  0,
                                  "\"" + std::string(taskName) + "=%0d\"",
                                  ValueType::String);
        ValueId fmtFinal = addConstant(graph,
                                       std::string(prefix) + "_const_final_fmt",
                                       std::string(prefix) + "_final_fmt",
                                       0,
                                       "\"final-" + std::string(taskName) + "=%0d\"",
                                       ValueType::String);

        OperationId task = graph.createOperation(OperationKind::kSystemTask,
                                                 graph.internSymbol(std::string(prefix) + "_task"));
        graph.addOperand(task, one);
        graph.addOperand(task, code);
        graph.addOperand(task, fmt);
        graph.addOperand(task, data);
        graph.setAttr(task, "name", std::string(taskName));
        graph.setAttr(task, "procKind", std::string("initial"));
        graph.setAttr(task, "hasTiming", false);

        OperationId finalTask = graph.createOperation(OperationKind::kSystemTask,
                                                      graph.internSymbol(std::string(prefix) + "_final_task"));
        graph.addOperand(finalTask, one);
        graph.addOperand(finalTask, fmtFinal);
        graph.addOperand(finalTask, data);
        graph.setAttr(finalTask, "name", std::string("display"));
        graph.setAttr(finalTask, "procKind", std::string("final"));
        graph.setAttr(finalTask, "hasTiming", false);

        return design;
    }

    Design buildDpiCallDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId a = makeLogicValue(graph, "a", 8, true);
        ValueId wide = makeLogicValue(graph, "wide", 130);
        ValueId realIn = makeRealValue(graph, "real_in");
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("a", a);
        graph.bindInputPort("wide", wide);
        graph.bindInputPort("real_in", realIn);

        ValueId one = addConstant(graph, "const_one_dpi", "one_dpi", 1, "1'b1");
        ValueId label = addConstant(graph, "const_dpi_label", "dpi_label", 0, "\"tag\"", ValueType::String);

        OperationId mixImport = graph.createOperation(OperationKind::kDpicImport,
                                                      graph.internSymbol("dpi_mix"));
        graph.setAttr(mixImport, "argsDirection",
                      std::vector<std::string>{"input", "input", "input", "input", "output", "output"});
        graph.setAttr(mixImport, "argsWidth", std::vector<int64_t>{8, 130, 64, 0, 16, 0});
        graph.setAttr(mixImport, "argsName",
                      std::vector<std::string>{"a", "wide", "r", "label", "sum", "text"});
        graph.setAttr(mixImport, "argsSigned", std::vector<bool>{true, false, false, false, true, false});
        graph.setAttr(mixImport, "argsType",
                      std::vector<std::string>{"logic", "logic", "real", "string", "logic", "string"});
        graph.setAttr(mixImport, "hasReturn", true);
        graph.setAttr(mixImport, "returnWidth", static_cast<int64_t>(32));
        graph.setAttr(mixImport, "returnSigned", true);
        graph.setAttr(mixImport, "returnType", std::string("logic"));

        ValueId retY = makeLogicValue(graph, "ret_y", 32, true);
        ValueId sumY = makeLogicValue(graph, "sum_y", 16, true);
        ValueId textY = makeStringValue(graph, "text_y");
        OperationId mixCall = graph.createOperation(OperationKind::kDpicCall,
                                                    graph.internSymbol("dpi_mix_call"));
        graph.addOperand(mixCall, a);
        graph.addOperand(mixCall, label);
        graph.addOperand(mixCall, realIn);
        graph.addOperand(mixCall, wide);
        graph.addOperand(mixCall, a);
        graph.addOperand(mixCall, clk);
        graph.addResult(mixCall, retY);
        graph.addResult(mixCall, textY);
        graph.addResult(mixCall, sumY);
        graph.setAttr(mixCall, "targetImportSymbol", std::string("dpi_mix"));
        graph.setAttr(mixCall, "inArgName", std::vector<std::string>{"label", "r", "wide", "a"});
        graph.setAttr(mixCall, "outArgName", std::vector<std::string>{"text", "sum"});
        graph.setAttr(mixCall, "hasReturn", true);
        graph.setAttr(mixCall, "eventEdge", std::vector<std::string>{"posedge"});
        graph.bindOutputPort("ret_y", retY);
        graph.bindOutputPort("sum_y", sumY);
        graph.bindOutputPort("text_y", textY);

        OperationId packImport = graph.createOperation(OperationKind::kDpicImport,
                                                       graph.internSymbol("dpi_pack"));
        graph.setAttr(packImport, "argsDirection", std::vector<std::string>{"input", "output"});
        graph.setAttr(packImport, "argsWidth", std::vector<int64_t>{8, 8});
        graph.setAttr(packImport, "argsName", std::vector<std::string>{"a", "mirror"});
        graph.setAttr(packImport, "argsSigned", std::vector<bool>{false, false});
        graph.setAttr(packImport, "argsType", std::vector<std::string>{"logic", "logic"});
        graph.setAttr(packImport, "hasReturn", false);

        ValueId mirrorY = makeLogicValue(graph, "mirror_y", 8);
        OperationId packCall = graph.createOperation(OperationKind::kDpicCall,
                                                     graph.internSymbol("dpi_pack_call"));
        graph.addOperand(packCall, one);
        graph.addOperand(packCall, a);
        graph.addOperand(packCall, clk);
        graph.addResult(packCall, mirrorY);
        graph.setAttr(packCall, "targetImportSymbol", std::string("dpi_pack"));
        graph.setAttr(packCall, "inArgName", std::vector<std::string>{"a"});
        graph.setAttr(packCall, "outArgName", std::vector<std::string>{"mirror"});
        graph.setAttr(packCall, "hasReturn", false);
        graph.setAttr(packCall, "eventEdge", std::vector<std::string>{"posedge"});
        graph.bindOutputPort("mirror_y", mirrorY);

        OperationId wideImport = graph.createOperation(OperationKind::kDpicImport,
                                                       graph.internSymbol("dpi_wide_echo"));
        graph.setAttr(wideImport, "argsDirection", std::vector<std::string>{"input", "output"});
        graph.setAttr(wideImport, "argsWidth", std::vector<int64_t>{130, 130});
        graph.setAttr(wideImport, "argsName", std::vector<std::string>{"wide", "out"});
        graph.setAttr(wideImport, "argsSigned", std::vector<bool>{false, false});
        graph.setAttr(wideImport, "argsType", std::vector<std::string>{"logic", "logic"});
        graph.setAttr(wideImport, "hasReturn", false);

        ValueId wideY = makeLogicValue(graph, "wide_y", 130);
        OperationId wideCall = graph.createOperation(OperationKind::kDpicCall,
                                                     graph.internSymbol("dpi_wide_call"));
        graph.addOperand(wideCall, wide);
        graph.addOperand(wideCall, wide);
        graph.addOperand(wideCall, clk);
        graph.addResult(wideCall, wideY);
        graph.setAttr(wideCall, "targetImportSymbol", std::string("dpi_wide_echo"));
        graph.setAttr(wideCall, "inArgName", std::vector<std::string>{"wide"});
        graph.setAttr(wideCall, "outArgName", std::vector<std::string>{"out"});
        graph.setAttr(wideCall, "hasReturn", false);
        graph.setAttr(wideCall, "eventEdge", std::vector<std::string>{"posedge"});
        graph.bindOutputPort("wide_y", wideY);

        return design;
    }

    Design buildInvalidDpiInoutDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        ValueId clk = makeLogicValue(graph, "clk", 1);
        ValueId data = makeLogicValue(graph, "data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("data", data);

        ValueId one = addConstant(graph, "const_one_invalid_dpi", "one_invalid_dpi", 1, "1'b1");

        OperationId dpiImport = graph.createOperation(OperationKind::kDpicImport,
                                                      graph.internSymbol("dpi_inout"));
        graph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"inout"});
        graph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8});
        graph.setAttr(dpiImport, "argsName", std::vector<std::string>{"x"});
        graph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
        graph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
        graph.setAttr(dpiImport, "hasReturn", false);

        OperationId dpiCall = graph.createOperation(OperationKind::kDpicCall,
                                                    graph.internSymbol("dpi_inout_call"));
        graph.addOperand(dpiCall, one);
        graph.addOperand(dpiCall, data);
        graph.addOperand(dpiCall, clk);
        graph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_inout"));
        graph.setAttr(dpiCall, "inArgName", std::vector<std::string>{});
        graph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
        graph.setAttr(dpiCall, "inoutArgName", std::vector<std::string>{"x"});
        graph.setAttr(dpiCall, "hasReturn", false);
        graph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildRegToMemIntentEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        const ValueId idx = makeLogicValue(graph, "idx", 2);
        graph.bindInputPort("idx", idx);

        auto addIntentGroup = [&](const std::string &group,
                                  const std::string &prefix,
                                  bool extraReadUser) {
            constexpr int64_t elementWidth = 8;
            constexpr int64_t elementCount = 4;
            std::vector<ValueId> readValues;
            readValues.reserve(static_cast<std::size_t>(elementCount));
            std::vector<std::string> regSymbols;
            regSymbols.reserve(static_cast<std::size_t>(elementCount));

            for (int64_t row = 0; row < elementCount; ++row)
            {
                const std::string regSymbol = prefix + "_r" + std::to_string(row);
                regSymbols.push_back(regSymbol);
                const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                              graph.internSymbol(regSymbol));
                graph.setAttr(reg, "width", elementWidth);
                graph.setAttr(reg, "isSigned", false);
                setRegToMemIntentShape(graph, reg, group, "register", elementWidth, elementCount);
                graph.setAttr(reg, "regToMem.intent.row", row);

                const ValueId readValue = makeLogicValue(graph, regSymbol + "_read", elementWidth);
                const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                               graph.internSymbol(regSymbol + "_read_op"));
                graph.addResult(read, readValue);
                graph.setAttr(read, "regSymbol", regSymbol);
                setRegToMemIntentShape(graph, read, group, "read", elementWidth, elementCount);
                graph.setAttr(read, "regToMem.intent.row", row);
                readValues.push_back(readValue);
            }

            if (extraReadUser)
            {
                const ValueId extra = makeLogicValue(graph, prefix + "_extra_read_user", elementWidth);
                const OperationId extraAssign = graph.createOperation(OperationKind::kAssign,
                                                                      graph.internSymbol(prefix + "_extra_assign"));
                graph.addOperand(extraAssign, readValues.front());
                graph.addResult(extraAssign, extra);
                graph.bindOutputPort(prefix + "_extra", extra);
            }

            const ValueId packed = makeLogicValue(graph, prefix + "_packed", elementWidth * elementCount);
            const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol(prefix + "_concat"));
            for (int64_t row = elementCount - 1; row >= 0; --row)
            {
                graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
            }
            graph.addResult(concat, packed);
            setRegToMemIntentShape(graph, concat, group, "concat", elementWidth, elementCount);
            graph.setAttr(concat, "regToMem.intent.regSymbols", regSymbols);
            graph.setAttr(concat, "regToMem.intent.operandRows", std::vector<int64_t>{3, 2, 1, 0});

            const ValueId selected = makeLogicValue(graph, prefix + "_selected", elementWidth);
            const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                            graph.internSymbol(prefix + "_slice"));
            graph.addOperand(slice, packed);
            graph.addOperand(slice, idx);
            graph.addResult(slice, selected);
            graph.setAttr(slice, "sliceWidth", elementWidth);
            graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-array"));
            setRegToMemIntentShape(graph, slice, group, "slice", elementWidth, elementCount);
            graph.bindOutputPort(prefix + "_selected", selected);
        };

        addIntentGroup("rtm_pure", "pure", false);
        addIntentGroup("rtm_extra", "extra", true);

        return design;
    }

    Design buildRegToMemTrueMultiWriteEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId addr = makeLogicValue(graph, "addr", 2);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId data = makeLogicValue(graph, "data", width);
        const ValueId addr2 = makeLogicValue(graph, "addr2", 2);
        const ValueId wen2 = makeLogicValue(graph, "wen2", 1);
        const ValueId data2 = makeLogicValue(graph, "data2", width);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("index", index);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("data", data);
        graph.bindInputPort("addr2", addr2);
        graph.bindInputPort("wen2", wen2);
        graph.bindInputPort("data2", data2);

        std::vector<std::string> regSymbols;
        std::vector<ValueId> readValues;
        regSymbols.reserve(rows);
        readValues.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string regSymbol = "multi_r" + std::to_string(row);
            regSymbols.push_back(regSymbol);
            const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol(regSymbol));
            graph.setAttr(reg, "width", int64_t{width});
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", std::string("8'h00"));

            const ValueId readValue = makeLogicValue(graph, regSymbol + "_read", width);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(regSymbol + "_read_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", regSymbol);
            readValues.push_back(readValue);
        }

        const ValueId packed = makeLogicValue(graph, "multi_packed", width * static_cast<int32_t>(rows) * 2);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("multi_concat"));
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            for (std::size_t row = rows; row-- > 0;)
            {
                graph.addOperand(concat, readValues[row]);
            }
        }
        graph.addResult(concat, packed);

        const ValueId selected = makeLogicValue(graph, "selected", width);
        const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                        graph.internSymbol("multi_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected", selected);

        const ValueId extra = makeLogicValue(graph, "extra", width);
        const OperationId extraAssign = graph.createOperation(OperationKind::kAssign,
                                                              graph.internSymbol("multi_extra_assign"));
        graph.addOperand(extraAssign, readValues.front());
        graph.addResult(extraAssign, extra);
        graph.bindOutputPort("extra", extra);

        const ValueId mask = addConstant(graph, "multi_mask_op", "multi_mask", width, "8'hff");
        auto addBinaryValue = [&](OperationKind kind,
                                  const std::string &base,
                                  ValueId lhs,
                                  ValueId rhs,
                                  int32_t valueWidth) {
            const ValueId out = makeLogicValue(graph, base + "_value", valueWidth);
            const OperationId op = graph.createOperation(kind, graph.internSymbol(base + "_op"));
            graph.addOperand(op, lhs);
            graph.addOperand(op, rhs);
            graph.addResult(op, out);
            return out;
        };
        auto addUnaryValue = [&](OperationKind kind, const std::string &base, ValueId operand) {
            const ValueId out = makeLogicValue(graph, base + "_value", 1);
            const OperationId op = graph.createOperation(kind, graph.internSymbol(base + "_op"));
            graph.addOperand(op, operand);
            graph.addResult(op, out);
            return out;
        };
        auto addMuxValue = [&](const std::string &base, ValueId cond, ValueId onTrue, ValueId onFalse) {
            const ValueId out = makeLogicValue(graph, base + "_value", width);
            const OperationId op = graph.createOperation(OperationKind::kMux, graph.internSymbol(base + "_op"));
            graph.addOperand(op, cond);
            graph.addOperand(op, onTrue);
            graph.addOperand(op, onFalse);
            graph.addResult(op, out);
            return out;
        };
        const ValueId sharedEnable = addBinaryValue(OperationKind::kLogicAnd, "multi_shared_enable", wen, wen2, 1);
        const ValueId fallback = addConstant(graph, "multi_fallback_op", "multi_fallback", width, "8'h00");
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string rowText = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "multi_first_const_" + rowText + "_op",
                                                 "multi_first_const_" + rowText,
                                                 2,
                                                 "2'd" + rowText);
            const ValueId rowConst2 = addConstant(graph,
                                                  "multi_second_const_" + rowText + "_op",
                                                  "multi_second_const_" + rowText,
                                                  2,
                                                  "2'd" + rowText);
            const ValueId firstHit = addBinaryValue(
                OperationKind::kEq, "multi_first_hit_" + rowText, addr, rowConst, 1);
            const ValueId secondHit = addBinaryValue(
                OperationKind::kEq, "multi_second_hit_" + rowText, addr2, rowConst2, 1);
            const ValueId secondGuard = addBinaryValue(
                OperationKind::kLogicAnd, "multi_second_guard_" + rowText, sharedEnable, secondHit, 1);
            const ValueId notSecondHit = addUnaryValue(
                OperationKind::kLogicNot, "multi_not_second_hit_" + rowText, secondHit);
            const ValueId firstEligible = addBinaryValue(
                OperationKind::kLogicAnd, "multi_first_eligible_" + rowText, sharedEnable, notSecondHit, 1);
            const ValueId firstGuard = addBinaryValue(
                OperationKind::kLogicAnd, "multi_first_guard_" + rowText, firstEligible, firstHit, 1);
            const ValueId updateCond = addBinaryValue(
                OperationKind::kLogicOr, "multi_update_" + rowText, firstGuard, secondGuard, 1);
            const ValueId firstNext = addMuxValue(
                "multi_first_mux_" + rowText, firstGuard, data, fallback);
            const ValueId next = addMuxValue(
                "multi_second_mux_" + rowText, secondGuard, data2, firstNext);
            const OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                            graph.internSymbol("multi_write_" + rowText));
            graph.addOperand(write, updateCond);
            graph.addOperand(write, next);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "regSymbol", regSymbols[row]);
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        }
        return design;
    }

    Design buildOrderedMemoryWriteAffineEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        constexpr std::size_t writerCount = 16;
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId readAddr = makeLogicValue(graph, "read_addr", 2);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("read_addr", readAddr);

        const OperationId memory = graph.createOperation(OperationKind::kMemory,
                                                         graph.internSymbol("ordered_mem"));
        graph.setAttr(memory, "width", int64_t{width});
        graph.setAttr(memory, "row", static_cast<int64_t>(rows));
        graph.setAttr(memory, "isSigned", false);
        graph.setAttr(memory, "initKind", std::vector<std::string>{});
        graph.setAttr(memory, "initFile", std::vector<std::string>{});
        graph.setAttr(memory, "initValue", std::vector<std::string>{});
        graph.setAttr(memory, "initStart", std::vector<int64_t>{});
        graph.setAttr(memory, "initLen", std::vector<int64_t>{});

        const ValueId readValue = makeLogicValue(graph, "ordered_read_value", width);
        const OperationId read = graph.createOperation(OperationKind::kMemoryReadPort,
                                                       graph.internSymbol("ordered_read"));
        graph.addOperand(read, readAddr);
        graph.addResult(read, readValue);
        graph.setAttr(read, "memSymbol", std::string("ordered_mem"));
        graph.bindOutputPort("selected", readValue);

        const ValueId mask = addConstant(graph,
                                         "ordered_mask_op",
                                         "ordered_mask",
                                         width,
                                         "8'hff");
        for (std::size_t writer = 0; writer < writerCount; ++writer)
        {
            const std::string writerText = std::to_string(writer);
            const ValueId enable = makeLogicValue(graph, "ordered_wen_" + writerText, 1);
            const ValueId addr = makeLogicValue(graph, "ordered_addr_" + writerText, 2);
            const ValueId data = makeLogicValue(graph, "ordered_data_" + writerText, width);
            graph.bindInputPort("ordered_wen_" + writerText, enable);
            graph.bindInputPort("ordered_addr_" + writerText, addr);
            graph.bindInputPort("ordered_data_" + writerText, data);

            const ValueId materializedEnable =
                makeLogicValue(graph, "ordered_wen_materialized_" + writerText, 1);
            const OperationId enableAssign = graph.createOperation(
                OperationKind::kAssign, graph.internSymbol("ordered_wen_assign_" + writerText));
            graph.addOperand(enableAssign, enable);
            graph.addResult(enableAssign, materializedEnable);
            const ValueId materializedAddr =
                makeLogicValue(graph, "ordered_addr_materialized_" + writerText, 2);
            const OperationId addrAssign = graph.createOperation(
                OperationKind::kAssign, graph.internSymbol("ordered_addr_assign_" + writerText));
            graph.addOperand(addrAssign, addr);
            graph.addResult(addrAssign, materializedAddr);
            const ValueId materializedData =
                makeLogicValue(graph, "ordered_data_materialized_" + writerText, width);
            const OperationId dataAssign = graph.createOperation(
                OperationKind::kAssign, graph.internSymbol("ordered_data_assign_" + writerText));
            graph.addOperand(dataAssign, data);
            graph.addResult(dataAssign, materializedData);

            const OperationId write = graph.createOperation(OperationKind::kMemoryWritePort,
                                                            graph.internSymbol("ordered_write_" + writerText));
            graph.addOperand(write, materializedEnable);
            graph.addOperand(write, materializedAddr);
            graph.addOperand(write, materializedData);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "memSymbol", std::string("ordered_mem"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            graph.setAttr(write,
                          wolvrix::lib::grh::kMemoryWritePriorityGroupAttr,
                          std::string("ordered_mem_writes"));
            graph.setAttr(write,
                          wolvrix::lib::grh::kMemoryWritePriorityAttr,
                          static_cast<int64_t>(writer));
        }
        return design;
    }

    Design buildMemoryRowReaderActivationDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 64;
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId writeAddr = makeLogicValue(graph, "write_addr", 6);
        const ValueId writeData = makeLogicValue(graph, "write_data", width);
        const ValueId readAddr = makeLogicValue(graph, "read_addr", 6);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("write_addr", writeAddr);
        graph.bindInputPort("write_data", writeData);
        graph.bindInputPort("read_addr", readAddr);

        const OperationId memory = graph.createOperation(OperationKind::kMemory,
                                                         graph.internSymbol("row_activation_mem"));
        graph.setAttr(memory, "width", int64_t{width});
        graph.setAttr(memory, "row", static_cast<int64_t>(rows));
        graph.setAttr(memory, "isSigned", false);
        graph.setAttr(memory, "initKind", std::vector<std::string>{});
        graph.setAttr(memory, "initFile", std::vector<std::string>{});
        graph.setAttr(memory, "initValue", std::vector<std::string>{});
        graph.setAttr(memory, "initStart", std::vector<int64_t>{});
        graph.setAttr(memory, "initLen", std::vector<int64_t>{});

        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string rowText = std::to_string(row);
            const ValueId addr = addConstant(graph,
                                             "row_activation_addr_" + rowText + "_op",
                                             "row_activation_addr_" + rowText,
                                             6,
                                             "6'd" + rowText);
            for (std::size_t copy = 0; copy < 2; ++copy)
            {
                const std::string copyText = std::to_string(copy);
                const ValueId value = makeLogicValue(
                    graph, "row_activation_value_" + rowText + "_" + copyText, width);
                const OperationId read = graph.createOperation(
                    OperationKind::kMemoryReadPort,
                    graph.internSymbol("row_activation_read_" + rowText + "_" + copyText));
                graph.addOperand(read, addr);
                graph.addResult(read, value);
                graph.setAttr(read, "memSymbol", std::string("row_activation_mem"));
                graph.bindOutputPort(
                    (copy == 0 ? "row_" : "row_mirror_") + rowText, value);
            }
        }

        const ValueId dynamicValue = makeLogicValue(graph, "row_activation_dynamic_value", width);
        const OperationId dynamicRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                              graph.internSymbol("row_activation_dynamic_read"));
        graph.addOperand(dynamicRead, readAddr);
        graph.addResult(dynamicRead, dynamicValue);
        graph.setAttr(dynamicRead, "memSymbol", std::string("row_activation_mem"));
        graph.bindOutputPort("dynamic_value", dynamicValue);

        const ValueId mask = addConstant(graph,
                                         "row_activation_mask_op",
                                         "row_activation_mask",
                                         width,
                                         "8'hff");
        const OperationId write = graph.createOperation(OperationKind::kMemoryWritePort,
                                                        graph.internSymbol("row_activation_write"));
        graph.addOperand(write, wen);
        graph.addOperand(write, writeAddr);
        graph.addOperand(write, writeData);
        graph.addOperand(write, mask);
        graph.addOperand(write, clk);
        graph.setAttr(write, "memSymbol", std::string("row_activation_mem"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    Design buildRegToMemDynamicInputIntentEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int64_t elementWidth = 8;
        constexpr int64_t elementCount = 4;
        const std::string group = "rtm_dyn_input";

        const ValueId idx = makeLogicValue(graph, "dyn_idx", 2);
        graph.bindInputPort("dyn_idx", idx);

        std::vector<ValueId> readValues;
        readValues.reserve(static_cast<std::size_t>(elementCount));
        std::vector<std::string> regSymbols;
        regSymbols.reserve(static_cast<std::size_t>(elementCount));
        const std::array<std::string, 4> initValues = {"8'h11", "8'h22", "8'h33", "8'h44"};
        for (int64_t row = 0; row < elementCount; ++row)
        {
            const std::string regSymbol = "dyn_r" + std::to_string(row);
            regSymbols.push_back(regSymbol);

            const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol(regSymbol));
            graph.setAttr(reg, "width", elementWidth);
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", initValues[static_cast<std::size_t>(row)]);
            setRegToMemIntentShape(graph, reg, group, "register", elementWidth, elementCount);
            graph.setAttr(reg, "regToMem.intent.row", row);

            const ValueId readValue = makeLogicValue(graph, regSymbol + "_read", elementWidth);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(regSymbol + "_read_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", regSymbol);
            setRegToMemIntentShape(graph, read, group, "read", elementWidth, elementCount);
            graph.setAttr(read, "regToMem.intent.row", row);
            readValues.push_back(readValue);
        }

        const ValueId packed = makeLogicValue(graph, "dyn_packed", elementWidth * elementCount);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("dyn_concat"));
        for (int64_t row = elementCount - 1; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        setRegToMemIntentShape(graph, concat, group, "concat", elementWidth, elementCount);
        graph.setAttr(concat, "regToMem.intent.regSymbols", regSymbols);
        graph.setAttr(concat, "regToMem.intent.operandRows", std::vector<int64_t>{3, 2, 1, 0});

        const ValueId widthConst = addConstant(graph, "dyn_element_width_const", "dyn_element_width", 4, "4'd8");
        const ValueId start = makeLogicValue(graph, "dyn_start", 5);
        const OperationId startMul = graph.createOperation(OperationKind::kMul,
                                                           graph.internSymbol("dyn_start_mul"));
        graph.addOperand(startMul, idx);
        graph.addOperand(startMul, widthConst);
        graph.addResult(startMul, start);

        const ValueId selected = makeLogicValue(graph, "dyn_selected", elementWidth);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("dyn_selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", elementWidth);
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-dynamic"));
        setRegToMemIntentShape(graph, slice, group, "slice", elementWidth, elementCount);
        graph.bindOutputPort("dyn_selected", selected);

        return design;
    }

    Design buildRegToMemOneBitDynamicIntentEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int64_t elementWidth = 1;
        constexpr int64_t elementCount = 4;

        const ValueId idx = makeLogicValue(graph, "bit_idx", 2);
        graph.bindInputPort("bit_idx", idx);

        std::vector<ValueId> readValues;
        readValues.reserve(static_cast<std::size_t>(elementCount));
        const std::array<std::string, 4> initValues = {"1'b1", "1'b0", "1'b1", "1'b0"};
        for (int64_t row = 0; row < elementCount; ++row)
        {
            const std::string regSymbol = "bit_r" + std::to_string(row);
            const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol(regSymbol));
            graph.setAttr(reg, "width", elementWidth);
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", initValues[static_cast<std::size_t>(row)]);

            const ValueId readValue = makeLogicValue(graph, regSymbol + "_read", elementWidth);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(regSymbol + "_read_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", regSymbol);
            readValues.push_back(readValue);
        }

        const ValueId packed = makeLogicValue(graph, "bit_packed", elementWidth * elementCount);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("bit_concat"));
        for (int64_t row = elementCount - 1; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId selected = makeLogicValue(graph, "bit_selected", elementWidth);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("bit_selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, idx);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", elementWidth);
        graph.bindOutputPort("bit_selected", selected);

        return design;
    }

    Design buildRegToMemMiddleSubsetIntentEmitDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int64_t elementWidth = 8;
        constexpr int64_t fullElementCount = 4;
        constexpr int64_t subsetElementCount = 2;

        const ValueId idxFull = makeLogicValue(graph, "idx_full", 2);
        const ValueId idxMid = makeLogicValue(graph, "idx_mid", 1);
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        const ValueId en = makeLogicValue(graph, "en", 1);
        const ValueId dataIn = makeLogicValue(graph, "data_in", elementWidth);
        graph.bindInputPort("idx_full", idxFull);
        graph.bindInputPort("idx_mid", idxMid);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("data_in", dataIn);

        std::vector<ValueId> fullReadValues;
        fullReadValues.reserve(static_cast<std::size_t>(fullElementCount));
        const std::array<std::string, 4> initValues = {"8'h11", "8'h22", "8'h33", "8'h44"};
        for (int64_t row = 0; row < fullElementCount; ++row)
        {
            const std::string regSymbol = "mid_r" + std::to_string(row);
            const OperationId reg = graph.createOperation(OperationKind::kRegister,
                                                          graph.internSymbol(regSymbol));
            graph.setAttr(reg, "width", elementWidth);
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", initValues[static_cast<std::size_t>(row)]);

            const ValueId readValue = makeLogicValue(graph, regSymbol + "_read_full", elementWidth);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(regSymbol + "_read_full_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", regSymbol);
            fullReadValues.push_back(readValue);
        }

        const ValueId packedFull = makeLogicValue(graph, "packed_full", elementWidth * fullElementCount);
        const OperationId concatFull = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("packed_full_concat"));
        for (int64_t row = fullElementCount - 1; row >= 0; --row)
        {
            graph.addOperand(concatFull, fullReadValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concatFull, packedFull);

        const ValueId selectedFull = makeLogicValue(graph, "selected_full", elementWidth);
        const OperationId sliceFull = graph.createOperation(OperationKind::kSliceArray,
                                                            graph.internSymbol("selected_full_slice"));
        graph.addOperand(sliceFull, packedFull);
        graph.addOperand(sliceFull, idxFull);
        graph.addResult(sliceFull, selectedFull);
        graph.setAttr(sliceFull, "sliceWidth", elementWidth);
        graph.bindOutputPort("selected_full", selectedFull);

        std::vector<ValueId> subsetReadValues;
        subsetReadValues.reserve(static_cast<std::size_t>(subsetElementCount));
        for (int64_t row = 0; row < subsetElementCount; ++row)
        {
            const int64_t storageRow = row + 1;
            const std::string regSymbol = "mid_r" + std::to_string(storageRow);
            const ValueId readValue = makeLogicValue(graph, regSymbol + "_read_mid", elementWidth);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(regSymbol + "_read_mid_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", regSymbol);
            subsetReadValues.push_back(readValue);
        }

        const ValueId packedMid = makeLogicValue(graph, "packed_mid", elementWidth * subsetElementCount);
        const OperationId concatMid = graph.createOperation(OperationKind::kConcat,
                                                            graph.internSymbol("packed_mid_concat"));
        for (int64_t row = subsetElementCount - 1; row >= 0; --row)
        {
            graph.addOperand(concatMid, subsetReadValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concatMid, packedMid);

        const ValueId selectedMid = makeLogicValue(graph, "selected_mid", elementWidth);
        const OperationId sliceMid = graph.createOperation(OperationKind::kSliceArray,
                                                           graph.internSymbol("selected_mid_slice"));
        graph.addOperand(sliceMid, packedMid);
        graph.addOperand(sliceMid, idxMid);
        graph.addResult(sliceMid, selectedMid);
        graph.setAttr(sliceMid, "sliceWidth", elementWidth);
        graph.bindOutputPort("selected_mid", selectedMid);

        const ValueId mask = addConstant(graph, "mask_all_op", "mask_all", elementWidth, "8'hff");
        const OperationId writeR2 = graph.createOperation(OperationKind::kRegisterWritePort,
                                                          graph.internSymbol("mid_r2_write"));
        graph.addOperand(writeR2, en);
        graph.addOperand(writeR2, dataIn);
        graph.addOperand(writeR2, mask);
        graph.addOperand(writeR2, clk);
        graph.setAttr(writeR2, "regSymbol", std::string("mid_r2"));
        graph.setAttr(writeR2, "eventEdge", std::vector<std::string>{"posedge"});

        return design;
    }

    bool runRegToMemIntentPass(Design &design, std::size_t minElementCount = 4)
    {
        PassManager manager;
        RegToMemOptions options;
        options.enableTrueMerge = false;
        options.minElementCount = minElementCount;
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            for (const auto &diag : diags.messages())
            {
                std::cerr << "[emit_grhsim_cpp] reg-to-mem diagnostic: "
                          << diag.message << '\n';
            }
        }
        return result.success && !diags.hasError();
    }

    bool runRegToMemTruePass(Design &design)
    {
        PassManager manager;
        RegToMemOptions options;
        options.enableTrueMerge = true;
        options.enableOrderedWrites = true;
        options.minElementCount = 4;
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            for (const auto &diag : diags.messages())
            {
                std::cerr << "[emit_grhsim_cpp] true reg-to-mem diagnostic: "
                          << diag.message << '\n';
            }
        }
        return result.success && !diags.hasError();
    }

    struct ActiveMaskGapPackFixture
    {
        Design design;
        SessionStore session;
    };

    template <typename T>
    void setActivityScheduleFixtureSlot(SessionStore &session,
                                        std::string_view name,
                                        T value,
                                        std::string_view prefix = "top")
    {
        session.insert_or_assign(
            std::string(prefix) + ".activity_schedule." + std::string(name),
            std::make_unique<SessionSlotValue<T>>(std::move(value), "active-mask-gap-pack-test"));
    }

    ActiveMaskGapPackFixture buildActiveMaskGapPackFixture()
    {
        constexpr std::size_t kActiveFlagByteCount = 180u;
        constexpr std::size_t kSupernodeCount = kActiveFlagByteCount * 8u;
        const auto byteActiveIds = [](std::initializer_list<std::size_t> bytes)
        {
            std::vector<std::size_t> activeIds;
            for (std::size_t byte : bytes)
            {
                activeIds.push_back(byte * 8u);
            }
            return activeIds;
        };
        const std::vector<std::vector<std::size_t>> groupActiveIds = {
            byteActiveIds({8u, 10u, 12u, 14u}),
            byteActiveIds({62u, 64u}),
            byteActiveIds({177u, 179u}),
            []
            {
                std::vector<std::size_t> indices;
                for (std::size_t index = 80u; index <= 110u; ++index)
                {
                    indices.push_back(index * 8u);
                }
                return indices;
            }(),
            []
            {
                std::vector<std::size_t> indices;
                for (std::size_t index = 112u; index <= 126u; index += 2u)
                {
                    indices.push_back(index * 8u);
                }
                for (std::size_t index = 130u; index <= 176u; index += 2u)
                {
                    indices.push_back(index * 8u);
                }
                return indices;
            }(),
            {42u, 43u, 160u, 161u, 176u},
        };
        const std::vector<std::size_t> sourceActiveIds = {7u, 15u, 23u, 31u, 39u, 41u};

        ActivityScheduleTopoOrder topoOrder(kSupernodeCount);
        for (std::size_t index = 0; index < topoOrder.size(); ++index)
        {
            topoOrder[index] = static_cast<uint32_t>(index);
        }
        for (std::size_t group = 0; group < groupActiveIds.size(); ++group)
        {
            const auto sourceIt = std::find(topoOrder.begin(),
                                            topoOrder.end(),
                                            static_cast<uint32_t>(group));
            std::swap(topoOrder[sourceActiveIds[group]], *sourceIt);
        }

        std::vector<int> targetGroupBySupernode(kSupernodeCount, -1);
        for (std::size_t group = 0; group < groupActiveIds.size(); ++group)
        {
            for (std::size_t activeId : groupActiveIds[group])
            {
                targetGroupBySupernode[topoOrder[activeId]] = static_cast<int>(group);
            }
        }

        ActiveMaskGapPackFixture fixture;
        Graph &graph = fixture.design.createGraph("top");
        fixture.design.markAsTop(graph.symbol());
        const ValueId trigger = makeLogicValue(graph, "gap_pack_trigger", 1);
        graph.bindInputPort("trigger", trigger);

        std::vector<ValueId> sourceValues;
        std::vector<OperationId> operations(kSupernodeCount);
        sourceValues.reserve(groupActiveIds.size());
        for (std::size_t group = 0; group < groupActiveIds.size(); ++group)
        {
            const std::string suffix = std::to_string(group);
            const ValueId value = makeLogicValue(graph, "gap_pack_source_value_" + suffix, 1);
            const OperationId op = graph.createOperation(
                OperationKind::kAssign,
                graph.internSymbol("gap_pack_source_op_" + suffix));
            graph.addOperand(op, trigger);
            graph.addResult(op, value);
            sourceValues.push_back(value);
            operations[group] = op;
        }
        for (std::size_t supernode = groupActiveIds.size(); supernode < kSupernodeCount; ++supernode)
        {
            const std::string suffix = std::to_string(supernode);
            const int targetGroup = targetGroupBySupernode[supernode];
            const ValueId operand = targetGroup < 0
                                        ? trigger
                                        : sourceValues[static_cast<std::size_t>(targetGroup)];
            const ValueId value = makeLogicValue(graph, "gap_pack_value_" + suffix, 1);
            const OperationId op = graph.createOperation(
                OperationKind::kAssign,
                graph.internSymbol("gap_pack_op_" + suffix));
            graph.addOperand(op, operand);
            graph.addResult(op, value);
            operations[supernode] = op;
        }

        ActivityScheduleSupernodeToOps supernodeToOps(kSupernodeCount);
        for (std::size_t supernode = 0; supernode < kSupernodeCount; ++supernode)
        {
            supernodeToOps[supernode].push_back(operations[supernode]);
        }
        ActivityScheduleValueFanout valueFanout(graph.values().size());
        for (std::size_t group = 0; group < groupActiveIds.size(); ++group)
        {
            auto &fanout = valueFanout[sourceValues[group].index - 1u];
            for (std::size_t activeId : groupActiveIds[group])
            {
                fanout.push_back(topoOrder[activeId]);
            }
        }
        setActivityScheduleFixtureSlot(fixture.session,
                                       "supernode_to_ops",
                                       std::move(supernodeToOps));
        setActivityScheduleFixtureSlot(fixture.session,
                                       "value_fanout",
                                       std::move(valueFanout));
        setActivityScheduleFixtureSlot(fixture.session,
                                       "topo_order",
                                       std::move(topoOrder));
        setActivityScheduleFixtureSlot(fixture.session,
                                       "state_read_supernodes",
                                       ActivityScheduleStateReadSupernodes{});
        return fixture;
    }

    ActiveMaskGapPackFixture buildDeferredActivationForwardFixture()
    {
        constexpr std::size_t kSupernodeCount = 128u;
        ActiveMaskGapPackFixture fixture;
        Graph &graph = fixture.design.createGraph("top");
        fixture.design.markAsTop(graph.symbol());
        const ValueId trigger = makeLogicValue(graph, "forward_trigger", 1);
        graph.bindInputPort("trigger", trigger);
        const ValueId eventFormat = addConstant(
            graph,
            "forward_event_format_op",
            "forward_event_format",
            0,
            "\"event=%0d\"",
            ValueType::String);

        ActivityScheduleSupernodeToOps supernodeToOps(kSupernodeCount);
        struct FanoutSpec
        {
            ValueId value;
            std::vector<uint32_t> targets;
        };
        std::vector<FanoutSpec> fanouts;
        std::map<uint32_t, std::vector<ValueId>> targetOperands;
        const auto addSourceGroup = [&](uint32_t source,
                                        std::initializer_list<uint32_t> targets,
                                        std::string_view name)
        {
            std::vector<ValueId> values;
            for (std::size_t valueIndex = 0; valueIndex < 2u; ++valueIndex)
            {
                const std::string suffix = std::string(name) + "_" + std::to_string(valueIndex);
                const ValueId value = makeLogicValue(graph, suffix + "_value", 1);
                const OperationId op = graph.createOperation(
                    OperationKind::kAssign,
                    graph.internSymbol(suffix + "_op"));
                graph.addOperand(op, trigger);
                graph.addResult(op, value);
                supernodeToOps[source].push_back(op);
                values.push_back(value);
                FanoutSpec spec{.value = value};
                spec.targets.assign(targets.begin(), targets.end());
                fanouts.push_back(std::move(spec));
            }
            for (uint32_t target : targets)
            {
                targetOperands[target].insert(
                    targetOperands[target].end(), values.begin(), values.end());
            }
        };

        const ValueId eventClock = makeLogicValue(graph, "forward_event_clock", 1);
        const OperationId eventClockOp = graph.createOperation(
            OperationKind::kAssign,
            graph.internSymbol("forward_event_clock_op"));
        graph.addOperand(eventClockOp, trigger);
        graph.addResult(eventClockOp, eventClock);
        supernodeToOps[13u].push_back(eventClockOp);
        fanouts.push_back(FanoutSpec{.value = eventClock, .targets = {12u}});

        // Exact same-byte candidate.
        addSourceGroup(0u, {1u}, "same_byte");
        // Nonexclusive control groups exercise 8/4/2-byte chunk accounting but are rejected.
        addSourceGroup(2u, {8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u}, "chunk8");
        addSourceGroup(3u, {80u, 88u, 96u, 104u}, "chunk4");
        addSourceGroup(4u, {112u, 120u}, "chunk2");
        // Cross-byte exact candidate.
        addSourceGroup(5u, {72u}, "global_byte");
        // Static special-head rejections.
        addSourceGroup(6u, {7u}, "state_head");
        addSourceGroup(9u, {10u}, "input_head");
        addSourceGroup(11u, {12u}, "event_head");

        for (std::size_t supernode = 0; supernode < kSupernodeCount; ++supernode)
        {
            if (!supernodeToOps[supernode].empty())
            {
                continue;
            }
            const auto targetIt = targetOperands.find(static_cast<uint32_t>(supernode));
            const ValueId result = makeLogicValue(
                graph,
                "forward_result_" + std::to_string(supernode),
                1);
            OperationId op;
            if (targetIt != targetOperands.end())
            {
                op = graph.createOperation(
                    OperationKind::kAnd,
                    graph.internSymbol("forward_target_" + std::to_string(supernode)));
                graph.addOperand(op, targetIt->second[0]);
                graph.addOperand(op, targetIt->second[1]);
            }
            else
            {
                op = graph.createOperation(
                    OperationKind::kAssign,
                    graph.internSymbol("forward_filler_" + std::to_string(supernode)));
                graph.addOperand(op, trigger);
            }
            graph.addResult(op, result);
            supernodeToOps[supernode].push_back(op);
            if (supernode == 10u)
            {
                const ValueId inputResult = makeLogicValue(graph, "forward_input_head_result", 1);
                const OperationId inputOp = graph.createOperation(
                    OperationKind::kAssign,
                    graph.internSymbol("forward_input_head_op"));
                graph.addOperand(inputOp, trigger);
                graph.addResult(inputOp, inputResult);
                supernodeToOps[supernode].push_back(inputOp);
            }
            else if (supernode == 12u)
            {
                const ValueId eventUseResult = makeLogicValue(
                    graph,
                    "forward_event_use_result",
                    1);
                const OperationId eventUse = graph.createOperation(
                    OperationKind::kAssign,
                    graph.internSymbol("forward_event_use_op"));
                graph.addOperand(eventUse, eventClock);
                graph.addResult(eventUse, eventUseResult);
                supernodeToOps[supernode].push_back(eventUse);
                const OperationId task = graph.createOperation(
                    OperationKind::kSystemTask,
                    graph.internSymbol("forward_event_head_task"));
                graph.addOperand(task, result);
                graph.addOperand(task, eventFormat);
                graph.addOperand(task, result);
                graph.addOperand(task, eventClock);
                graph.setAttr(task, "name", std::string("display"));
                graph.setAttr(task, "procKind", std::string("always_ff"));
                graph.setAttr(task, "hasTiming", false);
                graph.setAttr(task, "hasSideEffects", true);
                graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
                supernodeToOps[supernode].push_back(task);
            }
        }

        ActivityScheduleValueFanout valueFanout(graph.values().size());
        for (const auto &spec : fanouts)
        {
            valueFanout[spec.value.index - 1u] = spec.targets;
        }
        ActivityScheduleTopoOrder topoOrder(kSupernodeCount);
        for (std::size_t index = 0; index < topoOrder.size(); ++index)
        {
            topoOrder[index] = static_cast<uint32_t>(index);
        }
        ActivityScheduleStateReadSupernodes stateReads;
        stateReads["fixture_state"] = {7u};
        setActivityScheduleFixtureSlot(
            fixture.session, "supernode_to_ops", std::move(supernodeToOps));
        setActivityScheduleFixtureSlot(
            fixture.session, "value_fanout", std::move(valueFanout));
        setActivityScheduleFixtureSlot(
            fixture.session, "topo_order", std::move(topoOrder));
        setActivityScheduleFixtureSlot(
            fixture.session, "state_read_supernodes", std::move(stateReads));
        return fixture;
    }

    ActiveMaskGapPackFixture buildSameBatchActivationCohortFixture(
        std::string_view graphSymbol = "top")
    {
        constexpr std::size_t kSupernodeCount = 16u;
        ActiveMaskGapPackFixture fixture;
        Graph &graph = fixture.design.createGraph(std::string(graphSymbol));
        fixture.design.markAsTop(graph.symbol());

        const ValueId input = makeLogicValue(graph, "in", 8);
        graph.bindInputPort("in", input);
        const ValueId shared = makeLogicValue(graph, "shared", 8);
        ActivityScheduleSupernodeToOps supernodeToOps(kSupernodeCount);

        const OperationId producer = graph.createOperation(
            OperationKind::kAssign,
            graph.internSymbol("cohort_producer"));
        graph.addOperand(producer, input);
        graph.addResult(producer, shared);
        supernodeToOps[0].push_back(producer);

        for (std::size_t supernode = 1; supernode < kSupernodeCount; ++supernode)
        {
            const ValueId result = makeLogicValue(
                graph,
                "cohort_result_" + std::to_string(supernode),
                8);
            if (supernode >= 8u && supernode <= 12u)
            {
                const OperationId op = graph.createOperation(
                    OperationKind::kAssign,
                    graph.internSymbol("cohort_member_" + std::to_string(supernode)));
                graph.addOperand(op, shared);
                graph.addResult(op, result);
                supernodeToOps[supernode].push_back(op);
            }
            else
            {
                const OperationId op = graph.createOperation(
                    OperationKind::kConstant,
                    graph.internSymbol("cohort_padding_" + std::to_string(supernode)));
                graph.addResult(op, result);
                graph.setAttr(op, "constValue", std::string("8'd") +
                                                    std::to_string(supernode));
                supernodeToOps[supernode].push_back(op);
            }
        }

        ActivityScheduleValueFanout valueFanout(graph.values().size());
        valueFanout[shared.index - 1u] = {8u, 9u, 10u, 11u, 12u};
        ActivityScheduleTopoOrder topoOrder(kSupernodeCount);
        std::iota(topoOrder.begin(), topoOrder.end(), 0u);
        ActivityScheduleStateReadSupernodes stateReads;
        ActivityScheduleDag dag(kSupernodeCount);
        dag[0] = {8u, 9u, 10u, 11u, 12u};
        setActivityScheduleFixtureSlot(
            fixture.session, "supernode_to_ops", std::move(supernodeToOps), graphSymbol);
        setActivityScheduleFixtureSlot(
            fixture.session, "value_fanout", std::move(valueFanout), graphSymbol);
        setActivityScheduleFixtureSlot(
            fixture.session, "topo_order", std::move(topoOrder), graphSymbol);
        setActivityScheduleFixtureSlot(
            fixture.session, "state_read_supernodes", std::move(stateReads), graphSymbol);
        setActivityScheduleFixtureSlot(
            fixture.session, "dag", std::move(dag), graphSymbol);
        return fixture;
    }

    class StderrCapture
    {
    public:
        StderrCapture()
        {
            std::fflush(stderr);
            file_ = std::tmpfile();
            savedFd_ = ::dup(STDERR_FILENO);
            if (file_ != nullptr && savedFd_ >= 0 &&
                ::dup2(::fileno(file_), STDERR_FILENO) >= 0)
            {
                active_ = true;
            }
            else if (savedFd_ >= 0)
            {
                ::close(savedFd_);
                savedFd_ = -1;
            }
        }

        ~StderrCapture()
        {
            restore();
            if (file_ != nullptr)
            {
                std::fclose(file_);
            }
        }

        bool valid() const noexcept { return active_; }

        std::string finish()
        {
            if (!active_)
            {
                return {};
            }
            std::fflush(stderr);
            ::dup2(savedFd_, STDERR_FILENO);
            ::close(savedFd_);
            savedFd_ = -1;
            active_ = false;
            std::rewind(file_);
            std::string output;
            std::array<char, 4096> buffer{};
            while (const std::size_t count = std::fread(buffer.data(), 1u, buffer.size(), file_))
            {
                output.append(buffer.data(), count);
            }
            return output;
        }

    private:
        void restore()
        {
            if (!active_)
            {
                return;
            }
            std::fflush(stderr);
            ::dup2(savedFd_, STDERR_FILENO);
            ::close(savedFd_);
            savedFd_ = -1;
            active_ = false;
        }

        FILE *file_ = nullptr;
        int savedFd_ = -1;
        bool active_ = false;
    };

    struct ActiveMaskGapPackEmitRun
    {
        bool success = false;
        bool diagnosticError = false;
        std::string diagnostics;
        std::string stderrText;
        std::map<std::string, std::string> artifacts;
    };

    ActiveMaskGapPackEmitRun runActiveMaskGapPackEmit(
        const Design &design,
        SessionStore &session,
        const std::filesystem::path &outDir,
        std::optional<std::string_view> policy,
        std::size_t parallelism,
        bool emitRuntimeProfile = false)
    {
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);
        EmitOptions options;
        options.outputDir = outDir.string();
        options.session = &session;
        options.sessionPathPrefix = std::string("top");
        options.attributes["sched_batch_max_ops"] = "8";
        options.attributes["sched_batch_max_estimated_lines"] = "96";
        options.attributes["sched_batches_per_cpp"] = "4";
        options.attributes["emit_parallelism"] = std::to_string(parallelism);
        if (emitRuntimeProfile)
        {
            options.attributes["emit_runtime_profile"] = "1";
        }
        if (policy)
        {
            options.attributes["active_mask_gap_pack_policy"] = std::string(*policy);
        }

        EmitDiagnostics diagnostics;
        EmitGrhSimCpp emitter(&diagnostics);
        StderrCapture capture;
        ActiveMaskGapPackEmitRun run;
        if (!capture.valid())
        {
            run.diagnostics = "failed to capture stderr";
            return run;
        }
        const EmitResult result = emitter.emit(design, options);
        run.stderrText = capture.finish();
        run.success = result.success;
        run.diagnosticError = diagnostics.hasError();
        for (const auto &message : diagnostics.messages())
        {
            if (!run.diagnostics.empty())
            {
                run.diagnostics.push_back('\n');
            }
            run.diagnostics += message.message;
        }
        for (const std::string &artifact : result.artifacts)
        {
            const std::filesystem::path path(artifact);
            run.artifacts.insert_or_assign(path.filename().string(), readFile(path));
        }
        return run;
    }

    ActiveMaskGapPackEmitRun runDeferredActivationForwardEmit(
        const Design &design,
        SessionStore &session,
        const std::filesystem::path &outDir,
        std::optional<std::string_view> policy,
        const std::filesystem::path &profilePath,
        std::optional<std::string_view> activeMaskPolicy = std::nullopt)
    {
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);
        EmitOptions options;
        options.outputDir = outDir.string();
        options.session = &session;
        options.sessionPathPrefix = std::string("top");
        options.attributes["sched_batch_max_ops"] = "16";
        options.attributes["sched_batch_max_estimated_lines"] = "256";
        options.attributes["sched_batches_per_cpp"] = "2";
        options.attributes["emit_parallelism"] = "2";
        if (policy)
        {
            options.attributes["deferred_activation_forward_policy"] = std::string(*policy);
        }
        if (activeMaskPolicy)
        {
            options.attributes["active_mask_gap_pack_policy"] = std::string(*activeMaskPolicy);
        }
        if (!profilePath.empty())
        {
            options.attributes["deferred_activation_forward_profile_path"] =
                profilePath.string();
        }

        EmitDiagnostics diagnostics;
        EmitGrhSimCpp emitter(&diagnostics);
        StderrCapture capture;
        ActiveMaskGapPackEmitRun run;
        if (!capture.valid())
        {
            run.diagnostics = "failed to capture stderr";
            return run;
        }
        const EmitResult result = emitter.emit(design, options);
        run.stderrText = capture.finish();
        run.success = result.success;
        run.diagnosticError = diagnostics.hasError();
        for (const auto &message : diagnostics.messages())
        {
            if (!run.diagnostics.empty())
            {
                run.diagnostics.push_back('\n');
            }
            run.diagnostics += message.message;
        }
        for (const std::string &artifact : result.artifacts)
        {
            const std::filesystem::path path(artifact);
            run.artifacts.insert_or_assign(path.filename().string(), readFile(path));
        }
        return run;
    }

    ActiveMaskGapPackEmitRun runSameBatchActivationCohortEmit(
        const Design &design,
        SessionStore &session,
        const std::filesystem::path &outDir,
        std::optional<std::string_view> policy,
        const std::filesystem::path &profilePath,
        std::string_view sessionPrefix = "top")
    {
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);
        EmitOptions options;
        options.outputDir = outDir.string();
        options.session = &session;
        options.sessionPathPrefix = std::string(sessionPrefix);
        options.attributes["sched_batch_max_ops"] = "8";
        options.attributes["sched_batch_max_estimated_lines"] = "100000";
        options.attributes["sched_batches_per_cpp"] = "1";
        options.attributes["emit_parallelism"] = "1";
        if (policy)
        {
            options.attributes["same_batch_activation_cohort_policy"] =
                std::string(*policy);
        }
        if (!profilePath.empty())
        {
            options.attributes["same_batch_activation_cohort_profile_path"] =
                profilePath.string();
        }

        EmitDiagnostics diagnostics;
        EmitGrhSimCpp emitter(&diagnostics);
        StderrCapture capture;
        ActiveMaskGapPackEmitRun run;
        if (!capture.valid())
        {
            run.diagnostics = "failed to capture stderr";
            return run;
        }
        const EmitResult result = emitter.emit(design, options);
        run.stderrText = capture.finish();
        run.success = result.success;
        run.diagnosticError = diagnostics.hasError();
        for (const auto &message : diagnostics.messages())
        {
            if (!run.diagnostics.empty())
            {
                run.diagnostics.push_back('\n');
            }
            run.diagnostics += message.message;
        }
        for (const std::string &artifact : result.artifacts)
        {
            const std::filesystem::path path(artifact);
            run.artifacts.insert_or_assign(path.filename().string(), readFile(path));
        }
        return run;
    }

    std::vector<std::string> sessionKeys(const SessionStore &session)
    {
        std::vector<std::string> keys;
        keys.reserve(session.size());
        for (const auto &[key, _] : session)
        {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    std::string_view probeLogLine(std::string_view log, std::string_view prefix)
    {
        const std::size_t begin = log.find(prefix);
        if (begin == std::string_view::npos)
        {
            return {};
        }
        const std::size_t end = log.find('\n', begin);
        return log.substr(begin, end == std::string_view::npos ? log.size() - begin : end - begin);
    }

    std::string_view probeStatsLine(std::string_view log, std::string_view kind)
    {
        return probeLogLine(
            log,
            "[GRHSIM_ACTIVE_MASK_GAP_PACK] kind=" + std::string(kind) + " ");
    }

    std::optional<std::size_t> probeStatsUnsigned(std::string_view line,
                                                  std::string_view field)
    {
        const std::string prefix = std::string(field) + "=";
        const std::size_t begin = line.find(prefix);
        if (begin == std::string_view::npos)
        {
            return std::nullopt;
        }
        const std::size_t valueBegin = begin + prefix.size();
        const std::size_t valueEnd = line.find(' ', valueBegin);
        try
        {
            return static_cast<std::size_t>(std::stoull(
                std::string(line.substr(valueBegin, valueEnd - valueBegin))));
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    int runDeferredActivationForwardFocusedTests()
    {
        ActiveMaskGapPackFixture fixture = buildDeferredActivationForwardFixture();
        const std::filesystem::path baseDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
            "grhsim_cpp_deferred_activation_forward";
        std::filesystem::remove_all(baseDir);
        std::filesystem::create_directories(baseDir);
        const std::filesystem::path profilePath = baseDir / "profile.tsv";
        {
            std::ofstream profile(profilePath);
            profile << "supernode_id\tphase\tf\n";
            for (std::size_t supernode = 0; supernode < 128u; ++supernode)
            {
                profile << supernode << "\tcompute\t100\n";
            }
        }
        const std::filesystem::path invalidProfilePath = baseDir / "invalid_profile.tsv";
        {
            std::ofstream profile(invalidProfilePath);
            profile << "supernode_id\tphase\tf\n0\tcompute\t100\n";
        }

        ::unsetenv("WOLVRIX_GRHSIM_DEFERRED_ACTIVATION_FORWARD_POLICY");
        ::unsetenv("WOLVRIX_GRHSIM_DEFERRED_ACTIVATION_FORWARD_PROFILE_PATH");
        const std::vector<std::string> sessionKeysBefore = sessionKeys(fixture.session);
        const ActiveMaskGapPackEmitRun defaultRun = runDeferredActivationForwardEmit(
            fixture.design, fixture.session, baseDir / "default", std::nullopt, {});
        const ActiveMaskGapPackEmitRun offRun = runDeferredActivationForwardEmit(
            fixture.design, fixture.session, baseDir / "off", "off", profilePath);
        const ActiveMaskGapPackEmitRun probeRun = runDeferredActivationForwardEmit(
            fixture.design, fixture.session, baseDir / "probe", "probe", profilePath);
        const ActiveMaskGapPackEmitRun probeRepeatRun = runDeferredActivationForwardEmit(
            fixture.design, fixture.session, baseDir / "probe_repeat", "probe", profilePath);
        const ActiveMaskGapPackEmitRun invalidProfileRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "invalid_profile",
            "probe",
            invalidProfilePath);
        const ActiveMaskGapPackEmitRun emptyProfileRun = runDeferredActivationForwardEmit(
            fixture.design, fixture.session, baseDir / "empty_profile", "probe", {});
        const ActiveMaskGapPackEmitRun cofireWrongPairSetRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "cofire_wrong_pair_set",
            "cofire-probe",
            profilePath);
        const ActiveMaskGapPackEmitRun cofireInvalidProfileRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "cofire_invalid_profile",
            "cofire-probe",
            invalidProfilePath);
        const ActiveMaskGapPackEmitRun cofireStrictWrongPairSetRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "cofire_strict_wrong_pair_set",
            "cofire-strict",
            profilePath);
        const ActiveMaskGapPackEmitRun cofireStrictInvalidProfileRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "cofire_strict_invalid_profile",
            "cofire-strict",
            invalidProfilePath);
        const ActiveMaskGapPackEmitRun cofireStrictActiveMaskRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "cofire_strict_active_mask",
            "cofire-strict",
            profilePath,
            "probe");
        const ActiveMaskGapPackEmitRun cofireStrictExtendedWrongPairSetRun =
            runDeferredActivationForwardEmit(
                fixture.design,
                fixture.session,
                baseDir / "cofire_strict_extended_wrong_pair_set",
                "cofire-strict-extended",
                profilePath);
        const ActiveMaskGapPackEmitRun cofireStrictExtendedInvalidProfileRun =
            runDeferredActivationForwardEmit(
                fixture.design,
                fixture.session,
                baseDir / "cofire_strict_extended_invalid_profile",
                "cofire-strict-extended",
                invalidProfilePath);
        const ActiveMaskGapPackEmitRun cofireStrictExtendedActiveMaskRun =
            runDeferredActivationForwardEmit(
                fixture.design,
                fixture.session,
                baseDir / "cofire_strict_extended_active_mask",
                "cofire-strict-extended",
                profilePath,
                "probe");
        if (!defaultRun.success || defaultRun.diagnosticError ||
            !offRun.success || offRun.diagnosticError ||
            !probeRun.success || probeRun.diagnosticError ||
            !probeRepeatRun.success || probeRepeatRun.diagnosticError ||
            !invalidProfileRun.success || invalidProfileRun.diagnosticError ||
            !emptyProfileRun.success || emptyProfileRun.diagnosticError)
        {
            return fail("deferred-activation forward fixture emission failed");
        }
        if (defaultRun.artifacts != offRun.artifacts ||
            defaultRun.artifacts != probeRun.artifacts ||
            defaultRun.artifacts != probeRepeatRun.artifacts ||
            defaultRun.artifacts != invalidProfileRun.artifacts ||
            defaultRun.artifacts != emptyProfileRun.artifacts)
        {
            return fail("deferred-activation forward off/probe must preserve every generated artifact byte");
        }
        if (probeRun.stderrText != probeRepeatRun.stderrText)
        {
            return fail("deferred-activation forward probe selection and reporting must be deterministic");
        }
        if (sessionKeys(fixture.session) != sessionKeysBefore)
        {
            return fail("deferred-activation forward probe must not change the session key set");
        }
        constexpr std::string_view marker = "[GRHSIM_DEFERRED_ACTIVATION_FORWARD]";
        if (defaultRun.stderrText.find(marker) != std::string::npos ||
            offRun.stderrText.find(marker) != std::string::npos)
        {
            return fail("deferred-activation forward default/off should not emit probe logs");
        }
        const std::string_view summary = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] policy=probe ");
        const std::string_view rejects = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] rejects ");
        const std::string_view control = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] accounting=control ");
        const std::string_view candidate = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] accounting=candidate ");
        const std::string_view net = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] net ");
        if (summary.empty() || rejects.empty() || control.empty() || candidate.empty() || net.empty() ||
            summary.find("profile_valid=true") == std::string_view::npos ||
            summary.find("accounted=") == std::string_view::npos ||
            summary.find("static_positive=") == std::string_view::npos ||
            summary.find("selected_fire_weighted_work_proxy_lower=") == std::string_view::npos ||
            summary.find("selected_fire_weighted_work_proxy_upper=") == std::string_view::npos ||
            summary.find("selected_positive_work_proxy_candidates=") == std::string_view::npos ||
            summary.find("selected_positive_fire_weighted_work_proxy_lower=") == std::string_view::npos ||
            summary.find("selected_positive_fire_weighted_work_proxy_upper=") == std::string_view::npos ||
            !probeStatsUnsigned(summary, "selected").value_or(0) ||
            !probeStatsUnsigned(rejects, "rejected_nonexclusive").value_or(0) ||
            !probeStatsUnsigned(rejects, "rejected_state_head").value_or(0) ||
            !probeStatsUnsigned(rejects, "rejected_input_head").value_or(0) ||
            !probeStatsUnsigned(control, "chunk8").value_or(0) ||
            !probeStatsUnsigned(control, "chunk4").value_or(0) ||
            !probeStatsUnsigned(control, "chunk2").value_or(0) ||
            net.find("direct_to_aggregate=") == std::string_view::npos ||
            net.find("aggregate_to_direct=") == std::string_view::npos ||
            net.find("branchless_to_guarded=") == std::string_view::npos ||
            net.find("guarded_to_branchless=") == std::string_view::npos ||
            net.find("branchless_changed_sources=") == std::string_view::npos ||
            net.find("guarded_changed_sources=") == std::string_view::npos ||
            net.find("table_changed_sources=") == std::string_view::npos)
        {
            return fail("deferred-activation forward absolute accounting or reject funnel is incomplete");
        }
        if (!probeStatsUnsigned(rejects, "rejected_event_head").value_or(0))
        {
            return fail(
                "deferred-activation forward event-head rejection is missing: " +
                std::string(rejects));
        }
        const std::string_view sameByteRow = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] candidate rank=0 ");
        if (sameByteRow.find("source=0 target=1") == std::string_view::npos ||
            sameByteRow.find("report_kind=selected") == std::string_view::npos ||
            sameByteRow.find("source_cpp=grhsim_top_sched_group_0.cpp") == std::string_view::npos ||
            sameByteRow.find("target_cpp=grhsim_top_sched_group_0.cpp") == std::string_view::npos ||
            sameByteRow.find("fire_weighted_work_proxy_lower=") == std::string_view::npos ||
            sameByteRow.find("fire_weighted_work_proxy_upper=") == std::string_view::npos ||
            sameByteRow.find("forward_local_rmw=1") == std::string_view::npos ||
            sameByteRow.find("physical_supernode_tests_saved=0") == std::string_view::npos)
        {
            return fail("deferred-activation forward same-byte candidate accounting is missing");
        }
        const std::size_t expectedReportedRows =
            probeStatsUnsigned(summary, "reported_selected").value_or(0) +
            probeStatsUnsigned(summary, "reported_near_selected").value_or(0);
        if (expectedReportedRows == 0 ||
            countSubstring(
                probeRun.stderrText,
                "[GRHSIM_DEFERRED_ACTIVATION_FORWARD] candidate ") != expectedReportedRows)
        {
            return fail("deferred-activation forward must report every selected and capped near-selected row");
        }
        if (invalidProfileRun.stderrText.find("profile_valid=false") == std::string::npos ||
            invalidProfileRun.stderrText.find("selected=0") == std::string::npos ||
            emptyProfileRun.stderrText.find("profile path is empty") == std::string::npos ||
            emptyProfileRun.stderrText.find("selected=0") == std::string::npos)
        {
            return fail("deferred-activation forward invalid profile must fail closed");
        }
        if (cofireWrongPairSetRun.success || !cofireWrongPairSetRun.diagnosticError ||
            cofireWrongPairSetRun.stderrText.find("fail_closed=pair_count expected=13") == std::string::npos ||
            cofireWrongPairSetRun.diagnostics.find("cofire policy failed closed") == std::string::npos ||
            cofireInvalidProfileRun.success || !cofireInvalidProfileRun.diagnosticError ||
            cofireInvalidProfileRun.stderrText.find("profile_valid=false") == std::string::npos ||
            cofireInvalidProfileRun.diagnostics.find("cofire policy failed closed") == std::string::npos)
        {
            return fail("deferred-activation cofire probe must fail closed on a changed pair set or profile");
        }
        if (cofireStrictWrongPairSetRun.success || !cofireStrictWrongPairSetRun.diagnosticError ||
            (cofireStrictWrongPairSetRun.stderrText.find("fail_closed=pair_missing") == std::string::npos &&
             cofireStrictWrongPairSetRun.stderrText.find("fail_closed=profile_shape") == std::string::npos) ||
            cofireStrictWrongPairSetRun.diagnostics.find("cofire policy failed closed") == std::string::npos ||
            cofireStrictInvalidProfileRun.success || !cofireStrictInvalidProfileRun.diagnosticError ||
            cofireStrictInvalidProfileRun.stderrText.find("profile_valid=false") == std::string::npos ||
            cofireStrictInvalidProfileRun.diagnostics.find("cofire policy failed closed") == std::string::npos ||
            cofireStrictActiveMaskRun.success || !cofireStrictActiveMaskRun.diagnosticError ||
            cofireStrictActiveMaskRun.diagnostics.find("active_mask_gap_pack_policy=off") == std::string::npos)
        {
            return fail("deferred-activation cofire strict must fail closed on a changed pair set or profile");
        }
        if (cofireStrictExtendedWrongPairSetRun.success ||
            !cofireStrictExtendedWrongPairSetRun.diagnosticError ||
            (cofireStrictExtendedWrongPairSetRun.stderrText.find("fail_closed=pair_missing") ==
                 std::string::npos &&
             cofireStrictExtendedWrongPairSetRun.stderrText.find("fail_closed=profile_shape") ==
                 std::string::npos) ||
            cofireStrictExtendedWrongPairSetRun.diagnostics.find("cofire policy failed closed") ==
                std::string::npos ||
            cofireStrictExtendedInvalidProfileRun.success ||
            !cofireStrictExtendedInvalidProfileRun.diagnosticError ||
            cofireStrictExtendedInvalidProfileRun.stderrText.find("profile_valid=false") ==
                std::string::npos ||
            cofireStrictExtendedInvalidProfileRun.diagnostics.find("cofire policy failed closed") ==
                std::string::npos ||
            cofireStrictExtendedActiveMaskRun.success ||
            !cofireStrictExtendedActiveMaskRun.diagnosticError ||
            cofireStrictExtendedActiveMaskRun.diagnostics.find(
                "active_mask_gap_pack_policy=off") == std::string::npos)
        {
            return fail(
                "deferred-activation cofire strict extended must fail closed on a changed pair set or profile");
        }

        const ActiveMaskGapPackEmitRun invalidPolicyRun = runDeferredActivationForwardEmit(
            fixture.design,
            fixture.session,
            baseDir / "invalid_policy",
            "strict",
            profilePath);
        if (invalidPolicyRun.success || !invalidPolicyRun.diagnosticError ||
            invalidPolicyRun.diagnostics.find(
                "expected off, probe, cofire-probe, cofire-strict, or cofire-strict-extended") ==
                std::string::npos)
        {
            return fail("deferred-activation forward invalid policy must be rejected");
        }
        return 0;
    }

    int runSameBatchActivationCohortFocusedTests()
    {
        ActiveMaskGapPackFixture fixture =
            buildSameBatchActivationCohortFixture();
        const std::filesystem::path baseDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
            "grhsim_cpp_same_batch_activation_cohort";
        std::filesystem::remove_all(baseDir);
        std::filesystem::create_directories(baseDir);
        const std::filesystem::path profilePath = baseDir / "profile.tsv";
        {
            std::ofstream profile(profilePath);
            profile << "supernode_id\tphase\tf\n";
            for (std::size_t supernode = 0; supernode < 16u; ++supernode)
            {
                const std::uint64_t fire =
                    supernode >= 8u && supernode <= 12u ? 10u : supernode + 1u;
                profile << supernode << "\tcompute\t" << fire << '\n';
            }
        }

        ::unsetenv("WOLVRIX_GRHSIM_SAME_BATCH_ACTIVATION_COHORT_POLICY");
        ::unsetenv("WOLVRIX_GRHSIM_SAME_BATCH_ACTIVATION_COHORT_PROFILE_PATH");
        const std::vector<std::string> keysBefore = sessionKeys(fixture.session);
        const ActiveMaskGapPackEmitRun defaultRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "default",
                std::nullopt,
                profilePath);
        const ActiveMaskGapPackEmitRun offRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "off",
                "off",
                profilePath);
        const ActiveMaskGapPackEmitRun probeRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "probe",
                "probe",
                profilePath);
        const ActiveMaskGapPackEmitRun probeRepeatRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "probe_repeat",
                "probe",
                profilePath);
        const ActiveMaskGapPackEmitRun missingProfileRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "missing_profile",
                "probe",
                {});
        const ActiveMaskGapPackEmitRun invalidPolicyRun =
            runSameBatchActivationCohortEmit(
                fixture.design,
                fixture.session,
                baseDir / "invalid_policy",
                "strict",
                profilePath);
        ActiveMaskGapPackFixture productionContractFixture =
            buildSameBatchActivationCohortFixture("SimTop");
        const ActiveMaskGapPackEmitRun missingProductionWitnessRun =
            runSameBatchActivationCohortEmit(
                productionContractFixture.design,
                productionContractFixture.session,
                baseDir / "missing_production_witness",
                "probe",
                profilePath,
                "SimTop");

        if (!defaultRun.success || defaultRun.diagnosticError ||
            !offRun.success || offRun.diagnosticError ||
            !probeRun.success || probeRun.diagnosticError ||
            !probeRepeatRun.success || probeRepeatRun.diagnosticError)
        {
            return fail("same-batch activation cohort fixture emission failed");
        }
        if (defaultRun.artifacts != offRun.artifacts)
        {
            return fail("same-batch activation cohort default/off artifacts must be byte-identical");
        }
        if (probeRun.stderrText != probeRepeatRun.stderrText ||
            probeRun.artifacts.at("grhsim_top.hpp") !=
                probeRepeatRun.artifacts.at("grhsim_top.hpp") ||
            probeRun.artifacts.at("grhsim_top_sched_1.cpp") !=
                probeRepeatRun.artifacts.at("grhsim_top_sched_1.cpp"))
        {
            return fail("same-batch activation cohort probe must be deterministic");
        }
        if (sessionKeys(fixture.session) != keysBefore)
        {
            return fail("same-batch activation cohort probe must not mutate session keys");
        }
        constexpr std::string_view marker =
            "[GRHSIM_SAME_BATCH_ACTIVATION_COHORT]";
        if (defaultRun.stderrText.find(marker) != std::string::npos ||
            offRun.stderrText.find(marker) != std::string::npos)
        {
            return fail("same-batch activation cohort default/off must stay silent");
        }
        const std::string_view summary = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_SAME_BATCH_ACTIVATION_COHORT] policy=probe ");
        const std::string_view row = probeLogLine(
            probeRun.stderrText,
            "[GRHSIM_SAME_BATCH_ACTIVATION_COHORT] cohort=0 ");
        if (summary.find("selected=1 members=5 ops=5 control_bae=5 projected_bae=1") ==
                std::string_view::npos ||
            summary.find("no_mutation=true") == std::string_view::npos ||
            row.find("batch=1") == std::string_view::npos ||
            row.find("first_supernode=8 last_supernode=12") == std::string_view::npos ||
            row.find("first_active_id=8 last_active_id=12") == std::string_view::npos ||
            row.find("profile_fire=10") == std::string_view::npos)
        {
            return fail("same-batch activation cohort exact static row is incomplete");
        }
        if (missingProfileRun.success || !missingProfileRun.diagnosticError ||
            missingProfileRun.stderrText.find("profile path is empty") == std::string::npos ||
            missingProfileRun.diagnostics.find("probe failed closed") == std::string::npos ||
            invalidPolicyRun.success || !invalidPolicyRun.diagnosticError ||
            invalidPolicyRun.diagnostics.find("expected off or probe") == std::string::npos ||
            missingProductionWitnessRun.success ||
            !missingProductionWitnessRun.diagnosticError ||
            missingProductionWitnessRun.stderrText.find(
                "fail_closed=production_witness_missing production_request=true") ==
                std::string::npos ||
            missingProductionWitnessRun.diagnostics.find("probe failed closed") ==
                std::string::npos)
        {
            return fail("same-batch activation cohort policy/profile fail-closed gate is missing");
        }

        const auto schedIt = probeRun.artifacts.find("grhsim_top_sched_1.cpp");
        const auto headerIt = probeRun.artifacts.find("grhsim_top.hpp");
        const auto stateIt = probeRun.artifacts.find("grhsim_top_state.cpp");
        if (schedIt == probeRun.artifacts.end() ||
            headerIt == probeRun.artifacts.end() ||
            stateIt == probeRun.artifacts.end())
        {
            return fail("same-batch activation cohort generated artifacts are missing");
        }
        const std::string &sched = schedIt->second;
        const std::size_t method = sched.find("void GrhSIM_top::eval_compute_batch_");
        const std::size_t entryCounter = sched.find(
            "++runtime_profile_same_batch_cohort_entry_batch_count_[0u]",
            method);
        const std::size_t firstWordLoad = sched.find(
            "& dispatchMask",
            method);
        const std::size_t exitCounter = sched.find(
            "++runtime_profile_same_batch_cohort_exit_batch_count_[0u]",
            method);
        if (method == std::string::npos || entryCounter == std::string::npos ||
            firstWordLoad == std::string::npos || exitCounter == std::string::npos ||
            !(method < entryCounter && entryCounter < firstWordLoad &&
              firstWordLoad < exitCounter) ||
            sched.find("++runtime_profile_same_batch_cohort_body_fire_[0u]") ==
                std::string::npos ||
            headerIt->second.find("runtime_profile_same_batch_cohort_entry_pending_") ==
                std::string::npos)
        {
            return fail("same-batch activation cohort counters are not placed around word consumption");
        }
        bool resetPresent = false;
        for (const auto &[name, source] : probeRun.artifacts)
        {
            (void)name;
            if (source.find(
                    "runtime_profile_same_batch_cohort_entry_pending_.fill(UINT64_C(0));") !=
                std::string::npos)
            {
                resetPresent = true;
                break;
            }
        }
        if (!resetPresent)
        {
            return fail("same-batch activation cohort runtime counters are not reset");
        }

        const std::filesystem::path probeDir = baseDir / "probe";
        const std::string buildCommand =
            "make -C " + probeDir.string() + " CXX=clang++ CXXFLAGS='" +
            std::string(kHarnessCompileFlags) + "'";
        if (std::system(buildCommand.c_str()) != 0)
        {
            return fail("same-batch activation cohort generated archive failed to build");
        }
        const std::filesystem::path harnessPath = probeDir / "cohort_harness.cpp";
        {
            std::ofstream harness(harnessPath);
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "int main() { GrhSIM_top sim; sim.set_runtime_profile_enabled(true); "
                       "sim.init(); sim.in = 3; sim.eval(); sim.dump_runtime_profile(); return 0; }\n";
        }
        const std::filesystem::path harnessExe = probeDir / "cohort_harness";
        const std::string compileCommand =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" +
            probeDir.string() + " " + harnessPath.string() + " " +
            (probeDir / "libgrhsim_top.a").string() + " -o " +
            harnessExe.string();
        if (std::system(compileCommand.c_str()) != 0)
        {
            return fail("same-batch activation cohort harness failed to compile");
        }
        const std::filesystem::path cohortTsv = probeDir / "cohort.tsv";
        const std::filesystem::path fireTsv = probeDir / "fire.tsv";
        const std::string runCommand =
            "WOLVRIX_GRHSIM_SAME_BATCH_ACTIVATION_COHORT_TSV=" +
            cohortTsv.string() + " WOLVRIX_GRHSIM_SUPERNODE_TSV=" +
            fireTsv.string() + " " + harnessExe.string();
        if (std::system(runCommand.c_str()) != 0)
        {
            return fail("same-batch activation cohort harness failed to run");
        }
        const std::string tsv = readFile(cohortTsv);
        const std::size_t firstNewline = tsv.find('\n');
        const std::size_t secondNewline =
            firstNewline == std::string::npos ? std::string::npos :
                                                tsv.find('\n', firstNewline + 1u);
        const std::string_view firstRow =
            firstNewline == std::string::npos
                ? std::string_view{}
                : std::string_view(tsv).substr(
                      firstNewline + 1u,
                      secondNewline == std::string::npos
                          ? std::string_view::npos
                          : secondNewline - firstNewline - 1u);
        const std::vector<std::string_view> fields = splitTabs(firstRow);
        if (tsv.find("cohort_id\tbatch_id\tsource_count") != 0u ||
            fields.size() != 28u || fields[6] != "5" || fields[16] != "0" ||
            fields[20] != "0" || fields[24] != "0" || fields[18] != fields[19])
        {
            return fail("same-batch activation cohort runtime TSV counters are inconsistent");
        }
        return 0;
    }

    std::string normalizeActiveMaskGapPackLog(std::string log)
    {
        constexpr std::string_view marker = "elapsed_us=";
        std::size_t position = 0;
        while ((position = log.find(marker, position)) != std::string::npos)
        {
            const std::size_t valueBegin = position + marker.size();
            const std::size_t valueEnd = log.find_first_not_of("0123456789", valueBegin);
            log.replace(valueBegin, valueEnd - valueBegin, "<elapsed>");
            position = valueBegin + std::string_view("<elapsed>").size();
        }
        return log;
    }

    std::string stripActiveMaskWriteStatements(std::string_view source)
    {
        std::string stripped;
        std::size_t lineBegin = 0u;
        while (lineBegin < source.size())
        {
            const std::size_t lineEnd = source.find('\n', lineBegin);
            const std::string_view line = source.substr(
                lineBegin,
                lineEnd == std::string_view::npos ? source.size() - lineBegin
                                                  : lineEnd - lineBegin + 1u);
            const bool helperWrite =
                line.find("grhsim_or_active_u64(") != std::string_view::npos ||
                line.find("grhsim_or_active_u32(") != std::string_view::npos ||
                line.find("grhsim_or_active_u16(") != std::string_view::npos;
            const bool byteWrite =
                line.find("supernode_active_curr_[") != std::string_view::npos &&
                line.find(" |= ") != std::string_view::npos;
            if (!helperWrite && !byteWrite)
            {
                stripped.append(line);
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            lineBegin = lineEnd + 1u;
        }
        return stripped;
    }

    std::string conditionalWrapperLines(std::string_view source)
    {
        std::string wrappers;
        std::size_t lineBegin = 0u;
        while (lineBegin < source.size())
        {
            const std::size_t lineEnd = source.find('\n', lineBegin);
            const std::string_view line = source.substr(
                lineBegin,
                lineEnd == std::string_view::npos ? source.size() - lineBegin
                                                  : lineEnd - lineBegin + 1u);
            if (line.find("if (grhsim_changed_") != std::string_view::npos)
            {
                wrappers.append(line);
            }
            if (lineEnd == std::string_view::npos)
            {
                break;
            }
            lineBegin = lineEnd + 1u;
        }
        return wrappers;
    }

    int runActiveMaskGapPackFocusedTests()
    {
        ActiveMaskGapPackFixture fixture = buildActiveMaskGapPackFixture();
        const std::filesystem::path baseDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_active_mask_gap_pack";
        const std::vector<std::string> keysBefore = sessionKeys(fixture.session);

        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        const ActiveMaskGapPackEmitRun defaultRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "default", std::nullopt, 2u);
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY", "targeted-direct", 1);
        const ActiveMaskGapPackEmitRun offRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "off", "off", 2u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        const ActiveMaskGapPackEmitRun probeSerialRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "probe_serial", "probe", 1u);
        const ActiveMaskGapPackEmitRun probeParallelRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "probe_parallel", "probe", 4u);
        const ActiveMaskGapPackEmitRun probeRuntimeProfileRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "probe_runtime_profile",
            "probe",
            1u,
            true);
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY", "probe", 1);
        const ActiveMaskGapPackEmitRun probeEnvironmentRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "probe_environment", std::nullopt, 2u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        const ActiveMaskGapPackEmitRun targetedSerialRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "targeted_serial", "targeted-direct", 1u);
        const ActiveMaskGapPackEmitRun targetedParallelRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "targeted_parallel", "targeted-direct", 4u);
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY", "targeted-direct", 1);
        const ActiveMaskGapPackEmitRun targetedEnvironmentRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "targeted_environment", std::nullopt, 2u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        const ActiveMaskGapPackEmitRun tableContiguousSerialRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_contiguous_serial",
            "targeted-table-contiguous",
            1u);
        const ActiveMaskGapPackEmitRun tableContiguousParallelRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_contiguous_parallel",
            "targeted-table-contiguous",
            4u);
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY",
                 "targeted-table-contiguous",
                 1);
        const ActiveMaskGapPackEmitRun tableContiguousEnvironmentRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_contiguous_environment",
            std::nullopt,
            2u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        const ActiveMaskGapPackEmitRun tableGapSerialRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_gap_serial",
            "targeted-table-gap",
            1u);
        const ActiveMaskGapPackEmitRun tableGapParallelRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_gap_parallel",
            "targeted-table-gap",
            4u);
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY", "targeted-table-gap", 1);
        const ActiveMaskGapPackEmitRun tableGapEnvironmentRun = runActiveMaskGapPackEmit(
            fixture.design,
            fixture.session,
            baseDir / "table_gap_environment",
            std::nullopt,
            2u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");

        if (!defaultRun.success || defaultRun.diagnosticError ||
            !offRun.success || offRun.diagnosticError ||
            !probeSerialRun.success || probeSerialRun.diagnosticError ||
            !probeParallelRun.success || probeParallelRun.diagnosticError ||
            !probeRuntimeProfileRun.success || probeRuntimeProfileRun.diagnosticError ||
            !probeEnvironmentRun.success || probeEnvironmentRun.diagnosticError ||
            !targetedSerialRun.success || targetedSerialRun.diagnosticError ||
            !targetedParallelRun.success || targetedParallelRun.diagnosticError ||
            !targetedEnvironmentRun.success || targetedEnvironmentRun.diagnosticError ||
            !tableContiguousSerialRun.success || tableContiguousSerialRun.diagnosticError ||
            !tableContiguousParallelRun.success || tableContiguousParallelRun.diagnosticError ||
            !tableContiguousEnvironmentRun.success || tableContiguousEnvironmentRun.diagnosticError ||
            !tableGapSerialRun.success || tableGapSerialRun.diagnosticError ||
            !tableGapParallelRun.success || tableGapParallelRun.diagnosticError ||
            !tableGapEnvironmentRun.success || tableGapEnvironmentRun.diagnosticError)
        {
            return fail("active-mask gap-pack policy fixture emission failed");
        }
        if (defaultRun.artifacts != offRun.artifacts ||
            defaultRun.artifacts != probeSerialRun.artifacts ||
            probeSerialRun.artifacts != probeParallelRun.artifacts ||
            probeSerialRun.artifacts != probeEnvironmentRun.artifacts)
        {
            return fail("active-mask gap-pack off/probe must preserve every generated artifact byte");
        }
        constexpr std::string_view kProbePrefix = "[GRHSIM_ACTIVE_MASK_GAP_PACK]";
        if (defaultRun.stderrText.find(kProbePrefix) != std::string::npos ||
            offRun.stderrText.find(kProbePrefix) != std::string::npos)
        {
            return fail("active-mask gap-pack off must not construct or report probe statistics");
        }
        if (probeSerialRun.stderrText.find("validation=pass") == std::string::npos ||
            probeSerialRun.stderrText.find("coverage=classified_global_activation_path") == std::string::npos ||
            probeSerialRun.stderrText.find("commit_range_groups=") == std::string::npos ||
            probeSerialRun.stderrText.find("unclassified_groups=") == std::string::npos)
        {
            return fail("active-mask gap-pack probe summary is missing validation or coverage limits");
        }
        if (normalizeActiveMaskGapPackLog(probeSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(probeParallelRun.stderrText) ||
            normalizeActiveMaskGapPackLog(probeSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(probeEnvironmentRun.stderrText))
        {
            return fail("active-mask gap-pack probe aggregation must be parallel deterministic");
        }
        if (targetedSerialRun.artifacts != targetedParallelRun.artifacts ||
            targetedSerialRun.artifacts != targetedEnvironmentRun.artifacts ||
            normalizeActiveMaskGapPackLog(targetedSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(targetedParallelRun.stderrText) ||
            normalizeActiveMaskGapPackLog(targetedSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(targetedEnvironmentRun.stderrText))
        {
            return fail("active-mask gap-pack targeted-direct emission must be parallel deterministic");
        }
        if (tableContiguousSerialRun.artifacts != tableContiguousParallelRun.artifacts ||
            tableContiguousSerialRun.artifacts != tableContiguousEnvironmentRun.artifacts ||
            normalizeActiveMaskGapPackLog(tableContiguousSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(tableContiguousParallelRun.stderrText) ||
            normalizeActiveMaskGapPackLog(tableContiguousSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(tableContiguousEnvironmentRun.stderrText))
        {
            return fail("active-mask table-contiguous emission must be parallel deterministic");
        }
        if (tableGapSerialRun.artifacts != tableGapParallelRun.artifacts ||
            tableGapSerialRun.artifacts != tableGapEnvironmentRun.artifacts ||
            normalizeActiveMaskGapPackLog(tableGapSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(tableGapParallelRun.stderrText) ||
            normalizeActiveMaskGapPackLog(tableGapSerialRun.stderrText) !=
                normalizeActiveMaskGapPackLog(tableGapEnvironmentRun.stderrText))
        {
            return fail("active-mask table-gap emission must be parallel deterministic");
        }
        if (sessionKeys(fixture.session) != keysBefore)
        {
            return fail("active-mask gap-pack probe must not write session state");
        }
        for (const auto &[_, content] : probeSerialRun.artifacts)
        {
            if (content.find(kProbePrefix) != std::string::npos)
            {
                return fail("active-mask gap-pack probe log must not enter generated artifacts");
            }
        }

        const std::string_view nonTableLine =
            probeStatsLine(probeSerialRun.stderrText, "non-table");
        const std::string_view tableLine =
            probeStatsLine(probeSerialRun.stderrText, "table");
        const std::string_view excludedLine = probeLogLine(
            probeSerialRun.stderrText,
            "[GRHSIM_ACTIVE_MASK_GAP_PACK] owner=excluded ");
        const auto nonTableGroups = probeStatsUnsigned(nonTableLine, "groups");
        const auto nonTableEntries = probeStatsUnsigned(nonTableLine, "entries");
        const auto nonTableLocalTargets = probeStatsUnsigned(nonTableLine, "local_targets");
        const auto nonTableGlobalActiveIds = probeStatsUnsigned(nonTableLine, "global_active_ids");
        const auto nonTableConditionalGroups = probeStatsUnsigned(nonTableLine, "conditional_groups");
        const auto nonTableRealBytes = probeStatsUnsigned(nonTableLine, "real_covered_bytes");
        const auto nonTableCoveredBytes = probeStatsUnsigned(nonTableLine, "candidate_covered_bytes");
        const auto nonTableHolePpm = probeStatsUnsigned(nonTableLine, "hole_ppm");
        const auto nonTableBaseline = probeStatsUnsigned(nonTableLine, "baseline_writes");
        const auto nonTableContiguous = probeStatsUnsigned(nonTableLine, "contiguous_writes");
        const auto nonTableCandidate = probeStatsUnsigned(nonTableLine, "candidate_writes");
        const auto nonTableGapSaved = probeStatsUnsigned(nonTableLine, "gap_saved");
        const auto nonTableHoles = probeStatsUnsigned(nonTableLine, "hole_bytes");
        const auto nonTableGapImproved = probeStatsUnsigned(nonTableLine, "gap_improved");
        const auto nonTableInvalid = probeStatsUnsigned(nonTableLine, "invalid_groups");
        const auto nonTableBoundsRejects = probeStatsUnsigned(nonTableLine, "rejected_bounds_transitions");
        const auto nonTableLaneRejects = probeStatsUnsigned(nonTableLine, "rejected_lane_transitions");
        if (!nonTableGroups || !nonTableEntries || !nonTableLocalTargets ||
            !nonTableGlobalActiveIds || !nonTableConditionalGroups ||
            !nonTableRealBytes || !nonTableCoveredBytes || !nonTableHolePpm ||
            !nonTableBaseline ||
            !nonTableContiguous || !nonTableCandidate || !nonTableGapSaved ||
            !nonTableHoles || !nonTableGapImproved || !nonTableInvalid ||
            !nonTableBoundsRejects || !nonTableLaneRejects)
        {
            return fail("active-mask gap-pack non-table statistics are incomplete: " +
                        std::string(nonTableLine));
        }
        if (*nonTableGroups != 5u ||
            *nonTableEntries != 41u ||
            *nonTableLocalTargets != 2u ||
            *nonTableGlobalActiveIds != 42u ||
            *nonTableConditionalGroups != 5u ||
            *nonTableRealBytes != 41u ||
            *nonTableCoveredBytes != 48u ||
            *nonTableHolePpm != 145833u ||
            *nonTableBaseline != 16u ||
            *nonTableContiguous != *nonTableBaseline ||
            *nonTableCandidate != 10u ||
            *nonTableGapSaved != 6u ||
            *nonTableHoles != 7u ||
            *nonTableGapImproved != 3u ||
            *nonTableInvalid != 0u ||
            *nonTableBoundsRejects != 5u ||
            *nonTableLaneRejects != 2u)
        {
            return fail("active-mask gap-pack hole/tail/lane/non-table plan statistics are wrong: " +
                        std::string(nonTableLine));
        }

        const std::string_view targetedSummary = probeLogLine(
            targetedSerialRun.stderrText,
            "[GRHSIM_ACTIVE_MASK_GAP_PACK] policy=targeted-direct ");
        const std::string_view targetedNonTableLine =
            probeStatsLine(targetedSerialRun.stderrText, "non-table");
        const auto selectedGroups = probeStatsUnsigned(targetedSummary, "selected_groups");
        const auto selectedBaselineWrites =
            probeStatsUnsigned(targetedSummary, "selected_baseline_writes");
        const auto selectedCandidateWrites =
            probeStatsUnsigned(targetedSummary, "selected_candidate_writes");
        const auto selectedSavings = probeStatsUnsigned(targetedSummary, "selected_savings");
        const auto targetedBaselineWrites =
            probeStatsUnsigned(targetedNonTableLine, "baseline_writes");
        const auto targetedCandidateWrites =
            probeStatsUnsigned(targetedNonTableLine, "candidate_writes");
        if (!selectedGroups || !selectedBaselineWrites || !selectedCandidateWrites ||
            !selectedSavings || !targetedBaselineWrites || !targetedCandidateWrites ||
            *selectedGroups != 3u || *selectedBaselineWrites != 12u ||
            *selectedCandidateWrites != 6u || *selectedSavings != 6u ||
            *targetedBaselineWrites != 16u || *targetedCandidateWrites != 10u)
        {
            return fail("active-mask gap-pack targeted-direct selection statistics are wrong: " +
                        std::string(targetedSummary) + " / " +
                        std::string(targetedNonTableLine));
        }

        const auto tableGroups = probeStatsUnsigned(tableLine, "groups");
        const auto tableEntries = probeStatsUnsigned(tableLine, "entries");
        const auto tableBaseline = probeStatsUnsigned(tableLine, "baseline_writes");
        const auto tableContiguous = probeStatsUnsigned(tableLine, "contiguous_writes");
        const auto tableCandidate = probeStatsUnsigned(tableLine, "candidate_writes");
        const auto tableGapSaved = probeStatsUnsigned(tableLine, "gap_saved");
        const auto tableHoles = probeStatsUnsigned(tableLine, "hole_bytes");
        const auto tableCoveredBytes = probeStatsUnsigned(tableLine, "candidate_covered_bytes");
        const auto tableGapImproved = probeStatsUnsigned(tableLine, "gap_improved");
        const auto tableInvalid = probeStatsUnsigned(tableLine, "invalid_groups");
        const auto tableWidth2 = probeStatsUnsigned(tableLine, "width2");
        const auto tableWidth8 = probeStatsUnsigned(tableLine, "width8");
        if (!tableGroups || !tableEntries || !tableBaseline || !tableContiguous ||
            !tableCandidate || !tableGapSaved || !tableHoles || !tableCoveredBytes ||
            !tableGapImproved || !tableInvalid || !tableWidth2 || !tableWidth8 ||
            *tableGroups == 0u ||
            *tableEntries != *tableGroups * 32u ||
            *tableBaseline != *tableGroups * 32u ||
            *tableContiguous != *tableGroups * 32u ||
            *tableCandidate != *tableGroups * 8u ||
            *tableGapSaved != *tableGroups * 24u ||
            *tableHoles != *tableGroups * 32u ||
            *tableCoveredBytes != *tableGroups * 64u ||
            *tableGapImproved != *tableGroups ||
            *tableInvalid != 0u ||
            *tableWidth2 != 0u ||
            *tableWidth8 != *tableGroups * 8u)
        {
            return fail("active-mask gap-pack 31/32 table threshold statistics are wrong: " +
                        std::string(tableLine));
        }

        const auto checkTableSelection = [&](const ActiveMaskGapPackEmitRun &run,
                                             std::string_view policy,
                                             std::size_t expectedCandidateWrites) -> bool
        {
            const std::string_view summary = probeLogLine(
                run.stderrText,
                "[GRHSIM_ACTIVE_MASK_GAP_PACK] policy=" + std::string(policy) + " ");
            const auto groups = probeStatsUnsigned(summary, "selected_groups");
            const auto baseline = probeStatsUnsigned(summary, "selected_baseline_writes");
            const auto candidate = probeStatsUnsigned(summary, "selected_candidate_writes");
            const auto savings = probeStatsUnsigned(summary, "selected_savings");
            return groups && baseline && candidate && savings &&
                   *groups == *tableGroups &&
                   *baseline == *tableGroups * 32u &&
                   *candidate == *tableGroups * expectedCandidateWrites &&
                   *savings == *baseline - *candidate;
        };
        if (!checkTableSelection(tableContiguousSerialRun,
                                 "targeted-table-contiguous",
                                 32u) ||
            !checkTableSelection(tableGapSerialRun, "targeted-table-gap", 8u))
        {
            return fail("active-mask table policy selection statistics are wrong");
        }

        const auto profileHeaderIt = probeRuntimeProfileRun.artifacts.find("grhsim_top.hpp");
        const auto profileStateIt = probeRuntimeProfileRun.artifacts.find("grhsim_top_state.cpp");
        if (profileHeaderIt == probeRuntimeProfileRun.artifacts.end() ||
            profileStateIt == probeRuntimeProfileRun.artifacts.end())
        {
            return fail("active-mask table runtime-profile fixture is missing header/state artifacts");
        }
        std::string profileSchedSources;
        std::string profileStateSources;
        for (const auto &[name, content] : probeRuntimeProfileRun.artifacts)
        {
            if (name.find("grhsim_top_sched_") == 0u)
            {
                profileSchedSources += content;
            }
            if (name.find("grhsim_top_state") == 0u)
            {
                profileStateSources += content;
            }
        }
        const std::string &profileHeader = profileHeaderIt->second;
        const std::string &profileState = profileStateIt->second;
        if (profileHeader.find("runtime_profile_active_mask_table_evaluations_") == std::string::npos ||
            profileHeader.find("runtime_profile_active_mask_table_current_writes_") == std::string::npos ||
            profileHeader.find("runtime_profile_active_mask_table_contiguous_writes_") == std::string::npos ||
            profileHeader.find("runtime_profile_active_mask_table_zero_hole_writes_") == std::string::npos ||
            profileState.find("[GRHSIM_ACTIVE_MASK_TABLE_PROFILE]") == std::string::npos ||
            profileStateSources.find("runtime_profile_active_mask_table_evaluations_ = UINT64_C(0);") == std::string::npos ||
            profileSchedSources.find("++runtime_profile_active_mask_table_evaluations_;") == std::string::npos ||
            profileSchedSources.find("runtime_profile_active_mask_table_current_writes_ += UINT64_C(32);") == std::string::npos ||
            profileSchedSources.find("runtime_profile_active_mask_table_contiguous_writes_ += UINT64_C(32);") == std::string::npos ||
            profileSchedSources.find("runtime_profile_active_mask_table_zero_hole_writes_ += UINT64_C(8);") == std::string::npos)
        {
            return fail("active-mask table runtime-profile counters are missing or have wrong static costs");
        }
        for (const auto &[name, content] : probeSerialRun.artifacts)
        {
            if (content.find("runtime_profile_active_mask_table_") != std::string::npos)
            {
                return fail("active-mask table runtime-profile counters must remain opt-in");
            }
        }

        const auto seedGroups = probeStatsUnsigned(excludedLine, "seed_groups");
        const auto seedEntries = probeStatsUnsigned(excludedLine, "seed_entries");
        const auto commitRangeGroups = probeStatsUnsigned(excludedLine, "commit_range_groups");
        const auto commitRangeEntries = probeStatsUnsigned(excludedLine, "commit_range_entries");
        const auto unclassifiedGroups = probeStatsUnsigned(excludedLine, "unclassified_groups");
        const auto unclassifiedEntries = probeStatsUnsigned(excludedLine, "unclassified_entries");
        if (!seedGroups || !seedEntries || !commitRangeGroups || !commitRangeEntries ||
            !unclassifiedGroups || !unclassifiedEntries ||
            *seedGroups == 0u || *seedEntries == 0u ||
            *commitRangeGroups != 0u || *commitRangeEntries != 0u ||
            *unclassifiedGroups != 0u || *unclassifiedEntries != 0u)
        {
            return fail("active-mask gap-pack excluded-path classification is wrong: " +
                        std::string(excludedLine));
        }

        bool targetedSchedChanged = false;
        for (const auto &[name, content] : targetedSerialRun.artifacts)
        {
            const auto baselineIt = offRun.artifacts.find(name);
            if (baselineIt == offRun.artifacts.end())
            {
                return fail("active-mask gap-pack targeted-direct added an unexpected artifact: " + name);
            }
            const bool schedSource = name.starts_with("grhsim_top_sched_") && name.ends_with(".cpp");
            if (!schedSource)
            {
                if (content != baselineIt->second)
                {
                    return fail("active-mask gap-pack targeted-direct changed a non-schedule artifact: " + name);
                }
                continue;
            }
            targetedSchedChanged = targetedSchedChanged || content != baselineIt->second;
            if (stripActiveMaskWriteStatements(content) !=
                stripActiveMaskWriteStatements(baselineIt->second))
            {
                return fail("active-mask gap-pack targeted-direct changed schedule code outside active-mask writes: " +
                            name);
            }
            if (conditionalWrapperLines(content) != conditionalWrapperLines(baselineIt->second))
            {
                return fail("active-mask gap-pack targeted-direct changed a frozen conditional wrapper: " + name);
            }
        }
        if (targetedSerialRun.artifacts.size() != offRun.artifacts.size() || !targetedSchedChanged)
        {
            return fail("active-mask gap-pack targeted-direct artifact set or expected schedule diff is wrong");
        }

        const auto verifyTableArtifacts = [&](const ActiveMaskGapPackEmitRun &run) -> bool
        {
            if (run.artifacts.size() != offRun.artifacts.size())
            {
                return false;
            }
            bool schedChanged = false;
            for (const auto &[name, content] : run.artifacts)
            {
                const auto baselineIt = offRun.artifacts.find(name);
                if (baselineIt == offRun.artifacts.end())
                {
                    return false;
                }
                const bool schedSource =
                    name.starts_with("grhsim_top_sched_") && name.ends_with(".cpp");
                if (!schedSource)
                {
                    if (content != baselineIt->second)
                    {
                        return false;
                    }
                    continue;
                }
                schedChanged = schedChanged || content != baselineIt->second;
                if (conditionalWrapperLines(content) !=
                    conditionalWrapperLines(baselineIt->second))
                {
                    return false;
                }
            }
            if (!schedChanged)
            {
                return false;
            }
            return true;
        };
        if (!verifyTableArtifacts(tableContiguousSerialRun) ||
            !verifyTableArtifacts(tableGapSerialRun) ||
            tableContiguousSerialRun.artifacts == tableGapSerialRun.artifacts)
        {
            return fail("active-mask table policies changed frozen artifacts or emitted no distinct rewrite");
        }
        for (const auto &[name, contiguousContent] : tableContiguousSerialRun.artifacts)
        {
            if (!name.starts_with("grhsim_top_sched_") || !name.ends_with(".cpp"))
            {
                continue;
            }
            const std::string &gapContent = tableGapSerialRun.artifacts.at(name);
            if (stripActiveMaskWriteStatements(contiguousContent) !=
                stripActiveMaskWriteStatements(gapContent))
            {
                return fail("active-mask table policies differ outside selected write encoding: " + name);
            }
        }

        std::string schedSources;
        std::string targetedSchedSources;
        std::string tableContiguousSchedSources;
        std::string tableGapSchedSources;
        for (const auto &[name, content] : offRun.artifacts)
        {
            if (name.starts_with("grhsim_top_sched_") && name.ends_with(".cpp"))
            {
                schedSources += content;
                targetedSchedSources += targetedSerialRun.artifacts.at(name);
                tableContiguousSchedSources += tableContiguousSerialRun.artifacts.at(name);
                tableGapSchedSources += tableGapSerialRun.artifacts.at(name);
            }
        }
        const std::size_t condition = schedSources.find("if (grhsim_changed_2) {");
        const std::size_t openBrace = schedSources.find('{', condition);
        const std::size_t closeBrace = findMatchingBrace(schedSources, openBrace);
        if (condition == std::string::npos || closeBrace == std::string::npos)
        {
            return fail("active-mask gap-pack conditional baseline branch is missing");
        }
        const std::string_view conditionalBlock(schedSources.data() + openBrace,
                                                closeBrace - openBrace + 1u);
        for (std::size_t byteIndex : {8u, 10u, 12u, 14u})
        {
            if (conditionalBlock.find("supernode_active_curr_[" +
                                      std::to_string(byteIndex) + "u]") == std::string_view::npos)
            {
                return fail("active-mask gap-pack probe changed the frozen conditional lowering");
            }
        }
        const std::size_t targetedCondition = targetedSchedSources.find("if (grhsim_changed_2) {");
        const std::size_t targetedOpenBrace = targetedSchedSources.find('{', targetedCondition);
        const std::size_t targetedCloseBrace =
            findMatchingBrace(targetedSchedSources, targetedOpenBrace);
        if (targetedCondition == std::string::npos || targetedCloseBrace == std::string::npos)
        {
            return fail("active-mask gap-pack targeted-direct changed the frozen conditional branch");
        }
        const std::string_view targetedConditionalBlock(
            targetedSchedSources.data() + targetedOpenBrace,
            targetedCloseBrace - targetedOpenBrace + 1u);
        if (targetedConditionalBlock.find(
                "grhsim_or_active_u64(supernode_active_curr_.data(), 8u, "
                "UINT64_C(281479271743489));") ==
            std::string_view::npos)
        {
            return fail("active-mask gap-pack targeted-direct did not apply the selected hole chunk");
        }
        for (std::string_view statement : {
                 "grhsim_or_active_u64(supernode_active_curr_.data(), 80u,",
                 "grhsim_or_active_u64(supernode_active_curr_.data(), 88u,",
                 "grhsim_or_active_u64(supernode_active_curr_.data(), 96u,"})
        {
            if (targetedSchedSources.find(statement) == std::string::npos)
            {
                return fail("active-mask gap-pack targeted-direct did not preserve the 31-entry direct threshold");
            }
        }
        if (targetedSchedSources.find(
                "grhsim_or_active_u64(supernode_active_curr_.data(), 104u, "
                "UINT64_C(282578800148737));") == std::string::npos ||
            targetedSchedSources.find(
                "grhsim_or_active_u32(supernode_active_curr_.data(), 20u, "
                "static_cast<std::uint32_t>(static_cast<std::uint32_t>(-static_cast<std::uint32_t>("
                "grhsim_changed_7)) & UINT32_C(65539)));") == std::string::npos)
        {
            return fail("active-mask gap-pack targeted-direct selected mask contents are wrong");
        }
        if (schedSources.find(
                "grhsim_or_active_u32(supernode_active_curr_.data(), 104u,") == std::string::npos ||
            schedSources.find(
                "grhsim_or_active_u16(supernode_active_curr_.data(), 108u,") == std::string::npos ||
            schedSources.find("supernode_active_curr_[110u] |= UINT8_C(") == std::string::npos)
        {
            return fail("active-mask gap-pack 31-entry baseline fixture is missing its tail chunks");
        }
        if (targetedSchedSources.find("{112u, UINT8_C(") == std::string::npos ||
            targetedSchedSources.find("{176u, UINT8_C(") == std::string::npos ||
            targetedSchedSources.find(
                "supernode_active_curr_[entry.word_index] |= entry.mask;") == std::string::npos ||
            targetedSchedSources.find(
                "grhsim_or_active_u64(supernode_active_curr_.data(), 112u,") != std::string::npos)
        {
            return fail("active-mask gap-pack targeted-direct changed the 32-entry table lowering");
        }

        for (std::size_t byteIndex : {112u, 114u, 116u, 118u, 120u, 122u, 124u, 126u,
                                      130u, 132u, 134u, 136u, 138u, 140u, 142u, 144u,
                                      146u, 148u, 150u, 152u, 154u, 156u, 158u, 160u,
                                      162u, 164u, 166u, 168u, 170u, 172u, 174u, 176u})
        {
            if (tableContiguousSchedSources.find(
                    "supernode_active_curr_[" + std::to_string(byteIndex) +
                    "u] |= UINT8_C(1);") == std::string::npos)
            {
                return fail("active-mask table-contiguous candidate missed a sparse byte write");
            }
        }
        for (std::size_t byteIndex : {112u, 120u, 130u, 138u, 146u, 154u, 162u, 170u})
        {
            if (tableGapSchedSources.find(
                    "grhsim_or_active_u64(supernode_active_curr_.data(), " +
                    std::to_string(byteIndex) +
                    "u, UINT64_C(281479271743489));") == std::string::npos)
            {
                return fail("active-mask table-gap candidate missed an expected sparse u64 chunk");
            }
        }
        if (tableContiguousSchedSources.find(
                "grhsim_or_active_u64(supernode_active_curr_.data(), 112u,") != std::string::npos ||
            tableGapSchedSources.find(
                "supernode_active_curr_[112u] |= UINT8_C(1);") != std::string::npos)
        {
            return fail("active-mask table policies did not keep contiguous and gap encodings distinct");
        }
        const std::string_view tableLoop =
            "supernode_active_curr_[entry.word_index] |= entry.mask;";
        for (const std::string *candidate : {&tableContiguousSchedSources,
                                             &tableGapSchedSources})
        {
            if (countSubstring(*candidate, "{112u, UINT8_C(") >=
                    countSubstring(schedSources, "{112u, UINT8_C(") ||
                countSubstring(*candidate, "{176u, UINT8_C(") >=
                    countSubstring(schedSources, "{176u, UINT8_C(") ||
                countSubstring(*candidate, tableLoop) >= countSubstring(schedSources, tableLoop))
            {
                return fail("active-mask table candidate retained a selected generic table loop");
            }
            for (std::string_view statement : {
                     "grhsim_or_active_u64(supernode_active_curr_.data(), 80u,",
                     "grhsim_or_active_u64(supernode_active_curr_.data(), 88u,",
                     "grhsim_or_active_u64(supernode_active_curr_.data(), 96u,",
                     "grhsim_or_active_u32(supernode_active_curr_.data(), 104u,",
                     "grhsim_or_active_u16(supernode_active_curr_.data(), 108u,",
                     "supernode_active_curr_[110u] |= UINT8_C("})
            {
                if (candidate->find(statement) == std::string::npos)
                {
                    return fail("active-mask table candidate changed the 31-entry direct lowering");
                }
            }
            if (candidate->find(
                    "grhsim_or_active_u64(supernode_active_curr_.data(), 8u, "
                    "UINT64_C(281479271743489));") != std::string::npos ||
                candidate->find("supernode_active_curr_[8u] |= UINT8_C(") == std::string::npos)
            {
                return fail("active-mask table candidate changed a non-table direct group");
            }
        }

        const ActiveMaskGapPackEmitRun invalidAttributeRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "invalid_attribute", "targeted", 1u);
        if (invalidAttributeRun.success || !invalidAttributeRun.diagnosticError ||
            invalidAttributeRun.diagnostics.find("targeted-table-contiguous") == std::string::npos ||
            invalidAttributeRun.diagnostics.find("targeted-table-gap") == std::string::npos)
        {
            return fail("active-mask gap-pack unknown attribute policy must be rejected");
        }
        ::setenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY", "invalid", 1);
        const ActiveMaskGapPackEmitRun invalidEnvironmentRun = runActiveMaskGapPackEmit(
            fixture.design, fixture.session, baseDir / "invalid_environment", std::nullopt, 1u);
        ::unsetenv("WOLVRIX_GRHSIM_ACTIVE_MASK_GAP_PACK_POLICY");
        if (invalidEnvironmentRun.success || !invalidEnvironmentRun.diagnosticError ||
            invalidEnvironmentRun.diagnostics.find("targeted-table-contiguous") == std::string::npos ||
            invalidEnvironmentRun.diagnostics.find("targeted-table-gap") == std::string::npos)
        {
            return fail("active-mask gap-pack environment policy validation is missing");
        }
        return 0;
    }

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    if (std::getenv("WOLVRIX_TEST_DEFERRED_ACTIVATION_FORWARD") != nullptr)
    {
        return runDeferredActivationForwardFocusedTests();
    }
    if (std::getenv("WOLVRIX_TEST_SAME_BATCH_ACTIVATION_COHORT") != nullptr)
    {
        return runSameBatchActivationCohortFocusedTests();
    }
    if (std::getenv("WOLVRIX_TEST_ACTIVE_MASK_GAP_PACK") != nullptr)
    {
        return runActiveMaskGapPackFocusedTests();
    }
    try
    {
        EmitOptions options;
        options.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR) + "/grhsim_cpp";
        const std::filesystem::path outDir = std::filesystem::path(*options.outputDir);
        std::filesystem::create_directories(outDir);
        const std::filesystem::path wideMemInitPath = outDir / "wide_mem_init.hex";
        {
            std::ofstream initFile(wideMemInitPath);
            if (!initFile.is_open())
            {
                return fail("Failed to create wide memory init file");
            }
            initFile << "0\n";
            initFile << "100000000000000000000000000000001\n";
            initFile << "0\n";
            initFile << "0\n";
        }

        Design design = buildDesign(wideMemInitPath.string());
        SessionStore session;
        if (!runActivitySchedule(design, session))
        {
            return fail("activity-schedule pass failed");
        }

        EmitDiagnostics diag;
        EmitGrhSimCpp emitter(&diag);
        options.session = &session;
        options.sessionPathPrefix = std::string("top");
        options.attributes["sched_batch_max_ops"] = "8";
        options.attributes["sched_batch_max_estimated_lines"] = "96";
        options.attributes["emit_parallelism"] = "2";

        EmitResult result = emitter.emit(design, options);
        if (!result.success)
        {
            return fail("EmitGrhSimCpp failed");
        }
        if (diag.hasError())
        {
            return fail("EmitGrhSimCpp reported diagnostics errors");
        }
        if (result.artifacts.size() < 8)
        {
            return fail("EmitGrhSimCpp should report split state/schedule artifacts");
        }

        const std::filesystem::path headerPath = outDir / "grhsim_top.hpp";
        const std::filesystem::path runtimePath = outDir / "grhsim_top_runtime.hpp";
        const std::filesystem::path statePath = outDir / "grhsim_top_state.cpp";
        const std::filesystem::path evalPath = outDir / "grhsim_top_eval.cpp";
        const std::filesystem::path makefilePath = outDir / "Makefile";
        const std::filesystem::path emitStatsPath = outDir / "grhsim_emit_stats.json";
        const std::vector<std::filesystem::path> stateFiles = collectSchedFiles(outDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> schedFiles = collectSchedFiles(outDir, "grhsim_top_sched_");
        if (!std::filesystem::exists(headerPath) || !std::filesystem::exists(runtimePath) || !std::filesystem::exists(statePath) ||
            !std::filesystem::exists(evalPath) || !std::filesystem::exists(makefilePath) ||
            !std::filesystem::exists(emitStatsPath) || stateFiles.size() < 2 ||
            schedFiles.empty())
        {
            return fail("Expected generated grhsim split state/schedule artifacts to exist");
        }
        if (std::find(result.artifacts.begin(), result.artifacts.end(), emitStatsPath.string()) ==
            result.artifacts.end())
        {
            return fail("EmitGrhSimCpp should report grhsim_emit_stats.json as an artifact");
        }

        const std::string header = readFile(headerPath);
        const std::string runtime = readFile(runtimePath);
        const std::string state = readFiles(stateFiles);
        const std::string eval = readFile(evalPath);
        const std::string makefile = readFile(makefilePath);
        const std::string emitStats = readFile(emitStatsPath);
        const std::string sched = readFiles(schedFiles);

    if (header.find("class GrhSIM_top") == std::string::npos)
    {
        return fail("Missing simulator class declaration");
    }
    if (header.find("kBatchCount = ") == std::string::npos ||
        header.find("void eval_compute_batch_0();") == std::string::npos ||
        header.find("void eval_commit_batch_") == std::string::npos ||
        header.find("struct BatchEvalStats") != std::string::npos)
    {
        return fail("Missing split batch declarations");
    }
    if (header.find("wordchunk_") != std::string::npos)
    {
        return fail("Word helpers should be inlined into eval_batch declarations");
    }
    if (header.find("bool clk = false;") == std::string::npos)
    {
        return fail("Missing public input field declaration");
    }
    if (header.find("struct Inout_pad {") == std::string::npos || header.find("} pad;") == std::string::npos)
    {
        return fail("Missing public inout field declaration");
    }
    if (header.find("void init();") == std::string::npos)
    {
        return fail("Missing explicit init declaration");
    }
    if (header.find("void set_random_seed(std::uint64_t seed);") == std::string::npos)
    {
        return fail("Missing random seed setter declaration");
    }
    if (header.find("#define WOLVRIX_GRHSIM_PERF 0") == std::string::npos)
    {
        return fail("default grhsim emit should define perf feature macro as disabled");
    }
    if (header.find("static constexpr bool kRuntimeProfileCompiled = false;") == std::string::npos ||
        header.find("runtime_profile_active_supernodes_") != std::string::npos ||
        header.find("runtime_profile_fire_compute_") != std::string::npos ||
        sched.find("runtime_profile_enabled_") != std::string::npos ||
        sched.find("runtime_profile_compute_ops_") != std::string::npos ||
        sched.find("runtime_profile_fire_compute_") != std::string::npos)
    {
        return fail("default grhsim emit should omit runtime profile hot-path storage and updates");
    }
    if (header.find("struct PerfCounters") != std::string::npos ||
        header.find("PerfCounters perf_counters() const") != std::string::npos ||
        header.find("void reset_perf_counters()") != std::string::npos)
    {
        return fail("default grhsim emit should not generate perf counter APIs");
    }
    if (sched.find("// op reg_q_write [kRegisterWritePort] reg=reg_q") == std::string::npos ||
        sched.find("// op wide_mem_read [kMemoryReadPort] mem=wide_mem") == std::string::npos ||
        sched.find("// op wide_mem_write [kMemoryWritePort] mem=wide_mem") == std::string::npos ||
        sched.find("// op sum_add [kAdd]") == std::string::npos)
    {
        return fail("schedule comments should include operation kind and storage target annotations");
    }
    const auto writeSnippet = [&](std::string_view opComment)
    {
        const std::size_t begin = sched.find(opComment);
        if (begin == std::string::npos)
        {
            return std::string_view{};
        }
        const std::size_t end = sched.find("// op ", begin + opComment.size());
        return std::string_view(sched).substr(begin, end == std::string::npos ? end : end - begin);
    };
    const std::string_view scalarFullMaskWrite =
        writeSnippet("// op reg_q_write [kRegisterWritePort] reg=reg_q");
    const std::string_view wideFullMaskWrite =
        writeSnippet("// op wide_reg_q_write [kRegisterWritePort] reg=wide_reg_q");
    const std::string_view dynamicMaskWrite =
        writeSnippet("// op masked_reg_q_write [kRegisterWritePort] reg=masked_reg_q");
    const std::string_view wideDynamicMaskWrite =
        writeSnippet("// op wide_masked_reg_q_write [kRegisterWritePort] reg=wide_masked_reg_q");
    if (scalarFullMaskWrite.find("constant all-ones mask: direct register update") == std::string_view::npos ||
        wideFullMaskWrite.find("constant all-ones mask: direct register update") == std::string_view::npos)
    {
        return fail("constant full-mask register writes should bypass masked merge emission");
    }
    if (dynamicMaskWrite.empty() ||
        dynamicMaskWrite.find("constant all-ones mask: direct register update") != std::string_view::npos ||
        dynamicMaskWrite.find(" & ~") == std::string_view::npos)
    {
        return fail("dynamic register masks should retain masked merge emission");
    }
    if (wideDynamicMaskWrite.empty() ||
        wideDynamicMaskWrite.find("constant all-ones mask: direct register update") != std::string_view::npos ||
        wideDynamicMaskWrite.find("grhsim_merge_words_masked") == std::string_view::npos)
    {
        return fail("wide dynamic register masks should retain words masked merge emission");
    }
    if (scalarFullMaskWrite.find("if (unlikely(") == std::string_view::npos ||
        wideFullMaskWrite.find("if (unlikely(") == std::string_view::npos ||
        dynamicMaskWrite.find("if (unlikely(") == std::string_view::npos ||
        wideDynamicMaskWrite.find("if (unlikely(") == std::string_view::npos)
    {
        return fail("commit state-change hint should cover scalar and wide register writes");
    }
    {
        EmitOptions unhintedOptions = options;
        const std::filesystem::path unhintedOutDir =
            std::filesystem::path(std::string(WOLF_SV_EMIT_ARTIFACT_DIR)) / "grhsim_cpp_unhinted_commit";
        unhintedOptions.outputDir = unhintedOutDir.string();
        unhintedOptions.attributes["commit_state_change_unlikely"] = "0";
        EmitDiagnostics unhintedDiag;
        EmitGrhSimCpp unhintedEmitter(&unhintedDiag);
        const EmitResult unhintedResult = unhintedEmitter.emit(design, unhintedOptions);
        if (!unhintedResult.success || unhintedDiag.hasError())
        {
            return fail("unhinted commit EmitGrhSimCpp failed");
        }
        const std::string unhintedSched = readFiles(collectSchedFiles(unhintedOutDir, "grhsim_top_sched_"));
        const std::size_t unhintedWriteBegin =
            unhintedSched.find("// op reg_q_write [kRegisterWritePort] reg=reg_q");
        if (unhintedWriteBegin == std::string::npos)
        {
            return fail("unhinted commit emit should contain the scalar register write");
        }
        const std::size_t unhintedWriteEnd =
            unhintedSched.find("// op ", unhintedWriteBegin + 1);
        const std::string_view unhintedWrite =
            std::string_view(unhintedSched).substr(
                unhintedWriteBegin,
                unhintedWriteEnd == std::string::npos ? unhintedWriteEnd : unhintedWriteEnd - unhintedWriteBegin);
        if (unhintedWrite.find("if (unlikely(") != std::string_view::npos ||
            unhintedWrite.find(" != next_value") == std::string_view::npos)
        {
            return fail("commit state-change hint disable switch should preserve the unhinted condition");
        }
    }
    if (header.find("static const char value_") != std::string::npos ||
        state.find("const char GrhSIM_top::value_") != std::string::npos ||
        sched.find("\"q=%0d\"") == std::string::npos)
    {
        return fail("String constants should emit directly at their use sites");
    }
    if (runtime.find("inline std::uint64_t grhsim_mask") == std::string::npos)
    {
        return fail("Missing runtime helper header");
    }
    if (runtime.find("inline std::uint64_t grhsim_mux_u64") == std::string::npos)
    {
        return fail("Missing branchless scalar mux runtime helper");
    }
    if (runtime.find("grhsim_mux_words") == std::string::npos)
    {
        return fail("Missing branchless words mux runtime helper");
    }
    if (runtime.find("grhsim_concat_uniform_scalars_words") == std::string::npos)
    {
        return fail("Missing scalar concat loop helpers in runtime");
    }
    if (runtime.find("grhsim_pack_bits_u64") != std::string::npos ||
        runtime.find("grhsim_concat_scalars_u64") != std::string::npos ||
        runtime.find("grhsim_concat_uniform_scalars_u64") != std::string::npos ||
        runtime.find("grhsim_replicate_u64") != std::string::npos)
    {
        return fail("runtime should not emit unused small-width loop helpers");
    }
    if (runtime.find("grhsim_concat_words") == std::string::npos ||
        runtime.find("grhsim_replicate_words") == std::string::npos ||
        runtime.find("grhsim_add_words_2") == std::string::npos ||
        runtime.find("grhsim_concat_words_2_1_1") == std::string::npos ||
        runtime.find("grhsim_replicate_words_2_1") == std::string::npos ||
        runtime.find("grhsim_replicate_bit_words") == std::string::npos ||
        runtime.find("grhsim_assign_words_2") == std::string::npos ||
        runtime.find("grhsim_clog2_words") == std::string::npos)
    {
        return fail("Missing pure words runtime helpers");
    }
    if (header.find("std::uint8_t y = ") == std::string::npos)
    {
        return fail("Missing public output field declaration");
    }
    if (header.find("std::uint8_t out = ") == std::string::npos ||
        header.find("bool oe = false;") == std::string::npos)
    {
        return fail("Missing public inout output field declarations");
    }
    if (header.find("std::array<std::uint64_t, 3> wide_y") == std::string::npos)
    {
        return fail("Missing wide output field declaration");
    }
    const bool hasWideStateCommitHelperUsage =
        sched.find("grhsim_assign_words") != std::string::npos ||
        state.find("grhsim_assign_words") != std::string::npos;
    const bool hasWideStateWriteHelperUsage =
        sched.find("grhsim_merge_words_masked") != std::string::npos ||
        state.find("grhsim_merge_words_masked") != std::string::npos;
    if (!hasWideStateCommitHelperUsage || !hasWideStateWriteHelperUsage)
    {
        return fail("Missing wide runtime helper usage");
    }
    if (sched.find("grhsim_concat_uniform_scalars_u64(") != std::string::npos ||
        sched.find("grhsim_concat_scalars_u64(") != std::string::npos)
    {
        return fail("scalar concat with result width <= 64 should emit direct bit expressions instead of u64 concat helpers");
    }
    if (sched.find("grhsim_pack_bits_u64(std::array") != std::string::npos)
    {
        return fail("one-bit scalar concat should emit direct bit expressions instead of grhsim_pack_bits_u64(std::array...)");
    }
    if (sched.find("rep_op") != std::string::npos &&
        sched.find("grhsim_replicate_u64(") != std::string::npos)
    {
        return fail("scalar replicate with result width <= 64 should emit a direct expression instead of grhsim_replicate_u64");
    }
    if (sched.find("scalar_mux_op") == std::string::npos ||
        sched.find("grhsim_mux_u64(") == std::string::npos ||
        sched.find(" ? (") != std::string::npos)
    {
        return fail("scalar mux should emit branchless mask-select helper instead of ?: expressions");
    }
    const bool hasWideSliceHelperCoverage =
        sched.find("wide_slice_static_op") != std::string::npos &&
        sched.find("wide_slice_dyn_op") != std::string::npos &&
        sched.find("grhsim_slice_words<2>(") != std::string::npos &&
        sched.find("grhsim_index_words(wide_addr, 130)") != std::string::npos;
    const bool hasWideHelperCoverage =
        sched.find("grhsim_cast_words<") != std::string::npos &&
        sched.find("grhsim_add_words_2<") != std::string::npos &&
        sched.find("grhsim_concat_words<") != std::string::npos;
    if (sched.find("grhsim_udiv_words") == std::string::npos ||
        sched.find("grhsim_shl_words") == std::string::npos ||
        sched.find("grhsim_mux_words") == std::string::npos ||
        sched.find("grhsim_replicate_words") == std::string::npos ||
        !hasWideSliceHelperCoverage ||
        !hasWideHelperCoverage)
    {
        return fail("Missing emitted pure words wide combinational coverage");
    }
    if (sched.find("([&]()") != std::string::npos)
    {
        return fail("schedule emit should not contain inline lambda word helpers");
    }
    if (eval.find("++perf_counters_.") != std::string::npos)
    {
        return fail("default grhsim eval should not emit perf counter updates");
    }
    if (eval.find("if (((supernode_active_curr_[") != std::string::npos)
    {
        return fail("eval should call batches directly without top-level batch guards");
    }
    if (sched.find("grhsim_clog2_u64") == std::string::npos ||
        sched.find("slice_array_op") == std::string::npos ||
        sched.find("wildcard_eq_op") == std::string::npos)
    {
        return fail("Missing emitted small-width combinational coverage");
    }
    if (sched.find("std::array<std::uint8_t, 9> packed_array_lanes_local_") == std::string::npos ||
        sched.find("packed_array_idx_") == std::string::npos ||
        sched.find("? packed_array_lanes_local_") == std::string::npos ||
        sched.find("slice_array_y = static_cast<std::uint8_t>(grhsim_trunc_u64(scalar_slice_array") != std::string::npos)
    {
        return fail("packed-array kSliceArray should emit lane storage and direct lane lookup");
    }
    if (emitStats.find("\"packed_array_lane_emit\"") == std::string::npos ||
        emitStats.find("\"sv_packed_array_attr_defops\": 1") == std::string::npos ||
        emitStats.find("\"sv_packed_array_attr_concat_defops\": 1") == std::string::npos ||
        emitStats.find("\"packed_array_slice_array_users\": 1") == std::string::npos ||
        emitStats.find("\"packed_array_slice_dynamic_legacy_users\": 0") == std::string::npos ||
        emitStats.find("\"packed_array_lane_emit_values\": 1") == std::string::npos ||
        emitStats.find("\"packed_array_lane_emit_selects\": 1") == std::string::npos)
    {
        return fail("packed-array lane emit stats should report the kSliceArray fast path");
    }
    if (sched.find("grhsim_cast_u64") == std::string::npos ||
        sched.find("grhsim_compare_signed_u64") == std::string::npos ||
        sched.find("grhsim_compare_signed_words") == std::string::npos ||
        sched.find("grhsim_sdiv_words") == std::string::npos)
    {
        return fail("Missing emitted signed combinational helper coverage");
    }
    if (sched.find("grhsim_index_words") == std::string::npos ||
        sched.find("idx_mem_read") == std::string::npos)
    {
        return fail("Missing emitted memory read address handling coverage");
    }
    const bool hasPow2WriteRowAddressCoverage =
        sched.find("grhsim_index_pow2_words") != std::string::npos ||
        (sched.find("wide_masked_mem_write") != std::string::npos &&
         sched.find("[(static_cast<std::size_t>(static_cast<std::uint64_t>(wide_addr)) & 3u)]") != std::string::npos);
    if (!hasPow2WriteRowAddressCoverage)
    {
        return fail("Missing emitted pow2 memory write row addressing coverage");
    }
    if (sched.find("grhsim_apply_masked_words_inplace") == std::string::npos)
    {
        return fail("Missing emitted masked memory write helper usage");
    }
    if (eval.find("while (pending_eval_round)") == std::string::npos ||
        eval.find("Run compute-phase batches in direct schedule order") == std::string::npos ||
        eval.find("this->eval_compute_batch_0();") == std::string::npos ||
        eval.find("this->eval_commit_batch_") == std::string::npos ||
        eval.find("pending_eval_round = commit_activated_readers_ || grhsim_any_active_flags(supernode_active_curr_);") == std::string::npos)
    {
        return fail("Missing compute/commit fixed-point eval loop");
    }
    if (header.find("trace_eval_enabled_") != std::string::npos ||
        state.find("GRHSIM_TRACE_EVAL") != std::string::npos ||
        eval.find("trace_this_eval") != std::string::npos)
    {
        return fail("Perf tracing should be omitted by default");
    }
    if (header.find("static constexpr std::size_t kActiveFlagWordCount = ") == std::string::npos ||
        header.find("std::array<std::uint8_t, kActiveFlagWordCount> supernode_active_curr_{};") == std::string::npos)
    {
        return fail("Missing supernode activity state");
    }
    if (header.find("event_edge_storage_{};") == std::string::npos ||
        header.find("grhsim_event_edge_kind *event_edge_slots_ = nullptr;") == std::string::npos)
    {
        return fail("Missing arena-backed event-edge storage");
    }
    if (sched.find("if ((event_edge_slots_") != std::string::npos ||
        sched.find("if (((event_edge_slots_") != std::string::npos)
    {
        return fail("Exact event predicates should not emit redundant parentheses");
    }
    if (header.find("state_shadow_touched_slots_{};") != std::string::npos ||
        header.find("memory_write_touched_slots_{};") != std::string::npos)
    {
        return fail("Direct-commit emit should not keep shadow/write scratch storage");
    }
    if (header.find("value_u8_slots_") == std::string::npos ||
        header.find("value_words_2_slots_") == std::string::npos ||
        header.find("value_logic_storage_") != std::string::npos ||
        header.find("static constexpr std::size_t kStateLogicStorageBytes = ") == std::string::npos ||
        header.find("state_logic_storage_") == std::string::npos)
    {
        return fail("Typed value storage or packed state storage layout is missing");
    }
    if (header.find("value_bool_slot_ptrs_") != std::string::npos ||
        header.find("state_logic_u8_slot_ptrs_") != std::string::npos ||
        header.find("state_reg_reg_q_") != std::string::npos ||
        header.find("value_35_0_sum_") != std::string::npos ||
        header.find("value_107_0_wide_slice_static_y_") != std::string::npos)
    {
        return fail("Unexpected direct logic members or stale scalar slot pointer tables remain");
    }
    if (header.find("commit_state_updates(") != std::string::npos ||
        state.find("commit_state_updates(") != std::string::npos)
    {
        return fail("Direct-commit emit should not expose legacy commit_state_updates helpers");
    }
    if (header.find("std::array<std::uint8_t, 3> state_mem_idx_mem_") == std::string::npos ||
        header.find("std::array<std::array<std::uint64_t, 3>, 4> state_mem_wide_mem_") == std::string::npos ||
        header.find("std::array<std::array<std::uint64_t, 3>, 4> state_mem_wide_masked_mem_") == std::string::npos)
    {
        return fail("Missing per-memory static storage fields");
    }
    if (runtime.find("struct grhsim_active_mask_entry") == std::string::npos ||
        runtime.find("grhsim_popcount_u8") == std::string::npos ||
        runtime.find("grhsim_count_active_supernodes") == std::string::npos)
    {
        return fail("Missing supernode activity runtime helpers");
    }
    if (runtime.find("for (; index + 32u <= byteCount; index += 32u)") == std::string::npos ||
        runtime.find("(word0 | word1 | word2 | word3) != UINT64_C(0)") == std::string::npos ||
        runtime.find("for (; index + 8u <= byteCount; index += 8u)") == std::string::npos ||
        runtime.find("\"+r\"(word0), \"+r\"(word1), \"+r\"(word2), \"+r\"(word3)") == std::string::npos)
    {
        return fail("Activity pending checks should scan packed words instead of individual bytes");
    }
    if (eval.find("kBatchEvalFns") != std::string::npos ||
        eval.find("kComputeActiveWordBatchOffsets") != std::string::npos ||
        eval.find("kCommitActiveWordBatchOffsets") != std::string::npos ||
        eval.find("(this->*kBatchEvalFns[batchIndex])()") != std::string::npos ||
        eval.find("Direct-dispatch eval") == std::string::npos ||
        eval.find("this->eval_compute_batch_") == std::string::npos ||
        eval.find("this->eval_commit_batch_") == std::string::npos ||
        sched.find("void GrhSIM_top::eval_compute_batch_0()") == std::string::npos ||
        sched.find("BatchEvalStats") != std::string::npos ||
        sched.find("supernode_active_curr_[") == std::string::npos)
    {
        return fail("Missing direct multi-batch eval dispatch");
    }
    if (sched.find("wordchunk_") != std::string::npos)
    {
        return fail("Schedule batches should inline word bodies into eval_batch");
    }
    if (sched.find("activeWordFlags") == std::string::npos || sched.find("supernode_") == std::string::npos)
    {
        return fail("Missing emitted supernode scheduling code");
    }
    if (sched.find("newlyActivatedWordFlags") != std::string::npos)
    {
        return fail("Compute batch emit should keep same-word activations in local activeWordFlags");
    }
    if (sched.find("activeWordFlags & static_cast<std::uint8_t>(~UINT8_C(") == std::string::npos ||
        sched.find("supernode_active_curr_[") == std::string::npos ||
        sched.find(" | activeWordFlags);") == std::string::npos)
    {
        return fail("default compute word dispatch should retain mutable clear/restore protocol");
    }
    if (sched.find("display_task") == std::string::npos || sched.find("trace_dpi_call") == std::string::npos)
    {
        return fail("Missing side-effect op anchors in schedule file");
    }
    if (state.find(wideMemInitPath.string()) != std::string::npos)
    {
        return fail("Generated state file should embed init data instead of reading initFile at runtime");
    }
    if (state.find("k_mem_init_wide_mem_rows") == std::string::npos ||
        state.find("k_mem_init_wide_mem_data") == std::string::npos)
    {
        return fail("Missing embedded memory init data emission");
    }
    if (state.find("random_state_ = random_seed_") == std::string::npos)
    {
        return fail("Missing random seed plumbing in state init");
    }
    if (state.find("std::fill_n(event_edge_slots_, ") == std::string::npos ||
        state.find("state_shadow_touched_slots_ = {};") != std::string::npos ||
        state.find("memory_write_touched_slots_ = {};") != std::string::npos ||
        state.find("state_mem_wide_mem_") == std::string::npos ||
        state.find("state_mem_wide_masked_mem_") == std::string::npos ||
        state.find("state_mem_idx_mem_") == std::string::npos ||
        state.find(" = {};") == std::string::npos)
    {
        return fail("Missing static storage reset emission");
    }
    if (state.find(".assign(") != std::string::npos)
    {
        return fail("Generated state init should not use vector assign for fixed storage");
    }
    if (sched.find("extern \"C\" void trace_sum") == std::string::npos)
    {
        return fail("Missing DPI import declaration");
    }
    if (makefile.find("CXX ?= clang++") == std::string::npos ||
        makefile.find("AR ?= ar") == std::string::npos || makefile.find("all: $(LIB)") == std::string::npos ||
        makefile.find("grhsim_top_state_init_0.cpp") == std::string::npos ||
        makefile.find("grhsim_top_sched_0.cpp") == std::string::npos ||
        makefile.find("PCH_FILE := $(PCH_HEADER).pch") == std::string::npos ||
        makefile.find("-x c++-header") == std::string::npos ||
        makefile.find("-include-pch $(PCH_FILE)") == std::string::npos)
    {
        return fail("Missing split state/schedule Makefile skeleton or PCH support");
    }

    const std::filesystem::path trueMultiWriteDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_reg_to_mem_true_multi_write";
    std::filesystem::remove_all(trueMultiWriteDir);
    Design trueMultiWriteDesign = buildRegToMemTrueMultiWriteEmitDesign();
    if (!runRegToMemTruePass(trueMultiWriteDesign))
    {
        return fail("true reg-to-mem multi-write pass failed");
    }
    EmitDiagnostics trueMultiWriteDiag;
    EmitResult trueMultiWriteResult;
    if (!emitWithActivitySchedule(trueMultiWriteDesign,
                                  trueMultiWriteDir,
                                  trueMultiWriteDiag,
                                  trueMultiWriteResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInComputeSupernode = 8,
                                                          .maxOpInComputeNode = 2,
                                                          .enableCoarsen = false}))
    {
        return fail("true reg-to-mem multi-write activity-schedule pass failed");
    }
    if (!trueMultiWriteResult.success || trueMultiWriteDiag.hasError())
    {
        return fail("true reg-to-mem multi-write emit failed");
    }
    const std::string trueMultiWriteBuildCmd =
        "make -C " + trueMultiWriteDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(trueMultiWriteBuildCmd.c_str()) != 0)
    {
        return fail("true reg-to-mem multi-write archive failed to build");
    }
    const std::filesystem::path trueMultiWriteHarnessPath = trueMultiWriteDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(trueMultiWriteHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create true reg-to-mem multi-write harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.index = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.addr = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.addr2 = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.wen = true;\n";
        harness << "    sim.wen2 = true;\n";
        harness << "    sim.data = static_cast<std::uint8_t>(0x11);\n";
        harness << "    sim.data2 = static_cast<std::uint8_t>(0x22);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x22)) return 1;\n";
        harness << "    sim.index = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.extra != static_cast<std::uint8_t>(0x00)) return 4;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.addr = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.addr2 = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.data = static_cast<std::uint8_t>(0x33);\n";
        harness << "    sim.data2 = static_cast<std::uint8_t>(0x44);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    sim.index = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x33)) return 2;\n";
        harness << "    if (sim.extra != static_cast<std::uint8_t>(0x33)) return 5;\n";
        harness << "    sim.index = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x44)) return 3;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path trueMultiWriteHarnessExe = trueMultiWriteDir / "grhsim_top_harness";
    const std::string trueMultiWriteCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + trueMultiWriteDir.string() +
        " -include-pch " + (trueMultiWriteDir / "grhsim_top.hpp.pch").string() + " " +
        trueMultiWriteHarnessPath.string() + " " + (trueMultiWriteDir / "libgrhsim_top.a").string() +
        " -o " + trueMultiWriteHarnessExe.string();
    if (std::system(trueMultiWriteCompileCmd.c_str()) != 0)
    {
        return fail("true reg-to-mem multi-write harness failed to compile");
    }
    if (std::system(trueMultiWriteHarnessExe.string().c_str()) != 0)
    {
        return fail("true reg-to-mem multi-write collision priority is wrong");
    }

    const std::filesystem::path orderedAffineDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_ordered_memory_write_affine";
    std::filesystem::remove_all(orderedAffineDir);
    Design orderedAffineDesign = buildOrderedMemoryWriteAffineEmitDesign();
    EmitDiagnostics orderedAffineDiag;
    EmitResult orderedAffineResult;
    if (!emitWithActivitySchedule(orderedAffineDesign,
                                  orderedAffineDir,
                                  orderedAffineDiag,
                                  orderedAffineResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInCommitSupernode = 1,
                                                          .enableCoarsen = false,
                                                          .commitGuardEventBuckets = false}))
    {
        return fail("ordered memory write affine activity-schedule pass failed");
    }
    if (!orderedAffineResult.success || orderedAffineDiag.hasError())
    {
        return fail("ordered memory write affine emit failed");
    }
    const std::string orderedAffineSched =
        readFiles(collectSchedFiles(orderedAffineDir, "grhsim_top_sched_"));
    if (orderedAffineSched.find("Ordered memory write affine group: 16 writers in ") ==
            std::string::npos ||
        orderedAffineSched.find("for (std::size_t ordered_write_index_") == std::string::npos)
    {
        return fail("ordered memory write group was not emitted as affine loops");
    }
    if (orderedAffineSched.find("[kMemoryWritePort] mem=ordered_mem") != std::string::npos)
    {
        return fail("ordered memory write affine group retained per-writer commit bodies");
    }
    const std::string orderedAffineBuildCmd =
        "make -C " + orderedAffineDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(orderedAffineBuildCmd.c_str()) != 0)
    {
        return fail("ordered memory write affine archive failed to build");
    }
    const std::filesystem::path orderedAffineHarnessPath = orderedAffineDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(orderedAffineHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create ordered memory write affine harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.read_addr = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.ordered_wen_15 = true;\n";
        harness << "    sim.ordered_addr_15 = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.ordered_data_15 = static_cast<std::uint8_t>(0x11);\n";
        harness << "    sim.ordered_wen_0 = true;\n";
        harness << "    sim.ordered_addr_0 = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.ordered_data_0 = static_cast<std::uint8_t>(0x22);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x22)) return 1;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.ordered_addr_15 = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.ordered_data_15 = static_cast<std::uint8_t>(0x33);\n";
        harness << "    sim.ordered_addr_0 = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.ordered_data_0 = static_cast<std::uint8_t>(0x44);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    sim.read_addr = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x33)) return 2;\n";
        harness << "    sim.read_addr = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected != static_cast<std::uint8_t>(0x44)) return 3;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path orderedAffineHarnessExe = orderedAffineDir / "grhsim_top_harness";
    const std::string orderedAffineCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + orderedAffineDir.string() +
        " -include-pch " + (orderedAffineDir / "grhsim_top.hpp.pch").string() + " " +
        orderedAffineHarnessPath.string() + " " + (orderedAffineDir / "libgrhsim_top.a").string() +
        " -o " + orderedAffineHarnessExe.string();
    if (std::system(orderedAffineCompileCmd.c_str()) != 0)
    {
        return fail("ordered memory write affine harness failed to compile");
    }
    if (std::system(orderedAffineHarnessExe.string().c_str()) != 0)
    {
        return fail("ordered memory write affine priority or row update is wrong");
    }

    const std::filesystem::path rowActivationDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_memory_row_reader_activation";
    std::filesystem::remove_all(rowActivationDir);
    Design rowActivationDesign = buildMemoryRowReaderActivationDesign();
    EmitDiagnostics rowActivationDiag;
    EmitResult rowActivationResult;
    if (!emitWithActivitySchedule(rowActivationDesign,
                                  rowActivationDir,
                                  rowActivationDiag,
                                  rowActivationResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInComputeSupernode = 8,
                                                          .maxOpInComputeNode = 8,
                                                          .enableCoarsen = false}))
    {
        return fail("memory row reader activation activity-schedule pass failed");
    }
    if (!rowActivationResult.success || rowActivationDiag.hasError())
    {
        return fail("memory row reader activation emit failed");
    }
    const std::string rowActivationHeader = readFile(rowActivationDir / "grhsim_top.hpp");
    const std::string rowActivationState = readFile(rowActivationDir / "grhsim_top_state.cpp");
    const std::string rowActivationSched =
        readFiles(collectSchedFiles(rowActivationDir, "grhsim_top_sched_"));
    if (rowActivationHeader.find("void activate_memory_row_readers_0(std::size_t row") == std::string::npos ||
        rowActivationState.find("std::array<std::size_t, 65> kRowOffsets") == std::string::npos ||
        rowActivationState.find("std::array<grhsim_active_mask_entry, 128> kRowReaders") == std::string::npos ||
        rowActivationState.find("entryIndex < kRowOffsets[row + 1u]") == std::string::npos ||
        rowActivationState.find("entry.word_index == localWordIndex") == std::string::npos ||
        rowActivationSched.find("activate_memory_row_readers_0(") == std::string::npos)
    {
        return fail("large constant-address memory should emit row-specialized reader activation");
    }
    const std::string rowActivationBuildCmd =
        "make -C " + rowActivationDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(rowActivationBuildCmd.c_str()) != 0)
    {
        return fail("memory row reader activation archive failed to build");
    }
    const std::filesystem::path rowActivationHarnessPath = rowActivationDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(rowActivationHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create memory row reader activation harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.wen = true;\n";
        harness << "    sim.write_addr = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.write_data = static_cast<std::uint8_t>(0x11);\n";
        harness << "    sim.read_addr = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.row_0 != static_cast<std::uint8_t>(0x11)) return 1;\n";
        harness << "    if (sim.row_mirror_0 != static_cast<std::uint8_t>(0x11)) return 2;\n";
        harness << "    if (sim.dynamic_value != static_cast<std::uint8_t>(0x11)) return 3;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.write_addr = static_cast<std::uint8_t>(33);\n";
        harness << "    sim.write_data = static_cast<std::uint8_t>(0x22);\n";
        harness << "    sim.read_addr = static_cast<std::uint8_t>(33);\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.row_0 != static_cast<std::uint8_t>(0x11)) return 4;\n";
        harness << "    if (sim.row_mirror_0 != static_cast<std::uint8_t>(0x11)) return 5;\n";
        harness << "    if (sim.row_33 != static_cast<std::uint8_t>(0x22)) return 6;\n";
        harness << "    if (sim.row_mirror_33 != static_cast<std::uint8_t>(0x22)) return 7;\n";
        harness << "    if (sim.dynamic_value != static_cast<std::uint8_t>(0x22)) return 8;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path rowActivationHarnessExe = rowActivationDir / "grhsim_top_harness";
    const std::string rowActivationCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + rowActivationDir.string() +
        " -include-pch " + (rowActivationDir / "grhsim_top.hpp.pch").string() + " " +
        rowActivationHarnessPath.string() + " " + (rowActivationDir / "libgrhsim_top.a").string() +
        " -o " + rowActivationHarnessExe.string();
    if (std::system(rowActivationCompileCmd.c_str()) != 0)
    {
        return fail("memory row reader activation harness failed to compile");
    }
    if (std::system(rowActivationHarnessExe.string().c_str()) != 0)
    {
        return fail("memory row reader activation behavior is wrong");
    }

    const std::filesystem::path intentDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_reg_to_mem_intent";
    std::filesystem::remove_all(intentDir);
    std::filesystem::create_directories(intentDir);
    Design intentDesign = buildRegToMemIntentEmitDesign();
    SessionStore intentSession;
    if (!runActivitySchedule(intentDesign, intentSession))
    {
        return fail("reg-to-mem intent activity-schedule pass failed");
    }
    EmitOptions intentOptions;
    intentOptions.outputDir = intentDir.string();
    intentOptions.session = &intentSession;
    intentOptions.sessionPathPrefix = std::string("top");
    intentOptions.attributes["sched_batch_max_ops"] = "8";
    intentOptions.attributes["sched_batch_max_estimated_lines"] = "96";
    intentOptions.attributes["emit_parallelism"] = "2";
    EmitDiagnostics intentDiag;
    EmitGrhSimCpp intentEmitter(&intentDiag);
    const EmitResult intentResult = intentEmitter.emit(intentDesign, intentOptions);
    if (!intentResult.success || intentDiag.hasError())
    {
        return fail("reg-to-mem intent emit failed");
    }
    const std::string intentHeader = readFile(intentDir / "grhsim_top.hpp");
    const std::string intentSched = readFiles(collectSchedFiles(intentDir, "grhsim_top_sched_"));
    if (intentHeader.find("std::array<std::uint8_t, 4> state_reg_to_mem_rtm_pure_") == std::string::npos ||
        intentHeader.find("std::array<std::uint8_t, 4> state_reg_to_mem_rtm_extra_") == std::string::npos)
    {
        return fail("reg-to-mem intent emit should declare shared array storage");
    }
    if (intentSched.find("state_reg_to_mem_rtm_pure_[static_cast<std::size_t>(static_cast<std::uint64_t>(idx))]") ==
            std::string::npos ||
        intentSched.find("state_reg_to_mem_rtm_extra_[static_cast<std::size_t>(static_cast<std::uint64_t>(idx))]") ==
            std::string::npos)
    {
        return fail("reg-to-mem intent slices should emit direct array lookups");
    }
    if (intentSched.find("pure_concat") != std::string::npos ||
        intentSched.find("pure_r0_read_op") != std::string::npos ||
        intentSched.find("pure_r1_read_op") != std::string::npos ||
        intentSched.find("pure_r2_read_op") != std::string::npos ||
        intentSched.find("pure_r3_read_op") != std::string::npos)
    {
        return fail("pure reg-to-mem intent read/concat ops should be bypassed in emit");
    }
    if (intentSched.find("extra_concat") != std::string::npos)
    {
        return fail("reg-to-mem intent concat with only intent-slice users should be bypassed");
    }
    if (intentSched.find("[kRegisterReadPort] reg=extra_r0") == std::string::npos ||
        intentSched.find("extra_extra_assign") == std::string::npos)
    {
        return fail("intent read with an extra user should remain emitted");
    }

    const std::filesystem::path dynamicIntentDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_reg_to_mem_dynamic_input";
    std::filesystem::remove_all(dynamicIntentDir);
    Design dynamicIntentDesign = buildRegToMemDynamicInputIntentEmitDesign();
    EmitDiagnostics dynamicIntentDiag;
    EmitResult dynamicIntentResult;
    if (!emitWithActivitySchedule(dynamicIntentDesign,
                                  dynamicIntentDir,
                                  dynamicIntentDiag,
                                  dynamicIntentResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInComputeSupernode = 8,
                                                          .maxOpInComputeNode = 2,
                                                          .enableCoarsen = false}))
    {
        return fail("dynamic input reg-to-mem intent activity-schedule pass failed");
    }
    if (!dynamicIntentResult.success || dynamicIntentDiag.hasError())
    {
        return fail("dynamic input reg-to-mem intent emit failed");
    }
    const std::string dynamicIntentSched =
        readFiles(collectSchedFiles(dynamicIntentDir, "grhsim_top_sched_"));
    if (dynamicIntentSched.find(
            "state_reg_to_mem_rtm_dyn_input_[static_cast<std::size_t>(static_cast<std::uint64_t>(dyn_idx))]") ==
        std::string::npos)
    {
        return fail("dynamic input reg-to-mem intent should emit direct array lookup by semantic index");
    }
    const std::string dynamicIntentBuildCmd =
        "make -C " + dynamicIntentDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(dynamicIntentBuildCmd.c_str()) != 0)
    {
        return fail("dynamic input reg-to-mem intent archive failed to build");
    }
    const std::filesystem::path dynamicIntentHarnessPath = dynamicIntentDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(dynamicIntentHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create dynamic input reg-to-mem intent harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.dyn_idx = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.dyn_selected != static_cast<std::uint8_t>(0x11)) return 1;\n";
        harness << "    sim.dyn_idx = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.dyn_selected != static_cast<std::uint8_t>(0x33)) return 2;\n";
        harness << "    sim.dyn_idx = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.dyn_selected != static_cast<std::uint8_t>(0x44)) return 3;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path dynamicIntentHarnessExe = dynamicIntentDir / "grhsim_top_harness";
    const std::string dynamicIntentCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + dynamicIntentDir.string() +
        " -include-pch " + (dynamicIntentDir / "grhsim_top.hpp.pch").string() + " " +
        dynamicIntentHarnessPath.string() + " " + (dynamicIntentDir / "libgrhsim_top.a").string() +
        " -o " + dynamicIntentHarnessExe.string();
    if (std::system(dynamicIntentCompileCmd.c_str()) != 0)
    {
        return fail("dynamic input reg-to-mem intent harness failed to compile");
    }
    if (std::system(dynamicIntentHarnessExe.string().c_str()) != 0)
    {
        return fail("dynamic input reg-to-mem intent harness failed to run");
    }

    const std::filesystem::path oneBitIntentDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_reg_to_mem_one_bit_dynamic";
    std::filesystem::remove_all(oneBitIntentDir);
    Design oneBitIntentDesign = buildRegToMemOneBitDynamicIntentEmitDesign();
    if (!runRegToMemIntentPass(oneBitIntentDesign))
    {
        return fail("one-bit dynamic reg-to-mem intent pass failed");
    }
    EmitDiagnostics oneBitIntentDiag;
    EmitResult oneBitIntentResult;
    if (!emitWithActivitySchedule(oneBitIntentDesign,
                                  oneBitIntentDir,
                                  oneBitIntentDiag,
                                  oneBitIntentResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInComputeSupernode = 8,
                                                          .maxOpInComputeNode = 2,
                                                          .enableCoarsen = false}))
    {
        return fail("one-bit dynamic reg-to-mem intent activity-schedule pass failed");
    }
    if (!oneBitIntentResult.success || oneBitIntentDiag.hasError())
    {
        return fail("one-bit dynamic reg-to-mem intent emit failed");
    }
    const std::string oneBitIntentSched =
        readFiles(collectSchedFiles(oneBitIntentDir, "grhsim_top_sched_"));
    if (oneBitIntentSched.find(
            "state_reg_to_mem_rtm_intent_0_[static_cast<std::size_t>(static_cast<std::uint64_t>(bit_idx))]") ==
        std::string::npos)
    {
        return fail("one-bit dynamic reg-to-mem intent should emit direct array lookup by semantic index");
    }
    const std::string oneBitIntentBuildCmd =
        "make -C " + oneBitIntentDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(oneBitIntentBuildCmd.c_str()) != 0)
    {
        return fail("one-bit dynamic reg-to-mem intent archive failed to build");
    }
    const std::filesystem::path oneBitIntentHarnessPath = oneBitIntentDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(oneBitIntentHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create one-bit dynamic reg-to-mem intent harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.bit_idx = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (!sim.bit_selected) return 1;\n";
        harness << "    sim.bit_idx = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.bit_selected) return 2;\n";
        harness << "    sim.bit_idx = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.eval();\n";
        harness << "    if (!sim.bit_selected) return 3;\n";
        harness << "    sim.bit_idx = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.bit_selected) return 4;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path oneBitIntentHarnessExe = oneBitIntentDir / "grhsim_top_harness";
    const std::string oneBitIntentCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + oneBitIntentDir.string() +
        " -include-pch " + (oneBitIntentDir / "grhsim_top.hpp.pch").string() + " " +
        oneBitIntentHarnessPath.string() + " " + (oneBitIntentDir / "libgrhsim_top.a").string() +
        " -o " + oneBitIntentHarnessExe.string();
    if (std::system(oneBitIntentCompileCmd.c_str()) != 0)
    {
        return fail("one-bit dynamic reg-to-mem intent harness failed to compile");
    }
    if (std::system(oneBitIntentHarnessExe.string().c_str()) != 0)
    {
        return fail("one-bit dynamic reg-to-mem intent harness failed to run");
    }

    const std::filesystem::path middleSubsetIntentDir =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_reg_to_mem_middle_subset";
    std::filesystem::remove_all(middleSubsetIntentDir);
    Design middleSubsetIntentDesign = buildRegToMemMiddleSubsetIntentEmitDesign();
    if (!runRegToMemIntentPass(middleSubsetIntentDesign, 2))
    {
        return fail("middle subset reg-to-mem intent pass failed");
    }
    EmitDiagnostics middleSubsetIntentDiag;
    EmitResult middleSubsetIntentResult;
    if (!emitWithActivitySchedule(middleSubsetIntentDesign,
                                  middleSubsetIntentDir,
                                  middleSubsetIntentDiag,
                                  middleSubsetIntentResult,
                                  ActivityScheduleOptions{.path = "top",
                                                          .maxOpInComputeSupernode = 8,
                                                          .maxOpInComputeNode = 2,
                                                          .enableCoarsen = false}))
    {
        return fail("middle subset reg-to-mem intent activity-schedule pass failed");
    }
    if (!middleSubsetIntentResult.success || middleSubsetIntentDiag.hasError())
    {
        return fail("middle subset reg-to-mem intent emit failed");
    }
    const std::string middleSubsetSched =
        readFiles(collectSchedFiles(middleSubsetIntentDir, "grhsim_top_sched_"));
    if (middleSubsetSched.find(
            "state_reg_to_mem_rtm_intent_0_[static_cast<std::size_t>((static_cast<std::uint64_t>(idx_mid) + 1u))]") ==
        std::string::npos)
    {
        return fail("middle subset reg-to-mem intent should emit direct lookup with storage row offset");
    }
    const std::string middleSubsetBuildCmd =
        "make -C " + middleSubsetIntentDir.string() + " CXX=clang++ CXXFLAGS='" +
        std::string(kHarnessCompileFlags) + "'";
    if (std::system(middleSubsetBuildCmd.c_str()) != 0)
    {
        return fail("middle subset reg-to-mem intent archive failed to build");
    }
    const std::filesystem::path middleSubsetHarnessPath = middleSubsetIntentDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(middleSubsetHarnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create middle subset reg-to-mem intent harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <cstdint>\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    sim.init();\n";
        harness << "    sim.idx_full = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.idx_mid = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.en = false;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.data_in = static_cast<std::uint8_t>(0x99);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected_full != static_cast<std::uint8_t>(0x33)) return 1;\n";
        harness << "    if (sim.selected_mid != static_cast<std::uint8_t>(0x33)) return 2;\n";
        harness << "    sim.en = true;\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected_full != static_cast<std::uint8_t>(0x99)) return 3;\n";
        harness << "    if (sim.selected_mid != static_cast<std::uint8_t>(0x99)) return 4;\n";
        harness << "    sim.idx_mid = static_cast<std::uint8_t>(0);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.selected_mid != static_cast<std::uint8_t>(0x22)) return 5;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }
    const std::filesystem::path middleSubsetHarnessExe = middleSubsetIntentDir / "grhsim_top_harness";
    const std::string middleSubsetCompileCmd =
        "clang++ " + std::string(kHarnessCompileFlags) + " -I" + middleSubsetIntentDir.string() +
        " -include-pch " + (middleSubsetIntentDir / "grhsim_top.hpp.pch").string() + " " +
        middleSubsetHarnessPath.string() + " " + (middleSubsetIntentDir / "libgrhsim_top.a").string() +
        " -o " + middleSubsetHarnessExe.string();
    if (std::system(middleSubsetCompileCmd.c_str()) != 0)
    {
        return fail("middle subset reg-to-mem intent harness failed to compile");
    }
    if (std::system(middleSubsetHarnessExe.string().c_str()) != 0)
    {
        return fail("middle subset reg-to-mem intent harness failed to run");
    }

    const std::string buildCmd =
        "make -C " + outDir.string() + " CXX=clang++ CFLAGS='" + std::string(kHarnessCompileFlags) + "'";
    if (std::system(buildCmd.c_str()) != 0)
    {
        return fail("Generated Makefile failed to build grhsim archive");
    }
    if (!std::filesystem::exists(outDir / "libgrhsim_top.a"))
    {
        return fail("Generated grhsim archive missing after make");
    }
    if (!std::filesystem::exists(outDir / "grhsim_top.hpp.pch"))
        {
            return fail("Generated grhsim PCH missing after make");
        }

        const std::filesystem::path harnessPath = outDir / "grhsim_top_harness.cpp";
    {
        std::ofstream harness(harnessPath);
        if (!harness.is_open())
        {
            return fail("Failed to create grhsim harness");
        }
        harness << "#include \"grhsim_top.hpp\"\n";
        harness << "#include <array>\n";
        harness << "#include <cstddef>\n";
        harness << "#include <cstdint>\n";
        harness << "#include <iostream>\n\n";
        harness << "static std::uint8_t g_last_trace = 0;\n";
        harness << "template <std::size_t N>\n";
        harness << "static bool same_words(const std::array<std::uint64_t, N>& lhs,\n";
        harness << "                       const std::array<std::uint64_t, N>& rhs)\n";
        harness << "{\n";
        harness << "    return lhs == rhs;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static bool get_bit(const std::array<std::uint64_t, N>& value, std::size_t index)\n";
        harness << "{\n";
        harness << "    if (index / 64u >= N) return false;\n";
        harness << "    return (value[index / 64u] & (UINT64_C(1) << (index & 63u))) != 0;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static void put_bit(std::array<std::uint64_t, N>& value, std::size_t index, bool bit)\n";
        harness << "{\n";
        harness << "    if (index / 64u >= N) return;\n";
        harness << "    const std::uint64_t mask = UINT64_C(1) << (index & 63u);\n";
        harness << "    if (bit) value[index / 64u] |= mask;\n";
        harness << "    else value[index / 64u] &= ~mask;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static void trunc_words(std::array<std::uint64_t, N>& value, std::size_t width)\n";
        harness << "{\n";
        harness << "    const std::size_t live_words = (width + 63u) / 64u;\n";
        harness << "    for (std::size_t i = live_words; i < N; ++i) value[i] = 0;\n";
        harness << "    if (live_words != 0) {\n";
        harness << "        const std::size_t tail = width - (live_words - 1u) * 64u;\n";
        harness << "        if (tail < 64u) value[live_words - 1u] &= ((UINT64_C(1) << tail) - 1u);\n";
        harness << "    }\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> add_one(std::array<std::uint64_t, N> value, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::uint64_t carry = 1;\n";
        harness << "    for (std::size_t i = 0; i < N && carry != 0; ++i) {\n";
        harness << "        const std::uint64_t next = value[i] + carry;\n";
        harness << "        carry = next < value[i] ? 1 : 0;\n";
        harness << "        value[i] = next;\n";
        harness << "    }\n";
        harness << "    trunc_words(value, width);\n";
        harness << "    return value;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> sub_one(std::array<std::uint64_t, N> value, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::uint64_t borrow = 1;\n";
        harness << "    for (std::size_t i = 0; i < N && borrow != 0; ++i) {\n";
        harness << "        const std::uint64_t prev = value[i];\n";
        harness << "        value[i] = prev - borrow;\n";
        harness << "        borrow = prev < borrow ? 1 : 0;\n";
        harness << "    }\n";
        harness << "    trunc_words(value, width);\n";
        harness << "    return value;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> not_words(std::array<std::uint64_t, N> value, std::size_t width)\n";
        harness << "{\n";
        harness << "    for (auto& word : value) word = ~word;\n";
        harness << "    trunc_words(value, width);\n";
        harness << "    return value;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> mask_merge_words(std::array<std::uint64_t, N> base,\n";
        harness << "                                                const std::array<std::uint64_t, N>& data,\n";
        harness << "                                                const std::array<std::uint64_t, N>& mask,\n";
        harness << "                                                std::size_t width)\n";
        harness << "{\n";
        harness << "    for (std::size_t i = 0; i < N; ++i) base[i] = (base[i] & ~mask[i]) | (data[i] & mask[i]);\n";
        harness << "    trunc_words(base, width);\n";
        harness << "    return base;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> shl_words(const std::array<std::uint64_t, N>& value, std::size_t amount, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, N> out{};\n";
        harness << "    for (std::size_t bit = 0; bit + amount < width; ++bit) if (get_bit(value, bit)) put_bit(out, bit + amount, true);\n";
        harness << "    trunc_words(out, width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> lshr_words(const std::array<std::uint64_t, N>& value, std::size_t amount, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, N> out{};\n";
        harness << "    for (std::size_t bit = amount; bit < width; ++bit) if (get_bit(value, bit)) put_bit(out, bit - amount, true);\n";
        harness << "    trunc_words(out, width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "template <std::size_t DestN, std::size_t SrcN>\n";
        harness << "static std::array<std::uint64_t, DestN> slice_words(const std::array<std::uint64_t, SrcN>& value, std::size_t start, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, DestN> out{};\n";
        harness << "    for (std::size_t bit = 0; bit < width; ++bit) if (get_bit(value, start + bit)) put_bit(out, bit, true);\n";
        harness << "    trunc_words(out, width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static int compare_words(const std::array<std::uint64_t, N>& lhs,\n";
        harness << "                         const std::array<std::uint64_t, N>& rhs)\n";
        harness << "{\n";
        harness << "    for (std::size_t i = N; i-- > 0;) {\n";
        harness << "        if (lhs[i] < rhs[i]) return -1;\n";
        harness << "        if (lhs[i] > rhs[i]) return 1;\n";
        harness << "    }\n";
        harness << "    return 0;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> sub_words(std::array<std::uint64_t, N> lhs,\n";
        harness << "                                               const std::array<std::uint64_t, N>& rhs,\n";
        harness << "                                               std::size_t width)\n";
        harness << "{\n";
        harness << "    std::uint64_t borrow = 0;\n";
        harness << "    for (std::size_t i = 0; i < N; ++i) {\n";
        harness << "        const std::uint64_t rhs_word = rhs[i] + borrow;\n";
        harness << "        borrow = (rhs_word < rhs[i] || lhs[i] < rhs_word) ? 1 : 0;\n";
        harness << "        lhs[i] -= rhs_word;\n";
        harness << "    }\n";
        harness << "    trunc_words(lhs, width);\n";
        harness << "    return lhs;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::size_t highest_bit_words(const std::array<std::uint64_t, N>& value, std::size_t width)\n";
        harness << "{\n";
        harness << "    const std::size_t live_words = (width + 63u) / 64u;\n";
        harness << "    for (std::size_t i = live_words; i-- > 0;) {\n";
        harness << "        const std::size_t word_width = (i + 1u == live_words) ? (width - i * 64u) : 64u;\n";
        harness << "        const std::uint64_t word = word_width < 64u ? (value[i] & ((UINT64_C(1) << word_width) - 1u)) : value[i];\n";
        harness << "        if (word != 0) return i * 64u + (63u - static_cast<std::size_t>(__builtin_clzll(word)));\n";
        harness << "    }\n";
        harness << "    return 0;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> udiv_words_general(std::array<std::uint64_t, N> lhs,\n";
        harness << "                                                       const std::array<std::uint64_t, N>& rhs,\n";
        harness << "                                                       std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, N> quotient{};\n";
        harness << "    if (compare_words(lhs, rhs) < 0) return quotient;\n";
        harness << "    const std::size_t rhs_highest = highest_bit_words(rhs, width);\n";
        harness << "    while (compare_words(lhs, rhs) >= 0) {\n";
        harness << "        std::size_t shift = highest_bit_words(lhs, width) - rhs_highest;\n";
        harness << "        auto shifted = shl_words(rhs, shift, width);\n";
        harness << "        if (compare_words(lhs, shifted) < 0) {\n";
        harness << "            --shift;\n";
        harness << "            shifted = shl_words(rhs, shift, width);\n";
        harness << "        }\n";
        harness << "        lhs = sub_words(lhs, shifted, width);\n";
        harness << "        put_bit(quotient, shift, true);\n";
        harness << "    }\n";
        harness << "    trunc_words(quotient, width);\n";
        harness << "    return quotient;\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> umod_words_general(std::array<std::uint64_t, N> lhs,\n";
        harness << "                                                       const std::array<std::uint64_t, N>& rhs,\n";
        harness << "                                                       std::size_t width)\n";
        harness << "{\n";
        harness << "    if (compare_words(lhs, rhs) < 0) {\n";
        harness << "        trunc_words(lhs, width);\n";
        harness << "        return lhs;\n";
        harness << "    }\n";
        harness << "    const std::size_t rhs_highest = highest_bit_words(rhs, width);\n";
        harness << "    while (compare_words(lhs, rhs) >= 0) {\n";
        harness << "        std::size_t shift = highest_bit_words(lhs, width) - rhs_highest;\n";
        harness << "        auto shifted = shl_words(rhs, shift, width);\n";
        harness << "        if (compare_words(lhs, shifted) < 0) {\n";
        harness << "            --shift;\n";
        harness << "            shifted = shl_words(rhs, shift, width);\n";
        harness << "        }\n";
        harness << "        lhs = sub_words(lhs, shifted, width);\n";
        harness << "    }\n";
        harness << "    trunc_words(lhs, width);\n";
        harness << "    return lhs;\n";
        harness << "}\n\n";
        harness << "template <std::size_t DestN, std::size_t HiN, std::size_t LoN>\n";
        harness << "static std::array<std::uint64_t, DestN> concat_words(const std::array<std::uint64_t, HiN>& hi,\n";
        harness << "                                                      std::size_t hi_width,\n";
        harness << "                                                      const std::array<std::uint64_t, LoN>& lo,\n";
        harness << "                                                      std::size_t lo_width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, DestN> out{};\n";
        harness << "    for (std::size_t bit = 0; bit < lo_width; ++bit) if (get_bit(lo, bit)) put_bit(out, bit, true);\n";
        harness << "    for (std::size_t bit = 0; bit < hi_width; ++bit) if (get_bit(hi, bit)) put_bit(out, lo_width + bit, true);\n";
        harness << "    trunc_words(out, hi_width + lo_width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "template <std::size_t DestN, std::size_t SrcN>\n";
        harness << "static std::array<std::uint64_t, DestN> replicate_words(const std::array<std::uint64_t, SrcN>& value,\n";
        harness << "                                                         std::size_t value_width,\n";
        harness << "                                                         std::size_t rep)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, DestN> out{};\n";
        harness << "    for (std::size_t r = 0; r < rep; ++r)\n";
        harness << "        for (std::size_t bit = 0; bit < value_width; ++bit)\n";
        harness << "            if (get_bit(value, bit)) put_bit(out, r * value_width + bit, true);\n";
        harness << "    trunc_words(out, value_width * rep);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "static std::uint64_t splitmix64_next(std::uint64_t& state)\n";
        harness << "{\n";
        harness << "    std::uint64_t z = (state += UINT64_C(0x9E3779B97F4A7C15));\n";
        harness << "    z = (z ^ (z >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);\n";
        harness << "    z = (z ^ (z >> 27u)) * UINT64_C(0x94D049BB133111EB);\n";
        harness << "    return z ^ (z >> 31u);\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> random_words(std::uint64_t& state, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, N> out{};\n";
        harness << "    for (std::size_t i = 0; i < N; ++i) out[i] = splitmix64_next(state);\n";
        harness << "    trunc_words(out, width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "using u128 = unsigned __int128;\n\n";
        harness << "static u128 to_u128(const std::array<std::uint64_t, 2>& value)\n";
        harness << "{\n";
        harness << "    return static_cast<u128>(value[0]) | (static_cast<u128>(value[1]) << 64u);\n";
        harness << "}\n\n";
        harness << "template <std::size_t N>\n";
        harness << "static std::array<std::uint64_t, N> from_u128(u128 value, std::size_t width)\n";
        harness << "{\n";
        harness << "    std::array<std::uint64_t, N> out{};\n";
        harness << "    if constexpr (N > 0) out[0] = static_cast<std::uint64_t>(value);\n";
        harness << "    if constexpr (N > 1) out[1] = static_cast<std::uint64_t>(value >> 64u);\n";
        harness << "    trunc_words(out, width);\n";
        harness << "    return out;\n";
        harness << "}\n\n";
        harness << "extern \"C\" void trace_sum(std::uint8_t value)\n";
        harness << "{\n";
        harness << "    g_last_trace = value;\n";
        harness << "}\n\n";
        harness << "int main()\n";
        harness << "{\n";
        harness << "    GrhSIM_top sim;\n";
        harness << "    const std::uint64_t seed_a = UINT64_C(0x123456789ABCDEF0);\n";
        harness << "    const std::uint64_t seed_b = UINT64_C(0x0F1E2D3C4B5A6978);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_value_a{UINT64_C(0x0123456789ABCDEF), UINT64_C(0x0FEDCBA987654321), UINT64_C(0x2)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_mask_dyn{UINT64_C(0xFFFF0000FFFF0000), UINT64_C(0x00FF00FF00FF00FF), UINT64_C(0x1)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_mem_init{UINT64_C(1), UINT64_C(0), UINT64_C(1)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_zero{};\n";
        harness << "    const std::array<std::uint64_t, 1> two_bit_one{UINT64_C(1)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_general_divisor{UINT64_C(3), UINT64_C(0x8000000000000000), UINT64_C(0)};\n";
        harness << "    const std::array<std::uint64_t, 2> wide_mem_idx_row2{UINT64_C(2), UINT64_C(0)};\n";
        harness << "    const std::array<std::uint64_t, 2> wide_mem_idx_oor{UINT64_C(0), UINT64_C(1)};\n";
        harness << "    const std::array<std::uint64_t, 2> wide_signed_value{UINT64_C(0xFFFFFFFFFFFFFFFE), UINT64_C(0x1)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_signed_assign_expected{UINT64_C(0xFFFFFFFFFFFFFFFE), UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_signed_div_expected{UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3)};\n";
        harness << "    const std::array<std::uint64_t, 2> mid_value{UINT64_C(0x1122334455667788), UINT64_C(0x0000000012345678)};\n";
        harness << "    const std::array<std::uint64_t, 3> wide_one = add_one(wide_zero, 130);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_two = add_one(wide_one, 130);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_pow65 = shl_words(wide_one, 65, 130);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_div_general_expected = udiv_words_general(wide_value_a, wide_general_divisor, 130);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_mod_general_expected = umod_words_general(wide_value_a, wide_general_divisor, 130);\n";
        harness << "    const std::array<std::uint64_t, 3> wide_masked_expected = mask_merge_words(wide_zero, wide_value_a, wide_mask_dyn, 130);\n";
        harness << "    const u128 mid_rhs_u128 = (static_cast<u128>(UINT64_C(1)) << 64u) | UINT64_C(3);\n";
        harness << "    const std::array<std::uint64_t, 2> mid_mul_expected = from_u128<2>(to_u128(mid_value) * mid_rhs_u128, 96);\n";
        harness << "    const std::array<std::uint64_t, 2> mid_div_expected = from_u128<2>(to_u128(mid_value) / mid_rhs_u128, 96);\n";
        harness << "    const std::array<std::uint64_t, 2> mid_mod_expected = from_u128<2>(to_u128(mid_value) % mid_rhs_u128, 96);\n";
        harness << "    const std::array<std::uint64_t, 2> mid_add_expected = from_u128<2>(to_u128(mid_value) + mid_rhs_u128, 96);\n";
        harness << "    const std::array<std::uint64_t, 2> mid_sub_expected = from_u128<2>(to_u128(mid_value) - mid_rhs_u128, 96);\n";
        harness << "    std::uint64_t random_state_a = seed_a;\n";
        harness << "    const std::uint32_t rand_expected_a = static_cast<std::uint32_t>(splitmix64_next(random_state_a));\n";
        harness << "    const std::array<std::uint64_t, 3> rand_wide_expected_a = random_words<3>(random_state_a, 130);\n";
        harness << "    std::uint64_t random_state_b = seed_b;\n";
        harness << "    const std::uint32_t rand_expected_b = static_cast<std::uint32_t>(splitmix64_next(random_state_b));\n";
        harness << "    const std::array<std::uint64_t, 3> rand_wide_expected_b = random_words<3>(random_state_b, 130);\n";
        harness << "    sim.set_random_seed(seed_a);\n";
        harness << "    sim.init();\n";
        harness << "    sim.en = true;\n";
        harness << "    sim.a = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.comb = static_cast<std::uint8_t>(0xB6);\n";
        harness << "    sim.b = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.sh = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.rep2 = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.sa = static_cast<std::uint8_t>(0xF0);\n";
        harness << "    sim.ss4 = static_cast<std::uint8_t>(0xE);\n";
        harness << "    sim.mid_in = mid_value;\n";
        harness << "    sim.wide_in = wide_value_a;\n";
        harness << "    sim.wide_mask_dyn = wide_mask_dyn;\n";
        harness << "    sim.wide_addr = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_row2;\n";
        harness << "    sim.wide_signed_in = wide_signed_value;\n";
        harness << "    sim.pad.in = static_cast<std::uint8_t>(7);\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(5)) return 1;\n";
        harness << "    if (sim.pad_seen_y != static_cast<std::uint8_t>(10)) return 95;\n";
        harness << "    if (sim.pad.out != static_cast<std::uint8_t>(0xB5)) return 96;\n";
        harness << "    if (!sim.pad.oe) return 97;\n";
        harness << "    if (sim.rand_y != rand_expected_a) return 7;\n";
        harness << "    if (!same_words(sim.rand_wide_y, rand_wide_expected_a)) return 8;\n";
        harness << "    if (sim.mul_y != static_cast<std::uint8_t>(34)) return 11;\n";
        harness << "    if (sim.div_y != static_cast<std::uint8_t>(60)) return 12;\n";
        harness << "    if (sim.mod_y != static_cast<std::uint8_t>(2)) return 13;\n";
        harness << "    if (sim.shl_y != static_cast<std::uint8_t>(0xD8)) return 14;\n";
        harness << "    if (sim.lshr_y != static_cast<std::uint8_t>(0x2D)) return 15;\n";
        harness << "    if (sim.ashr_y != static_cast<std::uint8_t>(0xFC)) return 16;\n";
        harness << "    if (!sim.red_or_y) return 17;\n";
        harness << "    if (!sim.red_xor_y) return 18;\n";
        harness << "    if (sim.slice_y != static_cast<std::uint8_t>(5)) return 19;\n";
        harness << "    if (sim.rep_y != static_cast<std::uint8_t>(0xAA)) return 20;\n";
        harness << "    if (sim.scalar_mux_y != static_cast<std::uint8_t>(0xB6)) return 90;\n";
        harness << "    if (sim.case_eq_y) return 70;\n";
        harness << "    if (!sim.case_ne_y) return 71;\n";
        harness << "    if (!sim.wildcard_eq_y) return 72;\n";
        harness << "    if (!sim.wildcard_ne_y) return 73;\n";
        harness << "    if (sim.slice_array_y != static_cast<std::uint8_t>(0xB6)) return 74;\n";
        harness << "    if (sim.clog2_y != static_cast<std::uint32_t>(8)) return 75;\n";
        harness << "    if (sim.signed_assign_y != static_cast<std::uint8_t>(0xFE)) return 76;\n";
        harness << "    if (sim.signed_add_y != static_cast<std::uint8_t>(0xEE)) return 77;\n";
        harness << "    if (sim.mixed_add_y != static_cast<std::uint8_t>(1)) return 78;\n";
        harness << "    if (sim.signed_div_y != static_cast<std::uint8_t>(0xFF)) return 79;\n";
        harness << "    if (sim.signed_mod_y != static_cast<std::uint8_t>(0xFE)) return 80;\n";
        harness << "    if (!sim.signed_lt_y) return 81;\n";
        harness << "    if (sim.mixed_lt_y) return 82;\n";
        harness << "    if (!same_words(sim.wide_y, wide_two)) return 21;\n";
        harness << "    if (!same_words(sim.wide_mem_y, wide_mem_init)) return 22;\n";
        harness << "    if (!same_words(sim.wide_masked_mem_y, wide_zero)) return 89;\n";
        harness << "    if (sim.idx_mem_y != static_cast<std::uint8_t>(0x33)) return 87;\n";
        harness << "    if (!same_words(sim.wide_add_y, add_one(wide_value_a, 130))) return 31;\n";
        harness << "    if (!same_words(sim.wide_sub_y, sub_one(wide_value_a, 130))) return 32;\n";
        harness << "    if (!same_words(sim.wide_mul_y, shl_words(wide_value_a, 1, 132))) return 33;\n";
        harness << "    if (!same_words(sim.wide_div_y, lshr_words(wide_value_a, 1, 130))) return 34;\n";
        harness << "    if (!same_words(sim.wide_mod_y, wide_one)) return 35;\n";
        harness << "    if (!same_words(sim.wide_mul_pow_y, shl_words(wide_value_a, 65, 130))) return 58;\n";
        harness << "    if (!same_words(sim.wide_div_pow_y, lshr_words(wide_value_a, 65, 130))) return 59;\n";
        harness << "    if (!same_words(sim.wide_mod_pow_y, slice_words<3>(wide_value_a, 0, 65))) return 60;\n";
        harness << "    if (!same_words(sim.wide_div_general_y, wide_div_general_expected)) return 68;\n";
        harness << "    if (!same_words(sim.wide_mod_general_y, wide_mod_general_expected)) return 69;\n";
        harness << "    if (!same_words(sim.mid_mul_y, mid_mul_expected)) return 61;\n";
        harness << "    if (!same_words(sim.mid_div_y, mid_div_expected)) return 62;\n";
        harness << "    if (!same_words(sim.mid_mod_y, mid_mod_expected)) return 63;\n";
        harness << "    if (!same_words(sim.mid_add_y, mid_add_expected)) return 64;\n";
        harness << "    if (!same_words(sim.mid_sub_y, mid_sub_expected)) return 65;\n";
        harness << "    if (sim.mid_eq_y) return 66;\n";
        harness << "    if (sim.mid_lt_y) return 67;\n";
        harness << "    if (!same_words(sim.wide_and_y, wide_value_a)) return 36;\n";
        harness << "    if (!same_words(sim.wide_or_y, wide_value_a)) return 37;\n";
        harness << "    if (!same_words(sim.wide_xor_y, wide_value_a)) return 38;\n";
        harness << "    if (!same_words(sim.wide_xnor_y, wide_value_a)) return 39;\n";
        harness << "    if (!same_words(sim.wide_not_y, not_words(wide_value_a, 130))) return 40;\n";
        harness << "    if (!sim.wide_eq_y) return 41;\n";
        harness << "    if (!sim.wide_lt_y) return 42;\n";
        harness << "    if (!sim.wide_logic_and_y) return 43;\n";
        harness << "    if (!sim.wide_reduce_or_y) return 44;\n";
        harness << "    if (!same_words(sim.wide_shl_y, shl_words(wide_value_a, 1, 130))) return 45;\n";
        harness << "    if (!same_words(sim.wide_lshr_y, lshr_words(wide_value_a, 1, 130))) return 46;\n";
        harness << "    if (!same_words(sim.wide_ashr_y, not_words(wide_zero, 130))) return 47;\n";
        harness << "    if (!same_words(sim.wide_mux_y, wide_value_a)) return 48;\n";
        harness << "    if (!same_words(sim.wide_concat_y, concat_words<3>(two_bit_one, 2, wide_value_a, 130))) return 49;\n";
        harness << "    if (!same_words(sim.wide_rep_y, replicate_words<5>(wide_value_a, 130, 2))) return 50;\n";
        harness << "    if (!same_words(sim.wide_slice_static_y, slice_words<2>(wide_value_a, 5, 65))) return 51;\n";
        harness << "    if (!same_words(sim.wide_slice_dyn_y, slice_words<2>(wide_value_a, 1, 65))) return 52;\n";
        harness << "    if (!same_words(sim.wide_signed_assign_y, wide_signed_assign_expected)) return 83;\n";
        harness << "    if (!same_words(sim.wide_signed_div_y, wide_signed_div_expected)) return 84;\n";
        harness << "    if (!sim.wide_signed_lt_y) return 85;\n";
        harness << "    if (sim.wide_mixed_lt_y) return 86;\n";
        harness << "    sim.pad.in = static_cast<std::uint8_t>(0x10);\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.pad_seen_y != static_cast<std::uint8_t>(0x13)) return 98;\n";
        harness << "    if (sim.pad.out != static_cast<std::uint8_t>(0xB5)) return 99;\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_oor;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.idx_mem_y != static_cast<std::uint8_t>(0x00)) return 88;\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_row2;\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(8)) return 2;\n";
        harness << "    if (g_last_trace != static_cast<std::uint8_t>(5)) return 3;\n";
        harness << "    if (!same_words(sim.wide_y, wide_value_a)) return 23;\n";
        harness << "    if (!same_words(sim.wide_mem_y, wide_value_a)) return 24;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(8)) return 4;\n";
        harness << "    if (!same_words(sim.wide_y, wide_value_a)) return 25;\n";
        harness << "    if (!same_words(sim.wide_mem_y, wide_value_a)) return 26;\n";
        harness << "    if (!same_words(sim.wide_masked_mem_y, wide_masked_expected)) return 90;\n";
        harness << "    if (sim.idx_mem_y != static_cast<std::uint8_t>(0x44)) return 91;\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(11)) return 5;\n";
        harness << "    if (g_last_trace != static_cast<std::uint8_t>(8)) return 6;\n";
        harness << "    if (!same_words(sim.wide_y, wide_value_a)) return 27;\n";
        harness << "    if (!same_words(sim.wide_mem_y, wide_value_a)) return 28;\n";
        harness << "    if (!same_words(sim.wide_masked_mem_y, wide_masked_expected)) return 92;\n";
        harness << "    if (sim.rand_y != rand_expected_a) return 29;\n";
        harness << "    if (!same_words(sim.rand_wide_y, rand_wide_expected_a)) return 30;\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.eval();\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_oor;\n";
        harness << "    sim.clk = true;\n";
        harness << "    sim.eval();\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_row2;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.idx_mem_y != static_cast<std::uint8_t>(0x44)) return 93;\n";
        harness << "    sim.set_random_seed(seed_b);\n";
        harness << "    sim.init();\n";
        harness << "    sim.en = true;\n";
        harness << "    sim.a = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.comb = static_cast<std::uint8_t>(0xB6);\n";
        harness << "    sim.b = static_cast<std::uint8_t>(3);\n";
        harness << "    sim.sh = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.rep2 = static_cast<std::uint8_t>(2);\n";
        harness << "    sim.sa = static_cast<std::uint8_t>(0xF0);\n";
        harness << "    sim.ss4 = static_cast<std::uint8_t>(0xE);\n";
        harness << "    sim.mid_in = mid_value;\n";
        harness << "    sim.wide_in = wide_value_a;\n";
        harness << "    sim.wide_mask_dyn = wide_mask_dyn;\n";
        harness << "    sim.wide_addr = static_cast<std::uint8_t>(1);\n";
        harness << "    sim.wide_mem_idx = wide_mem_idx_row2;\n";
        harness << "    sim.wide_signed_in = wide_signed_value;\n";
        harness << "    sim.pad.in = static_cast<std::uint8_t>(7);\n";
        harness << "    sim.clk = false;\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(5)) return 53;\n";
        harness << "    if (!same_words(sim.wide_y, wide_two)) return 54;\n";
        harness << "    if (!same_words(sim.wide_mem_y, wide_mem_init)) return 55;\n";
        harness << "    if (!same_words(sim.wide_masked_mem_y, wide_zero)) return 94;\n";
        harness << "    if (sim.rand_y != rand_expected_b) return 56;\n";
        harness << "    if (!same_words(sim.rand_wide_y, rand_wide_expected_b)) return 57;\n";
        harness << "    sim.en = true;\n";
        harness << "    sim.a = static_cast<std::uint8_t>(9);\n";
        harness << "    sim.pad.in = static_cast<std::uint8_t>(4);\n";
        harness << "    sim.init();\n";
        harness << "    sim.eval();\n";
        harness << "    if (sim.y != static_cast<std::uint8_t>(2)) return 100;\n";
        harness << "    if (sim.pad_seen_y != static_cast<std::uint8_t>(0)) return 101;\n";
        harness << "    if (sim.pad.out != static_cast<std::uint8_t>(0)) return 102;\n";
        harness << "    if (sim.pad.oe) return 103;\n";
        harness << "    if (sim.rand_y != rand_expected_b) return 104;\n";
        harness << "    return 0;\n";
        harness << "}\n";
    }

        const std::filesystem::path harnessExe = outDir / "grhsim_top_harness";
        std::string compileHarnessCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + outDir.string();
        for (const auto &stateFile : stateFiles)
        {
            compileHarnessCmd += " " + stateFile.string();
        }
        compileHarnessCmd += " " + (outDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : schedFiles)
        {
            compileHarnessCmd += " " + schedPath.string();
        }
        compileHarnessCmd += " " + harnessPath.string() + " -o " + harnessExe.string();
        if (std::system(compileHarnessCmd.c_str()) != 0)
        {
            return fail("Generated grhsim harness failed to compile");
        }

        const std::filesystem::path harnessLog = outDir / "grhsim_top_harness.log";
        const std::string runHarnessCmd = harnessExe.string() + " > " + harnessLog.string() + " 2>&1";
        if (std::system(runHarnessCmd.c_str()) != 0)
        {
            return fail("Generated grhsim harness failed to run");
        }
        const std::string harnessOutput = readFile(harnessLog);
        if (harnessOutput.find("q=5") == std::string::npos)
        {
            return fail("Generated grhsim harness missing system task output");
        }

        const std::filesystem::path regWriteDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_regwrite";
        Design regWriteDesign = buildRegisterWriteInteractionDesign();
        EmitDiagnostics regWriteDiag;
        EmitResult regWriteResult;
        if (!emitWithActivitySchedule(regWriteDesign, regWriteDir, regWriteDiag, regWriteResult))
        {
            return fail("register-write interaction activity-schedule pass failed");
        }
        if (!regWriteResult.success || regWriteDiag.hasError())
        {
            return fail("register-write interaction emit failed");
        }
        const std::filesystem::path regWriteHeaderPath = regWriteDir / "grhsim_top.hpp";
        const std::filesystem::path regWriteStatePath = regWriteDir / "grhsim_top_state.cpp";
        const std::filesystem::path regWriteEvalPath = regWriteDir / "grhsim_top_eval.cpp";
        const std::vector<std::filesystem::path> regWriteStateFiles = collectSchedFiles(regWriteDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> regWriteSchedFiles = collectSchedFiles(regWriteDir, "grhsim_top_sched_");
        if (!std::filesystem::exists(regWriteHeaderPath) || !std::filesystem::exists(regWriteStatePath) ||
            !std::filesystem::exists(regWriteEvalPath) || regWriteStateFiles.empty() || regWriteSchedFiles.empty())
        {
            return fail("register-write interaction artifacts missing");
        }
        if (readFile(regWriteHeaderPath).find("had_register_write_conflict") == std::string::npos)
        {
            return fail("Missing register write conflict getter emission");
        }
        const std::string regWriteHeaderText = readFile(regWriteHeaderPath);
        const std::string regWriteStateText = readFiles(regWriteStateFiles);
        if (regWriteHeaderText.find("event_edge_storage_{};") == std::string::npos ||
            regWriteHeaderText.find("grhsim_event_edge_kind *event_edge_slots_ = nullptr;") == std::string::npos)
        {
            return fail("register-write interaction should emit event-edge storage");
        }
        if (regWriteHeaderText.find("state_shadow_") != std::string::npos)
        {
            return fail("register-write interaction should not keep shared state-shadow fields");
        }
        if (regWriteStateText.find(".assign(") != std::string::npos)
        {
            return fail("register-write interaction should not use vector assign for fixed storage");
        }
        if (regWriteHeaderText.find("seen_evt_") != std::string::npos ||
            regWriteStateText.find("prev_evt_") != std::string::npos)
        {
            return fail("register-write interaction should no longer emit prev/seen event state");
        }

        const std::filesystem::path regWriteHarnessPath = regWriteDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(regWriteHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create register-write harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.rst_n = true;\n";
            harness << "    sim.seq_d = static_cast<std::uint8_t>(0x12);\n";
            harness << "    sim.rst_value = static_cast<std::uint8_t>(0x34);\n";
            harness << "    sim.write_a = static_cast<std::uint8_t>(0x55);\n";
            harness << "    sim.write_b = static_cast<std::uint8_t>(0xAA);\n";
            harness << "    sim.fire_a = false;\n";
            harness << "    sim.fire_b = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.seq_q != static_cast<std::uint8_t>(0x00)) return 1;\n";
            harness << "    if (sim.had_register_write_conflict()) return 2;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.had_register_write_conflict()) return 3;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.seq_q != static_cast<std::uint8_t>(0x12)) return 4;\n";
            harness << "    sim.rst_n = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.had_register_write_conflict()) return 5;\n";
            harness << "    sim.rst_n = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.seq_q != static_cast<std::uint8_t>(0x34)) return 6;\n";
            harness << "    sim.fire_a = true;\n";
            harness << "    sim.fire_b = true;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.had_register_write_conflict()) return 7;\n";
            harness << "    sim.fire_a = false;\n";
            harness << "    sim.fire_b = false;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    const std::uint8_t conflict_q = sim.conflict_q;\n";
            harness << "    if (conflict_q != static_cast<std::uint8_t>(0x55) && conflict_q != static_cast<std::uint8_t>(0xAA)) return 8;\n";
            harness << "    if (sim.had_register_write_conflict()) return 9;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }

        const std::filesystem::path regWriteHarnessExe = regWriteDir / "grhsim_top_harness";
        std::string regWriteCompileCmd = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + regWriteDir.string();
        for (const auto &stateFile : regWriteStateFiles)
        {
            regWriteCompileCmd += " " + stateFile.string();
        }
        regWriteCompileCmd += " " + regWriteEvalPath.string();
        for (const auto &schedPath : regWriteSchedFiles)
        {
            regWriteCompileCmd += " " + schedPath.string();
        }
        regWriteCompileCmd += " " + regWriteHarnessPath.string() + " -o " + regWriteHarnessExe.string();
        if (std::system(regWriteCompileCmd.c_str()) != 0)
        {
            return fail("register-write harness failed to compile");
        }
        if (std::system(regWriteHarnessExe.string().c_str()) != 0)
        {
            return fail("register-write harness failed to run");
        }

        const std::filesystem::path localTempDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_local_temp";
        std::filesystem::remove_all(localTempDir);
        Design localTempDesign = buildLocalTempDesign();
        EmitDiagnostics localTempDiag;
        EmitResult localTempResult;
        if (!emitWithActivitySchedule(localTempDesign, localTempDir, localTempDiag, localTempResult))
        {
            return fail("local-temp activity-schedule pass failed");
        }
        if (!localTempResult.success || localTempDiag.hasError())
        {
            return fail("local-temp emit failed");
        }
        const std::vector<std::filesystem::path> localTempStateFiles =
            collectSchedFiles(localTempDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> localTempSchedFiles =
            collectSchedFiles(localTempDir, "grhsim_top_sched_");
        if (localTempStateFiles.empty() || localTempSchedFiles.empty())
        {
            return fail("local-temp state/schedule files missing");
        }
        const std::string localTempSchedText = readFiles(localTempSchedFiles);
        if (localTempSchedText.find("local_value_") != std::string::npos)
        {
            return fail("cheap single-user scalar locals should inline instead of emitting local_value temps");
        }
        const std::filesystem::path localTempHarnessPath = localTempDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(localTempHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create local-temp harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.a = static_cast<std::uint8_t>(5);\n";
            harness << "    sim.b = static_cast<std::uint8_t>(3);\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.y != static_cast<std::uint8_t>((5 + 3) ^ 3)) return 1;\n";
            harness << "    sim.a = static_cast<std::uint8_t>(10);\n";
            harness << "    sim.b = static_cast<std::uint8_t>(12);\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.y != static_cast<std::uint8_t>((10 + 12) ^ 12)) return 2;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path localTempHarnessExe = localTempDir / "grhsim_top_harness";
        std::string localTempCompileCmd = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + localTempDir.string();
        for (const auto &stateFile : localTempStateFiles)
        {
            localTempCompileCmd += " " + stateFile.string();
        }
        localTempCompileCmd += " " + (localTempDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : localTempSchedFiles)
        {
            localTempCompileCmd += " " + schedPath.string();
        }
        localTempCompileCmd += " " + localTempHarnessPath.string() + " -o " + localTempHarnessExe.string();
        if (std::system(localTempCompileCmd.c_str()) != 0)
        {
            return fail("local-temp harness failed to compile");
        }
        if (std::system(localTempHarnessExe.string().c_str()) != 0)
        {
            return fail("local-temp harness failed to run");
        }

        const std::filesystem::path wideConcatFastDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_wide_concat_fast";
        std::filesystem::remove_all(wideConcatFastDir);
        Design wideConcatFastDesign = buildWideConcatFastPathDesign();
        EmitDiagnostics wideConcatFastDiag;
        EmitResult wideConcatFastResult;
        ActivityScheduleOptions wideConcatFastSchedule;
        wideConcatFastSchedule.maxOpInComputeSupernode = 1;
        wideConcatFastSchedule.maxOpInCommitSupernode = 1;
        if (!emitWithActivitySchedule(wideConcatFastDesign,
                                      wideConcatFastDir,
                                      wideConcatFastDiag,
                                      wideConcatFastResult,
                                      wideConcatFastSchedule))
        {
            return fail("wide-concat-fast activity-schedule pass failed");
        }
        if (!wideConcatFastResult.success || wideConcatFastDiag.hasError())
        {
            return fail("wide-concat-fast emit failed");
        }
        const std::vector<std::filesystem::path> wideConcatFastStateFiles =
            collectSchedFiles(wideConcatFastDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> wideConcatFastSchedFiles =
            collectSchedFiles(wideConcatFastDir, "grhsim_top_sched_");
        if (wideConcatFastStateFiles.empty() || wideConcatFastSchedFiles.empty())
        {
            return fail("wide-concat-fast state/schedule files missing");
        }
        const std::string wideConcatFastSched = readFiles(wideConcatFastSchedFiles);
        if (wideConcatFastSched.find("wide_concat_fast_mid") == std::string::npos ||
            wideConcatFastSched.find("value_words_2_slots_[") ==
                std::string::npos ||
            wideConcatFastSched.find("= std::array<std::uint64_t, 2>{};") == std::string::npos ||
            wideConcatFastSched.find("value_words_2_slots_[") == std::string::npos ||
            wideConcatFastSched.find(")[") == std::string::npos)
        {
            return fail("wide-concat-fast should emit direct concat buffer statements");
        }
        if (wideConcatFastSched.find("grhsim_assign_words(") != std::string::npos)
        {
            return fail("wide-concat-fast should not emit grhsim_assign_words change detection");
        }

        const std::filesystem::path wideConcatFastHarnessPath = wideConcatFastDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(wideConcatFastHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create wide-concat-fast harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <array>\n";
            harness << "#include <cstdint>\n\n";
            harness << "template <std::size_t N>\n";
            harness << "static bool same_words(const std::array<std::uint64_t, N>& lhs,\n";
            harness << "                       const std::array<std::uint64_t, N>& rhs)\n";
            harness << "{\n";
            harness << "    for (std::size_t i = 0; i < N; ++i)\n";
            harness << "        if (lhs[i] != rhs[i]) return false;\n";
            harness << "    return true;\n";
            harness << "}\n\n";
            harness << "template <std::size_t N>\n";
            harness << "static void put_bit(std::array<std::uint64_t, N>& value, std::size_t bit, bool on)\n";
            harness << "{\n";
            harness << "    const std::size_t word = bit / 64u;\n";
            harness << "    const std::size_t shift = bit & 63u;\n";
            harness << "    const std::uint64_t mask = UINT64_C(1) << shift;\n";
            harness << "    if (on) value[word] |= mask;\n";
            harness << "    else value[word] &= ~mask;\n";
            harness << "}\n\n";
            harness << "template <std::size_t N>\n";
            harness << "static bool get_bit(const std::array<std::uint64_t, N>& value, std::size_t bit)\n";
            harness << "{\n";
            harness << "    return ((value[bit / 64u] >> (bit & 63u)) & UINT64_C(1)) != 0;\n";
            harness << "}\n\n";
            harness << "template <std::size_t DestN, std::size_t SrcN>\n";
            harness << "static std::array<std::uint64_t, DestN> slice_words(const std::array<std::uint64_t, SrcN>& src,\n";
            harness << "                                                     std::size_t start,\n";
            harness << "                                                     std::size_t width)\n";
            harness << "{\n";
            harness << "    std::array<std::uint64_t, DestN> out{};\n";
            harness << "    for (std::size_t bit = 0; bit < width; ++bit)\n";
            harness << "        if (get_bit(src, start + bit)) put_bit(out, bit, true);\n";
            harness << "    return out;\n";
            harness << "}\n\n";
            harness << "static std::array<std::uint64_t, 2> repeat_quad_bytes(std::uint8_t a,\n";
            harness << "                                                      std::uint8_t b,\n";
            harness << "                                                      std::uint8_t c,\n";
            harness << "                                                      std::uint8_t d)\n";
            harness << "{\n";
            harness << "    std::array<std::uint64_t, 2> out{};\n";
            harness << "    const std::array<std::uint8_t, 12> bytes{a, b, c, d, a, b, c, d, a, b, c, d};\n";
            harness << "    for (std::size_t i = 0; i < bytes.size(); ++i)\n";
            harness << "        for (std::size_t bit = 0; bit < 8u; ++bit)\n";
            harness << "            if (((bytes[i] >> bit) & UINT8_C(1)) != 0) put_bit(out, (96u - ((i + 1u) * 8u)) + bit, true);\n";
            harness << "    out[1] &= UINT64_C(0xFFFFFFFF);\n";
            harness << "    return out;\n";
            harness << "}\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.a = static_cast<std::uint8_t>(0x11);\n";
            harness << "    sim.b = static_cast<std::uint8_t>(0x22);\n";
            harness << "    sim.c = static_cast<std::uint8_t>(0x33);\n";
            harness << "    sim.d = static_cast<std::uint8_t>(0x44);\n";
            harness << "    sim.eval();\n";
            harness << "    const auto expected_a = repeat_quad_bytes(static_cast<std::uint8_t>(0x11), static_cast<std::uint8_t>(0x22), static_cast<std::uint8_t>(0x33), static_cast<std::uint8_t>(0x44));\n";
            harness << "    if (!same_words(sim.wide_concat_fast_mid, expected_a)) return 1;\n";
            harness << "    if (sim.wide_concat_fast_slice_y != static_cast<std::uint32_t>(slice_words<1>(expected_a, 32u, 32u)[0])) return 2;\n";
            harness << "    sim.a = static_cast<std::uint8_t>(0xAA);\n";
            harness << "    sim.b = static_cast<std::uint8_t>(0xBB);\n";
            harness << "    sim.c = static_cast<std::uint8_t>(0xCC);\n";
            harness << "    sim.d = static_cast<std::uint8_t>(0xDD);\n";
            harness << "    sim.eval();\n";
            harness << "    const auto expected_b = repeat_quad_bytes(static_cast<std::uint8_t>(0xAA), static_cast<std::uint8_t>(0xBB), static_cast<std::uint8_t>(0xCC), static_cast<std::uint8_t>(0xDD));\n";
            harness << "    if (!same_words(sim.wide_concat_fast_mid, expected_b)) return 3;\n";
            harness << "    if (sim.wide_concat_fast_slice_y != static_cast<std::uint32_t>(slice_words<1>(expected_b, 32u, 32u)[0])) return 4;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path wideConcatFastHarnessExe = wideConcatFastDir / "grhsim_top_harness";
        std::string wideConcatFastCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + wideConcatFastDir.string();
        for (const auto &stateFile : wideConcatFastStateFiles)
        {
            wideConcatFastCompileCmd += " " + stateFile.string();
        }
        wideConcatFastCompileCmd += " " + (wideConcatFastDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : wideConcatFastSchedFiles)
        {
            wideConcatFastCompileCmd += " " + schedPath.string();
        }
        wideConcatFastCompileCmd += " " + wideConcatFastHarnessPath.string() + " -o " + wideConcatFastHarnessExe.string();
        if (std::system(wideConcatFastCompileCmd.c_str()) != 0)
        {
            return fail("wide-concat-fast harness failed to compile");
        }
        if (std::system(wideConcatFastHarnessExe.string().c_str()) != 0)
        {
            return fail("wide-concat-fast harness failed to run");
        }

        const std::filesystem::path packedActivationDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_packed_activation";
        std::filesystem::remove_all(packedActivationDir);
        Design packedActivationDesign = buildPackedActivationDesign();
        EmitDiagnostics packedActivationDiag;
        EmitResult packedActivationResult;
        ActivityScheduleOptions packedActivationSchedule;
        packedActivationSchedule.maxOpInComputeSupernode = 1;
        packedActivationSchedule.enableCoarsen = false;
        if (!emitWithActivitySchedule(packedActivationDesign,
                                      packedActivationDir,
                                      packedActivationDiag,
                                      packedActivationResult,
                                      packedActivationSchedule))
        {
            return fail("packed-activation activity-schedule pass failed");
        }
        if (!packedActivationResult.success || packedActivationDiag.hasError())
        {
            return fail("packed-activation emit failed");
        }
        const std::vector<std::filesystem::path> packedActivationSchedFiles =
            collectSchedFiles(packedActivationDir, "grhsim_top_sched_");
        if (packedActivationSchedFiles.empty())
        {
            return fail("packed-activation schedule files missing");
        }
        const std::string packedActivationEval = readFile(packedActivationDir / "grhsim_top_eval.cpp");
        if (packedActivationEval.find("grhsim_or_active_u16(supernode_active_curr_.data()") == std::string::npos)
        {
            return fail("packed-activation fanout should emit packed active flag ORs");
        }

        const std::filesystem::path fullWordConsumeDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_full_active_word_consume";
        std::filesystem::remove_all(fullWordConsumeDir);
        Design fullWordConsumeDesign = buildFullActiveWordConsumeDesign();
        EmitDiagnostics fullWordConsumeDiag;
        EmitResult fullWordConsumeResult;
        ActivityScheduleOptions fullWordConsumeSchedule;
        fullWordConsumeSchedule.maxOpInComputeSupernode = 1;
        fullWordConsumeSchedule.enableCoarsen = false;
        fullWordConsumeSchedule.splitOversizeComputeNodes = true;
        fullWordConsumeSchedule.splitOversizeComputeNodeMaxOps = 1;
        if (!emitWithActivitySchedule(fullWordConsumeDesign,
                                      fullWordConsumeDir,
                                      fullWordConsumeDiag,
                                      fullWordConsumeResult,
                                      fullWordConsumeSchedule,
                                      false,
                                      false,
                                      false,
                                      false,
                                      false,
                                      true,
                                      true))
        {
            return fail("full-active-word-consume activity-schedule pass failed");
        }
        if (!fullWordConsumeResult.success || fullWordConsumeDiag.hasError())
        {
            return fail("full-active-word-consume emit failed");
        }
        const std::vector<std::filesystem::path> fullWordConsumeStateFiles =
            collectSchedFiles(fullWordConsumeDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> fullWordConsumeSchedFiles =
            collectSchedFiles(fullWordConsumeDir, "grhsim_top_sched_");
        if (fullWordConsumeStateFiles.empty() || fullWordConsumeSchedFiles.empty())
        {
            return fail("full-active-word-consume generated files missing");
        }
        const std::string fullWordConsumeSched = readFiles(fullWordConsumeSchedFiles);
        const std::string dispatchMarker =
            "constexpr std::uint8_t dispatchMask = UINT8_C(";
        std::string_view fullMaskBlock;
        std::string_view partialMaskBlock;
        std::size_t dispatchSearch = 0;
        while ((dispatchSearch = fullWordConsumeSched.find(dispatchMarker, dispatchSearch)) != std::string::npos)
        {
            const std::size_t maskBegin = dispatchSearch + dispatchMarker.size();
            const std::size_t maskEnd = fullWordConsumeSched.find(");", maskBegin);
            if (maskEnd == std::string::npos)
            {
                return fail("full-active-word-consume emitted a malformed dispatch mask");
            }
            const unsigned mask = static_cast<unsigned>(
                std::stoul(fullWordConsumeSched.substr(maskBegin, maskEnd - maskBegin)));
            const std::size_t blockEnd = fullWordConsumeSched.find(dispatchMarker, maskEnd);
            const std::string_view block = std::string_view(fullWordConsumeSched).substr(
                dispatchSearch,
                blockEnd == std::string::npos ? blockEnd : blockEnd - dispatchSearch);
            if (mask == 255u && block.find("activeWordFlags |=") != std::string_view::npos)
            {
                fullMaskBlock = block;
            }
            else if (mask != 255u && partialMaskBlock.empty())
            {
                partialMaskBlock = block;
            }
            dispatchSearch = maskEnd;
        }
        if (fullMaskBlock.empty())
        {
            return fail("full-active-word-consume should emit a complete word with forward activation");
        }
        if (fullMaskBlock.find("activeWordFlags |=") == std::string_view::npos ||
            fullMaskBlock.find("activeWordFlags & static_cast<std::uint8_t>(~UINT8_C(") !=
                std::string_view::npos ||
            fullMaskBlock.find(" | activeWordFlags);") != std::string_view::npos)
        {
            return fail("complete compute words should consume forward local activations without clear/restore");
        }
        if (partialMaskBlock.empty())
        {
            return fail("full-active-word-consume should retain a partial boundary word");
        }
        if (partialMaskBlock.find("activeWordFlags & static_cast<std::uint8_t>(~UINT8_C(") ==
                std::string_view::npos ||
            partialMaskBlock.find(" | activeWordFlags);") == std::string_view::npos)
        {
            return fail("partial compute words should retain mutable clear/restore protocol");
        }

        const std::filesystem::path fullWordConsumeHarnessPath =
            fullWordConsumeDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(fullWordConsumeHarnessPath);
            if (!harness.is_open())
            {
                return fail("failed to create full-active-word-consume harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.chain_in = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.chain_out != static_cast<std::uint8_t>(10)) return 1;\n";
            harness << "    sim.chain_in = static_cast<std::uint8_t>(7);\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.chain_out != static_cast<std::uint8_t>(16)) return 2;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path fullWordConsumeHarnessExe =
            fullWordConsumeDir / "grhsim_top_harness";
        std::string fullWordConsumeCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + fullWordConsumeDir.string();
        for (const auto &stateFile : fullWordConsumeStateFiles)
        {
            fullWordConsumeCompileCmd += " " + stateFile.string();
        }
        fullWordConsumeCompileCmd += " " + (fullWordConsumeDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : fullWordConsumeSchedFiles)
        {
            fullWordConsumeCompileCmd += " " + schedPath.string();
        }
        fullWordConsumeCompileCmd +=
            " " + fullWordConsumeHarnessPath.string() + " -o " + fullWordConsumeHarnessExe.string();
        if (std::system(fullWordConsumeCompileCmd.c_str()) != 0)
        {
            return fail("full-active-word-consume harness failed to compile");
        }
        if (std::system(fullWordConsumeHarnessExe.string().c_str()) != 0)
        {
            return fail("full-active-word-consume harness failed to run");
        }

        const std::filesystem::path overlappingActivationDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_overlapping_activation";
        std::filesystem::remove_all(overlappingActivationDir);
        Design overlappingActivationDesign = buildOverlappingActivationDesign();
        EmitDiagnostics overlappingActivationDiag;
        EmitResult overlappingActivationResult;
        ActivityScheduleOptions overlappingActivationSchedule;
        overlappingActivationSchedule.maxOpInComputeSupernode = 1;
        overlappingActivationSchedule.enableCoarsen = false;
        if (!emitWithActivitySchedule(overlappingActivationDesign,
                                      overlappingActivationDir,
                                      overlappingActivationDiag,
                                      overlappingActivationResult,
                                      overlappingActivationSchedule))
        {
            return fail("overlapping-activation activity-schedule pass failed");
        }
        if (!overlappingActivationResult.success || overlappingActivationDiag.hasError())
        {
            return fail("overlapping-activation emit failed");
        }
        const std::vector<std::filesystem::path> overlappingActivationSchedFiles =
            collectSchedFiles(overlappingActivationDir, "grhsim_top_sched_");
        if (overlappingActivationSchedFiles.empty())
        {
            return fail("overlapping-activation schedule files missing");
        }
        const std::string overlappingActivationSched = readFiles(overlappingActivationSchedFiles);
        if (overlappingActivationSched.find("grhsim_any_changed_") == std::string::npos)
        {
            return fail("overlapping activation should emit a deferred changed-value group");
        }
        if (countSubstring(overlappingActivationSched, "grhsim_any_changed_0_0 = static_cast<bool>") != 2)
        {
            return fail("overlapping activation should merge both changed values into one deferred condition");
        }
        if (countSubstring(overlappingActivationSched, "activeWordFlags |= static_cast<std::uint8_t>") != 1)
        {
            return fail("overlapping activation should activate the shared successor once");
        }

        const std::filesystem::path twoWordDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_two_word_helpers";
        std::filesystem::remove_all(twoWordDir);
        Design twoWordDesign = buildTwoWordHelperDesign();
        EmitDiagnostics twoWordDiag;
        EmitResult twoWordResult;
        if (!emitWithActivitySchedule(twoWordDesign, twoWordDir, twoWordDiag, twoWordResult))
        {
            return fail("two-word-helper activity-schedule pass failed");
        }
        if (!twoWordResult.success || twoWordDiag.hasError())
        {
            return fail("two-word-helper emit failed");
        }
        const std::vector<std::filesystem::path> twoWordStateFiles =
            collectSchedFiles(twoWordDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> twoWordSchedFiles =
            collectSchedFiles(twoWordDir, "grhsim_top_sched_");
        if (twoWordStateFiles.empty() || twoWordSchedFiles.empty())
        {
            return fail("two-word-helper state/schedule files missing");
        }
        const std::string twoWordSched = readFiles(twoWordSchedFiles);
        if (twoWordSched.find("grhsim_concat_words_2_1_1<") == std::string::npos ||
            twoWordSched.find("grhsim_concat_words_2_1_2<") == std::string::npos ||
            twoWordSched.find("grhsim_replicate_words_2_1<") == std::string::npos ||
            twoWordSched.find("grhsim_replicate_bit_words<2, 65>") == std::string::npos ||
            twoWordSched.find("grhsim_replicate_bit_words<4, 256>") == std::string::npos ||
            twoWordSched.find("grhsim_replicate_words<4>") != std::string::npos ||
            twoWordSched.find("grhsim_assign_words_2<") == std::string::npos)
        {
            return fail("two-word-helper should instantiate fixed two-word helpers");
        }
        const std::filesystem::path twoWordHarnessPath = twoWordDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(twoWordHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create two-word-helper harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <array>\n";
            harness << "#include <cstdint>\n\n";
            harness << "template <std::size_t N>\n";
            harness << "static bool same_words(const std::array<std::uint64_t, N>& lhs,\n";
            harness << "                       const std::array<std::uint64_t, N>& rhs)\n";
            harness << "{\n";
            harness << "    for (std::size_t i = 0; i < N; ++i)\n";
            harness << "        if (lhs[i] != rhs[i]) return false;\n";
            harness << "    return true;\n";
            harness << "}\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.a64 = UINT64_C(0x1122334455667788);\n";
            harness << "    sim.b64 = UINT64_C(0x99AABBCCDDEEFF00);\n";
            harness << "    sim.wide96 = std::array<std::uint64_t, 2>{UINT64_C(0x0123456789ABCDEF), UINT64_C(0x0000000012345678)};\n";
            harness << "    sim.tag8 = static_cast<std::uint8_t>(0x5A);\n";
            harness << "    sim.broadcast_bit = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!same_words(sim.concat128, std::array<std::uint64_t, 2>{UINT64_C(0x99AABBCCDDEEFF00), UINT64_C(0x1122334455667788)})) return 1;\n";
            harness << "    if (!same_words(sim.concat104, std::array<std::uint64_t, 2>{UINT64_C(0x0123456789ABCDEF), UINT64_C(0x0000005A12345678)})) return 2;\n";
            harness << "    if (!same_words(sim.rep96, std::array<std::uint64_t, 2>{UINT64_C(0x5A5A5A5A5A5A5A5A), UINT64_C(0x000000005A5A5A5A)})) return 3;\n";
            harness << "    if (!same_words(sim.rep65, std::array<std::uint64_t, 2>{UINT64_MAX, UINT64_C(1)})) return 4;\n";
            harness << "    if (!same_words(sim.rep256, std::array<std::uint64_t, 4>{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX})) return 5;\n";
            harness << "    if (!same_words(sim.reg_q, std::array<std::uint64_t, 2>{UINT64_C(0), UINT64_C(0)})) return 6;\n";
            harness << "    sim.broadcast_bit = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!same_words(sim.rep65, std::array<std::uint64_t, 2>{UINT64_C(0), UINT64_C(0)})) return 7;\n";
            harness << "    if (!same_words(sim.rep256, std::array<std::uint64_t, 4>{UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)})) return 8;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!same_words(sim.reg_q, std::array<std::uint64_t, 2>{UINT64_C(0x0123456789ABCDEF), UINT64_C(0x0000000012345678)})) return 9;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.wide96 = std::array<std::uint64_t, 2>{UINT64_C(0xFEEDFACECAFEBEEF), UINT64_C(0x00000000ABCDEF01)};\n";
            harness << "    sim.eval();\n";
            harness << "    if (!same_words(sim.reg_q, std::array<std::uint64_t, 2>{UINT64_C(0x0123456789ABCDEF), UINT64_C(0x0000000012345678)})) return 10;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!same_words(sim.reg_q, std::array<std::uint64_t, 2>{UINT64_C(0xFEEDFACECAFEBEEF), UINT64_C(0x00000000ABCDEF01)})) return 11;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path twoWordHarnessExe = twoWordDir / "grhsim_top_harness";
        std::string twoWordCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + twoWordDir.string();
        for (const auto &stateFile : twoWordStateFiles)
        {
            twoWordCompileCmd += " " + stateFile.string();
        }
        twoWordCompileCmd += " " + (twoWordDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : twoWordSchedFiles)
        {
            twoWordCompileCmd += " " + schedPath.string();
        }
        twoWordCompileCmd += " " + twoWordHarnessPath.string() + " -o " + twoWordHarnessExe.string();
        if (std::system(twoWordCompileCmd.c_str()) != 0)
        {
            return fail("two-word-helper harness failed to compile");
        }
        if (std::system(twoWordHarnessExe.string().c_str()) != 0)
        {
            return fail("two-word-helper harness failed to run");
        }

        const std::filesystem::path commitBatchDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_commit_cond_batch";
        std::filesystem::remove_all(commitBatchDir);
        Design commitBatchDesign = buildCommitCondBatchDesign();
        EmitDiagnostics commitBatchDiag;
        EmitResult commitBatchResult;
        if (!emitWithActivitySchedule(commitBatchDesign,
                                      commitBatchDir,
                                      commitBatchDiag,
                                      commitBatchResult,
                                      {},
                                      true,
                                      true,
                                      false,
                                      false,
                                      false,
                                      false,
                                      true,
                                      true))
        {
            return fail("commit-cond-batch activity-schedule pass failed");
        }
        if (!commitBatchResult.success || commitBatchDiag.hasError())
        {
            return fail("commit-cond-batch emit failed");
        }
        const std::string commitBatchHeader = readFile(commitBatchDir / "grhsim_top.hpp");
        const std::string commitBatchRuntime = readFile(commitBatchDir / "grhsim_top_runtime.hpp");
        const std::string commitBatchEval = readFile(commitBatchDir / "grhsim_top_eval.cpp");
        const std::string commitBatchSched =
            readFiles(collectSchedFiles(commitBatchDir, "grhsim_top_sched_"));
        if (commitBatchHeader.find("std::uint32_t condIndex = 0;") != std::string::npos ||
            commitBatchHeader.find("std::uint32_t condBase = 0;") != std::string::npos ||
            commitBatchRuntime.find("struct grhsim_active_mask_entry") == std::string::npos ||
            countSubstring(commitBatchSched, "if (event_edge_slots_[0] == grhsim_event_edge_kind::posedge)") != 1)
        {
            return fail("commit-cond-batch should share one exact-event guard without legacy cond descriptors");
        }
        if (countSubstring(commitBatchSched, "apply_commit_scalar_state_write_table(") != 0)
        {
            return fail("commit-cond-batch should not fall back to legacy commit tables");
        }
        if (commitBatchSched.find("// Pure-event compute word: an event miss consumes the cleared word.") !=
                std::string::npos ||
            commitBatchHeader.find("kPureEventComputeWordEligibleCount = 0u") == std::string::npos)
        {
            return fail("commit words must not use the pure-event compute-word bypass");
        }
        const std::size_t eventFastPathBegin = commitBatchEval.find("    if (event_fullpass_candidate) {");
        const std::size_t normalPathBegin = commitBatchEval.find("    while (pending_eval_round) {", eventFastPathBegin);
        if (eventFastPathBegin == std::string::npos || normalPathBegin == std::string::npos)
        {
            return fail("commit-cond-batch should emit the posedge event fast path");
        }
        const std::string eventFastPath =
            commitBatchEval.substr(eventFastPathBegin, normalPathBegin - eventFastPathBegin);
        if (eventFastPath.find("Preserve the commit reader frontier for adaptive post-commit settling") ==
                std::string::npos ||
            eventFastPath.find("post_commit_active_count * 4u <=") == std::string::npos ||
            eventFastPath.find("while (grhsim_any_active_flags(supernode_active_curr_))") == std::string::npos ||
            eventFastPath.find("_fullpass();") == std::string::npos ||
            eventFastPath.find("++perf_counters_.eventFastPathCount;") == std::string::npos ||
            eventFastPath.find("++perf_counters_.eventSparseSettleCount;") == std::string::npos ||
            eventFastPath.find("++perf_counters_.eventDenseSettleCount;") == std::string::npos)
        {
            return fail("commit-cond-batch event fast path should select sparse active settle or dense fullpass");
        }
        if (commitBatchHeader.find("eventPostCommitActiveSum") == std::string::npos ||
            commitBatchHeader.find("eventPostCommitActiveMin") == std::string::npos ||
            commitBatchHeader.find("eventPostCommitActiveMax") == std::string::npos)
        {
            return fail("commit-cond-batch perf counters should expose event settle density");
        }
        if (countSubstring(commitBatchSched, "Commit writes update visible state directly") != 4 ||
            commitBatchSched.find("batch_reg0_write") == std::string::npos ||
            commitBatchSched.find("batch_reg1_write") == std::string::npos ||
            commitBatchSched.find("batch_reg2_write") == std::string::npos ||
            commitBatchSched.find("batch_reg3_write") == std::string::npos)
        {
            return fail("commit-cond-batch should keep direct per-write commit bodies under the shared event guard");
        }
        const std::size_t reg0WritePos = commitBatchSched.find("batch_reg0_write");
        const std::size_t reg1WritePos = commitBatchSched.find("batch_reg1_write");
        const std::size_t reg2WritePos = commitBatchSched.find("batch_reg2_write");
        if (reg0WritePos == std::string::npos ||
            reg1WritePos == std::string::npos ||
            reg2WritePos == std::string::npos ||
            reg0WritePos > reg2WritePos)
        {
            return fail("commit-cond-batch shared guard fixture should emit reg0 before reg2");
        }
        const std::size_t sharedGuardPos = commitBatchSched.rfind("if (", reg0WritePos);
        const std::size_t sharedGuardOpenBrace =
            sharedGuardPos == std::string::npos ? std::string::npos : commitBatchSched.find('{', sharedGuardPos);
        const std::size_t sharedGuardCloseBrace = findMatchingBrace(commitBatchSched, sharedGuardOpenBrace);
        if (sharedGuardPos == std::string::npos ||
            sharedGuardOpenBrace == std::string::npos ||
            sharedGuardCloseBrace == std::string::npos ||
            sharedGuardOpenBrace > reg0WritePos ||
            sharedGuardCloseBrace < reg2WritePos ||
            (sharedGuardOpenBrace < reg1WritePos && reg1WritePos < sharedGuardCloseBrace))
        {
            return fail("commit-cond-batch should bucket non-adjacent reg0/reg2 writes under one guard");
        }
        if (commitBatchSched.find("if ((event_edge_slots_") != std::string::npos ||
            commitBatchSched.find("if (((event_edge_slots_") != std::string::npos)
        {
            return fail("commit-cond-batch should not emit redundant event parentheses");
        }

        const std::filesystem::path commitBatchHarnessPath = commitBatchDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(commitBatchHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create commit-cond-batch harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.fire0 = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.fire1 = static_cast<std::uint8_t>(0);\n";
            harness << "    sim.fire2 = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.fire3 = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.d0 = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.d1 = static_cast<std::uint8_t>(2);\n";
            harness << "    sim.d2 = static_cast<std::uint8_t>(4);\n";
            harness << "    sim.d3 = static_cast<std::uint8_t>(8);\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.y != static_cast<std::uint8_t>(13)) {\n";
            harness << "        return 1;\n";
            harness << "    }\n";
            harness << "    sim.fire0 = static_cast<std::uint8_t>(0);\n";
            harness << "    sim.fire1 = static_cast<std::uint8_t>(1);\n";
            harness << "    sim.fire2 = static_cast<std::uint8_t>(0);\n";
            harness << "    sim.fire3 = static_cast<std::uint8_t>(0);\n";
            harness << "    sim.d1 = static_cast<std::uint8_t>(16);\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.y != static_cast<std::uint8_t>(29)) {\n";
            harness << "        return 2;\n";
            harness << "    }\n";
            harness << "    const auto counters = sim.perf_counters();\n";
            harness << "    if (counters.eventFastPathCount != UINT64_C(2) ||\n";
            harness << "        counters.eventStateChangedCount != UINT64_C(2) ||\n";
            harness << "        counters.eventSparseSettleCount + counters.eventDenseSettleCount !=\n";
            harness << "            counters.eventStateChangedCount ||\n";
            harness << "        counters.eventPostCommitActiveSum == UINT64_C(0) ||\n";
            harness << "        counters.eventPostCommitActiveMin == ~UINT64_C(0) ||\n";
            harness << "        counters.eventPostCommitActiveMax < counters.eventPostCommitActiveMin) {\n";
            harness << "        return 3;\n";
            harness << "    }\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::vector<std::filesystem::path> commitBatchStateFiles =
            collectSchedFiles(commitBatchDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> commitBatchSchedFiles =
            collectSchedFiles(commitBatchDir, "grhsim_top_sched_");
        const std::filesystem::path commitBatchHarnessExe = commitBatchDir / "grhsim_top_harness";
        std::string commitBatchCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + commitBatchDir.string();
        for (const auto &stateFile : commitBatchStateFiles)
        {
            commitBatchCompileCmd += " " + stateFile.string();
        }
        commitBatchCompileCmd += " " + (commitBatchDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : commitBatchSchedFiles)
        {
            commitBatchCompileCmd += " " + schedPath.string();
        }
        commitBatchCompileCmd += " " + commitBatchHarnessPath.string() + " -o " + commitBatchHarnessExe.string();
        if (std::system(commitBatchCompileCmd.c_str()) != 0)
        {
            return fail("commit-cond-batch harness failed to compile");
        }
        if (std::system(commitBatchHarnessExe.string().c_str()) != 0)
        {
            return fail("commit-cond-batch harness failed to run");
        }

        const auto commitLocalityScheduleOptions = [](std::size_t commitCap)
        {
            ActivityScheduleOptions options;
            options.path = "top";
            options.maxOpInComputeSupernode = 1;
            options.maxOpInCommitSupernode = commitCap;
            options.enableCoarsen = false;
            return options;
        };
        Design commitLocalitySeedDesign = buildCommitLocalityPartitionDesign();
        SessionStore commitLocalitySeedSession;
        if (!runActivitySchedule(commitLocalitySeedDesign,
                                 commitLocalitySeedSession,
                                 commitLocalityScheduleOptions(1)))
        {
            return fail("commit-locality split seed schedule failed");
        }
        const auto *commitLocalitySeed =
            sessionValue<ActivityScheduleCommitLocalityGroupByOp>(
                commitLocalitySeedSession,
                "top.activity_schedule.commit_locality_group_by_op");
        const auto *commitLocalityOrderSeed =
            sessionValue<ActivityScheduleCommitLocalityGroupOrder>(
                commitLocalitySeedSession,
                "top.activity_schedule.commit_locality_group_order");
        if (commitLocalitySeed == nullptr || commitLocalityOrderSeed == nullptr)
        {
            return fail("commit-locality split seed metadata is missing");
        }
        std::vector<uint32_t> seedGroups;
        for (const uint32_t group : *commitLocalitySeed)
        {
            if (group != kInvalidActivitySupernodeId)
            {
                seedGroups.push_back(group);
            }
        }
        std::sort(seedGroups.begin(), seedGroups.end());
        seedGroups.erase(std::unique(seedGroups.begin(), seedGroups.end()), seedGroups.end());
        if (seedGroups != std::vector<uint32_t>{0, 1})
        {
            return fail("commit-locality split seed should contain two canonical groups");
        }
        if (commitLocalityOrderSeed->size() != seedGroups.size())
        {
            return fail("commit-locality split seed order should cover both canonical groups");
        }

        ActivityScheduleCommitLocalityGroupByOp malformedCommitLocality = *commitLocalitySeed;
        const auto malformedEntry =
            std::find_if(malformedCommitLocality.begin(),
                         malformedCommitLocality.end(),
                         [](uint32_t group)
                         {
                             return group != kInvalidActivitySupernodeId;
                         });
        if (malformedEntry == malformedCommitLocality.end())
        {
            return fail("commit-locality malformed fixture has no commit entry");
        }
        *malformedEntry = kInvalidActivitySupernodeId;
        ActivityScheduleCommitLocalityGroupByOp highCommitLocality =
            *commitLocalitySeed;
        const auto highEntry =
            std::find_if(highCommitLocality.begin(),
                         highCommitLocality.end(),
                         [](uint32_t group)
                         {
                             return group != kInvalidActivitySupernodeId;
                         });
        if (highEntry == highCommitLocality.end())
        {
            return fail("commit-locality high-group fixture has no commit entry");
        }
        *highEntry = std::numeric_limits<uint32_t>::max() - 1U;
        ActivityScheduleCommitLocalityGroupOrder malformedCommitLocalityOrder =
            *commitLocalityOrderSeed;
        if (malformedCommitLocalityOrder.size() < 2)
        {
            return fail("commit-locality malformed order fixture needs two groups");
        }
        malformedCommitLocalityOrder[1] = malformedCommitLocalityOrder[0];

        struct CommitLocalityEmitCase
        {
            std::string_view name;
            std::size_t commitCap = 1;
            bool removeMetadata = false;
            const ActivityScheduleCommitLocalityGroupByOp *metadataOverride = nullptr;
            const ActivityScheduleCommitLocalityGroupOrder *orderOverride = nullptr;
        };
        const std::array<CommitLocalityEmitCase, 8> commitLocalityCases = {{
            {"split", 1, false, nullptr, nullptr},
            {"split_fallback", 1, true, nullptr, nullptr},
            {"merged_stable", 2, false, commitLocalitySeed, commitLocalityOrderSeed},
            {"merged_native", 2, false, nullptr, nullptr},
            {"merged_fallback", 2, true, nullptr, nullptr},
            {"merged_malformed_map", 2, false, &malformedCommitLocality, nullptr},
            {"merged_high_group", 2, false, &highCommitLocality, nullptr},
            {"merged_malformed_order", 2, false, commitLocalitySeed,
             &malformedCommitLocalityOrder},
        }};
        const std::filesystem::path commitLocalityBaseDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
            "grhsim_cpp_commit_locality_partition";
        std::filesystem::remove_all(commitLocalityBaseDir);
        std::map<std::string, std::filesystem::path> commitLocalityDirs;
        for (const auto &testCase : commitLocalityCases)
        {
            const std::filesystem::path outDir =
                commitLocalityBaseDir / std::string(testCase.name);
            Design caseDesign = buildCommitLocalityPartitionDesign();
            EmitDiagnostics caseDiag;
            EmitResult caseResult;
            if (!emitCommitLocalityCase(caseDesign,
                                        outDir,
                                        commitLocalityScheduleOptions(testCase.commitCap),
                                        testCase.removeMetadata,
                                        testCase.metadataOverride,
                                        testCase.orderOverride,
                                        caseDiag,
                                        caseResult) ||
                !caseResult.success || caseDiag.hasError())
            {
                return fail("commit-locality emit failed for " + std::string(testCase.name));
            }
            commitLocalityDirs.emplace(testCase.name, outDir);
        }

        const auto slotMapping = [&](std::string_view name)
        {
            const auto &dir = commitLocalityDirs.at(std::string(name));
            return extractValueSlotMapping(
                readFiles(collectSchedFiles(dir, "grhsim_top_sched_")));
        };
        const auto slotDeclarations = [&](std::string_view name)
        {
            const auto &dir = commitLocalityDirs.at(std::string(name));
            return extractValueSlotDeclarations(readFile(dir / "grhsim_top.hpp"));
        };
        const auto splitSlotMapping = slotMapping("split");
        const auto stableSlotMapping = slotMapping("merged_stable");
        const auto nativeSlotMapping = slotMapping("merged_native");
        const auto fallbackSlotMapping = slotMapping("merged_fallback");
        const auto malformedMapSlotMapping = slotMapping("merged_malformed_map");
        const auto highGroupSlotMapping = slotMapping("merged_high_group");
        const auto malformedOrderSlotMapping = slotMapping("merged_malformed_order");
        if (splitSlotMapping.empty() || splitSlotMapping != stableSlotMapping ||
            slotDeclarations("split") != slotDeclarations("merged_stable"))
        {
            return fail("canonical commit locality groups should stabilize typed value slots across partitioning");
        }
        if (nativeSlotMapping != fallbackSlotMapping ||
            fallbackSlotMapping != malformedMapSlotMapping ||
            fallbackSlotMapping != highGroupSlotMapping ||
            fallbackSlotMapping != malformedOrderSlotMapping)
        {
            return fail("missing or malformed commit locality metadata should use the legacy value order");
        }
        if (splitSlotMapping == fallbackSlotMapping)
        {
            return fail("commit-locality fixture did not distinguish split and merged legacy anchors");
        }
        const auto nativeSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("merged_native"), "grhsim_top");
        const auto fallbackSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("merged_fallback"), "grhsim_top");
        const auto malformedMapSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("merged_malformed_map"), "grhsim_top");
        const auto highGroupSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("merged_high_group"), "grhsim_top");
        const auto malformedOrderSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("merged_malformed_order"), "grhsim_top");
        if (nativeSources.empty() || nativeSources != fallbackSources ||
            fallbackSources != malformedMapSources ||
            fallbackSources != highGroupSources ||
            fallbackSources != malformedOrderSources)
        {
            return fail("native commit locality metadata must preserve default generated source identity");
        }
        const auto splitSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("split"), "grhsim_top");
        const auto splitFallbackSources =
            collectGeneratedSourceFiles(commitLocalityDirs.at("split_fallback"), "grhsim_top");
        if (splitSources.empty() || splitSources != splitFallbackSources)
        {
            return fail("split canonical metadata must preserve legacy generated source identity");
        }

        const std::filesystem::path oneBitBaseDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_one_bit_bitwise";
        const std::array<std::string, 3> oneBitVariantNames = {"default", "zero", "enabled"};
        std::filesystem::remove_all(oneBitBaseDir);
        Design oneBitDesign = buildOneBitBitwiseDesign();
        SessionStore oneBitSession;
        if (!runActivitySchedule(oneBitDesign, oneBitSession))
        {
            return fail("one-bit-bitwise activity-schedule pass failed");
        }
        std::array<std::filesystem::path, 3> oneBitDirs;
        std::array<std::string, 3> oneBitCombinedSources;
        std::array<std::string, 3> oneBitRuntimeSources;
        std::array<std::string, 3> oneBitSchedSources;
        for (std::size_t variant = 0; variant < oneBitVariantNames.size(); ++variant)
        {
            oneBitDirs[variant] = oneBitBaseDir / oneBitVariantNames[variant];
            std::filesystem::create_directories(oneBitDirs[variant]);
            EmitOptions oneBitOptions;
            oneBitOptions.outputDir = oneBitDirs[variant].string();
            oneBitOptions.session = &oneBitSession;
            oneBitOptions.sessionPathPrefix = std::string("top");
            oneBitOptions.attributes["emit_parallelism"] = "2";
            oneBitOptions.attributes["direct_single_writer_state_reads"] = "1";
            if (variant == 1)
            {
                oneBitOptions.attributes["one_bit_bitwise_bytes"] = "0";
            }
            else if (variant == 2)
            {
                oneBitOptions.attributes["one_bit_bitwise_bytes"] = "1";
            }
            EmitDiagnostics oneBitDiag;
            EmitGrhSimCpp oneBitEmitter(&oneBitDiag);
            const EmitResult oneBitResult = oneBitEmitter.emit(oneBitDesign, oneBitOptions);
            if (!oneBitResult.success || oneBitDiag.hasError())
            {
                return fail("one-bit-bitwise emit failed for " + oneBitVariantNames[variant]);
            }
            const std::vector<std::filesystem::path> variantStateFiles =
                collectSchedFiles(oneBitDirs[variant], "grhsim_top_state");
            const std::vector<std::filesystem::path> variantSchedFiles =
                collectSchedFiles(oneBitDirs[variant], "grhsim_top_sched_");
            if (variantStateFiles.empty() || variantSchedFiles.empty())
            {
                return fail("one-bit-bitwise generated files missing");
            }
            oneBitRuntimeSources[variant] = readFile(oneBitDirs[variant] / "grhsim_top_runtime.hpp");
            oneBitSchedSources[variant] = readFiles(variantSchedFiles);
            oneBitCombinedSources[variant] = readFile(oneBitDirs[variant] / "grhsim_top.hpp") +
                                             oneBitRuntimeSources[variant] +
                                             readFiles(variantStateFiles) +
                                             readFile(oneBitDirs[variant] / "grhsim_top_eval.cpp") +
                                             oneBitSchedSources[variant];
        }
        if (oneBitCombinedSources[0] != oneBitCombinedSources[1])
        {
            return fail("one-bit-bitwise default and explicit zero output should match");
        }
        if (oneBitRuntimeSources[0].find("grhsim_assume_bit_u8") != std::string::npos ||
            oneBitSchedSources[0].find("const std::uint8_t next_value = grhsim_assume_bit_u8") !=
                std::string::npos)
        {
            return fail("one-bit-bitwise default output should stay disabled");
        }
        if (oneBitRuntimeSources[2].find("GRHSIM_ALWAYS_INLINE std::uint8_t grhsim_assume_bit_u8") ==
                std::string::npos ||
            oneBitSchedSources[2].find("const std::uint8_t next_value =") == std::string::npos ||
            oneBitSchedSources[2].find("grhsim_assume_bit_u8") == std::string::npos)
        {
            return fail("one-bit-bitwise enabled output should use assumed byte expressions");
        }
        if (oneBitSchedSources[2].find("const bool next_value = a ^ c;") == std::string::npos ||
            oneBitSchedSources[2].find("const bool next_value = (~(b ^ d)) & UINT64_C(1);") ==
                std::string::npos ||
            oneBitSchedSources[2].find("const bool next_value = (~(e)) & UINT64_C(1);") == std::string::npos)
        {
            return fail("one-bit-bitwise should not alter xor, xnor, or not");
        }
        if (oneBitSchedSources[2].find("const bool next_value = (a) && (d);") == std::string::npos)
        {
            return fail("one-bit-bitwise should not alter kLogicAnd");
        }
        const std::filesystem::path oneBitEnabledDir = oneBitDirs[2];
        const std::filesystem::path oneBitHarnessPath = oneBitEnabledDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(oneBitHarnessPath);
            if (!harness.is_open())
            {
                return fail("failed to create one-bit-bitwise harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    for (unsigned bits = 0; bits < 32; ++bits) {\n";
            harness << "        sim.a = (bits & 1u) != 0;\n";
            harness << "        sim.b = (bits & 2u) != 0;\n";
            harness << "        sim.c = (bits & 4u) != 0;\n";
            harness << "        sim.d = (bits & 8u) != 0;\n";
            harness << "        sim.e = (bits & 16u) != 0;\n";
            harness << "        sim.wide_a = static_cast<std::uint8_t>(bits * 7u);\n";
            harness << "        sim.wide_b = static_cast<std::uint8_t>(0xA5u ^ bits);\n";
            harness << "        sim.eval();\n";
            harness << "        if (sim.and_y != (sim.a & sim.b)) return 1;\n";
            harness << "        if (sim.or_y != (sim.c | sim.d)) return 2;\n";
            harness << "        if (sim.xor_y != (sim.a ^ sim.c)) return 3;\n";
            harness << "        if (sim.xnor_y != !(sim.b ^ sim.d)) return 4;\n";
            harness << "        if (sim.not_y != !sim.e) return 5;\n";
            harness << "        if (sim.chain_y != ((sim.c | sim.d) & sim.e)) return 6;\n";
            harness << "        if (sim.logic_and_y != (sim.a && sim.d)) return 7;\n";
            harness << "        if (sim.wide_and_y != static_cast<std::uint8_t>(sim.wide_a & sim.wide_b)) return 8;\n";
            harness << "    }\n";
            harness << "    sim.a = true;\n";
            harness << "    sim.b = true;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!sim.state_and_y) return 9;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.a = false;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.state_and_y) return 10;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::vector<std::filesystem::path> oneBitStateFiles =
            collectSchedFiles(oneBitEnabledDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> oneBitSchedFiles =
            collectSchedFiles(oneBitEnabledDir, "grhsim_top_sched_");
        const std::filesystem::path oneBitHarnessExe = oneBitEnabledDir / "grhsim_top_harness";
        std::string oneBitCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + oneBitEnabledDir.string();
        for (const auto &stateFile : oneBitStateFiles)
        {
            oneBitCompileCmd += " " + stateFile.string();
        }
        oneBitCompileCmd += " " + (oneBitEnabledDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedPath : oneBitSchedFiles)
        {
            oneBitCompileCmd += " " + schedPath.string();
        }
        oneBitCompileCmd += " " + oneBitHarnessPath.string() + " -o " + oneBitHarnessExe.string();
        if (std::system(oneBitCompileCmd.c_str()) != 0)
        {
            return fail("one-bit-bitwise harness failed to compile");
        }
        if (std::system(oneBitHarnessExe.string().c_str()) != 0)
        {
            return fail("one-bit-bitwise harness failed to run");
        }

        const std::filesystem::path gatedDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_gated_clock";
        std::filesystem::remove_all(gatedDir);
        Design gatedDesign = buildGatedClockDesign();
        SessionStore gatedSession;
        if (!runActivitySchedule(gatedDesign, gatedSession))
        {
            return fail("gated-clock activity-schedule pass failed");
        }
        std::filesystem::create_directories(gatedDir);
        EmitOptions gatedOptions;
        gatedOptions.outputDir = gatedDir.string();
        gatedOptions.session = &gatedSession;
        gatedOptions.sessionPathPrefix = std::string("top");
        gatedOptions.attributes["sched_batch_max_ops"] = "8";
        gatedOptions.attributes["sched_batch_max_estimated_lines"] = "96";
        gatedOptions.attributes["emit_parallelism"] = "2";
        EmitDiagnostics gatedDiag;
        EmitGrhSimCpp gatedEmitter(&gatedDiag);
        EmitResult gatedResult = gatedEmitter.emit(gatedDesign, gatedOptions);
        if (!gatedResult.success || gatedDiag.hasError())
        {
            return fail("gated-clock emit failed");
        }
        const std::filesystem::path gatedStatePath = gatedDir / "grhsim_top_state.cpp";
        const std::filesystem::path gatedEvalPath = gatedDir / "grhsim_top_eval.cpp";
        const std::vector<std::filesystem::path> gatedStateFiles = collectSchedFiles(gatedDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> gatedSchedFiles =
            collectSchedFiles(gatedDir, "grhsim_top_sched_");
        if (gatedStateFiles.empty() || gatedSchedFiles.empty())
        {
            return fail("gated-clock state/schedule files missing");
        }
        const std::string gatedSchedText = readFiles(gatedSchedFiles);
        const std::string gatedStateText = readFiles(gatedStateFiles);
        const std::string gatedEvalText = readFile(gatedEvalPath);
        const std::string gatedHeaderText = readFile(gatedDir / "grhsim_top.hpp");
        if (gatedHeaderText.find("event_edge_storage_{};") == std::string::npos ||
            gatedHeaderText.find("grhsim_event_edge_kind *event_edge_slots_ = nullptr;") == std::string::npos)
        {
            return fail("gated-clock emit should provide event-edge storage");
        }
        if (gatedStateText.find(".assign(") != std::string::npos)
        {
            return fail("gated-clock emit should not use vector assign for fixed storage");
        }
        if (gatedSchedText.find("grhsim_event_edge_kind::posedge") == std::string::npos)
        {
            return fail("gated-clock exact event logic should consume shared event-edge enums");
        }
        if (gatedSchedText.find("if ((event_edge_slots_") != std::string::npos ||
            gatedSchedText.find("if (((event_edge_slots_") != std::string::npos)
        {
            return fail("gated-clock exact event logic should not emit redundant parentheses");
        }
        if (gatedEvalText.find("while (pending_eval_round)") == std::string::npos ||
            gatedEvalText.find("Run compute-phase batches in direct schedule order") == std::string::npos ||
            gatedEvalText.find("this->eval_compute_batch_0();") == std::string::npos ||
            gatedEvalText.find("this->eval_commit_batch_") == std::string::npos ||
            gatedEvalText.find("pending_eval_round = commit_activated_readers_ || grhsim_any_active_flags(supernode_active_curr_);") == std::string::npos)
        {
            return fail("gated-clock eval should iterate until compute/commit reaches a fixed point");
        }
        if (gatedEvalText.find("grhsim_classify_edge(") == std::string::npos ||
            gatedEvalText.find("event_edge_slots_") == std::string::npos)
        {
            return fail("gated-clock eval should seed and clear event-edge state");
        }
        if (gatedSchedText.find("seen_evt_") != std::string::npos ||
            gatedEvalText.find("prev_evt_") != std::string::npos)
        {
            return fail("gated-clock emit should not keep the old prev/seen event state");
        }
        const std::filesystem::path gatedHarnessPath = gatedDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(gatedHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create gated-clock harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <array>\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    const std::array<std::uint64_t, 3> gate_magic{UINT64_C(1), UINT64_C(0), UINT64_C(2)};\n";
            harness << "    const std::array<std::uint64_t, 3> gate_zero{};\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.aux_clk = false;\n";
            harness << "    sim.data = static_cast<std::uint8_t>(0x5A);\n";
            harness << "    sim.gate_in = gate_magic;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gate_match) return 1;\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0x00)) return 2;\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x00)) return 13;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!sim.gate_match) return 3;\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0x5A)) return 4;\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x00)) return 14;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!sim.gate_match) return 5;\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0x5A)) return 6;\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x00)) return 15;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (!sim.gate_match) return 7;\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0x5A)) return 8;\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x00)) return 16;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0x5A)) return 9;\n";
            harness << "    sim.aux_clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.aux_clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x5A)) return 17;\n";
            harness << "    sim.gate_in = gate_zero;\n";
            harness << "    sim.data = static_cast<std::uint8_t>(0xA5);\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0xA5)) return 10;\n";
            harness << "    sim.aux_clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.aux_clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x5A)) return 18;\n";
            harness << "    if (sim.gate_match) return 11;\n";
            harness << "    sim.data = static_cast<std::uint8_t>(0x3C);\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_q != static_cast<std::uint8_t>(0xA5)) return 12;\n";
            harness << "    sim.aux_clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.aux_clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.gated_aux_q != static_cast<std::uint8_t>(0x5A)) return 19;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path gatedHarnessExe = gatedDir / "grhsim_top_harness";
        std::string gatedCompileCmd = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + gatedDir.string();
        for (const auto &stateFile : gatedStateFiles)
        {
            gatedCompileCmd += " " + stateFile.string();
        }
        gatedCompileCmd += " " + gatedEvalPath.string();
        for (const auto &schedPath : gatedSchedFiles)
        {
            gatedCompileCmd += " " + schedPath.string();
        }
        gatedCompileCmd += " " + gatedHarnessPath.string() + " -o " + gatedHarnessExe.string();
        if (std::system(gatedCompileCmd.c_str()) != 0)
        {
            return fail("gated-clock harness failed to compile");
        }
        if (std::system(gatedHarnessExe.string().c_str()) != 0)
        {
            return fail("gated-clock harness failed to run");
        }

        const std::filesystem::path scalarLocalityCoarsenedDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_scalar_locality_coarsened";
        std::filesystem::remove_all(scalarLocalityCoarsenedDir);
        Design scalarLocalityCoarsenedDesign = buildMaterializedScalarReadLocalityDesign();
        EmitDiagnostics scalarLocalityCoarsenedDiag;
        EmitResult scalarLocalityCoarsenedResult;
        if (!emitWithActivitySchedule(scalarLocalityCoarsenedDesign,
                                      scalarLocalityCoarsenedDir,
                                      scalarLocalityCoarsenedDiag,
                                      scalarLocalityCoarsenedResult,
                                      {},
                                      false,
                                      false,
                                      false,
                                      false,
                                      true) ||
            !scalarLocalityCoarsenedResult.success || scalarLocalityCoarsenedDiag.hasError())
        {
            return fail("coarsened scalar read-locality diagnostic emit failed");
        }
        std::istringstream scalarLocalityCoarsenedStream(readFile(
            scalarLocalityCoarsenedDir / "grhsim_materialized_scalar_read_locality.tsv"));
        std::string scalarLocalityCoarsenedLine;
        if (!std::getline(scalarLocalityCoarsenedStream, scalarLocalityCoarsenedLine))
        {
            return fail("coarsened scalar read-locality TSV is missing");
        }
        const std::vector<std::string_view> scalarLocalityCoarsenedHeader = splitTabs(scalarLocalityCoarsenedLine);
        const auto coarsenedColumnIndex = [&](std::string_view name) {
            const auto it = std::find(scalarLocalityCoarsenedHeader.begin(), scalarLocalityCoarsenedHeader.end(), name);
            return it == scalarLocalityCoarsenedHeader.end()
                       ? std::numeric_limits<std::size_t>::max()
                       : static_cast<std::size_t>(std::distance(scalarLocalityCoarsenedHeader.begin(), it));
        };
        const std::size_t coarsenedNameColumn = coarsenedColumnIndex("value_name");
        const std::size_t coarsenedTouchesColumn = coarsenedColumnIndex("operand_touches");
        const std::size_t coarsenedWritesColumn = coarsenedColumnIndex("result_writes");
        const std::size_t coarsenedCandidateColumn = coarsenedColumnIndex("candidate");
        const std::size_t coarsenedSavedColumn = coarsenedColumnIndex("loads_saved_per_fire");
        if (coarsenedNameColumn == std::numeric_limits<std::size_t>::max() ||
            coarsenedTouchesColumn == std::numeric_limits<std::size_t>::max() ||
            coarsenedWritesColumn == std::numeric_limits<std::size_t>::max() ||
            coarsenedCandidateColumn == std::numeric_limits<std::size_t>::max() ||
            coarsenedSavedColumn == std::numeric_limits<std::size_t>::max())
        {
            return fail("coarsened scalar read-locality TSV schema is incomplete");
        }
        bool foundCoarsenedRepeatedSource = false;
        while (std::getline(scalarLocalityCoarsenedStream, scalarLocalityCoarsenedLine))
        {
            const std::vector<std::string_view> fields = splitTabs(scalarLocalityCoarsenedLine);
            if (fields.size() != scalarLocalityCoarsenedHeader.size())
            {
                return fail("coarsened scalar read-locality TSV row width mismatch");
            }
            if (fields[coarsenedNameColumn] != "locality_repeated_source")
            {
                continue;
            }
            foundCoarsenedRepeatedSource = true;
            if (fields[coarsenedTouchesColumn] != "2" || fields[coarsenedWritesColumn] != "1" ||
                fields[coarsenedCandidateColumn] != "0" || fields[coarsenedSavedColumn] != "0")
            {
                return fail("same-supernode scalar writes must be reported as ineligible");
            }
        }
        if (!foundCoarsenedRepeatedSource)
        {
            return fail("coarsened scalar read-locality TSV should retain excluded read/write rows");
        }

        ActivityScheduleOptions scalarLocalitySplitSchedule;
        scalarLocalitySplitSchedule.maxOpInComputeSupernode = 1;
        scalarLocalitySplitSchedule.splitOversizeComputeNodes = true;
        scalarLocalitySplitSchedule.splitOversizeComputeNodeMaxOps = 1;
        const std::filesystem::path scalarLocalitySplitDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_scalar_locality_split";
        std::filesystem::remove_all(scalarLocalitySplitDir);
        Design scalarLocalitySplitDesign = buildMaterializedScalarReadLocalityDesign();
        EmitDiagnostics scalarLocalitySplitDiag;
        EmitResult scalarLocalitySplitResult;
        if (!emitWithActivitySchedule(scalarLocalitySplitDesign,
                                      scalarLocalitySplitDir,
                                      scalarLocalitySplitDiag,
                                      scalarLocalitySplitResult,
                                      scalarLocalitySplitSchedule,
                                      false,
                                      false,
                                      false,
                                      false,
                                      true) ||
            !scalarLocalitySplitResult.success || scalarLocalitySplitDiag.hasError())
        {
            return fail("split scalar read-locality diagnostic emit failed");
        }
        std::istringstream scalarLocalitySplitStream(readFile(
            scalarLocalitySplitDir / "grhsim_materialized_scalar_read_locality.tsv"));
        std::string scalarLocalitySplitLine;
        if (!std::getline(scalarLocalitySplitStream, scalarLocalitySplitLine))
        {
            return fail("split scalar read-locality TSV is missing");
        }
        const std::vector<std::string_view> scalarLocalitySplitHeader = splitTabs(scalarLocalitySplitLine);
        const auto splitColumnIndex = [&](std::string_view name) {
            const auto it = std::find(scalarLocalitySplitHeader.begin(), scalarLocalitySplitHeader.end(), name);
            return it == scalarLocalitySplitHeader.end()
                       ? std::numeric_limits<std::size_t>::max()
                       : static_cast<std::size_t>(std::distance(scalarLocalitySplitHeader.begin(), it));
        };
        const std::size_t splitNameColumn = splitColumnIndex("value_name");
        const std::size_t splitWidthColumn = splitColumnIndex("width");
        const std::size_t splitTouchesColumn = splitColumnIndex("operand_touches");
        const std::size_t splitWritesColumn = splitColumnIndex("result_writes");
        const std::size_t splitCandidateColumn = splitColumnIndex("candidate");
        const std::size_t splitSavedColumn = splitColumnIndex("loads_saved_per_fire");
        if (splitNameColumn == std::numeric_limits<std::size_t>::max() ||
            splitWidthColumn == std::numeric_limits<std::size_t>::max() ||
            splitTouchesColumn == std::numeric_limits<std::size_t>::max() ||
            splitWritesColumn == std::numeric_limits<std::size_t>::max() ||
            splitCandidateColumn == std::numeric_limits<std::size_t>::max() ||
            splitSavedColumn == std::numeric_limits<std::size_t>::max())
        {
            return fail("split scalar read-locality TSV schema is incomplete");
        }
        std::size_t scalarLocalitySplitRows = 0;
        bool foundSplitRepeatedSource = false;
        bool foundSplitSingleSource = false;
        while (std::getline(scalarLocalitySplitStream, scalarLocalitySplitLine))
        {
            const std::vector<std::string_view> fields = splitTabs(scalarLocalitySplitLine);
            if (fields.size() != scalarLocalitySplitHeader.size())
            {
                return fail("split scalar read-locality TSV row width mismatch");
            }
            ++scalarLocalitySplitRows;
            if (fields[splitNameColumn] == "locality_wide_source")
            {
                return fail("wide values must not be scalar locality rows");
            }
            if (fields[splitNameColumn] == "locality_repeated_source")
            {
                foundSplitRepeatedSource = true;
                if (fields[splitWidthColumn] != "8" || fields[splitTouchesColumn] != "2" ||
                    fields[splitWritesColumn] != "0" || fields[splitCandidateColumn] != "1" ||
                    fields[splitSavedColumn] != "1")
                {
                    return fail("split repeated scalar locality candidate has unexpected metrics");
                }
            }
            if (fields[splitNameColumn] == "locality_single_source")
            {
                foundSplitSingleSource = true;
                if (fields[splitTouchesColumn] != "1" || fields[splitWritesColumn] != "0" ||
                    fields[splitCandidateColumn] != "0" || fields[splitSavedColumn] != "0")
                {
                    return fail("single scalar read should be reported as ineligible");
                }
            }
        }
        if (scalarLocalitySplitRows != 2 || !foundSplitRepeatedSource || !foundSplitSingleSource)
        {
            return fail("split scalar locality rows should distinguish repeated and single reads");
        }

        const std::filesystem::path scalarLocalityDisabledDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_scalar_locality_disabled";
        std::filesystem::remove_all(scalarLocalityDisabledDir);
        Design scalarLocalityDisabledDesign = buildMaterializedScalarReadLocalityDesign();
        EmitDiagnostics scalarLocalityDisabledDiag;
        EmitResult scalarLocalityDisabledResult;
        if (!emitWithActivitySchedule(scalarLocalityDisabledDesign,
                                      scalarLocalityDisabledDir,
                                      scalarLocalityDisabledDiag,
                                      scalarLocalityDisabledResult,
                                      scalarLocalitySplitSchedule) ||
            !scalarLocalityDisabledResult.success || scalarLocalityDisabledDiag.hasError())
        {
            return fail("disabled scalar read-locality diagnostic emit failed");
        }
        if (std::filesystem::exists(
                scalarLocalityDisabledDir / "grhsim_materialized_scalar_read_locality.tsv"))
        {
            return fail("disabled scalar read-locality diagnostic must not emit a TSV");
        }
        const auto collectGeneratedModelFiles = [](const std::filesystem::path &dir) {
            std::map<std::string, std::string> files;
            for (const auto &entry : std::filesystem::directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const std::string name = entry.path().filename().string();
                if (!name.starts_with("grhsim_top") ||
                    (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp"))
                {
                    continue;
                }
                files.emplace(name, readFile(entry.path()));
            }
            return files;
        };
        if (collectGeneratedModelFiles(scalarLocalitySplitDir) !=
            collectGeneratedModelFiles(scalarLocalityDisabledDir))
        {
            return fail("scalar read-locality diagnostic must not change generated model code");
        }

        const std::filesystem::path repeatedReadDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_repeated_state_read";
        std::filesystem::remove_all(repeatedReadDir);
        Design repeatedReadDesign = buildRepeatedScalarStateReadDesign();
        ActivityScheduleOptions repeatedReadScheduleOptions;
        repeatedReadScheduleOptions.maxOpInComputeSupernode = 4;
        repeatedReadScheduleOptions.splitOversizeComputeNodes = true;
        repeatedReadScheduleOptions.splitOversizeComputeNodeMaxOps = 4;
        EmitDiagnostics repeatedReadDiag;
        EmitResult repeatedReadResult;
        if (!emitWithActivitySchedule(repeatedReadDesign,
                                      repeatedReadDir,
                                      repeatedReadDiag,
                                      repeatedReadResult,
                                      repeatedReadScheduleOptions,
                                      false,
                                      false,
                                      true,
                                      false,
                                      true))
        {
            return fail("repeated state-read activity-schedule pass failed");
        }
        if (!repeatedReadResult.success || repeatedReadDiag.hasError())
        {
            return fail("repeated state-read emit failed");
        }
        const std::vector<std::filesystem::path> repeatedReadStateFiles =
            collectSchedFiles(repeatedReadDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> repeatedReadSchedFiles =
            collectSchedFiles(repeatedReadDir, "grhsim_top_sched_");
        const std::string repeatedReadSched = readFiles(repeatedReadSchedFiles);
        if (countSubstring(repeatedReadSched,
                           "same-state scalar read: reuse synchronized change predicate") == 0)
        {
            return fail("repeated scalar state reads should share one changed predicate per supernode");
        }
        if (countSubstring(repeatedReadSched,
                           "same-state scalar read: reuse consolidated storage slot") == 0)
        {
            return fail("repeated scalar state reads should share one materialized slot per supernode");
        }
        const std::filesystem::path repeatedReadLocalityPath =
            repeatedReadDir / "grhsim_state_read_locality.tsv";
        const std::string repeatedReadLocality = readFile(repeatedReadLocalityPath);
        std::istringstream localityStream(repeatedReadLocality);
        std::string localityLine;
        if (!std::getline(localityStream, localityLine))
        {
            return fail("repeated state-read locality TSV is missing");
        }
        const std::vector<std::string_view> localityHeader = splitTabs(localityLine);
        const auto columnIndex = [&](std::string_view name) {
            const auto it = std::find(localityHeader.begin(), localityHeader.end(), name);
            return it == localityHeader.end()
                       ? std::numeric_limits<std::size_t>::max()
                       : static_cast<std::size_t>(std::distance(localityHeader.begin(), it));
        };
        const std::size_t stateSymbolColumn = columnIndex("state_symbol");
        const std::size_t materializedColumn = columnIndex("materialized");
        const std::size_t aliasColumn = columnIndex("alias");
        const std::size_t onlyReadsColumn = columnIndex("supernode_only_state_reads");
        if (stateSymbolColumn == std::numeric_limits<std::size_t>::max() ||
            materializedColumn == std::numeric_limits<std::size_t>::max() ||
            aliasColumn == std::numeric_limits<std::size_t>::max() ||
            onlyReadsColumn == std::numeric_limits<std::size_t>::max())
        {
            return fail("repeated state-read locality TSV schema is incomplete");
        }
        std::size_t repeatedReadRows = 0;
        bool foundRepeatedState = false;
        bool foundMaterialized = false;
        bool foundAlias = false;
        bool foundPureOrMixedClassification = false;
        while (std::getline(localityStream, localityLine))
        {
            const std::vector<std::string_view> fields = splitTabs(localityLine);
            if (fields.size() != localityHeader.size())
            {
                return fail("repeated state-read locality TSV row width mismatch");
            }
            ++repeatedReadRows;
            foundRepeatedState = foundRepeatedState || fields[stateSymbolColumn] == "repeated_q";
            foundMaterialized = foundMaterialized || fields[materializedColumn] == "1";
            foundAlias = foundAlias || fields[aliasColumn] == "1";
            foundPureOrMixedClassification =
                foundPureOrMixedClassification || fields[onlyReadsColumn] == "0" || fields[onlyReadsColumn] == "1";
        }
        if (repeatedReadRows == 0 || !foundRepeatedState || !foundMaterialized || !foundAlias ||
            !foundPureOrMixedClassification)
        {
            return fail("repeated state-read locality TSV contents are incomplete");
        }
        const std::filesystem::path repeatedReadScalarLocalityPath =
            repeatedReadDir / "grhsim_materialized_scalar_read_locality.tsv";
        std::istringstream scalarLocalityStream(readFile(repeatedReadScalarLocalityPath));
        std::string scalarLocalityLine;
        if (!std::getline(scalarLocalityStream, scalarLocalityLine))
        {
            return fail("materialized scalar read-locality TSV is missing");
        }
        const std::vector<std::string_view> scalarLocalityHeader = splitTabs(scalarLocalityLine);
        const auto scalarColumnIndex = [&](std::string_view name) {
            const auto it = std::find(scalarLocalityHeader.begin(), scalarLocalityHeader.end(), name);
            return it == scalarLocalityHeader.end()
                       ? std::numeric_limits<std::size_t>::max()
                       : static_cast<std::size_t>(std::distance(scalarLocalityHeader.begin(), it));
        };
        const std::size_t scalarPhaseColumn = scalarColumnIndex("phase");
        const std::size_t scalarValueNameColumn = scalarColumnIndex("value_name");
        const std::size_t scalarKindColumn = scalarColumnIndex("scalar_kind");
        const std::size_t scalarTouchesColumn = scalarColumnIndex("operand_touches");
        const std::size_t scalarUseOpsColumn = scalarColumnIndex("use_ops");
        const std::size_t scalarWritesColumn = scalarColumnIndex("result_writes");
        const std::size_t scalarCandidateColumn = scalarColumnIndex("candidate");
        const std::size_t scalarSavedColumn = scalarColumnIndex("loads_saved_per_fire");
        if (scalarPhaseColumn == std::numeric_limits<std::size_t>::max() ||
            scalarValueNameColumn == std::numeric_limits<std::size_t>::max() ||
            scalarKindColumn == std::numeric_limits<std::size_t>::max() ||
            scalarTouchesColumn == std::numeric_limits<std::size_t>::max() ||
            scalarUseOpsColumn == std::numeric_limits<std::size_t>::max() ||
            scalarWritesColumn == std::numeric_limits<std::size_t>::max() ||
            scalarCandidateColumn == std::numeric_limits<std::size_t>::max() ||
            scalarSavedColumn == std::numeric_limits<std::size_t>::max())
        {
            return fail("materialized scalar read-locality TSV schema is incomplete");
        }
        std::size_t repeatedScalarCandidateRows = 0;
        std::size_t repeatedScalarCandidateTouches = 0;
        std::size_t repeatedScalarCandidateSaved = 0;
        while (std::getline(scalarLocalityStream, scalarLocalityLine))
        {
            const std::vector<std::string_view> fields = splitTabs(scalarLocalityLine);
            if (fields.size() != scalarLocalityHeader.size())
            {
                return fail("materialized scalar read-locality TSV row width mismatch");
            }
            if (fields[scalarCandidateColumn] != "1")
            {
                continue;
            }
            if (fields[scalarPhaseColumn] != "compute" || fields[scalarKindColumn] != "u8" ||
                fields[scalarUseOpsColumn] != "1" || fields[scalarWritesColumn] != "0")
            {
                return fail("repeated scalar read-locality candidate has unexpected classification");
            }
            const std::size_t touches = static_cast<std::size_t>(std::stoull(std::string(fields[scalarTouchesColumn])));
            const std::size_t saved = static_cast<std::size_t>(std::stoull(std::string(fields[scalarSavedColumn])));
            if (touches < 2 || saved != touches - 1u)
            {
                return fail("repeated scalar read-locality candidate has unexpected metrics");
            }
            ++repeatedScalarCandidateRows;
            repeatedScalarCandidateTouches += touches;
            repeatedScalarCandidateSaved += saved;
        }
        if (repeatedScalarCandidateRows != 4 || repeatedScalarCandidateTouches != 15 ||
            repeatedScalarCandidateSaved != 11)
        {
            return fail("repeated materialized scalar reads should aggregate by canonical slot");
        }
        const auto runRepeatedReadHarness = [&](const std::filesystem::path &dir,
                                                const std::vector<std::filesystem::path> &stateFiles,
                                                const std::vector<std::filesystem::path> &schedFiles) {
            const std::filesystem::path harnessPath = dir / "grhsim_top_harness.cpp";
            std::ofstream harness(harnessPath);
            if (!harness.is_open())
            {
                return false;
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.data = static_cast<std::uint8_t>(9);\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.q_out != static_cast<std::uint8_t>(3)) return 1;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.q_out != static_cast<std::uint8_t>(9)) return 2;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.q_out != static_cast<std::uint8_t>(9)) return 3;\n";
            harness << "    return 0;\n";
            harness << "}\n";
            harness.close();

            const std::filesystem::path harnessExe = dir / "grhsim_top_harness";
            std::string compileCmd =
                "clang++ " + std::string(kHarnessCompileFlags) + " -I" + dir.string();
            for (const auto &stateFile : stateFiles)
            {
                compileCmd += " " + stateFile.string();
            }
            compileCmd += " " + (dir / "grhsim_top_eval.cpp").string();
            for (const auto &schedFile : schedFiles)
            {
                compileCmd += " " + schedFile.string();
            }
            compileCmd += " " + harnessPath.string() + " -o " + harnessExe.string();
            if (std::system(compileCmd.c_str()) != 0)
            {
                return false;
            }
            const std::filesystem::path harnessLog = dir / "grhsim_top_harness.log";
            const std::string runCmd = harnessExe.string() + " > " + harnessLog.string() + " 2>&1";
            return std::system(runCmd.c_str()) == 0;
        };
        if (!runRepeatedReadHarness(repeatedReadDir, repeatedReadStateFiles, repeatedReadSchedFiles))
        {
            return fail("repeated state-read harness failed");
        }

        const std::filesystem::path repeatedDirectDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_repeated_direct_state_read";
        std::filesystem::remove_all(repeatedDirectDir);
        Design repeatedDirectDesign = buildRepeatedScalarStateReadDesign();
        EmitDiagnostics repeatedDirectDiag;
        EmitResult repeatedDirectResult;
        if (!emitWithActivitySchedule(repeatedDirectDesign,
                                      repeatedDirectDir,
                                      repeatedDirectDiag,
                                      repeatedDirectResult,
                                      repeatedReadScheduleOptions,
                                      false,
                                      false,
                                      false,
                                      true,
                                      true))
        {
            return fail("repeated direct state-read activity-schedule pass failed");
        }
        if (!repeatedDirectResult.success || repeatedDirectDiag.hasError())
        {
            return fail("repeated direct state-read emit failed");
        }
        const std::vector<std::filesystem::path> repeatedDirectStateFiles =
            collectSchedFiles(repeatedDirectDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> repeatedDirectSchedFiles =
            collectSchedFiles(repeatedDirectDir, "grhsim_top_sched_");
        const std::string repeatedDirectSched = readFiles(repeatedDirectSchedFiles);
        std::istringstream repeatedDirectScalarLocalityStream(
            readFile(repeatedDirectDir / "grhsim_materialized_scalar_read_locality.tsv"));
        std::string repeatedDirectScalarLocalityLine;
        if (!std::getline(repeatedDirectScalarLocalityStream, repeatedDirectScalarLocalityLine))
        {
            return fail("direct state-read scalar locality TSV is missing");
        }
        const std::vector<std::string_view> repeatedDirectScalarHeader = splitTabs(repeatedDirectScalarLocalityLine);
        const auto repeatedDirectCandidateIt =
            std::find(repeatedDirectScalarHeader.begin(), repeatedDirectScalarHeader.end(), "candidate");
        if (repeatedDirectCandidateIt == repeatedDirectScalarHeader.end())
        {
            return fail("direct state-read scalar locality TSV schema is incomplete");
        }
        const std::size_t repeatedDirectCandidateColumn = static_cast<std::size_t>(
            std::distance(repeatedDirectScalarHeader.begin(), repeatedDirectCandidateIt));
        while (std::getline(repeatedDirectScalarLocalityStream, repeatedDirectScalarLocalityLine))
        {
            const std::vector<std::string_view> fields = splitTabs(repeatedDirectScalarLocalityLine);
            if (fields.size() != repeatedDirectScalarHeader.size())
            {
                return fail("direct state-read scalar locality TSV row width mismatch");
            }
            if (fields[repeatedDirectCandidateColumn] == "1")
            {
                return fail("direct state-read operands must not be scalar slot locality candidates");
            }
        }
        constexpr std::size_t expectedDirectRepeatedReads = 15;
        if (countSubstring(repeatedDirectSched,
                           "direct single-writer state read: consumer reads visible state") !=
                expectedDirectRepeatedReads ||
            countSubstring(repeatedDirectSched,
                           "same-state scalar read: reuse synchronized change predicate") != 0 ||
            countSubstring(repeatedDirectSched, "grhsim_any_changed_") != 0 ||
            countSubstring(repeatedDirectSched, "[kRegisterReadPort] reg=repeated_q") !=
                expectedDirectRepeatedReads + 2)
        {
            return fail("repeated direct state reads should forward canonical and alias groups atomically");
        }
        if (!runRepeatedReadHarness(repeatedDirectDir,
                                    repeatedDirectStateFiles,
                                    repeatedDirectSchedFiles))
        {
            return fail("repeated direct state-read harness failed");
        }

        const std::filesystem::path directReadDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_direct_state_read";
        constexpr const char *directReadEnv =
            "WOLVRIX_GRHSIM_DIRECT_SINGLE_WRITER_STATE_READS";
        if (unsetenv(directReadEnv) != 0)
        {
            return fail("failed to clear direct state-read environment option");
        }
        std::filesystem::remove_all(directReadDir);
        Design directReadDesign = buildDirectStateReadForwardDesign();
        ActivityScheduleOptions directReadScheduleOptions;
        directReadScheduleOptions.maxOpInComputeSupernode = 1;
        directReadScheduleOptions.splitOversizeComputeNodes = true;
        directReadScheduleOptions.splitOversizeComputeNodeMaxOps = 1;
        EmitDiagnostics directReadDiag;
        EmitResult directReadResult;
        if (!emitWithActivitySchedule(directReadDesign,
                                      directReadDir,
                                      directReadDiag,
                                      directReadResult,
                                      directReadScheduleOptions,
                                      false,
                                      false,
                                      false,
                                      std::nullopt,
                                      false,
                                      false,
                                      false))
        {
            return fail("direct state-read activity-schedule pass failed");
        }
        if (!directReadResult.success || directReadDiag.hasError())
        {
            return fail("direct state-read emit failed");
        }
        if (std::filesystem::exists(directReadDir / "grhsim_materialized_scalar_read_locality.tsv"))
        {
            return fail("materialized scalar read-locality diagnostic should be disabled by default");
        }
        const std::vector<std::filesystem::path> directReadStateFiles =
            collectSchedFiles(directReadDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> directReadSchedFiles =
            collectSchedFiles(directReadDir, "grhsim_top_sched_");
        const std::string directReadSched = readFiles(directReadSchedFiles);
        const auto directReadSnippet = [&](std::string_view opComment) {
            const std::size_t begin = directReadSched.find(opComment);
            if (begin == std::string::npos)
            {
                return std::string_view{};
            }
            const std::size_t end = directReadSched.find("// op ", begin + opComment.size());
            return std::string_view(directReadSched).substr(
                begin,
                end == std::string::npos ? end : end - begin);
        };
        const std::string_view directReadSource =
            directReadSnippet("[kRegisterReadPort] reg=direct_q");
        const std::string_view directReadConsumer =
            directReadSnippet("// op direct_plus_one_value_op [kAdd]");
        const std::string_view protectedReadSource =
            directReadSnippet("// op protected_q_read_op [kRegisterReadPort] reg=protected_q");
        const std::string_view multiReadSource =
            directReadSnippet("[kRegisterReadPort] reg=multi_q");
        if (directReadSource.empty() ||
            directReadSource.find("direct single-writer state read: consumer reads visible state") ==
                std::string_view::npos ||
            directReadSource.find("grhsim_changed_") != std::string_view::npos)
        {
            return fail("eligible single-writer register read should bypass its materialized slot");
        }
        if (directReadConsumer.empty() ||
            directReadConsumer.find("state_logic_storage_") == std::string_view::npos)
        {
            return fail("direct state-read consumer should reference visible state storage");
        }
        if (protectedReadSource.empty() || multiReadSource.empty() ||
            protectedReadSource.find("direct single-writer state read") != std::string_view::npos ||
            multiReadSource.find("direct single-writer state read") != std::string_view::npos)
        {
            return fail("protected and multi-writer register reads must retain the materialized path");
        }

        const auto emitDirectReadOptionFixture = [&](std::string_view suffix,
                                                     std::optional<bool> directReadOption)
            -> std::optional<std::filesystem::path>
        {
            const std::filesystem::path dir = directReadDir.string() + "_" + std::string(suffix);
            std::filesystem::remove_all(dir);
            Design fixture = buildDirectStateReadForwardDesign();
            EmitDiagnostics fixtureDiag;
            EmitResult fixtureResult;
            if (!emitWithActivitySchedule(fixture,
                                          dir,
                                          fixtureDiag,
                                          fixtureResult,
                                          directReadScheduleOptions,
                                          false,
                                          false,
                                          false,
                                          directReadOption,
                                          false,
                                          false,
                                          false) ||
                !fixtureResult.success || fixtureDiag.hasError())
            {
                return std::nullopt;
            }
            return dir;
        };
        const auto directReadExplicitOffDir =
            emitDirectReadOptionFixture("explicit_off", false);
        if (setenv(directReadEnv, "0", 1) != 0)
        {
            return fail("failed to set direct state-read environment option");
        }
        const auto directReadEnvOffDir =
            emitDirectReadOptionFixture("env_off", std::nullopt);
        const auto directReadAttributeOnEnvOffDir =
            emitDirectReadOptionFixture("attribute_on_env_off", true);
        if (setenv(directReadEnv, "1", 1) != 0)
        {
            return fail("failed to set direct state-read environment option");
        }
        const auto directReadAttributeOffEnvOnDir =
            emitDirectReadOptionFixture("attribute_off_env_on", false);
        if (unsetenv(directReadEnv) != 0)
        {
            return fail("failed to clear direct state-read environment option");
        }
        if (!directReadExplicitOffDir || !directReadEnvOffDir ||
            !directReadAttributeOnEnvOffDir || !directReadAttributeOffEnvOnDir)
        {
            return fail("direct state-read option fixture emit failed");
        }
        const auto directReadDefaultFiles =
            collectGeneratedSourceFiles(directReadDir, "grhsim_top");
        const auto directReadExplicitOffFiles =
            collectGeneratedSourceFiles(*directReadExplicitOffDir, "grhsim_top");
        const auto directReadEnvOffFiles =
            collectGeneratedSourceFiles(*directReadEnvOffDir, "grhsim_top");
        const auto directReadAttributeOnEnvOffFiles =
            collectGeneratedSourceFiles(*directReadAttributeOnEnvOffDir, "grhsim_top");
        const auto directReadAttributeOffEnvOnFiles =
            collectGeneratedSourceFiles(*directReadAttributeOffEnvOnDir, "grhsim_top");
        const std::string directReadExplicitOffSched =
            readFiles(collectSchedFiles(*directReadExplicitOffDir, "grhsim_top_sched_"));
        constexpr std::string_view directReadMarker =
            "direct single-writer state read: consumer reads visible state";
        if (directReadDefaultFiles != directReadAttributeOnEnvOffFiles ||
            directReadDefaultFiles == directReadExplicitOffFiles ||
            directReadSched.find(directReadMarker) == std::string::npos)
        {
            return fail("direct state-read native default should match explicit enable");
        }
        if (directReadExplicitOffFiles != directReadEnvOffFiles ||
            directReadExplicitOffFiles != directReadAttributeOffEnvOnFiles ||
            directReadExplicitOffSched.find(directReadMarker) != std::string::npos)
        {
            return fail("direct state-read explicit disable or option precedence changed legacy output");
        }

        const std::filesystem::path directReadHarnessPath = directReadDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(directReadHarnessPath);
            if (!harness.is_open())
            {
                return fail("failed to create direct state-read harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.direct_data = static_cast<std::uint8_t>(9);\n";
            harness << "    sim.protected_data = static_cast<std::uint8_t>(11);\n";
            harness << "    sim.multi_write_a = false;\n";
            harness << "    sim.multi_write_b = false;\n";
            harness << "    sim.multi_data_a = static_cast<std::uint8_t>(13);\n";
            harness << "    sim.multi_data_b = static_cast<std::uint8_t>(17);\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.direct_plus_one != static_cast<std::uint8_t>(4)) return 1;\n";
            harness << "    if (sim.protected_q_out != static_cast<std::uint8_t>(7)) return 2;\n";
            harness << "    if (sim.multi_plus_one != static_cast<std::uint8_t>(6)) return 3;\n";
            harness << "    sim.multi_write_a = true;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.direct_plus_one != static_cast<std::uint8_t>(10)) return 4;\n";
            harness << "    if (sim.protected_q_out != static_cast<std::uint8_t>(11)) return 5;\n";
            harness << "    if (sim.multi_plus_one != static_cast<std::uint8_t>(14)) return 6;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.multi_write_a = false;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.direct_plus_one != static_cast<std::uint8_t>(10)) return 7;\n";
            harness << "    if (sim.protected_q_out != static_cast<std::uint8_t>(11)) return 8;\n";
            harness << "    if (sim.multi_plus_one != static_cast<std::uint8_t>(14)) return 9;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    sim.direct_data = static_cast<std::uint8_t>(2);\n";
            harness << "    sim.protected_data = static_cast<std::uint8_t>(19);\n";
            harness << "    sim.multi_write_b = true;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (sim.direct_plus_one != static_cast<std::uint8_t>(3)) return 10;\n";
            harness << "    if (sim.protected_q_out != static_cast<std::uint8_t>(19)) return 11;\n";
            harness << "    if (sim.multi_plus_one != static_cast<std::uint8_t>(18)) return 12;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path directReadHarnessExe = directReadDir / "grhsim_top_harness";
        std::string directReadCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + directReadDir.string();
        for (const auto &stateFile : directReadStateFiles)
        {
            directReadCompileCmd += " " + stateFile.string();
        }
        directReadCompileCmd += " " + (directReadDir / "grhsim_top_eval.cpp").string();
        for (const auto &schedFile : directReadSchedFiles)
        {
            directReadCompileCmd += " " + schedFile.string();
        }
        directReadCompileCmd += " " + directReadHarnessPath.string() + " -o " + directReadHarnessExe.string();
        if (std::system(directReadCompileCmd.c_str()) != 0)
        {
            return fail("direct state-read harness failed to compile");
        }
        if (std::system(directReadHarnessExe.string().c_str()) != 0)
        {
            return fail("direct state-read harness failed to run");
        }

        const std::filesystem::path pureEventRoot =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_pure_event_word";
        constexpr const char *pureEventBypassEnv =
            "WOLVRIX_GRHSIM_PURE_EVENT_COMPUTE_WORD_BYPASS";
        if (unsetenv(pureEventBypassEnv) != 0)
        {
            return fail("failed to clear pure-event compute-word environment option");
        }
        ActivityScheduleOptions pureEventSchedule;
        pureEventSchedule.maxOpInComputeSupernode = 1;
        pureEventSchedule.enableCoarsen = false;
        pureEventSchedule.splitOversizeComputeNodes = true;
        pureEventSchedule.splitOversizeComputeNodeMaxOps = 1;
        const auto emitPureEventFixture = [&](std::string_view suffix,
                                              PureEventWordFixtureMode mode,
                                              std::optional<bool> bypass,
                                              bool posedgeFullpass,
                                              bool fullWordConsume,
                                              std::optional<bool> profile = std::nullopt,
                                              std::size_t taskCount = 16u,
                                              std::size_t schedBatchMaxOps = 8u,
                                              std::size_t schedBatchMaxEstimatedLines = 96u,
                                              std::optional<std::string_view> packPolicy = std::nullopt,
                                              std::optional<std::size_t> packMaxMovedSupernodePpm = std::nullopt,
                                              std::optional<std::size_t> packMaxChangedWordPpm = std::nullopt)
            -> std::optional<std::filesystem::path>
        {
            const std::filesystem::path dir = pureEventRoot.string() + "_" + std::string(suffix);
            std::filesystem::remove_all(dir);
            Design fixture = buildPureEventWordBypassDesign(mode, taskCount);
            EmitDiagnostics fixtureDiag;
            EmitResult fixtureResult;
            if (!emitWithActivitySchedule(fixture,
                                          dir,
                                          fixtureDiag,
                                          fixtureResult,
                                          pureEventSchedule,
                                          posedgeFullpass,
                                          false,
                                          false,
                                          false,
                                          false,
                                          fullWordConsume,
                                          bypass,
                                          profile,
                                          schedBatchMaxOps,
                                          schedBatchMaxEstimatedLines,
                                          packPolicy,
                                          packMaxMovedSupernodePpm,
                                          packMaxChangedWordPpm) ||
                !fixtureResult.success || fixtureDiag.hasError())
            {
                return std::nullopt;
            }
            return dir;
        };

        const auto pureEventDefaultDir = emitPureEventFixture("default",
                                                              PureEventWordFixtureMode::kHomogeneous,
                                                              std::nullopt,
                                                              false,
                                                              false);
        const auto pureEventDisabledDir = emitPureEventFixture("disabled",
                                                               PureEventWordFixtureMode::kHomogeneous,
                                                               false,
                                                               false,
                                                               false);
        const auto pureEventEnabledDir = emitPureEventFixture("enabled",
                                                              PureEventWordFixtureMode::kHomogeneous,
                                                              true,
                                                              false,
                                                              false);
        const auto pureEventProfileDisabledDir = emitPureEventFixture("profile_disabled",
                                                                      PureEventWordFixtureMode::kHomogeneous,
                                                                      std::nullopt,
                                                                      false,
                                                                      false,
                                                                      false);
        const auto pureEventProfileOnlyDir = emitPureEventFixture("profile_only",
                                                                  PureEventWordFixtureMode::kHomogeneous,
                                                                  false,
                                                                  false,
                                                                  false,
                                                                  true);
        const auto pureEventProfileBypassDir = emitPureEventFixture("profile_bypass",
                                                                    PureEventWordFixtureMode::kHomogeneous,
                                                                    true,
                                                                    false,
                                                                    false,
                                                                    true);
        const auto pureEventOnceDir = emitPureEventFixture("once",
                                                           PureEventWordFixtureMode::kOnceOnly,
                                                           true,
                                                           false,
                                                           false,
                                                           true);
        const auto pureEventMultiDir = emitPureEventFixture("multi",
                                                            PureEventWordFixtureMode::kMultiEvent,
                                                            true,
                                                            false,
                                                            false,
                                                            true);
        const auto pureEventFullpassDir = emitPureEventFixture("fullpass",
                                                               PureEventWordFixtureMode::kHomogeneous,
                                                               true,
                                                               true,
                                                               false,
                                                               true);
        const auto pureEventFullWordConsumeDir = emitPureEventFixture("full_word_consume",
                                                                      PureEventWordFixtureMode::kHomogeneous,
                                                                      true,
                                                                      false,
                                                                      true,
                                                                      true);
        const auto pureEventSparseOneDir = emitPureEventFixture("sparse_one",
                                                                PureEventWordFixtureMode::kHomogeneous,
                                                                true,
                                                                false,
                                                                false,
                                                                false,
                                                                8u,
                                                                100000u,
                                                                100000u);
        const auto pureEventSparseTwoDir = emitPureEventFixture("sparse_two",
                                                                PureEventWordFixtureMode::kHomogeneous,
                                                                true,
                                                                false,
                                                                false,
                                                                false,
                                                                16u,
                                                                100000u,
                                                                100000u);
        const auto pureEventDenseThreeDir = emitPureEventFixture("dense_three",
                                                                 PureEventWordFixtureMode::kHomogeneous,
                                                                 true,
                                                                 false,
                                                                 false,
                                                                 false,
                                                                 24u,
                                                                 100000u,
                                                                 100000u);
        const auto pureEventPackDefaultDir = emitPureEventFixture("pack_default",
                                                                  PureEventWordFixtureMode::kAlternatingEvents,
                                                                  true,
                                                                  false,
                                                                  false,
                                                                  false,
                                                                  16u,
                                                                  100000u,
                                                                  100000u);
        const auto pureEventPackOffDir = emitPureEventFixture("pack_off",
                                                              PureEventWordFixtureMode::kAlternatingEvents,
                                                              true,
                                                              false,
                                                              false,
                                                              false,
                                                              16u,
                                                              100000u,
                                                              100000u,
                                                              "off");
        const auto pureEventPackProbeDir = emitPureEventFixture("pack_probe",
                                                                PureEventWordFixtureMode::kAlternatingEvents,
                                                                true,
                                                                false,
                                                                false,
                                                                false,
                                                                16u,
                                                                100000u,
                                                                100000u,
                                                                "probe",
                                                                1000000u,
                                                                1000000u);
        const auto pureEventPackTargetedDir = emitPureEventFixture("pack_targeted",
                                                                   PureEventWordFixtureMode::kAlternatingEvents,
                                                                   true,
                                                                   false,
                                                                   false,
                                                                   false,
                                                                   16u,
                                                                   100000u,
                                                                   100000u,
                                                                   "targeted",
                                                                   1000000u,
                                                                   1000000u);
        const auto pureEventPackTargetedRepeatDir = emitPureEventFixture("pack_targeted_repeat",
                                                                         PureEventWordFixtureMode::kAlternatingEvents,
                                                                         true,
                                                                         false,
                                                                         false,
                                                                         false,
                                                                         16u,
                                                                         100000u,
                                                                         100000u,
                                                                         "targeted",
                                                                         1000000u,
                                                                         1000000u);
        const auto pureEventPackRemainderOffDir = emitPureEventFixture("pack_remainder_off",
                                                                       PureEventWordFixtureMode::kAlternatingEvents,
                                                                       true,
                                                                       false,
                                                                       false,
                                                                       false,
                                                                       20u,
                                                                       100000u,
                                                                       100000u,
                                                                       "off");
        const auto pureEventPackRemainderProbeDir = emitPureEventFixture("pack_remainder_probe",
                                                                         PureEventWordFixtureMode::kAlternatingEvents,
                                                                         true,
                                                                         false,
                                                                         false,
                                                                         false,
                                                                         20u,
                                                                         100000u,
                                                                         100000u,
                                                                         "probe",
                                                                         1000000u,
                                                                         1000000u);
        const auto pureEventPackRemainderTargetedDir = emitPureEventFixture("pack_remainder_targeted",
                                                                            PureEventWordFixtureMode::kAlternatingEvents,
                                                                            true,
                                                                            false,
                                                                            false,
                                                                            false,
                                                                            20u,
                                                                            100000u,
                                                                            100000u,
                                                                            "targeted",
                                                                            1000000u,
                                                                            1000000u);
        ActivityScheduleOptions pureEventPackEstimatedLineSchedule;
        pureEventPackEstimatedLineSchedule.maxOpInComputeSupernode = 4u;
        pureEventPackEstimatedLineSchedule.enableCoarsen = false;
        const auto emitPureEventPackEstimatedLineFixture = [&](std::string_view suffix,
                                                               std::string_view policy)
            -> std::optional<std::filesystem::path>
        {
            const std::filesystem::path dir = pureEventRoot.string() + "_" + std::string(suffix);
            std::filesystem::remove_all(dir);
            Design fixture = buildPureEventWordPackEstimatedLineDriftDesign();
            EmitDiagnostics fixtureDiag;
            EmitResult fixtureResult;
            if (!emitWithActivitySchedule(fixture,
                                          dir,
                                          fixtureDiag,
                                          fixtureResult,
                                          pureEventPackEstimatedLineSchedule,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          true,
                                          false,
                                          100000u,
                                          100000u,
                                          policy,
                                          1000000u,
                                          1000000u) ||
                !fixtureResult.success || fixtureDiag.hasError())
            {
                return std::nullopt;
            }
            return dir;
        };
        const auto pureEventPackEstimatedLineOffDir =
            emitPureEventPackEstimatedLineFixture("pack_estimated_line_off", "off");
        const auto pureEventPackEstimatedLineProbeDir =
            emitPureEventPackEstimatedLineFixture("pack_estimated_line_probe", "probe");
        const auto pureEventPackEstimatedLineTargetedDir =
            emitPureEventPackEstimatedLineFixture("pack_estimated_line_targeted", "targeted");
        if (setenv(pureEventBypassEnv, "0", 1) != 0)
        {
            return fail("failed to set pure-event compute-word environment option");
        }
        const auto pureEventEnvDisabledDir = emitPureEventFixture("env_disabled",
                                                                  PureEventWordFixtureMode::kHomogeneous,
                                                                  std::nullopt,
                                                                  false,
                                                                  false);
        const auto pureEventAttributeEnabledEnvDisabledDir =
            emitPureEventFixture("attribute_enabled_env_disabled",
                                 PureEventWordFixtureMode::kHomogeneous,
                                 true,
                                 false,
                                 false);
        if (setenv(pureEventBypassEnv, "1", 1) != 0)
        {
            return fail("failed to set pure-event compute-word environment option");
        }
        const auto pureEventAttributeDisabledEnvEnabledDir =
            emitPureEventFixture("attribute_disabled_env_enabled",
                                 PureEventWordFixtureMode::kHomogeneous,
                                 false,
                                 false,
                                 false);
        if (unsetenv(pureEventBypassEnv) != 0)
        {
            return fail("failed to clear pure-event compute-word environment option");
        }
        if (!pureEventDefaultDir || !pureEventDisabledDir || !pureEventEnabledDir || !pureEventOnceDir ||
            !pureEventProfileDisabledDir || !pureEventProfileOnlyDir || !pureEventProfileBypassDir ||
            !pureEventMultiDir || !pureEventFullpassDir || !pureEventFullWordConsumeDir ||
            !pureEventSparseOneDir || !pureEventSparseTwoDir || !pureEventDenseThreeDir ||
            !pureEventPackDefaultDir || !pureEventPackOffDir || !pureEventPackProbeDir ||
            !pureEventPackTargetedDir || !pureEventPackTargetedRepeatDir ||
            !pureEventPackRemainderOffDir || !pureEventPackRemainderProbeDir ||
            !pureEventPackRemainderTargetedDir || !pureEventPackEstimatedLineOffDir ||
            !pureEventPackEstimatedLineProbeDir || !pureEventPackEstimatedLineTargetedDir ||
            !pureEventEnvDisabledDir || !pureEventAttributeEnabledEnvDisabledDir ||
            !pureEventAttributeDisabledEnvEnabledDir)
        {
            return fail("pure-event compute-word fixture emit failed");
        }

        const auto readPureEventGenerated = [](const std::filesystem::path &dir) {
            return readFile(dir / "grhsim_top.hpp") +
                   readFile(dir / "grhsim_top_runtime.hpp") +
                   readFiles(collectSchedFiles(dir, "grhsim_top_state")) +
                   readFile(dir / "grhsim_top_eval.cpp") +
                   readFiles(collectSchedFiles(dir, "grhsim_top_sched_"));
        };
        const std::string pureEventDefaultSource = readPureEventGenerated(*pureEventDefaultDir);
        const std::string pureEventDisabledSource = readPureEventGenerated(*pureEventDisabledDir);
        const std::string pureEventEnabledSource = readPureEventGenerated(*pureEventEnabledDir);
        const std::string pureEventProfileDisabledSource =
            readPureEventGenerated(*pureEventProfileDisabledDir);
        const std::string pureEventEnvDisabledSource =
            readPureEventGenerated(*pureEventEnvDisabledDir);
        const std::string pureEventAttributeEnabledEnvDisabledSource =
            readPureEventGenerated(*pureEventAttributeEnabledEnvDisabledDir);
        const std::string pureEventAttributeDisabledEnvEnabledSource =
            readPureEventGenerated(*pureEventAttributeDisabledEnvEnabledDir);
        const std::string pureEventEnabledSched =
            readFiles(collectSchedFiles(*pureEventEnabledDir, "grhsim_top_sched_"));
        constexpr std::string_view pureEventMarker =
            "// Pure-event compute word: an event miss consumes the cleared word.";
        if (pureEventDefaultSource != pureEventEnabledSource ||
            pureEventDefaultSource != pureEventProfileDisabledSource ||
            pureEventDefaultSource != pureEventAttributeEnabledEnvDisabledSource ||
            pureEventDefaultSource.find(pureEventMarker) == std::string::npos)
        {
            return fail("pure-event compute-word native default should match explicit enable");
        }
        if (pureEventDisabledSource != pureEventEnvDisabledSource ||
            pureEventDisabledSource != pureEventAttributeDisabledEnvEnabledSource ||
            pureEventDisabledSource == pureEventDefaultSource ||
            pureEventDisabledSource.find(pureEventMarker) != std::string::npos)
        {
            return fail("pure-event compute-word explicit disable or option precedence changed legacy output");
        }

        const std::string pureEventPackDefaultSource = readPureEventGenerated(*pureEventPackDefaultDir);
        const std::string pureEventPackOffSource = readPureEventGenerated(*pureEventPackOffDir);
        const std::string pureEventPackProbeSource = readPureEventGenerated(*pureEventPackProbeDir);
        const std::string pureEventPackTargetedSource = readPureEventGenerated(*pureEventPackTargetedDir);
        const std::string pureEventPackTargetedRepeatSource =
            readPureEventGenerated(*pureEventPackTargetedRepeatDir);
        const std::string pureEventPackDefaultStats =
            readFile(*pureEventPackDefaultDir / "grhsim_emit_stats.json");
        const std::string pureEventPackOffStats =
            readFile(*pureEventPackOffDir / "grhsim_emit_stats.json");
        const std::string pureEventPackProbeStats =
            readFile(*pureEventPackProbeDir / "grhsim_emit_stats.json");
        const std::string pureEventPackTargetedStats =
            readFile(*pureEventPackTargetedDir / "grhsim_emit_stats.json");
        const std::string pureEventPackTargetedRepeatStats =
            readFile(*pureEventPackTargetedRepeatDir / "grhsim_emit_stats.json");
        if (pureEventPackDefaultSource != pureEventPackOffSource ||
            pureEventPackDefaultStats != pureEventPackOffStats ||
            pureEventPackOffStats.find("\"pure_event_word_pack\"") != std::string::npos)
        {
            return fail("pure-event word-pack default and explicit off artifacts should be byte-identical");
        }
        if (pureEventPackProbeSource != pureEventPackOffSource ||
            pureEventPackProbeStats.find("\"policy\": \"probe\"") == std::string::npos ||
            pureEventPackProbeStats.find("\"applied\": false") == std::string::npos ||
            pureEventPackProbeStats.find("\"validation_passed\": true") == std::string::npos)
        {
            return fail("pure-event word-pack probe should report a validated candidate without changing source");
        }
        const auto packProbeBaselinePureWords =
            jsonUnsignedField(pureEventPackProbeStats, "baseline_pure_word_count");
        const auto packProbeCandidatePureWords =
            jsonUnsignedField(pureEventPackProbeStats, "candidate_pure_word_count");
        const auto packProbeAddedPureWords =
            jsonUnsignedField(pureEventPackProbeStats, "added_pure_word_count");
        const auto packProbeLostPureWords =
            jsonUnsignedField(pureEventPackProbeStats, "lost_pure_word_count");
        const auto packProbeMovedSupernodes =
            jsonUnsignedField(pureEventPackProbeStats, "moved_supernode_count");
        if (!packProbeBaselinePureWords || !packProbeCandidatePureWords || !packProbeAddedPureWords ||
            !packProbeLostPureWords || !packProbeMovedSupernodes ||
            *packProbeCandidatePureWords <= *packProbeBaselinePureWords ||
            *packProbeAddedPureWords != *packProbeCandidatePureWords - *packProbeBaselinePureWords ||
            *packProbeLostPureWords != 0u || *packProbeMovedSupernodes == 0u)
        {
            return fail("pure-event word-pack probe should discover a lossless packing opportunity");
        }
        if (pureEventPackTargetedStats.find("\"policy\": \"targeted\"") == std::string::npos ||
            pureEventPackTargetedStats.find("\"applied\": true") == std::string::npos ||
            pureEventPackTargetedStats.find("\"validation_passed\": true") == std::string::npos ||
            jsonUnsignedField(pureEventPackTargetedStats, "baseline_pure_word_count") !=
                packProbeBaselinePureWords ||
            jsonUnsignedField(pureEventPackTargetedStats, "candidate_pure_word_count") !=
                packProbeCandidatePureWords ||
            jsonUnsignedField(pureEventPackTargetedStats, "moved_supernode_count") !=
                packProbeMovedSupernodes ||
            pureEventPackTargetedSource == pureEventPackOffSource ||
            countSubstring(pureEventPackTargetedSource, pureEventMarker) <=
                countSubstring(pureEventPackOffSource, pureEventMarker))
        {
            return fail("targeted pure-event word packing should apply and create additional bypass words");
        }
        if (pureEventPackTargetedSource != pureEventPackTargetedRepeatSource ||
            pureEventPackTargetedStats != pureEventPackTargetedRepeatStats)
        {
            return fail("targeted pure-event word packing should be deterministic across emits");
        }

        const std::string pureEventPackRemainderOffSource =
            readPureEventGenerated(*pureEventPackRemainderOffDir);
        const std::string pureEventPackRemainderProbeSource =
            readPureEventGenerated(*pureEventPackRemainderProbeDir);
        const std::string pureEventPackRemainderTargetedSource =
            readPureEventGenerated(*pureEventPackRemainderTargetedDir);
        const std::string pureEventPackRemainderProbeStats =
            readFile(*pureEventPackRemainderProbeDir / "grhsim_emit_stats.json");
        const std::string pureEventPackRemainderTargetedStats =
            readFile(*pureEventPackRemainderTargetedDir / "grhsim_emit_stats.json");
        const auto packRemainderBaselinePureWords =
            jsonUnsignedField(pureEventPackRemainderProbeStats, "baseline_pure_word_count");
        const auto packRemainderCandidatePureWords =
            jsonUnsignedField(pureEventPackRemainderProbeStats, "candidate_pure_word_count");
        const auto packRemainderAddedPureWords =
            jsonUnsignedField(pureEventPackRemainderProbeStats, "added_pure_word_count");
        if (pureEventPackRemainderProbeSource != pureEventPackRemainderOffSource ||
            !packRemainderBaselinePureWords || !packRemainderCandidatePureWords ||
            !packRemainderAddedPureWords ||
            *packRemainderCandidatePureWords <= *packRemainderBaselinePureWords ||
            *packRemainderAddedPureWords == 0u ||
            pureEventPackRemainderTargetedStats.find("\"applied\": true") == std::string::npos ||
            countSubstring(pureEventPackRemainderTargetedSource, pureEventMarker) <=
                countSubstring(pureEventPackRemainderOffSource, pureEventMarker))
        {
            return fail("pure-event word packing should preserve partial event-key remainders");
        }

        const std::string pureEventPackEstimatedLineOffSource =
            readPureEventGenerated(*pureEventPackEstimatedLineOffDir);
        const std::string pureEventPackEstimatedLineProbeSource =
            readPureEventGenerated(*pureEventPackEstimatedLineProbeDir);
        const std::string pureEventPackEstimatedLineTargetedSource =
            readPureEventGenerated(*pureEventPackEstimatedLineTargetedDir);
        const std::string pureEventPackEstimatedLineProbeStats =
            readFile(*pureEventPackEstimatedLineProbeDir / "grhsim_emit_stats.json");
        const std::string pureEventPackEstimatedLineTargetedStats =
            readFile(*pureEventPackEstimatedLineTargetedDir / "grhsim_emit_stats.json");
        const auto estimatedLineBaselinePureWords =
            jsonUnsignedField(pureEventPackEstimatedLineProbeStats, "baseline_pure_word_count");
        const auto estimatedLineCandidatePureWords =
            jsonUnsignedField(pureEventPackEstimatedLineProbeStats, "candidate_pure_word_count");
        const auto frozenBaselineEstimatedLines =
            jsonUnsignedField(pureEventPackEstimatedLineTargetedStats,
                              "frozen_batch_baseline_estimated_lines");
        const auto frozenRebuiltEstimatedLines =
            jsonUnsignedField(pureEventPackEstimatedLineTargetedStats,
                              "frozen_batch_rebuilt_estimated_lines");
        const auto frozenEstimatedLineChangedBatchCount =
            jsonUnsignedField(pureEventPackEstimatedLineTargetedStats,
                              "frozen_batch_estimated_line_changed_batch_count");
        const auto frozenMaxAbsEstimatedLineDelta =
            jsonUnsignedField(pureEventPackEstimatedLineTargetedStats,
                              "frozen_batch_max_abs_estimated_line_delta");
        bool frozenEstimatedLineDriftMatches = false;
        if (frozenBaselineEstimatedLines && frozenRebuiltEstimatedLines &&
            frozenEstimatedLineChangedBatchCount && frozenMaxAbsEstimatedLineDelta)
        {
            const std::size_t totalDelta =
                *frozenBaselineEstimatedLines > *frozenRebuiltEstimatedLines
                    ? *frozenBaselineEstimatedLines - *frozenRebuiltEstimatedLines
                    : *frozenRebuiltEstimatedLines - *frozenBaselineEstimatedLines;
            frozenEstimatedLineDriftMatches =
                *frozenEstimatedLineChangedBatchCount == 1u && totalDelta != 0u &&
                *frozenMaxAbsEstimatedLineDelta == totalDelta;
        }
        if (pureEventPackEstimatedLineProbeSource != pureEventPackEstimatedLineOffSource ||
            !estimatedLineBaselinePureWords || !estimatedLineCandidatePureWords ||
            *estimatedLineBaselinePureWords != 0u ||
            *estimatedLineCandidatePureWords <= *estimatedLineBaselinePureWords ||
            pureEventPackEstimatedLineTargetedStats.find("\"applied\": true") == std::string::npos ||
            !frozenEstimatedLineDriftMatches ||
            countSubstring(pureEventPackEstimatedLineTargetedSource, pureEventMarker) <=
                countSubstring(pureEventPackEstimatedLineOffSource, pureEventMarker))
        {
            return fail("targeted pure-event word packing should report the expected frozen-batch line drift");
        }

        const auto commitBatchBlock = [](std::string_view source) -> std::optional<std::string_view> {
            const std::size_t method = source.find("void GrhSIM_top::eval_commit_batch_1()");
            const std::size_t open = source.find('{', method);
            const std::size_t close = findMatchingBrace(source, open);
            if (method == std::string_view::npos || open == std::string_view::npos ||
                close == std::string_view::npos)
            {
                return std::nullopt;
            }
            return source.substr(method, close - method + 1u);
        };
        const auto collectCommitStructure = [](std::string_view block,
                                               std::string_view marker,
                                               char terminator) {
            std::vector<std::string> values;
            std::size_t pos = 0u;
            while ((pos = block.find(marker, pos)) != std::string_view::npos)
            {
                const std::size_t begin = pos + marker.size();
                const std::size_t end = block.find(terminator, begin);
                if (end == std::string_view::npos)
                {
                    values.clear();
                    return values;
                }
                values.emplace_back(block.substr(begin, end - begin));
                pos = end + 1u;
            }
            return values;
        };
        const auto estimatedLineOffCommit = commitBatchBlock(pureEventPackEstimatedLineOffSource);
        const auto estimatedLineTargetedCommit =
            commitBatchBlock(pureEventPackEstimatedLineTargetedSource);
        if (!estimatedLineOffCommit || !estimatedLineTargetedCommit)
        {
            return fail("estimated-line fixture should emit one identifiable commit batch");
        }
        const auto offCommitMembers =
            collectCommitStructure(*estimatedLineOffCommit, "// Supernode ", ':');
        const auto targetedCommitMembers =
            collectCommitStructure(*estimatedLineTargetedCommit, "// Supernode ", ':');
        const std::size_t offClkWrite = estimatedLineOffCommit->find("pack_estimate_clk_write");
        const std::size_t offAuxWrite = estimatedLineOffCommit->find("pack_estimate_aux_write");
        const std::size_t targetedClkWrite =
            estimatedLineTargetedCommit->find("pack_estimate_clk_write");
        const std::size_t targetedAuxWrite =
            estimatedLineTargetedCommit->find("pack_estimate_aux_write");
        if (offCommitMembers.size() != 2u || offCommitMembers != targetedCommitMembers ||
            offClkWrite == std::string_view::npos || offAuxWrite == std::string_view::npos ||
            targetedClkWrite == std::string_view::npos || targetedAuxWrite == std::string_view::npos ||
            !(offClkWrite < offAuxWrite) || !(targetedClkWrite < targetedAuxWrite) ||
            *estimatedLineOffCommit == *estimatedLineTargetedCommit)
        {
            return fail("estimated-line drift must preserve nontrivial commit membership and order");
        }

        const auto expectPackEmitFailure = [&](std::string_view suffix,
                                               std::string_view policy,
                                               bool bypass,
                                               std::optional<std::size_t> maxMovedSupernodePpm,
                                               std::optional<std::size_t> maxChangedWordPpm,
                                               bool removeDag,
                                               std::string_view expectedDiagnostic,
                                               std::optional<std::string_view> rawMaxMovedSupernodePpm = std::nullopt,
                                               std::optional<std::string_view> rawMaxChangedWordPpm = std::nullopt) {
            const std::filesystem::path dir = pureEventRoot.string() + "_" + std::string(suffix);
            std::filesystem::remove_all(dir);
            Design fixture = buildPureEventWordBypassDesign(PureEventWordFixtureMode::kAlternatingEvents);
            EmitDiagnostics fixtureDiag;
            EmitResult fixtureResult;
            if (!emitWithActivitySchedule(fixture,
                                          dir,
                                          fixtureDiag,
                                          fixtureResult,
                                          pureEventSchedule,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          bypass,
                                          false,
                                          100000u,
                                          100000u,
                                          policy,
                                          maxMovedSupernodePpm,
                                          maxChangedWordPpm,
                                          removeDag,
                                          rawMaxMovedSupernodePpm,
                                          rawMaxChangedWordPpm))
            {
                return false;
            }
            return !fixtureResult.success && fixtureDiag.hasError() &&
                   diagnosticsContain(fixtureDiag, expectedDiagnostic);
        };
        if (!expectPackEmitFailure("pack_invalid_policy",
                                   "invalid",
                                   true,
                                   std::nullopt,
                                   std::nullopt,
                                   false,
                                   "invalid pure_event_word_pack_policy: invalid") ||
            !expectPackEmitFailure("pack_moved_ppm_overflow",
                                   "probe",
                                   true,
                                   1000001u,
                                   std::nullopt,
                                   false,
                                   "pure_event_word_pack_max_moved_supernode_ppm") ||
            !expectPackEmitFailure("pack_changed_ppm_overflow",
                                   "probe",
                                   true,
                                   std::nullopt,
                                   1000001u,
                                   false,
                                   "pure_event_word_pack_max_changed_word_ppm") ||
            !expectPackEmitFailure("pack_moved_ppm_malformed",
                                   "probe",
                                   true,
                                   std::nullopt,
                                   std::nullopt,
                                   false,
                                   "pure_event_word_pack_max_moved_supernode_ppm",
                                   "not-a-number") ||
            !expectPackEmitFailure("pack_changed_ppm_malformed",
                                   "probe",
                                   true,
                                   std::nullopt,
                                   std::nullopt,
                                   false,
                                   "pure_event_word_pack_max_changed_word_ppm",
                                   std::nullopt,
                                   "12ppm") ||
            !expectPackEmitFailure("pack_targeted_without_bypass",
                                   "targeted",
                                   false,
                                   std::nullopt,
                                   std::nullopt,
                                   false,
                                   "pure_event_word_pack_policy=targeted requires pure_event_compute_word_bypass=true") ||
            !expectPackEmitFailure("pack_probe_without_dag",
                                   "probe",
                                   true,
                                   std::nullopt,
                                   std::nullopt,
                                   true,
                                   "missing activity-schedule dag") ||
            !expectPackEmitFailure("pack_targeted_without_dag",
                                   "targeted",
                                   true,
                                   std::nullopt,
                                   std::nullopt,
                                   true,
                                   "missing activity-schedule dag") ||
            !expectPackEmitFailure("pack_moved_budget",
                                   "targeted",
                                   true,
                                   0u,
                                   1000000u,
                                   false,
                                   "budget") ||
            !expectPackEmitFailure("pack_changed_budget",
                                   "targeted",
                                   true,
                                   1000000u,
                                   0u,
                                   false,
                                   "budget"))
        {
            return fail("pure-event word-pack option validation or rejection contract failed");
        }

        {
            const std::filesystem::path dir = pureEventRoot.string() + "_pack_off_without_dag";
            std::filesystem::remove_all(dir);
            Design fixture = buildPureEventWordBypassDesign(PureEventWordFixtureMode::kAlternatingEvents);
            EmitDiagnostics fixtureDiag;
            EmitResult fixtureResult;
            if (!emitWithActivitySchedule(fixture,
                                          dir,
                                          fixtureDiag,
                                          fixtureResult,
                                          pureEventSchedule,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          false,
                                          true,
                                          false,
                                          100000u,
                                          100000u,
                                          "off",
                                          std::nullopt,
                                          std::nullopt,
                                          true) ||
                !fixtureResult.success || fixtureDiag.hasError())
            {
                return fail("pure-event word-pack off policy should not require activity-schedule dag");
            }
        }
        const std::size_t pureEventMarkerCount = countSubstring(pureEventEnabledSched, pureEventMarker);
        if (pureEventMarkerCount == 0)
        {
            return fail("pure-event compute-word enabled fixture should emit at least one wrapper");
        }
        const std::size_t pureEventMarkerPos = pureEventEnabledSched.find(pureEventMarker);
        const std::size_t pureEventClearPos = pureEventEnabledSched.rfind("~clearMask", pureEventMarkerPos);
        const std::size_t pureEventWrapperIfPos =
            pureEventEnabledSched.find("if (grhsim_pure_event_word_hit_", pureEventMarkerPos);
        const std::size_t pureEventWrapperOpen =
            pureEventEnabledSched.find('{', pureEventWrapperIfPos);
        const std::size_t pureEventWrapperClose = findMatchingBrace(pureEventEnabledSched, pureEventWrapperOpen);
        const std::size_t pureEventFirstEntry = pureEventEnabledSched.find("// Supernode ", pureEventWrapperOpen);
        const std::size_t pureEventRestore = pureEventEnabledSched.find(" | activeWordFlags);", pureEventFirstEntry);
        if (pureEventClearPos == std::string::npos || pureEventWrapperIfPos == std::string::npos ||
            pureEventWrapperClose == std::string::npos || pureEventFirstEntry == std::string::npos ||
            pureEventRestore == std::string::npos || pureEventClearPos >= pureEventMarkerPos ||
            pureEventMarkerPos >= pureEventWrapperIfPos || pureEventWrapperIfPos >= pureEventFirstEntry ||
            pureEventFirstEntry >= pureEventRestore || pureEventRestore >= pureEventWrapperClose)
        {
            return fail("pure-event compute-word wrapper should follow clear and contain entries plus restore");
        }
        const std::string_view pureEventFirstWrapper =
            std::string_view(pureEventEnabledSched).substr(pureEventWrapperIfPos,
                                                           pureEventWrapperClose - pureEventWrapperIfPos + 1u);
        if (countSubstring(pureEventFirstWrapper, "event_edge_slots_[") < 1u)
        {
            return fail("pure-event compute-word wrapper should retain inner exact-event guards");
        }
        if (countSubstring(pureEventEnabledSched,
                           "const volatile bool grhsim_pure_event_word_hit_") != pureEventMarkerCount)
        {
            return fail("sparse pure-event batches should use one volatile hit per wrapper");
        }

        const std::string pureEventSparseOneSched =
            readFiles(collectSchedFiles(*pureEventSparseOneDir, "grhsim_top_sched_"));
        const std::string pureEventSparseTwoSched =
            readFiles(collectSchedFiles(*pureEventSparseTwoDir, "grhsim_top_sched_"));
        const std::string pureEventDenseThreeSched =
            readFiles(collectSchedFiles(*pureEventDenseThreeDir, "grhsim_top_sched_"));
        constexpr std::string_view pureEventVolatileHit =
            "const volatile bool grhsim_pure_event_word_hit_";
        constexpr std::string_view pureEventDirectOuter =
            "if (event_edge_slots_[0] == grhsim_event_edge_kind::posedge) {";
        if (countSubstring(pureEventSparseOneSched, pureEventMarker) != 1u ||
            countSubstring(pureEventSparseOneSched, pureEventVolatileHit) != 1u ||
            countSubstring(pureEventSparseTwoSched, pureEventMarker) != 2u ||
            countSubstring(pureEventSparseTwoSched, pureEventVolatileHit) != 2u ||
            countSubstring(pureEventDenseThreeSched, pureEventMarker) != 3u ||
            countSubstring(pureEventDenseThreeSched, pureEventVolatileHit) != 0u ||
            countSubstring(pureEventDenseThreeSched, pureEventDirectOuter) != 3u)
        {
            return fail("pure-event sparse predicate should switch at the two-word batch boundary");
        }

        const std::string pureEventProfileOnlySource = readPureEventGenerated(*pureEventProfileOnlyDir);
        const std::string pureEventProfileOnlySched =
            readFiles(collectSchedFiles(*pureEventProfileOnlyDir, "grhsim_top_sched_"));
        const std::string pureEventProfileBypassSource = readPureEventGenerated(*pureEventProfileBypassDir);
        const std::string pureEventProfileBypassSched =
            readFiles(collectSchedFiles(*pureEventProfileBypassDir, "grhsim_top_sched_"));
        if (pureEventProfileOnlySource.find("kPureEventComputeWordEligibleCount = 2u") == std::string::npos ||
            pureEventProfileOnlySource.find("pure_event_word_active_hit_by_batch_") == std::string::npos ||
            pureEventProfileOnlySource.find("pure_event_word_active_miss_by_batch_") == std::string::npos ||
            pureEventProfileOnlySched.find(pureEventMarker) != std::string::npos ||
            countSubstring(pureEventProfileOnlySched, "++pure_event_word_active_hit_by_batch_[") != 2u ||
            countSubstring(pureEventProfileOnlySched, "++pure_event_word_active_miss_by_batch_[") != 2u)
        {
            return fail("pure-event profile-only source should count two eligible words without bypass wrappers");
        }
        if (countSubstring(pureEventProfileBypassSched, pureEventMarker) != 2u ||
            countSubstring(pureEventProfileBypassSched,
                           "const volatile bool grhsim_pure_event_word_hit_") != 2u ||
            countSubstring(pureEventProfileBypassSched, "++pure_event_word_active_hit_by_batch_[") != 2u ||
            pureEventProfileBypassSource.find("PureEventComputeWordProfile") == std::string::npos)
        {
            return fail("combined pure-event profile and bypass should share one hit temporary per eligible word");
        }

        const std::string pureEventOnceSched =
            readFiles(collectSchedFiles(*pureEventOnceDir, "grhsim_top_sched_"));
        const std::string pureEventMultiSched =
            readFiles(collectSchedFiles(*pureEventMultiDir, "grhsim_top_sched_"));
        if (pureEventOnceSched.find(pureEventMarker) != std::string::npos ||
            pureEventMultiSched.find(pureEventMarker) != std::string::npos ||
            readPureEventGenerated(*pureEventOnceDir).find("kPureEventComputeWordEligibleCount = 0u") ==
                std::string::npos ||
            readPureEventGenerated(*pureEventMultiDir).find("kPureEventComputeWordEligibleCount = 0u") ==
                std::string::npos)
        {
            return fail("once-only and multi-event compute words must not use the pure-event bypass");
        }
        const std::string pureEventFullpassSched =
            readFiles(collectSchedFiles(*pureEventFullpassDir, "grhsim_top_sched_"));
        if (countSubstring(pureEventFullpassSched, pureEventMarker) != pureEventMarkerCount)
        {
            return fail("fullpass methods must not add pure-event compute-word wrappers");
        }

        const std::string pureEventFullWordConsumeSched =
            readFiles(collectSchedFiles(*pureEventFullWordConsumeDir, "grhsim_top_sched_"));
        if (readPureEventGenerated(*pureEventFullWordConsumeDir)
                .find("kPureEventComputeWordEligibleCount = 0u") == std::string::npos)
        {
            return fail("full-active-word consume fixture should have zero profile-eligible words");
        }
        constexpr std::string_view pureEventDispatchMarker =
            "constexpr std::uint8_t dispatchMask = UINT8_C(";
        bool sawPureEventFullWord = false;
        std::size_t pureEventDispatchPos = 0;
        while ((pureEventDispatchPos = pureEventFullWordConsumeSched.find(pureEventDispatchMarker,
                                                                          pureEventDispatchPos)) !=
               std::string::npos)
        {
            const std::size_t maskBegin = pureEventDispatchPos + pureEventDispatchMarker.size();
            const std::size_t maskEnd = pureEventFullWordConsumeSched.find(");", maskBegin);
            if (maskEnd == std::string::npos)
            {
                return fail("pure-event full-word consume emitted a malformed dispatch mask");
            }
            const unsigned mask = static_cast<unsigned>(
                std::stoul(pureEventFullWordConsumeSched.substr(maskBegin, maskEnd - maskBegin)));
            const std::size_t nextDispatch =
                pureEventFullWordConsumeSched.find(pureEventDispatchMarker, maskEnd);
            const std::string_view block = std::string_view(pureEventFullWordConsumeSched).substr(
                pureEventDispatchPos,
                nextDispatch == std::string::npos ? nextDispatch : nextDispatch - pureEventDispatchPos);
            if (mask == 255u)
            {
                sawPureEventFullWord = true;
                if (block.find(pureEventMarker) != std::string_view::npos)
                {
                    return fail("full-active-word consume words must not use the pure-event bypass");
                }
            }
            pureEventDispatchPos = maskEnd;
        }
        if (!sawPureEventFullWord)
        {
            return fail("pure-event fixture should contain a complete active word");
        }

        const auto runPureEventHarness = [&](const std::filesystem::path &dir,
                                             bool expectProfile = false) -> std::optional<std::string>
        {
            const std::filesystem::path harnessPath = dir / "grhsim_top_harness.cpp";
            {
                std::ofstream harness(harnessPath);
                if (!harness.is_open())
                {
                    return std::nullopt;
                }
                harness << "#include \"grhsim_top.hpp\"\n";
                harness << "#include <cstdint>\n\n";
                harness << "int main()\n{\n";
                harness << "    GrhSIM_top sim;\n";
                harness << "    sim.init();\n";
                harness << "    sim.clk = false;\n";
                harness << "    sim.aux_clk = true;\n";
                harness << "    sim.data = static_cast<std::uint8_t>(1);\n";
                harness << "    sim.eval();\n";
                if (expectProfile)
                {
                    harness << "    sim.set_runtime_profile_enabled(true);\n";
                }
                harness << "    sim.data = static_cast<std::uint8_t>(2);\n";
                harness << "    sim.eval();\n";
                harness << "    sim.clk = true;\n";
                harness << "    sim.eval();\n";
                harness << "    sim.data = static_cast<std::uint8_t>(3);\n";
                harness << "    sim.eval();\n";
                harness << "    sim.clk = false;\n";
                harness << "    sim.eval();\n";
                harness << "    sim.clk = true;\n";
                harness << "    sim.eval();\n";
                if (expectProfile)
                {
                    harness << "    const auto profile = sim.pure_event_compute_word_profile();\n";
                    harness << "    if (profile.eligibleWordCount != UINT64_C(2)) return 2;\n";
                    harness << "    if (profile.activeHitCount != UINT64_C(4)) return 3;\n";
                    harness << "    if (profile.activeMissCount != UINT64_C(6)) return 4;\n";
                    harness << "    sim.dump_runtime_profile();\n";
                }
                harness << "    return sim.data_out == static_cast<std::uint8_t>(3) ? 0 : 1;\n";
                harness << "}\n";
            }
            const std::vector<std::filesystem::path> stateFiles = collectSchedFiles(dir, "grhsim_top_state");
            const std::vector<std::filesystem::path> schedFiles = collectSchedFiles(dir, "grhsim_top_sched_");
            const std::filesystem::path exe = dir / "grhsim_top_harness";
            std::string command = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + dir.string();
            for (const auto &stateFile : stateFiles)
            {
                command += " " + stateFile.string();
            }
            command += " " + (dir / "grhsim_top_eval.cpp").string();
            for (const auto &schedFile : schedFiles)
            {
                command += " " + schedFile.string();
            }
            command += " " + harnessPath.string() + " -o " + exe.string();
            if (std::system(command.c_str()) != 0)
            {
                return std::nullopt;
            }
            const std::filesystem::path log = dir / "grhsim_top_harness.log";
            const std::filesystem::path profileTsv = dir / "pure_event_word_profile.tsv";
            std::filesystem::remove(profileTsv);
            command = (expectProfile
                           ? "WOLVRIX_GRHSIM_PURE_EVENT_WORD_TSV=" + profileTsv.string() + " "
                           : std::string()) +
                      exe.string() + " > " + log.string() + " 2>&1";
            if (std::system(command.c_str()) != 0)
            {
                return std::nullopt;
            }
            return readFile(log);
        };
        const auto pureEventDefaultLog = runPureEventHarness(*pureEventDefaultDir);
        const auto pureEventEnabledLog = runPureEventHarness(*pureEventEnabledDir);
        if (!pureEventDefaultLog || !pureEventEnabledLog || *pureEventDefaultLog != *pureEventEnabledLog ||
            countSubstring(*pureEventEnabledLog, "pure-event=") != 32u)
        {
            return fail("pure-event compute-word hit/miss harness output mismatch");
        }
        const auto pureEventPackOffLog = runPureEventHarness(*pureEventPackOffDir);
        const auto pureEventPackTargetedLog = runPureEventHarness(*pureEventPackTargetedDir);
        if (!pureEventPackOffLog || !pureEventPackTargetedLog ||
            *pureEventPackOffLog != *pureEventPackTargetedLog ||
            pureEventPackTargetedLog->find("pure-event=") == std::string::npos)
        {
            return fail("targeted pure-event word packing changed functional harness output");
        }
        const auto pureEventPackRemainderOffLog = runPureEventHarness(*pureEventPackRemainderOffDir);
        const auto pureEventPackRemainderTargetedLog =
            runPureEventHarness(*pureEventPackRemainderTargetedDir);
        if (!pureEventPackRemainderOffLog || !pureEventPackRemainderTargetedLog ||
            *pureEventPackRemainderOffLog != *pureEventPackRemainderTargetedLog ||
            pureEventPackRemainderTargetedLog->find("pure-event=") == std::string::npos)
        {
            return fail("targeted pure-event word packing changed remainder-fixture output");
        }
        const auto pureEventPackEstimatedLineOffLog =
            runPureEventHarness(*pureEventPackEstimatedLineOffDir);
        const auto pureEventPackEstimatedLineTargetedLog =
            runPureEventHarness(*pureEventPackEstimatedLineTargetedDir);
        if (!pureEventPackEstimatedLineOffLog || !pureEventPackEstimatedLineTargetedLog ||
            *pureEventPackEstimatedLineOffLog != *pureEventPackEstimatedLineTargetedLog ||
            pureEventPackEstimatedLineTargetedLog->find("pack-estimate=") == std::string::npos)
        {
            return fail("targeted pure-event word packing changed estimated-line fixture output");
        }
        const auto pureEventProfileOnlyLog = runPureEventHarness(*pureEventProfileOnlyDir, true);
        const auto pureEventProfileBypassLog = runPureEventHarness(*pureEventProfileBypassDir, true);
        const auto validatePureEventProfile = [&](const std::filesystem::path &dir,
                                                  const std::optional<std::string> &log) {
            if (!log || countSubstring(*log, "pure-event=") != 32u ||
                log->find("eligible=2 hit=4 miss=6 total=10 miss_ratio=0.600000") == std::string::npos)
            {
                return false;
            }
            const std::string tsv = readFile(dir / "pure_event_word_profile.tsv");
            return tsv.starts_with("batch_id\teligible_words\tactive_hit\tactive_miss\tactive_total\n") &&
                   countSubstring(tsv, "\t1\t2\t3\t5\n") == 2u;
        };
        if (!validatePureEventProfile(*pureEventProfileOnlyDir, pureEventProfileOnlyLog) ||
            !validatePureEventProfile(*pureEventProfileBypassDir, pureEventProfileBypassLog))
        {
            return fail("pure-event compute-word dynamic profile counts or TSV mismatch");
        }

        const std::filesystem::path systemTaskDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_systemtask";
        std::filesystem::remove_all(systemTaskDir);
        const std::filesystem::path systemTaskFilePath = systemTaskDir / "system_task_output.log";
        Design systemTaskDesign = buildSystemTaskDesign(systemTaskFilePath.string());
        EmitDiagnostics systemTaskDiag;
        EmitResult systemTaskResult;
        if (!emitWithActivitySchedule(systemTaskDesign, systemTaskDir, systemTaskDiag, systemTaskResult))
        {
            return fail("system-task activity-schedule pass failed");
        }
        if (!systemTaskResult.success || systemTaskDiag.hasError())
        {
            return fail("system-task emit failed");
        }
        const std::filesystem::path systemTaskHeaderPath = systemTaskDir / "grhsim_top.hpp";
        const std::filesystem::path systemTaskStatePath = systemTaskDir / "grhsim_top_state.cpp";
        const std::filesystem::path systemTaskEvalPath = systemTaskDir / "grhsim_top_eval.cpp";
        const std::vector<std::filesystem::path> systemTaskStateFiles =
            collectSchedFiles(systemTaskDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> systemTaskSchedFiles =
            collectSchedFiles(systemTaskDir, "grhsim_top_sched_");
        if (!std::filesystem::exists(systemTaskHeaderPath) || !std::filesystem::exists(systemTaskStatePath) ||
            !std::filesystem::exists(systemTaskEvalPath) || systemTaskStateFiles.empty() || systemTaskSchedFiles.empty())
        {
            return fail("system-task artifacts missing");
        }
        const std::string systemTaskSchedText = readFiles(systemTaskSchedFiles);
        if (systemTaskSchedText.find("(cond8) != 0") == std::string::npos)
        {
            return fail("system-task multi-bit condition should emit scalar truthiness check");
        }
        const std::filesystem::path systemTaskHarnessPath = systemTaskDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(systemTaskHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create system-task harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n";
            harness << "#include <string>\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    {\n";
            harness << "        GrhSIM_top sim;\n";
            harness << "        sim.init();\n";
            harness << "        sim.clk = false;\n";
            harness << "        sim.cond8 = static_cast<std::uint8_t>(0);\n";
            harness << "        sim.data = static_cast<std::uint8_t>(42);\n";
            harness << "        sim.eval();\n";
            harness << "        if (sim.dumpfile_path() != std::string(\"waves.out\")) return 1;\n";
            harness << "        if (!sim.dumpvars_enabled()) return 2;\n";
            harness << "        if (sim.file_error != static_cast<std::uint32_t>(0)) return 3;\n";
            harness << "        sim.clk = true;\n";
            harness << "        sim.eval();\n";
            harness << "        if (sim.data_out != static_cast<std::uint8_t>(42)) return 4;\n";
            harness << "        if (sim.file_error != static_cast<std::uint32_t>(0)) return 5;\n";
            harness << "        sim.clk = false;\n";
            harness << "        sim.eval();\n";
            harness << "        sim.cond8 = static_cast<std::uint8_t>(2);\n";
            harness << "        sim.clk = true;\n";
            harness << "        sim.eval();\n";
            harness << "    }\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }

        const std::filesystem::path systemTaskHarnessExe = systemTaskDir / "grhsim_top_harness";
        std::string systemTaskCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + systemTaskDir.string();
        for (const auto &stateFile : systemTaskStateFiles)
        {
            systemTaskCompileCmd += " " + stateFile.string();
        }
        systemTaskCompileCmd += " " + systemTaskEvalPath.string();
        for (const auto &schedPath : systemTaskSchedFiles)
        {
            systemTaskCompileCmd += " " + schedPath.string();
        }
        systemTaskCompileCmd += " " + systemTaskHarnessPath.string() + " -o " + systemTaskHarnessExe.string();
        if (std::system(systemTaskCompileCmd.c_str()) != 0)
        {
            return fail("system-task harness failed to compile");
        }
        const std::filesystem::path systemTaskHarnessLog = systemTaskDir / "grhsim_top_harness.log";
        const std::string runSystemTaskHarnessCmd =
            systemTaskHarnessExe.string() + " > " + systemTaskHarnessLog.string() + " 2>&1";
        if (std::system(runSystemTaskHarnessCmd.c_str()) != 0)
        {
            return fail("system-task harness failed to run");
        }
        const std::string systemTaskLog = readFile(systemTaskHarnessLog);
        const std::string systemTaskFileText = readFile(systemTaskFilePath);
        auto countSubstring = [](std::string_view text, std::string_view needle) -> std::size_t
        {
            if (needle.empty())
            {
                return 0;
            }
            std::size_t count = 0;
            std::size_t pos = 0;
            while ((pos = text.find(needle, pos)) != std::string_view::npos)
            {
                ++count;
                pos += needle.size();
            }
            return count;
        };
        if (countSubstring(systemTaskLog, "init-once") < 1)
        {
            return fail("system-task initial non-timed display should run at least once");
        }
        if (countSubstring(systemTaskLog, "init-edge=42") != 1)
        {
            return fail("system-task initial timed display should trigger on edge");
        }
        if (countSubstring(systemTaskLog, "d=42 h=2a b=101010 s=ok r=3.25") < 1)
        {
            return fail("system-task formatted display output mismatch");
        }
        if (systemTaskLog.find("[info] info=42") == std::string::npos ||
            systemTaskLog.find("[warning] warn=42") == std::string::npos ||
            systemTaskLog.find("[error] err=42") == std::string::npos)
        {
            return fail("system-task severity outputs missing");
        }
        if (systemTaskLog.find("final=42") == std::string::npos)
        {
            return fail("system-task final output missing");
        }
        if (countSubstring(systemTaskLog, "cond=42") != 1)
        {
            return fail("system-task multi-bit conditional display should trigger exactly once");
        }
        if (countSubstring(systemTaskFileText, "fw=42") < 1 || systemTaskFileText.find("|fd=42") == std::string::npos)
        {
            return fail("system-task file output mismatch");
        }

        struct TerminatingTaskCase
        {
            std::string name;
            int exitCode = 0;
        };
        for (const TerminatingTaskCase &taskCase :
             std::vector<TerminatingTaskCase>{{"finish", 7}, {"stop", 9}, {"fatal", 11}})
        {
            const std::filesystem::path termDir =
                std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / ("grhsim_cpp_systemtask_" + taskCase.name);
            std::filesystem::remove_all(termDir);
            Design termDesign =
                buildTerminatingSystemTaskDesign(taskCase.name, taskCase.exitCode, taskCase.name);
            EmitDiagnostics termDiag;
            EmitResult termResult;
            if (!emitWithActivitySchedule(termDesign, termDir, termDiag, termResult))
            {
                return fail("terminating system-task activity-schedule pass failed for " + taskCase.name);
            }
            if (!termResult.success || termDiag.hasError())
            {
                return fail("terminating system-task emit failed for " + taskCase.name);
            }
            const std::filesystem::path termStatePath = termDir / "grhsim_top_state.cpp";
            const std::filesystem::path termEvalPath = termDir / "grhsim_top_eval.cpp";
            const std::vector<std::filesystem::path> termStateFiles = collectSchedFiles(termDir, "grhsim_top_state");
            const std::vector<std::filesystem::path> termSchedFiles =
                collectSchedFiles(termDir, "grhsim_top_sched_");
            const std::filesystem::path termHarnessPath = termDir / "grhsim_top_harness.cpp";
            {
                std::ofstream harness(termHarnessPath);
                if (!harness.is_open())
                {
                    return fail("Failed to create terminating system-task harness for " + taskCase.name);
                }
                harness << "#include \"grhsim_top.hpp\"\n";
                harness << "#include <cstdint>\n\n";
                harness << "int main()\n";
                harness << "{\n";
                harness << "    GrhSIM_top sim;\n";
                harness << "    sim.init();\n";
                harness << "    sim.data = static_cast<std::uint8_t>(42);\n";
                harness << "    sim.eval();\n";
                harness << "    return 0;\n";
                harness << "}\n";
            }
            const std::filesystem::path termHarnessExe = termDir / "grhsim_top_harness";
            std::string termCompileCmd = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + termDir.string();
            for (const auto &stateFile : termStateFiles)
            {
                termCompileCmd += " " + stateFile.string();
            }
            termCompileCmd += " " + termEvalPath.string();
            for (const auto &schedPath : termSchedFiles)
            {
                termCompileCmd += " " + schedPath.string();
            }
            termCompileCmd += " " + termHarnessPath.string() + " -o " + termHarnessExe.string();
            if (std::system(termCompileCmd.c_str()) != 0)
            {
                return fail("terminating system-task harness failed to compile for " + taskCase.name);
            }
            const std::filesystem::path termHarnessLog = termDir / "grhsim_top_harness.log";
            const std::string runTermHarnessCmd =
                termHarnessExe.string() + " > " + termHarnessLog.string() + " 2>&1";
            const int termRunRc = std::system(runTermHarnessCmd.c_str());
            if (termRunRc != taskCase.exitCode && termRunRc != (taskCase.exitCode << 8))
            {
                return fail("terminating system-task exit code mismatch for " + taskCase.name);
            }
            const std::string termLog = readFile(termHarnessLog);
            if (termLog.find(taskCase.name + "=42") == std::string::npos)
            {
                return fail("terminating system-task main output missing for " + taskCase.name);
            }
            if (termLog.find("final-" + taskCase.name + "=42") == std::string::npos)
            {
                return fail("terminating system-task final output missing for " + taskCase.name);
            }
        }

        const std::filesystem::path dpiDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_dpi";
        std::filesystem::remove_all(dpiDir);
        Design dpiDesign = buildDpiCallDesign();
        EmitDiagnostics dpiDiag;
        EmitResult dpiResult;
        if (!emitWithActivitySchedule(dpiDesign, dpiDir, dpiDiag, dpiResult))
        {
            return fail("dpi activity-schedule pass failed");
        }
        if (!dpiResult.success || dpiDiag.hasError())
        {
            return fail("dpi emit failed");
        }
        const std::filesystem::path dpiStatePath = dpiDir / "grhsim_top_state.cpp";
        const std::filesystem::path dpiEvalPath = dpiDir / "grhsim_top_eval.cpp";
        const std::filesystem::path dpiHeaderPath = dpiDir / "grhsim_top.hpp";
        const std::vector<std::filesystem::path> dpiStateFiles = collectSchedFiles(dpiDir, "grhsim_top_state");
        const std::vector<std::filesystem::path> dpiSchedFiles =
            collectSchedFiles(dpiDir, "grhsim_top_sched_");
        if (dpiStateFiles.empty() || dpiSchedFiles.empty())
        {
            return fail("dpi state/schedule files missing");
        }
        const std::string dpiSchedText = readFiles(dpiSchedFiles);
        const std::string dpiStateText = readFile(dpiStatePath);
        if (dpiStateText.find("const char GrhSIM_top::value_") != std::string::npos ||
            dpiSchedText.find("\"tag\"") == std::string::npos)
        {
            return fail("dpi constant strings should emit directly at their use sites");
        }
        if (dpiSchedText.find("extern \"C\" std::int32_t dpi_mix") == std::string::npos ||
            dpiSchedText.find("const char * label") == std::string::npos ||
            dpiSchedText.find("std::int16_t * sum") == std::string::npos ||
            dpiSchedText.find("std::string * text") == std::string::npos)
        {
            return fail("dpi schedule declaration mismatch");
        }
        if (dpiSchedText.find(".c_str()") != std::string::npos)
        {
            return fail("dpi string constants should not route through std::string::c_str()");
        }
        if (dpiSchedText.find("(a) != 0") == std::string::npos)
        {
            return fail("dpi scalar multi-bit condition should emit scalar truthiness check");
        }
        if (dpiSchedText.find("grhsim_any_bits_words(wide, 130)") == std::string::npos)
        {
            return fail("dpi wide multi-bit condition should emit words truthiness check");
        }
        const std::filesystem::path dpiHarnessPath = dpiDir / "grhsim_top_harness.cpp";
        {
            std::ofstream harness(dpiHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create dpi harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <array>\n";
            harness << "#include <cstdint>\n";
            harness << "#include <string>\n\n";
            harness << "static int g_mix_calls = 0;\n";
            harness << "static int g_pack_calls = 0;\n";
            harness << "static int g_wide_calls = 0;\n\n";
            harness << "extern \"C\" std::int32_t dpi_mix(std::int8_t a,\n";
            harness << "                                 const std::array<std::uint64_t, 3> &wide,\n";
            harness << "                                 double r,\n";
            harness << "                                 const char *label,\n";
            harness << "                                 std::int16_t *sum,\n";
            harness << "                                 std::string *text)\n";
            harness << "{\n";
            harness << "    ++g_mix_calls;\n";
            harness << "    *sum = static_cast<std::int16_t>(static_cast<int>(a) + static_cast<int>(wide[0] & UINT64_C(0xFF)) + static_cast<int>(r * 4.0));\n";
            harness << "    *text = std::string(label) + \":\" + std::to_string(static_cast<int>(*sum));\n";
            harness << "    return static_cast<std::int32_t>(-2 * static_cast<std::int32_t>(*sum));\n";
            harness << "}\n\n";
            harness << "extern \"C\" void dpi_pack(std::uint8_t a, std::uint8_t *mirror)\n";
            harness << "{\n";
            harness << "    ++g_pack_calls;\n";
            harness << "    *mirror = static_cast<std::uint8_t>(a ^ UINT8_C(0x5A));\n";
            harness << "}\n\n";
            harness << "extern \"C\" void dpi_wide_echo(const std::array<std::uint64_t, 3> &wide,\n";
            harness << "                               std::array<std::uint64_t, 3> *out)\n";
            harness << "{\n";
            harness << "    ++g_wide_calls;\n";
            harness << "    *out = wide;\n";
            harness << "    (*out)[0] ^= UINT64_C(0xFF);\n";
            harness << "    (*out)[2] ^= UINT64_C(0x1);\n";
            harness << "}\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    const std::array<std::uint64_t, 3> wide_value{UINT64_C(0x21), UINT64_C(0x123456789ABCDEF0), UINT64_C(0x2)};\n";
            harness << "    auto wide_expected = wide_value;\n";
            harness << "    wide_expected[0] ^= UINT64_C(0xFF);\n";
            harness << "    wide_expected[2] ^= UINT64_C(0x1);\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.a = static_cast<std::uint8_t>(5);\n";
            harness << "    sim.wide = wide_value;\n";
            harness << "    sim.real_in = 1.5;\n";
            harness << "    sim.eval();\n";
            harness << "    if (g_mix_calls != 0 || g_pack_calls != 0 || g_wide_calls != 0) return 1;\n";
            harness << "    if (sim.ret_y != static_cast<std::uint32_t>(0)) return 2;\n";
            harness << "    if (sim.sum_y != static_cast<std::uint16_t>(0)) return 3;\n";
            harness << "    if (!sim.text_y.empty()) return 4;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (g_mix_calls != 1 || g_pack_calls != 1 || g_wide_calls != 1) return 5;\n";
            harness << "    if (sim.sum_y != static_cast<std::uint16_t>(44)) return 6;\n";
            harness << "    if (sim.ret_y != static_cast<std::uint32_t>(-88)) return 7;\n";
            harness << "    if (sim.text_y != std::string(\"tag:44\")) return 8;\n";
            harness << "    if (sim.mirror_y != static_cast<std::uint8_t>(0x5F)) return 9;\n";
            harness << "    if (sim.wide_y != wide_expected) return 10;\n";
            harness << "    sim.clk = false;\n";
            harness << "    sim.eval();\n";
            harness << "    if (g_mix_calls != 1 || g_pack_calls != 1 || g_wide_calls != 1) return 11;\n";
            harness << "    sim.a = static_cast<std::uint8_t>(0xF8);\n";
            harness << "    sim.real_in = 0.5;\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.eval();\n";
            harness << "    if (g_mix_calls != 2 || g_pack_calls != 2 || g_wide_calls != 2) return 12;\n";
            harness << "    if (sim.sum_y != static_cast<std::uint16_t>(27)) return 13;\n";
            harness << "    if (sim.ret_y != static_cast<std::uint32_t>(-54)) return 14;\n";
            harness << "    if (sim.text_y != std::string(\"tag:27\")) return 15;\n";
            harness << "    if (sim.mirror_y != static_cast<std::uint8_t>(0xA2)) return 16;\n";
            harness << "    if (sim.wide_y != wide_expected) return 17;\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path dpiHarnessExe = dpiDir / "grhsim_top_harness";
        std::string dpiCompileCmd = "clang++ " + std::string(kHarnessCompileFlags) + " -I" + dpiDir.string();
        for (const auto &stateFile : dpiStateFiles)
        {
            dpiCompileCmd += " " + stateFile.string();
        }
        dpiCompileCmd += " " + dpiEvalPath.string();
        for (const auto &schedPath : dpiSchedFiles)
        {
            dpiCompileCmd += " " + schedPath.string();
        }
        dpiCompileCmd += " " + dpiHarnessPath.string() + " -o " + dpiHarnessExe.string();
        if (std::system(dpiCompileCmd.c_str()) != 0)
        {
            return fail("dpi harness failed to compile");
        }
        if (std::system(dpiHarnessExe.string().c_str()) != 0)
        {
            return fail("dpi harness failed to run");
        }

        const std::filesystem::path invalidDpiDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_dpi_invalid_inout";
        std::filesystem::remove_all(invalidDpiDir);
        Design invalidDpiDesign = buildInvalidDpiInoutDesign();
        EmitDiagnostics invalidDpiDiag;
        EmitResult invalidDpiResult;
        if (!emitWithActivitySchedule(invalidDpiDesign, invalidDpiDir, invalidDpiDiag, invalidDpiResult))
        {
            return fail("invalid dpi activity-schedule pass failed");
        }
        if (invalidDpiResult.success || !invalidDpiDiag.hasError())
        {
            return fail("invalid dpi inout emit should fail validation");
        }

        const std::filesystem::path invalidDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_regwrite_invalid";
        Design invalidRegWriteDesign = buildInvalidRegisterWriteDesign();
        EmitDiagnostics invalidDiag;
        EmitResult invalidResult;
        if (!emitWithActivitySchedule(invalidRegWriteDesign, invalidDir, invalidDiag, invalidResult))
        {
            return fail("invalid register-write activity-schedule pass failed");
        }
        if (invalidResult.success || !invalidDiag.hasError())
        {
            return fail("invalid register-write emit should fail validation");
        }

        const std::filesystem::path limitDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_size_limit";
        std::filesystem::remove_all(limitDir);
        std::filesystem::create_directories(limitDir);
        Design limitDesign = buildDesign(wideMemInitPath.string());
        SessionStore limitSession;
        if (!runActivitySchedule(limitDesign, limitSession))
        {
            return fail("size-limit activity-schedule pass failed");
        }
        EmitOptions limitOptions;
        limitOptions.outputDir = limitDir.string();
        limitOptions.session = &limitSession;
        limitOptions.sessionPathPrefix = std::string("top");
        limitOptions.attributes["sched_batch_max_ops"] = "8";
        limitOptions.attributes["sched_batch_max_estimated_lines"] = "96";
        limitOptions.attributes["emit_parallelism"] = "2";
        limitOptions.maxOutputFileBytes = 256;
        EmitDiagnostics limitDiag;
        EmitGrhSimCpp limitEmitter(&limitDiag);
        EmitResult limitResult = limitEmitter.emit(limitDesign, limitOptions);
        if (limitResult.success || !limitDiag.hasError())
        {
            return fail("grhsim emit should fail when a generated cpp artifact exceeds the byte limit");
        }
        if (!std::filesystem::exists(limitDir / "grhsim_top_runtime.hpp"))
        {
            return fail("size-limited grhsim emit should keep the oversized partial artifact for inspection");
        }

        const std::filesystem::path packedSchedDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_sched_packed";
        std::filesystem::remove_all(packedSchedDir);
        std::filesystem::create_directories(packedSchedDir);
        Design packedSchedDesign = buildDesign(wideMemInitPath.string());
        SessionStore packedSchedSession;
        if (!runActivitySchedule(packedSchedDesign, packedSchedSession))
        {
            return fail("packed-sched activity-schedule pass failed");
        }
        EmitOptions packedSchedOptions;
        packedSchedOptions.outputDir = packedSchedDir.string();
        packedSchedOptions.session = &packedSchedSession;
        packedSchedOptions.sessionPathPrefix = std::string("top");
        packedSchedOptions.attributes["sched_batch_max_ops"] = "8";
        packedSchedOptions.attributes["sched_batch_max_estimated_lines"] = "96";
        packedSchedOptions.attributes["emit_parallelism"] = "2";
        packedSchedOptions.attributes["sched_batches_per_cpp"] = "2";
        EmitDiagnostics packedSchedDiag;
        EmitGrhSimCpp packedSchedEmitter(&packedSchedDiag);
        EmitResult packedSchedResult = packedSchedEmitter.emit(packedSchedDesign, packedSchedOptions);
        if (!packedSchedResult.success || packedSchedDiag.hasError())
        {
            return fail("packed-sched emit failed");
        }
        const std::vector<std::filesystem::path> packedSchedFiles =
            collectSchedFiles(packedSchedDir, "grhsim_top_sched_group_");
        if (packedSchedFiles.empty() || packedSchedFiles.size() >= schedFiles.size())
        {
            return fail("packed-sched emit should reduce emitted schedule cpp file count");
        }
        const std::string packedMakefile = readFile(packedSchedDir / "Makefile");
        if (packedMakefile.find("grhsim_top_sched_group_0.cpp") == std::string::npos ||
            packedMakefile.find("grhsim_top_sched_0.cpp") != std::string::npos)
        {
            return fail("packed-sched Makefile should reference packed schedule group files");
        }
        const std::string firstPackedSched = readFile(packedSchedFiles.front());
        if (countSubstring(firstPackedSched, "void GrhSIM_top::eval_") < 2)
        {
            return fail("packed-sched group file should contain multiple batch methods");
        }

        const std::filesystem::path runtimeProfileDir =
            std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_runtime_profile";
        std::filesystem::remove_all(runtimeProfileDir);
        std::filesystem::create_directories(runtimeProfileDir);
        Design runtimeProfileDesign = buildDesign(wideMemInitPath.string());
        SessionStore runtimeProfileSession;
        if (!runActivitySchedule(runtimeProfileDesign, runtimeProfileSession))
        {
            return fail("runtime-profile activity-schedule pass failed");
        }
        EmitOptions runtimeProfileOptions;
        runtimeProfileOptions.outputDir = runtimeProfileDir.string();
        runtimeProfileOptions.session = &runtimeProfileSession;
        runtimeProfileOptions.sessionPathPrefix = std::string("top");
        runtimeProfileOptions.attributes["sched_batch_max_ops"] = "8";
        runtimeProfileOptions.attributes["sched_batch_max_estimated_lines"] = "96";
        runtimeProfileOptions.attributes["emit_parallelism"] = "2";
        runtimeProfileOptions.attributes["emit_runtime_profile"] = "1";
        EmitDiagnostics runtimeProfileDiag;
        EmitGrhSimCpp runtimeProfileEmitter(&runtimeProfileDiag);
        EmitResult runtimeProfileResult = runtimeProfileEmitter.emit(runtimeProfileDesign, runtimeProfileOptions);
        if (!runtimeProfileResult.success || runtimeProfileDiag.hasError())
        {
            return fail("runtime-profile-enabled emit failed");
        }
        const std::string runtimeProfileHeader = readFile(runtimeProfileDir / "grhsim_top.hpp");
        const std::string runtimeProfileState = readFile(runtimeProfileDir / "grhsim_top_state.cpp");
        const std::string runtimeProfileSched = readFiles(collectSchedFiles(runtimeProfileDir, "grhsim_top_sched_"));
        if (runtimeProfileHeader.find("static constexpr bool kRuntimeProfileCompiled = true;") == std::string::npos ||
            runtimeProfileHeader.find("runtime_profile_fire_compute_") == std::string::npos ||
            runtimeProfileHeader.find("runtime_profile_fire_commit_") == std::string::npos ||
            runtimeProfileHeader.find("runtime_profile_active_supernodes_") != std::string::npos ||
            runtimeProfileHeader.find("runtime_profile_compute_supernodes_") != std::string::npos ||
            runtimeProfileHeader.find("runtime_profile_compute_nodes_") != std::string::npos ||
            runtimeProfileHeader.find("eval_invocation_count_") != std::string::npos ||
            runtimeProfileSched.find("if (runtime_profile_enabled_)") == std::string::npos ||
            runtimeProfileSched.find("++runtime_profile_fire_compute_[") == std::string::npos ||
            runtimeProfileSched.find("runtime_profile_compute_ops_") != std::string::npos ||
            runtimeProfileSched.find("runtime_profile_source_ops_") != std::string::npos ||
            runtimeProfileSched.find("runtime_profile_sink_ops_") != std::string::npos ||
            runtimeProfileState.find("WOLVRIX_GRHSIM_SUPERNODE_TSV") == std::string::npos ||
            runtimeProfileState.find("# total_evals") != std::string::npos ||
            runtimeProfileState.find("# N_rows") != std::string::npos ||
            runtimeProfileState.find("sim\\tsupernode_id") != std::string::npos ||
            runtimeProfileState.find("e_total") != std::string::npos ||
            runtimeProfileState.find("supernode_id\\tphase\\tf\\tn_comp") != std::string::npos ||
            runtimeProfileState.find("supernode_id\\tphase\\tf\\n") == std::string::npos)
        {
            return fail("runtime-profile-enabled emit should write the fire-only TSV (static columns move to the emit-time file)");
        }
        // NO0190 §10.1: static cost columns are an EMIT-time artifact, separate from the runtime fire file.
        const std::string runtimeProfileStaticTsv = readFile(runtimeProfileDir / "grhsim_supernode_static.tsv");
        if (runtimeProfileStaticTsv.find("supernode_id\tphase\tn_comp\tn_src\tn_sink\tn_const\ta_succ") == std::string::npos)
        {
            return fail("emit should write grhsim_supernode_static.tsv with the 7-column static header");
        }
        {
            bool sawStaticCompute = false, sawStaticCommit = false, sawStaticCommitASucc = false;
            std::size_t ls = 0;
            while (ls < runtimeProfileStaticTsv.size())
            {
                const std::size_t le = runtimeProfileStaticTsv.find('\n', ls);
                const std::string_view line(runtimeProfileStaticTsv.data() + ls,
                                            (le == std::string::npos ? runtimeProfileStaticTsv.size() : le) - ls);
                ls = le == std::string::npos ? runtimeProfileStaticTsv.size() : le + 1;
                if (line.empty() || line.front() == '#' || line.rfind("supernode_id", 0) == 0)
                {
                    continue;
                }
                const std::vector<std::string_view> f = splitTabs(line);
                if (f.size() != 7)
                {
                    return fail("static TSV rows should have exactly 7 fields");
                }
                if (f[1] == "compute")
                {
                    sawStaticCompute = true;
                }
                else if (f[1] == "commit")
                {
                    sawStaticCommit = true;
                    if (f[6] != "0")
                    {
                        sawStaticCommitASucc = true;
                    }
                }
            }
            if (!sawStaticCompute || !sawStaticCommit || !sawStaticCommitASucc)
            {
                return fail("static TSV should include compute rows and nonzero commit a_succ rows");
            }
        }
        const std::string runtimeProfileBuildCmd =
            "make -C " + runtimeProfileDir.string() + " CXX=clang++ CXXFLAGS='" +
            std::string(kHarnessCompileFlags) + "'";
        if (std::system(runtimeProfileBuildCmd.c_str()) != 0)
        {
            return fail("runtime-profile-enabled generated archive failed to build");
        }
        const std::filesystem::path runtimeProfileHarnessPath = runtimeProfileDir / "grhsim_top_profile_harness.cpp";
        {
            std::ofstream harness(runtimeProfileHarnessPath);
            if (!harness.is_open())
            {
                return fail("Failed to create runtime-profile grhsim harness");
            }
            harness << "#include \"grhsim_top.hpp\"\n";
            harness << "#include <cstdint>\n\n";
            harness << "extern \"C\" void trace_sum(std::uint8_t) {}\n\n";
            harness << "int main()\n";
            harness << "{\n";
            harness << "    GrhSIM_top sim;\n";
            harness << "    sim.set_runtime_profile_enabled(true);\n";
            harness << "    sim.init();\n";
            harness << "    sim.clk = true;\n";
            harness << "    sim.en = true;\n";
            harness << "    sim.a = static_cast<std::uint8_t>(3);\n";
            harness << "    sim.eval();\n";
            harness << "    sim.dump_runtime_profile();\n";
            harness << "    return 0;\n";
            harness << "}\n";
        }
        const std::filesystem::path runtimeProfileHarnessExe = runtimeProfileDir / "grhsim_top_profile_harness";
        const std::string runtimeProfileHarnessCompileCmd =
            "clang++ " + std::string(kHarnessCompileFlags) + " -I" + runtimeProfileDir.string() + " " +
            runtimeProfileHarnessPath.string() + " " + (runtimeProfileDir / "libgrhsim_top.a").string() +
            " -o " + runtimeProfileHarnessExe.string();
        if (std::system(runtimeProfileHarnessCompileCmd.c_str()) != 0)
        {
            return fail("runtime-profile grhsim harness failed to compile");
        }
        const std::filesystem::path runtimeProfileTsvPath = runtimeProfileDir / "profile.tsv";
        const std::string runtimeProfileHarnessRunCmd =
            "WOLVRIX_GRHSIM_SUPERNODE_TSV=" + runtimeProfileTsvPath.string() + " " +
            runtimeProfileHarnessExe.string();
        if (std::system(runtimeProfileHarnessRunCmd.c_str()) != 0)
        {
            return fail("runtime-profile grhsim harness failed to run");
        }
        const std::string runtimeProfileTsv = readFile(runtimeProfileTsvPath);
        if (runtimeProfileTsv.find("# total_evals") != std::string::npos ||
            runtimeProfileTsv.find("# N_rows=") != std::string::npos ||
            runtimeProfileTsv.find("supernode_id\tphase\tf\n") == std::string::npos ||
            runtimeProfileTsv.find("\tn_comp") != std::string::npos ||
            runtimeProfileTsv.find("grhsim\t") != std::string::npos ||
            runtimeProfileTsv.find("fire_count") != std::string::npos)
        {
            return fail("runtime-profile grhsim harness should write the fire-only per-supernode TSV");
        }
        bool sawComputeRow = false;
        bool sawCommitRow = false;
        std::size_t lineStart = 0;
        while (lineStart < runtimeProfileTsv.size())
        {
            const std::size_t lineEnd = runtimeProfileTsv.find('\n', lineStart);
            const std::string_view line(runtimeProfileTsv.data() + lineStart,
                                        (lineEnd == std::string::npos ? runtimeProfileTsv.size() : lineEnd) - lineStart);
            lineStart = lineEnd == std::string::npos ? runtimeProfileTsv.size() : lineEnd + 1;
            if (line.empty() || line.front() == '#' || line == "supernode_id\tphase\tf")
            {
                continue;
            }
            const std::vector<std::string_view> fields = splitTabs(line);
            if (fields.size() != 3)
            {
                return fail("runtime-profile fire TSV rows should have exactly 3 fields");
            }
            if (fields[1] == "compute")
            {
                sawComputeRow = true;
            }
            else if (fields[1] == "commit")
            {
                sawCommitRow = true;
            }
        }
        if (!sawComputeRow || !sawCommitRow)
        {
            return fail("runtime-profile fire TSV should include compute and commit rows");
        }

        const std::filesystem::path perfDir = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_cpp_perf";
        std::filesystem::remove_all(perfDir);
        std::filesystem::create_directories(perfDir);
        Design perfDesign = buildDesign(wideMemInitPath.string());
        SessionStore perfSession;
        if (!runActivitySchedule(perfDesign, perfSession))
        {
            return fail("perf activity-schedule pass failed");
        }
        EmitOptions perfOptions;
        perfOptions.outputDir = perfDir.string();
        perfOptions.session = &perfSession;
        perfOptions.sessionPathPrefix = std::string("top");
        perfOptions.attributes["sched_batch_max_ops"] = "8";
        perfOptions.attributes["sched_batch_max_estimated_lines"] = "96";
        perfOptions.attributes["emit_parallelism"] = "2";
        perfOptions.attributes["perf"] = "eval";
        EmitDiagnostics perfDiag;
        EmitGrhSimCpp perfEmitter(&perfDiag);
        EmitResult perfResult = perfEmitter.emit(perfDesign, perfOptions);
        if (!perfResult.success || perfDiag.hasError())
        {
            return fail("perf-enabled emit failed");
        }
        const std::string perfHeader = readFile(perfDir / "grhsim_top.hpp");
        const std::string perfState = readFile(perfDir / "grhsim_top_state.cpp");
        const std::string perfEval = readFile(perfDir / "grhsim_top_eval.cpp");
        if (perfHeader.find("#define WOLVRIX_GRHSIM_PERF 1") == std::string::npos ||
            perfHeader.find("struct PerfCounters") == std::string::npos ||
            perfHeader.find("PerfCounters perf_counters() const") == std::string::npos ||
            perfHeader.find("void reset_perf_counters()") == std::string::npos ||
            perfHeader.find("PerfCounters perf_counters_{};") == std::string::npos ||
            perfHeader.find("trace_eval_enabled_") == std::string::npos ||
            perfState.find("GRHSIM_TRACE_EVAL") == std::string::npos ||
            perfEval.find("trace_this_eval") == std::string::npos ||
            perfEval.find("++perf_counters_.computeBatchExecCount;") == std::string::npos ||
            perfEval.find("++perf_counters_.commitBatchExecCount;") == std::string::npos ||
            perfEval.find("#include <chrono>") == std::string::npos ||
            perfEval.find("if (((supernode_active_curr_[") != std::string::npos)
        {
            return fail("perf-enabled emit should include perf counters and eval tracing");
        }

        return 0;
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
