// originModules attribution: flatten stamping (incl. top ops and synthetic
// assigns), CSE signature exclusion + merge union, const pool dedup union.
#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/const_fold.hpp"
#include "transform/hier_flatten.hpp"
#include "transform/redundant_elim.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[transform-origin-attribution] " << message << '\n';
    return 1;
}

std::vector<std::string> originsOf(const wolvrix::lib::grh::Graph &graph,
                                   wolvrix::lib::grh::OperationId op)
{
    return getOriginModules(graph, op).value_or(std::vector<std::string>{});
}

bool hasOrigin(const std::vector<std::string> &origins, std::string_view module)
{
    return std::find(origins.begin(), origins.end(), module) != origins.end();
}

std::string joinOrigins(const std::vector<std::string> &origins)
{
    std::string out;
    for (const std::string &origin : origins)
    {
        if (!out.empty())
        {
            out.push_back(',');
        }
        out.append(origin);
    }
    return out;
}

void addLeafLogic(wolvrix::lib::grh::Graph &child, wolvrix::lib::grh::ValueId in,
                  wolvrix::lib::grh::ValueId out)
{
    // Constants are pooled/deduped by const-fold; distinct literals force
    // separate constant ops so the constant-op count stays predictable.
    const auto constOne = child.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                child.makeInternalOpSym());
    const auto constOneVal = child.createValue(child.makeInternalValSym(), 8, false);
    child.addResult(constOne, constOneVal);
    child.setAttr(constOne, "constValue", std::string("8'd1"));

    const auto add = child.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                           child.makeInternalOpSym());
    child.addOperand(add, in);
    child.addOperand(add, constOneVal);
    const auto sum = child.createValue(child.makeInternalValSym(), 8, false);
    child.addResult(add, sum);

    const auto assign = child.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                              child.makeInternalOpSym());
    child.addOperand(assign, sum);
    child.addResult(assign, out);
}

// child: in -> add(+1) -> out; top: glue invert into two child instances.
void buildTwoInstanceDesign(wolvrix::lib::grh::Design &design)
{
    wolvrix::lib::grh::Graph &child = design.createGraph("child");
    wolvrix::lib::grh::Graph &top = design.createGraph("top");
    design.markAsTop("top");

    const auto childIn = child.createValue(child.internSymbol("in"), 8, false);
    const auto childOut = child.createValue(child.internSymbol("out"), 8, false);
    child.bindInputPort("in", childIn);
    child.bindOutputPort("out", childOut);
    addLeafLogic(child, childIn, childOut);

    const auto topIn = top.createValue(top.internSymbol("tin"), 8, false);
    const auto topOut1 = top.createValue(top.internSymbol("tout1"), 8, false);
    const auto topOut2 = top.createValue(top.internSymbol("tout2"), 8, false);
    const auto inv = top.createValue(top.makeInternalValSym(), 8, false);
    const auto link1 = top.createValue(top.makeInternalValSym(), 8, false);
    const auto link2 = top.createValue(top.makeInternalValSym(), 8, false);
    top.bindInputPort("tin", topIn);
    top.bindOutputPort("tout1", topOut1);
    top.bindOutputPort("tout2", topOut2);

    const auto invOp = top.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                           top.makeInternalOpSym());
    top.addOperand(invOp, topIn);
    top.addResult(invOp, inv);

    const auto instA = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                           top.internSymbol("_op_a"));
    top.addOperand(instA, inv);
    top.addResult(instA, link1);
    top.setAttr(instA, "moduleName", std::string("child"));
    top.setAttr(instA, "instanceName", std::string("u_a"));

    const auto instB = top.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                           top.internSymbol("_op_b"));
    top.addOperand(instB, inv);
    top.addResult(instB, link2);
    top.setAttr(instB, "moduleName", std::string("child"));
    top.setAttr(instB, "instanceName", std::string("u_b"));

    const auto drive1 = top.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                            top.makeInternalOpSym());
    top.addOperand(drive1, link1);
    top.addResult(drive1, topOut1);
    const auto drive2 = top.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                            top.makeInternalOpSym());
    top.addOperand(drive2, link2);
    top.addResult(drive2, topOut2);
}

int testFlattenStamping()
{
    wolvrix::lib::grh::Design design;
    buildTwoInstanceDesign(design);

    PassManager manager;
    manager.addPass(std::make_unique<HierFlattenPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("flatten run failed");
    }
    if (design.graphs().size() != 1)
    {
        return fail("expected a single graph after flatten");
    }
    wolvrix::lib::grh::Graph &top = *design.findGraph("top");

    std::size_t topOps = 0;
    std::size_t uaOps = 0;
    std::size_t ubOps = 0;
    for (const auto opId : top.operations())
    {
        if (!opId.valid())
        {
            continue;
        }
        const std::vector<std::string> origins = originsOf(top, opId);
        if (origins.empty())
        {
            return fail("op missing originModules after flatten");
        }
        for (const std::string &origin : origins)
        {
            if (origin == "top")
            {
                ++topOps;
            }
            else if (origin == "u_a")
            {
                ++uaOps;
            }
            else if (origin == "u_b")
            {
                ++ubOps;
            }
            else
            {
                return fail("unexpected origin entry: " + origin);
            }
        }
    }
    // Two pre-existing top assigns + the glue invert.
    if (topOps != 3)
    {
        return fail("expected 3 top-stamped ops, got " + std::to_string(topOps));
    }
    // child has 3 ops (const/add/assign) cloned per instance.
    if (uaOps != 3 || ubOps != 3)
    {
        return fail("expected 3 ops per instance, got u_a=" + std::to_string(uaOps) +
                    " u_b=" + std::to_string(ubOps));
    }
    return 0;
}

// One top graph with two structurally identical adds stamped with different
// origins, plus a duplicate constant pair for the pool union check.
int testCseUnionAndPool()
{
    wolvrix::lib::grh::Design design;
    wolvrix::lib::grh::Graph &graph = design.createGraph("top");
    design.markAsTop("top");

    const auto in = graph.createValue(graph.internSymbol("in"), 8, false);
    const auto out = graph.createValue(graph.internSymbol("out"), 8, false);
    graph.bindInputPort("in", in);
    graph.bindOutputPort("out", out);

    const auto constA = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.makeInternalOpSym());
    const auto constValA = graph.createValue(graph.makeInternalValSym(), 8, false);
    graph.addResult(constA, constValA);
    graph.setAttr(constA, "constValue", std::string("8'd1"));
    graph.setAttr(constA, std::string(kOriginModulesKey), std::vector<std::string>{"u_a"});

    const auto constB = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.makeInternalOpSym());
    const auto constValB = graph.createValue(graph.makeInternalValSym(), 8, false);
    graph.addResult(constB, constValB);
    graph.setAttr(constB, "constValue", std::string("8'd1"));
    graph.setAttr(constB, std::string(kOriginModulesKey), std::vector<std::string>{"u_b"});

    const auto addA = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                            graph.makeInternalOpSym());
    graph.addOperand(addA, in);
    graph.addOperand(addA, constValA);
    const auto sumA = graph.createValue(graph.makeInternalValSym(), 8, false);
    graph.addResult(addA, sumA);
    graph.setAttr(addA, std::string(kOriginModulesKey), std::vector<std::string>{"u_a"});

    const auto addB = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                            graph.makeInternalOpSym());
    graph.addOperand(addB, in);
    graph.addOperand(addB, constValB);
    const auto sumB = graph.createValue(graph.makeInternalValSym(), 8, false);
    graph.addResult(addB, sumB);
    graph.setAttr(addB, std::string(kOriginModulesKey), std::vector<std::string>{"u_b"});

    const auto sink = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                            graph.makeInternalOpSym());
    graph.addOperand(sink, sumA);
    graph.addOperand(sink, sumB);
    graph.addResult(sink, out);
    graph.setAttr(sink, std::string(kOriginModulesKey), std::vector<std::string>{"top"});

    auto opCount = [&]() {
        std::size_t count = 0;
        for (const auto opId : graph.operations())
        {
            if (opId.valid())
            {
                ++count;
            }
        }
        return count;
    };
    const std::size_t beforeCount = opCount();
    if (beforeCount != 5)
    {
        return fail("test setup: expected 5 ops, got " + std::to_string(beforeCount));
    }

    PassManager manager;
    manager.addPass(std::make_unique<ConstantFoldPass>());
    manager.addPass(std::make_unique<RedundantElimPass>());
    PassDiagnostics diags;
    const PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError())
    {
        return fail("const-fold/redundant-elim run failed");
    }

    // const-fold dedups one constant; CSE must still merge the two adds even
    // though their originModules differ (u_a vs u_b).
    const std::size_t afterCount = opCount();
    if (afterCount != 3)
    {
        return fail("expected 3 ops after dedup+CSE (const, add, sink), got " +
                    std::to_string(afterCount));
    }

    std::optional<std::vector<std::string>> addOrigins;
    std::optional<std::vector<std::string>> constOrigins;
    for (const auto opId : graph.operations())
    {
        if (!opId.valid())
        {
            continue;
        }
        if (graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kConstant)
        {
            constOrigins = originsOf(graph, opId);
        }
        else if (graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kAdd &&
                 opId != sink)
        {
            addOrigins = originsOf(graph, opId);
        }
    }
    if (!addOrigins || !hasOrigin(*addOrigins, "u_a") || !hasOrigin(*addOrigins, "u_b") ||
        addOrigins->size() != 2)
    {
        return fail("surviving add must carry {u_a,u_b}, got {" +
                    (addOrigins ? joinOrigins(*addOrigins) : std::string("<none>")) + "}");
    }
    if (!constOrigins || !hasOrigin(*constOrigins, "u_a") || !hasOrigin(*constOrigins, "u_b") ||
        constOrigins->size() != 2)
    {
        return fail("pooled constant must carry {u_a,u_b}, got {" +
                    (constOrigins ? joinOrigins(*constOrigins) : std::string("<none>")) + "}");
    }
    return 0;
}

} // namespace

int main()
{
    if (const int rc = testFlattenStamping())
    {
        return rc;
    }
    if (const int rc = testCseUnionAndPool())
    {
        return rc;
    }
    return 0;
}
