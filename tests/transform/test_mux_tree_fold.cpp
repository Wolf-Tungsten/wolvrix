#include "core/grh.hpp"
#include "transform/mux_tree_fold.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[mux-tree-fold-tests] " << message << '\n';
        return 1;
    }

    namespace grh = wolvrix::lib::grh;

    grh::ValueId makeConst(grh::Graph &graph, const std::string &name, int64_t width,
                           const std::string &literal)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kConstant, graph.internSymbol(name + "_op"));
        graph.addResult(op, val);
        graph.setAttr(op, "constValue", literal);
        return val;
    }

    grh::ValueId makeInput(grh::Graph &graph, const std::string &name, int64_t width)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false);
        graph.bindInputPort(name, val);
        return val;
    }

    grh::OperationId makeRegisterArray(grh::Graph &graph, const std::string &sym,
                                       int64_t elemWidth, int64_t elems)
    {
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kRegister, graph.internSymbol(sym));
        graph.setAttr(op, "width", elems * elemWidth);
        graph.setAttr(op, "shape", std::vector<int64_t>{elems});
        return op;
    }

    grh::ValueId makeReadElem(grh::Graph &graph, const std::string &regSym,
                              const std::string &name, grh::ValueId constIndex,
                              int64_t elemWidth)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(elemWidth), false);
        const grh::OperationId op = graph.createOperation(
            grh::OperationKind::kRegisterReadArrayPort, graph.internSymbol(name + "_op"));
        graph.addOperand(op, constIndex);
        graph.addResult(op, val);
        graph.setAttr(op, "regSymbol", regSym);
        return val;
    }

    grh::ValueId makeOp1(grh::Graph &graph, grh::OperationKind kind, const std::string &name,
                         int64_t width, grh::ValueId a)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false);
        const grh::OperationId op = graph.createOperation(kind, graph.internSymbol(name + "_op"));
        graph.addOperand(op, a);
        graph.addResult(op, val);
        return val;
    }

    grh::ValueId makeOp2(grh::Graph &graph, grh::OperationKind kind, const std::string &name,
                         int64_t width, grh::ValueId a, grh::ValueId b)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false);
        const grh::OperationId op = graph.createOperation(kind, graph.internSymbol(name + "_op"));
        graph.addOperand(op, a);
        graph.addOperand(op, b);
        graph.addResult(op, val);
        return val;
    }

    grh::ValueId makeMux(grh::Graph &graph, const std::string &name, int64_t width,
                         grh::ValueId cond, grh::ValueId thenV, grh::ValueId elseV)
    {
        const grh::ValueId val =
            graph.createValue(graph.internSymbol(name), static_cast<int32_t>(width), false);
        const grh::OperationId op =
            graph.createOperation(grh::OperationKind::kMux, graph.internSymbol(name + "_op"));
        graph.addOperand(op, cond);
        graph.addOperand(op, thenV);
        graph.addOperand(op, elseV);
        graph.addResult(op, val);
        return val;
    }

    void makeArrayWrite(grh::Graph &graph, const std::string &name, const std::string &regSym,
                        grh::ValueId enable, grh::ValueId index, grh::ValueId data,
                        grh::ValueId clock)
    {
        const grh::OperationId op = graph.createOperation(
            grh::OperationKind::kRegisterWriteArrayPort, graph.internSymbol(name));
        graph.addOperand(op, enable);
        graph.addOperand(op, index);
        graph.addOperand(op, data);
        graph.addOperand(op, clock);
        graph.setAttr(op, "regSymbol", regSym);
        graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
    }

    int countKind(grh::Graph &graph, grh::OperationKind kind)
    {
        int count = 0;
        for (const grh::OperationId id : graph.operations())
        {
            if (id.valid() && graph.opKind(id) == kind)
            {
                ++count;
            }
        }
        return count;
    }

    grh::OperationId findWritePortWithDynamicIndex(grh::Graph &graph, const std::string &regSym)
    {
        for (const grh::OperationId id : graph.operations())
        {
            if (!id.valid() || graph.opKind(id) != grh::OperationKind::kRegisterWriteArrayPort)
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(id);
            const auto attr = op.attr("regSymbol");
            if (!attr || std::get_if<std::string>(&*attr) == nullptr ||
                *std::get_if<std::string>(&*attr) != regSym)
            {
                continue;
            }
            const auto indexConst = op.operands()[1];
            const grh::OperationId indexDef = graph.valueDef(indexConst);
            if (!indexDef.valid() || graph.opKind(indexDef) != grh::OperationKind::kConstant)
            {
                return id;
            }
        }
        return grh::OperationId::invalid();
    }

    // Pattern R: constant-index read mux tree folds to one dynamic array read.
    int testReadTreeFold()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        makeRegisterArray(graph, "arr", 8, 4);
        grh::ValueId ptr = makeInput(graph, "p", 2);
        grh::ValueId out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);

        // mux(p==0, arr[0], mux(p==1, arr[1], mux(p==2, arr[2], arr[3])))
        grh::ValueId leaf[4];
        for (int64_t e = 0; e < 4; ++e)
        {
            leaf[e] = makeReadElem(graph, "arr", "r" + std::to_string(e),
                                   makeConst(graph, "c" + std::to_string(e), 2,
                                             "2'h" + std::to_string(e)),
                                   8);
        }
        grh::ValueId chain = leaf[3];
        for (int64_t e = 2; e >= 0; --e)
        {
            grh::ValueId cond = makeOp2(graph, grh::OperationKind::kEq,
                                        "eq" + std::to_string(e), 1, ptr,
                                        makeConst(graph, "k" + std::to_string(e), 2,
                                                  "2'h" + std::to_string(e)));
            chain = makeMux(graph, "t" + std::to_string(e), 8, cond, leaf[e], chain);
        }
        const grh::OperationId drive =
            graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("drive_out"));
        graph.addOperand(drive, chain);
        graph.addResult(drive, out);

        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>());
        PassDiagnostics diags;
        PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("read-tree: pass run failed");
        }
        if (countKind(graph, grh::OperationKind::kMux) != 0)
        {
            return fail("read-tree: mux ops remain");
        }
        if (countKind(graph, grh::OperationKind::kEq) != 0)
        {
            return fail("read-tree: eq ops remain");
        }
        // exactly one read port with a dynamic (non-constant) index
        int dynReads = 0;
        for (const grh::OperationId id : graph.operations())
        {
            if (!id.valid() ||
                graph.opKind(id) != grh::OperationKind::kRegisterReadArrayPort)
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(id);
            const grh::OperationId idxDef = graph.valueDef(op.operands()[0]);
            if (!idxDef.valid() || graph.opKind(idxDef) != grh::OperationKind::kConstant)
            {
                ++dynReads;
            }
        }
        if (dynReads != 1)
        {
            return fail("read-tree: expected 1 dynamic read, got " + std::to_string(dynReads));
        }
        return 0;
    }

    // Pattern W: per-entry decoder writes fold to dynamic write ports + fill.
    int testWriteDecoderFold()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        makeRegisterArray(graph, "rarr", 8, 4);
        grh::ValueId clock = makeInput(graph, "clock", 1);
        grh::ValueId reset = makeInput(graph, "reset", 1);
        grh::ValueId notReset = makeOp1(graph, grh::OperationKind::kLogicNot, "not_reset", 1, reset);
        grh::ValueId p0 = makeInput(graph, "p0", 2);
        grh::ValueId p1 = makeInput(graph, "p1", 2);
        grh::ValueId en0 = makeInput(graph, "en0", 1);
        grh::ValueId en1 = makeInput(graph, "en1", 1);
        grh::ValueId d0 = makeInput(graph, "d0", 8);
        grh::ValueId d1 = makeInput(graph, "d1", 8);

        for (int64_t e = 0; e < 4; ++e)
        {
            const std::string es = std::to_string(e);
            grh::ValueId idxC = makeConst(graph, "we" + es, 2, "2'h" + std::to_string(e));
            makeArrayWrite(graph, "rst_w" + es, "rarr", reset, idxC,
                           makeConst(graph, "zero" + es, 8, "8'h0"), clock);
            // chain: mux(en1 & (p1==e), d1, mux(en0 & (p0==e), d0, rarr[e]))
            grh::ValueId eq1 = makeOp2(graph, grh::OperationKind::kEq, "eq1_" + es, 1, p1,
                                       makeConst(graph, "e1c" + es, 2, "2'h" + es));
            grh::ValueId hit1 = makeOp2(graph, grh::OperationKind::kAnd, "hit1_" + es, 1, en1, eq1);
            grh::ValueId eq0 = makeOp2(graph, grh::OperationKind::kEq, "eq0_" + es, 1, p0,
                                       makeConst(graph, "e0c" + es, 2, "2'h" + es));
            grh::ValueId hit0 = makeOp2(graph, grh::OperationKind::kAnd, "hit0_" + es, 1, en0, eq0);
            grh::ValueId hold = makeReadElem(graph, "rarr", "hold" + es, idxC, 8);
            grh::ValueId inner = makeMux(graph, "inner" + es, 8, hit0, d0, hold);
            grh::ValueId chain = makeMux(graph, "chain" + es, 8, hit1, d1, inner);
            makeArrayWrite(graph, "else_w" + es, "rarr", notReset, idxC, chain, clock);
        }

        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>());
        PassDiagnostics diags;
        PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("write-decoder: pass run failed");
        }
        // 8 original writes gone; expect 2 dynamic write ports + 1 reset fill
        int writePorts = 0;
        int fills = 0;
        std::vector<std::string> order;
        for (const grh::OperationId id : graph.operations())
        {
            if (!id.valid())
            {
                continue;
            }
            const grh::OperationKind kind = graph.opKind(id);
            if (kind == grh::OperationKind::kRegisterWriteArrayPort)
            {
                ++writePorts;
                order.push_back("write");
            }
            else if (kind == grh::OperationKind::kRegisterFillPort)
            {
                ++fills;
                order.push_back("fill");
            }
        }
        if (writePorts != 2 || fills != 1)
        {
            return fail("write-decoder: expected 2 write ports + 1 fill, got " +
                        std::to_string(writePorts) + "/" + std::to_string(fills));
        }
        // declaration order = priority: port0 (innermost), port1, reset fill last
        if (order.size() != 3 || order[0] != "write" || order[1] != "write" ||
            order[2] != "fill")
        {
            return fail("write-decoder: bad priority order");
        }
        // hold reads / mux chains swept
        if (countKind(graph, grh::OperationKind::kMux) != 0 ||
            countKind(graph, grh::OperationKind::kEq) != 0)
        {
            return fail("write-decoder: chain cone not swept");
        }
        return 0;
    }

    // Pattern P: padded-concat dynamic read folds to clamped array read.
    int testPadReadFold()
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("g");
        makeRegisterArray(graph, "parr", 8, 4);
        grh::ValueId idx = makeInput(graph, "pi", 3); // 0..7 over 6-element pad concat
        grh::ValueId out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);

        // padded = concat(read(0), read(0), read(3), read(2), read(1), read(0))
        // => element map [0,1,2,3,0,0] (identity on [0,4), pad elem 0 above)
        auto rd = [&](int64_t e, const std::string &name) {
            return makeReadElem(graph, "parr", name,
                                makeConst(graph, "pc_" + name, 2, "2'h" + std::to_string(e)), 8);
        };
        const grh::ValueId concatVal = graph.createValue(graph.internSymbol("padcat"), 48, false);
        const grh::OperationId concat =
            graph.createOperation(grh::OperationKind::kConcat, graph.internSymbol("padcat_op"));
        graph.addOperand(concat, rd(0, "pe5")); // element 5 (pad)
        graph.addOperand(concat, rd(0, "pe4")); // element 4 (pad)
        graph.addOperand(concat, rd(3, "pe3"));
        graph.addOperand(concat, rd(2, "pe2"));
        graph.addOperand(concat, rd(1, "pe1"));
        graph.addOperand(concat, rd(0, "pe0"));
        graph.addResult(concat, concatVal);
        const grh::ValueId sliceVal = graph.createValue(graph.internSymbol("pread"), 8, false);
        const grh::OperationId slice =
            graph.createOperation(grh::OperationKind::kSliceArray, graph.internSymbol("pread_op"));
        graph.addOperand(slice, concatVal);
        graph.addOperand(slice, idx);
        graph.addResult(slice, sliceVal);
        const grh::OperationId drive =
            graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("drive_out"));
        graph.addOperand(drive, sliceVal);
        graph.addResult(drive, out);
        (void)out;

        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>());
        PassDiagnostics diags;
        PassManagerResult res = manager.run(design, diags);
        if (!res.success || diags.hasError())
        {
            return fail("pad-read: pass run failed");
        }
        if (countKind(graph, grh::OperationKind::kConcat) != 0)
        {
            return fail("pad-read: concat cone not swept");
        }
        if (countKind(graph, grh::OperationKind::kLt) != 1 ||
            countKind(graph, grh::OperationKind::kMux) != 1)
        {
            return fail("pad-read: expected clamp lt+mux");
        }
        int dynReads = 0;
        for (const grh::OperationId opId : graph.operations())
        {
            if (!opId.valid() ||
                graph.opKind(opId) != grh::OperationKind::kRegisterReadArrayPort)
            {
                continue;
            }
            const grh::Operation op = graph.getOperation(opId);
            const grh::OperationId idxDef = graph.valueDef(op.operands()[0]);
            if (idxDef.valid() && graph.opKind(idxDef) == grh::OperationKind::kMux)
            {
                ++dynReads;
            }
        }
        if (dynReads != 1)
        {
            return fail("pad-read: expected clamped dynamic read");
        }
        return 0;
    }

} // namespace

int main()
{
    if (int rc = testReadTreeFold())
    {
        return rc;
    }
    if (int rc = testWriteDecoderFold())
    {
        return rc;
    }
    if (int rc = testPadReadFold())
    {
        return rc;
    }
    std::cout << "[mux-tree-fold-tests] all tests passed\n";
    return 0;
}
