#include "transform/repcut_grhsim_scenarios.hpp"

#include <iostream>
#include <stdexcept>

using namespace wolvrix::lib;
using namespace wolvrix::lib::transform;

int main()
{
    try
    {
        grh::Design design;
        auto &g = design.createGraph("scenario_fixture");
        using K = grh::OperationKind;
        unsigned serial = 0;
        const auto value = [&](int width = 1) {
            return g.createValue(g.internSymbol("v"+std::to_string(serial++)),width,false);
        };
        const auto input = [&](const char *name) { auto v = value(); g.bindInputPort(name,v); return v; };
        const auto clock = input("clock"), otherClock = input("other"), data = input("data");
        const auto unary = [&](K kind, grh::ValueId operand) {
            auto op = g.createOperation(kind,g.internSymbol("op"+std::to_string(serial++)));
            const auto result = value(); g.addOperand(op,operand); g.addResult(op,result); return result;
        };
        const auto inverse = unary(K::kLogicNot,unary(K::kAssign,clock));
        const auto one = value();
        const auto literal = g.createOperation(K::kConstant,g.internSymbol("one"));
        g.setAttr(literal,"constValue",std::string("1'b1")); g.addResult(literal,one);
        const auto state = [&](const char *name, grh::ValueId event) {
            auto write = g.createOperation(K::kRegisterWritePort,g.internSymbol(std::string(name)+"_w"));
            g.setAttr(write,"regSymbol",std::string(name));
            g.setAttr(write,"eventEdge",std::vector<std::string>{"posedge"});
            for (const auto operand : {one,data,one,event}) g.addOperand(write,operand);
            auto read = g.createOperation(K::kRegisterReadPort,g.internSymbol(std::string(name)+"_r"));
            g.setAttr(read,"regSymbol",std::string(name));
            const auto q = value(); g.addResult(read,q);
            return std::pair{write,q};
        };
        const auto [write,q] = state("a",clock);
        const auto [inverseWrite,inverseQ] = state("b",inverse);
        const auto [otherWrite,otherQ] = state("c",otherClock);
        auto gated = g.createOperation(K::kAnd,g.internSymbol("derived_clock"));
        auto gate = value(); g.addOperand(gated,clock); g.addOperand(gated,data); g.addResult(gated,gate);
        const auto [derivedWrite,derivedQ] = state("d",gate);
        const auto downstream = unary(K::kNot,q);
        const auto join = g.createOperation(K::kOr,g.internSymbol("join"));
        const auto joined = value();
        g.addOperand(join,q); g.addOperand(join,otherQ); g.addResult(join,joined);
        const auto memoryWrite = g.createOperation(K::kMemoryWritePort,g.internSymbol("mem_w"));
        g.setAttr(memoryWrite,"memSymbol",std::string("mem"));
        g.setAttr(memoryWrite,"eventEdge",std::vector<std::string>{"posedge"});
        for (const auto operand : {one,one,data,one,clock}) g.addOperand(memoryWrite,operand);
        const auto memoryFill = g.createOperation(K::kMemoryFillPort,g.internSymbol("mem_fill"));
        g.setAttr(memoryFill,"memSymbol",std::string("mem"));
        g.setAttr(memoryFill,"eventEdge",std::vector<std::string>{"posedge"});
        for (const auto operand : {one,data,otherClock}) g.addOperand(memoryFill,operand);
        const auto memoryRead = g.createOperation(K::kMemoryReadPort,g.internSymbol("mem_r"));
        g.setAttr(memoryRead,"memSymbol",std::string("mem"));
        const auto memoryQ = value(); g.addOperand(memoryRead,one); g.addResult(memoryRead,memoryQ);
        RepcutGrhsimScenarios scenarios;
        scenarios.inputs = {{clock,3},{otherClock,12},{data,0}};
        scenarios.events = {{clock,{1,2,3}},{otherClock,{4,8,12}}};
        scenarios.afterCommit[0]=2; scenarios.afterCommit[1]=1;
        scenarios.afterCommit[2]=8; scenarios.afterCommit[3]=4;
        const auto r = analyzeRepcutGrhsimScenarios(g,scenarios);
        const auto require = [](bool ok,const char *message) { if (!ok) throw std::runtime_error(message); };
        require(r.events[write.index]==1 && r.values[q.index]==2,"commit must activate later compute");
        require(r.events[inverseWrite.index]==2 && r.values[inverseQ.index]==1,"inverted event polarity lost");
        require(r.events[otherWrite.index]==4 && r.values[otherQ.index]==8,"independent clock scenarios merged");
        require(r.values[downstream.index]==2,"state-to-combinational propagation failed");
        require(r.values[joined.index]==10,"independent scenario union was lost");
        require(r.events[memoryWrite.index]==1 && r.events[memoryFill.index]==4 &&
                r.values[memoryQ.index]==10,"memory write/fill event offsets or writer union failed");
        require(r.events[derivedWrite.index]==kRepcutUnknownScenario &&
                r.values[derivedQ.index]==kRepcutUnknownScenario,"derived clock must stay uncertain");
        require(r.unresolvedEventReferences==1,"derived event accounting failed");
        auto unspecified = scenarios; unspecified.afterCommit[2]=0; unspecified.inputs.erase(data);
        const auto u = analyzeRepcutGrhsimScenarios(g,unspecified);
        require(u.values[otherQ.index]==kRepcutUnknownScenario,"unspecified visibility must stay unknown");
        require(u.values[data.index]==kRepcutUnknownScenario,"unseeded input became inactive");
        std::cout << "PASS raw scenarios: commit/compute separation, aliases, two clocks, memory writers, unknown events/inputs\n";
        return 0;
    }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
