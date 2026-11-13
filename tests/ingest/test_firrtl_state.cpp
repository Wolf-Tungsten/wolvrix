#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "data/firrtl/state/workload.hpp"

namespace {
using namespace firrtl_test;
using firrtl_state_test::Values;

const auto fixtures = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR) / "state";
ingest::IngestResult source(std::string_view name, ingest::FirrtlInitPolicy policy = ingest::FirrtlInitPolicy::Zero) {
    ingest::IngestRequest request;
    request.sources.push_back({ingest::SourceLanguage::Firrtl, fixtures / (std::string(name) + ".fir"), "state"});
    request.firrtl.invalidPolicy = ingest::FirrtlInvalidPolicy::Zero; request.firrtl.initPolicy = policy;
    request.firrtl.nodeAnnotations = false;
    return ingest::readSources(request);
}
void compare(const grh::Design& candidate, const grh::Design& reference, bool rename) {
    grhsim_test::Oracle fir(*candidate.findGraph("top")), sv(*reference.findGraph("top"));
    std::size_t evaluations = 0;
    const auto check = [&](const Values& inputs, const Values& expected) {
        const auto a = fir.eval(inputs), b = sv.eval(inputs); ++evaluations;
        if (a.outputs != b.outputs) {
            for (const auto& [name, value] : a.outputs) std::cerr << name << " FIRRTL=" << value << " SV=" << b.outputs.at(name) << '\n';
        }
        require(a.outputs == b.outputs, "FIRRTL/SV outputs differ at evaluation " + std::to_string(evaluations));
        for (const auto& [name, value] : expected) {
            const auto actual = slang::SVInt::fromString(a.outputs.at(name)).as<uint64_t>();
            require(actual && *actual == value, name + " differs from independent scoreboard at evaluation " + std::to_string(evaluations));
        }
    };
    if (rename) firrtl_state_test::renameTrace(check); else firrtl_state_test::priorityTrace(check);
}
void constantStateIndexBounds() {
    auto result = unannotatedText("constant-state-index-bounds", R"FIR(    input clk : Clock
    input i : UInt<2>
    output exactOut : UInt<4>
    output rowOut : UInt<4>[2]
    output selectedOut : UInt<4>
    output missingOut : UInt<4>
    reg r : UInt<4>[2][2], clk
    connect r[0][0], UInt<4>(3)
    connect r[0][1], UInt<4>(4)
    connect r[1][0], UInt<4>(5)
    connect r[1][1], UInt<4>(6)
    connect r[UInt<2>(2)][i], UInt<4>(15)
    node exact = r[0][0]
    node row = r[1]
    node selected = r[UInt<2>(0)][i]
    node missing = r[UInt<2>(2)][i]
    connect exactOut, exact
    connect rowOut, row
    connect selectedOut, selected
    connect missingOut, missing
)FIR");
    require(result.success(), diagnostics(result));
    auto& graph = *result.design->findGraph("top");
    require(count(graph, OperationKind::kLt) == 1, "constant state indices created runtime bounds checks");
    require(count(graph, OperationKind::kRegisterWriteArrayPort) == 4,
        "constant out-of-bounds state write was materialized");
    for (const auto op : graph.operations())
        if (graph.opKind(op) == OperationKind::kRegisterReadArrayPort || grh::isRegisterWritePortKind(graph.opKind(op)))
            require(!graph.getOperation(op).attr("firrtl.node.members"), "indexed state access entered node scope");
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); node::requireValid(graph);
    auto restored = roundTrip(*result.design);
    for (const auto* candidate : {&graph, restored.findGraph("top")}) {
        grhsim_test::Oracle oracle(*candidate);
        for (unsigned i = 0; i < 4; ++i) {
            oracle.eval({{"clk", 0}, {"i", i}});
            const auto evaluated = oracle.eval({{"clk", 1}, {"i", i}});
            const auto output = [&](std::string_view name) {
                return slang::SVInt::fromString(evaluated.outputs.at(std::string(name))).as<uint64_t>();
            };
            require(output("exactOut") == 3 && output("rowOut") == 0x65 &&
                output("selectedOut") == (i < 2 ? 3 + i : 0) && output("missingOut") == 0,
                "constant/mixed state indices changed old-state reads or no-op writes");
        }
    }
}
void typesAndPolicies() {
    error("state-clock", "    input clk : UInt<1>\n    reg r : UInt<4>, clk\n", "Clock type");
    error("state-reset", "    input clk : Clock\n    input rst : UInt<2>\n    regreset r : UInt<4>, clk, rst, UInt<4>(0)\n", "one-bit");
    error("state-reset-kind", "    input clk : Clock\n    input rst : SInt<1>\n    regreset r : UInt<4>, clk, rst, UInt<4>(0)\n", "register reset must");
    error("state-reset-init", "    input clk : Clock\n    input rst : UInt<1>\n    regreset r : UInt<4>, clk, rst, SInt<4>(0)\n", "non-equivalent");
    error("state-async-nonconst", "    input clk : Clock\n    input rst : AsyncReset\n    input data : UInt<4>\n    regreset r : UInt<4>, clk, rst, data\n", "const type");
    error("state-flipped", "    input clk : Clock\n    reg r : { flip x : UInt<4> }, clk\n", "storable");
    error("state-const", "    input clk : Clock\n    reg r : const UInt<4>, clk\n", "storable");
    error("state-clock-data", "    input clk : Clock\n    reg r : Clock, clk\n", "storable");
    auto unknown = source("rename", ingest::FirrtlInitPolicy::Unknown);
    require(unknown.success(), diagnostics(unknown));
    const auto& graph = *unknown.design->findGraph("top");
    const auto reg = graph.findOperation("table");
    require(grh::stateLogicalType(graph, reg).shape == std::vector<int64_t>{4}, "register shape lost");
    require(grh::stateLogicalType(graph, reg).domain == grh::LogicDomain::FourState, "state domain silently projected");
    require(grh::stateInitSpec(graph, reg).at(0).value == "32'bx", "default state init is not indeterminate");
    auto requestDefault = unannotatedText("state-default", "    input clk : Clock\n    regreset r : UInt<4>, clk, UInt<1>(0), UInt<4>(7)\n");
    require(requestDefault.success(), diagnostics(requestDefault));
    auto& defaultGraph = *requestDefault.design->findGraph("top");
    require(grh::stateInitSpec(defaultGraph, defaultGraph.findOperation("r")).at(0).value == "4'bx", "regreset value used as initial value");
    auto inferred = unannotatedText("state-inferred", "    input clk : Clock\n    input arst : AsyncReset\n    output y : UInt<7>\n    wire rst : Reset\n    connect rst, arst\n    regreset r : UInt, clk, rst, UInt<7>(65)\n    connect y, r\n");
    require(inferred.success(), diagnostics(inferred));
    auto& inf = *inferred.design->findGraph("top");
    require(grh::stateLogicalType(inf, inf.findOperation("r")).width == 7, "reset value did not constrain register width");
    bool async = false;
    for (const auto op : inf.operations()) if (grh::isRegisterWritePortKind(inf.opKind(op)))
        async |= std::get<std::vector<std::string>>(*inf.getOperation(op).attr("eventEdge")).size() == 2;
    require(async, "abstract reset inference did not reach writer events");
    auto scoped = unannotatedText("state-scoped", R"FIR(    input clk : Clock
    input sel : UInt<1>
    when sel :
      reg r : UInt<4>, clk
      wire w : UInt<4>
      connect w, UInt<4>(3)
      connect r, w
      when not(sel) :
        connect r, UInt<4>(9)
)FIR");
    require(scoped.success(), diagnostics(scoped));
    auto& sc = *scoped.design->findGraph("top");
    grhsim_test::Oracle scope(sc, {"r"});
    scope.eval({{"clk", 0}, {"sel", 0}});
    auto observation = scope.eval({{"clk", 1}, {"sel", 0}});
    require(slang::SVInt::fromString(observation.states.at("r")).as<uint64_t>() == 9, "enclosing declaration condition incorrectly gates state");
    auto self = unannotatedText("state-self-reset", "    input clk : Clock\n    input rst : UInt<1>\n    output y : UInt<4>\n    regreset r : UInt<4>, clk, rst, r\n    connect r, UInt<4>(5)\n    connect y, r\n");
    require(self.success(), diagnostics(self));
    run(*self.design, std::make_unique<tx::GrhVerifyPass>());
    grhsim_test::Oracle hold(*self.design->findGraph("top"));
    hold.eval({{"clk", 1}, {"rst", 0}}); hold.eval({{"clk", 0}, {"rst", 1}});
    require(slang::SVInt::fromString(hold.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>() == 5, "self reset did not sample old state");
    auto signedState = unannotatedText("state-signed", R"FIR(    input clk : Clock
    input en : UInt<1>
    input data : SInt<3>
    output y : SInt<6>
    reg r : SInt<6>, clk
    when en :
      connect r, data
    connect y, r
)FIR");
    require(signedState.success(), diagnostics(signedState));
    grhsim_test::Oracle signedOracle(*signedState.design->findGraph("top"));
    signedOracle.eval({{"clk", 0}, {"en", 1}, {"data", 7}});
    require(slang::SVInt::fromString(signedOracle.eval({{"clk", 1}}).outputs.at("y")).getRawPtr()[0] == 63, "signed state write lost sign extension");
    const std::string invalidBody = R"FIR(    input clk : Clock
    input en : UInt<1>
    output y : UInt<4>
    reg r : UInt<4>, clk
    connect r, UInt<4>(7)
    when en :
      invalidate r
    connect y, r
)FIR";
    for (const auto policy : {ingest::FirrtlInvalidPolicy::Unknown, ingest::FirrtlInvalidPolicy::Zero}) {
        auto inv = unannotatedText("state-invalid", invalidBody, policy); require(inv.success(), diagnostics(inv));
        auto& graph = *inv.design->findGraph("top"); bool found = false;
        for (const auto op : graph.operations()) if (graph.opKind(op) == OperationKind::kConstant)
            found |= std::get<std::string>(*graph.getOperation(op).attr("constValue")) == (policy == ingest::FirrtlInvalidPolicy::Unknown ? "4'bx" : "4'b0");
        require(found, "state invalid policy was ignored");
        run(*inv.design, std::make_unique<tx::GrhVerifyPass>());
        grhsim_test::Oracle oracle(graph);
        oracle.eval({{"clk", 1}, {"en", 0}}); oracle.eval({{"clk", 0}, {"en", 1}});
        require(slang::SVInt::fromString(oracle.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>() == 0, "invalid write did not override earlier data");
    }
    auto empty = unannotatedText("state-empty", "    input clk : Clock\n    reg r : UInt<0>[3], clk\n    reg e : UInt<4>[0], clk\n");
    require(empty.success(), diagnostics(empty));
    require(count(*empty.design->findGraph("top"), OperationKind::kRegister) == 0, "zero-width state materialized");
    auto asyncConst = unannotatedText("state-const-reset-input", "    input clk : Clock\n    input rst : AsyncReset\n    input data : const UInt<4>\n    regreset r : UInt<4>, clk, rst, data\n");
    require(asyncConst.success(), diagnostics(asyncConst));
    auto aggregateReset = unannotatedText("state-constant-wire-reset", R"FIR(    input clk : Clock
    input arst : AsyncReset
    output y : UInt<8>[2]
    wire value : UInt<8>[2]
    connect value[0], UInt<8>(7)
    connect value[1], UInt<8>(11)
    regreset r : UInt<8>[2], clk, arst, value
    connect r, r
    connect y, r
)FIR");
    require(aggregateReset.success(), diagnostics(aggregateReset));
    grhsim_test::Oracle aggregate(*aggregateReset.design->findGraph("top"));
    aggregate.eval({{"clk", 0}, {"arst", 0}});
    require(slang::SVInt::fromString(aggregate.eval({{"arst", 1}}).outputs.at("y")).as<uint64_t>() == 0x0b07,
        "aggregate constant wire reset did not fire asynchronously");
    auto invalidReset = unannotatedText("state-invalid-wire-reset", R"FIR(    input clk : Clock
    input arst : AsyncReset
    output y : {valid : UInt<1>, data : UInt<8>}[2]
    wire value : {valid : UInt<1>, data : UInt<8>}[2]
    connect value[0].valid, UInt<1>(0)
    invalidate value[0].data
    connect value[1].valid, UInt<1>(0)
    invalidate value[1].data
    regreset r : {valid : UInt<1>, data : UInt<8>}[2], clk, arst, value
    connect r[0].valid, UInt<1>(1)
    connect r[0].data, UInt<8>(7)
    connect r[1].valid, UInt<1>(1)
    connect r[1].data, UInt<8>(11)
    connect y, r
)FIR", ingest::FirrtlInvalidPolicy::Unknown);
    require(invalidReset.success(), diagnostics(invalidReset));
    grhsim_test::Oracle invalidOracle(*invalidReset.design->findGraph("top"), {}, nullptr, true);
    invalidOracle.eval({{"clk", 0}, {"arst", 0}});
    const auto definedOutput = invalidOracle.eval({{"clk", 1}}).outputs;
    require(slang::SVInt::fromString(definedOutput.at("y__DOT__data")).as<uint64_t>() == 0x0b07,
        "invalid reset fixture did not first write defined data");
    const auto invalidOutput = invalidOracle.eval({{"arst", 1}}).outputs;
    require(slang::SVInt::fromString(invalidOutput.at("y__DOT__valid")).as<uint64_t>() == 0 &&
            slang::SVInt::fromString(invalidOutput.at("y__DOT__data")).hasUnknown(),
        "invalid constant async reset lost defined fields or unknown data");
    auto fieldReset = unannotatedText("state-constant-field-reset", R"FIR(    input clk : Clock
    input arst : AsyncReset
    input d : UInt<8>
    output y : UInt<8>
    wire value : {c : UInt<8>, variable : UInt<8>}[2]
    connect value[0].c, UInt<8>(7)
    connect value[0].variable, d
    connect value[1].c, d
    connect value[1].variable, d
    wire copied : {c : UInt<8>, variable : UInt<8>}
    connect copied, value[0]
    regreset r : UInt<8>, clk, arst, copied.c
    connect r, d
    connect y, r
)FIR");
    require(fieldReset.success(), diagnostics(fieldReset));
    grhsim_test::Oracle fieldOracle(*fieldReset.design->findGraph("top"));
    fieldOracle.eval({{"clk", 0}, {"arst", 0}, {"d", 99}});
    require(slang::SVInt::fromString(fieldOracle.eval({{"clk", 1}}).outputs.at("y")).as<uint64_t>() == 99,
        "constant field reset fixture did not write runtime data");
    require(slang::SVInt::fromString(fieldOracle.eval({{"arst", 1}, {"d", 42}}).outputs.at("y")).as<uint64_t>() == 7,
        "unrelated runtime bundle fields or array elements changed constant asynchronous reset");
    error("state-variable-element-reset", "    input clk : Clock\n    input arst : AsyncReset\n    input d : UInt<8>\n    wire value : UInt<8>[2]\n    connect value[0], UInt<8>(7)\n    connect value[1], d\n    regreset r : UInt<8>, clk, arst, value[1]\n", "const type");
    error("state-variable-field-reset", "    input clk : Clock\n    input arst : AsyncReset\n    input d : UInt<8>\n    wire value : {c : UInt<8>, variable : UInt<8>}\n    connect value.c, UInt<8>(7)\n    connect value.variable, d\n    regreset r : UInt<8>, clk, arst, value.variable\n", "const type");
    error("state-variable-wire-reset", "    input clk : Clock\n    input arst : AsyncReset\n    input d : UInt<8>\n    wire value : UInt<8>\n    connect value, d\n    regreset r : UInt<8>, clk, arst, value\n", "const type");
}

std::string driver(bool rename, bool reference = false) {
    std::string text = "#include \"model/grhsim_top.hpp\"\n#include \"" + (fixtures / "workload.hpp").string() + "\"\nint main() { GrhSIM_top model; for(unsigned epoch=0;epoch<3;++epoch) { model.init();\n";
    text += rename ? "firrtl_state_test::renameTrace" : "firrtl_state_test::priorityTrace";
    text += R"CPP(([&](const auto& in, const auto& expected) {
        model.clk=in.at("clk");
)CPP";
    if (rename) text += R"CPP(
        model.reset=in.at("reset"); model.en0=in.at("en0"); model.en1=in.at("en1"); model.copy=in.at("copy");
        model.addr0=in.at("addr0"); model.addr1=in.at("addr1"); model.data0=in.at("data0"); model.data1=in.at("data1");
        for(unsigned i=0;i<4;++i) model.initialValue[i]=(in.at("initialValue")>>(i*8))&255;
)CPP";
    if (rename) text += reference ? "model.eval(); const uint64_t packed=model.tableOut;\n" : "model.eval(); uint64_t packed=0; for(unsigned i=0;i<4;++i) packed |= uint64_t(model.tableOut[i])<<(i*8);\n";
    if (rename) text += R"CPP(
        if (packed!=expected.at("tableOut") || model.readOut!=expected.at("readOut")) throw std::runtime_error("compiled rename scoreboard mismatch");
)CPP";
    else text += R"CPP(
        model.arst=in.at("arst"); model.srst=in.at("srst"); model.resetData=in.at("resetData"); model.value=in.at("value");
        model.enable=in.at("enable"); model.whole=in.at("whole"); model.row=in.at("row"); model.last=in.at("last"); model.overwrite=in.at("overwrite"); model.i=in.at("i"); model.j=in.at("j");
        for(unsigned i=0;i<2;++i) { model.source__DOT__flag[i]=(in.at("source__DOT__flag")>>i)&1;
            for(unsigned j=0;j<2;++j) model.source__DOT__data[i][j]=(in.at("source__DOT__data")>>((i*2+j)*4))&15; }
        model.eval();
)CPP";
    if (!rename && reference) text += "const uint64_t data=model.result__DOT__data, flags=model.result__DOT__flag;\n";
    if (!rename && !reference) text += R"CPP(
        uint64_t data=0,flags=0;
        for(unsigned i=0;i<2;++i) { flags |= uint64_t(model.result__DOT__flag[i])<<i;
            for(unsigned j=0;j<2;++j) data |= uint64_t(model.result__DOT__data[i][j])<<((i*2+j)*4); }
)CPP";
    if (!rename) text += R"CPP(
        if(model.asyncOut!=expected.at("asyncOut") || model.syncOut!=expected.at("syncOut") || model.nextOut!=expected.at("nextOut") || data!=expected.at("result__DOT__data") || flags!=expected.at("result__DOT__flag"))
            throw std::runtime_error("compiled priority scoreboard mismatch");
)CPP";
    return text + "}); }}\n";
}
void test() {
    typesAndPolicies();
    constantStateIndexBounds();
    const auto output = artifact("firrtl-state");
    for (const std::string name : {"rename", "priority"}) {
        auto result = source(name); require(result.success(), diagnostics(result));
        auto& graph = *result.design->findGraph("top");
        run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
        for (const auto op : graph.operations()) if (graph.opKind(op) == OperationKind::kRegister)
            require(grh::stateInitSpec(graph, op).at(0).value.ends_with("'b0"), "explicit zero init ignored");
        if (name == "rename") {
            require(count(graph, OperationKind::kRegister) == 1 && count(graph, OperationKind::kRegisterWriteArrayPort) == 2 && count(graph, OperationKind::kRegisterWritePort) == 1, "RenameTable writers expanded into full-array commits");
            require(count(graph, OperationKind::kShl) == 0 && count(graph, OperationKind::kMul) == 0, "indexed state writes converted to flat bit offsets");
        }
        for (const auto op : graph.operations())
            if (graph.opKind(op) == OperationKind::kRegister || graph.opKind(op) == OperationKind::kRegisterReadPort || graph.opKind(op) == OperationKind::kRegisterReadArrayPort || grh::isRegisterWritePortKind(graph.opKind(op)))
                require(!graph.getOperation(op).attr("firrtl.node.members"), "state/event operation entered node scope");
        ingest::IngestRequest request; request.sources.push_back({ingest::SourceLanguage::SystemVerilog, fixtures / (name + ".sv"), "reference"}); request.tops = {"top"};
        auto reference = ingest::readSources(request); require(reference.success(), diagnostics(reference));
        for (const auto& port : reference.design->findGraph("top")->outputPorts())
            require(bool(reference.design->findGraph("top")->valueDef(port.value)), "SV reference has an undriven output");
        wolvrix::lib::store::StoreJson store;
        std::ofstream(output / (name + "-firrtl.json")) << *store.storeToString(*result.design);
        std::ofstream(output / (name + "-sv.json")) << *store.storeToString(*reference.design);
        compare(*result.design, *reference.design, name == "rename");
        auto stored = roundTrip(*result.design);
        compare(stored, *reference.design, name == "rename");
        run(stored, std::make_unique<tx::SimplifyPass>()); valid(stored);
        compare(stored, *reference.design, name == "rename");
        generate(stored, output / name, driver(name == "rename"));
        generate(*reference.design, output / (name + "-sv"), driver(name == "rename", true));
    }
    std::ofstream(output / "run.mk") << ".PHONY: run rename priority rename-sv priority-sv\nrun: rename priority rename-sv priority-sv\nrename priority rename-sv priority-sv:\n\t$(MAKE) -C $@ -f run.mk run\n";
}
}
int main() {
    try { test(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
