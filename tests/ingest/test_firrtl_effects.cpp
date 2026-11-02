#include "firrtl_emit_fixture.hpp"

namespace {
using namespace firrtl_test;
void annotations() {
    auto result = read(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "effects/annotations.fir");
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>());
    auto restored = roundTrip(*result.design); auto& graph = *restored.findGraph("top");
    const auto records = graph.metadata("firrtl.annotations"), external = graph.metadata("firrtl.pendingExternalAnnotations");
    require(records && std::get<std::vector<std::string>>(*records).size() == 5, "annotation whitelist lost module records");
    require(external && std::get<std::vector<std::string>>(*external).size() == 1, "external annotation binding payload lost");
    require(graph.declaredSymbols().size() == 2, "DontTouch targets were not anchored");
    run(restored, std::make_unique<tx::SimplifyPass>());
    require(graph.findValue("observed") && graph.findValue("packet__DOT__bytes"), "DontTouch declaration lost in simplify");
    valid(restored);
    const auto path = artifact("firrtl-fixtures") / "annotations-unknown.fir";
    std::ofstream(path) << "FIRRTL version 6.0.0\ncircuit top :%[[{\"class\":\"unknown.Init\",\"target\":\"~|top\"}]]\n  public module top :\n";
    result = read(path);
    require(!result.success() && diagnostics(result).find("unsupported FIRRTL annotation class") != std::string::npos, "unknown observable annotation accepted");
    const auto reject = [&](std::string_view json, std::string_view message) {
        std::ofstream(path) << "FIRRTL version 6.0.0\ncircuit top :%[[\n  " << json << "\n]]\n  public module top :\n    skip\n";
        auto result = read(path);
        require(!result.success() && !result.design && diagnostics(result).find(message) != std::string::npos, diagnostics(result));
        const auto& location = result.diagnostics.front().location;
        require(location && location->line == 3 && location->sourceLine.find("class") != std::string::npos,
                "annotation error does not point to its JSON record");
    };
    reject(R"JSON({"class":"firrtl.passes.InlineAnnotation","target":"~|top","extra":"x"})JSON", "annotation fields");
    reject(R"JSON({"class":"firrtl.passes.InlineAnnotation","target":"~|top|other"})JSON", "annotation target");
    reject(R"JSON({"class":"firrtl.passes.InlineAnnotation","target":"~wrong|top"})JSON", "another circuit");
    reject(R"JSON({"class":"firrtl.passes.InlineAnnotation","target":"~|missing"})JSON", "unknown module");
    reject(R"JSON({"class":"firrtl.passes.InlineAnnotation","target":"~|top","x":"\ud800"})JSON", "surrogate");

    const std::string zeroWidth = R"FIR(FIRRTL version 6.0.0
circuit top :%[[
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>packet.mask"},
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>empty[1]"}
]]
  public module top :
    input packet : {mask : UInt<0>, data : UInt<8>}
    output y : UInt<8>
    wire empty : UInt<0>[2]
    connect y, packet.data
)FIR";
    std::ofstream(path) << zeroWidth;
    result = read(path); require(result.success(), diagnostics(result));
    restored = roundTrip(*result.design);
    auto& zeroGraph = *restored.findGraph("top");
    require(zeroGraph.declaredSymbols().empty() && !zeroGraph.findValue("packet__DOT__mask") && !zeroGraph.findValue("empty"),
        "zero-width DontTouch created a fake executable carrier");
    require(std::get<std::vector<std::string>>(*zeroGraph.metadata("firrtl.annotations")).size() == 2,
        "zero-width DontTouch annotation was dropped");
    valid(restored);
    auto missing = zeroWidth;
    missing.replace(missing.find("packet.mask"), std::string("packet.mask").size(), "packet.missing");
    std::ofstream(path) << missing;
    result = read(path);
    require(!result.success() && diagnostics(result).find("DontTouch target cannot be resolved") != std::string::npos,
        "zero-width target handling accepted an unknown field");

    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :%[[
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>state"},
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>packet"}
]]
  public module top :
    input clock : Clock
    input reset : Reset
    input a : UInt<3>
    regreset state : UInt<3>, clock, reset, UInt<3>(0)
    reg packet : {tag : UInt<3>, bytes : UInt<8>[2]}, clock
    connect state, a
    connect packet.tag, a
    connect packet.bytes[0], UInt<8>(1)
    connect packet.bytes[1], UInt<8>(2)
)FIR";
    result = read(path); require(result.success(), diagnostics(result));
    restored = roundTrip(*result.design);
    run(restored, std::make_unique<tx::SimplifyPass>());
    const auto& stateGraph = *restored.findGraph("top");
    for (const auto& name : {"state", "packet__DOT__tag", "packet__DOT__bytes"}) {
        const auto state = stateGraph.findOperation(name);
        require(state && stateGraph.getOperation(state).kind() == OperationKind::kRegister,
            "DontTouch did not preserve the register declaration");
    }
    valid(restored);

    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :%[[
  {"class":"firrtl.transforms.DontTouchAnnotation","target":"~|top>storage"}
]]
  public module top :
    cmem storage : {tag : UInt<3>, bytes : UInt<8>[2]}[4]
    cmem storageExtra : UInt<8>[4]
)FIR";
    result = read(path); require(result.success(), diagnostics(result));
    restored = roundTrip(*result.design);
    auto& memoryGraph = *restored.findGraph("top");
    require(memoryGraph.declaredSymbols().size() == 2,
        "memory DontTouch did not anchor exactly the aggregate memory leaves");
    for (const auto symbol : memoryGraph.declaredSymbols()) {
        const auto state = memoryGraph.findOperation(memoryGraph.symbolText(symbol));
        require(state && memoryGraph.getOperation(state).attr("firrtl.memory") == std::optional<grh::AttributeValue>(std::string("storage")),
            "memory DontTouch anchored an unrelated declaration");
    }
    run(restored, std::make_unique<tx::SimplifyPass>());
    for (const auto symbol : memoryGraph.declaredSymbols())
        require(memoryGraph.findOperation(memoryGraph.symbolText(symbol)).valid(), "memory DontTouch declaration lost in simplify");
    valid(restored);
}
void probes(const std::filesystem::path& output) {
    auto result = text("probe-aggregate", R"FIR(    input en : UInt<1>
    input a : {x : UInt<8>, y : UInt<4>[2]}
    output exposed : Probe<{x : UInt<8>, y : UInt<4>[2]}>
    output indexed : Probe<UInt<8>>[2]
    output y : UInt<16>
    wire local : Probe<{x : UInt<8>, y : UInt<4>[2]}>
    when en :
      define exposed = probe(a)
    define local = exposed
    define indexed[0] = probe(a.x)
    define indexed[1] = probe(a.x)
    invalidate exposed
    invalidate indexed
    invalidate local
    node observed = read(local)
    node first = read(indexed[0])
    connect y, cat(observed.x, cat(observed.y[1], observed.y[0]))
)FIR");
    require(result.success(), diagnostics(result));
    const auto& signature = result.modules.front();
    const auto exposed = std::find_if(signature.ports.begin(), signature.ports.end(), [](const auto& port) { return port.name == "exposed__DOT__x"; });
    require(exposed != signature.ports.end() && exposed->sourceType == "Probe<UInt>" && exposed->requiresTypeResolution,
            "lowered Probe interface lost its pending ABI distinction");
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    firrtl_test::Oracle oracle(*restored.findGraph("top"));
    for (const auto en : {0, 1})
        require(oracle.output("y", {{"en", en}, {"a__DOT__x", 65}, {"a__DOT__y", 0xba}}) == 0x41ba, "aggregate probe or unconditional define mismatch");
    generate(restored, output / "probes", R"CPP(#include "model/grhsim_top.hpp"
#include <stdexcept>
int main(){for(int i=0;i<3;++i){GrhSIM_top m;m.init();m.en=0;m.a__DOT__x=65;m.a__DOT__y[0]=10;m.a__DOT__y[1]=11;m.eval();if(m.y!=0x41ba||m.indexed[0]!=65||m.indexed[1]!=65)throw std::runtime_error("compiled probe mismatch");}}
)CPP");
    error("probe-duplicate", "    input a : UInt<8>\n    wire p : Probe<UInt<8>>\n    define p = probe(a)\n    define p = probe(a)\n", "multiple definitions");
    error("probe-undefined", "    wire p : Probe<UInt<8>>\n", "not fully initialized");
    error("probe-connect", "    input a : UInt<8>\n    wire p : Probe<UInt<8>>\n    connect p, probe(a)\n", "probes require define");
    error("probe-aggregate-connect", "    input a : {p : Probe<UInt<8>>}\n    output b : {p : Probe<UInt<8>>}\n    connect b, a\n", "probes require define");
    error("probe-vector-connect", "    input a : Probe<UInt<8>>[2]\n    output b : Probe<UInt<8>>[2]\n    connect b, a\n", "probes require define");
    error("probe-invalidate-undefined", "    wire p : Probe<UInt<8>>\n    invalidate p\n", "not fully initialized");
}
void layers(const std::filesystem::path& output) {
    const auto path = artifact("firrtl-fixtures") / "effects-layers.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  layer Verification, bind :
    layer Assert, inline :
  public module top :
    input clk : Clock
    input en : UInt<1>
    input data : UInt<8>
    output p : Probe<UInt<8>, Verification.Assert>
    output y : UInt<8>
    connect y, data
    layerblock Verification :
      layerblock Assert :
        define p = probe(data)
        node observed = read(p)
        printf(clk, en, "layer:%d\n", observed)
)FIR";
    for (const bool enabled : {false, true}) {
        ingest::IngestRequest request;
        request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
        if (enabled) request.firrtl.enabledLayers = {"Verification.Assert"};
        auto result = ingest::readSources(request); require(result.success(), diagnostics(result));
        run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
        require(count(*result.design->findGraph("top"), OperationKind::kSystemTask) == (enabled ? 1 : 0), "layer selection changed effect count");
        auto restored = roundTrip(*result.design); valid(restored);
        require(restored.findGraph("top")->metadata("firrtl.enabledLayers") == grh::AttributeValue(request.firrtl.enabledLayers), "layer configuration lost");
        if (enabled) generate(restored, output / "layers", R"CPP(#include "model/grhsim_top.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
int main(){std::ostringstream s;auto* old=std::cout.rdbuf(s.rdbuf());GrhSIM_top m;m.init();m.clk=0;m.en=1;m.data=42;m.eval();m.clk=1;m.eval();m.eval();std::cout.rdbuf(old);if(s.str()!="layer:42\n"||m.p!=42)throw std::runtime_error("layer compiled mismatch");}
)CPP");
    }
    const auto check = [&](std::string_view name, std::string_view body, std::vector<std::string> enabled, std::string_view message) {
        const auto path = artifact("firrtl-fixtures") / ("layer-" + std::string(name) + ".fir");
        std::ofstream(path) << "FIRRTL version 6.0.0\ncircuit top :\n  layer A, bind :\n    layer B, inline :\n  public module top :\n" << body;
        ingest::IngestRequest request;
        request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
        request.firrtl.enabledLayers = std::move(enabled);
        auto result = ingest::readSources(request);
        if (message.empty()) {
            require(result.success(), diagnostics(result)); valid(*result.design);
        } else {
            require(!result.success() && !result.design && diagnostics(result).find(message) != std::string::npos,
                    std::string(name) + ": " + diagnostics(result));
            require(result.diagnostics.front().location && result.diagnostics.front().location->line, "layer diagnostic has no source location");
        }
    };
    check("unknown", "    skip\n", {"missing"}, "unknown enabled");
    check("outside-write", "    output y : UInt<1>\n    connect y, UInt<1>(0)\n    layerblock A :\n      connect y, UInt<1>(1)\n", {"A"}, "outside its layer");
    check("outside-read", "    input p : Probe<UInt<1>, A>\n    output y : UInt<1>\n    connect y, read(p)\n", {"A"}, "outside its layer color");
    check("wrong-color", "    input a : UInt<1>\n    output p : Probe<UInt<1>, A>\n    layerblock A :\n      layerblock B :\n        define p = probe(a)\n", {"A.B"}, "same layer color");
    check("duplicate", "    input a : UInt<1>\n    output p : Probe<UInt<1>, A>\n    layerblock A :\n      define p = probe(a)\n    layerblock A :\n      define p = probe(a)\n", {"A"}, "multiple definitions");
    check("nested-local", "    input a : UInt<1>\n    output p : Probe<UInt<1>, A.B>\n    layerblock A :\n      wire local : Probe<UInt<1>>\n      define local = probe(a)\n      node observed = read(local)\n      layerblock B :\n        define p = probe(observed)\n", {"A.B"}, "");
    check("wrong-nesting", "    layerblock B :\n      skip\n", {"A.B"}, "incorrectly nested");
}
void ordered(const std::filesystem::path& output) {
    auto result = read(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "effects/ordered.fir");
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    auto restored = roundTrip(*result.design); valid(restored);
    std::size_t tasks = 0;
    for (const auto op : restored.findGraph("top")->operations())
        if (restored.findGraph("top")->opKind(op) == OperationKind::kSystemTask) ++tasks;
    require(tasks == 6, "effects lost during JSON round trip");
    generate(restored, output / "ordered", R"CPP(#include "model/grhsim_top.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
int main() {
    for (int epoch = 0; epoch < 3; ++epoch) {
        std::ostringstream trace; auto* previous = std::cout.rdbuf(trace.rdbuf());
        GrhSIM_top m; m.init(); m.clk=0; m.en=0; m.gate=0; m.data=65; m.eval();
        m.clk=1; m.eval(); m.eval();
        m.en=1; m.data=66; m.eval();
        if (!trace.str().empty()) throw std::runtime_error("effect without rising edge");
        m.clk=0; m.eval(); m.clk=1; m.eval(); m.eval();
        m.clk=0; m.gate=1; m.data=67; m.eval(); m.clk=1; m.eval();
        m.clk=0; m.en=0; m.eval(); m.clk=1; m.eval();
        std::cout.rdbuf(previous);
        const std::string expected = "A:65/41/101/A/%\nA:66/42/101/B/%\nB:66\nC%literal{{comment}}\n";
        if (trace.str() != expected) throw std::runtime_error("effect sampling/order/count: " + trace.str());
    }
}
)CPP");
}
void terminal(const std::filesystem::path& output, std::string_view name, std::string_view command, int code, std::string_view stderrText) {
    std::string body = "    input clk : Clock\n    input en : UInt<1>\n    input data : UInt<8>\n    printf(clk, en, \"before\\n\")\n    ";
    body += command; body += "\n    printf(clk, en, \"after\\n\")\n";
    auto result = text(name, body); require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    const auto path = output / name;
    generate(*result.design, path, R"CPP(#include "model/grhsim_top.hpp"
int main(){GrhSIM_top m;m.init();m.clk=0;m.en=0;m.data=9;m.eval();m.clk=1;m.eval();m.en=1;m.eval();m.clk=0;m.eval();m.clk=1;m.eval();return 99;}
)CPP");
    std::ofstream(path / "expected.out") << "before\n";
    std::ofstream(path / "expected.err") << stderrText;
    std::ofstream(path / "check.mk") << ".PHONY: check\ncheck:\n\t@set -eu; status=0; ./driver > actual.out 2> actual.err || status=$$?; test $$status -eq " << code
        << "; cmp expected.out actual.out; cmp expected.err actual.err\n";
}
void test() {
    const auto output = artifact("firrtl-effects");
    ordered(output);
    annotations();
    probes(output); layers(output);
    terminal(output, "stop", "stop(clk, en, 7) : end", 7, "");
    terminal(output, "stop-negative", "stop(clk, en, -1)", 255, "");
    terminal(output, "assert", "assert(clk, UInt<1>(0), en, \"bad=%d\", data)", 1, "[fatal] bad=9\n");
    terminal(output, "assume", "assume(clk, UInt<1>(0), en, \"assumed=%d\", data)", 1, "[fatal] assumed=9\n");
    terminal(output, "intrinsic", "intrinsic(circt_chisel_ifelsefatal<format=\"intrinsic bad\",label=\"chisel3_builtin\">,clk,UInt<1>(0),en)", 1, "[fatal] intrinsic bad\n");
    terminal(output, "intrinsic-args", "intrinsic(circt_chisel_ifelsefatal<format=\"intrinsic bad %d 0x%x\",label=\"chisel3_builtin\">,clk,UInt<1>(0),en,UInt<8>(42),UInt<8>(42))", 1, "[fatal] intrinsic bad 42 0x2a\n");
    error("effect-width", "    input c : Clock\n    printf(c, UInt<2>(1), \"x\")\n", "UInt<1>");
    error("effect-format", "    input c : Clock\n    printf(c, UInt<1>(1), \"%d\")\n", "argument count");
    error("effect-unknown-intrinsic", "    intrinsic(unknown_effect)\n", "unsupported effect intrinsic");
    error("effect-unknown-format", "    input c : Clock\n    printf(c, UInt<1>(1), \"%q\", UInt<1>(0))\n", "unsupported FIRRTL format");
    error("effect-special-format", "    input c : Clock\n    printf(c, UInt<1>(1), \"{{SimulationTime}}\")\n", "special format substitution");
    error("effect-force", "    input a : UInt<1>\n    wire p : RWProbe<UInt<1>>\n    define p = rwprobe(a)\n    force_initial(p, UInt<1>(0))\n", "not supported yet");
    error("external-binding", "    inst impl of External\n", "unknown module");
    const auto externalPath = artifact("firrtl-fixtures") / "effect-external.fir";
    std::ofstream(externalPath) << "FIRRTL version 6.0.0\ncircuit top :\n  extmodule External :\n    defname = Impl\n  public module top :\n    inst impl of External\n";
    auto external = read(externalPath);
    require(!external.success() && !external.design && diagnostics(external).find("external implementation binding for 'External'") != std::string::npos,
            "missing external implementation did not produce a binding request");
    require(external.modules.size() == 2 && external.modules.front().external && external.modules.front().implementationName == "Impl",
            "external signature was lost with the binding diagnostic");
    std::ofstream(output / "run.mk") << ".PHONY: run ordered probes layers stop stop-negative assert assume intrinsic intrinsic-args\nrun: ordered probes layers stop stop-negative assert assume intrinsic intrinsic-args\nordered probes layers:\n\t$(MAKE) -C $@ -f run.mk run\nstop stop-negative assert assume intrinsic intrinsic-args:\n\t$(MAKE) -C $@ -f run.mk driver\n\t$(MAKE) -C $@ -f check.mk check\n";
}
}
int main() { try { test(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
