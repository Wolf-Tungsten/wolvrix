#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::emit;

namespace {
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::string quote(const std::string &text) {
    std::string result = "'";
    for (char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}
template<class T> const T &slot(const SessionStore &session, const std::string &name) {
    return dynamic_cast<const SessionSlotValue<T> &>(*session.at("top.activity_schedule." + name)).value;
}

Design fixture(bool computeEffect = false, bool sharedChain = false, bool memory = false) {
    Design design;
    auto &g = design.createGraph("top");
    design.markAsTop(g.symbol());
    auto value = [&](const std::string &name, int width) {
        return g.createValue(g.internSymbol(name), width, false, ValueType::Logic);
    };
    auto input = [&](const std::string &name, int width) {
        auto v = value(name, width); g.bindInputPort(name, v); return v;
    };
    auto op = [&](OperationKind kind, const std::string &name, int width, std::vector<ValueId> args) {
        auto id = g.createOperation(kind, g.internSymbol(name + "_op"));
        for (auto v : args) g.addOperand(id, v);
        auto v = value(name, width); g.addResult(id, v); return v;
    };
    auto constant = [&](const std::string &name, int width, const std::string &literal) {
        auto v = op(OperationKind::kConstant, name, width, {});
        g.setAttr(g.valueDef(v), "constValue", literal); return v;
    };
    auto state = [&](const std::string &name, int width, int initial, bool latch) {
        auto id = g.createOperation(latch ? OperationKind::kLatch : OperationKind::kRegister, g.internSymbol(name));
        g.setAttr(id, "width", int64_t{width}); g.setAttr(id, "isSigned", false);
        g.setAttr(id, "initValue", std::to_string(width) + "'d" + std::to_string(initial));
        auto v = op(latch ? OperationKind::kLatchReadPort : OperationKind::kRegisterReadPort, name + "_read", width, {});
        g.setAttr(g.valueDef(v), latch ? "latchSymbol" : "regSymbol", name); return v;
    };
    auto clk = input("clk", 1), other = input("other", 1), rst = input("rst", 1);
    auto en = input("en", 1), x = input("x", 32);
    auto one = constant("one", 1, "1'b1"), mask = constant("mask", 32, "32'hffffffff");
    auto zero = constant("zero", 32, "32'd0"), shift = constant("shift", 32, "32'd3");
    auto a = state("qa", 32, 1, false), b = state("qb", 32, 2, false), c = state("qc", 32, 3, false);
    auto gate = state("gate", 1, 0, true);
    auto notClk = op(OperationKind::kLogicNot, "not_clk", 1, {clk});
    auto lw = g.createOperation(OperationKind::kLatchWritePort, g.internSymbol("write_gate"));
    for (auto v : {notClk, en, one}) g.addOperand(lw, v);
    g.setAttr(lw, "latchSymbol", std::string("gate"));
    auto gated = op(OperationKind::kAnd, "gated", 1, {clk, gate});
    auto common = op(OperationKind::kXor, "common", 32, {x, constant("salt", 32, "32'h1234")});
    auto chain = [&](const std::string &prefix, ValueId seed, ValueId data) {
        for (int i = 0; i < 12; ++i) {
            auto salt = constant(prefix + "_salt" + std::to_string(i), 32, "32'd" + std::to_string(17 + i));
            auto t = op(OperationKind::kXor, prefix + "_t" + std::to_string(i), 32, {seed, salt});
            if (computeEffect && prefix == "a" && i == 6)
                g.setAttr(g.valueDef(t), "hasSideEffects", true);
            auto sum = op(OperationKind::kAdd, prefix + "_sum" + std::to_string(i), 32, {t, data});
            auto shr = op(OperationKind::kLShr, prefix + "_shr" + std::to_string(i), 32, {t, shift});
            seed = op(OperationKind::kXor, prefix + "_v" + std::to_string(i), 32, {sum, shr});
        }
        return seed;
    };
    if (sharedChain) common = chain("shared", common, x);
    g.bindOutputPort("watch", common); // Visible even while both clocks are held.
    auto increment = constant("increment", 32, "32'd1");
    auto plus = op(OperationKind::kAdd, "plus", 32, {common, increment});
    auto nextA = chain("a", b, common), nextB = chain("b", a, plus);
    auto nextC = chain("c", a, common);
    if (memory) {
        const auto mem = g.createOperation(OperationKind::kMemory, g.internSymbol("mem"));
        g.setAttr(mem, "width", int64_t{32}); g.setAttr(mem, "row", int64_t{4}); g.setAttr(mem, "isSigned", false);
        g.setAttr(mem, "initKind", std::vector<std::string>(4, "literal"));
        g.setAttr(mem, "initFile", std::vector<std::string>(4, ""));
        g.setAttr(mem, "initValue", std::vector<std::string>{"32'd11", "32'd22", "32'd33", "32'd44"});
        g.setAttr(mem, "initStart", std::vector<int64_t>{0,1,2,3});
        g.setAttr(mem, "initLen", std::vector<int64_t>(4, 1));
        const auto index = op(OperationKind::kSliceStatic, "mem_index", 2, {common});
        g.setAttr(g.valueDef(index), "sliceStart", int64_t{0});
        g.setAttr(g.valueDef(index), "sliceEnd", int64_t{1});
        const auto read = op(OperationKind::kMemoryReadPort, "mem_read", 32, {index});
        g.setAttr(g.valueDef(read), "memSymbol", std::string("mem"));
        g.bindOutputPort("mem_out", read);
        // The next-state cone must read the pre-write memory snapshot.
        nextC = op(OperationKind::kXor, "c_with_mem", 32, {nextC, read});
        const auto write = g.createOperation(OperationKind::kMemoryWritePort, g.internSymbol("mem_write"));
        const auto byteMask = constant("byte_mask", 32, "32'h00ff00ff");
        for (const auto v : {en, index, x, byteMask, other}) g.addOperand(write, v);
        g.setAttr(write, "memSymbol", std::string("mem"));
        g.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
    }
    auto write = [&](const std::string &name, ValueId next, ValueId event) {
        next = op(OperationKind::kMux, name + "_reset", 32, {rst, next, zero});
        auto id = g.createOperation(OperationKind::kRegisterWritePort, g.internSymbol("write_" + name));
        for (auto v : {one, next, mask, event, rst}) g.addOperand(id, v);
        g.setAttr(id, "regSymbol", name);
        g.setAttr(id, "eventEdge", std::vector<std::string>{"posedge", "negedge"});
    };
    write("qa", nextA, gated); write("qb", nextB, gated); write("qc", nextC, other);
    g.bindOutputPort("a_out", a); g.bindOutputPort("b_out", b); g.bindOutputPort("c_out", c);
    return design;
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

SessionStore schedule(Design &design, ActivityScheduleOptions options,
                      const std::string &expectedError = {}, std::string *log = nullptr) {
    SessionStore session;
    PassManager manager; manager.options().session = &session;
    if (log) {
        manager.options().logLevel = wolvrix::lib::LogLevel::Info;
        manager.options().logSink = [log](wolvrix::lib::LogLevel, std::string_view, std::string_view message) {
            *log += std::string(message) + '\n';
        };
    }
    manager.addPass(std::make_unique<ActivitySchedulePass>(options));
    PassDiagnostics diagnostics;
    const bool success = manager.run(design, diagnostics).success;
    if (expectedError.empty()) {
        if (!success) for (const auto &d : diagnostics.messages()) std::cerr << d.message << '\n';
        require(success, "schedule fixture failed");
    } else {
        require(!success, "unsupported schedule unexpectedly succeeded");
        require(std::any_of(diagnostics.messages().begin(), diagnostics.messages().end(),
            [&](const auto &d) { return d.message.find(expectedError) != std::string::npos; }),
            "schedule failed for an unrelated reason");
    }
    return session;
}

void testProtectedGroup(bool local = false, bool connected = false) {
    auto off = fixture(true), on = fixture(true);
    ActivityScheduleOptions options;
    options.path = "top"; options.maxOpInComputeNode = 4; options.maxOpInComputeSupernode = 8;
    auto before = schedule(off, options);
    options.eventRegions = true;
    options.eventRegionsShared = true;
    options.eventRegionsSharedCycleRefinement = local;
    options.eventRegionsSharedConnected = connected;
    auto after = schedule(on, options);
    const auto &oldGroups = slot<ActivityScheduleSupernodeToOps>(before, "supernode_to_ops");
    const auto &newGroups = slot<ActivityScheduleSupernodeToOps>(after, "supernode_to_ops");
    const auto effect = off.findGraph("top")->findOperation("a_t6_op");
    require(effect.valid(), "missing effect fixture");
    const auto oldGroup = slot<ActivityScheduleOpToSupernode>(before, "op_to_supernode").at(effect.index - 1);
    const auto newGroup = slot<ActivityScheduleOpToSupernode>(after, "op_to_supernode").at(effect.index - 1);
    require(oldGroups.at(oldGroup).size() > 1, "effect fixture has no pure siblings to protect");
    require(oldGroups.at(oldGroup) == newGroups.at(newGroup),
            "effect group's activation context or operation order changed");
    require(newGroups.size() < oldGroups.size(), "protected-group check did not exercise region formation");
}

Design longChain(unsigned length, unsigned publicOutputs = 0) {
    Design design;
    auto &g = design.createGraph("top"); design.markAsTop("top");
    auto input = [&](const std::string &name, int width) {
        auto v = g.createValue(g.internSymbol(name), width, false, ValueType::Logic);
        g.bindInputPort(name, v); return v;
    };
    const auto x = input("x", 32), y = input("y", 32), clk = input("clk", 1);
    const auto en = input("en", 1), mask = input("mask", 32);
    auto cursor = x;
    for (unsigned i = 0; i < length; ++i) {
        const auto name = "chain_" + std::to_string(i);
        const auto op = g.createOperation(OperationKind::kXor, g.internSymbol(name + "_op"));
        const auto value = g.createValue(g.internSymbol(name), 32, false, ValueType::Logic);
        g.addOperand(op, cursor); g.addOperand(op, y); g.addResult(op, value); cursor = value;
    }
    const auto reg = g.createOperation(OperationKind::kRegister, g.internSymbol("q"));
    g.setAttr(reg, "width", int64_t{32}); g.setAttr(reg, "isSigned", false);
    g.setAttr(reg, "initValue", std::string("32'd0"));
    const auto write = g.createOperation(OperationKind::kRegisterWritePort, g.internSymbol("write_q"));
    for (const auto v : {en, cursor, mask, clk}) g.addOperand(write, v);
    g.setAttr(write, "regSymbol", std::string("q"));
    g.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
    for (unsigned i = 0; i < publicOutputs; ++i) g.bindOutputPort("y" + std::to_string(i), cursor);
    return design;
}

void testFallbackAndOptions() {
    for (const auto &args : std::vector<std::vector<std::string_view>>{
             {"-event-regions", "true"}, {"-event-regions=false"},
             {"-event-regions-shared", "true"}, {"-event-regions-shared=false"},
             {"-event-regions-shared-connected", "true"}, {"-event-regions-shared-connected=false"}}) {
        std::string error;
        require(makePass("activity-schedule", args, error) != nullptr, "valid region option rejected");
    }
    for (const auto &args : std::vector<std::vector<std::string_view>>{
             {"-event-regions", "maybe"}, {"-event-regions=maybe"}, {"-event-regions"},
             {"-event-regions-shared", "maybe"}, {"-event-regions-shared=maybe"}, {"-event-regions-shared"},
             {"-event-regions-shared-connected", "maybe"}, {"-event-regions-shared-connected=maybe"}, {"-event-regions-shared-connected"}}) {
        std::string error;
        require(makePass("activity-schedule", args, error) == nullptr && !error.empty(),
                "invalid region option accepted");
    }
    ActivityScheduleOptions options;
    options.path = "top"; options.maxOpInComputeNode = 32; options.maxOpInComputeSupernode = 64;
    // The sole terminal cone exceeds the region bound. Preserve the entire
    // original schedule, rather than selecting an arbitrary profitable prefix.
    auto off = longChain(8300), on = longChain(8300);
    auto before = schedule(off, options);
    options.eventRegions = true;
    auto after = schedule(on, options);
    const auto &groups = slot<ActivityScheduleSupernodeToOps>(before, "supernode_to_ops");
    require(groups.size() > 2, "oversize fixture did not cross original group boundaries");
    require(groups == slot<ActivityScheduleSupernodeToOps>(after, "supernode_to_ops") &&
            slot<ActivityScheduleValueFanout>(before, "value_fanout") ==
                slot<ActivityScheduleValueFanout>(after, "value_fanout") &&
            slot<ActivityScheduleTopoOrder>(before, "topo_order") ==
                slot<ActivityScheduleTopoOrder>(after, "topo_order"),
            "oversize-region fallback changed the schedule");
    auto split = longChain(10);
    options.maxOpInComputeNode = 16; options.maxOpInComputeSupernode = 2;
    options.enableCoarsen = false; options.splitOversizeComputeNodes = true;
    schedule(split, options, "event-regions requires whole compute nodes");
    options.eventRegions = false; options.eventRegionsShared = true;
    schedule(split, options, "event-regions-shared requires event-regions");
}

void testSharedFallback() {
    // One overlarge multi-endpoint cone, then a small cone with too many
    // observable endpoints. Neither fallback may partially fuse the cone.
    for (const auto [length, outputs] : {std::pair{8300u, 1u}, std::pair{100u, 33u}}) {
        auto off = longChain(length, outputs), on = longChain(length, outputs);
        ActivityScheduleOptions options;
        options.path = "top"; options.eventRegions = true;
        options.maxOpInComputeNode = 32; options.maxOpInComputeSupernode = 64;
        auto before = schedule(off, options);
        options.eventRegionsShared = true;
        auto after = schedule(on, options);
        const auto &groups = slot<ActivityScheduleSupernodeToOps>(before, "supernode_to_ops");
        require(groups.size() > 2, "shared fallback fixture has too few groups");
        require(groups == slot<ActivityScheduleSupernodeToOps>(after, "supernode_to_ops") &&
                slot<ActivityScheduleValueFanout>(before, "value_fanout") ==
                    slot<ActivityScheduleValueFanout>(after, "value_fanout") &&
                slot<ActivityScheduleTopoOrder>(before, "topo_order") ==
                    slot<ActivityScheduleTopoOrder>(after, "topo_order"),
                "shared region fallback changed the schedule");
    }
}

// Compare every exported schedule field, including commit order and reader
// activation, so a rejected candidate cannot leak partially rebuilt metadata.
bool sameSchedule(const SessionStore &a, const SessionStore &b) {
#define SAME(TYPE, KEY) if (slot<TYPE>(a, KEY) != slot<TYPE>(b, KEY)) return false
    SAME(ActivityScheduleSupernodeToOps, "supernode_to_ops");
    SAME(ActivityScheduleOpToSupernode, "op_to_supernode");
    SAME(ActivityScheduleCommitLocalityGroupByOp, "commit_locality_group_by_op");
    SAME(ActivityScheduleCommitLocalityGroupOrder, "commit_locality_group_order");
    SAME(ActivityScheduleDag, "dag");
    SAME(ActivityScheduleSupernodeKinds, "supernode_kind");
    SAME(ActivityScheduleComputeNodesBySupernode, "compute_nodes_by_supernode");
    SAME(ActivityScheduleValueFanout, "value_fanout");
    SAME(ActivityScheduleTopoOrder, "topo_order");
    SAME(ActivityScheduleStateReadSupernodes, "state_read_supernodes");
    SAME(std::string, "summary_stats");
#undef SAME
    return true;
}

std::array<std::size_t, 4> scheduleCosts(const SessionStore &s) {
    std::array<std::size_t, 4> cost{};
    for (const auto kind : slot<ActivityScheduleSupernodeKinds>(s, "supernode_kind"))
        cost[0] += kind == ActivityScheduleSupernodeKind::Compute;
    for (const auto &targets : slot<ActivityScheduleValueFanout>(s, "value_fanout")) {
        cost[1] += !targets.empty(); cost[2] += targets.size();
    }
    for (const auto &[name, targets] : slot<ActivityScheduleStateReadSupernodes>(s, "state_read_supernodes")) {
        (void)name; cost[3] += targets.size();
    }
    return cost;
}

void testSharedCostGuard() {
    require(!ActivityScheduleOptions{}.eventRegionsSharedCostGuard, "cost guard must default off");
    for (const auto &args : std::vector<std::vector<std::string_view>>{
             {"-event-regions-shared-cost-guard", "true"}, {"-event-regions-shared-cost-guard=false"},
             {"-event-regions-shared-cost-guard=on"}, {"-event-regions-shared-cost-guard=0"}}) {
        std::string error;
        require(makePass("activity-schedule", args, error) != nullptr, "valid cost guard option rejected");
    }
    for (const auto &args : std::vector<std::vector<std::string_view>>{
             {"-event-regions-shared-cost-guard", "maybe"}, {"-event-regions-shared-cost-guard=maybe"},
             {"-event-regions-shared-cost-guard"}}) {
        std::string error;
        require(makePass("activity-schedule", args, error) == nullptr && !error.empty(),
                "invalid cost guard option accepted");
    }
    ActivityScheduleOptions options;
    options.path = "top"; options.eventRegions = true; options.eventRegionsSharedCostGuard = true;
    auto invalid = longChain(10);
    schedule(invalid, options, "event-regions-shared-cost-guard requires event-regions-shared");

    // A single multi-endpoint chain internalizes real boundaries without
    // fragmenting any remainder. The guard must keep this useful contraction.
    options.eventRegionsSharedCostGuard = false;
    options.maxOpInComputeNode = 4; options.maxOpInComputeSupernode = 8;
    auto baselineDesign = longChain(100, 1), sharedDesign = longChain(100, 1);
    auto baseline = schedule(baselineDesign, options);
    options.eventRegionsShared = true;
    auto shared = schedule(sharedDesign, options);
    const auto before = scheduleCosts(baseline), after = scheduleCosts(shared);
    require(before != after, "acceptance fixture has no structural improvement");
    for (std::size_t i = 0; i < before.size(); ++i)
        require(after[i] <= before[i], "acceptance fixture has a growing cost");
    auto explicitOffDesign = longChain(100, 1);
    options.eventRegionsSharedCostGuard = false;
    std::string offLog;
    auto explicitOff = schedule(explicitOffDesign, options, {}, &offLog);
    require(sameSchedule(shared, explicitOff) && offLog.find("cost guard:") == std::string::npos,
            "explicit guard off changed shared schedule or emitted a decision");
    auto acceptedDesign = longChain(100, 1);
    options.eventRegionsSharedCostGuard = true;
    std::string acceptedLog;
    auto accepted = schedule(acceptedDesign, options, {}, &acceptedLog);
    require(sameSchedule(shared, accepted) && acceptedLog.find("cost guard: accepted=true") != std::string::npos,
            "guard failed to retain Pareto improvement");

    // No candidate contraction is not a strict improvement. Check exact
    // fallback including a real register commit and state-derived metadata.
    auto equalBaselineDesign = fixture(false, false), equalGuardedDesign = fixture(false, false);
    options.maxOpInComputeNode = 8192; options.maxOpInComputeSupernode = 8192;
    options.eventRegionsShared = false; options.eventRegionsSharedCostGuard = false;
    auto equalBaseline = schedule(equalBaselineDesign, options);
    options.eventRegionsShared = true; options.eventRegionsSharedCostGuard = true;
    std::string equalLog;
    auto equalGuarded = schedule(equalGuardedDesign, options, {}, &equalLog);
    require(sameSchedule(equalBaseline, equalGuarded) && equalLog.find("cost guard: accepted=false") != std::string::npos,
            "guard accepted a cost tie or changed fallback metadata");

    // The shared chain crosses groups which also hold unrelated output nodes.
    // Extracting it can fragment the remainders and grow maintenance edges.
    options.maxOpInComputeNode = 1; options.maxOpInComputeSupernode = 32;
    options.eventRegionsShared = false; options.eventRegionsSharedCostGuard = false;
    auto fragmentedBaselineDesign = fixture(false, true);
    auto fragmentedBaseline = schedule(fragmentedBaselineDesign, options);
    options.eventRegionsShared = true;
    auto fragmentedSharedDesign = fixture(false, true);
    auto fragmentedShared = schedule(fragmentedSharedDesign, options);
    const auto oldCost = scheduleCosts(fragmentedBaseline), newCost = scheduleCosts(fragmentedShared);
    require(newCost[0] > oldCost[0] && newCost[1] < oldCost[1] && newCost[2] < oldCost[2],
            "rejection fixture must trade fewer boundaries for more groups");
    options.eventRegionsSharedCostGuard = true;
    auto fragmentedGuardedDesign = fixture(false, true);
    std::string rejectedLog;
    auto fragmentedGuarded = schedule(fragmentedGuardedDesign, options, {}, &rejectedLog);
    require(sameSchedule(fragmentedBaseline, fragmentedGuarded) &&
            rejectedLog.find("cost guard: accepted=false") != std::string::npos,
            "guard did not restore the exact pre-shared schedule");
}

const char *harness = R"CPP(
#include "grhsim_top.hpp"
#include <cstdint>
#include <iostream>
uint32_t f(uint32_t seed, uint32_t data) {
    for (unsigned i=0;i<12;++i) { const uint32_t t=seed^(17+i); seed=(t+data)^(t>>3); }
    return seed;
}
int main() {
    GrhSIM_top m; m.init(); m.clk=0; m.other=0; m.rst=1; m.en=0; m.x=0; m.eval();
    uint32_t a=1,b=2,c=3,rng=1; bool gate=false,prevG=false,prevO=false,prevR=true;
    uint32_t memory[4]={11,22,33,44};
    for(unsigned step=0;step<3000;++step) {
        rng=rng*1664525U+1013904223U;
        m.clk=(step%4)>=2; m.other=(step%6)>=3; m.en=(rng>>24)&1;
        m.rst=(step%79)!=3; m.x=rng;
        if (!m.clk) gate=m.en;
        const bool g=m.clk&&gate;
        const uint32_t oldA=a,oldB=b;
        uint32_t common=m.x^0x1234U;
#ifdef TEST_SHARED_CHAIN
        common=f(common,m.x);
#endif
        uint32_t readMemory=0;
#ifdef TEST_MEMORY
        readMemory=memory[common&3];
#endif
        if((g&&!prevG)||(!m.rst&&prevR)) {a=m.rst?f(oldB,common):0;b=m.rst?f(oldA,common+1):0;}
        if((m.other&&!prevO)||(!m.rst&&prevR)) c=m.rst?(f(oldA,common)^readMemory):0;
#ifdef TEST_MEMORY
        if(m.other&&!prevO&&m.en)
            memory[common&3]=(memory[common&3]&0xff00ff00u)|(m.x&0x00ff00ffu);
#endif
        m.eval();
#ifdef TEST_MEMORY
        if(m.mem_out!=memory[common&3]) {std::cerr<<"memory mismatch step="<<step<<'\n';return 2;}
#endif
        if(m.a_out!=a || m.b_out!=b || m.c_out!=c || m.watch!=common) {
            std::cerr<<"snapshot mismatch step="<<step<<" a="<<m.a_out<<" expected="<<a<<'\n'; return 1;
        }
        prevG=g;prevO=m.other;prevR=m.rst;
    }
}
)CPP";
}

int main(int argc, char **argv) {
    try {
        testSharedCostGuard();
        if (argc == 2 && std::string(argv[1]) == "--cost-guard-only") return 0;
        const bool cycleOnly = argc == 2 && std::string(argv[1]) == "--cycle-refinement-only";
        const bool connectedOnly = argc == 2 && std::string(argv[1]) == "--connected-only";
        if (connectedOnly) {
            ActivityScheduleOptions invalid; invalid.path = "top"; invalid.eventRegionsSharedConnected = true;
            auto graph = fixture(); schedule(graph, invalid, "event-regions-shared-connected requires event-regions-shared");
        }
        if (cycleOnly) {
            ActivityScheduleOptions invalid; invalid.path="top";
            invalid.eventRegionsSharedCycleRefinement=true;
            auto graph=fixture();
            schedule(graph,invalid,"event-regions-shared-cycle-refinement requires event-regions-shared");
        }
        testProtectedGroup(cycleOnly, connectedOnly);
        testFallbackAndOptions();
        testSharedFallback();
        const auto root = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_event_regions";
        std::map<std::string, std::string> defaultFiles;
        std::size_t before = 0, after = 0;
        std::size_t sharedBefore = 0, sharedAfter = 0;
        for (const std::string mode : {"default", "off", "on", "shared-off", "shared", "memory-off", "memory", "cycle", "memory-cycle", "connected", "memory-connected"}) {
            const bool cycleMode = mode == "cycle" || mode == "memory-cycle";
            const bool connectedMode = mode == "connected" || mode == "memory-connected";
            if (connectedOnly && !connectedMode) continue;
            if (!connectedOnly && connectedMode) continue;
            if (cycleOnly && !cycleMode) continue;
            if (!cycleOnly && cycleMode) continue;
            const bool memoryFixture = mode == "memory-off" || mode == "memory" || mode == "memory-cycle" || mode == "memory-connected";
            const bool sharedFixture = mode == "shared-off" || mode == "shared" || memoryFixture || cycleMode || connectedMode;
            auto design = fixture(false, sharedFixture, memoryFixture);
            SessionStore session; PassManager manager; manager.options().session = &session;
            ActivityScheduleOptions schedule; schedule.path="top";
            schedule.maxOpInComputeNode=4; schedule.maxOpInComputeSupernode=8;
            if (mode != "default") schedule.eventRegions=mode=="on" || sharedFixture;
            schedule.eventRegionsShared = mode == "shared" || mode == "memory" || cycleMode || connectedMode;
            schedule.eventRegionsSharedCycleRefinement = cycleMode;
            schedule.eventRegionsSharedConnected = connectedMode;
            manager.addPass(std::make_unique<ActivitySchedulePass>(schedule));
            PassDiagnostics diagnostics;
            if (!manager.run(design, diagnostics).success) {
                for (const auto &d:diagnostics.messages()) std::cerr<<d.message<<'\n';
                throw std::runtime_error("schedule failed");
            }
            auto boundaries = slot<ActivityScheduleValueFanout>(session,"value_fanout");
            auto n = std::count_if(boundaries.begin(),boundaries.end(),[](const auto &v){return !v.empty();});
            if(mode=="on") after=n;
            else if(mode=="off") before=n;
            else if(mode=="shared-off") sharedBefore=n;
            else if(mode=="shared") sharedAfter=n;
            const auto dir=root/mode; std::filesystem::create_directories(dir);
            EmitOptions options; options.outputDir=dir; options.topOverrides={"top"}; options.session=&session;
            options.attributes["waveform"]="off";
            EmitDiagnostics ed;
            if(!EmitGrhSimCpp(&ed).emit(design,options).success) {
                for(const auto &d:ed.messages()) std::cerr<<d.message<<'\n';
                throw std::runtime_error("emitter failed");
            }
            std::map<std::string,std::string> files;
            for(const auto &p:std::filesystem::directory_iterator(dir))
                if(p.path().extension()==".cpp" || p.path().extension()==".hpp")
                    if(p.path().filename()!="harness.cpp") files.emplace(p.path().filename().string(),read(p.path()));
            if(mode=="default") defaultFiles=files;
            if(mode=="off" && files!=defaultFiles) throw std::runtime_error("default/off source drift");
            std::ofstream(dir/"harness.cpp") << (sharedFixture ? "#define TEST_SHARED_CHAIN\n" : "")
                << (memoryFixture ? "#define TEST_MEMORY\n" : "") << harness;
            if(mode=="default") continue;
            for(const auto flags: {"-O0", "-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"}) {
                const char *compiler=std::getenv("CXX");
                std::string command=quote(compiler ? compiler : WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
                for(const auto &[name, text]:files) if(std::filesystem::path(name).extension()==".cpp") command+=" "+quote((dir/name).string());
                command+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
                if(std::system(command.c_str())!=0 || std::system(quote((dir/"check").string()).c_str())!=0)
                    throw std::runtime_error("generated model test failed: "+mode);
            }
        }
        if (cycleOnly || connectedOnly) return 0;
        if(after>=before) throw std::runtime_error("event regions did not internalize boundaries");
        require(sharedAfter < sharedBefore, "shared region fixture did not internalize shared boundaries");
        std::cout<<"boundary values "<<before<<" -> "<<after<<'\n';
        std::cout<<"shared boundary values "<<sharedBefore<<" -> "<<sharedAfter<<'\n';
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
