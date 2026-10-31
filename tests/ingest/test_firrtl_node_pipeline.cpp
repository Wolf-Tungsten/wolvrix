#include "firrtl_emit_fixture.hpp"
#include "../transform/grhsim_oracle.hpp"
#include "transform/grhsim/node-cse.hpp"

namespace {
using namespace firrtl_test;
namespace sim = tx::grhsim;

void execute(grh::Design& design, tx::SessionStore& session, std::string_view name,
             std::initializer_list<std::string_view> arguments = {}) {
    std::string error;
    auto implementation = tx::makePass(name, std::span(arguments.begin(), arguments.size()), error);
    require(bool(implementation), error);
    tx::PassManagerOptions options; options.session = &session;
    tx::PassManager manager(options); manager.addPass(std::move(implementation));
    tx::PassDiagnostics diagnostics;
    if (!manager.run(design, diagnostics).success) {
        std::string text; for (const auto& entry : diagnostics.messages()) text += entry.message + '\n';
        throw std::runtime_error(std::string(name) + ": " + text);
    }
    valid(design);
}
void ordinary(grh::Design& design, tx::SessionStore& session) {
    for (const auto name : {"xmr-resolve", "mux-tree-fold"}) execute(design, session, name);
    execute(design, session, "register-write-normalize", {"-semantics=2state"});
    execute(design, session, "mux-tree-fold", {"-guarded-writes=2state"});
    for (const auto name : {"memory-read-retime", "multidriven-guard", "blackbox-guard",
         "latch-transparent-read", "hier-flatten", "comb-loop-elim", "grh.verify"}) execute(design, session, name);
    execute(design, session, "simplify", {"-semantics=2state"});
    execute(design, session, "grh-to-grhsim");
    for (const auto name : {"grhsim-simplify", "grhsim-pack-bit-registers",
         "grhsim-simplify", "grhsim-recover-masked-or", "grhsim-simplify"}) execute(design, session, name);
}
void partition(grh::Design& design, tx::SessionStore& session) {
    for (const auto name : {"grhsim-split-event-and-output", "grhsim-split-nonsink-and-sink",
         "grhsim-partition-nonsink", "grhsim-simplify", "grhsim-partition-sink"})
        if (std::string_view(name) == "grhsim-partition-nonsink")
            execute(design, session, name, {"-firrtl-nodes=prefer", "-cost-model=bytes", "-fuse-leaves=1",
                "-max-node-work=2", "-max-supernode-work=4"});
        else execute(design, session, name);
}
std::string fixture() {
    std::string body = "    input clk : Clock\n    input en : UInt<1>\n    input a : UInt<8>\n    input b : UInt<8>\n    input c : UInt<8>\n";
    for (unsigned i = 0; i < 5; ++i) body += "    output y" + std::to_string(i) + " : UInt<8>\n";
    body += "    output q : UInt<8>\n    output flags : UInt<2>\n    reg r : UInt<8>, clk\n    reg f0 : UInt<1>, clk\n    reg f1 : UInt<1>, clk\n";
    for (unsigned i = 0; i < 5; ++i) body += "    node n" + std::to_string(i) + " = xor(and(a, b), c)\n";
    body += "    node alias = a\n    node folded = add(UInt<4>(1), UInt<4>(2))\n    node dead = or(a, c)\n    node bit0 = bits(a, 0, 0)\n    node bit1 = bits(a, 1, 1)\n    when en :\n      connect r, n0\n      connect f0, bit0\n      connect f1, bit1\n    connect q, r\n    connect flags, cat(f1, f0)\n";
    for (unsigned i = 0; i < 5; ++i) body += "    connect y" + std::to_string(i) + ", n" + std::to_string(i) + "\n";
    return body;
}
void realPipeline(std::string mode) {
    auto input = text("node-pipeline-" + mode, fixture());
    require(input.success(), diagnostics(input));
    auto& design = *input.design;
    const auto reference = design.clone();
    auto stripped = design.clone();
    const auto strip = [](grh::Design& current) {
        for (const auto& [name, graph] : current.graphs()) {
            for (const auto op : graph->operations()) {
                node::write(*graph, op, {}); graph->eraseAttr(op, "grhsim.nodeClone");
            }
            std::vector<std::string> keys;
            for (const auto& entry : graph->metadata()) if (entry.key.starts_with("firrtl.node")) keys.push_back(entry.key);
            for (const auto& key : keys) graph->eraseMetadata(key);
        }
    };
    strip(stripped); tx::SessionStore strippedSession; ordinary(stripped, strippedSession);
    tx::SessionStore session; ordinary(design, session);
    auto normalized = design.clone(); strip(normalized);
    require(wolvrix::lib::store::StoreJson{}.storeToString(normalized) ==
            wolvrix::lib::store::StoreJson{}.storeToString(stripped),
            "node hints changed the normal production simplify/packing structure");
    const auto before = count(*design.findGraph("top"), OperationKind::kXor);
    require(before == 1, "ordinary primitive CSE was blocked by source node hints");
    execute(design, session, "grhsim-node-cse", {mode == "off" ? "-firrtl-node-cse=off" : "-firrtl-node-cse=gsim"});
    require(count(*design.findGraph("top"), OperationKind::kXor) == 1, "optional whole-node pass changed an already shared expression");
    auto checkpoint = roundTrip(design);
    partition(checkpoint, session);
    const auto& graph = *checkpoint.findGraph("top");
    const auto& schedule = sim::requireSinkSchedule(&session, graph);
    grhsim_test::Oracle expected(*reference.findGraph("top")), actual(graph, {}, &schedule);
    for (unsigned step = 0; step < 256; ++step) {
        const std::map<std::string, uint64_t> values{{"clk", step % 2}, {"en", step % 5 != 0},
            {"a", step * 17 % 256}, {"b", step * 29 % 256}, {"c", step * 7 % 256}};
        const auto a = expected.eval(values), b = actual.eval(values);
        require(a.outputs == b.outputs && a.effects == b.effects, "node CSE/partition changed FIRRTL behavior");
    }
    node::recordRetention(*checkpoint.findGraph("top"));
    const auto metrics = std::get<std::vector<std::string>>(*graph.metadata("firrtl.nodeRetention"));
    require(metrics.front() == "v2", "checkpoint did not use soft hint retention");
    wolvrix::lib::store::StoreJson store;
    std::ofstream(artifact("firrtl-node-pipeline") / (mode + ".json")) << *store.storeToString(checkpoint);
}
void retentionFailures() {
    auto input = text("node-loss", "    input a : UInt<8>\n    output y : UInt<8>\n    node n = not(a)\n    connect y, n\n");
    require(input.success(), diagnostics(input));
    auto& graph = *input.design->findGraph("top");
    for (const auto op : graph.operations()) if (node::annotated(graph, op)) node::write(graph, op, {});
    node::recordRetention(graph);
    const auto stats = std::get<std::vector<std::string>>(*graph.metadata("firrtl.nodeRetention"));
    require(std::find(stats.begin(), stats.end(), "missing=0") == stats.end(), "hint loss was not diagnosed");
}
void hierarchyInputs() {
    const auto path = artifact("firrtl-node-pipeline") / "hierarchy.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module child :
    input a : UInt<257>
    output y : UInt<257>
    node n = not(a)
    connect y, n
  public module top :
    input left : UInt<257>
    input right : UInt<257>
    output l : UInt<257>
    output r : UInt<257>
    inst lhs of child
    inst rhs of child
    connect lhs.a, left
    connect rhs.a, right
    connect l, lhs.y
    connect r, rhs.y
)FIR";
    auto input = read(path); require(input.success(), diagnostics(input));
    tx::SessionStore session; ordinary(*input.design, session);
    execute(*input.design, session, "grhsim-node-cse");
    const auto& graph = *input.design->findGraph("top");
    require(count(graph, OperationKind::kNot) == 2, "hierarchy instances with distinct input ports merged");
    const auto inspection = node::inspect(graph);
    require(inspection.nodes.size() == 2, "flattened child node identity was lost");
    const auto left = node::parseKey(inspection.nodes[0].key), right = node::parseKey(inspection.nodes[1].key);
    require(left.module == right.module && left.name == right.name && left.instancePath != right.instancePath,
        "actual instance paths did not qualify child node keys");
    require(std::get<std::vector<std::string>>(*graph.metadata("firrtl.nodeInventory")).size() == 2,
        "flattened inventory differs from operation identities");
}
void stateMemoryEffects() {
    const auto fixtures = std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR);
    for (const auto& path : {fixtures / "state/rename.fir", fixtures / "memory/masked.fir", fixtures / "memory/multi-clock.fir", fixtures / "effects/ordered.fir"}) {
        auto input = read(path); require(input.success(), diagnostics(input));
        const auto reference = input.design->clone();
        tx::SessionStore session; ordinary(*input.design, session);
        execute(*input.design, session, "grhsim-node-cse");
        partition(*input.design, session);
        const auto& graph = *input.design->findGraph("top");
        const auto& schedule = sim::requireSinkSchedule(&session, graph);
        // String formatting/effect ordering uses the existing compiled effects
        // scoreboard; the bit-vector oracle deliberately has no string values.
        if (path.parent_path().filename() == "effects") {
            require(count(graph, sim::kSystemTask) == count(*reference.findGraph("top"), OperationKind::kSystemTask), "effect count changed");
            continue;
        }
        grhsim_test::Oracle expected(*reference.findGraph("top")), actual(graph, {}, &schedule);
        for (unsigned step = 0; step < 128; ++step) {
            std::map<std::string, uint64_t> values;
            unsigned ordinal = 1;
            for (const auto& port : graph.inputPorts()) {
                const bool clock = port.name == "clk" || port.name == "c0" || port.name == "c1";
                values[port.name] = clock ? (step / ordinal) % 2 : step * (17 + ordinal);
                ++ordinal;
            }
            const auto a = expected.eval(values), b = actual.eval(values);
            require(a.outputs == b.outputs && a.effects == b.effects, path.string() + " pipeline behavior differs");
        }
    }
}
}
int main() {
    try { realPipeline("off"); realPipeline("gsim"); retentionFailures(); hierarchyInputs(); stateMemoryEffects(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
