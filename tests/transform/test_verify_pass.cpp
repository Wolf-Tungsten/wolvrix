#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/verify.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grh_verify_tests] " << message << '\n';
        return 1;
    }

    bool runVerify(Design &design)
    {
        PassManager manager;
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        for (const auto &message : diags.messages())
        {
            std::cerr << "[grh_verify_tests] diag: " << message.message << " | " << message.context
                      << '\n';
        }
        return result.success && !diags.hasError();
    }

    OperationId addRegister(Graph &graph, const std::string &name, int64_t width)
    {
        OperationId op = graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
        graph.setAttr(op, "width", width);
        graph.setAttr(op, "isSigned", false);
        return op;
    }

} // namespace

int main()
{
    // Case 1: array-shaped register and values pass verification.
    {
        Design design;
        Graph &graph = design.createGraph("ok");
        OperationId reg = addRegister(graph, "reg_arr", 32);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{4}));
        const std::vector<int64_t> shape{4};
        ValueId arr = graph.createValue(graph.internSymbol("arr"), 32, false, ValueType::Logic, shape);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("read_arr"));
        graph.addResult(read, arr);
        graph.setAttr(read, "regSymbol", std::string("reg_arr"));
        if (!runVerify(design))
        {
            return fail("valid array-shaped graph failed verification");
        }
    }

    // Case 2: register shape attr that does not divide the width is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("bad_reg");
        OperationId reg = addRegister(graph, "reg_bad", 32);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{3}));
        if (runVerify(design))
        {
            return fail("register with non-dividing shape passed verification");
        }
    }

    // Case 3: kLatch must not carry a shape attr.
    {
        Design design;
        Graph &graph = design.createGraph("bad_latch");
        OperationId latch = graph.createOperation(OperationKind::kLatch, graph.internSymbol("latch0"));
        graph.setAttr(latch, "width", int64_t(8));
        graph.setAttr(latch, "isSigned", false);
        graph.setAttr(latch, "shape", AttributeValue(std::vector<int64_t>{2}));
        if (runVerify(design))
        {
            return fail("latch with shape attr passed verification");
        }
    }

    // Case 4: invalid value shapes are rejected at construction time.
    {
        Design design;
        Graph &graph = design.createGraph("ctor");
        bool threw = false;
        try
        {
            const std::vector<int64_t> badShape{3};
            graph.createValue(graph.internSymbol("bad"), 32, false, ValueType::Logic, badShape);
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        if (!threw)
        {
            return fail("createValue accepted a shape that does not divide the width");
        }
        threw = false;
        try
        {
            const std::vector<int64_t> realShape{2};
            graph.createValue(graph.internSymbol("bad_real"), 32, false, ValueType::Real, realShape);
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        if (!threw)
        {
            return fail("createValue accepted a shape on a non-Logic value");
        }
    }

    // Case 5: instance port width mismatch is an error, shape mismatch a warning.
    {
        Design design;
        Graph &child = design.createGraph("child");
        const std::vector<int64_t> childShape{4};
        ValueId childIn =
            child.createValue(child.internSymbol("in"), 32, false, ValueType::Logic, childShape);
        child.bindInputPort("in", childIn);

        Graph &parent = design.createGraph("parent");
        ValueId parentIn = parent.createValue(parent.internSymbol("parent_in"), 32, false);
        OperationId inst = parent.createOperation(OperationKind::kInstance, parent.internSymbol("u_child"));
        parent.setAttr(inst, "moduleName", std::string("child"));
        parent.setAttr(inst, "instanceName", std::string("u_child"));
        parent.setAttr(inst, "inputPortName", std::vector<std::string>{"in"});
        parent.setAttr(inst, "outputPortName", std::vector<std::string>{});
        parent.addOperand(inst, parentIn);

        PassManager manager;
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);
        if (!result.success)
        {
            return fail("instance with matching widths failed verification");
        }
        bool sawShapeWarning = false;
        for (const auto &message : diags.messages())
        {
            if (message.kind == wolvrix::lib::diag::DiagnosticKind::Warning &&
                message.message.find("shape mismatch") != std::string::npos)
            {
                sawShapeWarning = true;
            }
        }
        if (!sawShapeWarning)
        {
            return fail("instance shape mismatch did not produce a warning");
        }
    }

    // Case 6: kSliceArray shape rules (stage-02 semantics).
    {
        // Valid: k=1 on rank-2 base -> sub-array with remaining shape.
        Design design;
        Graph &graph = design.createGraph("slice_ok");
        OperationId reg = addRegister(graph, "reg_arr", 64);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{2, 4}));
        const std::vector<int64_t> baseShape{2, 4};
        ValueId arr = graph.createValue(graph.internSymbol("arr"), 64, false, ValueType::Logic,
                                        baseShape);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("read_arr"));
        graph.addResult(read, arr);
        graph.setAttr(read, "regSymbol", std::string("reg_arr"));
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        const std::vector<int64_t> subShape{4};
        ValueId row = graph.createValue(graph.internSymbol("row"), 32, false, ValueType::Logic,
                                        subShape);
        OperationId sliceRow = graph.createOperation(OperationKind::kSliceArray,
                                                     graph.internSymbol("slice_row"));
        graph.addOperand(sliceRow, arr);
        graph.addOperand(sliceRow, idx);
        graph.addResult(sliceRow, row);
        if (!runVerify(design))
        {
            return fail("valid kSliceArray graph failed verification");
        }
    }
    {
        // Invalid: base without shape.
        Design design;
        Graph &graph = design.createGraph("slice_scalar_base");
        ValueId flat = graph.createValue(graph.internSymbol("flat"), 32, false);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId elem = graph.createValue(graph.internSymbol("elem"), 8, false);
        OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                  graph.internSymbol("slice"));
        graph.addOperand(slice, flat);
        graph.addOperand(slice, idx);
        graph.addResult(slice, elem);
        if (runVerify(design))
        {
            return fail("kSliceArray on a scalar base passed verification");
        }
    }
    {
        // Invalid: more indices than the base rank.
        Design design;
        Graph &graph = design.createGraph("slice_over_rank");
        const std::vector<int64_t> baseShape{4};
        ValueId arr = graph.createValue(graph.internSymbol("arr"), 32, false, ValueType::Logic,
                                        baseShape);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId elem = graph.createValue(graph.internSymbol("elem"), 8, false);
        OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                  graph.internSymbol("slice"));
        graph.addOperand(slice, arr);
        graph.addOperand(slice, idx);
        graph.addOperand(slice, idx);
        graph.addResult(slice, elem);
        if (runVerify(design))
        {
            return fail("kSliceArray with more indices than rank passed verification");
        }
    }
    {
        // Invalid: result shape does not match the remaining dimensions.
        Design design;
        Graph &graph = design.createGraph("slice_bad_result");
        const std::vector<int64_t> baseShape{2, 4};
        ValueId arr = graph.createValue(graph.internSymbol("arr"), 64, false, ValueType::Logic,
                                        baseShape);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        const std::vector<int64_t> wrongShape{2, 2};
        ValueId row = graph.createValue(graph.internSymbol("row"), 32, false, ValueType::Logic,
                                        wrongShape);
        OperationId slice = graph.createOperation(OperationKind::kSliceArray,
                                                  graph.internSymbol("slice"));
        graph.addOperand(slice, arr);
        graph.addOperand(slice, idx);
        graph.addResult(slice, row);
        if (runVerify(design))
        {
            return fail("kSliceArray with mismatched result shape passed verification");
        }
    }

    return 0;
}
