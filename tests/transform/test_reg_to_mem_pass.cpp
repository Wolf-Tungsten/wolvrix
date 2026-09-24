#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/comb_lane_pack.hpp"
#include "transform/reg_to_mem.hpp"
#include "transform/simplify.hpp"

#include <array>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[reg-to-mem-tests] " << message << '\n';
        return 1;
    }

    template <typename T>
    std::optional<T> getAttr(const Operation &op, std::string_view key)
    {
        auto attr = op.attr(key);
        if (!attr)
        {
            return std::nullopt;
        }
        if (const auto *value = std::get_if<T>(&*attr))
        {
            return *value;
        }
        return std::nullopt;
    }

    ValueId makeLogicValue(Graph &graph, std::string_view name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(std::string(name)), width, false, ValueType::Logic);
    }

    ValueId addConstant(Graph &graph,
                        std::string_view opName,
                        std::string_view valueName,
                        int32_t width,
                        std::string literal)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    OperationId addRegister(Graph &graph, std::string_view name, int32_t width)
    {
        const OperationId op = graph.createOperation(OperationKind::kRegister,
                                                     graph.internSymbol(std::string(name)));
        graph.setAttr(op, "width", static_cast<int64_t>(width));
        graph.setAttr(op, "isSigned", false);
        return op;
    }

    OperationId addRegisterRead(Graph &graph,
                                std::string_view opName,
                                std::string_view valueName,
                                std::string regSymbol,
                                int32_t width,
                                ValueId &result)
    {
        result = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kRegisterReadPort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addResult(op, result);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        return op;
    }

    ValueId addBinary(Graph &graph,
                      OperationKind kind,
                      std::string_view opName,
                      std::string_view valueName,
                      ValueId lhs,
                      ValueId rhs,
                      int32_t width = 1)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(kind, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, value);
        return value;
    }

    ValueId addUnary(Graph &graph,
                     OperationKind kind,
                     std::string_view opName,
                     std::string_view valueName,
                     ValueId operand,
                     int32_t width = 1)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(kind, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, operand);
        graph.addResult(op, value);
        return value;
    }

    ValueId addMux(Graph &graph,
                   std::string_view opName,
                   std::string_view valueName,
                   ValueId cond,
                   ValueId trueValue,
                   ValueId falseValue,
                   int32_t width)
    {
        ValueId value = makeLogicValue(graph, valueName, width);
        const OperationId op = graph.createOperation(OperationKind::kMux, graph.internSymbol(std::string(opName)));
        graph.addOperand(op, cond);
        graph.addOperand(op, trueValue);
        graph.addOperand(op, falseValue);
        graph.addResult(op, value);
        return value;
    }

    OperationId addRegisterWrite(Graph &graph,
                                 std::string_view opName,
                                 std::string regSymbol,
                                 ValueId guard,
                                 ValueId data,
                                 ValueId mask,
                                 ValueId clk,
                                 std::string edge = "posedge")
    {
        const OperationId op = graph.createOperation(OperationKind::kRegisterWritePort,
                                                     graph.internSymbol(std::string(opName)));
        graph.addOperand(op, guard);
        graph.addOperand(op, data);
        graph.addOperand(op, mask);
        graph.addOperand(op, clk);
        graph.setAttr(op, "regSymbol", std::move(regSymbol));
        graph.setAttr(op, "eventEdge", std::vector<std::string>{std::move(edge)});
        return op;
    }

    OperationId addSliceArrayAnchor(Graph &graph,
                                    std::string_view prefix,
                                    const std::vector<std::string> &regSymbols,
                                    ValueId index,
                                    int32_t width,
                                    ValueId &selected)
    {
        std::vector<ValueId> readValues(regSymbols.size());
        for (std::size_t row = 0; row < regSymbols.size(); ++row)
        {
            addRegisterRead(graph,
                            std::string(prefix) + "_r" + std::to_string(row) + "_read_op",
                            std::string(prefix) + "_r" + std::to_string(row) + "_read",
                            regSymbols[row],
                            width,
                            readValues[row]);
        }

        ValueId packed = makeLogicValue(graph,
                                        std::string(prefix) + "_packed",
                                        static_cast<int32_t>(width * regSymbols.size()));
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol(std::string(prefix) + "_concat"));
        for (auto it = readValues.rbegin(); it != readValues.rend(); ++it)
        {
            graph.addOperand(concat, *it);
        }
        graph.addResult(concat, packed);

        selected = makeLogicValue(graph, std::string(prefix) + "_selected", width);
        const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                        graph.internSymbol(std::string(prefix) + "_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", static_cast<int64_t>(width));
        return slice;
    }

    int runPass(Design &design,
                bool trueMerge,
                bool orderedWrites = false,
                bool decodedWriteStorage = true,
                RegToMemProfitabilityPolicy profitabilityFilter = RegToMemProfitabilityPolicy::None)
    {
        PassManager manager;
        RegToMemOptions options;
        options.minElementCount = 2;
        options.enableTrueMerge = trueMerge;
        options.enableOrderedWrites = orderedWrites;
        options.enableDecodedWriteStorage = decodedWriteStorage;
        options.profitabilityFilter = profitabilityFilter;
        manager.addPass(std::make_unique<RegToMemPass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("reg-to-mem pass failed");
        }
        return 0;
    }

    int runIntentPass(Design &design)
    {
        return runPass(design, false);
    }

    int runTruePass(Design &design)
    {
        return runPass(design, true);
    }

    int runOrderedTruePass(Design &design)
    {
        return runPass(design, true, true);
    }

    int runVerilatorProfitabilityPass(Design &design, bool orderedWrites = false)
    {
        return runPass(design, true, orderedWrites, true,
                       RegToMemProfitabilityPolicy::VerilatorV1);
    }

    int runCombLanePackAndSimplify(Design &design)
    {
        PassManager manager;
        CombLanePackOptions packOptions;
        packOptions.enableDeclaredRoots = false;
        manager.addPass(std::make_unique<CombLanePackPass>(packOptions));
        SimplifyOptions simplifyOptions;
        simplifyOptions.semantics = ConstantFoldOptions::Semantics::TwoState;
        manager.addPass(std::make_unique<SimplifyPass>(simplifyOptions));
        manager.addPass(std::make_unique<SimplifyPass>(simplifyOptions));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("comb-lane-pack + simplify pipeline failed");
        }
        return 0;
    }

    std::size_t countKind(const Graph &graph, OperationKind kind)
    {
        std::size_t count = 0;
        for (OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    std::vector<OperationId> opsOfKind(const Graph &graph, OperationKind kind)
    {
        std::vector<OperationId> ops;
        for (OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ops.push_back(opId);
            }
        }
        return ops;
    }

    bool valueDependsOn(const Graph &graph, ValueId root, ValueId target)
    {
        std::vector<ValueId> pending = {root};
        std::unordered_set<ValueId, ValueIdHash> visited;
        while (!pending.empty())
        {
            const ValueId value = pending.back();
            pending.pop_back();
            if (value == target)
            {
                return true;
            }
            if (!visited.insert(value).second)
            {
                continue;
            }
            const OperationId def = graph.valueDef(value);
            if (!def.valid())
            {
                continue;
            }
            const Operation op = graph.getOperation(def);
            pending.insert(pending.end(), op.operands().begin(), op.operands().end());
        }
        return false;
    }

    bool hasNegatedDependencies(const Graph &graph,
                                ValueId root,
                                ValueId first,
                                ValueId second)
    {
        std::vector<ValueId> pending = {root};
        std::unordered_set<ValueId, ValueIdHash> visited;
        while (!pending.empty())
        {
            const ValueId value = pending.back();
            pending.pop_back();
            if (!visited.insert(value).second)
            {
                continue;
            }
            const OperationId def = graph.valueDef(value);
            if (!def.valid())
            {
                continue;
            }
            const Operation op = graph.getOperation(def);
            if ((op.kind() == OperationKind::kLogicNot ||
                 (op.kind() == OperationKind::kNot && graph.valueWidth(value) == 1)) &&
                op.operands().size() == 1 &&
                valueDependsOn(graph, op.operands().front(), first) &&
                valueDependsOn(graph, op.operands().front(), second))
            {
                return true;
            }
            pending.insert(pending.end(), op.operands().begin(), op.operands().end());
        }
        return false;
    }

    bool hasIntentGroup(const Graph &graph, OperationId opId)
    {
        return getAttr<std::string>(graph.getOperation(opId), "regToMem.intent.group").has_value();
    }

    Design buildTrueMergeDesign(bool multiAnchor,
                                bool withReset,
                                bool reduceAndLastRow = false,
                                bool secondWriteFamily = false,
                                bool secondResetTerm = false,
                                bool fallbackWriteFamily = false)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        regs.reserve(rows);
        std::vector<ValueId> firstGuards(rows);
        std::vector<ValueId> firstHits(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId index2 = makeLogicValue(graph, "index2", 2);
        const ValueId addr = makeLogicValue(graph, "addr", 2);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId data = makeLogicValue(graph, "data", width);
        const ValueId addr2 = secondWriteFamily ? makeLogicValue(graph, "addr2", 2) : ValueId{};
        const ValueId wen2 = secondWriteFamily ? makeLogicValue(graph, "wen2", 1) : ValueId{};
        const ValueId data2 = secondWriteFamily ? makeLogicValue(graph, "data2", width) : ValueId{};
        const ValueId addr3 = fallbackWriteFamily ? makeLogicValue(graph, "addr3", 2) : ValueId{};
        const ValueId wen3 = fallbackWriteFamily ? makeLogicValue(graph, "wen3", 1) : ValueId{};
        const ValueId mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", index);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("data", data);
        graph.bindInputPort("clk", clk);
        if (secondWriteFamily)
        {
            graph.bindInputPort("addr2", addr2);
            graph.bindInputPort("wen2", wen2);
            graph.bindInputPort("data2", data2);
        }
        if (fallbackWriteFamily)
        {
            graph.bindInputPort("addr3", addr3);
            graph.bindInputPort("wen3", wen3);
        }

        ValueId selected;
        addSliceArrayAnchor(graph, "a0", regs, index, width, selected);
        graph.bindOutputPort("selected", selected);
        if (multiAnchor)
        {
            graph.bindInputPort("index2", index2);
            ValueId selected2;
            addSliceArrayAnchor(graph, "a1", regs, index2, width, selected2);
            graph.bindOutputPort("selected2", selected2);
        }

        for (std::size_t row = 0; row < rows; ++row)
        {
            ValueId hit;
            if (reduceAndLastRow && row + 1 == rows)
            {
                hit = addUnary(graph,
                               OperationKind::kReduceAnd,
                               "hit" + std::to_string(row) + "_op",
                               "hit" + std::to_string(row),
                               addr,
                               1);
            }
            else
            {
                const ValueId rowConst = addConstant(graph,
                                                     "row" + std::to_string(row) + "_op",
                                                     "row" + std::to_string(row),
                                                     2,
                                                     "2'd" + std::to_string(row));
                hit = addBinary(graph,
                                OperationKind::kEq,
                                "hit" + std::to_string(row) + "_op",
                                "hit" + std::to_string(row),
                                addr,
                                rowConst,
                                1);
            }
            const ValueId guard = addBinary(graph,
                                            reduceAndLastRow ? OperationKind::kAnd : OperationKind::kLogicAnd,
                                            "guard" + std::to_string(row) + "_op",
                                            "guard" + std::to_string(row),
                                            wen,
                                            hit,
                                            1);
            firstHits[row] = hit;
            firstGuards[row] = guard;
            if (!secondWriteFamily)
            {
                addRegisterWrite(graph,
                                 "write" + std::to_string(row),
                                 regs[row],
                                 guard,
                                 data,
                                 mask,
                                 clk);
            }
        }

        if (secondWriteFamily)
        {
            const ValueId sharedEnable = addBinary(graph,
                                                   OperationKind::kLogicAnd,
                                                   "shared_write_enable_op",
                                                   "shared_write_enable",
                                                   wen,
                                                   wen2,
                                                   1);
            ValueId consolidatedReset;
            ValueId consolidatedReset2;
            ValueId effectiveReset;
            ValueId activeEnable = sharedEnable;
            if (withReset)
            {
                consolidatedReset = makeLogicValue(graph, "priority_reset", 1);
                graph.bindInputPort("priority_reset", consolidatedReset);
                effectiveReset = consolidatedReset;
                if (secondResetTerm)
                {
                    consolidatedReset2 = makeLogicValue(graph, "priority_reset2", 1);
                    graph.bindInputPort("priority_reset2", consolidatedReset2);
                    effectiveReset = addBinary(graph,
                                               OperationKind::kLogicOr,
                                               "priority_combined_reset_op",
                                               "priority_combined_reset",
                                               consolidatedReset,
                                               consolidatedReset2,
                                               1);
                }
                const ValueId notReset = addUnary(graph,
                                                  OperationKind::kLogicNot,
                                                  "priority_not_reset_op",
                                                  "priority_not_reset",
                                                  effectiveReset);
                activeEnable = addBinary(graph,
                                         OperationKind::kLogicAnd,
                                         "priority_active_enable_op",
                                         "priority_active_enable",
                                         sharedEnable,
                                         notReset,
                                         1);
            }
            const ValueId fallback = addConstant(graph, "write_fallback_op", "write_fallback", width, "8'h00");
            for (std::size_t row = 0; row < rows; ++row)
            {
                const ValueId rowConst = addConstant(graph,
                                                     "second_row" + std::to_string(row) + "_op",
                                                     "second_row" + std::to_string(row),
                                                     2,
                                                     "2'd" + std::to_string(row));
                const ValueId hit = addBinary(graph,
                                              OperationKind::kEq,
                                              "second_hit" + std::to_string(row) + "_op",
                                              "second_hit" + std::to_string(row),
                                              addr2,
                                              rowConst,
                                              1);
                const ValueId secondGuard = addBinary(graph,
                                                      OperationKind::kLogicAnd,
                                                      "second_guard" + std::to_string(row) + "_op",
                                                      "second_guard" + std::to_string(row),
                                                      activeEnable,
                                                      hit,
                                                      1);
                const ValueId notSecondHit = addUnary(graph,
                                                      OperationKind::kLogicNot,
                                                      "not_second_hit" + std::to_string(row) + "_op",
                                                      "not_second_hit" + std::to_string(row),
                                                      hit);
                const ValueId firstEligible = addBinary(graph,
                                                        OperationKind::kLogicAnd,
                                                        "first_eligible" + std::to_string(row) + "_op",
                                                        "first_eligible" + std::to_string(row),
                                                        activeEnable,
                                                        notSecondHit,
                                                        1);
                const ValueId firstGuard = addBinary(graph,
                                                     OperationKind::kLogicAnd,
                                                     "priority_first_guard" + std::to_string(row) + "_op",
                                                     "priority_first_guard" + std::to_string(row),
                                                     firstEligible,
                                                     firstGuards[row],
                                                     1);
                ValueId updateCond = addBinary(graph,
                                               OperationKind::kLogicOr,
                                               "priority_update" + std::to_string(row) + "_op",
                                               "priority_update" + std::to_string(row),
                                               firstGuard,
                                               secondGuard,
                                               1);
                if (fallbackWriteFamily)
                {
                    const ValueId thirdRowConst = addConstant(
                        graph,
                        "third_row" + std::to_string(row) + "_op",
                        "third_row" + std::to_string(row),
                        2,
                        "2'd" + std::to_string(row));
                    const ValueId thirdHit = addBinary(graph,
                                                       OperationKind::kEq,
                                                       "third_hit" + std::to_string(row) + "_op",
                                                       "third_hit" + std::to_string(row),
                                                       addr3,
                                                       thirdRowConst,
                                                       1);
                    const ValueId notFirstHit = addUnary(graph,
                                                         OperationKind::kLogicNot,
                                                         "not_first_hit" + std::to_string(row) + "_op",
                                                         "not_first_hit" + std::to_string(row),
                                                         firstHits[row]);
                    const ValueId thirdEnable = addBinary(graph,
                                                           OperationKind::kLogicAnd,
                                                           "third_enable" + std::to_string(row) + "_op",
                                                           "third_enable" + std::to_string(row),
                                                           activeEnable,
                                                           wen3,
                                                           1);
                    const ValueId thirdBelowSecond = addBinary(
                        graph,
                        OperationKind::kLogicAnd,
                        "third_below_second" + std::to_string(row) + "_op",
                        "third_below_second" + std::to_string(row),
                        thirdEnable,
                        notSecondHit,
                        1);
                    const ValueId thirdBelowFirst = addBinary(
                        graph,
                        OperationKind::kLogicAnd,
                        "third_below_first" + std::to_string(row) + "_op",
                        "third_below_first" + std::to_string(row),
                        thirdBelowSecond,
                        notFirstHit,
                        1);
                    const ValueId thirdGuard = addBinary(graph,
                                                         OperationKind::kLogicAnd,
                                                         "third_guard" + std::to_string(row) + "_op",
                                                         "third_guard" + std::to_string(row),
                                                         thirdBelowFirst,
                                                         thirdHit,
                                                         1);
                    updateCond = addBinary(graph,
                                           OperationKind::kLogicOr,
                                           "third_update" + std::to_string(row) + "_op",
                                           "third_update" + std::to_string(row),
                                           updateCond,
                                           thirdGuard,
                                           1);
                }
                ValueId rowFallback = fallback;
                if (withReset)
                {
                    rowFallback = addConstant(graph,
                                              "priority_reset_data" + std::to_string(row) + "_op",
                                              "priority_reset_data" + std::to_string(row),
                                              width,
                                              "8'd" + std::to_string(0x10 + row));
                    updateCond = addBinary(graph,
                                           OperationKind::kLogicOr,
                                           "priority_reset_update" + std::to_string(row) + "_op",
                                           "priority_reset_update" + std::to_string(row),
                                           effectiveReset,
                                           updateCond,
                                           1);
                }
                const ValueId firstNext = addMux(graph,
                                                 "priority_first_mux" + std::to_string(row) + "_op",
                                                 "priority_first_mux" + std::to_string(row),
                                                 firstGuard,
                                                 data,
                                                 rowFallback,
                                                 width);
                const ValueId next = addMux(graph,
                                            "priority_second_mux" + std::to_string(row) + "_op",
                                            "priority_second_mux" + std::to_string(row),
                                            secondGuard,
                                            data2,
                                            firstNext,
                                            width);
                addRegisterWrite(graph,
                                 "priority_write" + std::to_string(row),
                                 regs[row],
                                 updateCond,
                                 next,
                                 mask,
                                 clk);
            }
        }

        if (withReset && !secondWriteFamily)
        {
            const ValueId rstN = makeLogicValue(graph, "rst_n", 1);
            const ValueId rstMask = addConstant(graph, "rst_mask_op", "rst_mask", width, "8'hff");
            graph.bindInputPort("rst_n", rstN);
            for (std::size_t row = 0; row < rows; ++row)
            {
                const ValueId rstData = addConstant(graph,
                                                    "rst_data" + std::to_string(row) + "_op",
                                                    "rst_data" + std::to_string(row),
                                                    width,
                                                    "8'd" + std::to_string(0x10 + row));
                addRegisterWrite(graph,
                                 "reset" + std::to_string(row),
                                 regs[row],
                                 rstN,
                                 rstData,
                                 rstMask,
                                 clk,
                                 "negedge");
            }
        }

        return design;
    }

    Design buildTrueMergeCompoundResetDesign(bool trailingNoOpResetMux = false,
                                              bool splitData = false)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        regs.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId addr = makeLogicValue(graph, "addr", 2);
        const ValueId wen = makeLogicValue(graph, "wen", 1);
        const ValueId reset = makeLogicValue(graph, "reset", 1);
        const ValueId reset2 = trailingNoOpResetMux ? makeLogicValue(graph, "reset2", 1) : ValueId{};
        const ValueId resetUsefulAddr = trailingNoOpResetMux ? makeLogicValue(graph, "reset_useful_addr", 2) : ValueId{};
        const ValueId resetUsefulB = trailingNoOpResetMux ? makeLogicValue(graph, "reset_useful_b", 1) : ValueId{};
        const ValueId data = makeLogicValue(graph, "data", splitData ? width - 1 : width);
        ValueId writeData = data;
        if (splitData)
        {
            const ValueId zero = addConstant(graph, "data_zero_op", "data_zero", 1, "1'b0");
            writeData = makeLogicValue(graph, "data_extended", width);
            const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("data_extended_op"));
            graph.addOperand(concat, zero);
            graph.addOperand(concat, data);
            graph.addResult(concat, writeData);
        }
        const ValueId mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", index);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("reset", reset);
        if (trailingNoOpResetMux)
        {
            graph.bindInputPort("reset2", reset2);
            graph.bindInputPort("reset_useful_addr", resetUsefulAddr);
            graph.bindInputPort("reset_useful_b", resetUsefulB);
        }
        graph.bindInputPort("data", data);
        graph.bindInputPort("clk", clk);

        ValueId selected;
        addSliceArrayAnchor(graph, "a0", regs, index, width, selected);
        graph.bindOutputPort("selected", selected);

        const ValueId effectiveReset = trailingNoOpResetMux
                                           ? addBinary(graph,
                                                       OperationKind::kLogicOr,
                                                       "combined_reset_op",
                                                       "combined_reset",
                                                       reset,
                                                       reset2,
                                                       1)
                                           : reset;
        const ValueId resetUsefulRow = trailingNoOpResetMux
                                           ? addConstant(graph,
                                                         "reset_useful_row_op",
                                                         "reset_useful_row",
                                                         2,
                                                         "2'd3")
                                           : ValueId{};
        const ValueId resetUsefulHit = trailingNoOpResetMux
                                           ? addBinary(graph,
                                                       OperationKind::kEq,
                                                       "reset_useful_hit_op",
                                                       "reset_useful_hit",
                                                       resetUsefulAddr,
                                                       resetUsefulRow,
                                                       1)
                                           : ValueId{};
        const ValueId resetUseful = trailingNoOpResetMux
                                        ? addBinary(graph,
                                                    OperationKind::kAnd,
                                                    "reset_useful_op",
                                                    "reset_useful",
                                                    resetUsefulHit,
                                                    resetUsefulB,
                                                    1)
                                        : ValueId{};
        const ValueId notReset = addUnary(graph,
                                          OperationKind::kLogicNot,
                                          "not_reset_op",
                                          "not_reset",
                                          effectiveReset,
                                          1);
        const ValueId notResetUseful = trailingNoOpResetMux
                                           ? addUnary(graph,
                                                      OperationKind::kLogicNot,
                                                      "not_reset_useful_op",
                                                      "not_reset_useful",
                                                      resetUseful,
                                                      1)
                                           : ValueId{};
        const ValueId activeResetEnable = trailingNoOpResetMux
                                              ? addBinary(graph,
                                                          OperationKind::kLogicAnd,
                                                          "active_reset_enable_op",
                                                          "active_reset_enable",
                                                          notReset,
                                                          notResetUseful,
                                                          1)
                                              : notReset;
        const ValueId resetUsefulGuard = trailingNoOpResetMux
                                             ? addBinary(graph,
                                                         OperationKind::kLogicAnd,
                                                         "reset_useful_guard_op",
                                                         "reset_useful_guard",
                                                         notReset,
                                                         resetUseful,
                                                         1)
                                             : ValueId{};
        const ValueId fillGuard = trailingNoOpResetMux
                                      ? addBinary(graph,
                                                  OperationKind::kLogicOr,
                                                  "fill_guard_op",
                                                  "fill_guard",
                                                  effectiveReset,
                                                  resetUsefulGuard,
                                                  1)
                                      : effectiveReset;
        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId rowConst = addConstant(graph,
                                                 "row" + std::to_string(row) + "_op",
                                                 "row" + std::to_string(row),
                                                 2,
                                                 "2'd" + std::to_string(row));
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "hit" + std::to_string(row) + "_op",
                                          "hit" + std::to_string(row),
                                          addr,
                                          rowConst,
                                          1);
            const ValueId wenHit = addBinary(graph,
                                             OperationKind::kAnd,
                                             "wen_hit" + std::to_string(row) + "_op",
                                             "wen_hit" + std::to_string(row),
                                             wen,
                                             hit,
                                             1);
            const ValueId active = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "active" + std::to_string(row) + "_op",
                                             "active" + std::to_string(row),
                                             activeResetEnable,
                                             wenHit,
                                             1);
            const ValueId update = addBinary(graph,
                                             OperationKind::kLogicOr,
                                             "update" + std::to_string(row) + "_op",
                                             "update" + std::to_string(row),
                                             fillGuard,
                                             active,
                                             1);
            const ValueId resetData = addConstant(graph,
                                                  "rst_data" + std::to_string(row) + "_op",
                                                  "rst_data" + std::to_string(row),
                                                  width,
                                                  "8'd" + std::to_string(splitData ? 0 : 0x20 + row));
            const ValueId fallback = trailingNoOpResetMux
                                         ? addMux(graph,
                                                  "noop_reset_mux" + std::to_string(row) + "_op",
                                                  "noop_reset_mux" + std::to_string(row),
                                                  resetUsefulGuard,
                                                  resetData,
                                                  resetData,
                                                  width)
                                         : resetData;
            const ValueId next = addMux(graph,
                                        "next" + std::to_string(row) + "_op",
                                        "next" + std::to_string(row),
                                        active,
                                        writeData,
                                        fallback,
                                        width);
            addRegisterWrite(graph,
                             "write" + std::to_string(row),
                             regs[row],
                             update,
                             next,
                             mask,
                             clk);
        }

        return design;
    }

    Design buildBranchNotInUpdateOuterResetDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 1;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        for (std::size_t row = 0; row < rows; ++row)
        {
            regs.push_back("outer_reset_r" + std::to_string(row));
            addRegister(graph, regs.back(), width);
        }

        const ValueId index = makeLogicValue(graph, "outer_reset_index", 2);
        const ValueId lock = makeLogicValue(graph, "outer_reset_lock", 1);
        const ValueId normalData = makeLogicValue(graph, "outer_reset_normal_data", width);
        const ValueId reset = makeLogicValue(graph, "outer_reset", 1);
        const ValueId clk = makeLogicValue(graph, "outer_reset_clk", 1);
        graph.bindInputPort("outer_reset_index", index);
        graph.bindInputPort("outer_reset_lock", lock);
        graph.bindInputPort("outer_reset_normal_data", normalData);
        graph.bindInputPort("outer_reset", reset);
        graph.bindInputPort("outer_reset_clk", clk);

        ValueId selected;
        addSliceArrayAnchor(graph, "outer_reset_anchor", regs, index, width, selected);
        graph.bindOutputPort("outer_reset_selected", selected);

        const ValueId notReset = addUnary(graph,
                                          OperationKind::kLogicNot,
                                          "outer_reset_not_op",
                                          "outer_reset_not",
                                          reset);
        const ValueId resetData = addConstant(graph,
                                              "outer_reset_data_op",
                                              "outer_reset_data",
                                              width,
                                              "1'b0");
        const ValueId mask = addConstant(graph,
                                         "outer_reset_mask_op",
                                         "outer_reset_mask",
                                         width,
                                         "1'b1");
        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId next = addMux(graph,
                                        "outer_reset_next" + std::to_string(row) + "_op",
                                        "outer_reset_next" + std::to_string(row),
                                        notReset,
                                        normalData,
                                        resetData,
                                        width);
            const OperationId write = graph.createOperation(
                OperationKind::kRegisterWritePort,
                graph.internSymbol("outer_reset_write" + std::to_string(row)));
            graph.addOperand(write, lock);
            graph.addOperand(write, next);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.addOperand(write, reset);
            graph.setAttr(write, "regSymbol", regs[row]);
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge", "posedge"});
        }
        return design;
    }

    Design buildTrueMergeCompoundPriorityDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        constexpr std::size_t rows = 4;
        std::vector<std::string> regs;
        regs.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "index", 2);
        const ValueId highAddr = makeLogicValue(graph, "high_addr", 3);
        const ValueId highLane = makeLogicValue(graph, "high_lane", 1);
        const ValueId highEnable = makeLogicValue(graph, "high_enable", 1);
        const ValueId highData = makeLogicValue(graph, "high_data", width - 1);
        const ValueId lowAddr = makeLogicValue(graph, "low_addr", 3);
        const ValueId lowLane = makeLogicValue(graph, "low_lane", 1);
        const ValueId lowEnable = makeLogicValue(graph, "low_enable", 1);
        const ValueId lowData = makeLogicValue(graph, "low_data", width - 1);
        const ValueId clk = makeLogicValue(graph, "clk", 1);
        graph.bindInputPort("index", index);
        graph.bindInputPort("high_addr", highAddr);
        graph.bindInputPort("high_lane", highLane);
        graph.bindInputPort("high_enable", highEnable);
        graph.bindInputPort("high_data", highData);
        graph.bindInputPort("low_addr", lowAddr);
        graph.bindInputPort("low_lane", lowLane);
        graph.bindInputPort("low_enable", lowEnable);
        graph.bindInputPort("low_data", lowData);
        graph.bindInputPort("clk", clk);

        ValueId selected;
        addSliceArrayAnchor(graph, "a0", regs, index, width, selected);
        graph.bindOutputPort("selected", selected);

        const ValueId laneZero = addConstant(graph, "lane_zero_op", "lane_zero", 1, "1'b0");
        const ValueId dataZero = addConstant(graph, "data_zero_op", "data_zero", 1, "1'b0");
        const ValueId fallback = addConstant(graph, "fallback_op", "fallback", width, "8'b0");
        const ValueId mask = addConstant(graph, "mask_op", "mask", width, "8'hff");
        const ValueId highLaneHit = addBinary(graph,
                                              OperationKind::kEq,
                                              "high_lane_hit_op",
                                              "high_lane_hit",
                                              highLane,
                                              laneZero);
        const ValueId lowLaneHit = addBinary(graph,
                                             OperationKind::kEq,
                                             "low_lane_hit_op",
                                             "low_lane_hit",
                                             lowLane,
                                             laneZero);
        const ValueId highBase = addBinary(graph,
                                           OperationKind::kLogicAnd,
                                           "high_base_op",
                                           "high_base",
                                           highEnable,
                                           highLaneHit);
        const ValueId lowBase = addBinary(graph,
                                          OperationKind::kLogicAnd,
                                          "low_base_op",
                                          "low_base",
                                          lowEnable,
                                          lowLaneHit);
        ValueId highWriteData = makeLogicValue(graph, "high_write_data", width);
        const OperationId highConcat = graph.createOperation(OperationKind::kConcat,
                                                              graph.internSymbol("high_write_data_op"));
        graph.addOperand(highConcat, dataZero);
        graph.addOperand(highConcat, highData);
        graph.addResult(highConcat, highWriteData);
        ValueId lowWriteData = makeLogicValue(graph, "low_write_data", width);
        const OperationId lowConcat = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("low_write_data_op"));
        graph.addOperand(lowConcat, dataZero);
        graph.addOperand(lowConcat, lowData);
        graph.addResult(lowConcat, lowWriteData);

        for (std::size_t row = 0; row < rows; ++row)
        {
            const ValueId rowConst = addConstant(graph,
                                                 "row" + std::to_string(row) + "_op",
                                                 "row" + std::to_string(row),
                                                 3,
                                                 "3'd" + std::to_string(row));
            ValueId highRowHit;
            if (row == 0)
            {
                const ValueId anyHighAddr = addUnary(graph,
                                                     OperationKind::kReduceOr,
                                                     "high_addr_any_op",
                                                     "high_addr_any",
                                                     highAddr);
                highRowHit = addUnary(graph,
                                      OperationKind::kLogicNot,
                                      "high_row0_hit_op",
                                      "high_row0_hit",
                                      anyHighAddr);
            }
            else
            {
                highRowHit = addBinary(graph,
                                       OperationKind::kEq,
                                       "high_row_hit" + std::to_string(row) + "_op",
                                       "high_row_hit" + std::to_string(row),
                                       highAddr,
                                       rowConst);
            }
            const ValueId lowRowHit = addBinary(graph,
                                                OperationKind::kEq,
                                                "low_row_hit" + std::to_string(row) + "_op",
                                                "low_row_hit" + std::to_string(row),
                                                lowAddr,
                                                rowConst);
            const ValueId highGuard = addBinary(graph,
                                                OperationKind::kLogicAnd,
                                                "high_guard" + std::to_string(row) + "_op",
                                                "high_guard" + std::to_string(row),
                                                highBase,
                                                highRowHit);
            const ValueId notHighGuard = addUnary(graph,
                                                  OperationKind::kLogicNot,
                                                  "not_high_guard" + std::to_string(row) + "_op",
                                                  "not_high_guard" + std::to_string(row),
                                                  highGuard);
            const ValueId lowCandidate = addBinary(graph,
                                                   OperationKind::kLogicAnd,
                                                   "low_candidate" + std::to_string(row) + "_op",
                                                   "low_candidate" + std::to_string(row),
                                                   lowBase,
                                                   lowRowHit);
            const ValueId lowGuard = addBinary(graph,
                                               OperationKind::kLogicAnd,
                                               "low_guard" + std::to_string(row) + "_op",
                                               "low_guard" + std::to_string(row),
                                               lowCandidate,
                                               notHighGuard);
            const ValueId update = addBinary(graph,
                                             OperationKind::kLogicOr,
                                             "update" + std::to_string(row) + "_op",
                                             "update" + std::to_string(row),
                                             highGuard,
                                             lowGuard);
            const ValueId lowNext = addMux(graph,
                                           "low_next" + std::to_string(row) + "_op",
                                           "low_next" + std::to_string(row),
                                           lowGuard,
                                           lowWriteData,
                                           fallback,
                                           width);
            const ValueId next = addMux(graph,
                                        "next" + std::to_string(row) + "_op",
                                        "next" + std::to_string(row),
                                        highGuard,
                                        highWriteData,
                                        lowNext,
                                        width);
            addRegisterWrite(graph,
                             "write" + std::to_string(row),
                             regs[row],
                             update,
                             next,
                             mask,
                             clk);
        }
        return design;
    }

    Design buildTrueMergeOrDecodedPriorityDesign()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 3;
        constexpr std::size_t rows = 4;
        constexpr std::size_t writers = 3;
        std::vector<std::string> regs;
        regs.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "decoded_r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
        }

        const ValueId index = makeLogicValue(graph, "decoded_index", 2);
        const ValueId reset = makeLogicValue(graph, "decoded_reset", 1);
        const ValueId highEnable = makeLogicValue(graph, "decoded_high_enable", 1);
        const ValueId highAddr = makeLogicValue(graph, "decoded_high_addr", 2);
        const ValueId clk = makeLogicValue(graph, "decoded_clk", 1);
        graph.bindInputPort("decoded_index", index);
        graph.bindInputPort("decoded_reset", reset);
        graph.bindInputPort("decoded_high_enable", highEnable);
        graph.bindInputPort("decoded_high_addr", highAddr);
        graph.bindInputPort("decoded_clk", clk);

        std::array<ValueId, writers> enables;
        std::array<ValueId, writers> addresses;
        for (std::size_t writer = 0; writer < writers; ++writer)
        {
            enables[writer] = makeLogicValue(graph,
                                             "decoded_enable_" + std::to_string(writer),
                                             1);
            addresses[writer] = makeLogicValue(graph,
                                               "decoded_addr_" + std::to_string(writer),
                                               2);
            graph.bindInputPort("decoded_enable_" + std::to_string(writer), enables[writer]);
            graph.bindInputPort("decoded_addr_" + std::to_string(writer), addresses[writer]);
        }

        ValueId selected;
        addSliceArrayAnchor(graph, "decoded", regs, index, width, selected);
        graph.bindOutputPort("decoded_selected", selected);

        const ValueId mask = addConstant(graph,
                                         "decoded_mask_op",
                                         "decoded_mask",
                                         width,
                                         "3'b111");
        const ValueId writeData = addConstant(graph,
                                              "decoded_data_op",
                                              "decoded_data",
                                              width,
                                              "3'd5");
        const ValueId resetData = addConstant(graph,
                                              "decoded_reset_data_op",
                                              "decoded_reset_data",
                                              width,
                                              "3'd0");
        const ValueId notReset = addUnary(graph,
                                          OperationKind::kLogicNot,
                                          "decoded_not_reset_op",
                                          "decoded_not_reset",
                                          reset);

        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string suffix = std::to_string(row);
            const ValueId rowConst = addConstant(graph,
                                                 "decoded_row_" + suffix + "_op",
                                                 "decoded_row_" + suffix,
                                                 2,
                                                 "2'd" + suffix);
            const ValueId highHit = addBinary(graph,
                                              OperationKind::kEq,
                                              "decoded_high_hit_" + suffix + "_op",
                                              "decoded_high_hit_" + suffix,
                                              highAddr,
                                              rowConst);
            const ValueId highGuard = addBinary(graph,
                                                OperationKind::kLogicAnd,
                                                "decoded_high_guard_" + suffix + "_op",
                                                "decoded_high_guard_" + suffix,
                                                highEnable,
                                                highHit);
            const ValueId notHighGuard = addUnary(graph,
                                                  OperationKind::kLogicNot,
                                                  "decoded_not_high_guard_" + suffix + "_op",
                                                  "decoded_not_high_guard_" + suffix,
                                                  highGuard);

            std::array<ValueId, writers> writerGuards;
            for (std::size_t writer = 0; writer < writers; ++writer)
            {
                const std::string writerSuffix = suffix + "_" + std::to_string(writer);
                const ValueId hit = addBinary(graph,
                                              OperationKind::kEq,
                                              "decoded_hit_" + writerSuffix + "_op",
                                              "decoded_hit_" + writerSuffix,
                                              addresses[writer],
                                              rowConst);
                writerGuards[writer] = addBinary(graph,
                                                 OperationKind::kLogicAnd,
                                                 "decoded_writer_guard_" + writerSuffix + "_op",
                                                 "decoded_writer_guard_" + writerSuffix,
                                                 enables[writer],
                                                 hit);
            }
            const ValueId firstOr = addBinary(graph,
                                              OperationKind::kLogicOr,
                                              "decoded_first_or_" + suffix + "_op",
                                              "decoded_first_or_" + suffix,
                                              writerGuards[0],
                                              writerGuards[1]);
            const ValueId anyWriter = addBinary(graph,
                                                OperationKind::kLogicOr,
                                                "decoded_any_writer_" + suffix + "_op",
                                                "decoded_any_writer_" + suffix,
                                                firstOr,
                                                writerGuards[2]);
            const ValueId priorityAllowed = addBinary(graph,
                                                      OperationKind::kLogicAnd,
                                                      "decoded_priority_allowed_" + suffix + "_op",
                                                      "decoded_priority_allowed_" + suffix,
                                                      notReset,
                                                      notHighGuard);
            const ValueId active = addBinary(graph,
                                             OperationKind::kLogicAnd,
                                             "decoded_active_" + suffix + "_op",
                                             "decoded_active_" + suffix,
                                             priorityAllowed,
                                             anyWriter);
            const ValueId update = addBinary(graph,
                                             OperationKind::kLogicOr,
                                             "decoded_update_" + suffix + "_op",
                                             "decoded_update_" + suffix,
                                             reset,
                                             active);
            const ValueId next = addMux(graph,
                                        "decoded_next_" + suffix + "_op",
                                        "decoded_next_" + suffix,
                                        active,
                                        writeData,
                                        resetData,
                                        width);
            addRegisterWrite(graph,
                             "decoded_write_" + suffix,
                             regs[row],
                             update,
                             next,
                             mask,
                             clk);
        }
        return design;
    }

    Design buildWriteOnlyDecodedStorageDesign(std::size_t writers = 65,
                                              std::size_t rows = 4)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        int32_t addrWidth = 1;
        while ((std::size_t{1} << addrWidth) <= rows)
        {
            ++addrWidth;
        }
        std::vector<std::string> regs;
        regs.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "write_only_r" + std::to_string(row + 1);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            ValueId read;
            addRegisterRead(graph,
                            "write_only_read_" + std::to_string(row + 1) + "_op",
                            "write_only_read_" + std::to_string(row + 1),
                            reg,
                            width,
                            read);
            graph.bindOutputPort("write_only_out_" + std::to_string(row + 1), read);
        }

        const ValueId clk = makeLogicValue(graph, "write_only_clk", 1);
        const ValueId reset = makeLogicValue(graph, "write_only_reset", 1);
        graph.bindInputPort("write_only_clk", clk);
        graph.bindInputPort("write_only_reset", reset);
        const ValueId notReset = addUnary(graph,
                                          OperationKind::kLogicNot,
                                          "write_only_not_reset_op",
                                          "write_only_not_reset",
                                          reset);
        const ValueId mask = addConstant(graph,
                                         "write_only_mask_op",
                                         "write_only_mask",
                                         width,
                                         "8'hff");
        std::vector<ValueId> enables(writers);
        std::vector<ValueId> addresses(writers);
        std::vector<ValueId> data(writers);
        const ValueId commitSize = makeLogicValue(graph, "write_only_commit_size", 9);
        graph.bindInputPort("write_only_commit_size", commitSize);
        for (std::size_t writer = 0; writer < writers; ++writer)
        {
            const std::string suffix = std::to_string(writer);
            enables[writer] = writer == 0
                                  ? addUnary(graph,
                                             OperationKind::kReduceAnd,
                                             "write_only_full_commit_op",
                                             "write_only_full_commit",
                                             commitSize)
                                  : makeLogicValue(graph, "write_only_enable_" + suffix, 1);
            addresses[writer] = makeLogicValue(graph, "write_only_addr_" + suffix, addrWidth);
            data[writer] = makeLogicValue(graph, "write_only_data_" + suffix, width);
            if (writer != 0)
            {
                graph.bindInputPort("write_only_enable_" + suffix, enables[writer]);
            }
            graph.bindInputPort("write_only_addr_" + suffix, addresses[writer]);
            graph.bindInputPort("write_only_data_" + suffix, data[writer]);
        }

        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string rowSuffix = std::to_string(row + 1);
            const ValueId fallback = addConstant(graph,
                                                 "write_only_fallback_" + rowSuffix + "_op",
                                                 "write_only_fallback_" + rowSuffix,
                                                 width,
                                                 "8'd" + rowSuffix);
            const ValueId rowConst = addConstant(graph,
                                                 "write_only_row_" + rowSuffix + "_op",
                                                 "write_only_row_" + rowSuffix,
                                                 addrWidth,
                                                 std::to_string(addrWidth) + "'d" + rowSuffix);
            std::vector<ValueId> guards;
            guards.reserve(writers);
            ValueId priorityAllowed = notReset;
            for (std::size_t writer = 0; writer < writers; ++writer)
            {
                const std::string suffix = rowSuffix + "_" + std::to_string(writer);
                const ValueId hit = addBinary(graph,
                                              OperationKind::kEq,
                                              "write_only_hit_" + suffix + "_op",
                                              "write_only_hit_" + suffix,
                                              addresses[writer],
                                              rowConst);
                const ValueId rawGuard = addBinary(graph,
                                                   OperationKind::kLogicAnd,
                                                   "write_only_raw_" + suffix + "_op",
                                                   "write_only_raw_" + suffix,
                                                   enables[writer],
                                                   hit);
                guards.push_back(addBinary(graph,
                                           OperationKind::kLogicAnd,
                                           "write_only_guard_" + suffix + "_op",
                                           "write_only_guard_" + suffix,
                                           priorityAllowed,
                                           rawGuard));
                const ValueId notRaw = addUnary(graph,
                                                OperationKind::kLogicNot,
                                                "write_only_not_raw_" + suffix + "_op",
                                                "write_only_not_raw_" + suffix,
                                                rawGuard);
                priorityAllowed = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "write_only_allowed_" + suffix + "_op",
                                            "write_only_allowed_" + suffix,
                                            priorityAllowed,
                                            notRaw);
            }

            ValueId update = guards.front();
            for (std::size_t writer = 1; writer < writers; ++writer)
            {
                const std::string suffix = rowSuffix + "_" + std::to_string(writer);
                update = addBinary(graph,
                                   OperationKind::kLogicOr,
                                   "write_only_update_" + suffix + "_op",
                                   "write_only_update_" + suffix,
                                   update,
                                   guards[writer]);
            }
            update = addBinary(graph,
                               OperationKind::kLogicOr,
                               "write_only_reset_update_" + rowSuffix + "_op",
                               "write_only_reset_update_" + rowSuffix,
                               reset,
                               update);
            ValueId next = fallback;
            for (std::size_t writer = writers; writer-- > 0;)
            {
                const std::string suffix = rowSuffix + "_" + std::to_string(writer);
                next = addMux(graph,
                              "write_only_next_" + suffix + "_op",
                              "write_only_next_" + suffix,
                              guards[writer],
                              data[writer],
                              next,
                              width);
            }
            addRegisterWrite(graph,
                             "write_only_write_" + rowSuffix,
                             regs[row],
                             update,
                             next,
                             mask,
                             clk);
        }
        return design;
    }

    Design buildTrueStorageOnlyDesign(bool completeWrites,
                                      std::size_t rows = 4,
                                      bool sharedPackedView = false,
                                      std::size_t leadingEdgePadding = 0)
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        std::vector<std::string> regs;
        std::vector<ValueId> reads;
        regs.reserve(rows);
        reads.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row)
        {
            const std::string reg = "shared_r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            const ValueId readValue = makeLogicValue(graph, reg + "_read", width);
            const OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                           graph.internSymbol(reg + "_read_op"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", reg);
            reads.push_back(readValue);
        }

        const std::size_t repeatCount = sharedPackedView ? 1 : 2;
        const std::size_t viewRows = rows * repeatCount + leadingEdgePadding;
        const ValueId packed = makeLogicValue(graph, "shared_circular_packed", width * viewRows);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("shared_circular_concat"));
        for (std::size_t padding = 0; padding < leadingEdgePadding; ++padding)
        {
            graph.addOperand(concat, reads.front());
        }
        for (std::size_t repeat = 0; repeat < repeatCount; ++repeat)
        {
            for (auto it = reads.rbegin(); it != reads.rend(); ++it)
            {
                graph.addOperand(concat, *it);
            }
        }
        graph.addResult(concat, packed);
        if (!sharedPackedView)
        {
            graph.bindOutputPort("packed", packed);

            const ValueId extra = makeLogicValue(graph, "shared_extra", width);
            const OperationId extraAssign = graph.createOperation(OperationKind::kAssign,
                                                                  graph.internSymbol("shared_extra_assign"));
            graph.addOperand(extraAssign, reads.front());
            graph.addResult(extraAssign, extra);
            graph.bindOutputPort("extra", extra);
        }

        int32_t addrWidth = 1;
        while ((std::size_t{1} << addrWidth) < viewRows)
        {
            ++addrWidth;
        }
        const ValueId addr = makeLogicValue(graph, "shared_addr", addrWidth);
        const ValueId wen = makeLogicValue(graph, "shared_wen", 1);
        const ValueId data = makeLogicValue(graph, "shared_data", width);
        const ValueId clk = makeLogicValue(graph, "shared_clk", 1);
        const ValueId mask = addConstant(graph, "shared_mask_op", "shared_mask", width, "8'hff");
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("wen", wen);
        graph.bindInputPort("data", data);
        graph.bindInputPort("clk", clk);
        if (sharedPackedView)
        {
            const ValueId secondAddr = makeLogicValue(graph, "shared_addr_2", addrWidth);
            graph.bindInputPort("addr2", secondAddr);
            const std::array<ValueId, 2> readAddresses{addr, secondAddr};
            for (std::size_t readIndex = 0; readIndex < readAddresses.size(); ++readIndex)
            {
                const std::string suffix = std::to_string(readIndex);
                const ValueId selected = makeLogicValue(graph, "shared_selected" + suffix, width);
                const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                                graph.internSymbol("shared_slice" + suffix));
                graph.addOperand(slice, packed);
                graph.addOperand(slice, readAddresses[readIndex]);
                graph.addResult(slice, selected);
                graph.setAttr(slice, "sliceWidth", int64_t{width});
                graph.bindOutputPort("selected" + suffix, selected);
            }
        }
        const std::size_t writeRows = completeWrites ? rows : rows - 1;
        for (std::size_t row = 0; row < writeRows; ++row)
        {
            const ValueId rowConst = addConstant(graph,
                                                 "shared_row" + std::to_string(row) + "_op",
                                                 "shared_row" + std::to_string(row),
                                                 addrWidth,
                                                 std::to_string(addrWidth) + "'d" + std::to_string(row));
            const ValueId hit = addBinary(graph,
                                          OperationKind::kEq,
                                          "shared_hit" + std::to_string(row) + "_op",
                                          "shared_hit" + std::to_string(row),
                                          addr,
                                          rowConst,
                                          1);
            const ValueId guard = addBinary(graph,
                                            OperationKind::kLogicAnd,
                                            "shared_guard" + std::to_string(row) + "_op",
                                            "shared_guard" + std::to_string(row),
                                            wen,
                                            hit,
                                            1);
            addRegisterWrite(graph,
                             "shared_write" + std::to_string(row),
                             regs[row],
                             guard,
                             data,
                             mask,
                             clk);
        }
        return design;
    }

    int testSliceDynamicIntent()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        std::vector<ValueId> readValues;
        std::vector<OperationId> readOps;
        readValues.resize(4);
        readOps.resize(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            addRegister(graph, reg, 8);
            readOps[static_cast<std::size_t>(row)] =
                addRegisterRead(graph,
                                reg + "_read_op",
                                reg + "_read",
                                reg,
                                8,
                                readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);
        const ValueId elementWidth = addConstant(graph, "const_elem_width", "elem_width", 4, "4'd8");
        ValueId start = makeLogicValue(graph, "start", 4);
        const OperationId mul = graph.createOperation(OperationKind::kMul,
                                                      graph.internSymbol("start_mul"));
        graph.addOperand(mul, index);
        graph.addOperand(mul, elementWidth);
        graph.addResult(mul, start);

        ValueId selected = makeLogicValue(graph, "selected", 8);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation sliceOp = graph.getOperation(slice);
        if (getAttr<std::string>(sliceOp, "regToMem.intent.sliceKind").value_or("") != "slice-dynamic")
        {
            return fail("sliceDynamic anchor was not annotated");
        }
        const std::string group = getAttr<std::string>(sliceOp, "regToMem.intent.group").value_or("");
        if (group.empty())
        {
            return fail("sliceDynamic anchor missing group attr");
        }
        if (getAttr<int64_t>(sliceOp, "regToMem.intent.elementWidth").value_or(0) != 8 ||
            getAttr<int64_t>(sliceOp, "regToMem.intent.elementCount").value_or(0) != 4)
        {
            return fail("sliceDynamic anchor has wrong element shape attrs");
        }

        const Operation concatOp = graph.getOperation(concat);
        const auto regSymbols = getAttr<std::vector<std::string>>(concatOp, "regToMem.intent.regSymbols");
        const auto operandRows = getAttr<std::vector<int64_t>>(concatOp, "regToMem.intent.operandRows");
        if (!regSymbols || *regSymbols != std::vector<std::string>{"r0", "r1", "r2", "r3"})
        {
            return fail("concat regSymbols attr does not preserve row order");
        }
        if (!operandRows || *operandRows != std::vector<int64_t>{3, 2, 1, 0})
        {
            return fail("concat operandRows attr is wrong");
        }

        for (int row = 0; row < 4; ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation("r" + std::to_string(row)));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("register row attrs are wrong");
            }
            const Operation readOp = graph.getOperation(readOps[static_cast<std::size_t>(row)]);
            if (getAttr<std::string>(readOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("read row attrs are wrong");
            }
        }
        return 0;
    }

    int testSliceDynamicWidthOneBareIndexIntent()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        std::vector<ValueId> readValues(4);
        std::vector<OperationId> readOps(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            addRegister(graph, reg, 1);
            readOps[static_cast<std::size_t>(row)] =
                addRegisterRead(graph,
                                reg + "_read_op",
                                reg + "_read",
                                reg,
                                1,
                                readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 4);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);

        ValueId selected = makeLogicValue(graph, "selected", 1);
        const OperationId slice = graph.createOperation(OperationKind::kSliceDynamic,
                                                        graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{1});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation sliceOp = graph.getOperation(slice);
        if (getAttr<std::string>(sliceOp, "regToMem.intent.sliceKind").value_or("") != "slice-dynamic")
        {
            return fail("width-one dynamic slice with bare index was not annotated");
        }
        const std::string group = getAttr<std::string>(sliceOp, "regToMem.intent.group").value_or("");
        if (group.empty())
        {
            return fail("width-one dynamic slice missing group attr");
        }
        if (getAttr<int64_t>(sliceOp, "regToMem.intent.elementWidth").value_or(0) != 1 ||
            getAttr<int64_t>(sliceOp, "regToMem.intent.elementCount").value_or(0) != 4)
        {
            return fail("width-one dynamic slice has wrong element shape attrs");
        }

        for (int row = 0; row < 4; ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation("r" + std::to_string(row)));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("width-one register row attrs are wrong");
            }
            const Operation readOp = graph.getOperation(readOps[static_cast<std::size_t>(row)]);
            if (getAttr<std::string>(readOp, "regToMem.intent.group").value_or("") != group ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").value_or(-1) != row)
            {
                return fail("width-one read row attrs are wrong");
            }
        }

        return 0;
    }

    int testIntentRejectsSharedConcat()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        std::vector<std::string> regs;
        std::vector<ValueId> readValues(4);
        regs.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            addRegisterRead(graph,
                            reg + "_read_op",
                            reg + "_read",
                            reg,
                            width,
                            readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0 = makeLogicValue(graph, "selected0", width);
        const OperationId slice0 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected0_slice"));
        graph.addOperand(slice0, packed);
        graph.addOperand(slice0, index0);
        graph.addResult(slice0, selected0);
        graph.setAttr(slice0, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1 = makeLogicValue(graph, "selected1", width);
        const OperationId slice1 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected1_slice"));
        graph.addOperand(slice1, packed);
        graph.addOperand(slice1, index1);
        graph.addResult(slice1, selected1);
        graph.setAttr(slice1, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }

        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kConcat) != 1)
        {
            return fail("shared-concat reject should not true-merge or delete original ops");
        }
        if (hasIntentGroup(graph, slice0) || hasIntentGroup(graph, slice1))
        {
            return fail("shared-concat slices should not be annotated under single-user anchor rules");
        }
        const Operation concatOp = graph.getOperation(concat);
        if (getAttr<std::string>(concatOp, "regToMem.intent.group").has_value())
        {
            return fail("shared concat should not get intent attrs under single-user anchor rules");
        }
        return 0;
    }

    int testIntentRejectsReadWithExtraUser()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        std::vector<std::string> regs;
        std::vector<ValueId> readValues(4);
        regs.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            regs.push_back(reg);
            addRegister(graph, reg, width);
            addRegisterRead(graph,
                            reg + "_read_op",
                            reg + "_read",
                            reg,
                            width,
                            readValues[static_cast<std::size_t>(row)]);
        }

        ValueId packed = makeLogicValue(graph, "packed_extra_user", 32);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_extra_user_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);

        ValueId extra = makeLogicValue(graph, "extra_user", width);
        const OperationId extraAssign = graph.createOperation(OperationKind::kAssign,
                                                              graph.internSymbol("extra_user_assign"));
        graph.addOperand(extraAssign, readValues.front());
        graph.addResult(extraAssign, extra);
        graph.bindOutputPort("extra", extra);

        const ValueId index = makeLogicValue(graph, "index", 2);
        graph.bindInputPort("index", index);
        ValueId selected = makeLogicValue(graph, "selected_extra_user", width);
        const OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                        graph.internSymbol("selected_extra_user_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, index);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected", selected);

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }

        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 4 ||
            countKind(graph, OperationKind::kAssign) != 1)
        {
            return fail("extra-user reject should not true-merge or delete original ops");
        }
        if (hasIntentGroup(graph, slice))
        {
            return fail("slice with extra read user should not be annotated under single-user anchor rules");
        }
        const Operation concatOp = graph.getOperation(concat);
        if (getAttr<std::string>(concatOp, "regToMem.intent.group").has_value())
        {
            return fail("concat with extra read user should not get intent attrs under single-user anchor rules");
        }
        return 0;
    }

    int testIntentKeepsStableFamilyForConflictingRowOrder()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> reversedRegs{"r3", "r2", "r1", "r0"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", reversedRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("stable row-order family should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("conflicting reversed row-order anchor should not be annotated");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation(regs[row]));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != group0 ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != static_cast<int64_t>(row))
            {
                return fail("stable row-order family should own register storage attrs");
            }
        }
        return 0;
    }

    int testIntentAllowsOverlappingSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> subsetRegs{"r0", "r1"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", subsetRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("overlapping subset anchors should be separate intent views");
        }

        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("overlapping subset view should share the superset storage group");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.elementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.elementCount").value_or(0) != 2)
        {
            return fail("overlapping subset views should preserve local element counts");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.storageElementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageElementCount").value_or(0) != 4)
        {
            return fail("overlapping subset views should use superset storage element count");
        }
        if (getAttr<int64_t>(slice0Op, "regToMem.intent.storageRowOffset").value_or(-1) != 0 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageRowOffset").value_or(-1) != 0)
        {
            return fail("prefix subset view should have zero storage row offset");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation regOp = graph.getOperation(graph.findOperation(regs[row]));
            if (getAttr<std::string>(regOp, "regToMem.intent.group").value_or("") != storage0 ||
                getAttr<int64_t>(regOp, "regToMem.intent.row").value_or(-1) != static_cast<int64_t>(row))
            {
                return fail("storage register attrs should be owned by the superset storage group");
            }
        }
        return 0;
    }

    int testIntentAllowsMiddleSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        const std::vector<std::string> middleRegs{"r1", "r2"};
        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", middleRegs, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("middle subset anchors should be separate intent views");
        }

        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("middle subset view should share superset storage");
        }
        if (getAttr<int64_t>(slice1Op, "regToMem.intent.elementCount").value_or(0) != 2 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageElementCount").value_or(0) != 4 ||
            getAttr<int64_t>(slice1Op, "regToMem.intent.storageRowOffset").value_or(-1) != 1)
        {
            return fail("middle subset view should preserve local count and storage row offset");
        }
        return 0;
    }

    int testIntentRejectsSharedReadSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        std::vector<ValueId> readValues(regs.size());
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            addRegister(graph, regs[row], width);
            addRegisterRead(graph,
                            regs[row] + "_read_op",
                            regs[row] + "_read",
                            regs[row],
                            width,
                            readValues[row]);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId packedFull = makeLogicValue(graph, "packed_full", 32);
        const OperationId concatFull = graph.createOperation(OperationKind::kConcat,
                                                             graph.internSymbol("packed_full_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concatFull, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concatFull, packedFull);

        ValueId selectedFull = makeLogicValue(graph, "selected_full", width);
        const OperationId sliceFull = graph.createOperation(OperationKind::kSliceArray,
                                                            graph.internSymbol("selected_full_slice"));
        graph.addOperand(sliceFull, packedFull);
        graph.addOperand(sliceFull, index0);
        graph.addResult(sliceFull, selectedFull);
        graph.setAttr(sliceFull, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected_full", selectedFull);

        ValueId packedSubset = makeLogicValue(graph, "packed_subset", 16);
        const OperationId concatSubset = graph.createOperation(OperationKind::kConcat,
                                                               graph.internSymbol("packed_subset_concat"));
        graph.addOperand(concatSubset, readValues[2]);
        graph.addOperand(concatSubset, readValues[1]);
        graph.addResult(concatSubset, packedSubset);

        ValueId selectedSubset = makeLogicValue(graph, "selected_subset", width);
        const OperationId sliceSubset = graph.createOperation(OperationKind::kSliceArray,
                                                              graph.internSymbol("selected_subset_slice"));
        graph.addOperand(sliceSubset, packedSubset);
        graph.addOperand(sliceSubset, index1);
        graph.addResult(sliceSubset, selectedSubset);
        graph.setAttr(sliceSubset, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected_subset", selectedSubset);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        if (hasIntentGroup(graph, sliceFull))
        {
            return fail("shared-read superset should not be annotated under single-user anchor rules");
        }
        if (hasIntentGroup(graph, sliceSubset))
        {
            return fail("shared-read subset should not be annotated under single-user anchor rules");
        }
        for (std::size_t row = 0; row < regs.size(); ++row)
        {
            const Operation readOp = graph.getOperation(graph.valueDef(readValues[row]));
            if (getAttr<std::string>(readOp, "regToMem.intent.group").has_value() ||
                getAttr<int64_t>(readOp, "regToMem.intent.row").has_value())
            {
                return fail("shared-read rejected anchors should not leave read attrs");
            }
        }
        return 0;
    }

    int testIntentRejectsSiblingSharedReadSubsetView()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        const ValueId index2 = makeLogicValue(graph, "index2", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);
        graph.bindInputPort("index2", index2);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", regs, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        std::vector<ValueId> sharedReads(3);
        for (std::size_t row = 0; row < sharedReads.size(); ++row)
        {
            addRegisterRead(graph,
                            "shared_r" + std::to_string(row) + "_read_op",
                            "shared_r" + std::to_string(row) + "_read",
                            regs[row],
                            width,
                            sharedReads[row]);
        }

        ValueId packed1 = makeLogicValue(graph, "packed1", 16);
        const OperationId concat1 = graph.createOperation(OperationKind::kConcat,
                                                          graph.internSymbol("packed1_concat"));
        graph.addOperand(concat1, sharedReads[1]);
        graph.addOperand(concat1, sharedReads[0]);
        graph.addResult(concat1, packed1);

        ValueId selected1 = makeLogicValue(graph, "selected1", width);
        const OperationId slice1 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected1_slice"));
        graph.addOperand(slice1, packed1);
        graph.addOperand(slice1, index1);
        graph.addResult(slice1, selected1);
        graph.setAttr(slice1, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected1", selected1);

        ValueId packed2 = makeLogicValue(graph, "packed2", 16);
        const OperationId concat2 = graph.createOperation(OperationKind::kConcat,
                                                          graph.internSymbol("packed2_concat"));
        graph.addOperand(concat2, sharedReads[2]);
        graph.addOperand(concat2, sharedReads[1]);
        graph.addResult(concat2, packed2);

        ValueId selected2 = makeLogicValue(graph, "selected2", width);
        const OperationId slice2 = graph.createOperation(OperationKind::kSliceArray,
                                                         graph.internSymbol("selected2_slice"));
        graph.addOperand(slice2, packed2);
        graph.addOperand(slice2, index2);
        graph.addResult(slice2, selected2);
        graph.setAttr(slice2, "sliceWidth", int64_t{width});
        graph.bindOutputPort("selected2", selected2);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("independent root view should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("first sibling view with a shared read should not be annotated");
        }
        if (hasIntentGroup(graph, slice2))
        {
            return fail("second sibling view with a shared read should not be annotated");
        }
        const Operation sharedRead1 = graph.getOperation(graph.valueDef(sharedReads[1]));
        if (getAttr<std::string>(sharedRead1, "regToMem.intent.group").has_value() ||
            getAttr<int64_t>(sharedRead1, "regToMem.intent.row").has_value())
        {
            return fail("rejected sibling views should not leave shared read attrs");
        }
        return 0;
    }

    int testIntentKeepsStableFamilyForPartialOverlap()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 1);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);

        ValueId selected0;
        const OperationId slice0 = addSliceArrayAnchor(graph, "a0", std::vector<std::string>{"r0", "r1"}, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1;
        const OperationId slice1 = addSliceArrayAnchor(graph, "a1", std::vector<std::string>{"r1", "r2"}, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const std::string group0 =
            getAttr<std::string>(graph.getOperation(slice0), "regToMem.intent.group").value_or("");
        if (group0.empty())
        {
            return fail("stable partial-overlap family should remain annotated");
        }
        if (hasIntentGroup(graph, slice1))
        {
            return fail("later partial-overlap family without superset storage should not be annotated");
        }
        if (getAttr<std::string>(graph.getOperation(graph.findOperation("r2")), "regToMem.intent.group").has_value())
        {
            return fail("register only used by dropped partial-overlap family should not get intent attrs");
        }
        return 0;
    }

    int testIntentDoesNotDropCompatibleFamilyBecauseOfBridgeConflict()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        design.markAsTop(graph.symbol());

        constexpr int32_t width = 8;
        const std::vector<std::string> regs{"r0", "r1", "r2", "r3", "r4"};
        for (const std::string &reg : regs)
        {
            addRegister(graph, reg, width);
        }

        const ValueId index0 = makeLogicValue(graph, "index0", 2);
        const ValueId index1 = makeLogicValue(graph, "index1", 1);
        const ValueId index2 = makeLogicValue(graph, "index2", 2);
        graph.bindInputPort("index0", index0);
        graph.bindInputPort("index1", index1);
        graph.bindInputPort("index2", index2);

        ValueId selected0;
        const OperationId slice0 =
            addSliceArrayAnchor(graph, "a0", std::vector<std::string>{"r0", "r1", "r2", "r3"}, index0, width, selected0);
        graph.bindOutputPort("selected0", selected0);

        ValueId selected1;
        const OperationId slice1 =
            addSliceArrayAnchor(graph, "a1", std::vector<std::string>{"r0", "r1"}, index1, width, selected1);
        graph.bindOutputPort("selected1", selected1);

        ValueId selected2;
        const OperationId slice2 =
            addSliceArrayAnchor(graph, "a2", std::vector<std::string>{"r2", "r3", "r4"}, index2, width, selected2);
        graph.bindOutputPort("selected2", selected2);

        if (const int rc = runIntentPass(design); rc != 0)
        {
            return rc;
        }

        const Operation slice0Op = graph.getOperation(slice0);
        const Operation slice1Op = graph.getOperation(slice1);
        const std::string group0 = getAttr<std::string>(slice0Op, "regToMem.intent.group").value_or("");
        const std::string group1 = getAttr<std::string>(slice1Op, "regToMem.intent.group").value_or("");
        if (group0.empty() || group1.empty() || group0 == group1)
        {
            return fail("compatible superset/subset family should survive bridge conflict");
        }
        const std::string storage0 =
            getAttr<std::string>(slice0Op, "regToMem.intent.storageGroup").value_or(group0);
        const std::string storage1 =
            getAttr<std::string>(slice1Op, "regToMem.intent.storageGroup").value_or(group1);
        if (storage0.empty() || storage0 != storage1)
        {
            return fail("compatible family should still share storage under bridge conflict");
        }
        if (hasIntentGroup(graph, slice2))
        {
            return fail("bridge conflict family should not be annotated");
        }
        if (getAttr<std::string>(graph.getOperation(graph.findOperation("r4")), "regToMem.intent.group").has_value())
        {
            return fail("register only used by bridge conflict should not get intent attrs");
        }
        return 0;
    }

    int testTrueMergeSliceArray()
    {
        Design design = buildTrueMergeDesign(false, false);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0 ||
            countKind(graph, OperationKind::kConcat) != 0 ||
            countKind(graph, OperationKind::kSliceArray) != 0)
        {
            return fail("true merge did not remove register/read/write/concat/slice ops");
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1)
        {
            return fail("true merge did not create expected memory ports");
        }
        const Operation mem = graph.getOperation(opsOfKind(graph, OperationKind::kMemory).front());
        const std::string memSymbol = std::string(mem.symbolText());
        if (getAttr<int64_t>(mem, "width").value_or(0) != 8 ||
            getAttr<int64_t>(mem, "row").value_or(0) != 4)
        {
            return fail("true merge memory shape is wrong");
        }
        const Operation read = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryReadPort).front());
        const Operation write = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryWritePort).front());
        if (getAttr<std::string>(read, "memSymbol").value_or("") != memSymbol ||
            getAttr<std::string>(write, "memSymbol").value_or("") != memSymbol)
        {
            return fail("true merge memory ports reference wrong memSymbol");
        }
        const ValueId output = graph.outputPortValue("selected");
        if (!output.valid() || graph.valueDef(output) != read.id())
        {
            return fail("true merge did not rebind output port to memory read result");
        }
        return 0;
    }

    int testTrueMergeReduceAndLastRowGuard()
    {
        Design design = buildTrueMergeDesign(false, false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge did not match reduce-and all-ones row guard");
        }
        return 0;
    }

    int testTrueMergeMultiAnchor()
    {
        Design design = buildTrueMergeDesign(true, false);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 2 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0)
        {
            return fail("true merge multi-anchor shape is wrong");
        }
        return 0;
    }

    int testTrueMergeMultipleWriteFamilies()
    {
        Design design = buildTrueMergeDesign(false, false, false, true);
        Graph &graph = *design.findGraph("top");
        const ValueId addr = graph.inputPortValue("addr");
        const ValueId data = graph.inputPortValue("data");
        const ValueId addr2 = graph.inputPortValue("addr2");
        const ValueId data2 = graph.inputPortValue("data2");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge multiple-write family shape is wrong");
        }
        const std::vector<OperationId> writes = opsOfKind(graph, OperationKind::kMemoryWritePort);
        if (writes.size() != 2)
        {
            return fail("true merge multiple-write family count is wrong");
        }
        const Operation first = graph.getOperation(writes[0]);
        const Operation second = graph.getOperation(writes[1]);
        if (first.operands().size() < 4 || second.operands().size() < 4 ||
            first.operands()[1] != addr2 || first.operands()[2] != data2 ||
            second.operands()[1] != addr || second.operands()[2] != data)
        {
            return fail("true merge multiple-write family order or operands are wrong");
        }
        return 0;
    }

    int testTrueMergeConsolidatedWriteFamiliesWithReset()
    {
        Design design = buildTrueMergeDesign(false, true, false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge consolidated write families with reset shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3 || graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("true merge consolidated reset did not create packed fill data");
        }
        return 0;
    }

    int testTrueMergeConsolidatedWriteFamiliesWithMultipleResetTerms()
    {
        Design design = buildTrueMergeDesign(false, true, false, true, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge multiple-reset family shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().empty() ||
            graph.getOperation(graph.valueDef(fill.operands().front())).kind() != OperationKind::kLogicOr)
        {
            return fail("true merge multiple-reset family did not rebuild the fill guard OR");
        }
        return 0;
    }

    int testTrueMergeConsolidatedFallbackDataWriteFamily()
    {
        Design design = buildTrueMergeDesign(false, false, false, true, false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 3 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 0 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge did not recover the fallback-data write family");
        }
        return 0;
    }

    int testTrueMergeStorageOnlySharedReads()
    {
        Design design = buildTrueStorageOnlyDesign(true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 4 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0 ||
            countKind(graph, OperationKind::kConcat) != 1 ||
            countKind(graph, OperationKind::kAssign) != 1)
        {
            return fail("true storage-only shared-read rewrite shape is wrong");
        }
        const ValueId packed = graph.outputPortValue("packed");
        const ValueId extra = graph.outputPortValue("extra");
        if (!packed.valid() || !extra.valid() ||
            graph.getOperation(graph.valueDef(packed)).kind() != OperationKind::kConcat ||
            graph.getOperation(graph.valueDef(extra)).kind() != OperationKind::kAssign)
        {
            return fail("true storage-only rewrite did not preserve circular concat and extra user");
        }
        return 0;
    }

    int testTrueMergeStorageOnlySharedPackedView()
    {
        Design design = buildTrueStorageOnlyDesign(true, 4, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 4 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0 ||
            countKind(graph, OperationKind::kConcat) != 1 ||
            countKind(graph, OperationKind::kSliceArray) != 2)
        {
            return fail("true storage-only shared packed-view rewrite shape is wrong");
        }
        for (std::size_t readIndex = 0; readIndex < 2; ++readIndex)
        {
            const ValueId selected = graph.outputPortValue("selected" + std::to_string(readIndex));
            if (!selected.valid() ||
                graph.getOperation(graph.valueDef(selected)).kind() != OperationKind::kSliceArray)
            {
                return fail("true storage-only rewrite did not preserve shared packed-view slices");
            }
        }
        return 0;
    }

    int testTrueMergeStorageOnlyEdgePaddedPackedView()
    {
        Design design = buildTrueStorageOnlyDesign(true, 4, true, 2);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 4 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0 ||
            countKind(graph, OperationKind::kConcat) != 1 ||
            countKind(graph, OperationKind::kSliceArray) != 2 ||
            countKind(graph, OperationKind::kLt) != 1)
        {
            return fail("true storage-only edge-padded rewrite shape is wrong");
        }
        const Operation memory = graph.getOperation(opsOfKind(graph, OperationKind::kMemory).front());
        const Operation concat = graph.getOperation(opsOfKind(graph, OperationKind::kConcat).front());
        if (getAttr<int64_t>(memory, "row").value_or(0) != 4 ||
            concat.operands().size() != 6 ||
            concat.operands()[0] != concat.operands()[1] ||
            concat.operands()[0] != concat.operands().back())
        {
            return fail("edge-padded view no longer aliases padding to the edge storage row");
        }
        return 0;
    }

    int testTrueMergeStorageOnlyNonPowerOfTwoDomainGuard()
    {
        Design design = buildTrueStorageOnlyDesign(true, 3);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 3 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kLt) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0)
        {
            return fail("non-power-of-two storage rewrite did not use one compact domain guard");
        }
        const Operation memory = graph.getOperation(opsOfKind(graph, OperationKind::kMemory).front());
        if (getAttr<int64_t>(memory, "row").value_or(0) != 3)
        {
            return fail("non-power-of-two storage rewrite has wrong memory depth");
        }
        return 0;
    }

    int testTrueMergeStorageOnlyFailureDoesNotAnnotateIntent()
    {
        Design design = buildTrueStorageOnlyDesign(false);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 3)
        {
            return fail("failed true storage-only candidate changed scalar IR");
        }
        const OperationId concat = graph.findOperation("shared_circular_concat");
        if (!concat.valid() || hasIntentGroup(graph, concat))
        {
            return fail("failed true storage-only candidate leaked intent attrs");
        }
        for (std::size_t row = 0; row < 4; ++row)
        {
            const OperationId reg = graph.findOperation("shared_r" + std::to_string(row));
            if (!reg.valid() || hasIntentGroup(graph, reg))
            {
                return fail("failed true storage-only candidate annotated register intent");
            }
        }
        return 0;
    }

    int testTrueMergeResetFill()
    {
        Design design = buildTrueMergeDesign(false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge reset/fill shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3)
        {
            return fail("memory fill operand count is wrong");
        }
        if (graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("memory fill did not use packed reset data for per-row reset values");
        }
        return 0;
    }

    int testTrueMergeCompoundResetFill()
    {
        Design design = buildTrueMergeCompoundResetDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("true merge compound reset/write shape is wrong");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() != 3)
        {
            return fail("compound reset memory fill operand count is wrong");
        }
        if (graph.valueWidth(fill.operands()[1]) != 32)
        {
            return fail("compound reset memory fill did not use packed reset data");
        }
        return 0;
    }

    int testTrueMergeCompoundResetWithTrailingNoOpMux()
    {
        Design design = buildTrueMergeCompoundResetDesign(true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("trailing no-op reset mux blocked true merge");
        }
        return 0;
    }

    int testBranchNotInUpdateOuterResetRemainsScalar()
    {
        Design design = buildBranchNotInUpdateOuterResetDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 4)
        {
            return fail("outer reset-mux diagnostic changed rejected scalar storage");
        }
        return 0;
    }

    int testTrueMergeCombLanePackedMuxWrites()
    {
        Design design = buildTrueMergeCompoundResetDesign(false, true);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runCombLanePackAndSimplify(design); rc != 0)
        {
            return rc;
        }
        for (OperationId writeOpId : opsOfKind(graph, OperationKind::kRegisterWritePort))
        {
            const Operation writeOp = graph.getOperation(writeOpId);
            if (writeOp.operands().size() < 2)
            {
                return fail("comb-lane packed register write is malformed");
            }
            const OperationId dataDef = graph.valueDef(writeOp.operands()[1]);
            if (!dataDef.valid() || graph.getOperation(dataDef).kind() != OperationKind::kSliceStatic)
            {
                return fail("comb-lane-pack did not rewrite register write data to a static slice");
            }
        }
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 1 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("comb-lane packed mux writes blocked true merge");
        }
        const Operation write = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryWritePort).front());
        if (write.operands().size() < 3 || graph.valueWidth(write.operands()[2]) != 8)
        {
            return fail("projected comb-lane write data has the wrong width");
        }
        return 0;
    }

    int testTrueMergeCombLanePackedCompoundPriorityWrites()
    {
        Design design = buildTrueMergeCompoundPriorityDesign();
        Graph &graph = *design.findGraph("top");
        const ValueId highAddr = graph.inputPortValue("high_addr");
        const ValueId highLane = graph.inputPortValue("high_lane");
        const ValueId lowAddr = graph.inputPortValue("low_addr");
        if (const int rc = runCombLanePackAndSimplify(design); rc != 0)
        {
            return rc;
        }
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 2 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("comb-lane compound-priority writes blocked true merge");
        }
        for (OperationId writeOpId : opsOfKind(graph, OperationKind::kMemoryWritePort))
        {
            const Operation write = graph.getOperation(writeOpId);
            if (write.operands().size() < 4 || write.operands()[1] != lowAddr)
            {
                continue;
            }
            if (!hasNegatedDependencies(graph, write.operands()[0], highAddr, highLane))
            {
                return fail("compound-priority memory guard lost the high-address/lane conflict");
            }
            return 0;
        }
        return fail("compound-priority low-address memory write is missing");
    }

    int testTrueMergeOrDecodedPriorityWrites()
    {
        Design design = buildTrueMergeOrDecodedPriorityDesign();
        Graph &graph = *design.findGraph("top");
        const ValueId highEnable = graph.inputPortValue("decoded_high_enable");
        const ValueId highAddr = graph.inputPortValue("decoded_high_addr");
        std::unordered_set<ValueId, ValueIdHash> expectedAddresses;
        for (std::size_t writer = 0; writer < 3; ++writer)
        {
            expectedAddresses.insert(graph.inputPortValue("decoded_addr_" + std::to_string(writer)));
        }

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 3 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("OR-decoded priority writes blocked true merge");
        }
        for (OperationId writeOpId : opsOfKind(graph, OperationKind::kMemoryWritePort))
        {
            const Operation write = graph.getOperation(writeOpId);
            if (write.operands().size() < 4 || expectedAddresses.erase(write.operands()[1]) != 1)
            {
                return fail("OR-decoded memory write has the wrong address family");
            }
            if (!hasNegatedDependencies(graph, write.operands()[0], highEnable, highAddr))
            {
                return fail("OR-decoded memory guard lost the high-priority conflict");
            }
        }
        if (!expectedAddresses.empty())
        {
            return fail("OR-decoded memory write family is incomplete");
        }
        return 0;
    }

    int testTrueMergeWriteOnlyDecodedStorageWithRowOffset()
    {
        Design design = buildWriteOnlyDecodedStorageDesign();
        Graph &graph = *design.findGraph("top");
        std::unordered_set<ValueId, ValueIdHash> expectedAddresses;
        for (std::size_t writer = 0; writer < 65; ++writer)
        {
            expectedAddresses.insert(graph.inputPortValue("write_only_addr_" + std::to_string(writer)));
        }

        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryReadPort) != 4 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 65 ||
            countKind(graph, OperationKind::kMemoryFillPort) != 1 ||
            countKind(graph, OperationKind::kRegister) != 0 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 0 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 0)
        {
            return fail("write-only decoded storage did not true-merge");
        }
        const Operation memory = graph.getOperation(opsOfKind(graph, OperationKind::kMemory).front());
        if (getAttr<int64_t>(memory, "row").value_or(0) != 5)
        {
            return fail("write-only decoded storage did not preserve the missing row zero");
        }
        const Operation fill = graph.getOperation(opsOfKind(graph, OperationKind::kMemoryFillPort).front());
        if (fill.operands().size() < 2 || graph.valueWidth(fill.operands()[1]) != 40)
        {
            return fail("write-only decoded packed reset did not include the missing row");
        }
        const Operation fillData = graph.getOperation(graph.valueDef(fill.operands()[1]));
        if (fillData.kind() != OperationKind::kConcat || fillData.operands().size() != 5)
        {
            return fail("write-only decoded packed reset has the wrong row layout");
        }
        const Operation padding = graph.getOperation(graph.valueDef(fillData.operands().back()));
        if (padding.kind() != OperationKind::kConstant ||
            getAttr<std::string>(padding, "constValue").value_or("") != "8'd0")
        {
            return fail("write-only decoded packed reset did not zero the missing row");
        }
        for (OperationId writeOpId : opsOfKind(graph, OperationKind::kMemoryWritePort))
        {
            const Operation write = graph.getOperation(writeOpId);
            if (write.operands().size() < 4 || expectedAddresses.erase(write.operands()[1]) != 1)
            {
                return fail("write-only decoded memory write has the wrong address family");
            }
        }
        if (!expectedAddresses.empty())
        {
            return fail("write-only decoded memory write family is incomplete");
        }
        if (countKind(graph, OperationKind::kGe) != 65 || countKind(graph, OperationKind::kLt) != 65)
        {
            return fail("write-only decoded memory writes lost the offset/domain guard");
        }
        return 0;
    }

    int testTrueMergeWriteOnlyDecodedStorageRejectsSmallFamily()
    {
        Design design = buildWriteOnlyDecodedStorageDesign(63);
        Graph &graph = *design.findGraph("top");
        if (const int rc = runTruePass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 4)
        {
            return fail("small write-only decoded family bypassed the discovery threshold");
        }
        return 0;
    }

    int testTrueMergeWriteOnlyDecodedStorageCanBeDisabled()
    {
        Design design = buildWriteOnlyDecodedStorageDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runPass(design, true, false, false); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 4)
        {
            return fail("disabled write-only decoded storage changed scalar IR");
        }
        return 0;
    }

    int testTrueMergeWriteOnlyDecodedStorageOrderedWrites()
    {
        constexpr std::size_t writers = 65;
        constexpr std::size_t rows = 4;
        Design design = buildWriteOnlyDecodedStorageDesign(writers);
        Graph &graph = *design.findGraph("top");
        std::unordered_map<ValueId, int64_t, ValueIdHash> expectedPriorityByAddress;
        for (std::size_t writer = 0; writer < writers; ++writer)
        {
            expectedPriorityByAddress.emplace(
                graph.inputPortValue("write_only_addr_" + std::to_string(writer)),
                static_cast<int64_t>(writer));
        }
        if (const int rc = runOrderedTruePass(design); rc != 0)
        {
            return rc;
        }
        const std::vector<OperationId> writes = opsOfKind(graph, OperationKind::kMemoryWritePort);
        if (writes.size() != writers || countKind(graph, OperationKind::kEq) != rows * writers)
        {
            return fail("ordered write-only lowering retained the pairwise conflict network");
        }
        std::string priorityGroup;
        std::unordered_set<int64_t> priorities;
        for (OperationId writeOpId : writes)
        {
            const Operation write = graph.getOperation(writeOpId);
            const auto group = getAttr<std::string>(write, kMemoryWritePriorityGroupAttr);
            const auto priority = getAttr<int64_t>(write, kMemoryWritePriorityAttr);
            if (!group || group->empty() || !priority || *priority < 0 ||
                static_cast<std::size_t>(*priority) >= writers)
            {
                return fail("ordered write-only lowering emitted invalid priority attrs");
            }
            const auto expectedPriority = expectedPriorityByAddress.find(write.operands()[1]);
            if (expectedPriority == expectedPriorityByAddress.end() ||
                expectedPriority->second != *priority)
            {
                return fail("ordered write-only lowering did not preserve semantic priority rank");
            }
            expectedPriorityByAddress.erase(expectedPriority);
            if (priorityGroup.empty())
            {
                priorityGroup = *group;
            }
            else if (priorityGroup != *group)
            {
                return fail("ordered write-only lowering split one priority group");
            }
            priorities.insert(*priority);
        }
        if (priorities.size() != writers || !expectedPriorityByAddress.empty())
        {
            return fail("ordered write-only lowering emitted duplicate priorities");
        }
        return 0;
    }

    int testVerilatorProfitabilityRejectsHighConflictStorage()
    {
        Design design = buildWriteOnlyDecodedStorageDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runVerilatorProfitabilityPass(design); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 0 ||
            countKind(graph, OperationKind::kRegister) != 4 ||
            countKind(graph, OperationKind::kRegisterWritePort) != 4 ||
            countKind(graph, OperationKind::kRegisterReadPort) != 4)
        {
            return fail("Verilator profitability filter changed rejected scalar storage");
        }
        for (OperationId regOpId : opsOfKind(graph, OperationKind::kRegister))
        {
            if (hasIntentGroup(graph, regOpId))
            {
                return fail("Verilator profitability rejection leaked intent annotation");
            }
        }
        return 0;
    }

    int testVerilatorProfitabilityKeepsBoundaryAndLowConflictStorage()
    {
        // 65 writers have 2080 ordered pair conflicts; 32 rows make P == D * W.
        Design boundary = buildWriteOnlyDecodedStorageDesign(65, 32);
        Graph &boundaryGraph = *boundary.findGraph("top");
        if (const int rc = runVerilatorProfitabilityPass(boundary); rc != 0)
        {
            return rc;
        }
        if (countKind(boundaryGraph, OperationKind::kMemory) != 1 ||
            countKind(boundaryGraph, OperationKind::kRegister) != 0)
        {
            return fail("Verilator profitability filter rejected the P == D * W boundary");
        }

        Design lowConflict = buildTrueStorageOnlyDesign(true);
        Graph &lowConflictGraph = *lowConflict.findGraph("top");
        if (const int rc = runVerilatorProfitabilityPass(lowConflict); rc != 0)
        {
            return rc;
        }
        if (countKind(lowConflictGraph, OperationKind::kMemory) != 1)
        {
            return fail("Verilator profitability filter rejected conflict-free storage");
        }
        return 0;
    }

    int testVerilatorProfitabilityKeepsOrderedWrites()
    {
        Design design = buildWriteOnlyDecodedStorageDesign();
        Graph &graph = *design.findGraph("top");
        if (const int rc = runVerilatorProfitabilityPass(design, true); rc != 0)
        {
            return rc;
        }
        if (countKind(graph, OperationKind::kMemory) != 1 ||
            countKind(graph, OperationKind::kMemoryWritePort) != 65)
        {
            return fail("Verilator profitability filter rejected ordered-write lowering");
        }
        return 0;
    }

    int testVerilatorProfitabilityOptionParsing()
    {
        const std::array<std::string_view, 2> enabled{
            "-profitability-filter", "verilator-v1"};
        const std::array<std::string_view, 2> disabled{
            "-profitability-filter", "none"};
        const std::array<std::string_view, 2> invalid{
            "-profitability-filter", "unknown"};
        const std::array<std::string_view, 1> missing{"-profitability-filter"};
        std::string error;
        if (!makePass("reg-to-mem", enabled, error) ||
            !makePass("reg-to-mem", disabled, error) ||
            !makePass("reg-to-mem", {}, error))
        {
            return fail("valid profitability option was not accepted");
        }
        error.clear();
        if (makePass("reg-to-mem", invalid, error) || error.empty())
        {
            return fail("unknown profitability policy was accepted");
        }
        error.clear();
        if (makePass("reg-to-mem", missing, error) || error.empty())
        {
            return fail("missing profitability policy was accepted");
        }
        return 0;
    }

} // namespace

int main()
{
    try
    {
        if (const int rc = testSliceDynamicIntent(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testSliceDynamicWidthOneBareIndexIntent(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSharedConcat(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsReadWithExtraUser(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentKeepsStableFamilyForConflictingRowOrder(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentAllowsOverlappingSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentAllowsMiddleSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSharedReadSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentRejectsSiblingSharedReadSubsetView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentKeepsStableFamilyForPartialOverlap(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testIntentDoesNotDropCompatibleFamilyBecauseOfBridgeConflict(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeSliceArray(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeReduceAndLastRowGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeMultiAnchor(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeMultipleWriteFamilies(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeConsolidatedWriteFamiliesWithReset(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeConsolidatedWriteFamiliesWithMultipleResetTerms(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeConsolidatedFallbackDataWriteFamily(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeStorageOnlySharedReads(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeStorageOnlySharedPackedView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeStorageOnlyEdgePaddedPackedView(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeStorageOnlyNonPowerOfTwoDomainGuard(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeStorageOnlyFailureDoesNotAnnotateIntent(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeResetFill(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeCompoundResetFill(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeCompoundResetWithTrailingNoOpMux(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testBranchNotInUpdateOuterResetRemainsScalar(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeCombLanePackedMuxWrites(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeCombLanePackedCompoundPriorityWrites(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeOrDecodedPriorityWrites(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeWriteOnlyDecodedStorageWithRowOffset(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeWriteOnlyDecodedStorageRejectsSmallFamily(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeWriteOnlyDecodedStorageCanBeDisabled(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testTrueMergeWriteOnlyDecodedStorageOrderedWrites(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testVerilatorProfitabilityRejectsHighConflictStorage(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testVerilatorProfitabilityKeepsBoundaryAndLowConflictStorage(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testVerilatorProfitabilityKeepsOrderedWrites(); rc != 0)
        {
            return rc;
        }
        if (const int rc = testVerilatorProfitabilityOptionParsing(); rc != 0)
        {
            return rc;
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
    return 0;
}
