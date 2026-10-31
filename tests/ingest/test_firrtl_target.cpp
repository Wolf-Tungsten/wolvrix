#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "data/firrtl/memory/target_workload.hpp"

namespace {
using namespace firrtl_test;
void test(const std::filesystem::path& input) {
    ingest::IngestRequest request;
    request.sources.push_back({ingest::SourceLanguage::Firrtl, input, "frozen-xs"});
    request.tops = {"Queue2_TLBundleA", "sram_array_1p128x138m69s1h0l1b_icache_meta", "JtagBypassChain", "MbistClockGateCell"};
    request.firrtl.invalidPolicy = ingest::FirrtlInvalidPolicy::Zero;
    request.firrtl.initPolicy = ingest::FirrtlInitPolicy::Zero;
    request.firrtl.enabledLayers = {"Verification.Assert"};
    auto result = ingest::readSources(request); require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    const auto output = artifact("firrtl-target");
    wolvrix::lib::store::StoreJson store;
    std::ofstream(output / "selected.json") << *store.storeToString(restored);
    for (const auto& name : request.tops) {
        const auto& graph = *restored.findGraph(name);
        if (name != "MbistClockGateCell")
            require(count(graph, OperationKind::kInstance) == 0 && count(graph, OperationKind::kBlackbox) == 0, "target accepted a fake external implementation");
        require(graph.metadata("firrtl.pendingExternalAnnotations").has_value(), "external binding payload was not retained");
    }
    auto queue = restored.clone();
    queue.cloneGraph(request.tops[0], "top");
    for (const auto& name : request.tops) queue.deleteGraph(name);
    queue.markAsTop("top");
    grhsim_test::Oracle oracle(*queue.findGraph("top"));
    firrtl_memory_test::queueTrace([&](const firrtl_memory_test::Values& inputs, const firrtl_memory_test::Values& expected) {
        const auto observation = oracle.eval(inputs);
        for (const auto& [port, value] : expected) {
            const auto bits = slang::SVInt::fromString(observation.outputs.at(port));
            require(bits.getRawPtr()[0] == value, "frozen queue scoreboard mismatch " + port);
        }
    });
    const auto workload = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "memory/target_workload.hpp";
    std::string driver = "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + "\"\nint main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();firrtl_memory_test::queueTrace([&](const auto& inputs,const auto& expected){\n";
    for (const auto& port : queue.findGraph("top")->inputPorts()) driver += "if(const auto v=inputs.find(\"" + port.name + "\");v!=inputs.end())m." + port.name + "=v->second;\n";
    driver += "m.eval();\n";
    for (const auto& port : queue.findGraph("top")->outputPorts()) driver += "if(const auto v=expected.find(\"" + port.name + "\");v!=expected.end()&&m." + port.name + "!=v->second)throw std::runtime_error(\"frozen queue compiled mismatch " + port.name + "\");\n";
    driver += "});const auto p=m.perf_counters();std::printf(\"queue epoch=%d eval=%llu rounds=%llu compute=%llu commit=%llu writes=%llu\\n\",epoch,(unsigned long long)p.evalCount,(unsigned long long)p.totalRoundCount,(unsigned long long)p.computeBatchExecCount,(unsigned long long)p.commitBatchExecCount,(unsigned long long)p.touchedWriteCount);}}\n";
    const auto queueSource = queue.clone();
    generate(queue, output / "queue", driver, {"-clone-shared=0"});
    for (const std::string mode : {"prefer", "bytes", "fused", "clone"}) {
        auto candidate = queueSource.clone();
        generate(candidate, output / ("queue-" + mode), driver, {"-firrtl-nodes=prefer",
            mode == "bytes" ? "-cost-model=bytes" : "-cost-model=legacy",
            mode == "fused" ? "-fuse-leaves=1" : "-fuse-leaves=0",
            mode == "clone" ? "-clone-shared=1" : "-clone-shared=0"});
    }
    auto sram = restored.clone();
    sram.cloneGraph(request.tops[1], "top");
    for (const auto& name : request.tops) sram.deleteGraph(name);
    sram.markAsTop("top");
    generate(sram, output / "sram", "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + R"CPP("
int main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();firrtl_memory_test::sramTrace([&](const auto& inputs,const auto& expected){
if(const auto v=inputs.find("RW0__DOT__clk");v!=inputs.end())m.RW0__DOT__clk=v->second;
if(const auto v=inputs.find("RW0__DOT__addr");v!=inputs.end())m.RW0__DOT__addr=v->second;
if(const auto v=inputs.find("RW0__DOT__en");v!=inputs.end())m.RW0__DOT__en=v->second;
if(const auto v=inputs.find("RW0__DOT__wmode");v!=inputs.end())m.RW0__DOT__wmode=v->second;
if(const auto v=inputs.find("RW0__DOT__wmask");v!=inputs.end())m.RW0__DOT__wmask=v->second;
if(inputs.contains("lo0"))m.RW0__DOT__wdata=static_cast<unsigned _BitInt(138)>(inputs.at("lo0"))|(static_cast<unsigned _BitInt(138)>(inputs.at("hi0"))<<64)|(static_cast<unsigned _BitInt(138)>(inputs.at("lo1"))<<69)|(static_cast<unsigned _BitInt(138)>(inputs.at("hi1"))<<133);
m.eval();
if(!expected.empty()){
const auto result=static_cast<unsigned _BitInt(138)>(expected.at("lo0"))|(static_cast<unsigned _BitInt(138)>(expected.at("hi0"))<<64)|(static_cast<unsigned _BitInt(138)>(expected.at("lo1"))<<69)|(static_cast<unsigned _BitInt(138)>(expected.at("hi1"))<<133);
if(m.RW0__DOT__rdata!=result)throw std::runtime_error("frozen masked 138-bit SRAM mismatch");
}
});}}
)CPP");
    auto jtag = restored.clone();
    jtag.cloneGraph(request.tops[2], "top");
    for (const auto& name : request.tops) jtag.deleteGraph(name);
    jtag.markAsTop("top");
    require(count(*jtag.findGraph("top"), OperationKind::kSystemTask) == 1, "frozen verification layer lost its intrinsic");
    grhsim_test::Oracle jtagOracle(*jtag.findGraph("top"));
    firrtl_memory_test::jtagTrace([&](const firrtl_memory_test::Values& inputs, const firrtl_memory_test::Values& expected) {
        const auto observation = jtagOracle.eval(inputs);
        require(observation.effects.empty(), "legal frozen JTAG inputs triggered an assertion");
        for (const auto& [port, value] : expected)
            require(slang::SVInt::fromString(observation.outputs.at(port)).as<uint64_t>() == value, "frozen JTAG scoreboard mismatch");
    });
    driver = "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + "\"\nint main(int argc,char**){GrhSIM_top m;m.init();firrtl_memory_test::jtagTrace([&](const auto& inputs,const auto& expected){\n";
    for (const auto& port : jtag.findGraph("top")->inputPorts()) driver += "if(const auto v=inputs.find(\"" + port.name + "\");v!=inputs.end())m." + port.name + "=v->second;\n";
    driver += "m.eval();\n";
    for (const auto& port : jtag.findGraph("top")->outputPorts()) driver += "if(m." + port.name + "!=expected.at(\"" + port.name + "\"))throw std::runtime_error(\"frozen JTAG compiled mismatch\");\n";
    driver += "});if(argc>1){m.clock=0;m.reset=0;m.io__DOT__chainIn__DOT__capture=1;m.io__DOT__chainIn__DOT__shift=1;m.io__DOT__chainIn__DOT__update=0;m.eval();m.clock=1;m.eval();return 99;}}\n";
    generate(jtag, output / "jtag", driver);
    std::ofstream(output / "jtag/expected.err") << "[fatal] Assertion failed at JtagShifter.scala:72\n\n";
    std::ofstream(output / "jtag/check.mk") << ".PHONY: check\ncheck: driver\n\t@set -eu; status=0; ./driver fail > actual.out 2> actual.err || status=$$?; test $$status -eq 1; test ! -s actual.out; cmp expected.err actual.err\n";
    auto clockgate = restored.clone();
    clockgate.cloneGraph("MbistClockGateCell", "top");
    for (const auto& name : request.tops) { clockgate.unmarkAsTop(name); }
    clockgate.markAsTop("top");
    tx::SessionStore clockgateSession;
    pass(clockgate, clockgateSession, "hier-flatten");
    pass(clockgate, clockgateSession, "latch-transparent-read");
    const auto& clockgraph = *clockgate.findGraph("top");
    require(count(clockgraph, OperationKind::kInstance) == 0 && count(clockgraph, OperationKind::kBlackbox) == 0,
        "frozen ClockGate did not bind a real SV implementation");
    require(!node::inspect(clockgraph).nodes.empty(), "frozen clockgate lost FIRRTL node provenance");
    require(std::count_if(result.dependencies.begin(), result.dependencies.end(), [](const auto& entry) { return entry.starts_with("inline:"); }) == 1,
        "frozen mixed read collected unreachable blackboxes");
    generate(clockgate, output / "clockgate", R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main(){GrhSIM_top m;m.init();bool held=false;
for(unsigned bits=0;bits<128;++bits){
m.clock=0;m.reset=0;m.E=bits&1;m.dft__DOT__cgen=(bits>>1)&1;
m.mbist__DOT__req=(bits>>2)&1;m.mbist__DOT__readen=(bits>>3)&1;m.mbist__DOT__writeen=(bits>>4)&1;
m.dft__DOT__ram_mcp_hold=0;m.dft__DOT__ram_aux_clk=0;m.dft__DOT__ram_aux_ckbp=0;
held=m.dft__DOT__cgen||(m.mbist__DOT__req?(m.mbist__DOT__readen||m.mbist__DOT__writeen):m.E);
m.eval();assert(m.out_clock==0);m.clock=1;m.eval();assert(m.out_clock==held);
m.E=!m.E;m.dft__DOT__cgen=!m.dft__DOT__cgen;m.mbist__DOT__req=!m.mbist__DOT__req;
m.eval();assert(m.out_clock==held);m.clock=0;m.eval();assert(m.out_clock==0);
}}
)CPP");
    std::ofstream(output / "run.mk") << ".PHONY: run queue queue-prefer queue-bytes queue-fused queue-clone sram jtag clockgate jtag-failure\nrun: queue queue-prefer queue-bytes queue-fused queue-clone sram jtag clockgate jtag-failure\nqueue queue-prefer queue-bytes queue-fused queue-clone sram jtag clockgate:\n\t$(MAKE) -C $@ -f run.mk run\njtag-failure: jtag\n\t$(MAKE) -C jtag -f check.mk check\n";
    std::cout << "frozen Queue2/SRAM/Jtag ingest and JSON verified; queue/Jtag ordinary GRH scoreboards verified; compiled drivers generated\n";
}
}
int main(int argc, char** argv) { try { require(argc == 2, "expected frozen FIRRTL input"); test(argv[1]); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
