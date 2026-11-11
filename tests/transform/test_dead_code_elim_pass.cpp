#include "core/grh.hpp"
#include "transform/dead_code_elim.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[dead-code-elim-tests] " << message << '\n';
        return 1;
    }

    template <typename T>
    const T *getSessionValue(const SessionStore &session, std::string_view key)
    {
        auto it = session.find(std::string(key));
        if (it == session.end())
        {
            return nullptr;
        }
        auto *typed = dynamic_cast<const SessionSlotValue<T> *>(it->second.get());
        if (typed == nullptr)
        {
            return nullptr;
        }
        return &typed->value;
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
        const wolvrix::lib::grh::OperationId op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant, opSym);
        graph.addResult(op, val);
        graph.setAttr(op, "constValue", literal);
        return val;
    }

    wolvrix::lib::grh::OperationId makeRegisterDecl(wolvrix::lib::grh::Graph &graph,
                                                    const std::string &sym)
    {
        return graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister, graph.internSymbol(sym));
    }

    wolvrix::lib::grh::ValueId makeRegisterRead(wolvrix::lib::grh::Graph &graph,
                                                const std::string &regSym,
                                                const std::string &valueName,
                                                const std::string &opName,
                                                int32_t width,
                                                bool isSigned)
    {
        const auto valueSym = graph.internSymbol(valueName);
        const auto opSym = graph.internSymbol(opName);
        const auto val = graph.createValue(valueSym, width, isSigned);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort, opSym);
        graph.addResult(op, val);
        graph.setAttr(op, "regSymbol", regSym);
        return val;
    }

    wolvrix::lib::grh::OperationId makeRegisterWrite(wolvrix::lib::grh::Graph &graph,
                                                     const std::string &regSym,
                                                     const std::string &opName,
                                                     const std::vector<wolvrix::lib::grh::ValueId> &operands)
    {
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                              graph.internSymbol(opName));
        for (const auto operand : operands)
        {
            graph.addOperand(op, operand);
        }
        graph.setAttr(op, "regSymbol", regSym);
        return op;
    }

    wolvrix::lib::grh::ValueId makeArray(wolvrix::lib::grh::Graph &graph,
                                        const std::string &name,
                                        const std::vector<wolvrix::lib::grh::ValueId> &elements)
    {
        namespace grh = wolvrix::lib::grh;
        const std::vector<int64_t> shape{static_cast<int64_t>(elements.size())};
        const auto value = graph.createValue(graph.internSymbol(name),
            static_cast<int32_t>(elements.size() * graph.valueWidth(elements.front())), false,
            grh::ValueType::Logic, shape);
        const auto op = graph.createOperation(grh::OperationKind::kArrayCreate,
                                              graph.internSymbol(name + "_op"));
        for (const auto element : elements) graph.addOperand(op, element);
        graph.addResult(op, value);
        return value;
    }

    wolvrix::lib::grh::ValueId makeSlice(wolvrix::lib::grh::Graph &graph,
                                        const std::string &name,
                                        wolvrix::lib::grh::ValueId source,
                                        int64_t start, int32_t width)
    {
        namespace grh = wolvrix::lib::grh;
        const auto value = graph.createValue(graph.internSymbol(name), width, false);
        const auto op = graph.createOperation(grh::OperationKind::kSliceStatic,
                                              graph.internSymbol(name + "_op"));
        graph.addOperand(op, source);
        graph.addResult(op, value);
        graph.setAttr(op, "sliceStart", start);
        graph.setAttr(op, "sliceEnd", start + width - 1);
        return value;
    }

} // namespace

int main()
{
    for (const bool sourceSigned : {false, true})
    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("array_index_signedness_alias");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 16; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        const auto source = makeConst(graph, "index_source", "index_source_op", 8,
            sourceSigned, sourceSigned ? "4'hf" : "4'shf");
        const auto index = graph.createValue(graph.internSymbol("index"), 8, !sourceSigned);
        const auto assign = graph.createOperation(grh::OperationKind::kAssign,
                                                  graph.internSymbol("index_op"));
        graph.addOperand(assign, source);
        graph.addResult(assign, index);
        const auto output = graph.createValue(graph.internSymbol("output"), 8, false);
        const auto slice = graph.createOperation(grh::OperationKind::kSliceArray,
                                                 graph.internSymbol("output_op"));
        graph.addOperand(slice, array);
        graph.addOperand(slice, index);
        graph.addResult(slice, output);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a signedness-changing array index assign");
        // Producer projection creates 8'hff for a signed source, then the
        // unsigned assign sees index 255 (out of range). An unsigned source
        // becomes 8'h0f, so the signed assign still selects element 15.
        for (int64_t i = 0; i < 16; ++i) {
            const bool expected = sourceSigned || i == 15;
            if (graph.findOperation("element" + std::to_string(i) + "_op").valid() != expected)
                return fail("DCE sized an index literal using its assign destination's signedness");
        }
    }

    struct IndexCase {
        const char *literal;
        int32_t width;
        bool isSigned;
        int64_t selected;
    };
    for (const auto indexCase : {
             IndexCase{"4'b1_0", 4, false, 2},
             IndexCase{"8'h12", 2, false, 2},
             IndexCase{"4'sh2", 4, true, 2},
             IndexCase{"2", 4, false, 2},
             IndexCase{"'1", 1, false, 1},
             IndexCase{"4'bx0_10", 4, false, -1},
             IndexCase{"4'shf", 4, true, -1},
             IndexCase{"4'd7", 4, false, -1}})
    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("array_index_literal");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        const auto index = makeConst(graph, "index", "index_op", indexCase.width,
                                      indexCase.isSigned, indexCase.literal);
        const auto output = graph.createValue(graph.internSymbol("output"), 8, false);
        const auto slice = graph.createOperation(grh::OperationKind::kSliceArray,
                                                 graph.internSymbol("output_op"));
        graph.addOperand(slice, array);
        graph.addOperand(slice, index);
        graph.addResult(slice, output);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on array index literal " + std::string(indexCase.literal));
        for (int64_t i = 0; i < 3; ++i) {
            const bool expected = indexCase.selected < 0 || i == indexCase.selected;
            if (graph.findOperation("element" + std::to_string(i) + "_op").valid() != expected)
                return fail("DCE projected array index literal incorrectly: " + std::string(indexCase.literal));
        }
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("fill_partial_storage");
        const auto data = makeConst(graph, "data", "data_op", 8, false, "8'd83");
        const auto enable = makeConst(graph, "enable", "enable_op", 1, false, "1'b1");
        const auto clock = makeConst(graph, "clock", "clock_op", 1, false, "1'b0");
        const auto state = makeRegisterDecl(graph, "state");
        graph.setAttr(state, "width", int64_t{24});
        graph.setAttr(state, "isSigned", false);
        graph.setAttr(state, "shape", std::vector<int64_t>{3});
        const auto fill = graph.createOperation(grh::OperationKind::kRegisterFillPort,
                                                 graph.internSymbol("fill"));
        for (const auto operand : {enable, data, clock}) graph.addOperand(fill, operand);
        graph.setAttr(fill, "regSymbol", std::string("state"));
        graph.setAttr(fill, "eventEdge", std::vector<std::string>{"posedge"});
        const auto read = makeRegisterRead(graph, "state", "read", "read_op", 24, false);
        const auto output = makeSlice(graph, "output", read, 20, 4);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !graph.findOperation("data_op"))
            return fail("whole-storage demand was mistaken for broadcast-fill local coordinates");
    }

    for (const bool dynamicIndex : {false, true})
    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("indexed_read_partial_result");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto data = makeArray(graph, "array", elements);
        const auto enable = makeConst(graph, "enable", "enable_op", 1, false, "1'b1");
        const auto clock = makeConst(graph, "clock", "clock_op", 1, false, "1'b0");
        const auto state = makeRegisterDecl(graph, "state");
        graph.setAttr(state, "width", int64_t{24});
        graph.setAttr(state, "isSigned", false);
        graph.setAttr(state, "shape", std::vector<int64_t>{3});
        (void)makeRegisterWrite(graph, "state", "write", {enable, data, clock});
        const auto index = dynamicIndex ? graph.createValue(graph.internSymbol("index"), 2, false) :
            makeConst(graph, "index", "index_op", 2, false, "2'd2");
        if (dynamicIndex) graph.bindInputPort("index", index);
        const auto read = graph.createValue(graph.internSymbol("read"), 8, false);
        const auto readOp = graph.createOperation(grh::OperationKind::kRegisterReadArrayPort,
                                                  graph.internSymbol("read_op"));
        graph.addOperand(readOp, index);
        graph.addResult(readOp, read);
        graph.setAttr(readOp, "regSymbol", std::string("state"));
        const auto output = makeSlice(graph, "output", read, 4, 4);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a partial indexed-read result");
        for (int64_t i = 0; i < 3; ++i)
            if (!graph.findOperation("element" + std::to_string(i) + "_op"))
                return fail("indexed-read local demand was mistaken for whole-storage coordinates");
    }

    for (const bool dynamicIndex : {false, true})
    for (const bool masked : {false, true})
    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("indexed_write_partial_storage");
        const auto data = makeConst(graph, "data", "data_op", 8, false, "8'd83");
        const auto enable = makeConst(graph, "enable", "enable_op", 1, false, "1'b1");
        const auto clock = makeConst(graph, "clock", "clock_op", 1, false, "1'b0");
        const auto state = makeRegisterDecl(graph, "state");
        graph.setAttr(state, "width", int64_t{24});
        graph.setAttr(state, "isSigned", false);
        graph.setAttr(state, "shape", std::vector<int64_t>{3});
        const auto index = dynamicIndex ? graph.createValue(graph.internSymbol("index"), 2, false) :
            makeConst(graph, "index", "index_op", 2, false, "2'd2");
        if (dynamicIndex) graph.bindInputPort("index", index);
        const auto write = graph.createOperation(masked ? grh::OperationKind::kRegisterWriteArrayMaskPort :
            grh::OperationKind::kRegisterWriteArrayPort, graph.internSymbol("write"));
        for (const auto operand : {enable, index, data}) graph.addOperand(write, operand);
        if (masked) {
            const auto mask = makeConst(graph, "mask", "mask_op", 8, false, "8'hff");
            graph.addOperand(write, mask);
        }
        graph.addOperand(write, clock);
        graph.setAttr(write, "regSymbol", std::string("state"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        const auto read = makeRegisterRead(graph, "state", "read", "read_op", 24, false);
        const auto output = makeSlice(graph, "output", read, 20, 4);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !graph.findOperation("data_op") ||
            (masked && !graph.findOperation("mask_op")))
            return fail("whole-storage demand was mistaken for indexed-write local coordinates");
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("asymmetric_concat_demand");
        const auto high = makeConst(graph, "high", "high_op", 7, false, "7'd85");
        const auto middle = makeConst(graph, "middle", "middle_op", 5, false, "5'd19");
        const auto low = makeConst(graph, "low", "low_op", 3, false, "3'd5");
        const auto packed = graph.createValue(graph.internSymbol("packed"), 15, false);
        const auto concat = graph.createOperation(grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_op"));
        for (const auto element : {high, middle, low}) graph.addOperand(concat, element);
        graph.addResult(concat, packed);
        const auto output = makeSlice(graph, "output", packed, 3, 5);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !graph.findOperation("middle_op") ||
            graph.findOperation("high_op") || graph.findOperation("low_op"))
            return fail("DCE mapped an asymmetric concat demand to the wrong operand");
    }

    // ArrayCreate stores e0 in the low bits. Each partial demand must keep
    // exactly the producer of that element, including nonzero offsets.
    for (int64_t selected = 0; selected < 3; ++selected)
    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("array_element_demand");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        const auto output = makeSlice(graph, "output", array, selected * 8, 8);
        graph.bindOutputPort("output", output);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a partial ArrayCreate demand");
        for (int64_t i = 0; i < 3; ++i) {
            const auto producer = graph.findOperation("element" + std::to_string(i) + "_op");
            if (producer.valid() != (i == selected))
                return fail("ArrayCreate retained the wrong element producer for index " + std::to_string(selected));
        }
        if (!graph.findOperation("array_op") || !graph.findOperation("output_op"))
            return fail("partial ArrayCreate demand removed a live view");
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("array_full_demand");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        graph.bindOutputPort("array", array);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a full ArrayCreate demand");
        for (int64_t i = 0; i < 3; ++i)
            if (!graph.findOperation("element" + std::to_string(i) + "_op"))
                return fail("full ArrayCreate demand removed an element producer");
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("later_disjoint_array_demand");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        const auto low = makeSlice(graph, "low", array, 0, 8);
        graph.bindOutputPort("low", low);
        const auto high = makeSlice(graph, "high", array, 16, 8);
        const auto delayed = graph.createValue(graph.internSymbol("delayed"), 8, false);
        const auto assign = graph.createOperation(grh::OperationKind::kAssign,
                                                  graph.internSymbol("delayed_op"));
        graph.addOperand(assign, high);
        graph.addResult(assign, delayed);
        graph.bindOutputPort("delayed", delayed);

        // The extra assign makes [16,24) arrive after [0,8) has already
        // propagated. Both outputs use the same port root-mask bit.
        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a later disjoint ArrayCreate demand");
        if (!graph.findOperation("element0_op") || !graph.findOperation("element2_op") ||
            graph.findOperation("element1_op"))
            return fail("later disjoint demand lost its root reason or retained an undemanded producer");
        if (!graph.valueDef(elements[2]))
            return fail("later disjoint demand left the required high element undriven");
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto &graph = design.createGraph("later_disjoint_storage_demand");
        std::vector<grh::ValueId> elements;
        for (int64_t i = 0; i < 3; ++i)
            elements.push_back(makeConst(graph, "element" + std::to_string(i),
                "element" + std::to_string(i) + "_op", 8, false, "8'd" + std::to_string(i + 1)));
        const auto array = makeArray(graph, "array", elements);
        const auto data = graph.createValue(graph.internSymbol("data"), 24, false);
        const auto pack = graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("pack"));
        graph.addOperand(pack, array);
        graph.addResult(pack, data);
        const auto enable = makeConst(graph, "enable", "enable_op", 1, false, "1'b1");
        const auto clock = makeConst(graph, "clock", "clock_op", 1, false, "1'b0");
        const auto state = makeRegisterDecl(graph, "state");
        graph.setAttr(state, "width", int64_t{24});
        graph.setAttr(state, "isSigned", false);
        const auto read = makeRegisterRead(graph, "state", "read", "read_op", 24, false);
        (void)makeRegisterWrite(graph, "state", "write", {enable, data, clock});
        const auto low = makeSlice(graph, "low", read, 0, 8);
        graph.bindOutputPort("low", low);
        const auto high = makeSlice(graph, "high", read, 16, 8);
        const auto delayed = graph.createValue(graph.internSymbol("delayed"), 8, false);
        const auto assign = graph.createOperation(grh::OperationKind::kAssign,
                                                  graph.internSymbol("delayed_op"));
        graph.addOperand(assign, high);
        graph.addResult(assign, delayed);
        graph.bindOutputPort("delayed", delayed);

        PassManager manager;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("DCE failed on a later disjoint storage demand");
        if (!graph.findOperation("state") || !graph.findOperation("read_op") ||
            !graph.findOperation("write") || !graph.findOperation("element0_op") ||
            !graph.findOperation("element2_op") || graph.findOperation("element1_op"))
            return fail("storage region growth lost a root reason or retained an undemanded producer");
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto& graph = design.createGraph("frozen_inout_roots");
        const auto in = makeConst(graph, "io_in", "io_in_op", 8, false, "8'h11");
        const auto out = makeConst(graph, "io_out", "io_out_op", 8, false, "8'h22");
        const auto oe = makeConst(graph, "io_oe", "io_oe_op", 1, false, "1'b1");
        graph.bindInoutPort("io", in, out, oe);
        const auto dead = makeConst(graph, "dead", "dead_op", 8, false, "8'h33");
        const auto orphan = graph.createValue(graph.internSymbol("orphan"), 8, false);
        (void)dead; (void)orphan;
        graph.freeze();
        PassManager manager; manager.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !result.changed)
            return fail("DCE on a frozen graph with inout roots failed");
        if (graph.findOperation("dead_op") || graph.findValue("dead") || graph.findValue("orphan"))
            return fail("frozen DCE retained a dead operation or isolated value");
        for (const auto name : {"io_in", "io_out", "io_oe"}) {
            const auto value = graph.findValue(name);
            if (!value || !graph.valueIsInout(value) || !graph.valueDef(value) ||
                graph.opKind(graph.valueDef(value)) != grh::OperationKind::kConstant || !graph.valueUsers(value).empty())
                return fail("frozen DCE lost an inout in/out/oe root or damaged its users");
        }
    }

    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("g");

        wolvrix::lib::grh::ValueId liveConst = makeConst(graph, "c_live", "c_live_op", 1, false, "1'b1");
        wolvrix::lib::grh::ValueId deadConst = makeConst(graph, "c_dead", "c_dead_op", 1, false, "1'b0");
        wolvrix::lib::grh::ValueId keptConst = makeConst(graph, "c_keep", "c_keep_op", 1, false, "1'b0");
        graph.addDeclaredSymbol(graph.lookupSymbol("c_keep"));
        (void)keptConst;

        wolvrix::lib::grh::ValueId deadTmp = graph.createValue(graph.internSymbol("dead_tmp"), 1, false);
        wolvrix::lib::grh::OperationId deadNot = graph.createOperation(
            wolvrix::lib::grh::OperationKind::kNot, graph.internSymbol("dead_not"));
        graph.addOperand(deadNot, deadConst);
        graph.addResult(deadNot, deadTmp);

        wolvrix::lib::grh::ValueId out = graph.createValue(graph.internSymbol("out"), 1, false);
        graph.bindOutputPort("out", out);
        wolvrix::lib::grh::OperationId assign = graph.createOperation(
            wolvrix::lib::grh::OperationKind::kAssign, graph.internSymbol("assign_out"));
        graph.addOperand(assign, liveConst);
        graph.addResult(assign, out);

        PassManager manager;
        manager.options().keepDeclaredSymbols = true;
        manager.addPass(std::make_unique<DeadCodeElimPass>());

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
            return fail("Expected DCE to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected DCE to report changes");
        }

        if (graph.findOperation("dead_not").valid())
        {
            return fail("dead_not should be removed");
        }
        if (graph.findOperation("c_dead_op").valid())
        {
            return fail("c_dead_op should be removed");
        }
        if (graph.findValue("c_dead").valid())
        {
            return fail("c_dead value should be removed");
        }
        if (!graph.findOperation("c_live_op").valid())
        {
            return fail("c_live_op should remain");
        }
        if (!graph.findValue("c_live").valid())
        {
            return fail("c_live value should remain");
        }
        if (!graph.findOperation("assign_out").valid())
        {
            return fail("assign_out should remain");
        }
        if (!graph.findOperation("c_keep_op").valid())
        {
            return fail("c_keep_op should remain because it is declared");
        }
        if (!graph.findValue("c_keep").valid())
        {
            return fail("c_keep value should remain because it is declared");
        }
    }

    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("dead_reg_family");

        const auto liveConst = makeConst(graph, "c_live", "c_live_op", 1, false, "1'b1");
        const auto deadCond = makeConst(graph, "dead_cond", "dead_cond_op", 1, false, "1'b1");
        const auto deadData = makeConst(graph, "dead_data", "dead_data_op", 8, false, "8'h12");
        const auto deadClk = makeConst(graph, "dead_clk", "dead_clk_op", 1, false, "1'b1");
        (void)makeRegisterDecl(graph, "reg_dead");
        const auto regDeadRead = makeRegisterRead(graph, "reg_dead", "reg_dead_q", "reg_dead_read", 8, false);
        (void)makeRegisterWrite(graph, "reg_dead", "reg_dead_write", {deadCond, deadData, deadClk});

        const auto deadTmp = graph.createValue(graph.internSymbol("dead_tmp"), 8, false);
        const auto deadNot = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                   graph.internSymbol("dead_not"));
        graph.addOperand(deadNot, regDeadRead);
        graph.addResult(deadNot, deadTmp);

        const auto out = graph.createValue(graph.internSymbol("out"), 1, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, liveConst);
        graph.addResult(assignOut, out);

        PassManager manager;
        manager.options().keepDeclaredSymbols = false;
        manager.addPass(std::make_unique<DeadCodeElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during dead reg family run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected dead reg family DCE to succeed");
        }
        if (!res.changed)
        {
            return fail("Expected dead reg family DCE to report changes");
        }

        if (graph.findOperation("reg_dead").valid())
        {
            return fail("dead register declaration should be removed");
        }
        if (graph.findOperation("reg_dead_read").valid())
        {
            return fail("dead register read port should be removed");
        }
        if (graph.findOperation("reg_dead_write").valid())
        {
            return fail("dead register write port should be removed");
        }
        if (graph.findValue("reg_dead_q").valid())
        {
            return fail("dead register read result should be removed");
        }
        if (!graph.findOperation("assign_out").valid())
        {
            return fail("live output assign should remain after dead register pruning");
        }
    }

    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("live_reg_family");

        const auto liveCond = makeConst(graph, "live_cond", "live_cond_op", 1, false, "1'b1");
        const auto liveData = makeConst(graph, "live_data", "live_data_op", 8, false, "8'h34");
        const auto liveClk = makeConst(graph, "live_clk", "live_clk_op", 1, false, "1'b1");
        (void)makeRegisterDecl(graph, "reg_live");
        const auto regLiveRead = makeRegisterRead(graph, "reg_live", "reg_live_q", "reg_live_read", 8, false);
        (void)makeRegisterWrite(graph, "reg_live", "reg_live_write", {liveCond, liveData, liveClk});

        const auto out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, regLiveRead);
        graph.addResult(assignOut, out);

        PassManager manager;
        manager.options().keepDeclaredSymbols = false;
        manager.addPass(std::make_unique<DeadCodeElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during live reg family run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected live reg family DCE to succeed");
        }

        if (!graph.findOperation("reg_live").valid())
        {
            return fail("live register declaration should remain");
        }
        if (!graph.findOperation("reg_live_read").valid())
        {
            return fail("live register read port should remain");
        }
        if (!graph.findOperation("reg_live_write").valid())
        {
            return fail("live register write port should remain");
        }
        if (!graph.findOperation("live_data_op").valid())
        {
            return fail("write cone feeding live register should remain");
        }
    }

    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("declared_reg_family");

        const auto keepCond = makeConst(graph, "keep_cond", "keep_cond_op", 1, false, "1'b1");
        const auto keepData = makeConst(graph, "keep_data", "keep_data_op", 8, false, "8'h56");
        const auto keepClk = makeConst(graph, "keep_clk", "keep_clk_op", 1, false, "1'b1");
        (void)makeRegisterDecl(graph, "reg_keep");
        graph.addDeclaredSymbol(graph.lookupSymbol("reg_keep"));
        (void)makeRegisterWrite(graph, "reg_keep", "reg_keep_write", {keepCond, keepData, keepClk});

        const auto liveConst = makeConst(graph, "c_live", "c_live_op", 1, false, "1'b1");
        const auto out = graph.createValue(graph.internSymbol("out"), 1, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, liveConst);
        graph.addResult(assignOut, out);

        PassManager manager;
        manager.options().keepDeclaredSymbols = true;
        manager.addPass(std::make_unique<DeadCodeElimPass>());

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during declared reg family run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected declared reg family DCE to succeed");
        }

        if (!graph.findOperation("reg_keep").valid())
        {
            return fail("declared register declaration should remain");
        }
        if (!graph.findOperation("reg_keep_write").valid())
        {
            return fail("declared register write port should remain");
        }
    }

    {
        wolvrix::lib::grh::Design design;
        wolvrix::lib::grh::Graph &graph = design.createGraph("report_reg_family");

        const auto liveCond = makeConst(graph, "live_cond", "live_cond_op", 1, false, "1'b1");
        const auto liveData = makeConst(graph, "live_data", "live_data_op", 8, false, "8'h78");
        const auto liveClk = makeConst(graph, "live_clk", "live_clk_op", 1, false, "1'b1");
        (void)makeRegisterDecl(graph, "reg_live_report");
        const auto regLiveRead = makeRegisterRead(graph, "reg_live_report", "reg_live_report_q", "reg_live_report_read", 8, false);
        (void)makeRegisterWrite(graph, "reg_live_report", "reg_live_report_write", {liveCond, liveData, liveClk});

        const auto deadCond = makeConst(graph, "dead_cond", "dead_cond_op", 1, false, "1'b1");
        const auto deadData = makeConst(graph, "dead_data", "dead_data_op", 8, false, "8'h9a");
        const auto deadClk = makeConst(graph, "dead_clk", "dead_clk_op", 1, false, "1'b1");
        (void)makeRegisterDecl(graph, "reg_dead_report");
        (void)makeRegisterRead(graph, "reg_dead_report", "reg_dead_report_q", "reg_dead_report_read", 8, false);
        (void)makeRegisterWrite(graph, "reg_dead_report", "reg_dead_report_write", {deadCond, deadData, deadClk});

        const auto out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);
        const auto assignOut = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                     graph.internSymbol("assign_out"));
        graph.addOperand(assignOut, regLiveRead);
        graph.addResult(assignOut, out);

        PassManager manager;
        SessionStore session;
        manager.options().keepDeclaredSymbols = false;
        manager.options().session = &session;
        DeadCodeElimOptions options;
        options.outputKey = "dce.report";
        options.sampleLimit = 4;
        manager.addPass(std::make_unique<DeadCodeElimPass>(options));

        PassDiagnostics diags;
        PassManagerResult res{};
        try
        {
            res = manager.run(design, diags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Exception during report reg family run: ") + ex.what());
        }
        if (!res.success || diags.hasError())
        {
            return fail("Expected report reg family DCE to succeed");
        }
        const auto *report = getSessionValue<std::string>(session, "dce.report");
        if (report == nullptr)
        {
            return fail("Expected dead-code-elim report in session");
        }
        if (report->find("\"live\":1") == std::string::npos || report->find("\"dead\":1") == std::string::npos)
        {
            return fail("Expected report to contain live/dead register counts");
        }
        if (report->find("reg_dead_report") == std::string::npos || report->find("reg_live_report") == std::string::npos)
        {
            return fail("Expected report to include register samples");
        }
        if (report->find("\"port\":1") == std::string::npos)
        {
            return fail("Expected report to record live port reachability");
        }
    }

    {
        namespace grh = wolvrix::lib::grh;
        grh::Design design;
        auto& graph = design.createGraph("semantic");
        auto& tree = graph.getOrCreateSemanticTree("unit", "circuit", "semantic");
        const auto liveDeclaration = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "live");
        const auto inputDeclaration = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "input");
        const auto declaredDeclaration = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "declared");
        const auto deadDeclaration = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "dead");

        const auto liveConst = makeConst(graph, "semantic_live", "semantic_live_op", 1, false, "1'b1");
        graph.setValueSemanticIds(liveConst, std::span<const grh::SemanticId>(&liveDeclaration, 1));
        const auto liveOp = graph.valueDef(liveConst);
        graph.setOpSemanticIds(liveOp, std::span<const grh::SemanticId>(&liveDeclaration, 1));
        const auto out = graph.createValue(graph.internSymbol("semantic_out"), 1, false);
        graph.bindOutputPort("semantic_out", out);
        graph.setValueSemanticIds(out, std::span<const grh::SemanticId>(&liveDeclaration, 1));
        const auto assign = graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("semantic_assign"));
        graph.addOperand(assign, liveConst);
        graph.addResult(assign, out);
        graph.setOpSemanticIds(assign, std::span<const grh::SemanticId>(&liveDeclaration, 1));

        const auto input = graph.createValue(graph.internSymbol("semantic_input"), 1, false);
        graph.bindInputPort("semantic_input", input);
        graph.setValueSemanticIds(input, std::span<const grh::SemanticId>(&inputDeclaration, 1));
        const auto kept = makeConst(graph, "semantic_keep", "semantic_keep_op", 1, false, "1'b0");
        graph.addDeclaredSymbol(graph.lookupSymbol("semantic_keep"));
        graph.setValueSemanticIds(kept, std::span<const grh::SemanticId>(&declaredDeclaration, 1));
        graph.setOpSemanticIds(graph.valueDef(kept), std::span<const grh::SemanticId>(&declaredDeclaration, 1));

        const auto deadConst = makeConst(graph, "semantic_dead", "semantic_dead_op", 1, false, "1'b0");
        const auto deadTmp = graph.createValue(graph.internSymbol("semantic_dead_tmp"), 1, false);
        const auto deadNot = graph.createOperation(grh::OperationKind::kNot, graph.internSymbol("semantic_dead_not"));
        graph.addOperand(deadNot, deadConst);
        graph.addResult(deadNot, deadTmp);
        for (const auto value : {deadConst, deadTmp})
            graph.setValueSemanticIds(value, std::span<const grh::SemanticId>(&deadDeclaration, 1));
        for (const auto op : {graph.valueDef(deadConst), deadNot})
            graph.setOpSemanticIds(op, std::span<const grh::SemanticId>(&deadDeclaration, 1));

        PassManager manager;
        manager.options().keepDeclaredSymbols = true;
        auto pass = std::make_unique<DeadCodeElimPass>();
        const auto* passState = pass.get();
        manager.addPass(std::move(pass));
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !result.changed)
            return fail("SemanticTree DCE failed");
        if (graph.findOperation("semantic_dead_op") || graph.findOperation("semantic_dead_not") ||
            graph.findValue("semantic_dead") || graph.findValue("semantic_dead_tmp"))
            return fail("SemanticTree DCE retained the dead cone");
        if (!graph.findOperation("semantic_live_op") || !graph.findOperation("semantic_keep_op") ||
            !graph.findValue("semantic_input"))
            return fail("SemanticTree DCE removed a live or declared entity");
        if (tree.contains(deadDeclaration) || tree.size() != 4)
            return fail("SemanticTree DCE did not reclaim the dead source subtree");
        if (tree.path(liveDeclaration) != std::vector<std::string>{"semantic", "live"} ||
            tree.path(inputDeclaration) != std::vector<std::string>{"semantic", "input"} ||
            tree.path(declaredDeclaration) != std::vector<std::string>{"semantic", "declared"})
            return fail("SemanticTree DCE changed a surviving source path");
        if (passState->stats().semanticNodesRemoved != 1 || passState->stats().opsRemoved != 2)
            return fail("SemanticTree DCE reported incorrect deletion counts");

        PassManager second;
        second.options().keepDeclaredSymbols = true;
        second.addPass(std::make_unique<DeadCodeElimPass>());
        PassDiagnostics secondDiagnostics;
        const auto secondResult = second.run(design, secondDiagnostics);
        if (!secondResult.success || secondResult.changed || secondDiagnostics.hasError())
            return fail("SemanticTree DCE is not idempotent");
    }

    return 0;
}
