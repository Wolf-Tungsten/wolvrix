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

} // namespace

int main()
{
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

    return 0;
}
