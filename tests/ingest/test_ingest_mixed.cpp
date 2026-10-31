#include "firrtl_emit_fixture.hpp"
#include "ingest/sv.hpp"

namespace {
using namespace firrtl_test;
constexpr std::string_view driver = R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main() {
    GrhSIM_top model; model.init();
    for (unsigned a=0; a<256; ++a) {
        model.a=a; model.eval(); assert(model.y==(((a+3)&255)^85));
    }
}
)CPP";
void negativeTests(const std::filesystem::path& output, ingest::IngestRequest good) {
    const auto reject = [&](const ingest::IngestRequest& request, std::string_view message) {
        auto result = ingest::readSources(request);
        require(!result.success() && !result.design, "failed mixed request published a partial design");
        require(diagnostics(result).find(message) != std::string::npos, std::string(message) + ": " + diagnostics(result));
    };
    auto missing = good; missing.sources.pop_back(); reject(missing, "missing external implementation");
    auto duplicate = good; duplicate.sources.push_back(good.sources.front()); reject(duplicate, "duplicate FIRRTL source unit");
    const auto wrongSv = output / "wrong.sv";
    for (const auto text : {
        "module bridge(input logic [7:0] a,output logic [6:0] y);assign y=a;endmodule",
        "module bridge(input logic [7:0] a,input logic [7:0] y);endmodule",
        "module bridge(input logic [7:0] a,output logic [7:0] wrong);assign wrong=a;endmodule",
        "module bridge(input logic [7:0] a,output logic [7:0] y,output logic extra);assign y=a;assign extra=0;endmodule"}) {
        std::ofstream(wrongSv) << text;
        auto request = good; request.sources.back().path = wrongSv;
        auto result = ingest::readSources(request);
        require(!result.success() && !result.design, "bad boundary ABI published a design");
        require(!result.diagnostics.empty(), "bad boundary ABI lacked diagnostics");
    }
    std::ofstream(wrongSv) << "module bridge(input logic [7:0] a,output logic [7:0] y);assign y=a;endmodule\nmodule leaf;endmodule\n";
    auto duplicateReal = good; duplicateReal.sources.back().path = wrongSv;
    reject(duplicateReal, "duplicate real module");
    std::ofstream(wrongSv) << "module bridge(input logic [7:0] a,output logic [7:0] y);top again(.a(a),.y(y));endmodule";
    auto recursive = good; recursive.sources.back().path = wrongSv; reject(recursive, "recursive module");
    ingest::ExternalBinding binding; binding.moduleName = "External";
    binding.implementation = {ingest::SourceLanguage::SystemVerilog, "sv", "bridge", {}};
    auto duplicateBinding = good; duplicateBinding.externalBindings = {binding, binding};
    reject(duplicateBinding, "duplicate external binding");
    auto unknownUnit = good; binding.implementation.sourceUnit = "absent"; unknownUnit.externalBindings = {binding};
    reject(unknownUnit, "unknown SV implementation source unit");
    binding.implementation = {ingest::SourceLanguage::Firrtl, "wrong", "leaf", {}};
    auto wrongFirrtlUnit = good; wrongFirrtlUnit.externalBindings = {binding};
    reject(wrongFirrtlUnit, "FIRRTL implementation belongs to another source unit");
    binding.implementation = {ingest::SourceLanguage::SystemVerilog, {}, "bridge", {
        {"N", ingest::IntegerParameter{8, false, "00000001"}}, {"N", ingest::IntegerParameter{8, false, "00000010"}}}};
    auto duplicateParameter = good; duplicateParameter.externalBindings = {binding};
    reject(duplicateParameter, "duplicate specialization parameter");
    binding.implementation = {ingest::SourceLanguage::SystemVerilog, {}, "bridge", {}};
    binding.ports = {{"a", "a"}, {"a", "a"}};
    auto duplicatePort = good; duplicatePort.externalBindings = {binding}; reject(duplicatePort, "duplicate source port binding");
    const auto unresolvedFir = output / "unresolved-boundary.fir";
    std::ofstream(unresolvedFir) << R"FIR(FIRRTL version 6.0.0
circuit External :
  extmodule External :
    input reset : Reset
    output y : UInt<1>
    defname = reset_impl
)FIR";
    std::ofstream(wrongSv) << "module reset_impl(input logic reset, output logic y);assign y=reset;endmodule";
    auto unresolved = good;
    unresolved.sources = {{ingest::SourceLanguage::Firrtl, unresolvedFir, "fir"},
                          {ingest::SourceLanguage::SystemVerilog, wrongSv, "sv"}};
    unresolved.tops.clear();
    reject(unresolved, "unresolved FIRRTL external boundary");
    const auto annotation = output / "invalid-sidecar.json";
    std::ofstream(annotation) << "[\n{\"class\":\"unknown.Annotation\",\"target\":\"~|External\"}\n]";
    auto sidecarRequest = good; sidecarRequest.firrtl.annotationFiles = {annotation.string()};
    auto sidecar = ingest::readSources(sidecarRequest);
    require(!sidecar.success() && !sidecar.design && sidecar.diagnostics.front().location &&
        sidecar.diagnostics.front().location->file == annotation && sidecar.diagnostics.front().location->line == 2,
        "sidecar diagnostic lost its owning filename/line");
    auto first = ingest::readSources(good); auto second = ingest::readSources(good);
    require(first.success() && second.success() && first.dependencies == second.dependencies, "repeated mixed dependencies are unstable");
    auto owners = first.sourceOwners;
    owners.insert(owners.end(), second.sourceOwners.begin(), second.sourceOwners.end());
    auto cloned = first.design->clone(); first = {}; second = {};
    require(!ingest::svSourceManager(owners), "multiple SV SourceManagers must not alias their buffer IDs");
    tx::SessionStore session; pass(cloned, session, "hier-flatten");
    const auto records = node::inspect(*cloned.findGraph("top")); require(!records.nodes.empty(), "cloned mixed source lost node identities");
    require(std::any_of(records.nodes.begin(), records.nodes.end(), [](const auto& record) {
        return node::parseKey(record.key).instancePath.size() >= 3;
    }), "flatten collapsed a mixed hierarchy into a single node path component");
    auto specialized = ingest::specializeModule(good, {ingest::SourceLanguage::Firrtl, "fir", "top", {}});
    require(specialized.success(), diagnostics(specialized));
    std::vector<ingest::IngestResult> modules; modules.push_back(std::move(specialized));
    auto linked = ingest::linkModules({}, std::move(modules)); require(linked.success(), diagnostics(linked));
    ingest::IngestResult cyclic;
    cyclic.design.emplace();
    auto& graph = cyclic.design->createGraph("cycle"); cyclic.design->markAsTop("cycle");
    const auto call = graph.createOperation(OperationKind::kInstance);
    graph.setAttr(call, "moduleName", std::string("cycle"));
    graph.setAttr(call, "inputPortName", std::vector<std::string>{});
    graph.setAttr(call, "outputPortName", std::vector<std::string>{});
    graph.setAttr(call, "inoutPortName", std::vector<std::string>{});
    modules.push_back(std::move(cyclic));
    auto recursion = ingest::linkModules({}, std::move(modules));
    require(!recursion.success() && !recursion.design && diagnostics(recursion).find("recursive linked") != std::string::npos,
        "public linkModules accepted recursion");
}
void test() {
    const auto output = artifact("ingest-mixed");
    const auto path = output / "hierarchy.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt<8>
    output y : UInt<8>
    node n = add(a, UInt<8>(3))
    connect y, n
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    inst child of leaf
    connect child.a, a
    connect y, xor(child.y, UInt<8>(85))
)FIR";
    auto result = read(path); require(result.success(), diagnostics(result));
    require(result.design->graphs().size()==2, "hierarchy dependency was not lowered");
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    require(!node::inspect(*result.design->findGraph("top")).nodes.empty(), "hierarchy lost node provenance");
    generate(*result.design, output / "hierarchy", driver);
    const auto mixedPath = output / "mixed.fir";
    std::ofstream(mixedPath) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt<8>
    output y : UInt<8>
    node n = add(a, UInt<8>(3))
    connect y, n
  extmodule External :
    input a : UInt<8>
    output y : UInt<8>
    defname = bridge
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    inst child of External
    connect child.a, a
    connect y, child.y
)FIR";
    const auto svPath = output / "mixed.sv";
    std::ofstream(svPath) << R"SV(module bridge(input logic [7:0] a, output logic [7:0] y);
    logic [7:0] t;
    leaf child(.a(a), .y(t));
    assign y = t ^ 8'd85;
endmodule
module svtop(input logic [7:0] a, output logic [7:0] y);
    top child(.a(a), .y(y));
endmodule
)SV";
    ingest::IngestRequest good; good.logLevel = wolvrix::lib::LogLevel::Off; good.tops = {"top"};
    good.sources = {{ingest::SourceLanguage::Firrtl, mixedPath, "fir"}, {ingest::SourceLanguage::SystemVerilog, svPath, "sv"}};
    negativeTests(output, good);
    const auto actualPath = output / "actual.fir";
    std::ofstream(actualPath) << R"FIR(FIRRTL version 6.0.0
circuit actual :
  public module actual :
    input a : UInt<8>
    output y : UInt<8>
    node increment = add(a, UInt<8>(3))
    node result = xor(bits(increment, 7, 0), UInt<8>(85))
    connect y, result
)FIR";
    auto crossSourceRequest = good; crossSourceRequest.sources.pop_back();
    crossSourceRequest.sources.push_back({ingest::SourceLanguage::Firrtl, actualPath, "other"});
    ingest::ExternalBinding crossSourceBinding; crossSourceBinding.moduleName = "External";
    crossSourceBinding.implementation = {ingest::SourceLanguage::Firrtl, "other", "actual", {}};
    crossSourceRequest.externalBindings.push_back(crossSourceBinding);
    auto crossSource = ingest::readSources(crossSourceRequest); require(crossSource.success(), diagnostics(crossSource));
    tx::SessionStore crossSourceSession; pass(*crossSource.design, crossSourceSession, "hier-flatten");
    generate(*crossSource.design, output / "cross-source", driver);
    for (const std::string top : {"top", "svtop"}) {
        ingest::IngestRequest request; request.logLevel = wolvrix::lib::LogLevel::Off; request.tops = {top};
        request.sources = {{ingest::SourceLanguage::Firrtl, mixedPath, "fir"}, {ingest::SourceLanguage::SystemVerilog, svPath, "sv"}};
        auto mixed = ingest::readSources(request); require(mixed.success(), diagnostics(mixed));
        require(mixed.sourceOwners.size()>=2, "mixed request lost source owners");
        tx::SessionStore mixedSession; pass(*mixed.design, mixedSession, "hier-flatten");
        if (mixed.design->topGraphs().front() != "top") {
            const auto name = mixed.design->topGraphs().front();
            // The fixture generator has a fixed model basename.
            mixed.design->deleteGraph("top");
            mixed.design->cloneGraph(name, "top"); mixed.design->unmarkAsTop(name); mixed.design->markAsTop("top");
        }
        auto hinted = mixed.design->clone();
        generate(*mixed.design, output / top, driver);
        generate(hinted, output / (top + "-hints"), driver, {"-firrtl-nodes=prefer", "-cost-model=bytes", "-fuse-leaves=1"});
    }
    const auto parameterPath = output / "parameters.fir";
    std::ofstream(parameterPath) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule Add3 :
    input a : UInt<8>
    output y : UInt<8>
    defname = adder
    parameter W = 8
    parameter N = 3
  extmodule Add9 :
    input a : UInt<8>
    output y : UInt<8>
    defname = adder
    parameter W = 8
    parameter N = 9
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    output z : UInt<8>
    inst u of Add3
    inst v of Add9
    inst duplicate of Add3
    connect u.a, a
    connect v.a, a
    connect duplicate.a, a
    connect y, u.y
    connect z, v.y
)FIR";
    const auto parameterSv = output / "parameters.sv";
    std::ofstream(parameterSv) << R"SV(module adder #(parameter int W=4, N=1) (input logic [W-1:0] a, output logic [W-1:0] y);
    assign y = a + N;
endmodule
)SV";
    ingest::IngestRequest parameterRequest; parameterRequest.logLevel = wolvrix::lib::LogLevel::Off;
    parameterRequest.sources = {{ingest::SourceLanguage::Firrtl, parameterPath, "fir"}, {ingest::SourceLanguage::SystemVerilog, parameterSv, "sv"}};
    auto parameters = ingest::readSources(parameterRequest); require(parameters.success(), diagnostics(parameters));
    unsigned implementations = 0;
    for (const auto& name : parameters.design->graphOrder())
        if (parameters.design->findGraph(name)->metadata("ingest.sourceModule") == grh::AttributeValue(std::string("adder"))) ++implementations;
    require(implementations==2, "typed parameter specialization was merged or duplicated");
    tx::SessionStore parameterSession; pass(*parameters.design, parameterSession, "hier-flatten");
    generate(*parameters.design, output / "parameters", R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main(){GrhSIM_top m;m.init();for(unsigned a=0;a<256;++a){m.a=a;m.eval();assert(m.y==((a+3)&255));assert(m.z==((a+9)&255));}}
)CPP");
    const auto abiFir = output / "abi.fir";
    std::ofstream(abiFir) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule Flat :
    input io : { a : UInt<8>, flip y : SInt<8> }
    defname = flat
  extmodule Aggregate :
    input a : { lo : UInt<8>, hi : UInt<8> }
    output y : { lo : UInt<8>, hi : UInt<8> }
    defname = aggregate
  extmodule Arrays :
    input a : UInt<8>[2][1]
    output y : UInt<8>[2][1]
    defname = arrays
  public module top :
    input a : UInt<8>
    input b : UInt<8>
    output flat : SInt<8>
    output aggregate : UInt<16>
    output array : UInt<16>
    inst f of Flat
    inst g of Aggregate
    inst v of Arrays
    connect f.io.a, a
    connect flat, f.io.y
    connect g.a.lo, a
    connect g.a.hi, b
    connect aggregate, cat(g.y.hi, g.y.lo)
    connect v.a[0][0], a
    connect v.a[0][1], b
    connect array, cat(v.y[0][1], v.y[0][0])
)FIR";
    const auto abiSv = output / "abi.sv";
    std::ofstream(abiSv) << R"SV(module flat(input logic [7:0] io_a, output logic [7:0] io_y);
assign io_y = io_a ^ 8'ha5;
endmodule
typedef struct packed {logic [7:0] lo; logic [7:0] hi;} pair_t;
module aggregate(input pair_t a, output pair_t y);
assign y.lo = a.hi; assign y.hi = a.lo;
endmodule
module arrays(input logic [7:0] a[0:0][0:1], output logic [7:0] y[0:0][0:1]);
assign y[0][0] = a[0][1]; assign y[0][1] = a[0][0];
endmodule
)SV";
    ingest::IngestRequest abiRequest; abiRequest.logLevel = wolvrix::lib::LogLevel::Off;
    abiRequest.sources = {{ingest::SourceLanguage::Firrtl, abiFir, "fir"}, {ingest::SourceLanguage::SystemVerilog, abiSv, "sv"}};
    auto abi = ingest::readSources(abiRequest); require(abi.success(), diagnostics(abi));
    const auto conflictingShape = output / "conflicting-shape.sv";
    std::ofstream(conflictingShape) << R"SV(module arrays(
input logic [7:0] a[0:1][0:0], output logic [7:0] y[0:1][0:0]);
assign y[0][0]=a[0][0]; assign y[1][0]=a[1][0];
endmodule
)SV";
    auto shapeRequest = abiRequest;
    shapeRequest.sources.back().path = conflictingShape;
    shapeRequest.tops = {"Arrays"};
    auto shapeFailure = ingest::readSources(shapeRequest);
    require(!shapeFailure.success() && !shapeFailure.design &&
        diagnostics(shapeFailure).find("incompatible boundary ABI") != std::string::npos,
        "equal-width incompatible array shapes were accepted: " + diagnostics(shapeFailure));
    wolvrix::lib::store::StoreJson abiStore;
    std::ofstream(output / "abi-linked.json") << *abiStore.storeToString(*abi.design);
    tx::SessionStore abiSession; pass(*abi.design, abiSession, "hier-flatten");
    generate(*abi.design, output / "abi", R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main(){GrhSIM_top m;m.init();for(unsigned a=0;a<256;++a){for(unsigned b:{0U,1U,127U,255U}){m.a=a;m.b=b;m.eval();assert((m.flat&255)==(a^165));assert(m.aggregate==((a<<8)|b));assert(m.array==((a<<8)|b));}}}
)CPP");
    // Each blackbox source transport must feed a real SV implementation.
    const auto sourceFir = output / "source.fir";
    std::ofstream(sourceFir) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule External :
    input a : UInt<8>
    output y : UInt<8>
    defname = implementation
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    inst u of External
    connect u.a, a
    connect y, u.y
)FIR";
    const std::string implementation = "module implementation(input logic [7:0] a,output logic [7:0] y);assign y=(a+3)^85;endmodule";
    std::filesystem::create_directories(output / "resources");
    std::ofstream(output / "resources/implementation.sv") << implementation;
    for (const std::string kind : {"inline", "path", "resource"}) {
        const auto annotation = output / (kind + ".json");
        auto record = std::string("{\"class\":\"firrtl.transforms.BlackBox");
        if (kind == "inline") record += "InlineAnno\",\"target\":\"~|External\",\"name\":\"implementation.sv\",\"text\":\"" + implementation + "\"}";
        if (kind == "path") record += "PathAnno\",\"target\":\"~|External\",\"path\":\"resources/implementation.sv\"}";
        if (kind == "resource") record += "ResourceAnno\",\"target\":\"~|External\",\"resourceId\":\"/implementation.sv\"}";
        std::ofstream(annotation) << '[' << record << (kind == "inline" ? "," + record : "") << ']';
        ingest::IngestRequest request; request.logLevel = wolvrix::lib::LogLevel::Off; request.inputRoot = output;
        request.sources = {{ingest::SourceLanguage::Firrtl, sourceFir, "fir"}};
        request.firrtl.annotationFiles = {annotation.string()}; request.firrtl.resourceRoots = {"resources"};
        auto parsed = ingest::readSources(request); require(parsed.success(), diagnostics(parsed));
        require(std::any_of(parsed.dependencies.begin(), parsed.dependencies.end(), [](const auto& entry){return entry.starts_with("annotation:");}), "missing sidecar dependency");
        tx::SessionStore session; pass(*parsed.design, session, "hier-flatten");
        generate(*parsed.design, output / kind, driver);
    }
    const auto macroFir = output / "macro.fir";
    std::ofstream(macroFir) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule External :
    input a : UInt<8>
    output y : UInt<8>
    defname = macro_impl
    parameter W = '`WIDTH'
    parameter N = '`INCREMENT'
    parameter LABEL = "eight"
  public module top :
    input a : UInt<8>
    output y : UInt<8>
    inst u of External
    connect u.a, a
    connect y, u.y
)FIR";
    std::ofstream(output / "constants.svh") << "`define WIDTH 8\n";
    const auto macroSv = output / "macro.sv";
    std::ofstream(macroSv) << R"SV(`include "constants.svh"
module macro_impl #(parameter int W=4, N=1, parameter string LABEL="", parameter real SCALE=1.0)
(input logic [W-1:0] a,output logic [W-1:0] y);
assign y = (a + N + int'(SCALE) + (LABEL == "eight" ? 0 : 1)) ^ 8'd85;
endmodule
)SV";
    ingest::IngestRequest macroRequest; macroRequest.logLevel = wolvrix::lib::LogLevel::Off;
    macroRequest.sources = {{ingest::SourceLanguage::Firrtl,macroFir,"fir"},{ingest::SourceLanguage::SystemVerilog,macroSv,"sv"}};
    macroRequest.slangArgs = {"-I"+output.string(), "-DINCREMENT=2"};
    ingest::ExternalBinding macroBinding; macroBinding.moduleName = "External";
    macroBinding.implementation = {ingest::SourceLanguage::SystemVerilog, "sv", "macro_impl", {
        {"W",ingest::RawStringParameter{"`WIDTH"}}, {"N",ingest::RawStringParameter{"`INCREMENT"}}, {"LABEL",std::string("eight")}, {"SCALE",1.0}}};
    macroRequest.externalBindings = {macroBinding};
    auto macro = ingest::readSources(macroRequest); require(macro.success(), diagnostics(macro));
    require(std::any_of(macro.dependencies.begin(),macro.dependencies.end(),[](const auto& dep){return dep.find("constants.svh")!=std::string::npos;}), "include dependency missing");
    tx::SessionStore macroSession; pass(*macro.design, macroSession, "hier-flatten");
    generate(*macro.design, output / "macro", driver);
    const auto dpiFir = output / "dpi.fir";
    std::ofstream(dpiFir) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule External :
    input clk : Clock
    input a : UInt<8>
    output y : UInt<8>
    defname = dpi_impl
  public module top :
    input clk : Clock
    input a : UInt<8>
    output y : UInt<8>
    inst u of External
    connect u.clk, clk
    connect u.a, a
    connect y, u.y
)FIR";
    const auto dpiSv = output / "dpi.sv";
    std::ofstream(dpiSv) << R"SV(import "DPI-C" function void ingest_capture(input byte unsigned a, output byte unsigned y);
module dpi_impl(input logic clk,input logic [7:0] a,output byte unsigned y);
always @(posedge clk) ingest_capture(a,y);
endmodule
)SV";
    ingest::IngestRequest dpiRequest; dpiRequest.logLevel = wolvrix::lib::LogLevel::Off;
    dpiRequest.sources = {{ingest::SourceLanguage::Firrtl,dpiFir,"fir"},{ingest::SourceLanguage::SystemVerilog,dpiSv,"sv"}};
    auto dpi = ingest::readSources(dpiRequest); require(dpi.success(), diagnostics(dpi));
    tx::SessionStore dpiSession; pass(*dpi.design,dpiSession,"hier-flatten");
    require(count(*dpi.design->findGraph("top"),OperationKind::kDpicCall)==1,"external implementation lost DPI effect");
    generate(*dpi.design,output / "dpi",R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
static unsigned calls;
extern "C" void ingest_capture(std::uint8_t a,std::uint8_t* y){++calls;*y=(a+3)^85;}
int main(){GrhSIM_top m;m.init();for(unsigned a=0;a<100;++a){m.clk=0;m.a=a;m.eval();const auto before=calls;m.clk=1;m.eval();assert(calls==before+1);assert(m.y==(((a+3)&255)^85));m.eval();assert(calls==before+1);}}
)CPP");
    std::ofstream(output / "run.mk") << ".PHONY: run hierarchy cross-source top svtop top-hints svtop-hints parameters abi inline path resource macro dpi\nrun: hierarchy cross-source top svtop top-hints svtop-hints parameters abi inline path resource macro dpi\nhierarchy cross-source top svtop top-hints svtop-hints parameters abi inline path resource macro dpi:\n\t$(MAKE) -C $@ -f run.mk run\n";
}
}
int main() { try { test(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
