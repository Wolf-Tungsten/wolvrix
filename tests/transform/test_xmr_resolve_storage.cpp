#include "core/grh.hpp"
#include "transform/xmr_resolve.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-xmr-resolve-storage] " << message << '\n';
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

std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation &op,
                                                       std::string_view key)
{
    auto attr = op.attr(key);
    if (!attr)
    {
        return std::nullopt;
    }
    if (const auto *value = std::get_if<std::vector<std::string>>(&*attr))
    {
        return *value;
    }
    return std::nullopt;
}

bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool hasPortOp(const wolvrix::lib::grh::Graph &graph,
               wolvrix::lib::grh::OperationKind kind,
               std::string_view attrKey,
               std::string_view attrValue)
{
    for (const auto opId : graph.operations())
    {
        const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
        if (op.kind() != kind)
        {
            continue;
        }
        auto attr = getAttrString(op, attrKey);
        if (attr && *attr == attrValue)
        {
            return true;
        }
    }
    return false;
}

bool hasRegisterReadOutput(const wolvrix::lib::grh::Graph &graph)
{
    for (const auto &port : graph.outputPorts())
    {
        if (port.name.empty())
        {
            continue;
        }
        const std::string &name = port.name;
        if (!startsWith(name, "__xmr_r_"))
        {
            continue;
        }
        const wolvrix::lib::grh::ValueId value = port.value;
        if (!value.valid())
        {
            continue;
        }
        const wolvrix::lib::grh::OperationId defOpId = graph.getValue(value).definingOp();
        if (!defOpId.valid())
        {
            continue;
        }
        const wolvrix::lib::grh::Operation defOp = graph.getOperation(defOpId);
        if (defOp.kind() != wolvrix::lib::grh::OperationKind::kRegisterReadPort)
        {
            continue;
        }
        auto regSymbol = getAttrString(defOp, "regSymbol");
        if (regSymbol && *regSymbol == "reg_a")
        {
            return true;
        }
    }
    return false;
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
    wolvrix::lib::grh::Graph &leaf = design.createGraph("leaf");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto regOp = leaf.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                            leaf.internSymbol("reg_a"));
    leaf.setAttr(regOp, "width", static_cast<int64_t>(8));
    leaf.setAttr(regOp, "isSigned", false);

    const auto latchOp = leaf.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                              leaf.internSymbol("lat_b"));
    leaf.setAttr(latchOp, "width", static_cast<int64_t>(4));
    leaf.setAttr(latchOp, "isSigned", false);

    const auto cond = top.createValue(top.internSymbol("cond"), 1, false);
    const auto data = top.createValue(top.internSymbol("data"), 8, false);
    const auto mask = top.createValue(top.internSymbol("mask"), 8, false);
    const auto clk = top.createValue(top.internSymbol("clk"), 1, false);
    const auto latchCond = top.createValue(top.internSymbol("latch_cond"), 1, false);
    const auto latchData = top.createValue(top.internSymbol("latch_data"), 4, false);
    const auto latchMask = top.createValue(top.internSymbol("latch_mask"), 4, false);

    wolvrix::lib::grh::OperationId instOp =
        top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                            top.internSymbol("_op_test_xmr_inst"));
    top.setAttr(instOp, "moduleName", std::string("leaf"));
    top.setAttr(instOp, "instanceName", std::string("u_leaf"));
    top.setAttr(instOp, "inputPortName", std::vector<std::string>{});
    top.setAttr(instOp, "outputPortName", std::vector<std::string>{});
    top.setAttr(instOp, "inoutPortName", std::vector<std::string>{});

    const auto readValue = top.createValue(top.internSymbol("xmr_read"), 8, false);
    const auto xmrRead =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRRead,
                            top.internSymbol("_op_test_xmr_read"));
    top.addResult(xmrRead, readValue);
    top.setAttr(xmrRead, "xmrPath", std::string("u_leaf.reg_a"));

    const auto xmrWriteReg =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRWrite,
                            top.internSymbol("_op_test_xmr_write_reg"));
    top.addOperand(xmrWriteReg, cond);
    top.addOperand(xmrWriteReg, data);
    top.addOperand(xmrWriteReg, mask);
    top.addOperand(xmrWriteReg, clk);
    top.setAttr(xmrWriteReg, "xmrPath", std::string("u_leaf.reg_a"));
    top.setAttr(xmrWriteReg, "eventEdge", std::vector<std::string>{"posedge"});

    const auto xmrWriteLatch =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRWrite,
                            top.internSymbol("_op_test_xmr_write_latch"));
    top.addOperand(xmrWriteLatch, latchCond);
    top.addOperand(xmrWriteLatch, latchData);
    top.addOperand(xmrWriteLatch, latchMask);
    top.setAttr(xmrWriteLatch, "xmrPath", std::string("u_leaf.lat_b"));

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
        return fail("XMR ops were not fully resolved");
    }
    if (!hasRegisterReadOutput(leaf))
    {
        return fail("Register XMR read port not created");
    }
    if (!hasPortOp(leaf, wolvrix::lib::grh::OperationKind::kRegisterWriteMaskPort, "regSymbol", "reg_a"))
    {
        return fail("Register XMR write port not created");
    }
    if (!hasPortOp(leaf, wolvrix::lib::grh::OperationKind::kLatchWritePort, "latchSymbol", "lat_b"))
    {
        return fail("Latch XMR write port not created");
    }

    const wolvrix::lib::grh::Operation inst = top.getOperation(instOp);
    const auto inputNames = getAttrStrings(inst, "inputPortName");
    const auto outputNames = getAttrStrings(inst, "outputPortName");
    if (!inputNames || !outputNames)
    {
        return fail("Instance missing port name attributes");
    }
    if (inputNames->size() != 7)
    {
        return fail("Unexpected input port count after XMR resolve");
    }
    for (const auto &name : *inputNames)
    {
        if (!startsWith(name, "__xmr_w_"))
        {
            return fail("Unexpected XMR write port name prefix");
        }
    }
    if (outputNames->size() != 1 || !startsWith((*outputNames)[0], "__xmr_r_"))
    {
        return fail("Unexpected XMR read output port names");
    }

    return 0;
}
