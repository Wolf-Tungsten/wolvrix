#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "ingest/sv.hpp"
#include "../../lib/ingest/mixed.hpp"

#include <tuple>

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
void explicitPrefix(ingest::IngestRequest request) {
    request.firrtl.lowerThreads = 1;
    auto baseline = ingest::readSources(request);
    require(baseline.success(), diagnostics(baseline));
    require(baseline.prefixStages.empty(), "raw ingest unexpectedly ran the production prefix");
    const auto portOrder = [](const auto& graph) {
        std::vector<std::string> order;
        for (const auto& port : graph.inputPorts()) order.push_back("input:" + port.name);
        for (const auto& port : graph.outputPorts()) order.push_back("output:" + port.name);
        for (const auto& port : graph.inoutPorts()) order.push_back("inout:" + port.name);
        return order;
    };
    std::map<std::string, std::vector<std::string>> topPorts;
    for (const auto& top : baseline.design->topGraphs()) topPorts.emplace(top, portOrder(*baseline.design->findGraph(top)));
    tx::PassManagerOptions options; options.keepDeclaredSymbols = true;
    tx::PassManager manager(options);
    const auto add = [&](std::string_view name, std::initializer_list<std::string_view> arguments = {}) {
        std::string error;
        auto pass = tx::makePass(name, std::span(arguments.begin(), arguments.size()), error);
        require(bool(pass), error); manager.addPass(std::move(pass));
    };
    add("xmr-resolve"); add("mux-tree-fold");
    add("register-write-normalize", {"-semantics=2state"});
    add("mux-tree-fold", {"-guarded-writes=2state"});
    add("memory-read-retime"); add("multidriven-guard");
    add("blackbox-guard"); add("latch-transparent-read"); add("hier-flatten");
    tx::PassDiagnostics expected;
    require(manager.run(*baseline.design, expected).success, "serial production prefix failed");
    wolvrix::lib::store::StoreJson store;
    const auto json = store.storeToString(*baseline.design);
    require(json.has_value(), "serial prefix could not be serialized");
    for (const std::size_t threads : {1, 2, 4, 10}) {
        request.firrtl.lowerThreads = threads;
        request.pipeline = ingest::IngestPipeline::GrhPreFlatten;
        auto fused = ingest::readSources(request);
        require(fused.success(), diagnostics(fused));
        require(fused.prefixStages.size() == 8 && fused.ingestDiagnosticCount == baseline.diagnostics.size(),
            "explicit prefix lost its raw endpoint boundary or production stages");
        for (const auto& [top, order] : topPorts)
            require(portOrder(*fused.design->findGraph(top)) == order, "explicit prefix sorted observable top-level port order");
        tx::PassManager flatten(options);
        std::string error; flatten.addPass(tx::makePass("hier-flatten", {}, error));
        tx::PassDiagnostics flatDiagnostics;
        require(flatten.run(*fused.design, flatDiagnostics).success, "flatten after explicit prefix failed");
        const auto actual = store.storeToString(*fused.design);
        require(actual.has_value(), "explicit worker prefix could not be serialized");
        if (*actual != *json) {
            const auto label = request.sources.front().path.stem().string() + "-t" + std::to_string(threads);
            const auto expectedPath = artifact("ingest-mixed") / ("prefix-" + label + "-expected.json");
            const auto actualPath = artifact("ingest-mixed") / ("prefix-" + label + "-actual.json");
            std::ofstream(expectedPath, std::ios::binary) << *json;
            std::ofstream(actualPath, std::ios::binary) << *actual;
            const auto mismatch = std::mismatch(json->begin(), json->end(), actual->begin(), actual->end());
            const auto offset = static_cast<std::size_t>(mismatch.first - json->begin());
            const auto line = 1 + std::count(json->begin(), mismatch.first, '\n');
            const auto begin = offset > 96 ? offset - 96 : 0;
            throw std::runtime_error("explicit worker prefix changed complete flattened JSON: byte=" +
                std::to_string(offset) + " expected_line=" + std::to_string(line) +
                " expected=" + expectedPath.string() + " actual=" + actualPath.string() +
                "\nexpected context: " + json->substr(begin, 240) +
                "\nactual context: " + actual->substr(std::min(begin, actual->size()), 240));
        }
        std::vector<std::tuple<int, std::string, std::string, std::string, std::string>> oldMessages, newMessages;
        for (const auto& entry : expected.messages()) oldMessages.emplace_back(static_cast<int>(entry.kind),
            entry.message, entry.context, entry.passName, entry.originSymbol);
        for (const auto& stage : fused.prefixStages)
            for (const auto& entry : stage.diagnostics) newMessages.emplace_back(static_cast<int>(entry.kind),
                entry.message, entry.context, entry.passName, entry.originSymbol);
        for (const auto& entry : flatDiagnostics.messages()) newMessages.emplace_back(static_cast<int>(entry.kind),
            entry.message, entry.context, entry.passName, entry.originSymbol);
        require(oldMessages == newMessages, "explicit prefix changed serial diagnostic order or text");
    }
}
void compactionEligibility() {
    grh::Design design;
    auto& top = design.createGraph("top"); design.markAsTop("top");
    auto& child = design.createGraph("child"); design.createGraph("unreferenced");
    for (const auto name : {"z", "a"}) {
        const auto value = child.createValue(child.internSymbol(name), 1, false);
        child.bindInputPort(name, value);
    }
    const auto instance = top.createOperation(OperationKind::kInstance);
    top.setAttr(instance, "moduleName", std::string("child"));
    top.setAttr(instance, "inputPortName", std::vector<std::string>{"z", "a"});
    auto eligible = ingest::detail::compactablePrefixSources(design);
    require(eligible.size() == 1 && eligible.contains(&child), "named source was not eligible for worker compaction");
    require(!eligible.contains(&top), "top graph entered source compaction");
    top.eraseAttr(instance, "inputPortName");
    require(ingest::detail::compactablePrefixSources(design).empty(), "positional source would have reordered its port ABI");
    top.setAttr(instance, "inputPortName", std::vector<std::string>{"a", "a"});
    require(ingest::detail::compactablePrefixSources(design).empty(), "incomplete named map entered source compaction");
    top.setAttr(instance, "inputPortName", std::vector<std::string>{"z", "a"});
    const auto blackbox = top.createOperation(OperationKind::kBlackbox);
    top.setAttr(blackbox, "moduleName", std::string("child"));
    require(ingest::detail::compactablePrefixSources(design).empty(), "positional blackbox reference was ignored");
    top.setAttr(blackbox, "inputPortName", std::vector<std::string>{"a", "z"});
    require(ingest::detail::compactablePrefixSources(design).contains(&child), "all-named source failed compaction eligibility");
    require(child.inputPorts().front().name == "z", "compaction eligibility mutated source port order");
}
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
void hierarchicalReset() {
    const auto path = artifact("ingest-mixed") / "hierarchical-reset.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input clk : Clock
    input rst : Reset
    input d : UInt<8>
    output q : UInt<8>
    regreset r : UInt<8>, clk, rst, UInt<8>(7)
    connect r, d
    connect q, r
  module middle :
    input clk : Clock
    input rst : Reset
    input d : UInt<8>
    output q : UInt<8>
    inst l of leaf
    connect l.clk, clk
    connect l.rst, rst
    connect l.d, d
    connect q, l.q
  public module top :
    input clk : Clock
    input arst : AsyncReset
    input d : UInt<8>
    output q : UInt<8>
    inst m of middle
    connect m.clk, clk
    connect m.rst, arst
    connect m.d, d
    connect q, m.q
)FIR";
    auto result = read(path); require(result.success(), diagnostics(result));
    ingest::IngestRequest hierarchical;
    hierarchical.sources = {{ingest::SourceLanguage::Firrtl, path, "fixture"}};
    hierarchical.tops = {"top"};
    explicitPrefix(hierarchical);
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    const auto& graph = *result.design->findGraph("top");
    bool async = false;
    for (const auto op : graph.operations()) if (grh::isRegisterWritePortKind(graph.opKind(op)))
        async |= std::get<std::vector<std::string>>(*graph.getOperation(op).attr("eventEdge")).size() == 2;
    require(async, "parent AsyncReset did not propagate through abstract child ports");
    grhsim_test::Oracle oracle(graph);
    oracle.eval({{"clk", 0}, {"arst", 0}, {"d", 99}});
    oracle.eval({{"clk", 1}});
    oracle.eval({{"clk", 0}});
    const auto reset = oracle.eval({{"arst", 1}});
    require(slang::SVInt::fromString(reset.outputs.at("q")).as<uint64_t>() == 7,
        "hierarchical reset did not fire independently of the clock");
}
void externalReset() {
    const auto output = artifact("ingest-mixed");
    const auto path = output / "external-reset.fir";
    const auto implementation = output / "external-reset.sv";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule Sync :
    input clk : Clock
    input rst : Reset
    input d : UInt<8>
    output q : UInt<8>
    defname = sync_reset_impl
  extmodule Async :
    input clk : Clock
    input rst : Reset
    input d : UInt<8>
    output q : UInt<8>
    defname = async_reset_impl
  module middle :
    input clk : Clock
    input rst : Reset
    input arst : Reset
    input d : UInt<8>
    output sync : UInt<8>
    output async : UInt<8>
    inst s of Sync
    inst a of Async
    connect s.clk, clk
    connect s.rst, rst
    connect s.d, d
    connect a.clk, clk
    connect a.rst, arst
    connect a.d, d
    connect sync, s.q
    connect async, a.q
  public module top :
    input clk : Clock
    input rst : UInt<1>
    input arst : AsyncReset
    input d : UInt<8>
    output sync : UInt<8>
    output async : UInt<8>
    inst m of middle
    connect m.clk, clk
    connect m.rst, rst
    connect m.arst, arst
    connect m.d, d
    connect sync, m.sync
    connect async, m.async
)FIR";
    std::ofstream(implementation) << R"SV(
module sync_reset_impl(input logic clk, rst, input logic [7:0] d, output logic [7:0] q);
  always @(posedge clk) if(rst) q <= 8'd7; else q <= d;
endmodule
module async_reset_impl(input logic clk, rst, input logic [7:0] d, output logic [7:0] q);
  always @(posedge clk or posedge rst) if(rst) q <= 8'd9; else q <= d;
endmodule
)SV";
    ingest::IngestRequest request;
    request.sources = {{ingest::SourceLanguage::Firrtl, path, "fir"},
                       {ingest::SourceLanguage::SystemVerilog, implementation, "sv"}};
    auto result = ingest::readSources(request); require(result.success(), diagnostics(result));
    for (const auto& [name, kind] : {std::pair{"Sync", "UInt"}, std::pair{"Async", "AsyncReset"}}) {
        const auto signature = std::find_if(result.modules.begin(), result.modules.end(),
            [&](const auto& module) { return module.name == name; });
        require(signature != result.modules.end(), "external Reset signature disappeared");
        const auto port = std::find_if(signature->ports.begin(), signature->ports.end(),
            [](const auto& leaf) { return leaf.name == "rst"; });
        require(port != signature->ports.end() && port->sourceType == kind && !port->requiresTypeResolution,
            "external Reset signature ignored its parent constraints");
    }
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    grhsim_test::Oracle oracle(*result.design->findGraph("top"));
    oracle.eval({{"clk", 0}, {"rst", 0}, {"arst", 0}, {"d", 99}});
    oracle.eval({{"clk", 1}});
    oracle.eval({{"clk", 0}});
    const auto reset = oracle.eval({{"rst", 1}, {"arst", 1}});
    require(slang::SVInt::fromString(reset.outputs.at("sync")).as<uint64_t>() == 99 &&
            slang::SVInt::fromString(reset.outputs.at("async")).as<uint64_t>() == 9,
        "external asynchronous reset failed independently of the clock");
    const auto edge = oracle.eval({{"clk", 1}});
    require(slang::SVInt::fromString(edge.outputs.at("sync")).as<uint64_t>() == 7,
        "external synchronous reset failed on the clock edge");

    std::ifstream input(path); std::string text(std::istreambuf_iterator<char>{input}, {});
    const auto position = text.find("inst a of Async"); require(position != std::string::npos, "missing reset fixture instance");
    text.replace(position, std::string("inst a of Async").size(), "inst a of Sync");
    std::ofstream(path) << text;
    const auto conflict = ingest::readSources(request);
    require(!conflict.success() && !conflict.design &&
            diagnostics(conflict).find("both synchronous and asynchronous") != std::string::npos,
        "one external reset interface accepted conflicting parent reset types");
}
void hierarchicalWidths() {
    const auto path = artifact("ingest-mixed") / "hierarchical-widths.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt
    output y : UInt
    connect y, not(a)
  module middle :
    input a : UInt
    output y : UInt
    inst l of leaf
    connect l.a, a
    connect y, l.y
  public module top :
    input a : UInt<8>
    input b : UInt<12>
    output y : UInt<8>
    output z : UInt<12>
    inst m of middle
    inst wide of middle
    connect m.a, a
    connect y, m.y
    connect wide.a, b
    connect z, wide.y
)FIR";
    auto result = read(path); require(result.success(), diagnostics(result));
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    const auto& graph = *result.design->findGraph("top");
    Oracle oracle(graph);
    for (unsigned value = 0; value < 256; ++value) {
        const auto wide = (value * 17) & 4095;
        require(oracle.output("y", {{"a", value}, {"b", wide}}) == (value ^ 255), "hierarchical width inference truncated input");
        require(oracle.output("z", {{"a", value}, {"b", wide}}) == (wide ^ 4095), "width inference did not join all instances");
    }
}
void hierarchicalArrayDependencies() {
    const auto path = artifact("ingest-mixed") / "hierarchical-array-dependencies.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input enq : UInt<1>
    output ready : UInt<1>
    output seen : UInt<1>
    connect ready, UInt<1>(1)
    connect seen, enq
  public module top :
    output y : UInt<8>[2]
    output seen : UInt<1>
    wire w : UInt<8>[2]
    inst child of leaf
    connect child.enq, child.ready
    connect seen, child.seen
    connect w[0], UInt<8>(7)
    connect w[1], w[0]
    connect y, w
)FIR";
    auto result = read(path); require(result.success(), diagnostics(result));
    require(!result.design->findGraph("top")->metadata("firrtl.pendingCombLoopCheck"),
        "local array dependencies were not resolved before publication");
    ingest::IngestRequest request;
    request.sources = {{ingest::SourceLanguage::Firrtl, path, "fixture"}};
    request.tops = {"top"};
    explicitPrefix(request);
    tx::SessionStore session;
    pass(*result.design, session, "hier-flatten");
    pass(*result.design, session, "comb-loop-elim", {"-fail-on-true-loop"});
    Oracle oracle(*result.design->findGraph("top"));
    require(oracle.output("y", {}) == 0x0707 && oracle.output("seen", {}) == 1,
        "local range analysis corrupted hierarchical handshake or array dependencies");
}
void conditionalInstance() {
    const auto path = artifact("ingest-mixed") / "conditional-instance.fir";
    std::string source = R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt<8>
    output y : UInt<8>
    connect y, add(a, UInt<8>(3))
  public module top :
    input sel : UInt<1>
    input choose : UInt<1>
    input a : UInt<8>
    output y : UInt<8>
    when sel :
      inst child of leaf
      when choose : connect child.a, a
      else : connect child.a, UInt<8>(7)
      connect y, child.y
    else : connect y, UInt<8>(9)
)FIR";
    std::ofstream(path) << source;
    auto result = read(path); require(result.success(), diagnostics(result));
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    Oracle oracle(*result.design->findGraph("top"));
    for (unsigned sel = 0; sel < 2; ++sel) for (unsigned choose = 0; choose < 2; ++choose)
        for (unsigned a = 0; a < 256; ++a)
            require(oracle.output("y", {{"sel", sel}, {"choose", choose}, {"a", a}}) ==
                (sel ? (choose ? (a + 3) & 255 : 10) : 9), "conditional instance changed connection guards");
    const std::string otherwise = "      else : connect child.a, UInt<8>(7)\n";
    source.erase(source.find(otherwise), otherwise.size());
    std::ofstream(path) << source;
    auto missing = read(path);
    require(!missing.success() && !missing.design && diagnostics(missing).find("not fully initialized") != std::string::npos,
        "conditional instance accepted an input missing an inner branch");
}
void scalarizedArrayAbi(const std::filesystem::path& output) {
    const auto fir = output / "scalarized-abi.fir";
    std::ofstream(fir) << R"FIR(FIRRTL version 6.0.0
circuit top :
  extmodule Single :
    output r : { flip enable : UInt<1>, flip index : UInt<8>, data : UInt<8>, async : UInt<1>}[1]
    defname = single
  extmodule Nested :
    input io : { data : UInt<8>[2], flip reply : UInt<8>[2]}[2]
    input plain : UInt<8>[2][2]
    output plainY : UInt<8>[2][2]
    defname = nested
  public module top :
    input a : UInt<8>
    input b : UInt<8>
    output singleData : UInt<8>
    output singleAsync : UInt<1>
    output nestedData : UInt<32>
    output plainData : UInt<32>
    inst s of Single
    inst n of Nested
    connect s.r[0].enable, bits(b, 0, 0)
    connect s.r[0].index, a
    connect singleData, s.r[0].data
    connect singleAsync, s.r[0].async
    connect n.io[0].data[0], a
    connect n.io[0].data[1], b
    connect n.io[1].data[0], xor(a, UInt<8>(3))
    connect n.io[1].data[1], xor(b, UInt<8>(5))
    connect n.plain[0][0], b
    connect n.plain[0][1], a
    connect n.plain[1][0], xor(b, UInt<8>(7))
    connect n.plain[1][1], xor(a, UInt<8>(11))
    connect nestedData, cat(cat(n.io[1].reply[1], n.io[1].reply[0]), cat(n.io[0].reply[1], n.io[0].reply[0]))
    connect plainData, cat(cat(n.plainY[1][1], n.plainY[1][0]), cat(n.plainY[0][1], n.plainY[0][0]))
)FIR";
    const auto sv = output / "scalarized-abi.sv";
    const std::string implementation = R"SV(module single(input logic r_0_enable, input logic [7:0] r_0_index,
output logic [7:0] r_0_data, output logic r_0_async);
assign r_0_data = r_0_enable ? r_0_index ^ 8'ha5 : 8'hff;
assign r_0_async = !r_0_enable;
endmodule
module nested(input logic [7:0] io_0_data_0, io_0_data_1, io_1_data_0, io_1_data_1,
output logic [7:0] io_0_reply_0, io_0_reply_1, io_1_reply_0, io_1_reply_1,
input logic [7:0] plain_0_0, plain_0_1, plain_1_0, plain_1_1,
output logic [7:0] plainY_0_0, plainY_0_1, plainY_1_0, plainY_1_1);
assign io_0_reply_0 = io_1_data_1 ^ 8'h01;
assign io_0_reply_1 = io_1_data_0 ^ 8'h02;
assign io_1_reply_0 = io_0_data_1 ^ 8'h04;
assign io_1_reply_1 = io_0_data_0 ^ 8'h08;
assign plainY_0_0 = plain_1_1 ^ 8'h10;
assign plainY_0_1 = plain_1_0 ^ 8'h20;
assign plainY_1_0 = plain_0_1 ^ 8'h40;
assign plainY_1_1 = plain_0_0 ^ 8'h80;
endmodule
)SV";
    std::ofstream(sv) << implementation;
    ingest::IngestRequest request; request.logLevel = wolvrix::lib::LogLevel::Off;
    request.sources = {{ingest::SourceLanguage::Firrtl, fir, "fir"}, {ingest::SourceLanguage::SystemVerilog, sv, "sv"}};
    auto scanRequest = request; scanRequest.sources.pop_back();
    const auto scan = ingest::scanInterfaces(scanRequest);
    require(scan.success, "scalarized boundary scan failed");
    const auto& single = scan.modules[0].ports[0];
    require(single.shape == std::vector<int64_t>{1} && single.shapeFieldDepths == std::vector<std::size_t>{1},
        "single-element Vector lost its field depth");
    const auto& nested = scan.modules[1].ports[0];
    require(nested.shape == std::vector<int64_t>{2, 2} && nested.shapeFieldDepths == std::vector<std::size_t>{1, 2},
        "Vector/Bundle/Vector ordering was lost");
    require(scan.modules[1].ports[2].shapeFieldDepths == std::vector<std::size_t>{1, 1},
        "adjacent Vector dimensions acquired a field boundary");
    auto result = ingest::readSources(request); require(result.success(), diagnostics(result));
    unsigned adapters = 0;
    for (const auto& name : result.design->graphOrder()) {
        const auto& graph = *result.design->findGraph(name);
        if (!graph.metadata("ingest.link.kind")) continue;
        if (graph.inputPortValue("r__DOT__enable")) {
            require(std::ranges::equal(graph.valueShape(graph.inputPortValue("r__DOT__enable")), single.shape),
                "scalarized binding erased the singleton array shape");
            ++adapters;
        }
        if (graph.inputPortValue("io__DOT__data")) {
            require(std::ranges::equal(graph.valueShape(graph.inputPortValue("io__DOT__data")), nested.shape),
                "scalarized binding erased the multidimensional array shape");
            require(count(graph, OperationKind::kSliceArray) && count(graph, OperationKind::kArrayCreate),
                "scalarized boundary lacks array extraction/reassembly");
            ++adapters;
        }
    }
    require(adapters == 2, "scalarized implementations were not bound through shaped adapters");
    const auto reject = [&](std::string text, std::string_view message) {
        std::ofstream(sv) << text;
        auto bad = ingest::readSources(request);
        require(!bad.success() && !bad.design && diagnostics(bad).find(message) != std::string::npos,
            "invalid scalarized ABI: " + diagnostics(bad));
    };
    const auto replace = [&](std::string from, std::string to) {
        auto text = implementation;
        for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size()) text.replace(at, from.size(), to);
        return text;
    };
    reject(replace("io_1_reply_1", "missing_reply"), "wrong-direction implementation port");
    reject(replace("io_0_data_0", "io_data_0_0"), "wrong-direction implementation port");
    reject(replace("output logic [7:0] r_0_data", "output logic [6:0] r_0_data"), "incompatible boundary ABI");
    auto wrongDirection = replace("output logic r_0_async", "input logic r_0_async");
    const auto assignment = wrongDirection.find("assign r_0_async = !r_0_enable;");
    wrongDirection.erase(assignment, std::string("assign r_0_async = !r_0_enable;").size());
    reject(wrongDirection, "wrong-direction implementation port");
    reject(replace("input logic [7:0] io_0_data_0,", "input logic [31:0] io_data, input logic [7:0] io_0_data_0,"),
        "implementation port count differs");
    const auto collision = output / "scalarized-collision.fir";
    std::ofstream(collision) << R"FIR(FIRRTL version 6.0.0
circuit Collision :
  extmodule Collision :
    input a : UInt<8>[1]
    input a_0 : UInt<8>
    output y : UInt<8>
    defname = collision
)FIR";
    std::ofstream(sv) << "module collision(input logic [7:0] a_0, output logic [7:0] y); assign y=a_0; endmodule\n";
    auto collisionRequest = request; collisionRequest.sources.front().path = collision;
    auto badCollision = ingest::readSources(collisionRequest);
    require(!badCollision.success() && !badCollision.design && diagnostics(badCollision).find("duplicate or wrong-direction") != std::string::npos,
        "scalar array element and named port shared an implementation port");
    std::ofstream(sv) << implementation;
    tx::SessionStore session; pass(*result.design, session, "hier-flatten");
    generate(*result.design, output / "scalarized-abi", R"CPP(#include "model/grhsim_top.hpp"
#include <cassert>
int main(){GrhSIM_top m;m.init();for(unsigned a=0;a<256;++a)for(unsigned b:{0U,1U,3U,17U,128U,255U}){
m.a=a;m.b=b;m.eval();assert(m.singleData==((b&1)?(a^165):255));assert(m.singleAsync==!(b&1));
const unsigned nested=((a^8)<<24)|((b^4)<<16)|((a^3^2)<<8)|(b^5^1);
const unsigned plain=((b^128)<<24)|((a^64)<<16)|((b^7^32)<<8)|(a^11^16);
assert(m.nestedData==nested);assert(m.plainData==plain);}}
)CPP");
}
void parallelSvPublication(const std::filesystem::path& output) {
    const auto path = output / "parallel-order.sv";
    std::ofstream source(path);
    source << "module leaf(input [7:0] a,output [7:0] y,output unused);assign y=~a;assign unused=a[0];endmodule\n";
    for (unsigned i = 0; i < 16; ++i) {
        source << "module child" << i << "(input [7:0] a,output [7:0] y);\n";
        source << "wire [7:0] n0=a;\n";
        for (unsigned n = 1; n <= i + 1; ++n) source << "wire [7:0] n" << n << "=n" << n - 1 << "+1;\n";
        source << "leaf u(.a(n" << i + 1 << "),.y(y),.unused());endmodule\n";
    }
    source << "module top(input [7:0] a,output [127:0] y);\n";
    for (unsigned i = 0; i < 16; ++i) source << "child" << i << " u" << i << "(a,y[" << i * 8 << "+:8]);\n";
    source << "endmodule\n"; source.close();
    ingest::IngestRequest request; request.tops = {"top"}; request.logLevel = wolvrix::lib::LogLevel::Off;
    request.sources = {{ingest::SourceLanguage::SystemVerilog, path, "sv"}};
    const auto snapshot = [](const ingest::IngestDiagnostic& entry) {
        const ingest::SourcePosition empty;
        const auto& loc = entry.location ? *entry.location : empty;
        return std::tuple{entry.kind, entry.message, entry.context, entry.passName, entry.originSymbol,
                          entry.location.has_value(), loc.file.generic_string(), loc.line, loc.column,
                          loc.unit, loc.sourceLine, loc.sourceInfo};
    };
    std::optional<std::string> expectedJson;
    std::vector<decltype(snapshot(ingest::IngestDiagnostic{}))> expectedDiagnostics;
    for (unsigned run = 0; run < 3; ++run) {
        auto parsed = ingest::readSources(request); require(parsed.success(), diagnostics(parsed));
        require(parsed.design->graphs().size() == 18, "parallel SV publication dropped modules");
        require(!parsed.diagnostics.empty(), "parallel SV fixture did not exercise diagnostics");
        std::vector<decltype(snapshot(ingest::IngestDiagnostic{}))> actualDiagnostics;
        for (const auto& entry : parsed.diagnostics) actualDiagnostics.push_back(snapshot(entry));
        wolvrix::lib::store::StoreJson store;
        auto json = store.storeToString(*parsed.design); require(json.has_value(), "parallel SV graph store failed");
        if (!run) { expectedJson = *json; expectedDiagnostics = actualDiagnostics; }
        require(*json == *expectedJson && actualDiagnostics == expectedDiagnostics,
                "parallel SV completion order changed published graph or diagnostic identity");
    }
}
void test() {
    compactionEligibility();
    hierarchicalReset();
    externalReset();
    hierarchicalWidths();
    hierarchicalArrayDependencies();
    conditionalInstance();
    const auto output = artifact("ingest-mixed");
    parallelSvPublication(output);
    scalarizedArrayAbi(output);
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
    {
        auto queued = good;
        queued.firrtl.lowerThreads = 4;
        queued.logLevel = wolvrix::lib::LogLevel::Info;
        std::size_t batches = 0;
        queued.logSink = [&](const auto& event) {
            if (event.message.starts_with("parallel FIRRTL lowering threads=")) ++batches;
        };
        const auto result = ingest::readSources(queued);
        require(result.success(), diagnostics(result));
        require(batches >= 2, "SV-discovered FIRRTL module bypassed the lowering worker queue");
    }
    explicitPrefix(good);
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
        generate(hinted, output / (top + "-hints"), driver, {"-firrtl-nodes=prefer", "-cost-model=bytes", "-max-supernode-nodes=4"});
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
        auto repeated = ingest::readSources(request); require(repeated.success(), diagnostics(repeated));
        wolvrix::lib::store::StoreJson identityStore;
        const auto originalJson = identityStore.storeToString(*parsed.design);
        const auto repeatedJson = identityStore.storeToString(*repeated.design);
        require(originalJson && repeatedJson && *originalJson == *repeatedJson,
                "generated SV transaction paths changed serialized mixed graph identity");
        require(originalJson->find("transactions/") == std::string::npos,
                "published mixed graph retained a request-local source path");
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
    std::ofstream(output / "run.mk") << ".PHONY: run hierarchy cross-source top svtop top-hints svtop-hints parameters abi scalarized-abi inline path resource macro dpi\nrun: hierarchy cross-source top svtop top-hints svtop-hints parameters abi scalarized-abi inline path resource macro dpi\nhierarchy cross-source top svtop top-hints svtop-hints parameters abi scalarized-abi inline path resource macro dpi:\n\t$(MAKE) -C $@ -f run.mk run\n";
}
}
int main() { try { test(); return 0; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
