#include "core/grh.hpp"
#include "transform/redundant_elim.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <string>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[redundant-elim-tests] " << message << '\n';
        return 1;
    }

} // namespace

int main()
{
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &graph = design.createGraph("g");

    const wolvrix::lib::grh::SymbolId resetSym = graph.internSymbol("reset");
    wolvrix::lib::grh::ValueId reset = graph.createValue(resetSym, 1, false);
    graph.bindInputPort("reset", reset);

    wolvrix::lib::grh::ValueId notReset = graph.createValue(graph.internSymbol("not_reset"), 1, false);
    wolvrix::lib::grh::OperationId notOp =
        graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicNot,
                              graph.internSymbol("not_op"));
    graph.addOperand(notOp, reset);
    graph.addResult(notOp, notReset);

    wolvrix::lib::grh::ValueId guard = graph.createValue(graph.internSymbol("guard"), 1, false);
    wolvrix::lib::grh::OperationId orOp =
        graph.createOperation(wolvrix::lib::grh::OperationKind::kLogicOr,
                              graph.internSymbol("or_op"));
    graph.addOperand(orOp, reset);
    graph.addOperand(orOp, notReset);
    graph.addResult(orOp, guard);

    const std::string outName = "out";
    graph.bindOutputPort(outName, guard);

    PassManager manager;
    manager.addPass(std::make_unique<RedundantElimPass>());

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
        return fail("Expected redundant elimination to succeed");
    }
    if (!res.changed)
    {
        return fail("Expected redundant elimination to report changes");
    }

    if (graph.findOperation("or_op").valid())
    {
        return fail("or_op should be removed");
    }

    wolvrix::lib::grh::ValueId outValueId = wolvrix::lib::grh::ValueId::invalid();
    for (const auto &port : graph.outputPorts())
    {
        if (port.name == outName)
        {
            outValueId = port.value;
            break;
        }
    }
    if (!outValueId.valid())
    {
        return fail("Output port not found");
    }

    wolvrix::lib::grh::Value outValue = graph.getValue(outValueId);
    wolvrix::lib::grh::OperationId defOpId = outValue.definingOp();
    if (!defOpId.valid())
    {
        return fail("Output should be driven by a constant");
    }
    wolvrix::lib::grh::Operation defOp = graph.getOperation(defOpId);
    if (defOp.kind() != wolvrix::lib::grh::OperationKind::kConstant)
    {
        return fail("Output should be driven by kConstant");
    }
    auto constAttr = defOp.attr("constValue");
    if (!constAttr)
    {
        return fail("Constant is missing constValue attribute");
    }
    const auto *literal = std::get_if<std::string>(&*constAttr);
    if (!literal || *literal != "1'b1")
    {
        return fail("Expected constValue to be 1'b1");
    }

    {
        wolvrix::lib::grh::Design fillDesign;
        wolvrix::lib::grh::Graph &fillGraph = fillDesign.createGraph("fill");

        wolvrix::lib::grh::OperationId mem =
            fillGraph.createOperation(wolvrix::lib::grh::OperationKind::kMemory,
                                      fillGraph.internSymbol("mem"));
        fillGraph.setAttr(mem, "width", static_cast<int64_t>(8));
        fillGraph.setAttr(mem, "row", static_cast<int64_t>(4));
        fillGraph.setAttr(mem, "isSigned", false);

        wolvrix::lib::grh::ValueId cond = fillGraph.createValue(fillGraph.internSymbol("cond"), 1, false);
        wolvrix::lib::grh::ValueId data = fillGraph.createValue(fillGraph.internSymbol("data"), 8, false);
        wolvrix::lib::grh::ValueId clk = fillGraph.createValue(fillGraph.internSymbol("clk"), 1, false);
        fillGraph.bindInputPort("cond", cond);
        fillGraph.bindInputPort("data", data);
        fillGraph.bindInputPort("clk", clk);

        wolvrix::lib::grh::OperationId fill =
            fillGraph.createOperation(wolvrix::lib::grh::OperationKind::kMemoryFillPort,
                                      fillGraph.internSymbol("fill"));
        fillGraph.addOperand(fill, cond);
        fillGraph.addOperand(fill, data);
        fillGraph.addOperand(fill, clk);
        fillGraph.setAttr(fill, "memSymbol", std::string("mem"));
        fillGraph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});

        PassManager fillManager;
        fillManager.addPass(std::make_unique<RedundantElimPass>());

        PassDiagnostics fillDiags;
        PassManagerResult fillRes{};
        try
        {
            fillRes = fillManager.run(fillDesign, fillDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during fill run: ") + ex.what());
        }
        if (!fillRes.success || fillDiags.hasError())
        {
            return fail("Expected redundant elimination fill run to succeed");
        }
        if (!fillGraph.findOperation("fill").valid())
        {
            return fail("kMemoryFillPort must not be removed as redundant");
        }
    }

    {
        wolvrix::lib::grh::Design declaredDesign;
        wolvrix::lib::grh::Graph &declaredGraph = declaredDesign.createGraph("declared");

        const wolvrix::lib::grh::SymbolId declResetSym = declaredGraph.internSymbol("reset");
        wolvrix::lib::grh::ValueId declReset = declaredGraph.createValue(declResetSym, 1, false);
        declaredGraph.bindInputPort("reset", declReset);

        wolvrix::lib::grh::ValueId declNotReset =
            declaredGraph.createValue(declaredGraph.internSymbol("not_reset"), 1, false);
        wolvrix::lib::grh::OperationId declNotOp =
            declaredGraph.createOperation(wolvrix::lib::grh::OperationKind::kLogicNot,
                                          declaredGraph.internSymbol("not_op"));
        declaredGraph.addOperand(declNotOp, declReset);
        declaredGraph.addResult(declNotOp, declNotReset);

        const wolvrix::lib::grh::SymbolId declGuardSym = declaredGraph.internSymbol("guard");
        wolvrix::lib::grh::ValueId declGuard = declaredGraph.createValue(declGuardSym, 1, false);
        declaredGraph.addDeclaredSymbol(declGuardSym);
        wolvrix::lib::grh::OperationId declOrOp =
            declaredGraph.createOperation(wolvrix::lib::grh::OperationKind::kLogicOr,
                                          declaredGraph.internSymbol("or_op"));
        declaredGraph.addOperand(declOrOp, declReset);
        declaredGraph.addOperand(declOrOp, declNotReset);
        declaredGraph.addResult(declOrOp, declGuard);

        PassManager declaredManager;
        declaredManager.addPass(std::make_unique<RedundantElimPass>());

        PassDiagnostics declaredDiags;
        PassManagerResult declaredRes{};
        try
        {
            declaredRes = declaredManager.run(declaredDesign, declaredDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during declared run: ") + ex.what());
        }
        if (!declaredRes.success || declaredDiags.hasError())
        {
            return fail("Expected redundant elimination declared run to succeed");
        }
        if (!declaredGraph.findOperation("or_op").valid())
        {
            return fail("declared guard kLogicOr must not be folded away");
        }
        const wolvrix::lib::grh::ValueId guardAfter = declaredGraph.findValue("guard");
        if (!guardAfter.valid())
        {
            return fail("declared guard value must survive");
        }
        if (declaredGraph.getValue(guardAfter).definingOp() != declOrOp)
        {
            return fail("declared guard must stay driven by or_op");
        }
        const std::vector<std::string> issues = declaredGraph.validateDeclaredSymbols();
        if (!issues.empty())
        {
            return fail(std::string("declared symbols inconsistent after run: ") + issues.front());
        }
    }

    return 0;
}
