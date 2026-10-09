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

    // Case 7: register port family on an array register (stage-03 semantics).
    {
        Design design;
        Graph &graph = design.createGraph("ports_ok");
        OperationId reg = addRegister(graph, "reg_arr", 128);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{4, 4}));
        // element = 8 bit; sub-array at k=1 = 32 bit shape [4]
        ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
        ValueId en = graph.createValue(graph.internSymbol("en"), 1, false);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId idx2 = graph.createValue(graph.internSymbol("idx2"), 4, false);
        const std::vector<int64_t> wholeShape{4, 4};
        ValueId whole = graph.createValue(graph.internSymbol("whole"), 128, false,
                                          ValueType::Logic, wholeShape);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("read"));
        graph.addResult(read, whole);
        graph.setAttr(read, "regSymbol", std::string("reg_arr"));

        ValueId elem = graph.createValue(graph.internSymbol("elem"), 8, false);
        OperationId readElem = graph.createOperation(OperationKind::kRegisterReadArrayPort,
                                                     graph.internSymbol("read_elem"));
        graph.addOperand(readElem, idx);
        graph.addOperand(readElem, idx2);
        graph.addResult(readElem, elem);
        graph.setAttr(readElem, "regSymbol", std::string("reg_arr"));

        const std::vector<int64_t> rowShape{4};
        ValueId row = graph.createValue(graph.internSymbol("row"), 32, false, ValueType::Logic,
                                        rowShape);
        OperationId readRow = graph.createOperation(OperationKind::kRegisterReadArrayPort,
                                                    graph.internSymbol("read_row"));
        graph.addOperand(readRow, idx);
        graph.addResult(readRow, row);
        graph.setAttr(readRow, "regSymbol", std::string("reg_arr"));

        OperationId writeRow = graph.createOperation(OperationKind::kRegisterWriteArrayPort,
                                                     graph.internSymbol("write_row"));
        graph.addOperand(writeRow, en);
        graph.addOperand(writeRow, idx);
        graph.addOperand(writeRow, row);
        graph.addOperand(writeRow, clk);
        graph.setAttr(writeRow, "regSymbol", std::string("reg_arr"));
        graph.setAttr(writeRow, "eventEdge", std::vector<std::string>{"posedge"});

        ValueId mask8 = graph.createValue(graph.internSymbol("mask8"), 8, false);
        OperationId writeElem = graph.createOperation(OperationKind::kRegisterWriteArrayMaskPort,
                                                      graph.internSymbol("write_elem"));
        graph.addOperand(writeElem, en);
        graph.addOperand(writeElem, idx);
        graph.addOperand(writeElem, idx2);
        graph.addOperand(writeElem, elem);
        graph.addOperand(writeElem, mask8);
        graph.addOperand(writeElem, clk);
        graph.setAttr(writeElem, "regSymbol", std::string("reg_arr"));
        graph.setAttr(writeElem, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId fill = graph.createOperation(OperationKind::kRegisterFillPort,
                                                 graph.internSymbol("fill"));
        graph.addOperand(fill, en);
        graph.addOperand(fill, elem);
        graph.addOperand(fill, clk);
        graph.setAttr(fill, "regSymbol", std::string("reg_arr"));
        graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});

        ValueId mask128 = graph.createValue(graph.internSymbol("mask128"), 128, false);
        OperationId writeMask = graph.createOperation(OperationKind::kRegisterWriteMaskPort,
                                                      graph.internSymbol("write_mask"));
        graph.addOperand(writeMask, en);
        graph.addOperand(writeMask, whole);
        graph.addOperand(writeMask, mask128);
        graph.addOperand(writeMask, clk);
        graph.setAttr(writeMask, "regSymbol", std::string("reg_arr"));
        graph.setAttr(writeMask, "eventEdge", std::vector<std::string>{"posedge"});

        OperationId writeWhole = graph.createOperation(OperationKind::kRegisterWritePort,
                                                       graph.internSymbol("write_whole"));
        graph.addOperand(writeWhole, en);
        graph.addOperand(writeWhole, whole);
        graph.addOperand(writeWhole, clk);
        graph.setAttr(writeWhole, "regSymbol", std::string("reg_arr"));
        graph.setAttr(writeWhole, "eventEdge", std::vector<std::string>{"posedge"});
        if (!runVerify(design))
        {
            return fail("valid register port family graph failed verification");
        }
    }

    // Case 8: read array port with more indices than the register rank is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("read_over_rank");
        OperationId reg = addRegister(graph, "reg_arr", 32);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{4}));
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId elem = graph.createValue(graph.internSymbol("elem"), 8, false);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadArrayPort,
                                                 graph.internSymbol("read"));
        graph.addOperand(read, idx);
        graph.addOperand(read, idx);
        graph.addResult(read, elem);
        graph.setAttr(read, "regSymbol", std::string("reg_arr"));
        if (runVerify(design))
        {
            return fail("kRegisterReadArrayPort with more indices than rank passed verification");
        }
    }

    // Case 9: array write port whose data width does not match the sub-array is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("write_bad_width");
        OperationId reg = addRegister(graph, "reg_arr", 32);
        graph.setAttr(reg, "shape", AttributeValue(std::vector<int64_t>{4}));
        ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
        ValueId en = graph.createValue(graph.internSymbol("en"), 1, false);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId data = graph.createValue(graph.internSymbol("data"), 16, false);
        OperationId write = graph.createOperation(OperationKind::kRegisterWriteArrayPort,
                                                  graph.internSymbol("write"));
        graph.addOperand(write, en);
        graph.addOperand(write, idx);
        graph.addOperand(write, data);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("reg_arr"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        if (runVerify(design))
        {
            return fail("kRegisterWriteArrayPort with mismatched data width passed verification");
        }
    }

    // Case 10: a write port with a multi-bit updateCond is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("write_bad_cond");
        OperationId reg = addRegister(graph, "reg0", 8);
        ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
        ValueId en = graph.createValue(graph.internSymbol("en"), 2, false);
        ValueId data = graph.createValue(graph.internSymbol("data"), 8, false);
        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("write"));
        graph.addOperand(write, en);
        graph.addOperand(write, data);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("reg0"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        if (runVerify(design))
        {
            return fail("kRegisterWritePort with a multi-bit updateCond passed verification");
        }
    }

    // Case 11: a write port without an eventEdge attr is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("write_no_events");
        OperationId reg = addRegister(graph, "reg0", 8);
        ValueId en = graph.createValue(graph.internSymbol("en"), 1, false);
        ValueId data = graph.createValue(graph.internSymbol("data"), 8, false);
        OperationId write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("write"));
        graph.addOperand(write, en);
        graph.addOperand(write, data);
        graph.setAttr(write, "regSymbol", std::string("reg0"));
        if (runVerify(design))
        {
            return fail("kRegisterWritePort without eventEdge passed verification");
        }
    }

    // Case 12: a port that references an unknown register is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("read_unknown_reg");
        ValueId value = graph.createValue(graph.internSymbol("v"), 8, false);
        OperationId read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                 graph.internSymbol("read"));
        graph.addResult(read, value);
        graph.setAttr(read, "regSymbol", std::string("missing_reg"));
        if (runVerify(design))
        {
            return fail("kRegisterReadPort with an unknown regSymbol passed verification");
        }
    }

    // Case 13: an array write port on a scalar register is rejected.
    {
        Design design;
        Graph &graph = design.createGraph("array_write_scalar");
        OperationId reg = addRegister(graph, "reg0", 8);
        ValueId clk = graph.createValue(graph.internSymbol("clk"), 1, false);
        ValueId en = graph.createValue(graph.internSymbol("en"), 1, false);
        ValueId idx = graph.createValue(graph.internSymbol("idx"), 4, false);
        ValueId data = graph.createValue(graph.internSymbol("data"), 8, false);
        OperationId write = graph.createOperation(OperationKind::kRegisterWriteArrayPort,
                                                  graph.internSymbol("write"));
        graph.addOperand(write, en);
        graph.addOperand(write, idx);
        graph.addOperand(write, data);
        graph.addOperand(write, clk);
        graph.setAttr(write, "regSymbol", std::string("reg0"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        if (runVerify(design))
        {
            return fail("kRegisterWriteArrayPort on a scalar register passed verification");
        }
    }

    return 0;
}
