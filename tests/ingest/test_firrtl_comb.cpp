#include "firrtl_emit_fixture.hpp"

namespace {
using namespace firrtl_test;

constexpr std::string_view basicDriver = R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main() {
    GrhSIM_top model;
    for (unsigned epoch=0; epoch<3; ++epoch) {
        model.init();
        for (unsigned a=0; a<32; ++a) for (unsigned b=0; b<8; ++b) for (unsigned s=0; s<2; ++s) {
            model.a=a; model.b=b; model.sel=s; model.eval();
            assert(model.y == (a&b)+(s?3:2));
            assert(model.local == 2*(a&b));
            assert(model.aliasOut == a && model.constantOut == 42);
        }
    }
}
)CPP";
constexpr std::string_view aggregateDriver = R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
#include <cstdint>
int main() {
    GrhSIM_top model;
    for (unsigned epoch=0; epoch<3; ++epoch) {
        model.init();
        for (unsigned n=0; n<64; ++n) for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) {
            const uint64_t data=(n*149+0x3579)&65535, small=(n*17+0x53)&255;
            const unsigned sel=n&1, sel2=(n>>1)&1;
            for (unsigned row=0;row<2;++row) {
                model.packet__DOT__valid[row]=(n>>row)&1;
                model.packet__DOT__meta__DOT__flag[row]=(n>>(2+row))&1;
                model.other__DOT__valid[row]=(n>>(1+row))&1;
                model.other__DOT__meta__DOT__flag[row]=(n>>(3+row))&1;
                for (unsigned col=0;col<2;++col) {
                    model.packet__DOT__data[row][col]=(data>>((row*2+col)*4))&15;
                    model.other__DOT__data[row][col]=(small>>((row*2+col)*2))&3;
                }
            }
            model.one[0][0]=n&1; model.sel=sel; model.sel2=sel2; model.i=i; model.j=j; model.data=n&15;
            model.eval();
            uint64_t table=data;
            const auto write=[&](unsigned row,unsigned col,unsigned value) { const auto off=(row*2+col)*4; table=(table&~(UINT64_C(15)<<off))|(uint64_t(value)<<off); };
            if(i<2 && j<2) { if(sel) write(i,j,n&15); if(sel2) write(j,i,15); }
            assert(model.packed==table && model.equal==1 && model.single[0][0]==(n&1));
            assert(model.readOut==(i<2 && j<2 ? (table>>((i*2+j)*4))&15 : 0));
            for(unsigned row=0;row<2;++row) for(unsigned col=0;col<2;++col) {
                const auto cell=row*2+col;
                assert(model.tableOut[row][col]==((table>>(cell*4))&15));
                assert(model.result__DOT__data[row][col]==(sel ? (data>>(cell*4))&15 : (small>>(cell*2))&3));
            }
        }
    }
}
)CPP";
constexpr std::string_view signedDriver = R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
#include <cstdint>
int main() {
    GrhSIM_top model; model.init();
    for(int a=-16;a<16;++a) for(int b=-4;b<4;++b) for(unsigned sh=0;sh<4;++sh) {
        model.a=a; model.b=b; model.sh=sh; model.eval();
        assert((unsigned(model.sum)&63)==(unsigned(a+b)&63));
        assert((unsigned(model.product)&255)==(unsigned(a*b)&255));
        assert((unsigned(model.quotient)&63)==(unsigned(b?a/b:0)&63));
        assert((unsigned(model.remainder)&7)==(unsigned(b?a%b:0)&7));
        assert(model.less==(a<b) && model.bitxor==(unsigned(a^b)&31));
        assert((unsigned(model.left)&255)==(unsigned(a*(1<<sh))&255));
        const int scale=1<<sh;
        const int right=a>=0 ? a/scale : -((-a+scale-1)/scale);
        assert((unsigned(model.right)&31)==(unsigned(right)&31));
        assert((unsigned(model.negative)&63)==(unsigned(-a)&63));
    }
}
)CPP";
void selection() {
    const auto path=artifact("firrtl-fixtures")/"selection.fir";
    std::ofstream(path)<<R"FIR(FIRRTL version 6.0.0
circuit main :
  module helper :
    output y : UInt<4>
    connect y,UInt<4>(3)
  module state :
    input clk : Clock
    inst r of helper
  public module main :
    output y : UInt<4>
    node n = UInt<4>(7)
    connect y,n
)FIR";
    ingest::IngestRequest request; request.sources.push_back({ingest::SourceLanguage::Firrtl,path,{}});
    auto main=ingest::readSources(request);
    require(main.success() && main.design->findGraph("main") && !main.design->findGraph("helper"),"default top ignored circuit name");
    const auto inspection=node::inspect(*main.design->findGraph("main"));
    require(node::parseKey(inspection.nodes.at(0).key).sourceUnit=="selection.fir","implicit source identity depends on absolute checkout path");
    request.tops={"helper","main"}; auto both=ingest::readSources(request);
    require(both.success() && both.design->graphs().size()==2,"explicit FIRRTL top selection did not lower both modules");
    request.tops={"main","state"}; auto hierarchy=ingest::readSources(request);
    require(hierarchy.success() && hierarchy.design->graphs().size()==3,"multi-top hierarchy did not lower dependencies");
    request.tops={"main","missing"}; auto failed=ingest::readSources(request);
    require(!failed.success() && !failed.design,"failed multi-top read published a partial design");
    request.tops={"missing"}; auto missing=ingest::readSources(request);
    require(!missing.success() && !missing.design && diagnostics(missing).find("missing")!=std::string::npos,"missing top silently succeeded");
    request.tops={"main"}; request.sources.push_back({ingest::SourceLanguage::Firrtl,path,"other"});
    auto ambiguous=ingest::readSources(request);
    require(!ambiguous.success() && !ambiguous.design && diagnostics(ambiguous).find("ambiguous")!=std::string::npos,"ambiguous module identity silently bound");
}
void sourceInfoOwnership() {
    const std::string first = "@[/long/source/path/that/exceeds/small/string/storage/First.scala 152:69]";
    const std::string second = "@[/another/long/source/path/that/exceeds/small/string/storage/Second.scala 153:70]";
    auto result = text("source-info-ownership",
        "    input a : UInt<5>\n"
        "    input b : UInt<5>\n"
        "    output y : UInt<8>\n"
        "    node n = add(and(a, b), or(a, b)) " + first + "\n"
        "    connect y, add(and(n, n), or(n, n)) " + second + "\n");
    require(result.success(), diagnostics(result));
    result.sourceOwners.clear();
    const auto check = [&](const grh::Graph& graph) {
        unsigned checked = 0;
        for (const auto op : graph.operations()) {
            const auto kind = graph.opKind(op);
            if (kind != OperationKind::kAnd && kind != OperationKind::kOr && kind != OperationKind::kAdd) continue;
            const auto loc = graph.opSrcLoc(op);
            require(loc && (loc->line == 7 || loc->line == 8), "nested expression source line lost");
            const auto& expected = loc->line == 7 ? first : second;
            require(loc->note == expected, "nested expression source info borrowed temporary storage");
            for (const auto value : graph.opResults(op)) {
                const auto valueLoc = graph.valueSrcLoc(value);
                require(valueLoc && valueLoc->note == expected, "expression result source info changed");
            }
            ++checked;
        }
        require(checked == 6, "source info fixture did not exercise nested expressions on both lines");
    };
    check(*result.design->findGraph("top"));
    auto stored = roundTrip(*result.design);
    check(*stored.findGraph("top"));
}
void test() {
    sourceInfoOwnership();
    selection();
    const auto path = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "comb/basic.fir";
    auto source = design(path); auto& graph = *source.findGraph("top");
    require(graph.valueWidth(graph.findValue("selected")) == 6, "inferred wire width lost FIRRTL add carry");
    for (const auto value : graph.values()) require(graph.valueDomain(value) == grh::LogicDomain::FourState, "FIRRTL silently projected to TwoState");
    auto inspection = node::inspect(graph); require(inspection.errors.empty(), inspection.report(graph));
    require(inspection.nodes.size() == 6, "node leaves or aliases lost before simplify");
    require(count(graph, OperationKind::kAnd) == 4, "node expressions copied or prematurely shared");
    bool sourceInfo = false;
    for (const auto op : graph.operations()) if (auto loc = graph.opSrcLoc(op); loc && loc->note.find("comb.scala") != std::string::npos) sourceInfo = true;
    require(sourceInfo, "source info did not reach ordinary GRH");
    Oracle oracle(graph);
    for (unsigned a=0; a<32; ++a) for (unsigned b=0; b<8; ++b) for (unsigned s=0; s<2; ++s) {
        const std::map<std::string,uint64_t> inputs{{"a",a},{"b",b},{"sel",s}};
        require(oracle.output("y", inputs) == (a&b)+(s?3:2), "last connect/node reference semantics differ");
        require(oracle.output("local", inputs) == 2*(a&b), "node internal expression semantics differ");
    }
    run(source, std::make_unique<tx::GrhVerifyPass>());
    run(source, std::make_unique<tx::SimplifyPass>());
    valid(source); require(count(graph, OperationKind::kAnd) == 1, "node hints blocked ordinary cross-source CSE");
    auto stored = roundTrip(source); valid(stored); run(stored, std::make_unique<tx::GrhVerifyPass>());
    auto unannotated = design(path, false);
    require(node::inspect(*unannotated.findGraph("top")).nodes.empty(), "disabled node annotations still present");
    const auto output = artifact("firrtl-comb");
    generate(stored, output, basicDriver);
    auto aggregate = design(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR)/"comb/aggregate.fir");
    generate(aggregate, output/"aggregate", aggregateDriver);
    auto signedArithmetic = design(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR)/"comb/signed.fir");
    generate(signedArithmetic, output/"signed", signedDriver);
    std::ofstream(output/"run.mk", std::ios::app) << "\n.PHONY: aggregate signed\nrun: aggregate signed\naggregate signed:\n\t$(MAKE) -C $@ -f run.mk run\n";
}
}
int main() {
    try { test(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
