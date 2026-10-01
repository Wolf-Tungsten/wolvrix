#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/memory_read_retime.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[transform-memory-read-retime] " << message << '\n';
        return 1;
    }

    ValueId makeConst(Graph &graph,
                      const std::string &valueName,
                      const std::string &opName,
                      int32_t width,
                      bool isSigned,
                      const std::string &literal)
    {
        const SymbolId valueSym = graph.internSymbol(valueName);
        const SymbolId opSym = graph.internSymbol(opName);
        const ValueId value = graph.createValue(valueSym, width, isSigned);
        const OperationId op = graph.createOperation(OperationKind::kConstant, opSym);
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", literal);
        return value;
    }

    std::size_t countOps(const Graph &graph, OperationKind kind)
    {
        std::size_t count = 0;
        for (const OperationId opId : graph.operations())
        {
            if (graph.getOperation(opId).kind() == kind)
            {
                ++count;
            }
        }
        return count;
    }

    Graph &buildBaseReadPath(Design &design,
                             bool withMemoryWrite,
                             bool secondMemoryWrite,
                             bool withAddrInit = true)
    {
        Graph &graph = design.createGraph(withMemoryWrite ? "simple_ram" : "simple_rom");

        ValueId clk = graph.createValue(1, false);
        graph.bindInputPort("clk", clk);
        ValueId en = graph.createValue(1, false);
        graph.bindInputPort("en", en);
        ValueId addrD = graph.createValue(2, false);
        graph.bindInputPort("addr_d", addrD);

        const ValueId addrMask = makeConst(graph, "addr_mask", "addr_mask_op", 2, false, "2'b11");
        const ValueId memMask = makeConst(graph, "mem_mask", "mem_mask_op", 8, false, "8'hff");

        const SymbolId memSym = graph.internSymbol("mem0");
        const OperationId mem = graph.createOperation(OperationKind::kMemory, memSym);
        graph.setAttr(mem, "width", static_cast<int64_t>(8));
        graph.setAttr(mem, "row", static_cast<int64_t>(4));
        graph.setAttr(mem, "isSigned", false);
        graph.setAttr(mem, "initKind", std::vector<std::string>{"literal"});
        graph.setAttr(mem, "initFile", std::vector<std::string>{""});
        graph.setAttr(mem, "initValue", std::vector<std::string>{"8'h34"});
        graph.setAttr(mem, "initStart", std::vector<int64_t>{-1});
        graph.setAttr(mem, "initLen", std::vector<int64_t>{0});

        const SymbolId regSym = graph.internSymbol("addr_q");
        const OperationId reg = graph.createOperation(OperationKind::kRegister, regSym);
        graph.setAttr(reg, "width", static_cast<int64_t>(2));
        graph.setAttr(reg, "isSigned", false);
        if (withAddrInit)
        {
            graph.setAttr(reg, "initValue", std::string("2'd1"));
        }

        const OperationId regWrite = graph.createOperation(OperationKind::kRegisterWritePort,
                                                           graph.internSymbol("addr_q_write"));
        graph.setAttr(regWrite, "regSymbol", std::string("addr_q"));
        graph.setAttr(regWrite, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(regWrite, en);
        graph.addOperand(regWrite, addrD);
        graph.addOperand(regWrite, addrMask);
        graph.addOperand(regWrite, clk);

        const OperationId regRead = graph.createOperation(OperationKind::kRegisterReadPort,
                                                          graph.internSymbol("addr_q_read"));
        graph.setAttr(regRead, "regSymbol", std::string("addr_q"));
        ValueId addrQ = graph.createValue(graph.internSymbol("addr_q_val"), 2, false);
        graph.addResult(regRead, addrQ);

        const OperationId memRead = graph.createOperation(OperationKind::kMemoryReadPort,
                                                          graph.internSymbol("mem0_read"));
        graph.setAttr(memRead, "memSymbol", std::string("mem0"));
        graph.addOperand(memRead, addrQ);
        ValueId data = graph.createValue(graph.internSymbol("data"), 8, false);
        graph.addResult(memRead, data);
        graph.bindOutputPort("data", data);

        if (withMemoryWrite)
        {
            ValueId wen = graph.createValue(1, false);
            graph.bindInputPort("wen", wen);
            ValueId waddr = graph.createValue(2, false);
            graph.bindInputPort("waddr", waddr);
            ValueId wdata = graph.createValue(8, false);
            graph.bindInputPort("wdata", wdata);

            const OperationId memWrite = graph.createOperation(OperationKind::kMemoryWritePort,
                                                               graph.internSymbol("mem0_write"));
            graph.setAttr(memWrite, "memSymbol", std::string("mem0"));
            graph.setAttr(memWrite, "eventEdge", std::vector<std::string>{"posedge"});
            graph.addOperand(memWrite, wen);
            graph.addOperand(memWrite, waddr);
            graph.addOperand(memWrite, wdata);
            graph.addOperand(memWrite, memMask);
            graph.addOperand(memWrite, clk);

            if (secondMemoryWrite)
            {
                ValueId wen2 = graph.createValue(1, false);
                graph.bindInputPort("wen2", wen2);
                ValueId waddr2 = graph.createValue(2, false);
                graph.bindInputPort("waddr2", waddr2);
                ValueId wdata2 = graph.createValue(8, false);
                graph.bindInputPort("wdata2", wdata2);

                const OperationId memWrite2 = graph.createOperation(OperationKind::kMemoryWritePort,
                                                                    graph.internSymbol("mem0_write_2"));
                graph.setAttr(memWrite2, "memSymbol", std::string("mem0"));
                graph.setAttr(memWrite2, "eventEdge", std::vector<std::string>{"posedge"});
                graph.addOperand(memWrite2, wen2);
                graph.addOperand(memWrite2, waddr2);
                graph.addOperand(memWrite2, wdata2);
                graph.addOperand(memWrite2, memMask);
                graph.addOperand(memWrite2, clk);
            }
        }

        return graph;
    }
} // namespace

int main()
{
    {
        Design design;
        Graph &graph = buildBaseReadPath(design, false, false);

        PassManager manager;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("ROM case should succeed");
        }
        if (!result.changed)
        {
            return fail("ROM case should change graph");
        }
        if (graph.findOperation("addr_q").valid())
        {
            return fail("ROM case should remove original address register");
        }
        if (countOps(graph, OperationKind::kRegister) != 1)
        {
            return fail("ROM case should leave exactly one register");
        }
        const ValueId out = graph.outputPortValue("data");
        if (!out.valid())
        {
            return fail("ROM case output port missing");
        }
        const OperationId outDef = graph.getValue(out).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != OperationKind::kRegisterReadPort)
        {
            return fail("ROM case output should come from register read port");
        }
    }

    {
        Design design;
        Graph &graph = buildBaseReadPath(design, false, false, false);

        PassManager manager;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("ROM case without address init should still succeed");
        }
        if (!result.changed)
        {
            return fail("ROM case without address init should still change graph");
        }
        if (graph.findOperation("addr_q").valid())
        {
            return fail("ROM case without address init should still remove original address register");
        }
    }

    {
        Design design;
        Graph &graph = buildBaseReadPath(design, true, false);

        PassManager manager;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("simple RAM case should succeed");
        }
        if (!result.changed)
        {
            return fail("simple RAM case should change graph");
        }
        if (!graph.findOperation("addr_q").valid())
        {
            return fail("simple RAM case should keep original address register as shadow state");
        }
        if (countOps(graph, OperationKind::kRegister) != 2)
        {
            return fail("simple RAM case should contain address and data registers");
        }
        if (countOps(graph, OperationKind::kMemoryWritePort) != 1)
        {
            return fail("simple RAM case should preserve the original memory write port");
        }
        if (countOps(graph, OperationKind::kLogicAnd) == 0 || countOps(graph, OperationKind::kLogicOr) == 0)
        {
            return fail("simple RAM case should build write-hit refresh logic");
        }
        const ValueId out = graph.outputPortValue("data");
        const OperationId outDef = graph.getValue(out).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != OperationKind::kRegisterReadPort)
        {
            return fail("simple RAM case output should come from register read port");
        }
    }

    {
        Design design;
        Graph &graph = buildBaseReadPath(design, true, true);

        PassManager manager;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("multi-write skip case should not fail");
        }
        if (result.changed)
        {
            return fail("multi-write memory should be skipped");
        }
        const ValueId out = graph.outputPortValue("data");
        const OperationId outDef = graph.getValue(out).definingOp();
        if (!outDef.valid() || graph.getOperation(outDef).kind() != OperationKind::kMemoryReadPort)
        {
            return fail("multi-write skip case should leave original memory read path intact");
        }
    }

    // The ROM retime erases the address register, so a declared address
    // register must block the rewrite instead of silently losing the anchor.
    {
        Design design;
        Graph &graph = buildBaseReadPath(design, false, false);
        const OperationId addrReg = graph.findOperation("addr_q");
        if (!addrReg.valid())
        {
            return fail("fixture is missing the address register");
        }
        graph.addDeclaredSymbol(graph.getOperation(addrReg).symbol());

        PassManager manager;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("declared ROM case should succeed");
        }
        if (result.changed)
        {
            return fail("declared address register must block the ROM retime");
        }
        if (!graph.findOperation("addr_q").valid())
        {
            return fail("declared address register was erased");
        }
        if (!graph.validateDeclaredSymbols().empty())
        {
            return fail("declared ROM case broke declared symbol resolution");
        }
    }

    // Control: with declaration retention off, the same graph retimes and the
    // erased register leaves the declared set (no dangling anchor).
    {
        Design design;
        Graph &graph = buildBaseReadPath(design, false, false);
        const OperationId addrReg = graph.findOperation("addr_q");
        if (!addrReg.valid())
        {
            return fail("control fixture is missing the address register");
        }
        graph.addDeclaredSymbol(graph.getOperation(addrReg).symbol());

        PassManager manager;
        manager.options().keepDeclaredSymbols = false;
        manager.addPass(std::make_unique<MemoryReadRetimePass>());
        PassDiagnostics diags;
        PassManagerResult result = manager.run(design, diags);

        if (!result.success || diags.hasError())
        {
            return fail("declared-off ROM control case should succeed");
        }
        if (!result.changed)
        {
            return fail("declared-off control case should retime");
        }
        if (graph.findOperation("addr_q").valid())
        {
            return fail("declared-off control case should erase the address register");
        }
        if (!graph.validateDeclaredSymbols().empty())
        {
            return fail("erasing the declared register left a dangling anchor");
        }
    }

    return 0;
}
