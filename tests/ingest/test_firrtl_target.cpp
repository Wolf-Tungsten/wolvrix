#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "data/firrtl/memory/target_workload.hpp"

namespace {
using namespace firrtl_test;
void test(const std::filesystem::path& input) {
    ingest::IngestRequest request;
    request.sources.push_back({ingest::SourceLanguage::Firrtl, input, "frozen-xs"});
    request.tops = {"Queue2_TLBundleA", "sram_array_1p128x138m69s1h0l1b_icache_meta", "JtagBypassChain"};
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
    driver += "});}}\n";
    generate(queue, output / "queue", driver);
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
    std::ofstream(output / "run.mk") << ".PHONY: run queue sram jtag jtag-failure\nrun: queue sram jtag jtag-failure\nqueue sram jtag:\n\t$(MAKE) -C $@ -f run.mk run\njtag-failure: jtag\n\t$(MAKE) -C jtag -f check.mk check\n";
    std::cout << "frozen Queue2/SRAM/Jtag ingest and JSON verified; queue/Jtag ordinary GRH scoreboards verified; compiled drivers generated\n";
}
}
int main(int argc, char** argv) { try { require(argc == 2, "expected frozen FIRRTL input"); test(argv[1]); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
