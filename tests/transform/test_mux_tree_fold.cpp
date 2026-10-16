#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/mux_tree_fold.hpp"
#include "transform/register_write_normalize.hpp"
#include "transform/verify.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
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
        graph.setAttr(op, "isSigned", false);
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

    // Small independent two-state evaluator: every write samples the same
    // pre-event state and declaration order resolves write collisions.
    using Inputs = std::map<uint32_t, uint64_t>;
    std::vector<uint64_t> evaluateWrites(grh::Graph &graph, const Inputs &inputs,
                                        const std::vector<uint64_t> &old)
    {
        std::map<uint32_t, uint64_t> cache;
        std::function<uint64_t(grh::ValueId)> eval = [&](grh::ValueId value) -> uint64_t {
            if (auto it = inputs.find(value.index); it != inputs.end()) return it->second;
            if (auto it = cache.find(value.index); it != cache.end()) return it->second;
            const auto def = graph.valueDef(value);
            if (!def.valid()) throw std::runtime_error("unbound evaluator input");
            const auto op = graph.getOperation(def);
            const auto args = op.operands();
            uint64_t result = 0;
            switch (op.kind())
            {
            case grh::OperationKind::kConstant:
            {
                const auto text = std::get<std::string>(*op.attr("constValue"));
                const auto tick = text.find('\'');
                const int base = tick == std::string::npos ? 10 : text[tick + 1] == 'h' ? 16 :
                                 text[tick + 1] == 'b' ? 2 : 10;
                result = std::stoull(text.substr(tick == std::string::npos ? 0 : tick + 2), nullptr, base);
                break;
            }
            case grh::OperationKind::kAssign: result = eval(args[0]); break;
            case grh::OperationKind::kMux: result = eval(args[0]) ? eval(args[1]) : eval(args[2]); break;
            case grh::OperationKind::kAnd:
            case grh::OperationKind::kLogicAnd: result = eval(args[0]) & eval(args[1]); break;
            case grh::OperationKind::kOr:
            case grh::OperationKind::kLogicOr: result = eval(args[0]) | eval(args[1]); break;
            case grh::OperationKind::kNot: result = ~eval(args[0]); break;
            case grh::OperationKind::kLogicNot: result = !eval(args[0]); break;
            case grh::OperationKind::kReduceOr: result = eval(args[0]) != 0; break;
            case grh::OperationKind::kConcat:
                for (auto arg : args) result = (result << graph.valueWidth(arg)) | eval(arg);
                break;
            case grh::OperationKind::kEq: result = eval(args[0]) == eval(args[1]); break;
            case grh::OperationKind::kNe: result = eval(args[0]) != eval(args[1]); break;
            case grh::OperationKind::kRegisterReadArrayPort:
            {
                const auto row = eval(args[0]);
                result = row < old.size() ? old[row] : 0;
                break;
            }
            default: throw std::runtime_error("unsupported evaluator operation");
            }
            const auto width = graph.valueWidth(value);
            if (width < 64) result &= (UINT64_C(1) << width) - 1;
            cache[value.index] = result;
            return result;
        };
        auto next = old;
        for (auto id : graph.operations())
        {
            const auto op = graph.getOperation(id);
            if (!grh::isRegisterWritePortKind(op.kind())) continue;
            const auto args = op.operands();
            if (!eval(args[0])) continue;
            if (op.kind() == grh::OperationKind::kRegisterWriteArrayPort)
            {
                auto row = eval(args[1]);
                if (row < next.size()) next[row] = eval(args[2]);
            }
            else if (op.kind() == grh::OperationKind::kRegisterWritePort)
            {
                const auto packed = eval(args[1]);
                for (std::size_t row = 0; row < next.size(); ++row)
                    next[row] = (packed >> (row * 8)) & 255;
            }
            else if (op.kind() == grh::OperationKind::kRegisterFillPort)
                std::fill(next.begin(), next.end(), eval(args[1]));
            else throw std::runtime_error("unsupported evaluator write");
        }
        return next;
    }

    int testGuardedWriteNormalization(bool withOr)
    {
        grh::Design design;
        auto &graph = design.createGraph("guarded");
        makeRegisterArray(graph, "arr", 8, 4);
        const auto clock = makeInput(graph, "clk", 1);
        const auto reset = makeInput(graph, "rst", 1);
        const auto notReset = makeOp1(graph, grh::OperationKind::kNot, "nr", 1, reset);
        const auto one = makeConst(graph, "one", 1, "1'b1");
        grh::ValueId ptr[4], valid[4], data[3];
        for (int p = 0; p < 4; ++p)
        {
            ptr[p] = makeInput(graph, "p" + std::to_string(p), 3);
            valid[p] = makeInput(graph, "v" + std::to_string(p), 1);
            if (p < 3) data[p] = makeInput(graph, "d" + std::to_string(p), 8);
        }
        // Whole reset is deliberately mixed with the decoder group.
        const auto resetOp = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                    graph.internSymbol("reset_write"));
        graph.addOperand(resetOp, reset);
        graph.addOperand(resetOp, makeConst(graph, "zeros", 32, "32'h0"));
        graph.addOperand(resetOp, clock);
        graph.setAttr(resetOp, "regSymbol", std::string("arr"));
        graph.setAttr(resetOp, "eventEdge", std::vector<std::string>{"posedge"});
        for (int row = 0; row < 4; ++row)
        {
            const auto suffix = std::to_string(row);
            const auto idx = makeConst(graph, "idx" + suffix, 3, "3'd" + suffix);
            grh::ValueId hit[4];
            for (int p = 0; p < 4; ++p)
            {
                const auto tag = std::to_string(p) + "_" + suffix;
                const auto eq = row == 0
                    ? makeOp1(graph, grh::OperationKind::kNot, "eq" + tag, 1,
                        makeOp1(graph, grh::OperationKind::kReduceOr, "nonzero" + tag, 1, ptr[p]))
                    : makeOp2(graph, grh::OperationKind::kEq, "eq" + tag, 1, ptr[p], idx);
                hit[p] = makeOp2(graph, grh::OperationKind::kAnd, "hit" + tag, 1, valid[p], eq);
            }
            const auto top = withOr ? makeOp2(graph, grh::OperationKind::kOr, "or" + suffix, 1, hit[2], hit[3]) : hit[2];
            const auto topEnable = makeOp2(graph, grh::OperationKind::kAnd, "top_en" + suffix, 1, notReset, top);
            const auto topData = withOr ? makeConst(graph, "zero_data" + suffix, 8, "8'h0") : data[2];
            if (row != 0)
                makeArrayWrite(graph, "top_w" + suffix, "arr", topEnable, idx, topData, clock);
            const auto noTop = makeOp1(graph, grh::OperationKind::kNot, "no_top" + suffix, 1, top);
            const auto elseEnable = makeOp2(graph, grh::OperationKind::kAnd, "else_en" + suffix, 1, notReset, noTop);
            const auto middleEnable = makeOp2(graph, grh::OperationKind::kAnd, "mid_en" + suffix, 1, elseEnable, hit[1]);
            const auto noMiddle = makeOp1(graph, grh::OperationKind::kNot, "no_mid" + suffix, 1, hit[1]);
            const auto tailEnable = makeOp2(graph, grh::OperationKind::kAnd, "tail_en" + suffix, 1, elseEnable, noMiddle);
            const auto hold = makeReadElem(graph, "arr", "hold" + suffix, idx, 8);
            if (withOr && row == 0) graph.bindOutputPort("old_out", hold);
            const auto tail = makeMux(graph, "tail" + suffix, 8, hit[0], data[0], hold);
            if (row < 2)
            {
                auto middle = makeMux(graph, "middle" + suffix, 8, hit[1], data[1], tail);
                if (row == 0) middle = makeMux(graph, "top_mux" + suffix, 8, top, topData, middle);
                makeArrayWrite(graph, "tail_w" + suffix, "arr", row == 0 ? notReset : elseEnable, idx, middle, clock);
            }
            else
            {
                makeArrayWrite(graph, "mid_w" + suffix, "arr", middleEnable, idx, data[1], clock);
                makeArrayWrite(graph, "tail_w" + suffix, "arr", tailEnable, idx, tail, clock);
            }
        }
        const auto output = makeReadElem(graph, "arr", "read_output", ptr[0], 8);
        graph.bindOutputPort("out", output);
        struct Case { Inputs inputs; std::vector<uint64_t> old, expected; };
        std::vector<Case> cases;
        uint64_t random = 7;
        for (int trial = 0; trial < 4096; ++trial)
        {
            auto nextRandom = [&] { random = random * UINT64_C(6364136223846793005) + 1; return random >> 32; };
            Case c;
            c.inputs[clock.index] = 1;
            c.inputs[reset.index] = trial % 17 == 0;
            c.inputs[one.index] = 1;
            for (int p = 0; p < 4; ++p)
            {
                c.inputs[ptr[p].index] = nextRandom() & 7;
                c.inputs[valid[p].index] = nextRandom() & 1;
                if (p < 3) c.inputs[data[p].index] = nextRandom() & 255;
            }
            for (int row = 0; row < 4; ++row) c.old.push_back(nextRandom() & 255);
            c.expected = evaluateWrites(graph, c.inputs, c.old);
            cases.push_back(std::move(c));
        }
        const auto originalWrites = countKind(graph, grh::OperationKind::kRegisterWriteArrayPort);
        PassManager normalizer;
        normalizer.addPass(std::make_unique<RegisterWriteNormalizePass>(true));
        PassDiagnostics diags;
        if (!normalizer.run(design, diags).success || diags.hasError()) return fail("guarded writes: normalization failed");
        if (countKind(graph, grh::OperationKind::kRegisterWriteArrayPort) > originalWrites)
            return fail("guarded writes: normalizer expanded write ports");
        for (const auto &c : cases)
            if (evaluateWrites(graph, c.inputs, c.old) != c.expected)
                return fail("guarded writes: normalization changed state transition");
        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>(true));
        manager.addPass(std::make_unique<GrhVerifyPass>());
        if (!manager.run(design, diags).success || diags.hasError())
        {
            for (const auto &diag : diags.messages())
                std::cerr << diag.passName << ": " << diag.message << " [" << diag.context << "]\n";
            return fail("guarded writes: pipeline failed");
        }
        const auto expectedPorts = withOr ? 4 : 3;
        if (countKind(graph, grh::OperationKind::kRegisterWriteArrayPort) != expectedPorts)
            return fail("guarded writes: expected " + std::to_string(expectedPorts) + " dynamic ports, got " +
                        std::to_string(countKind(graph, grh::OperationKind::kRegisterWriteArrayPort)));
        if (countKind(graph, grh::OperationKind::kRegisterReadArrayPort) != (withOr ? 2 : 1))
            return fail("guarded writes: hold reads remain or functional read lost");
        if (countKind(graph, grh::OperationKind::kRegisterWritePort) != 1)
            return fail("guarded writes: whole reset lost");
        for (const auto &c : cases)
            if (evaluateWrites(graph, c.inputs, c.old) != c.expected)
                return fail("guarded writes: before/after transition mismatch");
        return 0;
    }

    int testIdentityOverridesEarlierWrite()
    {
        grh::Design design;
        auto &graph = design.createGraph("identity_priority");
        makeRegisterArray(graph, "arr", 8, 4);
        const auto clock = makeInput(graph, "clk", 1);
        const auto condition = makeInput(graph, "condition", 1);
        const auto data = makeInput(graph, "data", 8);
        const auto index = makeConst(graph, "idx", 2, "2'd0");
        const auto one = makeConst(graph, "one", 1, "1'b1");
        makeArrayWrite(graph, "first", "arr", one, index, data, clock);
        const auto hold = makeReadElem(graph, "arr", "old", index, 8);
        const auto selected = makeMux(graph, "last_data", 8, condition,
                                      makeConst(graph, "last_value", 8, "8'd99"), hold);
        makeArrayWrite(graph, "last", "arr", one, index, selected, clock);
        PassManager manager;
        manager.addPass(std::make_unique<RegisterWriteNormalizePass>(true));
        PassDiagnostics diags;
        const auto res = manager.run(design, diags);
        if (!res.success || res.changed) return fail("priority: identity overwrite must remain");
        Inputs inputs{{clock.index, 1}, {condition.index, 0}, {data.index, 42}};
        if (evaluateWrites(graph, inputs, {7, 8, 9, 10})[0] != 7)
            return fail("priority: higher-priority self assignment exposed earlier data");
        return 0;
    }

    int testTwoStateOptIn()
    {
        std::string error;
        if (makePass("register-write-normalize", {}, error))
            return fail("normalization must require explicit two-state semantics");
        const std::string_view arg = "-semantics=2state";
        if (!makePass("register-write-normalize", std::span(&arg, 1), error))
            return fail("normalization two-state factory option failed");
        grh::Design design;
        auto &graph = design.createGraph("default_semantics");
        makeRegisterArray(graph, "arr", 8, 4);
        const auto index = makeConst(graph, "i", 2, "2'd0");
        const auto enable = makeConst(graph, "en", 1, "1'b1");
        const auto hold = makeReadElem(graph, "arr", "old", index, 8);
        makeArrayWrite(graph, "identity", "arr", enable, index, hold, makeInput(graph, "clk", 1));
        PassManager manager;
        manager.addPass(std::make_unique<RegisterWriteNormalizePass>());
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || result.changed || countKind(graph, grh::OperationKind::kRegisterWriteArrayPort) != 1)
            return fail("normalization default must preserve four-state behavior");
        return 0;
    }

    int testGuardedFoldRejections()
    {
        for (int scenario = 0; scenario < 4; ++scenario)
        {
            grh::Design design;
            auto &graph = design.createGraph("guarded_reject");
            makeRegisterArray(graph, "arr", 8, 4);
            const auto clock = makeInput(graph, "clk", 1);
            const auto otherClock = makeInput(graph, "clk2", 1);
            const auto pointer = makeInput(graph, "p", 2);
            const auto pointer2 = makeInput(graph, "p2", 2);
            const auto data = makeInput(graph, "d", 8);
            const auto data2 = makeInput(graph, "d2", 8);
            const int rows = scenario == 0 ? 3 : 4;
            for (int row = 0; row < rows; ++row)
            {
                const auto suffix = std::to_string(row);
                const auto index = makeConst(graph, "i" + suffix, 2, "2'd" + suffix);
                const auto hit = makeOp2(graph, grh::OperationKind::kEq, "eq" + suffix, 1, pointer, index);
                const auto hit2 = makeOp2(graph, grh::OperationKind::kEq, "eq2_" + suffix, 1, pointer2, index);
                const auto rowData = scenario == 1 ? makeInput(graph, "row_data" + suffix, 8) : data;
                if (scenario == 3 && row % 2 == 0)
                    makeArrayWrite(graph, "w2_" + suffix, "arr", hit2, index, data2, clock);
                makeArrayWrite(graph, "w" + suffix, "arr", hit, index, rowData,
                               scenario == 2 && row == 0 ? otherClock : clock);
                if (scenario == 3 && row % 2 != 0)
                    makeArrayWrite(graph, "w2_" + suffix, "arr", hit2, index, data2, clock);
            }
            PassManager manager;
            manager.addPass(std::make_unique<MuxTreeFoldPass>(true));
            manager.addPass(std::make_unique<GrhVerifyPass>());
            PassDiagnostics diags;
            if (!manager.run(design, diags).success || diags.hasError())
                return fail("guarded reject: invalid resulting graph");
            if (findWritePortWithDynamicIndex(graph, "arr").valid())
                return fail("guarded reject: folded unsafe scenario " + std::to_string(scenario));
        }
        return 0;
    }

    int testWholeWriteBarrier()
    {
        grh::Design design;
        auto &graph = design.createGraph("guarded_barrier");
        makeRegisterArray(graph, "arr", 8, 4);
        const auto clock = makeInput(graph, "clk", 1);
        const auto reset = makeInput(graph, "rst", 1);
        grh::ValueId ptr[2], data[2];
        for (int source = 0; source < 2; ++source)
        {
            ptr[source] = makeInput(graph, "p" + std::to_string(source), 3);
            data[source] = makeInput(graph, "d" + std::to_string(source), 8);
            if (source == 1)
            {
                const auto whole = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                         graph.internSymbol("whole"));
                graph.addOperand(whole, reset);
                graph.addOperand(whole, makeConst(graph, "nonuniform", 32, "32'h11223344"));
                graph.addOperand(whole, clock);
                graph.setAttr(whole, "regSymbol", std::string("arr"));
                graph.setAttr(whole, "eventEdge", std::vector<std::string>{"posedge"});
            }
            for (int row = 0; row < 4; ++row)
            {
                const auto suffix = std::to_string(source) + "_" + std::to_string(row);
                const auto index = makeConst(graph, "i" + suffix, 3, "3'd" + std::to_string(row));
                const auto hit = makeOp2(graph, grh::OperationKind::kEq, "hit" + suffix, 1, ptr[source], index);
                makeArrayWrite(graph, "w" + suffix, "arr", hit, index, data[source], clock);
            }
        }
        struct Case { Inputs inputs; std::vector<uint64_t> expected; };
        const std::vector<uint64_t> old{1, 2, 3, 4};
        std::vector<Case> cases;
        for (unsigned a = 0; a < 8; ++a)
            for (unsigned b = 0; b < 8; ++b)
                for (unsigned rst = 0; rst < 2; ++rst)
                {
                    Inputs inputs{{clock.index, 1}, {reset.index, rst},
                                  {ptr[0].index, a}, {ptr[1].index, b},
                                  {data[0].index, 91}, {data[1].index, 173}};
                    cases.push_back({inputs, evaluateWrites(graph, inputs, old)});
                }
        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>(true));
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError()) return fail("barrier: pipeline failed");
        if (countKind(graph, grh::OperationKind::kRegisterWriteArrayPort) != 2 ||
            countKind(graph, grh::OperationKind::kRegisterWritePort) != 1)
            return fail("barrier: static segments were not independently folded");
        for (const auto &c : cases)
            if (evaluateWrites(graph, c.inputs, old) != c.expected)
                return fail("barrier: whole-write priority changed");
        return 0;
    }

    int testBoundedPointerDomain()
    {
        grh::Design design;
        auto &graph = design.createGraph("bounded_pointer");
        makeRegisterArray(graph, "arr", 8, 4);
        const auto clock = makeInput(graph, "clk", 1);
        const auto bit = makeInput(graph, "bit", 1);
        const auto data = makeInput(graph, "data", 8);
        const auto pointer = makeOp2(graph, grh::OperationKind::kConcat, "pointer", 3,
                                     makeConst(graph, "zero", 2, "2'b00"), bit);
        for (int row = 0; row < 2; ++row)
        {
            const auto suffix = std::to_string(row);
            const auto index = makeConst(graph, "i" + suffix, 3, "3'd" + suffix);
            const auto hit = makeOp2(graph, grh::OperationKind::kEq, "hit" + suffix, 1, pointer, index);
            makeArrayWrite(graph, "w" + suffix, "arr", hit, index, data, clock);
        }
        PassManager manager;
        manager.addPass(std::make_unique<MuxTreeFoldPass>(true));
        manager.addPass(std::make_unique<GrhVerifyPass>());
        PassDiagnostics diags;
        if (!manager.run(design, diags).success || diags.hasError()) return fail("bounded pointer: pipeline failed");
        if (countKind(graph, grh::OperationKind::kRegisterWriteArrayPort) != 1 ||
            !findWritePortWithDynamicIndex(graph, "arr").valid())
            return fail("bounded pointer: complete reachable domain was not folded");
        for (uint64_t row = 0; row < 2; ++row)
        {
            Inputs inputs{{clock.index, 1}, {bit.index, row}, {data.index, 87}};
            auto expected = std::vector<uint64_t>{1, 2, 3, 4};
            expected[row] = 87;
            if (evaluateWrites(graph, inputs, {1, 2, 3, 4}) != expected)
                return fail("bounded pointer: wrong row updated");
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
    if (int rc = testGuardedWriteNormalization(false)) return rc;
    if (int rc = testGuardedWriteNormalization(true)) return rc;
    if (int rc = testIdentityOverridesEarlierWrite()) return rc;
    if (int rc = testTwoStateOptIn()) return rc;
    if (int rc = testGuardedFoldRejections()) return rc;
    if (int rc = testWholeWriteBarrier()) return rc;
    if (int rc = testBoundedPointerDomain()) return rc;
    std::cout << "[mux-tree-fold-tests] all tests passed\n";
    return 0;
}
