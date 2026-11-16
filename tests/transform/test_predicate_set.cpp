#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/predicate_set.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;
using namespace grh;
using namespace transform;

namespace
{
    void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
    ValueId input(Graph &g, const char *name, int width, bool sign = false)
    {
        auto v = g.createValue(g.internSymbol(name), width, sign); g.bindInputPort(name, v); return v;
    }
    ValueId operation(Graph &g, OperationKind kind, int width, std::vector<ValueId> operands)
    {
        const auto op = g.createOperation(kind);
        const auto value = g.createValue(width, false);
        for (const auto v : operands) g.addOperand(op, v);
        g.addResult(op, value); return value;
    }
    ValueId constant(Graph &g, int width, uint64_t value)
    {
        const auto v = operation(g, OperationKind::kConstant, width, {});
        g.setAttr(g.valueDef(v), "constValue", std::to_string(width) + "'d" + std::to_string(value)); return v;
    }
    ValueId equal(Graph &g, ValueId source, uint64_t value, bool commute = false)
    {
        const auto c = constant(g, g.valueWidth(source), value);
        return operation(g, OperationKind::kEq, 1, commute ? std::vector{c, source} : std::vector{source, c});
    }
    ValueId reduce(Graph &g, const std::vector<ValueId> &terms)
    {
        return operation(g, OperationKind::kReduceOr, 1,
            {operation(g, OperationKind::kConcat, static_cast<int>(terms.size()), terms)});
    }
    bool applyRule(Design &design, bool enabled)
    {
        PassManager manager; manager.addPass(std::make_unique<PredicateSetPass>(PredicateSetOptions{enabled}));
        PassDiagnostics diagnostics; const auto result = manager.run(design, diagnostics);
        require(result.success && !diagnostics.hasError(), "predicate pass failed"); return result.changed;
    }

    void fallbacks()
    {
        for (unsigned mode = 0; mode != 9; ++mode)
        {
            Design design; auto &g = design.createGraph("top");
            const auto x = input(g, "x", mode == 3 ? 65 : 12, mode == 1);
            std::vector<ValueId> terms;
            const unsigned count = mode == 4 ? 513 : mode == 5 ? 15 : 32;
            for (unsigned i = 0; i != count; ++i)
            {
                const auto eq = equal(g, x, mode == 2 ? i * 3 : i);
                if (mode == 6)
                {
                    const auto c = g.getOperation(g.valueDef(eq)).operands()[1];
                    g.setAttr(g.valueDef(c), "constValue", std::string("12'bx"));
                }
                terms.push_back(eq);
            }
            if (mode == 8) terms.push_back(constant(g, 1, 0));
            const auto out = reduce(g, terms); g.bindOutputPort("out", out);
            if (mode == 7) g.setAttr(g.valueDef(out), "hasSideEffects", true);
            if (mode == 8)
            {
                const auto joined = g.getOperation(g.valueDef(out)).operands()[0];
                g.replaceOperand(g.valueDef(joined), terms.size() - 1, out);
            }
            const auto operations = g.operations().size();
            require(!applyRule(design, mode != 0), "unsupported case transformed");
            require(g.operations().size() == operations, "fallback mutated graph");
        }
        std::string error;
        const std::vector<std::string_view> valid{"-semantics", "2state"}, invalid{"-semantics", "guess"};
        require(makePass("predicate-set", valid, error) != nullptr, "CLI did not accept two-state");
        require(makePass("predicate-set", invalid, error) == nullptr && !error.empty(), "CLI accepted invalid semantics");
    }

    Design fixture()
    {
        Design design; auto &g = design.createGraph("top"); design.markAsTop(g.symbol());
        const auto x = input(g, "x", 12), y = input(g, "y", 12), extra = input(g, "extra", 1);
        const auto wide = input(g, "wide", 64), clk = input(g, "clk", 1);
        const auto nibble = input(g, "nibble", 4);
        std::vector<ValueId> domain;
        for (unsigned i = 0; i < 16; ++i) domain.push_back(equal(g, nibble, i));
        g.bindOutputPort("full", reduce(g, domain));
        std::vector<ValueId> high, low;
        for (const uint64_t base : {UINT64_C(0xb00), UINT64_C(0xc00)})
            for (unsigned i = 0; i < 32; ++i) if (i != 1)
            {
                low.push_back(equal(g, x, base + i, i % 2));
                high.push_back(equal(g, x, base + 128 + i, i % 2));
            }
        // A predicate remains independently observable after consumer fusion.
        g.bindOutputPort("tap", high.front());
        auto highValue = reduce(g, high), lowValue = reduce(g, low);
        g.bindOutputPort("high", highValue); g.bindOutputPort("low", lowValue);
        auto all = high; all.insert(all.end(), low.begin(), low.end());
        all.push_back(equal(g, x, 0x7b3)); all.push_back(extra); all.push_back(high.front());
        const auto any = reduce(g, all); g.bindOutputPort("any", any);
        std::vector<ValueId> mixed;
        for (unsigned i = 0; i < 32; ++i)
        {
            mixed.push_back(equal(g, x, 128 + i)); mixed.push_back(equal(g, y, 2048 + i, true));
        }
        g.bindOutputPort("mixed", reduce(g, mixed));
        std::vector<ValueId> edge;
        for (uint64_t i = 0; i < 16; ++i) edge.push_back(equal(g, wide, UINT64_MAX - i));
        g.bindOutputPort("edge", reduce(g, edge));
        std::vector<ValueId> start;
        for (uint64_t i = 0; i < 32; ++i) start.push_back(equal(g, wide, i));
        g.bindOutputPort("start", reduce(g, start));
        const auto reg = g.createOperation(OperationKind::kRegister, g.internSymbol("q"));
        g.setAttr(reg, "width", int64_t{1}); g.setAttr(reg, "isSigned", false);
        g.setAttr(reg, "initValue", std::string("1'b0"));
        const auto read = operation(g, OperationKind::kRegisterReadPort, 1, {});
        g.setAttr(g.valueDef(read), "regSymbol", std::string("q")); g.bindOutputPort("q_out", read);
        const auto write = g.createOperation(OperationKind::kRegisterWritePort);
        const auto one = constant(g, 1, 1);
        for (const auto v : {one, any, one, clk}) g.addOperand(write, v);
        g.setAttr(write, "regSymbol", std::string("q"));
        g.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        return design;
    }
    std::string quote(const std::string &s)
    {
        std::string out = "'"; for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c); return out + "'";
    }
    const char *harness = R"CPP(
#include "grhsim_top.hpp"
#include <cstdint>
#include <iostream>
int main() {
    GrhSIM_top m; m.init(); m.clk=0; m.extra=0; m.x=0; m.y=0; m.wide=0; m.nibble=0; m.eval();
    bool q=false;
    for(unsigned x=0;x<4096;++x) for(unsigned e=0;e<2;++e) {
        m.x=x; m.y=(x*97+41)&4095; m.extra=e; m.nibble=x&15;
        m.wide=(x&1)?UINT64_MAX-(x&31):(x&63); m.clk=0; m.eval();
        const auto member=[](unsigned x,unsigned base) {return x>=base && x<base+32 && x!=base+1;};
        const bool hi=member(x,0xb80)||member(x,0xc80), lo=member(x,0xb00)||member(x,0xc00);
        const bool any=hi||lo||x==0x7b3||e;
        if(!m.full || m.high!=hi || m.low!=lo || m.any!=any || m.tap!=(x==0xb80) || m.q_out!=q ||
           m.mixed!=((x>=128&&x<160)||(m.y>=2048&&m.y<2080)) ||
           m.edge!=(m.wide>=UINT64_MAX-15) || m.start!=(m.wide<32)) return 1;
        m.clk=1; m.eval(); q=any; if(m.q_out!=q) return 2;
        // A combinational change without another edge must leave the register alone.
        m.extra=!e; m.eval(); if(m.any!=(hi||lo||x==0x7b3||!e) || m.q_out!=q) return 3;
    }
}
)CPP";
}

int main()
{
    try
    {
        fallbacks();
        const auto root = std::filesystem::path(WOLF_PREDICATE_ARTIFACT_DIR);
        for (const bool enabled : {false, true})
        {
            auto design = fixture(); require(applyRule(design, enabled) == enabled, "rule failed to trigger");
            PassManager cleanup; cleanup.addPass(std::make_unique<DeadCodeElimPass>());
            PassDiagnostics diagnostics; require(cleanup.run(design, diagnostics).success, "DCE failed");
            if (enabled) require(!applyRule(design, true), "rule not idempotent");
            SessionStore session; PassManager manager; manager.options().session = &session;
            ActivityScheduleOptions schedule; schedule.path = "top"; schedule.eventRegions = true;
            manager.addPass(std::make_unique<ActivitySchedulePass>(schedule));
            require(manager.run(design, diagnostics).success && !diagnostics.hasError(), "schedule failed");
            const auto dir = root / (enabled ? "on" : "off"); std::filesystem::create_directories(dir);
            emit::EmitOptions options; options.outputDir = dir; options.topOverrides = {"top"}; options.session = &session;
            options.attributes["waveform"] = "off"; options.attributes["perf"] = "off";
            emit::EmitDiagnostics ed;
            require(emit::EmitGrhSimCpp(&ed).emit(design, options).success && !ed.hasError(), "emit failed");
            std::ofstream(dir / "harness.cpp") << harness;
            for (const auto flags : {"-O0", "-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"})
            {
                const char *compiler = std::getenv("CXX");
                std::string command = quote(compiler ? compiler : WOLF_TEST_CXX_COMPILER) + " -std=c++20 " + flags +
                                      " -I" + quote(dir.string());
                for (const auto &p : std::filesystem::directory_iterator(dir))
                    if (p.path().extension() == ".cpp") command += " " + quote(p.path().string());
                command += " -o " + quote((dir / "check").string());
                require(std::system(command.c_str()) == 0 && std::system(quote((dir / "check").string()).c_str()) == 0,
                        "generated oracle failed");
            }
        }
        std::cout << "predicate-set: exhaustive 12-bit domains, 64-bit edges, mixed sources, shared users, held clocks and fallbacks passed\n";
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
