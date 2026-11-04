#include "core/decode_table.hpp"
#include "core/store.hpp"
#include "emit/grhsim_activity_model.hpp"
#include "emit/grhsim_cpp.hpp"
#include "emit/system_verilog.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/simplify.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace wolvrix::lib;
using namespace grh;
using namespace emit;
using namespace transform;
namespace
{
void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
std::string read(const std::filesystem::path &p)
{
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), {}};
}
template <class T> void slot(SessionStore &s, const char *name, T value)
{
    s.insert_or_assign(
        std::string("top.activity_schedule.") + name,
        std::make_unique<SessionSlotValue<T>>(std::move(value), "decode-test"));
}
void structure(const std::filesystem::path &root)
{
    for (int width : {1, 7, 16, 65, 129, 1024})
    {
        Design d;
        auto &g = d.createGraph("top");
        d.markAsTop(g.symbol());
        auto value = [&](const std::string &n, int w)
        { return g.createValue(g.internSymbol(n), w, false); };
        auto x = value("x", width);
        g.bindInputPort("x", x);
        auto unused = value("unused", width);
        g.bindInputPort("unused", unused);
        auto op = [&](OperationKind kind, const std::string &n,
                      std::vector<ValueId> args, int w)
        {
            auto id = g.createOperation(kind, g.internSymbol(n + "_op"));
            for (auto a : args)
                g.addOperand(id, a);
            auto v = value(n, w);
            g.addResult(id, v);
            if (kind == OperationKind::kDecodeTable)
                g.setAttr(id, "decode.table",
                          std::string("WDT1|P|") + std::to_string(width) +
                              "|8|8|3|ff|1,1,7,ff");
            return v;
        };
        int rw = 8;
        auto a = op(OperationKind::kDecodeTable, "_val_a", {x}, rw);
        auto b = op(OperationKind::kDecodeTable, "_val_b", {x}, rw);
        op(OperationKind::kDecodeTable, "_val_dead", {unused}, rw);
        auto zero = op(OperationKind::kConstant, "_val_zero", {}, width);
        g.setAttr(g.valueDef(zero), "constValue",
                  std::to_string(width) + "'h0");
        auto c = op(OperationKind::kDecodeTable, "_val_const", {zero}, rw);
        auto sum = op(OperationKind::kAdd, "sum", {a, b}, rw + 1);
        g.bindOutputPort("sum", sum);
        g.bindOutputPort("constant", c);
        g.setOpSrcLoc(g.valueDef(a), SrcLoc{"Encoder.scala", 12, 3, 12, 30,
                                            "utility", "", "DecodeTable"});
        store::StoreOptions so;
        so.outputFilename = (root / "roundtrip.json").string();
        check(store::StoreJson().store(d, so).success, "store failed");
        auto json = read(*so.outputFilename);
        check(json.find("kDecodeTable") != std::string::npos &&
                  json.find("Encoder.scala") != std::string::npos,
              "operation or provenance lost in serialization");
        auto restored = Design::fromJsonString(json);
        (void)restored;
        GrhSimActivitySeeds seeds;
        seeds.inputs[x] = {{0, unsigned(width), 1}};
        seeds.inputs[unused] = {{0, unsigned(width), 2}};
        GrhSimExecutionPlan plan;
        plan.graph = g.id();
        plan.inputs = {{x, {}}, {unused, {}}};
        auto activity =
            analyzeGrhSimActivity(g, plan, seeds, GrhSimActivityOptions{});
        check(activity.operationMasks[g.valueDef(a).index] == 1,
              "DecodeTable acquired unknown/unrelated activity");
        SimplifyOptions opts;
        opts.semantics = ConstantFoldOptions::Semantics::TwoState;
        PassManager pm;
        pm.addPass(std::make_unique<SimplifyPass>(opts));
        PassDiagnostics diag;
        check(pm.run(d, diag).success && !diag.hasError(), "simplify failed");
        unsigned counts = 0;
        for (auto id : g.operations())
            counts += g.getOperation(id).kind() == OperationKind::kDecodeTable;
        check(counts == 1, "CSE, constant folding or DCE failed");
        EmitOptions svOptions;
        svOptions.outputDir = (root / "unsupported_sv").string();
        EmitDiagnostics svDiag;
        check(!EmitSystemVerilog(&svDiag).emit(d, svOptions).success &&
                  svDiag.hasError(),
              "SV emitter silently dropped DecodeTable");
    }
}
void constantTables(const std::filesystem::path &root, bool fold = false) {
    const std::vector<std::pair<std::string, bool>> cases = {
        {"WDT1|P|4|6|6|0|3f|1,1,0,3f;2,2,0,3f", true},
        {"WDT1|T|4|6|6|7|3|1,1,3,3;2,2,3,3", true},
        {"WDT1|P|65|129|64,65|100000000000000000000000000000001|1ffffffffffffffffffffffffffffffff|1,1,100000000000000000000000000000001,1ffffffffffffffffffffffffffffffff", true},
        {"WDT1|T|3|4|4|0|f|", true},
        // A default or unmatched-care difference must retain a real dependency.
        {"WDT1|P|4|6|6|1|3f|1,1,0,3f", false},
        {"WDT1|T|3|4|4|0|f|1,1,1,1;2,2,4,4", false},
        {"WDT1|P|3|4|4|3|f|1,1,3,1", false}
    };
    unsigned index = 0;
    for (const auto &[payload, expectedConstant] : cases) {
        const auto table = decode::parse(payload);
        const auto expected = decode::constantOutput(table);
        check(expected.has_value() == expectedConstant, "unsound uniform-table proof");
        Design design;
        auto &g = design.createGraph("top");
        design.markAsTop(g.symbol());
        auto input = g.createValue(g.internSymbol("input"), table.input, false);
        auto output = g.createValue(g.internSymbol("output"), table.output, false);
        g.bindInputPort("input", input);
        g.bindOutputPort("output", output);
        auto id = g.createOperation(OperationKind::kDecodeTable);
        g.addOperand(id, input);
        g.addResult(id, output);
        g.setAttr(id, "decode.table", payload);
        g.setOpSrcLoc(id, SrcLoc{"ConstantTable.scala", 20, 4, 20, 30, "utility", "", "table"});
        SimplifyOptions options;
        options.semantics = ConstantFoldOptions::Semantics::TwoState;
        PassManager pm;
        pm.addPass(std::make_unique<SimplifyPass>(options));
        PassDiagnostics diagnostics;
        check(pm.run(design, diagnostics).success && !diagnostics.hasError(), "table simplify failed");
        unsigned remaining = 0;
        for (auto op : g.operations()) {
            remaining += g.getOperation(op).kind() == OperationKind::kDecodeTable;
        }
        check(remaining == (fold && expectedConstant ? 0u : 1u), "incorrect research folding gate");
        const auto outputOp = g.getOperation(g.valueDef(g.outputPorts()[0].value));
        if (!fold || !expectedConstant) {
            check(outputOp.kind() == OperationKind::kDecodeTable &&
                  outputOp.operands().size() == 1 && outputOp.operands()[0] == input,
                  "dynamic table input dependency was lost");
        } else {
            check(outputOp.kind() == OperationKind::kConstant, "uniform table was not constant");
        }
        store::StoreOptions storeOptions;
        storeOptions.outputFilename = (root / ("constant-table-" + std::to_string(index++) + ".json")).string();
        check(store::StoreJson().store(design, storeOptions).success, "constant table store failed");
    }
}

void booleanTables(const std::filesystem::path &root) {
    // Cover register bitsets, the 64-entry boundary, and sparse multiword input.
    const std::vector<std::string> payloads = {
        "WDT1|P|1|1|1|0|1|1,1,1,1",
        "WDT1|T|5|1|1|0|1|1f,0,1,1;1f,4,1,1;1f,5,1,1;1f,6,1,1;1f,c,1,1;1f,d,1,1;1f,e,1,1;1f,f,1,1;1f,10,1,1;1f,14,1,1;1f,15,1,1",
        "WDT1|P|6|1|1|0|1|3f,3f,1,1",
        "WDT1|T|65|1|1|0|1|10000000000000001,10000000000000001,1,1"
    };
    std::ofstream source(root / "boolean-tables.cpp");
    source << "#include <array>\n#include <cstdint>\n#include <cassert>\n";
    std::string checks;
    for (const auto &payload : payloads) {
        auto table = decode::parse(payload);
        const auto symbol = decode::name(payload);
        auto stats = decode::emit(source, table, symbol);
        check(stats.bitset && stats.storage == 8, "small Boolean table not a register bitset");
        const unsigned count = table.input <= 6 ? (1u << table.input) : 4u;
        for (unsigned x = 0; x < count; ++x) {
            decode::Words input((table.input + 63) / 64, 0);
            input[0] = table.input <= 6 ? x : x & 1;
            if (table.input > 64) input[1] = x >> 1;
            checks += "assert(" + symbol + "(" + decode::literal(input, table.input) + ") == " +
                      std::to_string(decode::evaluate(table, input)[0]) + ");\n";
        }
    }
    source << "int main(){\n" << checks << "}\n";
    source.close();
    const auto file = (root / "boolean-tables").string();
    check(file.find('\'') == std::string::npos, "test path contains quote");
    check(std::system(("c++ -std=c++20 -O2 '" + file + ".cpp' -o '" + file + "' && '" + file + "'").c_str()) == 0,
          "Boolean table evaluation failed");
}
void runtime(const std::filesystem::path &root)
{
    Design d;
    auto &g = d.createGraph("top");
    d.markAsTop(g.symbol());
    ActivityScheduleSupernodeToOps nodes(3);
    auto val = [&](const std::string &n, int w)
    { return g.createValue(g.internSymbol(n), w, false); };
    auto x = val("x", 65), other = val("other", 1);
    g.bindInputPort("x", x);
    g.bindInputPort("other", other);
    auto op = [&](unsigned node, OperationKind kind, const std::string &n,
                  std::vector<ValueId> args, int w)
    {
        auto id = g.createOperation(kind, g.internSymbol(n + "_op"));
        for (auto a : args)
            g.addOperand(id, a);
        auto v = val(n, w);
        g.addResult(id, v);
        nodes[node].push_back(id);
        if (kind == OperationKind::kDecodeTable)
            g.setAttr(id, "decode.table",
                      std::string("WDT1|P|65|8|8|3|ff|1,1,7,ff"));
        return v;
    };
    auto count = op(0, OperationKind::kDecodeTable, "count", {x}, 8);
    auto one = op(1, OperationKind::kConstant, "one", {}, 8);
    g.setAttr(g.valueDef(one), "constValue", std::string("8'd1"));
    auto sum = op(1, OperationKind::kAdd, "sum", {count, one}, 8);
    g.bindOutputPort("sum", sum);
    auto inverse = op(2, OperationKind::kNot, "inverse", {other}, 1);
    g.bindOutputPort("inverse", inverse);
    ActivityScheduleValueFanout fanout(g.values().size());
    fanout[x.index - 1] = {0};
    fanout[count.index - 1] = {1};
    fanout[other.index - 1] = {2};
    SessionStore session;
    slot(session, "supernode_to_ops", nodes);
    slot(session, "value_fanout", fanout);
    slot(session, "topo_order", ActivityScheduleTopoOrder{0, 1, 2});
    slot(session, "state_read_supernodes",
         ActivityScheduleStateReadSupernodes{});
    EmitOptions opts;
    opts.outputDir = (root / "runtime").string();
    opts.session = &session;
    opts.sessionPathPrefix = "top";
    opts.attributes["waveform"] = "off";
    opts.attributes["perf"] = "off";
    EmitDiagnostics diag;
    check(EmitGrhSimCpp(&diag).emit(d, opts).success && !diag.hasError(),
          "runtime emit failed");
    const auto dir = std::filesystem::path(*opts.outputDir);
    unsigned instrumented = 0;
    for (const auto &f : std::filesystem::directory_iterator(dir))
    {
        if (f.path().extension() != ".cpp")
            continue;
        auto s = read(f.path());
        bool edited = false;
        for (unsigned n = 0; n < 3; ++n)
        {
            auto pos = s.find("// Supernode " + std::to_string(n) + ":");
            if (pos == std::string::npos)
                continue;
            auto brace = s.find('{', pos);
            check(brace != std::string::npos, "missing dispatch body");
            s.insert(brace + 1,
                     "\n ++decode_test_visits[" + std::to_string(n) + "];\n");
            edited = true;
            ++instrumented;
        }
        if (edited)
        {
            std::ofstream out(f.path());
            out << "extern unsigned decode_test_visits[3];\n" << s;
        }
    }
    check(instrumented == 3, "incomplete diagnostic hooks");
    std::ofstream h(dir / "harness.cpp");
    h << R"CPP(#include "grhsim_top.hpp"
#include <cassert>
unsigned decode_test_visits[3]={};
void reset(){for(auto &v:decode_test_visits)v=0;}
int main(){GrhSIM_top m;m.x={1,0};m.eval();assert(m.sum==8);reset();
m.eval();assert(!decode_test_visits[0]&&!decode_test_visits[1]&&!decode_test_visits[2]);
m.other=1;m.eval();assert(!decode_test_visits[0]&&!decode_test_visits[1]&&decode_test_visits[2]);reset();
m.x[1]=1;m.eval();assert(m.sum==8&&decode_test_visits[0]&&!decode_test_visits[1]);reset();
m.x[0]=0;m.eval();assert(m.sum==4&&decode_test_visits[0]&&decode_test_visits[1]);reset();
m.x[1]=0;m.eval();assert(m.sum==4&&decode_test_visits[0]&&!decode_test_visits[1]);}
)CPP";
    h.close();
    auto path = dir.string();
    check(path.find('\'') == std::string::npos, "test path contains quote");
    std::string cmd = "c++ -std=c++20 -O2 -I '" + path + "' '" + path +
                      "'/*.cpp -o '" + path + "/check' && '" + path + "/check'";
    check(std::system(cmd.c_str()) == 0, "activity runtime check failed");
}
} // namespace
int main()
{
    try
    {
        auto root = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                    "grhsim_decode_table";
        std::filesystem::create_directories(root);
        structure(root);
        constantTables(root);
        setenv("WOLVRIX_RESEARCH_FOLD_CONSTANT_DECODE", "1", 1);
        constantTables(root, true);
        unsetenv("WOLVRIX_RESEARCH_FOLD_CONSTANT_DECODE");
        constantTables(root); // The opt-in must not leak into a later generation.
        booleanTables(root);
        runtime(root);
        std::cout << "PASS DecodeTable serialization, provenance, CSE, DCE, "
                     "folding, SSA boundary and activity\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
