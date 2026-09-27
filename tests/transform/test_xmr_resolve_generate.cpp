#include "core/grh.hpp"
#include "transform/xmr_resolve.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-xmr-resolve-generate] " << message << '\n';
    return 1;
}

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                         std::string_view key)
{
    auto attr = op.attr(key);
    if (!attr)
    {
        return std::nullopt;
    }
    if (const auto *value = std::get_if<std::string>(&*attr))
    {
        return *value;
    }
    return std::nullopt;
}

bool hasNoXmrOps(const wolvrix::lib::grh::Graph &graph)
{
    for (const auto opId : graph.operations())
    {
        const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kXMRRead ||
            op.kind() == wolvrix::lib::grh::OperationKind::kXMRWrite)
        {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    // Generate-scope copies live inside the enclosing graph under
    // "<block>$<round>$<name>" symbols.
    const wolvrix::lib::grh::SymbolId sigSym = top.internSymbol("gen_loop$3$sig");
    const auto sigValue = top.createValue(sigSym, 8, false);
    top.addDeclaredSymbol(sigSym);
    const std::size_t group =
        top.addGenerateGroup(top.internSymbol("gen_loop"), top.internSymbol("sig"));
    top.addGenerateGroupSymbol(group, sigSym);

    const wolvrix::lib::grh::SymbolId accSym = top.internSymbol("gen_loop$1$acc");
    const auto regOp = top.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                           accSym);
    top.setAttr(regOp, "width", static_cast<int64_t>(8));
    top.setAttr(regOp, "isSigned", false);
    top.addDeclaredSymbol(accSym);

    const auto dstValue = top.createValue(top.internSymbol("dst"), 8, false);
    const auto cond = top.createValue(top.internSymbol("cond"), 1, false);
    const auto data = top.createValue(top.internSymbol("data"), 8, false);
    const auto mask = top.createValue(top.internSymbol("mask"), 8, false);
    const auto clk = top.createValue(top.internSymbol("clk"), 1, false);

    const auto readValue = top.createValue(top.internSymbol("xmr_read"), 8, false);
    const auto xmrRead =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRRead,
                            top.internSymbol("_op_test_xmr_gen_read"));
    top.addResult(xmrRead, readValue);
    top.setAttr(xmrRead, "xmrPath", std::string("gen_loop[3].sig"));

    const auto assign =
        top.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                            top.internSymbol("_op_test_xmr_gen_assign"));
    top.addOperand(assign, readValue);
    top.addResult(assign, dstValue);

    const auto xmrWrite =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRWrite,
                            top.internSymbol("_op_test_xmr_gen_write"));
    top.addOperand(xmrWrite, cond);
    top.addOperand(xmrWrite, data);
    top.addOperand(xmrWrite, mask);
    top.addOperand(xmrWrite, clk);
    top.setAttr(xmrWrite, "xmrPath", std::string("gen_loop[1].acc"));
    top.setAttr(xmrWrite, "eventEdge", std::vector<std::string>{"posedge"});

    PassManager manager;
    manager.addPass(std::make_unique<XmrResolvePass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("XMR resolve pass failed");
    }

    if (!hasNoXmrOps(top))
    {
        return fail("Generate-scope XMR ops were not resolved");
    }

    const wolvrix::lib::grh::Operation assignOp = top.getOperation(assign);
    if (assignOp.operands().size() != 1)
    {
        return fail("Assign operand count mismatch after XMR read resolve");
    }
    if (top.getValue(assignOp.operands().front()).symbolText() != "gen_loop$3$sig")
    {
        return fail("XMR read did not resolve to the generate-scope value");
    }

    bool foundWritePort = false;
    for (const auto opId : top.operations())
    {
        const wolvrix::lib::grh::Operation op = top.getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kRegisterWritePort)
        {
            continue;
        }
        auto regSymbol = getAttrString(op, "regSymbol");
        if (regSymbol && *regSymbol == "gen_loop$1$acc")
        {
            foundWritePort = true;
            break;
        }
    }
    if (!foundWritePort)
    {
        return fail("XMR write did not create a write port on the generate-scope register");
    }

    (void)sigValue;
    return 0;
}
