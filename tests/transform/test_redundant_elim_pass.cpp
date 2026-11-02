#include "core/grh.hpp"
#include "transform/redundant_elim.hpp"
#include "transform/const_fold.hpp"
#include "core/transform.hpp"
#include "ingest/node_attributes.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
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
            fillGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                      fillGraph.internSymbol("mem"));
        fillGraph.setAttr(mem, "width", static_cast<int64_t>(32));
        fillGraph.setAttr(mem, "isSigned", false);
        fillGraph.setAttr(mem, "shape", std::vector<int64_t>{4});

        wolvrix::lib::grh::ValueId cond = fillGraph.createValue(fillGraph.internSymbol("cond"), 1, false);
        wolvrix::lib::grh::ValueId data = fillGraph.createValue(fillGraph.internSymbol("data"), 8, false);
        wolvrix::lib::grh::ValueId clk = fillGraph.createValue(fillGraph.internSymbol("clk"), 1, false);
        fillGraph.bindInputPort("cond", cond);
        fillGraph.bindInputPort("data", data);
        fillGraph.bindInputPort("clk", clk);

        wolvrix::lib::grh::OperationId fill =
            fillGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterFillPort,
                                      fillGraph.internSymbol("fill"));
        fillGraph.addOperand(fill, cond);
        fillGraph.addOperand(fill, data);
        fillGraph.addOperand(fill, clk);
        fillGraph.setAttr(fill, "regSymbol", std::string("mem"));
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
            return fail("kRegisterFillPort must not be removed as redundant");
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

    {
        wolvrix::lib::grh::Design shapeDesign;
        auto &shapeGraph = shapeDesign.createGraph("shape_cse");
        const auto source = shapeGraph.createValue(shapeGraph.internSymbol("source"), 1, false);
        shapeGraph.bindInputPort("source", source);
        const auto flat = shapeGraph.createValue(shapeGraph.internSymbol("flat"), 1, false);
        const auto flatNot = shapeGraph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                        shapeGraph.makeInternalOpSym());
        shapeGraph.addOperand(flatNot, source);
        shapeGraph.addResult(flatNot, flat);
        const std::array<int64_t, 1> one{1};
        const auto shaped = shapeGraph.createValue(shapeGraph.internSymbol("shaped"), 1, false,
                                                    wolvrix::lib::grh::ValueType::Logic, one);
        const auto shapedNot = shapeGraph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                          shapeGraph.makeInternalOpSym());
        shapeGraph.addOperand(shapedNot, source);
        shapeGraph.addResult(shapedNot, shaped);
        const auto mem = shapeGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                    shapeGraph.internSymbol("mem"));
        shapeGraph.setAttr(mem, "width", int64_t{2});
        shapeGraph.setAttr(mem, "isSigned", false);
        shapeGraph.setAttr(mem, "shape", std::vector<int64_t>{2});
        const auto readValue = shapeGraph.createValue(shapeGraph.internSymbol("read"), 1, false);
        const auto read = shapeGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort,
                                                     shapeGraph.makeInternalOpSym());
        shapeGraph.addOperand(read, flat);
        shapeGraph.addResult(read, readValue);
        shapeGraph.setAttr(read, "regSymbol", std::string("mem"));
        shapeGraph.bindOutputPort("read", readValue);
        const auto sliceValue = shapeGraph.createValue(shapeGraph.internSymbol("slice"), 1, false);
        const auto slice = shapeGraph.createOperation(wolvrix::lib::grh::OperationKind::kSliceArray,
                                                      shapeGraph.makeInternalOpSym());
        shapeGraph.addOperand(slice, shaped);
        shapeGraph.addOperand(slice, source);
        shapeGraph.addResult(slice, sliceValue);
        shapeGraph.bindOutputPort("slice", sliceValue);

        PassManager shapeManager;
        shapeManager.addPass(std::make_unique<RedundantElimPass>());
        PassDiagnostics shapeDiagnostics;
        const PassManagerResult shapeResult = shapeManager.run(shapeDesign, shapeDiagnostics);
        if (!shapeResult.success || shapeDiagnostics.hasError() ||
            !shapeGraph.valueShape(flat).empty() || shapeGraph.valueShape(shaped).size() != 1 ||
            shapeGraph.opOperands(read)[0] != flat || shapeGraph.opOperands(slice)[0] != shaped)
            return fail("CSE merged flat index and shaped array values");
    }

    // Regression (CASE_009): a kAssign across a shape boundary is an aggregate
    // pack/unpack, not a pure rename. Inlining it would backfill the aggregate
    // shape onto the flat destination via replaceResult, corrupting an output
    // port's declared type.
    {
        wolvrix::lib::grh::Design portDesign;
        auto &portGraph = portDesign.createGraph("assign_shape_port");
        const auto reg = portGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   portGraph.internSymbol("pri"));
        portGraph.setAttr(reg, "width", int64_t{4});
        portGraph.setAttr(reg, "isSigned", false);
        portGraph.setAttr(reg, "shape", std::vector<int64_t>{2});
        const std::array<int64_t, 1> aggShape{2};
        const auto agg = portGraph.createValue(portGraph.internSymbol("_val_agg"), 4, false,
                                               wolvrix::lib::grh::ValueType::Logic, aggShape);
        const auto read = portGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                    portGraph.makeInternalOpSym());
        portGraph.setAttr(read, "regSymbol", std::string("pri"));
        portGraph.addResult(read, agg);
        const auto flatOut = portGraph.createValue(portGraph.internSymbol("flat_out"), 4, false);
        const auto pack = portGraph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                    portGraph.internSymbol("pack"));
        portGraph.addOperand(pack, agg);
        portGraph.addResult(pack, flatOut);
        portGraph.bindOutputPort("flat_out", flatOut);

        PassManager portManager;
        portManager.addPass(std::make_unique<RedundantElimPass>());
        PassDiagnostics portDiagnostics;
        const PassManagerResult portResult = portManager.run(portDesign, portDiagnostics);
        if (!portResult.success || portDiagnostics.hasError())
            return fail("assign pack run failed");
        if (!portGraph.findOperation("pack").valid())
            return fail("aggregate pack assign into output port must not be inlined");
        if (!portGraph.valueShape(flatOut).empty())
            return fail("output port value must stay flat");
        const wolvrix::lib::grh::ValueId portValue = portGraph.outputPorts().front().value;
        if (portValue != flatOut)
            return fail("output port must stay bound to the flat assign result");
    }

    {
        wolvrix::lib::grh::Design tempDesign;
        auto &tempGraph = tempDesign.createGraph("assign_shape_temp");
        const auto reg = tempGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   tempGraph.internSymbol("pri"));
        tempGraph.setAttr(reg, "width", int64_t{4});
        tempGraph.setAttr(reg, "isSigned", false);
        tempGraph.setAttr(reg, "shape", std::vector<int64_t>{2});
        const std::array<int64_t, 1> aggShape{2};
        const auto agg = tempGraph.createValue(tempGraph.internSymbol("_val_agg"), 4, false,
                                               wolvrix::lib::grh::ValueType::Logic, aggShape);
        const auto read = tempGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                    tempGraph.makeInternalOpSym());
        tempGraph.setAttr(read, "regSymbol", std::string("pri"));
        tempGraph.addResult(read, agg);
        const auto flatW = tempGraph.createValue(tempGraph.internSymbol("flat_w"), 4, false);
        const auto pack = tempGraph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                    tempGraph.internSymbol("pack"));
        tempGraph.addOperand(pack, agg);
        tempGraph.addResult(pack, flatW);
        tempGraph.bindOutputPort("flat_w_user", flatW);

        PassManager tempManager;
        tempManager.addPass(std::make_unique<RedundantElimPass>());
        PassDiagnostics tempDiagnostics;
        const PassManagerResult tempResult = tempManager.run(tempDesign, tempDiagnostics);
        if (!tempResult.success || tempDiagnostics.hasError())
            return fail("temp assign pack run failed");
        if (!tempGraph.findOperation("pack").valid())
            return fail("aggregate pack assign into flat temp must not be inlined");
        if (!tempGraph.valueShape(flatW).empty())
            return fail("flat temp value must stay flat");
        if (tempGraph.getValue(flatW).definingOp() != pack)
            return fail("flat temp must stay driven by the pack assign");
    }

    // A shared constant feeding output assigns must keep both output drivers
    // and their source origins when the assigns are replaced with constants.
    {
        using namespace wolvrix::lib::grh;
        Design constantDesign;
        Graph& constantGraph = constantDesign.createGraph("constant_output_assign");
        const ValueId literal = constantGraph.createValue(constantGraph.internSymbol("literal"), 8, false);
        const OperationId constant = constantGraph.createOperation(OperationKind::kConstant,
            constantGraph.internSymbol("literal_op"));
        constantGraph.addResult(constant, literal);
        constantGraph.setAttr(constant, "constValue", std::string("8'h5a"));
        constantGraph.setAttr(constant, "originModules", std::vector<std::string>{"constant_source"});
        namespace node = wolvrix::lib::ingest::node;
        const auto nodeKey = [&](const std::string& name) {
            return node::key({"source", "Circuit", "constant_output_assign", {}, name, {}});
        };
        node::write(constantGraph, constant, {{nodeKey("literal")}, {{nodeKey("literal"), 0}}, {}});
        for (const std::string name : {"first", "second"}) {
            const ValueId output = constantGraph.createValue(constantGraph.internSymbol(name), 8, false);
            const OperationId assign = constantGraph.createOperation(OperationKind::kAssign,
                constantGraph.internSymbol(name + "_assign"));
            constantGraph.addOperand(assign, literal);
            constantGraph.addResult(assign, output);
            constantGraph.setAttr(assign, "originModules", std::vector<std::string>{name});
            node::write(constantGraph, assign, {{nodeKey(name)}, {{nodeKey(name), 0}}, {}});
            constantGraph.bindOutputPort(name, output);
        }
        node::establishPolicy(constantGraph);
        PassManager constantManager;
        constantManager.addPass(std::make_unique<RedundantElimPass>());
        PassDiagnostics constantDiagnostics;
        PassManagerResult constantResult;
        try {
            constantResult = constantManager.run(constantDesign, constantDiagnostics);
        } catch (const std::exception& error) {
            return fail(std::string("constant output assign rewrite threw: ") + error.what());
        }
        if (!constantResult.success || constantDiagnostics.hasError())
            return fail("constant output assign rewrite failed");
        for (const auto& port : constantGraph.outputPorts()) {
            const OperationId definition = constantGraph.valueDef(port.value);
            if (constantGraph.opKind(definition) != OperationKind::kConstant)
                return fail("constant output assign was not folded into a constant driver");
            if ((port.name == "second") != (definition == constant))
                return fail("shared constant should clone for first output and reparent for final user");
            const auto origins = getOriginModules(constantGraph, definition);
            if (!origins || std::find(origins->begin(), origins->end(), port.name) == origins->end() ||
                std::find(origins->begin(), origins->end(), "constant_source") == origins->end())
                return fail("constant output assign lost source origins");
            const auto attributes = node::read(constantGraph, definition);
            if (std::find(attributes.members.begin(), attributes.members.end(), nodeKey(port.name)) == attributes.members.end() ||
                std::find(attributes.members.begin(), attributes.members.end(), nodeKey("literal")) == attributes.members.end())
                return fail("constant output assign lost node associations");
        }
        if (!node::inspect(constantGraph).errors.empty())
            return fail("constant output assign produced invalid node associations");
    }

    // Short literals are first interpreted in the source value's type. Moving
    // them directly to a result with different signedness changes extension.
    for (const bool sourceSigned : {false, true}) {
        for (const int outputs : {1, 2}) {
            using namespace wolvrix::lib::grh;
            Design signedDesign;
            Graph& signedGraph = signedDesign.createGraph("constant_output_cast");
            const ValueId literal = signedGraph.createValue(signedGraph.internSymbol("literal"), 8, sourceSigned);
            const OperationId constant = signedGraph.createOperation(OperationKind::kConstant,
                signedGraph.internSymbol("literal_op"));
            signedGraph.addResult(constant, literal);
            signedGraph.setAttr(constant, "constValue", std::string("1'b1"));
            for (int index = 0; index < outputs; ++index) {
                const std::string name = "out" + std::to_string(index);
                const ValueId output = signedGraph.createValue(signedGraph.internSymbol(name), 8, !sourceSigned);
                const OperationId assign = signedGraph.createOperation(OperationKind::kAssign,
                    signedGraph.internSymbol(name + "_assign"));
                signedGraph.addOperand(assign, literal);
                signedGraph.addResult(assign, output);
                signedGraph.bindOutputPort(name, output);
            }
            PassManager signedManager;
            signedManager.addPass(std::make_unique<RedundantElimPass>());
            signedManager.addPass(std::make_unique<ConstantFoldPass>());
            PassDiagnostics signedDiagnostics;
            if (!signedManager.run(signedDesign, signedDiagnostics).success || signedDiagnostics.hasError())
                return fail("constant output cast rewrite failed");
            for (const auto& port : signedGraph.outputPorts()) {
                const auto definition = signedGraph.getOperation(signedGraph.valueDef(port.value));
                const auto text = definition.attr("constValue");
                if (definition.kind() != OperationKind::kConstant || !text || !std::holds_alternative<std::string>(*text))
                    return fail("constant output cast did not evaluate to a constant");
                auto bits = slang::SVInt::fromString(std::get<std::string>(*text));
                bits.setSigned(signedGraph.valueSigned(port.value));
                bits = bits.resize(8);
                bits.setSigned(false);
                if (bits.as<uint64_t>() != std::optional<uint64_t>(sourceSigned ? 255 : 1))
                    return fail("constant output rewrite changed short-literal sign extension");
            }
        }
    }

    return 0;
}
