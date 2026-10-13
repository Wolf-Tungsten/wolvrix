#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/slice_index_const.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <span>
#include <string>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    ValueId constant(Graph &graph, const char *name, int32_t width, const char *literal)
    {
        const ValueId value = graph.createValue(graph.internSymbol(name), width, false);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                    graph.internSymbol(std::string(name) + "_constant"));
        graph.setAttr(op, "constValue", std::string(literal));
        graph.addResult(op, value);
        return value;
    }

    int fail(const char *message)
    {
        std::cerr << "[slice-index-const] " << message << '\n';
        return 1;
    }
}

int main()
{
    Design design;
    Graph &graph = design.createGraph("g");
    const std::array<int64_t, 2> shape{2, 3};
    const ValueId array = graph.createValue(graph.internSymbol("array"), 48, false,
                                            ValueType::Logic, shape);
    graph.bindInputPort("array", array);
    const ValueId first = constant(graph, "first", 2, "1");
    const ValueId second = constant(graph, "second", 2, "2");

    const ValueId subarray = graph.createValue(graph.internSymbol("subarray"), 24, false,
                                               ValueType::Logic, std::span<const int64_t>(shape).subspan(1));
    const OperationId partial = graph.createOperation(OperationKind::kSliceArray,
                                                      graph.internSymbol("partial"));
    graph.addOperand(partial, array);
    graph.addOperand(partial, first);
    graph.addResult(partial, subarray);

    const ValueId element = graph.createValue(graph.internSymbol("element"), 8, false);
    const OperationId full = graph.createOperation(OperationKind::kSliceArray,
                                                   graph.internSymbol("full"));
    graph.addOperand(full, array);
    graph.addOperand(full, first);
    graph.addOperand(full, second);
    graph.addResult(full, element);

    const ValueId packed = graph.createValue(graph.internSymbol("packed"), 32, false);
    graph.bindInputPort("packed", packed);
    const ValueId offset = constant(graph, "offset", 5, "8");
    const ValueId byte = graph.createValue(graph.internSymbol("byte"), 8, false);
    const OperationId bitSlice = graph.createOperation(OperationKind::kSliceDynamic,
                                                       graph.internSymbol("bit_slice"));
    graph.addOperand(bitSlice, packed);
    graph.addOperand(bitSlice, offset);
    graph.setAttr(bitSlice, "sliceWidth", int64_t{8});
    graph.addResult(bitSlice, byte);

    PassManager manager;
    manager.addPass(std::make_unique<SliceIndexConstPass>());
    PassDiagnostics diagnostics;
    const PassManagerResult result = manager.run(design, diagnostics);
    if (!result.success || diagnostics.hasError() || !result.changed)
        return fail("pass did not fold the constant bit offset");
    const Operation partialOp = graph.getOperation(partial);
    const Operation fullOp = graph.getOperation(full);
    if (partialOp.kind() != OperationKind::kSliceArray ||
        partialOp.operands().size() != 2 || partialOp.operands()[0] != array ||
        partialOp.operands()[1] != first ||
        fullOp.kind() != OperationKind::kSliceArray ||
        fullOp.operands().size() != 3 || fullOp.operands()[0] != array ||
        fullOp.operands()[1] != first || fullOp.operands()[2] != second)
        return fail("constant array indices lost their array access");
    if (graph.valueShape(array).size() != 2 || graph.valueShape(array)[0] != 2 ||
        graph.valueShape(array)[1] != 3 || graph.valueShape(subarray).size() != 1 ||
        graph.valueShape(subarray)[0] != 3 || !graph.valueShape(element).empty())
        return fail("constant array indices lost their array shape");
    const Operation bitOp = graph.getOperation(bitSlice);
    if (bitOp.kind() != OperationKind::kSliceStatic || bitOp.operands().size() != 1 ||
        bitOp.attr("sliceStart") != AttributeValue(int64_t{8}) ||
        bitOp.attr("sliceEnd") != AttributeValue(int64_t{15}))
        return fail("constant bit offset did not become a static range");

    PassDiagnostics secondDiagnostics;
    const PassManagerResult secondResult = manager.run(design, secondDiagnostics);
    if (!secondResult.success || secondDiagnostics.hasError() || secondResult.changed)
        return fail("second pass run should not change the graph");
    return 0;
}
