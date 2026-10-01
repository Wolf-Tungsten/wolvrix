#include "core/grh.hpp"
#include "transform/simplify.hpp"
#include "core/transform.hpp"

#include "slang/numeric/SVInt.h"

#include <iostream>
#include <optional>
#include <string>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[simplify-tests] " << message << '\n';
        return 1;
    }

    wolvrix::lib::grh::ValueId makeConst(wolvrix::lib::grh::Graph &graph,
                                         const std::string &valueName,
                                         const std::string &opName,
                                         int64_t width,
                                         bool isSigned,
                                         const std::string &literal)
    {
        const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
        const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
        const wolvrix::lib::grh::ValueId val = graph.createValue(valueSym, static_cast<int32_t>(width), isSigned);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
        graph.addResult(op, val);
        graph.setAttr(op, "constValue", literal);
        return val;
    }

    std::optional<slang::SVInt> getConstLiteral(const wolvrix::lib::grh::Graph &graph,
                                                const wolvrix::lib::grh::Operation &op)
    {
        (void)graph;
        auto attr = op.attr("constValue");
        if (!attr)
        {
            return std::nullopt;
        }
        const auto *val = std::get_if<std::string>(&*attr);
        if (!val)
        {
            return std::nullopt;
        }
        try
        {
            return slang::SVInt::fromString(*val);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    wolvrix::lib::grh::ValueId makeSliceStatic(wolvrix::lib::grh::Graph &graph,
                                               const std::string &valueName,
                                               const std::string &opName,
                                               wolvrix::lib::grh::ValueId base,
                                               int64_t low,
                                               int64_t high)
    {
        const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
        const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
        const int32_t width = static_cast<int32_t>(high - low + 1);
        const wolvrix::lib::grh::ValueId val = graph.createValue(valueSym, width, false);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, opSym);
        graph.addOperand(op, base);
        graph.addResult(op, val);
        graph.setAttr(op, "sliceStart", low);
        graph.setAttr(op, "sliceEnd", high);
        return val;
    }

    wolvrix::lib::grh::ValueId makeConcat(wolvrix::lib::grh::Graph &graph,
                                          const std::string &valueName,
                                          const std::string &opName,
                                          const std::vector<wolvrix::lib::grh::ValueId> &operands,
                                          int32_t width)
    {
        const wolvrix::lib::grh::SymbolId valueSym = graph.internSymbol(valueName);
        const wolvrix::lib::grh::SymbolId opSym = graph.internSymbol(opName);
        const wolvrix::lib::grh::ValueId val = graph.createValue(valueSym, width, false);
        const wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat, opSym);
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.addResult(op, val);
        return val;
    }

} // namespace

int main()
{
    // Case 1: multi-iteration folding rewires downstream users via simplify
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g");
        wolvrix::lib::grh::ValueId c0 = makeConst(graph, "c0", "c0_op", 4, false, "4'h3");
        wolvrix::lib::grh::ValueId c1 = makeConst(graph, "c1", "c1_op", 4, false, "4'h1");

        wolvrix::lib::grh::ValueId sum = graph.createValue(graph.internSymbol("sum"), 4, false);
        wolvrix::lib::grh::OperationId add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                                   graph.internSymbol("add0"));
        graph.addOperand(add, c0);
        graph.addOperand(add, c1);
        graph.addResult(add, sum);

        wolvrix::lib::grh::ValueId pass = graph.createValue(graph.internSymbol("pass"), 4, false);
        wolvrix::lib::grh::OperationId assign = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                                      graph.internSymbol("assign0"));
        graph.addOperand(assign, sum);
        graph.addResult(assign, pass);

        wolvrix::lib::grh::ValueId neg = graph.createValue(graph.internSymbol("neg"), 4, false);
        wolvrix::lib::grh::OperationId invert = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                                      graph.internSymbol("not0"));
        graph.addOperand(invert, pass);
        graph.addResult(invert, neg);

        wolvrix::lib::grh::ValueId finalSum = graph.createValue(graph.internSymbol("finalSum"), 4, false);
        wolvrix::lib::grh::OperationId add2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                                    graph.internSymbol("add1"));
        graph.addOperand(add2, neg);
        graph.addOperand(add2, c1);
        graph.addResult(add2, finalSum);

        wolvrix::lib::grh::ValueId out = graph.createValue(graph.internSymbol("out"), 4, false);
        graph.bindOutputPort("out", out);
        wolvrix::lib::grh::OperationId assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                                         graph.internSymbol("assign1"));
        graph.addOperand(assignOut, finalSum);
        graph.addResult(assignOut, out);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());

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
            return fail("Expected simplify to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected simplify to mark changes");
        }

        if (graph.findOperation("assign1").valid())
        {
            return fail("assign1 should be removed after folding");
        }
        wolvrix::lib::grh::ValueId outVal = graph.outputPortValue("out");
        if (!outVal.valid())
        {
            return fail("Output port was not rewired to a constant");
        }
        wolvrix::lib::grh::Value outValue = graph.getValue(outVal);
        if (!outValue.definingOp().valid() ||
            graph.getOperation(outValue.definingOp()).kind() != wolvrix::lib::grh::OperationKind::kConstant)
        {
            return fail("Output port was not rewired to a constant");
        }
        auto literal = getConstLiteral(graph, graph.getOperation(outValue.definingOp()));
        slang::SVInt expected = slang::SVInt::fromString("4'hc").resize(4);
        if (!literal || !static_cast<bool>((*literal == expected)))
        {
            return fail("Final constant value mismatch");
        }
        const wolvrix::lib::grh::ValueId negValue = graph.findValue("neg");
        if (negValue.valid() && !graph.getValue(negValue).users().empty())
        {
            return fail("Expected users of intermediate value to be rewired");
        }
    }

    // Case 2: X operand blocks folding when xFold=known (default)
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g2");
        wolvrix::lib::grh::ValueId xval = makeConst(graph, "cx", "cx_op", 1, false, "1'bx");
        wolvrix::lib::grh::ValueId one = makeConst(graph, "c1", "c1_op", 1, false, "1'b1");

        wolvrix::lib::grh::ValueId andOut = graph.createValue(graph.internSymbol("andOut"), 1, false);
        wolvrix::lib::grh::OperationId op =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd, graph.internSymbol("and0"));
        graph.addOperand(op, xval);
        graph.addOperand(op, one);
        graph.addResult(op, andOut);
        graph.bindOutputPort("out", andOut);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
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
            return fail("Unexpected failure when X folding is blocked");
        }
        if (res.changed)
        {
            return fail("Simplify should not change graph when blocked by X");
        }
        const wolvrix::lib::grh::Operation opView = graph.getOperation(op);
        if (opView.operands()[0] != xval || opView.operands()[1] != one)
        {
            return fail("Operands should remain unchanged when folding is skipped");
        }
    }

    // Case 3: missing attribute triggers failure
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g3");
        wolvrix::lib::grh::ValueId c = makeConst(graph, "c", "c_op", 2, false, "2'h1");

        wolvrix::lib::grh::ValueId repOut = graph.createValue(graph.internSymbol("repOut"), 4, false);
        wolvrix::lib::grh::OperationId rep =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kReplicate, graph.internSymbol("rep0"));
        graph.addOperand(rep, c);
        graph.addResult(rep, repOut);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
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
        if (res.success)
        {
            return fail("Expected simplify to fail on missing replicate attribute");
        }
        if (!diags.hasError())
        {
            return fail("Expected diagnostics for missing attribute");
        }
    }

    // Case 4: simplify removes identity static slice without introducing extra structure
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g4");
        const wolvrix::lib::grh::SymbolId inSym = graph.internSymbol("in");
        const wolvrix::lib::grh::ValueId in = graph.createValue(inSym, 8, false);
        graph.bindInputPort("in", in);
        const wolvrix::lib::grh::ValueId slice =
            makeSliceStatic(graph, "_val_slice", "slice0", in, 0, 7);
        graph.bindOutputPort("out", slice);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during identity slice simplify: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected identity slice simplify to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected identity slice simplify to change graph");
        }
        if (graph.findOperation("slice0").valid())
        {
            return fail("Identity kSliceStatic should be removed");
        }
        if (graph.outputPortValue("out") != in)
        {
            return fail("Output should be rebound directly to slice base");
        }
    }

    // Case 5: simplify combines nested static slices without creating new feedback structure
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g5");
        const wolvrix::lib::grh::SymbolId inSym = graph.internSymbol("in");
        const wolvrix::lib::grh::ValueId in = graph.createValue(inSym, 16, false);
        graph.bindInputPort("in", in);
        const wolvrix::lib::grh::ValueId inner =
            makeSliceStatic(graph, "_val_inner", "inner_slice", in, 4, 11);
        const wolvrix::lib::grh::ValueId outer =
            makeSliceStatic(graph, "_val_outer", "outer_slice", inner, 2, 5);
        graph.bindOutputPort("out", outer);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during nested slice simplify: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected nested slice simplify to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected nested slice simplify to change graph");
        }
        if (graph.findOperation("inner_slice").valid())
        {
            return fail("Inner kSliceStatic should become dead after retargeting outer slice");
        }
        const wolvrix::lib::grh::OperationId outerOpId = graph.findOperation("outer_slice");
        if (!outerOpId.valid())
        {
            return fail("Outer kSliceStatic should remain after combination");
        }
        const wolvrix::lib::grh::Operation outerOp = graph.getOperation(outerOpId);
        if (outerOp.operands().size() != 1 || outerOp.operands()[0] != in)
        {
            return fail("Outer kSliceStatic should be retargeted to original base");
        }
        auto startAttr = outerOp.attr("sliceStart");
        auto endAttr = outerOp.attr("sliceEnd");
        if (!startAttr || !endAttr)
        {
            return fail("Combined outer kSliceStatic should keep slice attributes");
        }
        const auto *start = std::get_if<int64_t>(&*startAttr);
        const auto *end = std::get_if<int64_t>(&*endAttr);
        if (!start || !end || *start != 6 || *end != 9)
        {
            return fail("Nested kSliceStatic should combine to slice [9:6]");
        }
    }

    // Case 6: simplify retargets a slice into a single concat operand instead of keeping the full concat path
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g6");
        const wolvrix::lib::grh::SymbolId hiSym = graph.internSymbol("hi");
        const wolvrix::lib::grh::SymbolId midSym = graph.internSymbol("mid");
        const wolvrix::lib::grh::SymbolId loSym = graph.internSymbol("lo");
        const wolvrix::lib::grh::ValueId hi = graph.createValue(hiSym, 4, false);
        const wolvrix::lib::grh::ValueId mid = graph.createValue(midSym, 8, false);
        const wolvrix::lib::grh::ValueId lo = graph.createValue(loSym, 4, false);
        graph.bindInputPort("hi", hi);
        graph.bindInputPort("mid", mid);
        graph.bindInputPort("lo", lo);
        const wolvrix::lib::grh::ValueId concat =
            makeConcat(graph, "_val_concat", "concat0", {hi, mid, lo}, 16);
        const wolvrix::lib::grh::ValueId slice =
            makeSliceStatic(graph, "_val_slice", "slice0", concat, 6, 9);
        graph.bindOutputPort("out", slice);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during concat slice retarget: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected concat slice retarget to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected concat slice retarget to change graph");
        }
        const wolvrix::lib::grh::OperationId sliceOpId = graph.findOperation("slice0");
        if (!sliceOpId.valid())
        {
            return fail("Retargeted kSliceStatic should remain when selecting a sub-range of one operand");
        }
        const wolvrix::lib::grh::Operation sliceOp = graph.getOperation(sliceOpId);
        if (sliceOp.operands().size() != 1 || sliceOp.operands()[0] != mid)
        {
            return fail("Retargeted kSliceStatic should use the matching concat operand as base");
        }
        const auto startAttr = sliceOp.attr("sliceStart");
        const auto endAttr = sliceOp.attr("sliceEnd");
        const auto *start = startAttr ? std::get_if<int64_t>(&*startAttr) : nullptr;
        const auto *end = endAttr ? std::get_if<int64_t>(&*endAttr) : nullptr;
        if (!start || !end || *start != 2 || *end != 5)
        {
            return fail("Retargeted kSliceStatic should translate the slice to operand-local coordinates");
        }
    }

    // Case 7: simplify must not retarget a slice into a concat operand that depends on the slice itself
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g7");
        const wolvrix::lib::grh::SymbolId hiSym = graph.internSymbol("hi");
        const wolvrix::lib::grh::ValueId hi = graph.createValue(hiSym, 4, false);
        graph.bindInputPort("hi", hi);
        const wolvrix::lib::grh::ValueId low =
            graph.createValue(graph.internSymbol("_val_low"), 4, false);
        const wolvrix::lib::grh::ValueId concat =
            makeConcat(graph, "_val_concat", "concat0", {hi, low}, 8);
        const wolvrix::lib::grh::OperationId lowOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, graph.internSymbol("slice0"));
        graph.addOperand(lowOp, concat);
        graph.addResult(lowOp, low);
        graph.setAttr(lowOp, "sliceStart", int64_t{0});
        graph.setAttr(lowOp, "sliceEnd", int64_t{3});
        graph.bindOutputPort("out", low);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during pseudo-loop guard case: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected pseudo-loop guard case to succeed");
        }
        const wolvrix::lib::grh::Operation guardedSlice = graph.getOperation(lowOp);
        if (guardedSlice.operands().size() != 1 || guardedSlice.operands()[0] != concat)
        {
            return fail("Slice should keep its original concat base when retargeting would create a pseudo-loop");
        }
        const auto startAttr = guardedSlice.attr("sliceStart");
        const auto endAttr = guardedSlice.attr("sliceEnd");
        const auto *start = startAttr ? std::get_if<int64_t>(&*startAttr) : nullptr;
        const auto *end = endAttr ? std::get_if<int64_t>(&*endAttr) : nullptr;
        if (!start || !end || *start != 0 || *end != 3)
        {
            return fail("Pseudo-loop guard case should keep the original slice range");
        }
    }

    // Case 8: simplify flattens a single-use nested concat into its outer concat
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g8");
        const auto hi = graph.createValue(graph.internSymbol("hi"), 4, false);
        const auto a = graph.createValue(graph.internSymbol("a"), 3, false);
        const auto b = graph.createValue(graph.internSymbol("b"), 5, false);
        const auto lo = graph.createValue(graph.internSymbol("lo"), 2, false);
        graph.bindInputPort("hi", hi);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("lo", lo);

        const auto inner = makeConcat(graph, "_val_inner", "inner_concat", {a, b}, 8);
        const auto outer = makeConcat(graph, "_val_outer", "outer_concat", {hi, inner, lo}, 14);
        graph.bindOutputPort("out", outer);

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during nested concat flatten: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected nested concat flatten to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected nested concat flatten to change graph");
        }
        if (graph.findOperation("inner_concat").valid())
        {
            return fail("Inner concat should become dead after flattening");
        }
        const auto outerOpId = graph.findOperation("outer_concat");
        if (!outerOpId.valid())
        {
            return fail("Outer concat should remain after flattening");
        }
        const auto outerOp = graph.getOperation(outerOpId);
        if (outerOp.operands().size() != 4)
        {
            return fail("Flattened outer concat should have four operands");
        }
        if (outerOp.operands()[0] != hi || outerOp.operands()[1] != a ||
            outerOp.operands()[2] != b || outerOp.operands()[3] != lo)
        {
            return fail("Flattened outer concat should preserve operand order");
        }
    }

    // Case 9: simplify must not flatten a nested concat if one inner operand depends on the outer concat result
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g9");
        const auto top = graph.createValue(graph.internSymbol("top"), 4, false);
        const auto hi = graph.createValue(graph.internSymbol("hi"), 4, false);
        const auto low = graph.createValue(graph.internSymbol("_val_low"), 4, false);
        graph.bindInputPort("top", top);
        graph.bindInputPort("hi", hi);

        const auto inner = makeConcat(graph, "_val_inner", "inner_concat", {hi, low}, 8);
        const auto outer = makeConcat(graph, "_val_outer", "outer_concat", {top, inner}, 12);
        graph.bindOutputPort("out", outer);

        const auto lowOp =
            graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceStatic, graph.internSymbol("slice_low"));
        graph.addOperand(lowOp, outer);
        graph.addResult(lowOp, low);
        graph.setAttr(lowOp, "sliceStart", int64_t{0});
        graph.setAttr(lowOp, "sliceEnd", int64_t{3});

        PassManager manager;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during nested concat pseudo-loop guard: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected nested concat pseudo-loop guard case to succeed");
        }
        const auto innerOpId = graph.findOperation("inner_concat");
        const auto outerOpId = graph.findOperation("outer_concat");
        if (!innerOpId.valid() || !outerOpId.valid())
        {
            return fail("Pseudo-loop guard case should keep both concat operations");
        }
        const auto outerOp = graph.getOperation(outerOpId);
        if (outerOp.operands().size() != 2 || outerOp.operands()[0] != top || outerOp.operands()[1] != inner)
        {
            return fail("Outer concat should keep the nested concat operand when flattening would expose self-dependence");
        }
    }

    // Case 10: simplify keeps declared symbols resolvable — const-fold
    // materializes a declared value's folded constant, redundant-elim never
    // merges a declared alias away, and DCE treats declared storage as a
    // liveness root. The same graph with retention off folds the declared
    // value away entirely.
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g10");
        const auto c0 = makeConst(graph, "c0d", "c0d_op", 4, false, "4'h3");
        const auto c1 = makeConst(graph, "c1d", "c1d_op", 4, false, "4'h1");

        const auto folded = graph.createValue(graph.internSymbol("folded_sum"), 4, false);
        const auto add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                               graph.internSymbol("add_d"));
        graph.addOperand(add, c0);
        graph.addOperand(add, c1);
        graph.addResult(add, folded);

        const auto alias = graph.createValue(graph.internSymbol("alias_wire"), 4, false);
        const auto assign = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                  graph.internSymbol("assign_d"));
        graph.addOperand(assign, folded);
        graph.addResult(assign, alias);

        const auto out = graph.createValue(graph.internSymbol("out"), 4, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, alias);
        graph.addResult(assignOut, out);

        // Declared register with a write port but no readers: a DCE root.
        const auto held = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                graph.internSymbol("held_q"));
        graph.setAttr(held, "width", int64_t{4});
        graph.setAttr(held, "isSigned", false);
        graph.setAttr(held, "initValue", std::string("4'h0"));
        const auto heldWrite = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                     graph.internSymbol("held_q_write"));
        graph.setAttr(heldWrite, "regSymbol", std::string("held_q"));
        graph.setAttr(heldWrite, "eventEdge", std::vector<std::string>{"posedge"});
        const auto heldMask = makeConst(graph, "held_mask", "held_mask_op", 4, false, "4'hf");
        const auto heldEn = graph.createValue(graph.internSymbol("held_en"), 1, false);
        graph.bindInputPort("held_en", heldEn);
        const auto heldClk = graph.createValue(graph.internSymbol("held_clk"), 1, false);
        graph.bindInputPort("held_clk", heldClk);
        graph.addOperand(heldWrite, heldEn);
        graph.addOperand(heldWrite, alias);
        graph.addOperand(heldWrite, heldMask);
        graph.addOperand(heldWrite, heldClk);

        graph.addDeclaredSymbol(graph.lookupSymbol("folded_sum"));
        graph.addDeclaredSymbol(graph.lookupSymbol("alias_wire"));
        graph.addDeclaredSymbol(graph.lookupSymbol("held_q"));

        PassManager manager; // keepDeclaredSymbols defaults to true
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during declared-preservation run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected declared-preservation simplify to succeed");
        }
        if (!graph.validateDeclaredSymbols().empty())
        {
            return fail("simplify left unresolvable declared symbols");
        }
        const auto foldedAfter = graph.findValue("folded_sum");
        if (!foldedAfter.valid() || !graph.isDeclaredSymbol(graph.lookupSymbol("folded_sum")))
        {
            return fail("declared folded value lost its entity or membership");
        }
        const auto foldedDef = graph.getValue(foldedAfter).definingOp();
        if (!foldedDef.valid() ||
            graph.getOperation(foldedDef).kind() != wolvrix::lib::grh::OperationKind::kConstant)
        {
            return fail("declared folded value was not materialized as a constant");
        }
        if (!graph.findValue("alias_wire").valid() ||
            !graph.isDeclaredSymbol(graph.lookupSymbol("alias_wire")))
        {
            return fail("declared alias wire was merged away");
        }
        if (!graph.findOperation("held_q").valid())
        {
            return fail("declared unread register was eliminated");
        }
    }

    // Case 11: control — with retention off, the folded declared value is
    // replaced and erased like any other value.
    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g11");
        const auto c0 = makeConst(graph, "c0x", "c0x_op", 4, false, "4'h3");
        const auto c1 = makeConst(graph, "c1x", "c1x_op", 4, false, "4'h1");
        const auto folded = graph.createValue(graph.internSymbol("folded_sum"), 4, false);
        const auto add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                               graph.internSymbol("add_x"));
        graph.addOperand(add, c0);
        graph.addOperand(add, c1);
        graph.addResult(add, folded);
        const auto out = graph.createValue(graph.internSymbol("out"), 4, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, folded);
        graph.addResult(assignOut, out);
        graph.addDeclaredSymbol(graph.lookupSymbol("folded_sum"));

        PassManager manager;
        manager.options().keepDeclaredSymbols = false;
        manager.addPass(std::make_unique<SimplifyPass>());
        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during declared-off control run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected declared-off control simplify to succeed");
        }
        if (graph.findValue("folded_sum").valid() ||
            graph.isDeclaredSymbol(graph.lookupSymbol("folded_sum")))
        {
            return fail("declared-off control should fold and erase the value");
        }
    }

    return 0;
}
