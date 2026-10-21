#include "core/grh.hpp"
#include "transform/comb_loop_elim.hpp"
#include "core/transform.hpp"
#include "transform/verify.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[comb-loop-elim-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        return graph.createValue(sym, width, false);
    }

    wolvrix::lib::grh::OperationId makeSliceStatic(wolvrix::lib::grh::Graph &graph,
                                                   wolvrix::lib::grh::ValueId base,
                                                   wolvrix::lib::grh::ValueId result,
                                                   int64_t low,
                                                   int64_t high,
                                                   const std::string &name)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, sym);
        graph.addOperand(op, base);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceStart", low);
        graph.setAttr(op, "sliceEnd", high);
        return op;
    }

    wolvrix::lib::grh::OperationId makeConcat(wolvrix::lib::grh::Graph &graph,
                                              const std::string &name,
                                              const std::vector<wolvrix::lib::grh::ValueId> &operands,
                                              wolvrix::lib::grh::ValueId result)
    {
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat, sym);
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.addResult(op, result);
        return op;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width,
                                         const std::string &literal)
    {
        const auto value = makeValue(graph, name, width);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.internSymbol(name + "_op"));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", literal);
        return value;
    }

    wolvrix::lib::grh::OperationId makeSliceDynamic(wolvrix::lib::grh::Graph &graph,
                                                    wolvrix::lib::grh::ValueId base,
                                                    wolvrix::lib::grh::ValueId index,
                                                    wolvrix::lib::grh::ValueId result,
                                                    int64_t width,
                                                    const std::string &name)
    {
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                              graph.internSymbol(name));
        graph.addOperand(op, base);
        graph.addOperand(op, index);
        graph.addResult(op, result);
        graph.setAttr(op, "sliceWidth", width);
        return op;
    }

    int test_false_loop_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_false");

        auto a_high = makeValue(graph, "a_high", 4);
        auto b_high = makeValue(graph, "b_high", 4);

        auto a_low_from_b = makeValue(graph, "a_low_from_b", 4);
        auto a = makeValue(graph, "a", 8);
        auto b_low_from_a = makeValue(graph, "b_low_from_a", 4);
        auto b = makeValue(graph, "b", 8);

        makeSliceStatic(graph, b, a_low_from_b, 0, 3, "slice_b_low");
        makeConcat(graph, "concat_a", {a_high, a_low_from_b}, a);

        makeSliceStatic(graph, a, b_low_from_a, 4, 7, "slice_a_high");
        makeConcat(graph, "concat_b", {b_high, b_low_from_a}, b);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected comb-loop-elim to report changes for false loop");
        }
        bool foundResolvedInfo = false;
        bool foundSummaryInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected fixed false loop to avoid warning diagnostics");
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim resolved false loops") != std::string::npos)
            {
                foundResolvedInfo = true;
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim summary") != std::string::npos)
            {
                foundSummaryInfo = true;
            }
        }
        if (!foundResolvedInfo)
        {
            return fail("Expected fixed false loop to emit an info diagnostic");
        }
        if (!foundSummaryInfo)
        {
            return fail("Expected fixed false loop to emit an info summary diagnostic");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed during verify run");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after split");
            }
        }

        return 0;
    }

    int test_true_loop_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_true");

        auto a = makeValue(graph, "a", 1);
        auto x = makeValue(graph, "x", 1);
        auto y = makeValue(graph, "y", 1);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_xy"));
        graph.addOperand(xorOp, a);
        graph.addOperand(xorOp, y);
        graph.addResult(xorOp, x);

        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not_x"));
        graph.addOperand(notOp, x);
        graph.addResult(notOp, y);

        CombLoopElimOptions options;
        options.fixFalseLoops = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected comb-loop-elim to succeed with warnings");
        }
        if (res.changed)
        {
            return fail("Expected comb-loop-elim to avoid changes for true loop");
        }

        bool foundWarning = false;
        bool foundSummaryInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("comb-loop-elim summary") != std::string::npos)
            {
                foundSummaryInfo = true;
            }
        }
        if (!foundWarning)
        {
            return fail("Expected comb-loop-elim warning for true loop");
        }
        if (!foundSummaryInfo)
        {
            return fail("Expected comb-loop-elim info summary for true loop");
        }
        return 0;
    }

    int test_false_loop_dynamic_slice_nested_offset()
    {
        using namespace wolvrix::lib::grh;
        for (const int64_t index : {0, 11})
        {
            Design design;
            Graph &graph = design.createGraph("g_dyn_nested_offset");
            const auto input = makeValue(graph, "input", 20);
            const auto output = makeValue(graph, "output", 1);
            const auto bus = makeValue(graph, "bus", 21);
            const auto lane = makeValue(graph, "lane", 12);
            graph.bindInputPort("input", input);
            graph.bindOutputPort("output", output);

            // bus = {input, output}; lane = bus[20:9] = input[19:8].
            // The apparent cycle vanishes when the slices select input.
            makeConcat(graph, "bus_concat", {input, output}, bus);
            makeSliceStatic(graph, bus, lane, 9, 20, "lane_slice");
            const auto selector = makeConst(graph, "selector", 4, "4'd" + std::to_string(index));
            const auto select = makeSliceDynamic(graph, lane, selector, output, 1, "select");

            PassManager manager;
            manager.addPass(std::make_unique<CombLoopElimPass>());
            manager.addPass(std::make_unique<GrhVerifyPass>());
            PassDiagnostics diagnostics;
            const auto result = manager.run(design, diagnostics);
            if (!result.success || diagnostics.hasError())
                return fail("Nested constant-index slice should eliminate the false loop");

            const auto rewritten = graph.getOperation(select);
            if (rewritten.kind() != OperationKind::kSliceStatic ||
                rewritten.operands().size() != 1 || rewritten.operands()[0] != input)
                return fail("Retargeted constant-index slice must select the input with a static offset");
            const auto low = rewritten.attr("sliceStart");
            const auto high = rewritten.attr("sliceEnd");
            if (!low || !high || std::get<int64_t>(*low) != index + 8 ||
                std::get<int64_t>(*high) != index + 8 || rewritten.attr("sliceWidth"))
                return fail("Retargeted nested slice lost its input[19:8] offset");
        }
        return 0;
    }

    int test_false_loop_dynamic_slice_retargeted()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_dyn_false");

        const auto ext = makeValue(graph, "ext", 1);
        graph.bindInputPort("ext", ext);

        const auto low = makeValue(graph, "low", 1);
        const auto hi = makeValue(graph, "hi", 1);
        const auto busView = makeValue(graph, "bus_view", 2);
        const auto bus = makeValue(graph, "bus", 2);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");

        const auto notOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                 graph.internSymbol("not_low"));
        graph.addOperand(notOp, low);
        graph.addResult(notOp, hi);

        makeConcat(graph, "concat_bus_view", {hi, ext}, busView);
        const auto assignBus = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_bus"));
        graph.addOperand(assignBus, busView);
        graph.addResult(assignBus, bus);

        const auto sliceOp = makeSliceDynamic(graph, bus, idx0, low, 1, "slice_low");

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected dynamic false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected dynamic false-loop case to report changes");
        }

        const auto rewritten = graph.getOperation(sliceOp);
        if (rewritten.kind() != wolvrix::lib::grh::OperationKind::kAssign)
        {
            return fail("Expected dynamic slice to retarget into a direct assign");
        }
        if (rewritten.operands().size() != 1 || rewritten.operands()[0] != ext)
        {
            return fail("Expected retargeted assign to use the selected concat operand");
        }
        if (rewritten.attr("sliceWidth") || rewritten.attr("sliceStart") || rewritten.attr("sliceEnd"))
        {
            return fail("Expected retargeted assign to drop slice attributes");
        }

        bool foundRetargetInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim" || msg.kind != PassDiagnosticKind::Info)
            {
                continue;
            }
            if (msg.message.find("retargeted-slices=") != std::string::npos)
            {
                foundRetargetInfo = true;
            }
            if (msg.message.find("resolved false loops") != std::string::npos &&
                msg.message.find("split-values=0") == std::string::npos)
            {
                return fail("Expected dynamic retarget fast path to avoid generic value splitting");
            }
        }
        if (!foundRetargetInfo)
        {
            return fail("Expected dynamic false-loop case to report retargeted slices");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dynamic verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected dynamic verify run to succeed");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after dynamic slice retarget");
            }
        }

        return 0;
    }

    int test_false_loop_bitwise_partition_propagation()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_bitwise_false");

        const auto mask = makeValue(graph, "mask", 6);
        const auto ext = makeValue(graph, "ext", 4);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 6);
        const auto x = makeValue(graph, "x", 6);
        const auto mid = makeValue(graph, "mid", 2);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_y_mask"));
        graph.addOperand(xorOp, y);
        graph.addOperand(xorOp, mask);
        graph.addResult(xorOp, x);

        makeSliceStatic(graph, x, mid, 1, 2, "slice_x_mid");
        makeConcat(graph, "concat_y", {mid, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during bitwise false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected bitwise false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected bitwise false-loop case to report changes");
        }

        bool foundSplitInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected bitwise false-loop case to avoid warning diagnostics");
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("resolved false loops") != std::string::npos &&
                msg.message.find("split-values=0") == std::string::npos)
            {
                foundSplitInfo = true;
            }
        }
        if (!foundSplitInfo)
        {
            return fail("Expected bitwise false-loop case to use value splitting");
        }

        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));

        PassDiagnostics verifyDiags;
        PassManagerResult verifyRes{};
        try
        {
            verifyRes = verifyManager.run(design, verifyDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during bitwise verify run: ") + ex.what());
        }
        if (!verifyRes.success || verifyDiags.hasError())
        {
            return fail("Expected bitwise verify run to succeed");
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail("Expected no comb-loop warnings after bitwise split");
            }
        }

        return 0;
    }

    int test_false_loop_compare_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_compare_false");

        const auto mask = makeValue(graph, "mask", 4);
        const auto ext = makeValue(graph, "ext", 3);
        const auto rhs = makeConst(graph, "rhs", 3, "3'h3");
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto x = makeValue(graph, "x", 4);
        const auto low = makeValue(graph, "low", 3);
        const auto bit = makeValue(graph, "bit", 1);

        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor_y_mask"));
        graph.addOperand(xorOp, y);
        graph.addOperand(xorOp, mask);
        graph.addResult(xorOp, x);

        makeSliceStatic(graph, x, low, 0, 2, "slice_x_low");

        const auto eqOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kEq,
                                                graph.internSymbol("eq_x_rhs"));
        graph.addOperand(eqOp, low);
        graph.addOperand(eqOp, rhs);
        graph.addResult(eqOp, bit);

        makeConcat(graph, "concat_y", {bit, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during compare false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected compare false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected compare false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected compare false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

    int test_false_loop_replicate_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_replicate_false");

        const auto ext = makeValue(graph, "ext", 2);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto low = makeValue(graph, "low", 1);
        const auto repeated = makeValue(graph, "repeated", 2);

        makeSliceStatic(graph, y, low, 0, 0, "slice_y_low");

        const auto repOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kReplicate,
                                                 graph.internSymbol("rep_low"));
        graph.addOperand(repOp, low);
        graph.addResult(repOp, repeated);
        graph.setAttr(repOp, "rep", int64_t{2});

        makeConcat(graph, "concat_y", {repeated, ext}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during replicate false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected replicate false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected replicate false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected replicate false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

    int test_false_loop_reduce_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_reduce_false");

        const auto ext = makeValue(graph, "ext", 2);
        graph.bindInputPort("ext", ext);

        const auto y = makeValue(graph, "y", 4);
        const auto low = makeValue(graph, "low", 1);
        const auto high = makeValue(graph, "high", 1);
        const auto pair = makeValue(graph, "pair", 2);
        const auto bit = makeValue(graph, "bit", 1);

        makeSliceStatic(graph, y, low, 0, 0, "slice_y_low");
        makeSliceStatic(graph, y, high, 1, 1, "slice_y_high");
        makeConcat(graph, "concat_pair", {high, low}, pair);

        const auto reduceOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kReduceAnd,
                                                    graph.internSymbol("reduce_pair"));
        graph.addOperand(reduceOp, pair);
        graph.addResult(reduceOp, bit);

        makeConcat(graph, "concat_y", {bit, ext, high}, y);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during reduce false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected reduce false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected reduce false-loop case to report changes");
        }
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected reduce false-loop case to avoid warning diagnostics");
            }
        }
        return 0;
    }

    wolvrix::lib::grh::ValueId makeArrayValue(wolvrix::lib::grh::Graph &graph,
                                              const std::string &name,
                                              int32_t elementWidth,
                                              std::vector<int64_t> shape)
    {
        int64_t total = elementWidth;
        for (const int64_t dim : shape)
        {
            total *= dim;
        }
        const wolvrix::lib::grh::SymbolId sym = graph.internSymbol(name);
        return graph.createValue(sym, static_cast<int32_t>(total), false,
                                 wolvrix::lib::grh::ValueType::Logic, shape);
    }

    wolvrix::lib::grh::OperationId makeSliceArray(wolvrix::lib::grh::Graph &graph,
                                                  wolvrix::lib::grh::ValueId base,
                                                  const std::vector<wolvrix::lib::grh::ValueId> &indices,
                                                  wolvrix::lib::grh::ValueId result,
                                                  const std::string &name)
    {
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceArray,
                                              graph.internSymbol(name));
        graph.addOperand(op, base);
        for (const auto index : indices)
        {
            graph.addOperand(op, index);
        }
        graph.addResult(op, result);
        return op;
    }

    wolvrix::lib::grh::OperationId makeUnaryOp(wolvrix::lib::grh::Graph &graph,
                                               wolvrix::lib::grh::OperationKind kind,
                                               wolvrix::lib::grh::ValueId operand,
                                               wolvrix::lib::grh::ValueId result,
                                               const std::string &name)
    {
        const auto op = graph.createOperation(kind, graph.internSymbol(name));
        graph.addOperand(op, operand);
        graph.addResult(op, result);
        return op;
    }

    wolvrix::lib::grh::OperationId makeBinaryOp(wolvrix::lib::grh::Graph &graph,
                                                wolvrix::lib::grh::OperationKind kind,
                                                wolvrix::lib::grh::ValueId lhs,
                                                wolvrix::lib::grh::ValueId rhs,
                                                wolvrix::lib::grh::ValueId result,
                                                const std::string &name)
    {
        const auto op = graph.createOperation(kind, graph.internSymbol(name));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, result);
        return op;
    }

    // Shared checker: a second pass run with fixing disabled must observe no
    // remaining loop, and the graph must stay verifier-clean.
    int verifyLoopFreeAndLegal(wolvrix::lib::grh::Design &design, const std::string &caseName)
    {
        CombLoopElimOptions verifyOptions;
        verifyOptions.fixFalseLoops = false;
        PassManager verifyManager;
        verifyManager.addPass(std::make_unique<CombLoopElimPass>(verifyOptions));
        PassDiagnostics verifyDiags;
        try
        {
            const PassManagerResult verifyRes = verifyManager.run(design, verifyDiags);
            if (!verifyRes.success || verifyDiags.hasError())
            {
                return fail(caseName + ": verify run failed");
            }
        }
        catch (const std::exception &ex)
        {
            return fail(caseName + ": exception during verify run: " + ex.what());
        }
        for (const auto &msg : verifyDiags.messages())
        {
            if (msg.passName == "comb-loop-elim" &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                return fail(caseName + ": expected no comb-loop warnings after split");
            }
        }

        PassManager grhVerifyManager;
        grhVerifyManager.addPass(std::make_unique<wolvrix::lib::transform::GrhVerifyPass>());
        PassDiagnostics grhVerifyDiags;
        try
        {
            const PassManagerResult verifyRes = grhVerifyManager.run(design, grhVerifyDiags);
            if (!verifyRes.success || grhVerifyDiags.hasError())
            {
                return fail(caseName + ": grh.verify failed after comb-loop-elim");
            }
        }
        catch (const std::exception &ex)
        {
            return fail(caseName + ": exception during grh.verify: " + ex.what());
        }
        return 0;
    }

    // stage-05: arrays whose value-level SCC is bit-disjoint at element
    // granularity are false loops; constant kSliceArray indices participate
    // in the exact bit-range analysis.
    //
    //   r1    = b[1]            (kSliceArray const)
    //   xv    = r1 ^ in0        (a[0])
    //   a     = {a1in, xv}      (concat + assign, shape [2])
    //   r2    = a[0]            (kSliceArray const)
    //   yv    = r2 | in0        (b[0])
    //   b     = {b1in, yv}      (shape [2])
    //
    // Value cycle b -> r1 -> xv -> a -> r2 -> yv -> b, but element-wise
    // b[1] -> a[0] -> b[0] is acyclic (a[1]/b[1] come from outside).
    int test_array_false_loop_const_index_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_const_false");

        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto aflat = makeValue(graph, "aflat", 16);
        const auto r2 = makeValue(graph, "r2", 8);
        const auto yv = makeValue(graph, "yv", 8);
        const auto bflat = makeValue(graph, "bflat", 16);

        makeSliceArray(graph, b, {idx1}, r1, "read_b1");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, aflat, a, "assign_a");
        makeSliceArray(graph, a, {idx0}, r2, "read_a0");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kOr, r2, in0, yv, "or_r2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, bflat, b, "assign_b");

        // External consumers keep using the array values (exercise the
        // shape-carrying reassembly view).
        const auto e0 = makeValue(graph, "e0", 8);
        const auto e1 = makeValue(graph, "e1", 8);
        const auto outv = makeValue(graph, "outv", 8);
        makeSliceArray(graph, a, {idx0}, e0, "ext_read_a0");
        makeSliceArray(graph, b, {idx1}, e1, "ext_read_b1");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, e0, e1, outv, "xor_out");
        graph.bindOutputPort("out", outv);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during array const-index false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected array const-index false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected array const-index false-loop case to report changes");
        }
        bool foundResolvedInfo = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected array const-index false-loop case to avoid warnings: " + msg.message);
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("resolved false loops") != std::string::npos)
            {
                foundResolvedInfo = true;
            }
        }
        if (!foundResolvedInfo)
        {
            return fail("Expected array const-index false loop to be resolved");
        }
        return verifyLoopFreeAndLegal(design, "array-const-false");
    }

    int test_array_false_loop_const_index_retargeted()
    {
        using wolvrix::lib::grh::OperationKind;

        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_const_retarget");
        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto aflat = makeArrayValue(graph, "aflat", 8, {2});
        const auto bflat = makeArrayValue(graph, "bflat", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto r2 = makeValue(graph, "r2", 8);
        const auto yv = makeValue(graph, "yv", 8);

        const auto readB = makeSliceArray(graph, b, {idx1}, r1, "read_b1");
        makeBinaryOp(graph, OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, OperationKind::kAssignAs, aflat, a, "assign_a");
        const auto readA = makeSliceArray(graph, a, {idx0}, r2, "read_a0");
        makeBinaryOp(graph, OperationKind::kOr, r2, in0, yv, "or_r2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, OperationKind::kAssignAs, bflat, b, "assign_b");
        graph.bindOutputPort("out", yv);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected shaped const-index false loop to be retargeted");
        }
        if (graph.opKind(readB) != OperationKind::kAssign ||
            graph.opKind(readA) != OperationKind::kAssign ||
            graph.opOperands(readB)[0] != b1in ||
            graph.opOperands(readA)[0] != xv)
        {
            return fail("Expected array reads to select their concat operands");
        }
        return verifyLoopFreeAndLegal(design, "array-const-retarget");
    }

    int test_array_false_loop_selected_operand_feedback()
    {
        using wolvrix::lib::grh::OperationKind;

        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_selected_feedback");
        const auto inHigh = makeValue(graph, "in_high", 4);
        const auto inLow = makeValue(graph, "in_low", 8);
        graph.bindInputPort("in_high", inHigh);
        graph.bindInputPort("in_low", inLow);
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");
        const auto base = makeArrayValue(graph, "base", 8, {2});
        const auto selected = makeValue(graph, "selected", 8);
        const auto readResult = makeValue(graph, "read_result", 8);
        const auto feedback = makeValue(graph, "feedback", 4);

        const auto read = makeSliceArray(graph, base, {idx1}, readResult, "read1");
        makeSliceStatic(graph, readResult, feedback, 4, 7, "feedback_high");
        makeConcat(graph, "selected_concat", {inHigh, feedback}, selected);
        makeConcat(graph, "array_concat", {selected, inLow}, base);
        graph.bindOutputPort("out", readResult);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || !res.changed)
        {
            return fail("Expected selected-operand feedback false loop to be resolved");
        }
        if (graph.opKind(read) != OperationKind::kAssign ||
            graph.opOperands(read)[0] != selected)
        {
            return fail("Expected array selection to reach its concat operand");
        }
        return verifyLoopFreeAndLegal(design, "array-selected-feedback");
    }

    int test_array_false_loop_flat_alias_fallback()
    {
        using wolvrix::lib::grh::OperationKind;

        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_flat_alias");
        const auto inHigh = makeValue(graph, "in_high", 4);
        const auto inLow = makeValue(graph, "in_low", 8);
        graph.bindInputPort("in_high", inHigh);
        graph.bindInputPort("in_low", inLow);
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");
        const auto base = makeArrayValue(graph, "base", 8, {2});
        const auto flat = makeValue(graph, "flat", 16);
        const auto selected = makeValue(graph, "selected", 8);
        const auto readResult = makeValue(graph, "read_result", 8);
        const auto feedback = makeValue(graph, "feedback", 4);

        const auto read = makeSliceArray(graph, base, {idx1}, readResult, "read1");
        makeSliceStatic(graph, readResult, feedback, 4, 7, "feedback_high");
        makeConcat(graph, "selected_concat", {inHigh, feedback}, selected);
        makeConcat(graph, "array_concat", {selected, inLow}, flat);
        makeUnaryOp(graph, OperationKind::kAssignAs, flat, base, "array_alias");
        graph.bindOutputPort("out", readResult);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());
        PassDiagnostics diags;
        const PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError() || !res.changed ||
            graph.opKind(read) == OperationKind::kSliceArray)
        {
            return fail("Expected flat array alias false loop to select a static range");
        }
        return verifyLoopFreeAndLegal(design, "array-flat-alias");
    }

    // stage-05: a dynamic-index kSliceArray inside a loop keeps its result as
    // one whole fragment and reads the reassembled base view; when no fragment
    // of the base depends on the read result, the loop is fixed.
    //
    //   r1 = b[idx]   (dynamic)
    //   xv = r1 ^ in0           (a[0])
    //   a  = {a1in, xv}
    //   r2 = a[1]             (const kSliceArray, = a1in, outside the cycle)
    //   yv = r2 | in0           (b[0])
    //   b  = {b1in, yv}
    //
    // The back-edge uses a constant kSliceArray (not kSliceStatic) so the
    // slice-retarget fast path cannot dissolve the loop and the generic split
    // path is exercised.
    int test_array_false_loop_dynamic_index_fixed()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_dyn_false");

        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        const auto idx = makeValue(graph, "idx", 1);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        graph.bindInputPort("idx", idx);
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto aflat = makeValue(graph, "aflat", 16);
        const auto s2 = makeValue(graph, "s2", 8);
        const auto yv = makeValue(graph, "yv", 8);
        const auto bflat = makeValue(graph, "bflat", 16);

        makeSliceArray(graph, b, {idx}, r1, "read_b_dyn");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, aflat, a, "assign_a");
        makeSliceArray(graph, a, {idx1}, s2, "read_a1");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kOr, s2, in0, yv, "or_s2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, bflat, b, "assign_b");

        const auto e0 = makeValue(graph, "e0", 8);
        const auto outv = makeValue(graph, "outv", 8);
        makeSliceArray(graph, a, {idx}, e0, "ext_read_a_dyn");
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssign, e0, outv, "assign_out");
        graph.bindOutputPort("out", outv);

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during array dynamic-index false-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected array dynamic-index false-loop case to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected array dynamic-index false-loop case to report changes");
        }
        bool foundResolvedInfo = false;
        bool foundDynStat = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim")
            {
                continue;
            }
            if (msg.kind == PassDiagnosticKind::Warning)
            {
                return fail("Expected array dynamic-index false-loop case to avoid warnings: " + msg.message);
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("resolved false loops") != std::string::npos)
            {
                foundResolvedInfo = true;
            }
            if (msg.kind == PassDiagnosticKind::Info &&
                msg.message.find("dyn-array=1") != std::string::npos)
            {
                foundDynStat = true;
            }
        }
        if (!foundResolvedInfo)
        {
            return fail("Expected array dynamic-index false loop to be resolved");
        }
        if (!foundDynStat)
        {
            return fail("Expected dyn-array=1 in the comb-loop-elim summary");
        }
        return verifyLoopFreeAndLegal(design, "array-dyn-false");
    }

    // stage-05: a dynamic read whose result feeds back into the base array is
    // a real (undecidable) loop: reported with the dynamic-array-read note and
    // never silently eliminated.
    //
    //   r1 = b[idx]   (dynamic)
    //   xv = r1 ^ in0           (a[0])
    //   a  = {a1in, xv}
    //   s2 = a[7:0]             (= xv, real feedback when idx == 0)
    //   yv = s2 | in0           (b[0])
    //   b  = {b1in, yv}
    int test_array_true_loop_dynamic_index_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_dyn_true");

        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        const auto idx = makeValue(graph, "idx", 1);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        graph.bindInputPort("idx", idx);

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto aflat = makeValue(graph, "aflat", 16);
        const auto s2 = makeValue(graph, "s2", 8);
        const auto yv = makeValue(graph, "yv", 8);
        const auto bflat = makeValue(graph, "bflat", 16);

        makeSliceArray(graph, b, {idx}, r1, "read_b_dyn");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, aflat, a, "assign_a");
        makeSliceStatic(graph, a, s2, 0, 7, "read_a0");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kOr, s2, in0, yv, "or_s2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, bflat, b, "assign_b");

        const auto e0 = makeValue(graph, "e0", 8);
        makeSliceArray(graph, a, {idx}, e0, "ext_read_a_dyn");

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during array dynamic true-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected array dynamic true-loop case to succeed (warning expected)");
        }
        bool foundWarning = false;
        bool foundDynNote = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName != "comb-loop-elim" || msg.kind != PassDiagnosticKind::Warning)
            {
                continue;
            }
            if (msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
                if (msg.message.find("dynamic-array-read") != std::string::npos)
                {
                    foundDynNote = true;
                }
            }
        }
        if (!foundWarning)
        {
            return fail("Expected a comb-loop warning for the dynamic true loop");
        }
        if (!foundDynNote)
        {
            return fail("Expected the dynamic-array-read note in the true-loop warning");
        }

        // The graph must stay verifier-clean even though a warning remains.
        PassManager grhVerifyManager;
        grhVerifyManager.addPass(std::make_unique<wolvrix::lib::transform::GrhVerifyPass>());
        PassDiagnostics verifyDiags;
        try
        {
            const PassManagerResult verifyRes = grhVerifyManager.run(design, verifyDiags);
            if (!verifyRes.success || verifyDiags.hasError())
            {
                return fail("grh.verify failed after the dynamic true-loop run");
            }
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during grh.verify: ") + ex.what());
        }
        return 0;
    }

    // Loops whose fragmentation would exceed maxFragmentsPerValue are left
    // intact and reported unresolved.
    int test_fragment_cap_blocks_split()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_fragment_cap");
        const auto mask = makeValue(graph, "mask", 6);
        const auto ext = makeValue(graph, "ext", 4);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("ext", ext);
        const auto y = makeValue(graph, "y", 6);
        const auto x = makeValue(graph, "x", 6);
        const auto mid = makeValue(graph, "mid", 2);
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, y, mask, x, "xor_y_mask");
        makeSliceStatic(graph, x, mid, 1, 2, "slice_x_mid");
        makeConcat(graph, "concat_y", {mid, ext}, y);

        CombLoopElimOptions options;
        options.maxFragmentsPerValue = 1;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during fragment-cap run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected fragment-cap case to succeed (warning expected)");
        }
        if (res.changed)
        {
            return fail("Expected the capped split to leave the graph unchanged");
        }
        bool foundWarning = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
            }
        }
        if (!foundWarning)
        {
            return fail("Expected the capped loop to be reported as unresolved");
        }
        return 0;
    }

    // stage-05: with enableArrayFragmentation=false the pass keeps the legacy
    // behavior (kSliceArray aborts the bit-range analysis as incomplete).
    int test_array_fragmentation_disabled()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_off");

        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");
        const auto idx1 = makeConst(graph, "idx1", 32, "32'd1");

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto aflat = makeValue(graph, "aflat", 16);
        const auto r2 = makeValue(graph, "r2", 8);
        const auto yv = makeValue(graph, "yv", 8);
        const auto bflat = makeValue(graph, "bflat", 16);

        makeSliceArray(graph, b, {idx1}, r1, "read_b1");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, aflat, a, "assign_a");
        makeSliceArray(graph, a, {idx0}, r2, "read_a0");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kOr, r2, in0, yv, "or_r2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, bflat, b, "assign_b");

        CombLoopElimOptions options;
        options.enableArrayFragmentation = false;
        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>(options));

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during disabled-fragmentation run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected disabled-fragmentation case to succeed (warning expected)");
        }
        if (res.changed)
        {
            return fail("Expected no changes when array fragmentation is disabled");
        }
        bool foundIncomplete = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning &&
                msg.message.find("analysis=incomplete") != std::string::npos)
            {
                foundIncomplete = true;
            }
        }
        if (!foundIncomplete)
        {
            return fail("Expected the legacy analysis=incomplete warning when disabled");
        }
        return 0;
    }

    // stage-05: a real element-level cycle through constant indices (a[0] and
    // b[0] genuinely feed each other) must remain reported; splitting is
    // equivalence-preserving and the residual loop re-appears as a warning.
    int test_array_true_loop_const_index_reported()
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g_array_const_true");

        const auto in0 = makeValue(graph, "in0", 8);
        const auto a1in = makeValue(graph, "a1in", 8);
        const auto b1in = makeValue(graph, "b1in", 8);
        graph.bindInputPort("in0", in0);
        graph.bindInputPort("a1in", a1in);
        graph.bindInputPort("b1in", b1in);
        const auto idx0 = makeConst(graph, "idx0", 32, "32'd0");

        const auto a = makeArrayValue(graph, "a", 8, {2});
        const auto b = makeArrayValue(graph, "b", 8, {2});
        const auto r1 = makeValue(graph, "r1", 8);
        const auto xv = makeValue(graph, "xv", 8);
        const auto aflat = makeValue(graph, "aflat", 16);
        const auto r2 = makeValue(graph, "r2", 8);
        const auto yv = makeValue(graph, "yv", 8);
        const auto bflat = makeValue(graph, "bflat", 16);

        makeSliceArray(graph, b, {idx0}, r1, "read_b0");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kXor, r1, in0, xv, "xor_r1");
        makeConcat(graph, "concat_a", {a1in, xv}, aflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, aflat, a, "assign_a");
        makeSliceArray(graph, a, {idx0}, r2, "read_a0");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kOr, r2, in0, yv, "or_r2");
        makeConcat(graph, "concat_b", {b1in, yv}, bflat);
        makeUnaryOp(graph, wolvrix::lib::grh::OperationKind::kAssignAs, bflat, b, "assign_b");

        PassManager manager;
        manager.addPass(std::make_unique<CombLoopElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during array const-index true-loop run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected array const-index true-loop case to succeed (warning expected)");
        }
        bool foundWarning = false;
        for (const auto &msg : diags.messages())
        {
            if (msg.passName == "comb-loop-elim" && msg.kind == PassDiagnosticKind::Warning &&
                msg.message.find("comb loop detected") != std::string::npos)
            {
                foundWarning = true;
            }
        }
        if (!foundWarning)
        {
            return fail("Expected a comb-loop warning for the const-index true loop");
        }

        PassManager grhVerifyManager;
        grhVerifyManager.addPass(std::make_unique<wolvrix::lib::transform::GrhVerifyPass>());
        PassDiagnostics verifyDiags;
        try
        {
            const PassManagerResult verifyRes = grhVerifyManager.run(design, verifyDiags);
            if (!verifyRes.success || verifyDiags.hasError())
            {
                return fail("grh.verify failed after the const-index true-loop run");
            }
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during grh.verify: ") + ex.what());
        }
        return 0;
    }

} // namespace

int main()
{
    if (int rc = test_false_loop_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_true_loop_reported(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_dynamic_slice_nested_offset(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_dynamic_slice_retargeted(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_bitwise_partition_propagation(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_compare_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_replicate_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_false_loop_reduce_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_false_loop_const_index_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_false_loop_const_index_retargeted(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_false_loop_selected_operand_feedback(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_false_loop_flat_alias_fallback(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_false_loop_dynamic_index_fixed(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_true_loop_dynamic_index_reported(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_fragment_cap_blocks_split(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_fragmentation_disabled(); rc != 0)
    {
        return rc;
    }
    if (int rc = test_array_true_loop_const_index_reported(); rc != 0)
    {
        return rc;
    }
    return 0;
}
