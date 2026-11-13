#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/xmr_resolve.hpp"

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
    std::cerr << "[transform-xmr-resolve-inout] " << message << '\n';
    return 1;
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

} // namespace

int main()
{
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &child = design.createGraph("child");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto childA = child.createValue(child.internSymbol("a"), 1, false);
    const auto childY = child.createValue(child.internSymbol("y"), 1, false);
    const auto childIoIn = child.createValue(child.internSymbol("io__in"), 1, false);
    const auto childIoOut = child.createValue(child.internSymbol("io__out"), 1, false);
    const auto childIoOe = child.createValue(child.internSymbol("io__oe"), 1, false);
    child.bindInputPort("a", childA);
    child.bindOutputPort("y", childY);
    child.bindInoutPort("io", childIoIn, childIoOut, childIoOe);
    const auto childRead = child.createValue(child.internSymbol("leaf_r"), 1, false);
    const auto childWrite = child.createValue(child.internSymbol("leaf_w"), 1, false);

    const auto topA = top.createValue(top.internSymbol("a"), 1, false);
    const auto topY = top.createValue(top.internSymbol("y"), 1, false);
    const auto topIoOut = top.createValue(top.internSymbol("io__out"), 1, false);
    const auto topIoOe = top.createValue(top.internSymbol("io__oe"), 1, false);
    const auto topIoIn = top.createValue(top.internSymbol("io__in"), 1, false);

    wolvrix::lib::grh::OperationId instOp =
        top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                            top.internSymbol("_op_test_xmr_inst"));
    top.addOperand(instOp, topA);
    top.addOperand(instOp, topIoIn);
    top.addResult(instOp, topY);
    top.addResult(instOp, topIoOut);
    top.addResult(instOp, topIoOe);
    top.setAttr(instOp, "moduleName", std::string("child"));
    top.setAttr(instOp, "instanceName", std::string("u_child"));
    top.setAttr(instOp, "inputPortName", std::vector<std::string>{"a"});
    top.setAttr(instOp, "outputPortName", std::vector<std::string>{"y"});
    top.setAttr(instOp, "inoutPortName", std::vector<std::string>{"io"});

    const auto readValue = top.createValue(top.internSymbol("xmr_read"), 1, false);
    wolvrix::lib::grh::OperationId xmrRead =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRRead,
                            top.internSymbol("_op_test_xmr_read"));
    top.addResult(xmrRead, readValue);
    top.setAttr(xmrRead, "xmrPath", std::string("u_child.leaf_r"));

    const auto writeValue = top.createValue(top.internSymbol("xmr_write"), 1, false);
    wolvrix::lib::grh::OperationId xmrWrite =
        top.createOperation(wolvrix::lib::grh::OperationKind::kXMRWrite,
                            top.internSymbol("_op_test_xmr_write"));
    top.addOperand(xmrWrite, writeValue);
    top.setAttr(xmrWrite, "xmrPath", std::string("u_child.leaf_w"));

    auto& childTree = child.getOrCreateSemanticTree("fixture", "Circuit", "child");
    const auto childReadNode = childTree.addNode(childTree.root(), "leaf_r");
    const auto childWriteNode = childTree.addNode(childTree.root(), "leaf_w");
    child.setValueSemanticIds(childRead, std::span<const wolvrix::lib::grh::SemanticId>(&childReadNode, 1));
    child.setValueSemanticIds(childWrite, std::span<const wolvrix::lib::grh::SemanticId>(&childWriteNode, 1));
    auto& topTree = top.getOrCreateSemanticTree("fixture", "Circuit", "top");
    const auto readNode = topTree.addNode(topTree.root(), "read");
    const auto writeNode = topTree.addNode(topTree.root(), "write");
    top.setValueSemanticIds(readValue, std::span<const wolvrix::lib::grh::SemanticId>(&readNode, 1));
    top.setOpSemanticIds(xmrRead, std::span<const wolvrix::lib::grh::SemanticId>(&readNode, 1));
    top.setOpSemanticIds(xmrWrite, std::span<const wolvrix::lib::grh::SemanticId>(&writeNode, 1));

    PassManager manager;
    manager.addPass(std::make_unique<XmrResolvePass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("XMR resolve pass failed");
    }

    const wolvrix::lib::grh::Operation op = top.getOperation(instOp);
    const auto inputNames = getAttrStrings(op, "inputPortName");
    const auto outputNames = getAttrStrings(op, "outputPortName");
    const auto inoutNames = getAttrStrings(op, "inoutPortName");
    if (!inputNames || !outputNames || !inoutNames)
    {
        return fail("Instance missing port name attributes");
    }
    if (inputNames->size() != 2 || (*inputNames)[0] != "a")
    {
        return fail("Input port names not updated as expected");
    }
    if (outputNames->size() != 2 || (*outputNames)[0] != "y")
    {
        return fail("Output port names not updated as expected");
    }
    if (inoutNames->size() != 1 || (*inoutNames)[0] != "io")
    {
        return fail("Inout port names changed unexpectedly");
    }
    if (!startsWith((*inputNames)[1], "__xmr_w_"))
    {
        return fail("XMR write port not inserted into input names");
    }
    if (!startsWith((*outputNames)[1], "__xmr_r_"))
    {
        return fail("XMR read port not inserted into output names");
    }

    const auto operands = op.operands();
    const auto results = op.results();
    if (operands.size() != 3 || results.size() != 4)
    {
        return fail("Instance operand/result count mismatch after resolve");
    }
    if (operands[0] != topA || operands[2] != topIoIn)
    {
        return fail("Inout operands were reordered by XMR resolve");
    }
    if (results[0] != topY || results[2] != topIoOut || results[3] != topIoOe)
    {
        return fail("Inout results were reordered by XMR resolve");
    }
    if (!top.validateSemanticTree().empty() || !child.validateSemanticTree().empty())
        return fail("XMR port insertion left invalid Node members or anchors");
    const auto anchors = child.semanticNodeAnchors(childWriteNode);
    const auto members = child.semanticNodeOperations(childWriteNode);
    if (anchors.size() != 1 || anchors.front() != child.findValue("leaf_w") ||
        members.size() != 1 || child.opKind(members.front()) != wolvrix::lib::grh::OperationKind::kAssign)
        return fail("plain XMR write did not move the target Node anchor and assign membership");
    if (top.semanticNodeAnchors(readNode).size() != 1 || !top.valueSemanticIds(readValue).empty())
        return fail("XMR read retained its obsolete anchor");

    return 0;
}
