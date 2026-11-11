#include "grhsim_oracle.hpp"

#include "core/grh.hpp"
#include "core/state.hpp"
#include "core/transform.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/split_static_array.hpp"
#include "transform/verify.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[split-static-array-tests] " << message << '\n';
        return 1;
    }

    ValueId constant(Graph &graph, const std::string &name, int32_t width, const std::string &literal)
    {
        const ValueId value = graph.createValue(graph.internSymbol(name), width, false);
        const OperationId op = graph.createOperation(OperationKind::kConstant,
                                                     graph.internSymbol(name + "_op"));
        graph.setAttr(op, "constValue", literal);
        graph.addResult(op, value);
        return value;
    }

    ValueId unary(Graph &graph, OperationKind kind, const std::string &name, ValueId operand,
                  std::span<const int64_t> shape = {})
    {
        const ValueId value = graph.createValue(graph.internSymbol(name), graph.valueWidth(operand),
                                                graph.valueSigned(operand), graph.valueType(operand), shape,
                                                graph.valueDomain(operand));
        const OperationId op = graph.createOperation(kind, graph.internSymbol(name + "_op"));
        graph.addOperand(op, operand);
        graph.addResult(op, value);
        return value;
    }

    ValueId arrayCreate(Graph &graph, const std::string &name, std::span<const ValueId> elements,
                        std::span<const int64_t> suffixShape = {})
    {
        std::vector<int64_t> shape{static_cast<int64_t>(elements.size())};
        shape.insert(shape.end(), suffixShape.begin(), suffixShape.end());
        int64_t width = 0;
        for (const ValueId element : elements) width += graph.valueWidth(element);
        const ValueId value = graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width),
                                                graph.valueSigned(elements.front()), ValueType::Logic, shape,
                                                graph.valueDomain(elements.front()));
        const OperationId op = graph.createOperation(OperationKind::kArrayCreate,
                                                     graph.internSymbol(name + "_op"));
        for (const ValueId element : elements) graph.addOperand(op, element);
        graph.addResult(op, value);
        return value;
    }

    ValueId readElement(Graph &graph, const std::string &name, ValueId base,
                        std::span<const ValueId> indices, std::span<const int64_t> resultShape = {})
    {
        int64_t elementCount = 1;
        for (const int64_t dim : graph.valueShape(base)) elementCount *= dim;
        int32_t width = graph.valueWidth(base) / static_cast<int32_t>(elementCount);
        for (std::size_t i = indices.size(); i < graph.valueShape(base).size(); ++i)
            width *= static_cast<int32_t>(graph.valueShape(base)[i]);
        const ValueId result = graph.createValue(graph.internSymbol(name), width,
                                                 graph.valueSigned(base), graph.valueType(base), resultShape,
                                                 graph.valueDomain(base));
        const OperationId op = graph.createOperation(OperationKind::kSliceArray,
                                                     graph.internSymbol(name + "_op"));
        graph.addOperand(op, base);
        for (const ValueId index : indices) graph.addOperand(op, index);
        graph.addResult(op, result);
        graph.bindOutputPort(name, result);
        return result;
    }

    OperationId makeState(Graph &graph, const std::string &name, const LogicalType &type, InitStep init)
    {
        const OperationId state = graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
        setStateLogicalType(graph, state, type);
        setStateInitSpec(graph, state, {std::move(init)});
        return state;
    }

    ValueId readState(Graph &graph, const std::string &name, const std::string &target,
                      const LogicalType &resultType, std::span<const ValueId> indices = {}, bool output = true)
    {
        const ValueId result = graph.createValue(graph.internSymbol(name), resultType);
        const OperationId read = graph.createOperation(indices.empty() ? OperationKind::kRegisterReadPort
                                                                      : OperationKind::kRegisterReadArrayPort,
                                                       graph.internSymbol(name + "_op"));
        for (const ValueId index : indices) graph.addOperand(read, index);
        graph.addResult(read, result);
        graph.setAttr(read, "regSymbol", target);
        if (output) graph.bindOutputPort(name, result);
        return result;
    }

    OperationId writeState(Graph &graph, const std::string &name, const std::string &target,
                           OperationKind kind, std::span<const ValueId> operands, int64_t order)
    {
        const OperationId write = graph.createOperation(kind, graph.internSymbol(name));
        for (const ValueId operand : operands) graph.addOperand(write, operand);
        graph.setAttr(write, "regSymbol", target);
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        graph.setAttr(write, "writerOrder", order);
        return write;
    }

    ValueId staticSlice(Graph &graph, const std::string &name, ValueId base, int64_t start, int64_t width,
                        bool output = false)
    {
        const ValueId result = graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false,
                                                 graph.valueType(base), std::span<const int64_t>{},
                                                 graph.valueDomain(base));
        const OperationId slice = graph.createOperation(OperationKind::kSliceStatic,
                                                        graph.internSymbol(name + "_op"));
        graph.addOperand(slice, base);
        graph.addResult(slice, result);
        graph.setAttr(slice, "sliceStart", start);
        graph.setAttr(slice, "sliceEnd", start + width - 1);
        if (output) graph.bindOutputPort(name, result);
        return result;
    }

    bool split(Graph &graph, Design &design, std::optional<bool> keepDeclared = std::nullopt)
    {
        PassManager manager;
        manager.options().graphThreads = 2;
        manager.options().keepDeclaredSymbols = keepDeclared;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || !result.changed) return false;
        for (const OperationId op : graph.operations())
        {
            if (graph.opKind(op) == OperationKind::kAssign && graph.opOperands(op).size() == 1 &&
                graph.opResults(op).size() == 1 && graph.opOperands(op)[0] == graph.opResults(op)[0])
            {
                std::cerr << "[split-static-array-tests] self assignment produced at op " << op.index << '\n';
                return false;
            }
        }
        return true;
    }

    bool dce(Design &design, std::optional<bool> keepDeclared = std::nullopt)
    {
        PassManager manager;
        manager.options().keepDeclaredSymbols = keepDeclared;
        manager.addPass(std::make_unique<DeadCodeElimPass>());
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        return result.success && !diagnostics.hasError() && result.changed;
    }

    struct ConcatFixture
    {
        Design design;
        Graph *graph = nullptr;
        ValueId array;
        ValueId read;
        SemanticId arraySemantic = 0;

        ConcatFixture()
        {
            graph = &design.createGraph("g");
            const ValueId e0 = constant(*graph, "e0", 8, "8'h10");
            const ValueId e1 = constant(*graph, "e1", 8, "8'h11");
            const ValueId e2 = constant(*graph, "e2", 8, "8'h12");
            const std::array<int64_t, 1> shape{3};
            const ValueId packed = graph->createValue(graph->internSymbol("packed"), 24, false,
                                                       ValueType::Logic, shape);
            const OperationId concat = graph->createOperation(OperationKind::kConcat,
                                                               graph->internSymbol("packed_op"));
            for (const ValueId element : {e2, e1, e0}) graph->addOperand(concat, element);
            graph->addResult(concat, packed);
            array = unary(*graph, OperationKind::kAssign, "array", packed, shape);
            graph->addDeclaredSymbol(graph->valueSymbol(array));
            auto &tree = graph->getOrCreateSemanticTree("unit", "circuit", "g");
            arraySemantic = tree.addNode(tree.root(), SemanticNodeKind::Declaration, "array");
            graph->setValueSemanticIds(array, std::span<const SemanticId>(&arraySemantic, 1));
            const ValueId zero = constant(*graph, "zero", 2, "2'd0");
            read = readElement(*graph, "out", array, std::span<const ValueId>(&zero, 1));
        }
    };
}

int main()
{
    for (const auto &[literal, width] : {
             std::pair<const char *, int32_t>{"4'b1_0", 4},
             std::pair<const char *, int32_t>{"8'h12", 2}})
    {
        Design design;
        Graph &graph = design.createGraph("index_literal_projection");
        const std::array<ValueId, 3> elements{
            constant(graph, "e0", 8, "8'd1"), constant(graph, "e1", 8, "8'd2"),
            constant(graph, "e2", 8, "8'd3")};
        const ValueId array = arrayCreate(graph, "array", elements);
        const ValueId index = constant(graph, "index", width, literal);
        const ValueId read = readElement(graph, "out", array, std::span<const ValueId>(&index, 1));
        if (!split(graph, design) || graph.opKind(graph.valueDef(read)) != OperationKind::kAssign ||
            graph.opOperands(graph.valueDef(read))[0] != elements[2])
            return fail("static array index did not respect literal separators and value width");
    }

    for (const bool sourceSigned : {false, true})
    {
        Design design;
        Graph &graph = design.createGraph("index_signedness_alias");
        std::vector<ValueId> elements;
        for (int64_t i = 0; i < 16; ++i)
            elements.push_back(constant(graph, "e" + std::to_string(i), 8, "8'd" + std::to_string(i + 1)));
        const ValueId array = arrayCreate(graph, "array", elements);
        const ValueId source = graph.createValue(graph.internSymbol("index_source"), 8, sourceSigned);
        const OperationId sourceOp = graph.createOperation(OperationKind::kConstant,
                                                           graph.internSymbol("index_source_op"));
        graph.addResult(sourceOp, source);
        graph.setAttr(sourceOp, "constValue", std::string(sourceSigned ? "4'hf" : "4'shf"));
        const ValueId index = graph.createValue(graph.internSymbol("index"), 8, !sourceSigned);
        const OperationId assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("index_op"));
        graph.addOperand(assign, source);
        graph.addResult(assign, index);
        const ValueId read = readElement(graph, "out", array, std::span<const ValueId>(&index, 1));

        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diagnostics;
        const auto result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("split failed on a signedness-changing index assign");
        const OperationId readOp = graph.valueDef(read);
        if (sourceSigned) {
            if (graph.opKind(readOp) != OperationKind::kSliceArray)
                return fail("out-of-range unsigned index 255 was incorrectly split at element 15");
        } else if (graph.opKind(readOp) != OperationKind::kAssign ||
                   graph.opOperands(readOp)[0] != elements[15]) {
            return fail("signed index view did not select the producer's zero-extended element 15");
        }
    }

    {
        ConcatFixture fixture;
        Graph &graph = *fixture.graph;
        if (!split(graph, fixture.design)) return fail("concat/assign array was not split");
        if (graph.opKind(graph.valueDef(fixture.read)) != OperationKind::kAssign ||
            graph.opOperands(graph.valueDef(fixture.read)).size() != 1 ||
            graph.opOperands(graph.valueDef(fixture.read))[0] != graph.findValue("e0"))
            return fail("static concat read did not select element zero directly");
        if (!dce(fixture.design)) return fail("default DCE failed after array splitting");
        if (graph.findValue("array").valid() || graph.findValue("packed").valid() ||
            graph.findValue("e1").valid() || graph.findValue("e2").valid() ||
            !graph.findValue("e0").valid())
            return fail("default DCE retained the declared aggregate or its unread elements");
        if (graph.isDeclaredSymbol(graph.lookupSymbol("array")) || graph.semanticTree()->size() != 1)
            return fail("aggregate declaration or semantic node was not reclaimed");
    }

    {
        ConcatFixture fixture;
        Graph &graph = *fixture.graph;
        if (!split(graph, fixture.design)) return fail("explicit-protection fixture was not split");
        if (!dce(fixture.design, true)) return fail("explicit-protection DCE failed");
        if (!graph.findValue("array").valid() || !graph.findValue("packed").valid() ||
            !graph.findValue("e1").valid() || !graph.findValue("e2").valid() ||
            !graph.isDeclaredSymbol(graph.lookupSymbol("array")))
            return fail("keepDeclaredSymbols=true no longer protects the aggregate cone");
    }

    {
        Design design;
        Graph &graph = design.createGraph("nested");
        const ValueId a = constant(graph, "a", 4, "4'h1");
        const ValueId b = constant(graph, "b", 4, "4'h2");
        const ValueId c = constant(graph, "c", 4, "4'h3");
        const ValueId d = constant(graph, "d", 4, "4'h4");
        const std::array<ValueId, 2> row0Values{a, b};
        const std::array<ValueId, 2> row1Values{c, d};
        const ValueId row0 = arrayCreate(graph, "row0", row0Values);
        const ValueId row1 = arrayCreate(graph, "row1", row1Values);
        const std::array<ValueId, 2> rows{row0, row1};
        const std::array<int64_t, 1> suffix{2};
        const ValueId array = arrayCreate(graph, "array", rows, suffix);
        const ValueId one = constant(graph, "one", 1, "1'd1");
        const ValueId zero = constant(graph, "zero", 1, "1'd0");
        const std::array<ValueId, 2> indices{one, zero};
        const ValueId read = readElement(graph, "out", array, indices);
        if (!split(graph, design)) return fail("nested kArrayCreate read was not split");
        if (graph.opOperands(graph.valueDef(read))[0] != c)
            return fail("multi-index read selected the wrong row-major element");
        if (!dce(design, false)) return fail("nested-array DCE failed");
        if (!graph.findValue("c").valid() || graph.findValue("a").valid() ||
            graph.findValue("b").valid() || graph.findValue("d").valid() ||
            graph.findValue("array").valid())
            return fail("nested-array DCE did not remove unselected members");
    }

    {
        Design design;
        Graph &graph = design.createGraph("mux");
        const ValueId a = constant(graph, "a", 8, "8'h10");
        const ValueId b = constant(graph, "b", 8, "8'h11");
        const ValueId c = constant(graph, "c", 8, "8'h12");
        const ValueId d = constant(graph, "d", 8, "8'h13");
        const std::array<ValueId, 2> leftValues{a, b};
        const std::array<ValueId, 2> rightValues{c, d};
        const ValueId left = arrayCreate(graph, "left", leftValues);
        const ValueId right = arrayCreate(graph, "right", rightValues);
        const ValueId cond = constant(graph, "cond", 1, "1'b0");
        const ValueId array = graph.createValue(graph.internSymbol("array"), 16, false,
                                                ValueType::Logic, std::array<int64_t, 1>{2});
        const OperationId mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("array_op"));
        for (const ValueId operand : {cond, left, right}) graph.addOperand(mux, operand);
        graph.addResult(mux, array);
        const ValueId one = constant(graph, "one", 1, "1'd1");
        const ValueId read = readElement(graph, "out", array, std::span<const ValueId>(&one, 1));
        auto &tree = graph.getOrCreateSemanticTree("unit", "circuit", "mux");
        const SemanticId readSemantic = tree.addNode(tree.root(), SemanticNodeKind::Expression, "read");
        const SemanticId arraySemantic = tree.addNode(tree.root(), SemanticNodeKind::Expression, "array");
        graph.setOpSemanticIds(graph.valueDef(read), std::span<const SemanticId>(&readSemantic, 1));
        graph.setOpSemanticIds(mux, std::span<const SemanticId>(&arraySemantic, 1));
        if (!split(graph, design)) return fail("shaped mux read was not split");
        const ValueId selected = graph.opOperands(graph.valueDef(read))[0];
        const OperationId selectedMux = graph.valueDef(selected);
        if (graph.opKind(selectedMux) != OperationKind::kMux ||
            graph.opOperands(selectedMux).size() != 3 ||
            graph.opOperands(selectedMux)[1] != b || graph.opOperands(selectedMux)[2] != d ||
            graph.opSemanticIds(selectedMux).size() != 1 ||
            graph.opSemanticIds(selectedMux)[0] != readSemantic)
            return fail("mux split did not preserve the selected branches and semantics");
        if (!dce(design, false)) return fail("mux-array DCE failed");
        if (graph.findValue("a").valid() || graph.findValue("c").valid() ||
            !graph.findValue("b").valid() || !graph.findValue("d").valid())
            return fail("mux-array DCE did not remove the unselected element cone");
    }

    {
        Design design;
        Graph &graph = design.createGraph("recursive_concat_arena_growth");
        std::vector<ValueId> level;
        std::vector<std::pair<std::string, int32_t>> inputs;
        for (int i = 0; i < 128; ++i)
        {
            const std::string name = "input_" + std::to_string(i);
            const int32_t width = i % 2 == 0 ? 11 : 13;
            const ValueId value = graph.createValue(graph.internSymbol(name), width, false);
            graph.bindInputPort(name, value);
            level.push_back(value);
            inputs.emplace_back(name, width);
        }
        int depth = 0;
        while (level.size() > 1)
        {
            std::vector<ValueId> next;
            for (std::size_t i = 0; i < level.size(); i += 2)
            {
                const std::string name = "concat_" + std::to_string(depth) + "_" + std::to_string(i);
                const ValueId value = graph.createValue(graph.internSymbol(name),
                                                        graph.valueWidth(level[i]) + graph.valueWidth(level[i + 1]),
                                                        false);
                const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                                  graph.internSymbol(name + "_op"));
                graph.addOperand(concat, level[i]);
                graph.addOperand(concat, level[i + 1]);
                graph.addResult(concat, value);
                next.push_back(value);
            }
            level.swap(next);
            ++depth;
        }
        const ValueId packed = level.front();
        const OperationId packedOp = graph.valueDef(packed);
        const ValueId array = unary(graph, OperationKind::kAssign, "array", packed,
                                    std::array<int64_t, 1>{3});
        const ValueId one = constant(graph, "one", 2, "2'd1");
        const ValueId out = readElement(graph, "out", array, std::span<const ValueId>(&one, 1));
        // Compact leaves no spare edge capacity. Selecting the middle 512
        // bits crosses both leaf boundaries and recursive concat branches;
        // the first generated view grows the shared operand/result arenas.
        graph.compact();
        auto reference = design.clone();
        const auto originalArena = reinterpret_cast<std::uintptr_t>(graph.opOperands(packedOp).data());
        if (!split(graph, design)) return fail("recursive concat split failed during arena growth");
        if (reinterpret_cast<std::uintptr_t>(graph.opOperands(packedOp).data()) == originalArena)
            return fail("recursive concat fixture did not reallocate the operand arena");
        const OperationId outOp = graph.valueDef(out);
        if (graph.opKind(outOp) != OperationKind::kAssign ||
            graph.valueWidth(graph.opOperands(outOp)[0]) != 512)
            return fail("recursive concat split produced the wrong selected width");

        grhsim_test::Oracle original(*reference.findGraph("recursive_concat_arena_growth"));
        grhsim_test::Oracle rewritten(graph);
        std::mt19937_64 random(417);
        for (int sample = 0; sample < 48; ++sample)
        {
            std::map<std::string, uint64_t> values;
            for (const auto &[name, width] : inputs)
                values.emplace(name, sample == 0 ? 0 : sample == 1 ? (uint64_t{1} << width) - 1 : random());
            if (original.eval(values) != rewritten.eval(values))
                return fail("recursive concat split changed the requested middle bits");
        }
    }

    {
        Design design;
        Graph &graph = design.createGraph("partial_selection_cache");
        const std::array<int64_t, 1> shape{4};
        const ValueId input = graph.createValue(graph.internSymbol("input"), 4, false,
                                                 ValueType::Logic, shape);
        graph.bindInputPort("input", input);
        const ValueId base = unary(graph, OperationKind::kNot, "opaque", input, shape);
        const ValueId index = constant(graph, "index", 2, "2'd3");
        const ValueId direct = readElement(graph, "direct", base, std::span<const ValueId>(&index, 1));
        const ValueId alias = unary(graph, OperationKind::kAssign, "alias", base, shape);
        const ValueId nested = readElement(graph, "nested", alias, std::span<const ValueId>(&index, 1));
        auto reference = design.clone();

        // The unsupported direct selection runs first. Its whole-width base
        // must not become a cached result for the recursive alias selection.
        if (!split(graph, design)) return fail("partial selection cache fixture failed");
        if (graph.opKind(graph.valueDef(direct)) != OperationKind::kSliceArray ||
            graph.opKind(graph.valueDef(nested)) != OperationKind::kAssign ||
            graph.valueWidth(graph.opOperands(graph.valueDef(nested))[0]) != 1)
            return fail("unsupported partial selection poisoned a recursive selection");
        const auto compareOutputs = [&]() {
            grhsim_test::Oracle original(*reference.findGraph("partial_selection_cache"), {}, nullptr, true);
            grhsim_test::Oracle rewritten(graph, {}, nullptr, true);
            for (uint64_t bits = 0; bits < 16; ++bits)
            {
                const std::map<std::string, uint64_t> inputs{{"input", bits}};
                if (original.eval(inputs) != rewritten.eval(inputs)) return false;
            }
            return true;
        };
        if (!compareOutputs()) return fail("partial selection cache rewrite changed selected bits before DCE");
        if (!dce(design, false)) return fail("partial selection cache DCE failed");
        if (!compareOutputs()) return fail("partial selection cache rewrite changed selected bits after DCE");
    }

    {
        Design design;
        Graph &graph = design.createGraph("dynamic");
        const ValueId a = constant(graph, "a", 8, "8'h10");
        const ValueId b = constant(graph, "b", 8, "8'h11");
        const std::array<ValueId, 2> elements{a, b};
        const ValueId array = arrayCreate(graph, "array", elements);
        const ValueId index = graph.createValue(graph.internSymbol("index"), 1, false);
        graph.bindInputPort("index", index);
        const ValueId read = readElement(graph, "out", array, std::span<const ValueId>(&index, 1));
        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || result.changed ||
            graph.opKind(graph.valueDef(read)) != OperationKind::kSliceArray)
            return fail("dynamic array read was not left intact");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_whole");
        const LogicalType arrayType{ValueType::Logic, 24, false, LogicDomain::FourState, {3}};
        makeState(graph, "r", arrayType, {InitKind::Constant, "24'h030201"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId unused = constant(graph, "unused", 8, "8'hff");
        const ValueId update = constant(graph, "update", 8, "8'h5a");
        const ValueId current = readState(
            graph, "current", "r", {ValueType::Logic, 24, false, LogicDomain::FourState, {}}, {}, false);
        const ValueId low = staticSlice(graph, "low", current, 0, 8, true);
        const ValueId one = constant(graph, "one", 32, "32'd1");
        readState(graph, "one_out", "r", {ValueType::Logic, 8, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&one, 1));
        const ValueId next = graph.createValue(graph.internSymbol("next"), 24, false);
        const OperationId concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("next_op"));
        for (const ValueId value : {unused, update, low}) graph.addOperand(concat, value);
        graph.addResult(concat, next);
        const std::array<ValueId, 3> writeOperands{enable, next, clock};
        const OperationId wholeWrite = writeState(graph, "whole_write", "r", OperationKind::kRegisterWritePort,
                                                  writeOperands, 7);
        if (!split(graph, design)) return fail("whole state array was not split");
        if (graph.findOperation("r").valid() || !graph.findOperation("r_0").valid() ||
            !graph.findOperation("r_1").valid() || !graph.findOperation("r_2").valid())
            return fail("state members were not created or the aggregate declaration survived");
        const std::string init0 = stateInitSpec(graph, graph.findOperation("r_0")).front().value;
        const std::string init1 = stateInitSpec(graph, graph.findOperation("r_1")).front().value;
        if (init0 != "8'h1" || init1 != "8'h2")
            return fail("state initializer was not sliced by element: " + init0 + ", " + init1);
        if (graph.opKind(graph.valueDef(low)) != OperationKind::kAssign)
            return fail("whole-read static slice was not forwarded to a member");
        const std::string wholeTarget(graph.opAttrRange(wholeWrite).find("regSymbol")->value().asString());
        if (wholeTarget != "r_0" || graph.opKind(wholeWrite) != OperationKind::kRegisterWritePort)
            return fail("whole write was not converted to a scalar member write");
        if (!dce(design, false)) return fail("state-split DCE failed");
        if (!graph.findOperation("r_0").valid() || !graph.findOperation("r_1").valid() ||
            graph.findOperation("r_2").valid() || graph.findValue("unused").valid() ||
            graph.findValue("current").valid() || graph.findValue("next").valid())
            return fail("DCE did not remove the unread state member and its whole-value cone");
        for (const OperationId op : graph.operations())
        {
            if (!isRegisterWritePortKind(graph.opKind(op))) continue;
            const auto target = graph.opAttrRange(op).find("regSymbol");
            if (target && target->value().asString() == "r")
                return fail("a write still targets the removed aggregate state");
        }
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_multidim");
        const LogicalType arrayType{ValueType::Logic, 4, false, LogicDomain::FourState, {2, 2}};
        makeState(graph, "a", arrayType, {InitKind::Constant, "4'b0000"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId one = constant(graph, "one", 32, "32'd1");
        const ValueId current = readState(graph, "current", "a", arrayType, {}, false);
        const std::array<ValueId, 3> writeOperands{enable, current, clock};
        writeState(graph, "hold", "a", OperationKind::kRegisterWritePort, writeOperands, 0);
        const std::array<ValueId, 2> indices{one, zero};
        readState(graph, "out", "a", {ValueType::Logic, 1, false, LogicDomain::FourState, {}}, indices);
        if (!split(graph, design)) return fail("multidimensional whole state read was not rebuilt");
        if (!graph.findOperation("a_0_0").valid() || !graph.findOperation("a_1_0").valid() ||
            !graph.findOperation("a_1_1").valid())
            return fail("multidimensional member names are incorrect");
        if (!dce(design, false)) return fail("multidimensional state DCE failed");
        if (graph.findOperation("a_0_0").valid() || graph.findOperation("a_0_1").valid() ||
            !graph.findOperation("a_1_0").valid() || graph.findOperation("a_1_1").valid())
            return fail("multidimensional self-hold did not collapse to the read member");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_degenerate_dimension");
        const LogicalType arrayType{ValueType::Logic, 6, false, LogicDomain::FourState, {1, 3}};
        makeState(graph, "degenerate", arrayType, {InitKind::Constant, "6'b000000"});
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId one = constant(graph, "one", 32, "32'd1");
        const std::array<ValueId, 2> indices{zero, one};
        readState(graph, "out", "degenerate", {ValueType::Logic, 2, false, LogicDomain::FourState, {}}, indices);
        if (!split(graph, design)) return fail("degenerate-dimension state was not split");
        if (!graph.findOperation("degenerate_0").valid() || !graph.findOperation("degenerate_1").valid() ||
            !graph.findOperation("degenerate_2").valid() || graph.findOperation("degenerate_0_0").valid() ||
            graph.findOperation("degenerate_0_1").valid() || graph.findOperation("degenerate_0_2").valid())
            return fail("degenerate dimensions were not removed from member names");
        if (!dce(design, false)) return fail("degenerate-dimension DCE failed");
        if (graph.findOperation("degenerate_0").valid() || !graph.findOperation("degenerate_1").valid() ||
            graph.findOperation("degenerate_2").valid())
            return fail("degenerate-dimension DCE retained the wrong state elements");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_singleton");
        makeState(graph, "single", {ValueType::Logic, 1, false, LogicDomain::FourState, {1}},
                  {InitKind::Constant, "1'b0"});
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        readState(graph, "out", "single", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || result.changed || !graph.findOperation("single").valid() ||
            graph.findOperation("single_0").valid())
            return fail("singleton state was needlessly renamed or split");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_memory");
        const OperationId memory = makeState(graph, "mem", {ValueType::Logic, 2, false, LogicDomain::FourState, {1, 2}},
                                             {InitKind::Constant, "2'b00"});
        graph.setAttr(memory, "firrtl.memory", "ram");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId one = constant(graph, "one", 32, "32'd1");
        const std::array<ValueId, 2> indices{zero, one};
        readState(graph, "out", "mem", {ValueType::Logic, 1, false, LogicDomain::FourState, {}}, indices);
        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || result.changed || !graph.findOperation("mem").valid() ||
            graph.findOperation("mem_0").valid() || graph.findOperation("mem_0_0").valid())
            return fail("memory was incorrectly split as an ordinary state array");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_cross_family_fallback");
        const LogicalType arrayType{ValueType::Logic, 2, false, LogicDomain::FourState, {2}};
        makeState(graph, "dst", arrayType, {InitKind::Constant, "2'b00"});
        makeState(graph, "src", arrayType, {InitKind::Constant, "2'b00"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        readState(graph, "out", "dst", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        const ValueId current = readState(graph, "current", "src", arrayType, {}, false);
        const std::array<ValueId, 3> writeOperands{enable, current, clock};
        writeState(graph, "write", "dst", OperationKind::kRegisterWritePort, writeOperands, 0);
        staticSlice(graph, "low", current, 0, 1, true);
        if (!split(graph, design)) return fail("cross-family fallback produced an invalid rewrite");
        if (!dce(design, false)) return fail("cross-family fallback DCE failed");
        if (!graph.findOperation("dst_0").valid() || graph.findOperation("dst_1").valid() ||
            !graph.findOperation("src_0").valid() || graph.findOperation("src_1").valid())
            return fail("cross-family fallback retained the wrong state members");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_member_group");
        const LogicalType arrayType{ValueType::Logic, 4, false, LogicDomain::FourState, {4}};
        makeState(graph, "grouped", arrayType, {InitKind::Constant, "4'b0000"});
        const ValueId current = readState(graph, "current", "grouped", arrayType, {}, false);
        const ValueId middle = staticSlice(graph, "middle", current, 1, 2, true);
        if (!split(graph, design)) return fail("member-group slice was not split");
        ValueId view = graph.opOperands(graph.valueDef(middle))[0];
        if (graph.opKind(graph.valueDef(view)) == OperationKind::kAssign)
            view = graph.opOperands(graph.valueDef(view))[0];
        const OperationId create = graph.valueDef(view);
        if (graph.opKind(create) != OperationKind::kArrayCreate || graph.opOperands(create).size() != 2)
            return fail("member-group slice did not become a two-element array view");
        const std::array<std::string_view, 2> expectedMembers{"grouped_1", "grouped_2"};
        for (std::size_t i = 0; i < expectedMembers.size(); ++i)
        {
            const OperationId read = graph.valueDef(graph.opOperands(create)[i]);
            const auto target = graph.opAttrRange(read).find("regSymbol");
            if (!target || target->value().asString() != expectedMembers[i])
                return fail("member-group slice selected the wrong state elements");
        }
        if (!dce(design, false)) return fail("member-group DCE failed");
        if (graph.findOperation("grouped_0").valid() || !graph.findOperation("grouped_1").valid() ||
            !graph.findOperation("grouped_2").valid() || graph.findOperation("grouped_3").valid())
            return fail("member-group DCE retained the wrong state elements");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_nested_view_slices");
        const LogicalType arrayType{ValueType::Logic, 4, false, LogicDomain::FourState, {4}};
        makeState(graph, "nested", arrayType, {InitKind::Constant, "4'b0000"});
        const ValueId high = constant(graph, "high", 1, "1'b0");
        const ValueId current = readState(graph, "current", "nested", arrayType, {}, false);
        const ValueId packed = graph.createValue(graph.internSymbol("packed"), 5, false);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_op"));
        graph.addOperand(concat, high);
        graph.addOperand(concat, current);
        graph.addResult(concat, packed);
        const ValueId middle = staticSlice(graph, "middle", packed, 1, 2);
        staticSlice(graph, "out", middle, 1, 1, true);
        if (!split(graph, design)) return fail("nested view slices were not split");
        if (!dce(design, false)) return fail("nested view DCE failed");
        if (graph.findOperation("nested_0").valid() || graph.findOperation("nested_1").valid() ||
            !graph.findOperation("nested_2").valid() || graph.findOperation("nested_3").valid())
            return fail("nested view DCE retained the wrong state elements");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_cross_concat_slice");
        const LogicalType arrayType{ValueType::Logic, 4, false, LogicDomain::FourState, {4}};
        makeState(graph, "cross", arrayType, {InitKind::Constant, "4'b0000"});
        const ValueId high = constant(graph, "high", 1, "1'b0");
        const ValueId low = constant(graph, "low", 1, "1'b0");
        const ValueId current = readState(graph, "current", "cross", arrayType, {}, false);
        const ValueId packed = graph.createValue(graph.internSymbol("packed"), 6, false);
        const OperationId concat = graph.createOperation(OperationKind::kConcat,
                                                         graph.internSymbol("packed_op"));
        graph.addOperand(concat, high);
        graph.addOperand(concat, current);
        graph.addOperand(concat, low);
        graph.addResult(concat, packed);
        staticSlice(graph, "out", packed, 0, 3, true);
        if (!split(graph, design)) return fail("cross-concat slice was not split");
        if (!dce(design, false)) return fail("cross-concat DCE failed");
        if (!graph.findOperation("cross_0").valid() || !graph.findOperation("cross_1").valid() ||
            graph.findOperation("cross_2").valid() || graph.findOperation("cross_3").valid())
            return fail("cross-concat DCE retained the wrong state elements");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_writes");
        const LogicalType arrayType{ValueType::Logic, 16, false, LogicDomain::FourState, {2}};
        makeState(graph, "m", arrayType, {InitKind::Fill, "8'h00"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId one = constant(graph, "one", 32, "32'd1");
        const ValueId data0 = constant(graph, "data0", 8, "8'h12");
        const ValueId data1 = constant(graph, "data1", 8, "8'h34");
        const ValueId mask0 = constant(graph, "mask0", 8, "8'hff");
        const ValueId mask1 = constant(graph, "mask1", 8, "8'h0f");
        const ValueId next = constant(graph, "next", 16, "16'habcd");
        const ValueId mask = constant(graph, "mask", 16, "16'hff00");
        const std::array<ValueId, 4> firstOperands{enable, zero, data0, clock};
        writeState(graph, "first", "m", OperationKind::kRegisterWriteArrayPort, firstOperands, 3);
        const std::array<ValueId, 5> secondOperands{enable, one, data1, mask1, clock};
        const OperationId maskedElement = writeState(graph, "second", "m",
                                                     OperationKind::kRegisterWriteArrayMaskPort,
                                                     secondOperands, 4);
        const std::array<ValueId, 4> wholeOperands{enable, next, mask, clock};
        const OperationId wholeMask = writeState(graph, "whole", "m", OperationKind::kRegisterWriteMaskPort,
                                                 wholeOperands, 5);
        readState(graph, "out0", "m", {ValueType::Logic, 8, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        readState(graph, "out1", "m", {ValueType::Logic, 8, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&one, 1));
        if (!split(graph, design)) return fail("mixed state writes prevented state splitting");
        if (graph.opKind(maskedElement) != OperationKind::kRegisterWriteMaskPort ||
            graph.opOperands(maskedElement).size() != 4 ||
            graph.opAttrRange(maskedElement).find("regSymbol")->value().asString() != "m_1")
            return fail("masked array write did not become a scalar member write");
        if (graph.opKind(wholeMask) != OperationKind::kRegisterWriteMaskPort ||
            graph.opAttrRange(wholeMask).find("regSymbol")->value().asString() != "m_0")
            return fail("whole mask write was not assigned to member zero");
        for (const OperationId op : graph.operations())
        {
            const auto target = graph.opAttrRange(op).find("regSymbol");
            if (!target) continue;
            const std::string_view name = target->value().asString();
            if (name != "m_0" && name != "m_1") continue;
            const OperationKind kind = graph.opKind(op);
            if (kind == OperationKind::kRegisterReadPort) continue;
            if (kind != OperationKind::kRegisterWritePort && kind != OperationKind::kRegisterWriteMaskPort)
                return fail("an array or fill write survived state memberization");
            if (kind == OperationKind::kRegisterWriteMaskPort &&
                (graph.valueWidth(graph.opOperands(op)[1]) != 8 ||
                 graph.valueWidth(graph.opOperands(op)[2]) != 8))
                return fail("whole mask write data or mask was not sliced to member width");
        }
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_fill");
        const LogicalType arrayType{ValueType::Logic, 2, false, LogicDomain::FourState, {2}};
        makeState(graph, "f", arrayType, {InitKind::Constant, "2'b00"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId data = constant(graph, "data", 1, "1'b1");
        const std::array<ValueId, 3> fillOperands{enable, data, clock};
        const OperationId fill = writeState(graph, "fill", "f", OperationKind::kRegisterFillPort,
                                            fillOperands, 1);
        readState(graph, "out", "f", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        if (!split(graph, design)) return fail("fill write prevented state splitting");
        if (graph.opKind(fill) != OperationKind::kRegisterWritePort ||
            graph.opAttrRange(fill).find("regSymbol")->value().asString() != "f_0")
            return fail("fill was not converted to per-member ordinary writes");
        if (!dce(design, false)) return fail("fill-state DCE failed");
        if (!graph.findOperation("f_0").valid() || graph.findOperation("f_1").valid())
            return fail("DCE did not remove the unread fill member");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_deep_selection");
        const LogicalType arrayType{ValueType::Logic, 2, false, LogicDomain::FourState, {2}};
        makeState(graph, "deep", arrayType, {InitKind::Constant, "2'b00"});
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId element0 = constant(graph, "element0", 1, "1'b0");
        const ValueId element1 = constant(graph, "element1", 1, "1'b1");
        const std::array<ValueId, 2> elements{element0, element1};
        ValueId next = arrayCreate(graph, "packed", elements);
        for (int i = 0; i < 80; ++i)
            next = unary(graph, OperationKind::kAssign, "alias" + std::to_string(i), next);
        const std::array<ValueId, 3> writeOperands{enable, next, clock};
        const OperationId write = writeState(graph, "write", "deep", OperationKind::kRegisterWritePort,
                                             writeOperands, 0);
        readState(graph, "out", "deep", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        if (!split(graph, design)) return fail("deep state next-value chain was not split");
        const ValueId selected = graph.opOperands(write)[1];
        if (selected == next || (graph.opKind(graph.valueDef(selected)) == OperationKind::kSliceStatic &&
                                 graph.opOperands(graph.valueDef(selected))[0] == next))
            return fail("deep next-value chain fell back to a whole-value slice");
        if (selected != element0) return fail("deep next-value chain selected the wrong member");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_run_created_fallback");
        const LogicalType arrayType{ValueType::Logic, 4, false, LogicDomain::FourState, {2, 2}};
        makeState(graph, "queued", arrayType, {InitKind::Constant, "4'b0000"});
        const ValueId current = readState(graph, "current", "queued", arrayType, {}, false);
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        const ValueId high = constant(graph, "high", 1, "1'b0");
        const ValueId element = graph.createValue(
            graph.internSymbol("element"), {ValueType::Logic, 2, false, LogicDomain::FourState, {2}});
        const ValueId packed = graph.createValue(graph.internSymbol("packed"), 3, false);
        const ValueId out = staticSlice(graph, "out", packed, 0, 1, true);
        const OperationId concat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("packed_op"));
        graph.addOperand(concat, high);
        graph.addOperand(concat, element);
        graph.addResult(concat, packed);
        const OperationId indexed = graph.createOperation(OperationKind::kSliceArray,
                                                          graph.internSymbol("element_op"));
        graph.addOperand(indexed, current);
        graph.addOperand(indexed, zero);
        graph.addResult(indexed, element);
        if (!split(graph, design)) return fail("run-created static fallback was not rewritten");
        const OperationId outDef = graph.valueDef(out);
        if (graph.opKind(outDef) != OperationKind::kAssign || graph.opOperands(outDef).size() != 1)
            return fail("run-created static fallback did not become an element assignment");
        ValueId selected = graph.opOperands(outDef)[0];
        OperationId selectedDef = graph.valueDef(selected);
        for (int depth = 0; depth < 4 && selectedDef.valid() &&
             (graph.opKind(selectedDef) == OperationKind::kAssign ||
              graph.opKind(selectedDef) == OperationKind::kSliceStatic) &&
             graph.opOperands(selectedDef).size() == 1; ++depth)
        {
            selected = graph.opOperands(selectedDef)[0];
            selectedDef = graph.valueDef(selected);
        }
        const auto target = graph.opAttrRange(selectedDef).find("regSymbol");
        if (graph.opKind(selectedDef) != OperationKind::kRegisterReadPort || !target ||
            target->value().asString() != "queued_0_0")
            return fail("run-created static fallback did not resolve to the first row member");
        if (!dce(design, false)) return fail("run-created static fallback DCE failed");
        if (!graph.findOperation("queued_0_0").valid() || graph.findOperation("queued_0_1").valid() ||
            graph.findOperation("queued_1_0").valid() || graph.findOperation("queued_1_1").valid() ||
            graph.findValue("high").valid())
            return fail("run-created static fallback retained unread members or concat inputs");
    }

    {
        Design design;
        Graph &graph = design.createGraph("run_created_fallback_rollback");
        const ValueId condition = graph.createValue(graph.internSymbol("condition"), 1, false);
        graph.bindInputPort("condition", condition);
        const ValueId trueValue = graph.createValue(graph.internSymbol("true_value"), 2, false);
        graph.bindInputPort("true_value", trueValue);
        ValueId falseValue = graph.createValue(graph.internSymbol("false_value"), 2, false);
        graph.bindInputPort("false_value", falseValue);
        const ValueId array = graph.createValue(graph.internSymbol("array"), 2, false,
                                                ValueType::Logic, std::array<int64_t, 1>{2});
        const ValueId out = staticSlice(graph, "out", array, 0, 1, true);
        for (int i = 0; i < 300; ++i)
            falseValue = unary(graph, OperationKind::kAssign, "alias" + std::to_string(i), falseValue);
        const OperationId mux = graph.createOperation(OperationKind::kMux, graph.internSymbol("array_op"));
        for (const ValueId operand : {condition, trueValue, falseValue}) graph.addOperand(mux, operand);
        graph.addResult(mux, array);
        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || result.changed ||
            graph.opKind(graph.valueDef(out)) != OperationKind::kSliceStatic)
            return fail("rolled-back run-created fallback was not removed from the worklist");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_symbol_collision");
        makeState(graph, "r", {ValueType::Logic, 3, false, LogicDomain::FourState, {3}},
                  {InitKind::Constant, "3'b000"});
        const OperationId other = makeState(graph, "r_2", {ValueType::Logic, 2, false, LogicDomain::FourState, {2}},
                                            {InitKind::Constant, "2'b00"});
        graph.addDeclaredSymbol(graph.operationSymbol(other));
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        readState(graph, "first_out", "r", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        const ValueId secondRead = readState(
            graph, "second_out", "r_2", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
            std::span<const ValueId>(&zero, 1));
        const ValueId enable = constant(graph, "enable", 1, "1'b1");
        const ValueId clock = constant(graph, "clock", 1, "1'b1");
        const ValueId next = constant(graph, "next", 2, "2'b01");
        const std::array<ValueId, 3> writeOperands{enable, next, clock};
        const OperationId otherWrite = writeState(graph, "other_write", "r_2",
                                                  OperationKind::kRegisterWritePort, writeOperands, 0);
        if (!split(graph, design)) return fail("state symbol collision prevented all splitting");
        if (!graph.findOperation("r_0").valid() || !graph.findOperation("r_1").valid() ||
            !graph.findOperation("r_2").valid() || graph.findOperation("r_2_0").valid() ||
            !graph.findOperation("r_2_0_0").valid() || !graph.findOperation("r_2_0_1").valid())
            return fail("colliding state declarations were not renamed and split correctly");
        if (graph.opAttrRange(graph.valueDef(secondRead)).find("regSymbol")->value().asString() != "r_2_0_0" ||
            graph.opAttrRange(otherWrite).find("regSymbol")->value().asString() != "r_2_0_0")
            return fail("state symbol rename did not update register references");
        if (!graph.isDeclaredSymbol(graph.lookupSymbol("r_2_0_0")) ||
            !graph.isDeclaredSymbol(graph.lookupSymbol("r_2_0_1")))
            return fail("state symbol rename did not preserve declared symbols");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_value_collision");
        const ValueId existing = constant(graph, "v_0", 1, "1'b1");
        makeState(graph, "v", {ValueType::Logic, 2, false, LogicDomain::FourState, {2}},
                  {InitKind::Constant, "2'b00"});
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        readState(graph, "out", "v", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        if (!split(graph, design)) return fail("value symbol collision prevented state splitting");
        if (!graph.findOperation("v_0").valid() || !graph.findValue("v_0_0").valid() ||
            graph.findValue("v_0").valid() || graph.valueDef(existing) != graph.valueDef(graph.findValue("v_0_0")))
            return fail("value symbol collision was not renamed without changing its definition");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_dynamic");
        const LogicalType arrayType{ValueType::Logic, 2, false, LogicDomain::FourState, {2}};
        makeState(graph, "d", arrayType, {InitKind::Constant, "2'b00"});
        const ValueId index = graph.createValue(graph.internSymbol("index"), 1, false);
        graph.bindInputPort("index", index);
        readState(graph, "out", "d", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&index, 1));
        PassManager manager;
        manager.addPass(std::make_unique<SplitStaticArrayPass>());
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diagnostics;
        const PassManagerResult result = manager.run(design, diagnostics);
        if (!result.success || diagnostics.hasError() || result.changed || !graph.findOperation("d").valid() ||
            graph.findOperation("d_0").valid())
            return fail("dynamic state array was not left intact");
    }

    {
        Design design;
        Graph &graph = design.createGraph("state_declared");
        const LogicalType arrayType{ValueType::Logic, 2, false, LogicDomain::FourState, {2}};
        const OperationId state = makeState(graph, "p", arrayType, {InitKind::Constant, "2'b00"});
        graph.addDeclaredSymbol(graph.operationSymbol(state));
        const ValueId zero = constant(graph, "zero", 32, "32'd0");
        readState(graph, "out", "p", {ValueType::Logic, 1, false, LogicDomain::FourState, {}},
                  std::span<const ValueId>(&zero, 1));
        {
            PassManager manager;
            manager.options().keepDeclaredSymbols = true;
            manager.addPass(std::make_unique<SplitStaticArrayPass>());
            PassDiagnostics diagnostics;
            const PassManagerResult result = manager.run(design, diagnostics);
            if (!result.success || diagnostics.hasError() || result.changed)
                return fail("explicit declaration protection did not skip state replacement");
        }
        if (!split(graph, design, false)) return fail("declared state was not split by the default policy");
        if (!dce(design, true)) return fail("member declaration protection DCE failed");
        if (!graph.isDeclaredSymbol(graph.lookupSymbol("p_0")) ||
            !graph.isDeclaredSymbol(graph.lookupSymbol("p_1")) ||
            graph.isDeclaredSymbol(graph.lookupSymbol("p")))
            return fail("declared symbols were not transferred to state members");
    }

    {
        std::string error;
        if (!makePass("split-static-array", {}, error)) return fail("pass factory registration is missing");
        if (makePass("split-static-array", std::array<std::string_view, 1>{"-unexpected"}, error))
            return fail("pass factory accepted an unknown option");
    }
    return 0;
}
