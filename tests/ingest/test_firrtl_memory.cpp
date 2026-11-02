#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "data/firrtl/memory/workload.hpp"

namespace {
using namespace firrtl_test;
std::vector<std::string> models;
void aggregateAndClocks(const std::string& name) {
    auto result = read(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "memory" / (name + ".fir"));
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    const auto check = [&](const firrtl_memory_test::Values& inputs, const firrtl_memory_test::Values& expected) {
        const auto observation = oracle.eval(inputs);
        for (const auto& [port, value] : expected) {
            const auto bits = slang::SVInt::fromString(observation.outputs.at(port));
            require((bits.getRawPtr()[0] & ((uint64_t{1} << bits.getBitWidth()) - 1)) == value, name + " scoreboard mismatch " + port);
        }
    };
    if (name == "masked") firrtl_memory_test::maskTrace(check);
    else if (name == "chirrtl") firrtl_memory_test::chirrtlTrace(check);
    else firrtl_memory_test::multiClockTrace(check);
    const auto workload = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "memory/workload.hpp";
    std::string driver = "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + "\"\nint main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();firrtl_memory_test::" + (name == "masked" ? "maskTrace" : name == "chirrtl" ? "chirrtlTrace" : "multiClockTrace") + "([&](const auto& inputs,const auto& expected){\n";
    for (const auto& port : restored.findGraph("top")->inputPorts()) {
        driver += "if(const auto v=inputs.find(\"" + port.name + "\");v!=inputs.end()) {";
        if (port.name == "data__DOT__a" || port.name == "mask__DOT__a") {
            const auto width = port.name == "data__DOT__a" ? 4 : 1;
            driver += "m." + port.name + "[0]=v->second&" + std::to_string((1 << width) - 1) + ";m." + port.name + "[1]=v->second>>" + std::to_string(width) + ";";
        } else driver += "m." + port.name + "=v->second;";
        driver += "}\n";
    }
    driver += "m.eval();\n";
    if (name == "masked") driver += "if(!expected.empty()&&(uint64_t(m.y__DOT__a[0])|(uint64_t(m.y__DOT__a[1])<<4))!=expected.at(\"y__DOT__a\"))throw std::runtime_error(\"compiled aggregate mask\");if(!expected.empty()&&(uint64_t(m.y__DOT__b)&31)!=expected.at(\"y__DOT__b\"))throw std::runtime_error(\"compiled signed mask\");";
    else driver += "if(!expected.empty()&&m.y!=expected.at(\"y\"))throw std::runtime_error(\"compiled memory " + name + "\");";
    driver += "});}}\n";
    generate(restored, artifact("firrtl-memory") / name, driver); models.push_back(name);
}
void matrix(int readLatency, int writeLatency, const std::string& policy, bool independent = false, bool aliases = false) {
    const auto name = std::string(aliases ? "alias-" : independent ? "independent-" : "matrix-") + policy + "-r" + std::to_string(readLatency) + "-w" + std::to_string(writeLatency);
    std::string source = "    input clk : Clock\n    input ren : UInt<1>\n    input wen : UInt<1>\n    input mask : UInt<1>\n    input raddr : UInt<2>\n    input waddr : UInt<2>\n    input data : UInt<8>\n    output y : UInt<8>\n    mem m :\n      data-type => UInt<8>\n      depth => 3\n      reader => r\n      writer => w\n      read-latency => " + std::to_string(readLatency) + "\n      write-latency => " + std::to_string(writeLatency) + "\n      read-under-write => " + policy + R"FIR(
    connect m.r.addr, raddr
    connect m.r.en, ren
    connect m.r.clk, clk
    connect m.w.addr, waddr
    connect m.w.en, wen
    connect m.w.clk, clk
    connect m.w.data, data
    connect m.w.mask, mask
    connect y, m.r.data
)FIR";
    if (independent) {
        source.replace(source.find("input clk : Clock"), std::string_view("input clk : Clock").size(), "input rclk : Clock\n    input wclk : Clock");
        source.replace(source.find("m.r.clk, clk"), std::string_view("m.r.clk, clk").size(), "m.r.clk, rclk");
        source.replace(source.find("m.w.clk, clk"), std::string_view("m.w.clk, clk").size(), "m.w.clk, wclk");
    } else if (aliases) {
        source.insert(source.find("    mem m :"), "    node readClock = clk\n    wire writeClock : Clock\n    connect writeClock, clk\n");
        source.replace(source.find("m.r.clk, clk"), std::string_view("m.r.clk, clk").size(), "m.r.clk, readClock");
        source.replace(source.find("m.w.clk, clk"), std::string_view("m.w.clk, clk").size(), "m.w.clk, writeClock");
    }
    auto result = text(name, source);
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    const auto check = [&](const firrtl_memory_test::Values& inputs, const firrtl_memory_test::Values& expected) {
        const auto observation = oracle.eval(inputs);
        for (const auto& [port, value] : expected) {
            const auto actual = slang::SVInt::fromString(observation.outputs.at(port)).as<uint64_t>();
            require(actual && *actual == value, name + ": " + port + " got " + observation.outputs.at(port) + " expected " + std::to_string(value));
        }
    };
    if (independent) firrtl_memory_test::independentTrace(readLatency, writeLatency, policy, check);
    else firrtl_memory_test::standardTrace(readLatency, writeLatency, policy, check);
    const auto workload = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "memory/workload.hpp";
    std::string driver = "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + "\"\nint main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();firrtl_memory_test::" + std::string(independent ? "independentTrace" : "standardTrace") + "(" + std::to_string(readLatency) + "," + std::to_string(writeLatency) + ",\"" + policy + "\",[&](const auto& inputs,const auto& expected){\n";
    for (const auto& port : restored.findGraph("top")->inputPorts())
        driver += "if(const auto v=inputs.find(\"" + port.name + "\");v!=inputs.end()) m." + port.name + "=v->second;\n";
    driver += "m.eval();for(const auto& [port,value]:expected)if(m.y!=value)throw std::runtime_error(\"" + name + " compiled mismatch got \"+std::to_string(m.y)+\" expected \"+std::to_string(value));});}}\n";
    generate(restored, artifact("firrtl-memory") / name, driver); models.push_back(name);
}
void standard() {
    auto result = text("memory-standard", R"FIR(    input clk : Clock
    input wen : UInt<1>
    input ren : UInt<1>
    input addr : UInt<2>
    input data : UInt<8>
    input mask : UInt<1>
    output y : UInt<8>
    mem m :
      data-type => UInt<8>
      depth => 4
      read-latency => 1
      write-latency => 2
      read-under-write => old
      reader => r
      writer => w
    connect m.r.addr, addr
    connect m.r.en, ren
    connect m.r.clk, clk
    connect m.w.addr, addr
    connect m.w.en, wen
    connect m.w.clk, clk
    connect m.w.data, data
    connect m.w.mask, mask
    connect y, m.r.data
)FIR");
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    oracle.eval({{"clk", 0}, {"wen", 1}, {"ren", 1}, {"addr", 2}, {"data", 53}, {"mask", 1}});
    oracle.eval({{"clk", 1}}); oracle.eval({{"clk", 0}, {"wen", 0}}); oracle.eval({{"clk", 1}});
    oracle.eval({{"clk", 0}});
    require(slang::SVInt::fromString(oracle.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>() == 53, "standard memory delayed write/read mismatch");
    generate(restored, artifact("firrtl-memory") / "standard", R"CPP(#include "model/grhsim_top.hpp"
#include <stdexcept>
int main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();m.clk=0;m.wen=1;m.ren=1;m.addr=2;m.data=53;m.mask=1;m.eval();m.clk=1;m.eval();m.clk=0;m.wen=0;m.eval();m.clk=1;m.eval();m.clk=0;m.eval();m.clk=1;m.eval();if(m.y!=53)throw std::runtime_error("standard compiled mismatch");}}
)CPP");
}
void fourState() {
    const auto path = artifact("firrtl-fixtures") / "memory-four-state.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input ren : UInt<1>
    input wen : UInt<1>
    input addr : UInt<1>
    input data : UInt<8>
    output y : UInt<8>
    mem m :
      data-type => UInt<8>
      depth => 2
      reader => r
      writer => w
      read-latency => 2
      write-latency => 1
      read-under-write => undefined
    connect m.r.addr, addr
    connect m.r.en, ren
    connect m.r.clk, clk
    connect m.w.addr, addr
    connect m.w.en, wen
    connect m.w.clk, clk
    connect m.w.data, data
    connect m.w.mask, UInt<1>(1)
    connect y, m.r.data
)FIR";
    ingest::IngestRequest request;
    request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
    request.firrtl.initPolicy = ingest::FirrtlInitPolicy::Zero;
    auto result = ingest::readSources(request); require(result.success(), diagnostics(result));
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"), {}, nullptr, true);
    const auto cycle = [&](uint64_t ren, uint64_t wen, uint64_t data) {
        oracle.eval({{"clk", 0}, {"ren", ren}, {"wen", wen}, {"addr", 1}, {"data", data}});
        return slang::SVInt::fromString(oracle.eval({{"clk", 1}}).outputs.at("y"));
    };
    cycle(0, 1, 91);
    cycle(1, 0, 0);
    require(cycle(1, 0, 0).as<uint64_t>() == 91, "defined read did not survive FourState GRH");
    cycle(1, 0, 0);
    require(cycle(1, 1, 37).hasUnknown(), "undefined collision was silently converted to zero");
    require(cycle(1, 0, 0).hasUnknown(), "colliding initiating read lost unknown history");
    require(cycle(1, 0, 0).as<uint64_t>() == 37, "collision poison leaked into later reads");
    cycle(0, 0, 0);
    require(cycle(0, 0, 0).hasUnknown(), "disabled read must use selected invalid policy");
}
void depthOne() {
    auto result = text("memory-depth-one", R"FIR(    input clk : Clock
    input mode : UInt<1>
    input en : UInt<1>
    input mask : UInt<1>
    input data : UInt<8>
    output y : UInt<8>
    mem m :
      data-type => UInt<8>
      depth => 1
      readwriter => p
      read-latency => 0
      write-latency => 1
      read-under-write => new
    connect m.p.addr, UInt<0>(0)
    connect m.p.clk, clk
    connect m.p.en, en
    connect m.p.wmode, mode
    connect m.p.wdata, data
    connect m.p.wmask, mask
    connect y, m.p.rdata
)FIR");
    require(result.success(), diagnostics(result)); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    oracle.eval({{"clk", 0}, {"mode", 1}, {"en", 1}, {"mask", 1}, {"data", 93}});
    oracle.eval({{"clk", 1}});
    require(slang::SVInt::fromString(oracle.eval({{"mode", 0}}).outputs.at("y")).as<uint64_t>() == 93, "depth-one zero-address readwriter failed");
    oracle.eval({{"clk", 0}, {"mode", 1}, {"mask", 0}, {"data", 17}}); oracle.eval({{"clk", 1}});
    require(slang::SVInt::fromString(oracle.eval({{"mode", 0}}).outputs.at("y")).as<uint64_t>() == 93, "readwriter mask-zero wrote memory");
    generate(restored, artifact("firrtl-memory") / "depth-one", R"CPP(#include "model/grhsim_top.hpp"
#include <stdexcept>
int main(){GrhSIM_top m;m.init();m.clk=0;m.mode=1;m.en=1;m.mask=1;m.data=93;m.eval();m.clk=1;m.eval();m.mode=0;m.eval();if(m.y!=93)throw std::runtime_error("depth-one readwriter");m.clk=0;m.mode=1;m.mask=0;m.data=17;m.eval();m.clk=1;m.eval();m.mode=0;m.eval();if(m.y!=93)throw std::runtime_error("mask-zero readwriter");}
)CPP");
    models.push_back("depth-one");
}
void bundleRdwr() {
    auto result = text("memory-bundle-rdwr", R"FIR(    input clk : Clock
    input addr : UInt<2>
    input wa : UInt<1>
    input wb : UInt<1>
    input a : UInt<8>
    input b : UInt<8>
    output pa : UInt<8>
    output pb : UInt<8>
    output qa : UInt<8>
    output qb : UInt<8>
    cmem m : {a : UInt<8>, b : UInt<8>}[3]
    rdwr mport p = m[addr], clk
    read mport q = m[addr], clk
    when wa :
      connect p.a, a
    when wb :
      connect p.b, b
    connect pa, p.a
    connect pb, p.b
    connect qa, q.a
    connect qb, q.b
)FIR");
    require(result.success(), diagnostics(result)); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    firrtl_memory_test::bundleRdwrTrace([&](const firrtl_memory_test::Values& inputs, const firrtl_memory_test::Values& expected) {
        const auto observation = oracle.eval(inputs);
        for (const auto& [port, value] : expected)
            require(slang::SVInt::fromString(observation.outputs.at(port)).as<uint64_t>() == value, "bundle rdwr mode mismatch: " + port);
    });
    const auto workload = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "memory/workload.hpp";
    generate(restored, artifact("firrtl-memory") / "bundle-rdwr",
        "#include \"model/grhsim_top.hpp\"\n#include \"" + workload.string() + "\"\n"
        "int main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();firrtl_memory_test::bundleRdwrTrace([&](const auto& inputs,const auto& expected){"
        "for(const auto& [port,value]:inputs){if(port==\"clk\")m.clk=value;else if(port==\"addr\")m.addr=value;else if(port==\"wa\")m.wa=value;else if(port==\"wb\")m.wb=value;else if(port==\"a\")m.a=value;else if(port==\"b\")m.b=value;}m.eval();"
        "if(m.pa!=expected.at(\"pa\")||m.pb!=expected.at(\"pb\")||m.qa!=expected.at(\"qa\")||m.qb!=expected.at(\"qb\"))throw std::runtime_error(\"compiled bundle rdwr mode\");});}}\n");
    models.push_back("bundle-rdwr");
}
void partialArrayPort() {
    auto result = text("memory-partial-array-port", R"FIR(    input clk : Clock
    input wen : UInt<1>
    input second : UInt<1>
    input a : UInt<4>
    input b : UInt<4>
    output y : UInt<4>[2]
    cmem m : UInt<4>[2][3]
    write mport w = m[UInt<2>(1)], clk
    read mport r = m[UInt<2>(1)], clk
    when wen :
      connect w[0], a
      when second : connect w[1], b
    connect y, r
)FIR");
    require(result.success(), diagnostics(result)); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    grhsim_test::Oracle oracle(*restored.findGraph("top"));
    const auto cycle = [&](unsigned wen, unsigned second, unsigned a, unsigned b) {
        oracle.eval({{"clk", 0}, {"wen", wen}, {"second", second}, {"a", a}, {"b", b}});
        return slang::SVInt::fromString(oracle.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>();
    };
    require(cycle(1, 1, 3, 12) == 0xc3 && cycle(1, 0, 5, 9) == 0xc5 && cycle(0, 1, 15, 15) == 0xc5,
        "partial array mport lost its shape, mask or untouched element");
    generate(restored, artifact("firrtl-memory") / "partial-array-port", R"CPP(#include "model/grhsim_top.hpp"
#include <stdexcept>
int main(){GrhSIM_top m;m.init();
    const auto cycle=[&](unsigned wen,unsigned second,unsigned a,unsigned b,unsigned expected){
        m.clk=0;m.wen=wen;m.second=second;m.a=a;m.b=b;m.eval();m.clk=1;m.eval();
        if((unsigned(m.y[0])|(unsigned(m.y[1])<<4))!=expected)throw std::runtime_error("partial array mport");
    };
    cycle(1,1,3,12,0xc3);cycle(1,0,5,9,0xc5);cycle(0,1,15,15,0xc5);
}
)CPP");
    models.push_back("partial-array-port");
}
void test() {
    partialArrayPort();
    bundleRdwr();
    fourState(); depthOne();
    aggregateAndClocks("masked"); aggregateAndClocks("multi-clock"); aggregateAndClocks("chirrtl");
    error("memory-zero-depth", "    cmem m : UInt<8>[0]\n", "depth and write latency");
    error("memory-const-write", "    input clk : Clock\n    cmem m : const UInt<8>[2]\n    write mport w = m[UInt<1>(0)], clk\n    connect w, UInt<8>(1)\n", "constant memory");
    error("memory-inferred-const-write", "    input clk : Clock\n    cmem m : const UInt<8>[2]\n    infer mport w = m[UInt<1>(0)], clk\n    connect w, UInt<8>(1)\n", "constant memory");
    error("memory-non-clock", "    input clk : UInt<1>\n    cmem m : UInt<8>[2]\n    read mport r = m[UInt<1>(0)], clk\n", "Clock type");
    for (const std::string policy : {"old", "new", "undefined"})
        for (const auto [r, w] : {std::pair{0, 1}, {1, 1}, {2, 1}, {1, 2}, {3, 3}}) matrix(r, w, policy);
    for (const std::string policy : {"old", "new", "undefined"})
        for (const auto [r, w] : {std::pair{1, 1}, {2, 1}, {3, 2}, {2, 3}}) matrix(r, w, policy, true);
    for (const std::string policy : {"old", "new", "undefined"}) matrix(2, 1, policy, false, true);
    standard();
    auto result = text("memory-cmem", R"FIR(    input clk : Clock
    input wen : UInt<1>
    input addr : UInt<3>
    input data : UInt<8>
    output y : UInt<8>
    cmem m : UInt<8>[4]
    read mport r = m[addr], clk
    when wen :
      write mport w = m[addr], clk
      connect w, data
    connect y, r
)FIR");
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    grhsim_test::Oracle oracle(*result.design->findGraph("top"));
    oracle.eval({{"clk", 0}, {"wen", 1}, {"addr", 2}, {"data", 53}});
    require(slang::SVInt::fromString(oracle.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>() == 53, "cmem indexed write/read mismatch");
    auto restored = roundTrip(*result.design); valid(restored);
    const auto output = artifact("firrtl-memory");
    generate(restored, output / "cmem", R"CPP(#include "model/grhsim_top.hpp"
#include <stdexcept>
int main(){for(int epoch=0;epoch<3;++epoch){GrhSIM_top m;m.init();m.clk=0;m.wen=1;m.addr=2;m.data=53;m.eval();m.clk=1;m.eval();if(m.y!=53)throw std::runtime_error("cmem compiled mismatch");}}
)CPP");
    std::ofstream make(output / "run.mk");
    make << ".PHONY: run cmem standard";
    for (const auto& name : models) make << ' ' << name;
    make << "\nrun: cmem standard";
    for (const auto& name : models) make << ' ' << name;
    make << "\ncmem standard";
    for (const auto& name : models) make << ' ' << name;
    make << ":\n\t$(MAKE) -C $@ -f run.mk run\n";
}
}
int main() { try { test(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
