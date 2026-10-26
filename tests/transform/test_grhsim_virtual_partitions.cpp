#include "grhsim_partition_fixture.hpp"

namespace {
using namespace partition_test;

void testConesHistoriesAndRestart() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable");
    const auto address = fixture.input("address", 2), data = fixture.input("data", 8);
    const auto one = fixture.constant(1, "1'h1");
    const auto memory = fixture.state("memory", 32, {4});
    graph.setAttr(memory, "initSpecValues", std::vector<std::string>{"32'h01010101"});
    const auto q = fixture.state("q", 8);
    const auto loaded = fixture.read(memory, {address});
    const auto bit = fixture.compute(OperationKind::kSliceStatic, Fixture::type(1), {loaded});
    graph.setAttr(graph.valueDef(bit), "sliceStart", int64_t{0}); graph.setAttr(graph.valueDef(bit), "sliceEnd", int64_t{0});
    const auto gated = fixture.compute(OperationKind::kAnd, Fixture::type(1), {clk, bit});
    fixture.write(OperationKind::kRegisterWritePort, q, {enable, loaded, gated});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {enable, address, data, clk});
    graph.bindOutputPort("value", loaded); graph.bindOutputPort("q", fixture.read(q));
    fixture.task("monitor", {one, loaded});
    fixture.task("strobe", {enable, loaded, gated}, {"posedge"});
    fixture.convert();
    const auto reference = fixture.design.clone();
    const auto detectors = count(graph, sim::kEdgeDetect);
    require(fixture.session.empty(), "conversion/simplify published a plan");
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    require(count(graph, OperationKind::kRegisterReadArrayPort) == 3, "dynamic-address read cone not extracted into all three regions");
    require(count(graph, sim::kEdgeDetect) == detectors && count(graph, sim::kEdgeDetect, {"event"}) == detectors, "detector/history was copied");
    require(count(graph, sim::kHistoryWrite, {"output"}) == 2, "free monitor lacks condition/argument histories");
    for (const auto op : graph.operations()) {
        const auto kind = graph.opKind(op);
        if (kind == OperationKind::kRegister || kind == OperationKind::kDpicImport) require(owner(graph, op).empty(), "declaration acquired an owner");
        else require(owner(graph, op).size() == 1, "first split prematurely refined a region");
    }
    const auto checkpoint = json(fixture.design);
    const auto stamp = tx::DesignStamp::capture(graph);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    require(checkpoint == json(fixture.design) && stamp.matches(graph), "event split is not idempotent");
    auto candidate = grh::Design::fromJsonString(checkpoint);
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned i = 0; i < 64; ++i) inputs.push_back({{"clk", (i / 2) & 1}, {"enable", i % 7 != 0}, {"address", (i / 4) & 3}, {"data", (i * 3) & 255}});
    inputs.push_back(inputs.back()); inputs.push_back(inputs.back());
    compare(reference, candidate, {"memory", "q"}, inputs, "event-output");
    run(candidate, "grhsim-split-nonsink-and-sink", fixture.session);
    require(fixture.session.empty(), "split published an extra plan");
    const auto generalCheckpoint = json(candidate);
    const auto generalStamp = tx::DesignStamp::capture(*candidate.findGraph("top"));
    run(candidate, "grhsim-split-nonsink-and-sink", fixture.session);
    require(generalCheckpoint == json(candidate) && generalStamp.matches(*candidate.findGraph("top")), "General split is not idempotent");
    candidate = grh::Design::fromJsonString(json(candidate));
    compare(reference, candidate, {"memory", "q"}, inputs, "nonsink-sink");
    run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=2", "-max-supernode-work=4"});
    require(fixture.session.empty(), "activity cutter published an extra plan");
    const auto activity = json(candidate);
    const auto activityStamp = tx::DesignStamp::capture(*candidate.findGraph("top"));
    partition_test::checkpoint(candidate, "nonsink-partitioned");
    run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=2", "-max-supernode-work=4"});
    require(activity == json(candidate) && activityStamp.matches(*candidate.findGraph("top")), "activity cutter is not idempotent");
    candidate = grh::Design::fromJsonString(activity);
    run(candidate, "grhsim-simplify", fixture.session);
    run(candidate, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, *candidate.findGraph("top"));
    compare(reference, candidate, {"memory", "q"}, inputs, "sink-schedule", &schedule);
    auto reloaded = grh::Design::fromJsonString(json(candidate));
    rejects([&] { sim::requireSinkSchedule(&fixture.session, *reloaded.findGraph("top")); }, "load reused resolved schedule");
    run(reloaded, "grhsim-partition-sink", fixture.session);
    const auto& rebuilt = sim::requireSinkSchedule(&fixture.session, *reloaded.findGraph("top"));
    compare(reference, reloaded, {"memory", "q"}, inputs, "sink-restart", &rebuilt);
}

void testSizePolicyAndFailures() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable");
    const auto small = fixture.state("smallArray", 32, {4});
    const auto large = fixture.state("largeScalar", 512);
    const auto d32 = fixture.input("d32", 32);
    const auto d512 = fixture.compute(OperationKind::kReplicate, Fixture::type(512), {d32});
    fixture.graph.setAttr(fixture.graph.valueDef(d512), "rep", int64_t{16});
    const auto smallWrite = fixture.write(OperationKind::kRegisterWritePort, small, {enable, d32, clk});
    const auto largeWrite = fixture.write(OperationKind::kRegisterWritePort, large, {enable, d512, clk});
    fixture.convert();
    const auto before = json(fixture.design);
    require(!run(fixture.design, "grhsim-partition-nonsink", fixture.session, {}, false) && before == json(fixture.design), "out-of-order pass mutated the graph");
    fixture.split();
    require(owner(fixture.graph, smallWrite) == Owner{"general", "small-sink"}, "small array classified by shape");
    require(owner(fixture.graph, largeWrite) == Owner{"general", "large-sink"}, "large scalar classified by shape");
    const auto split = json(fixture.design);
    require(!run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=128"}, false) && split == json(fixture.design), "threshold changed without restarting from prior stage");
    auto& graph = fixture.graph;
    graph.setAttr(smallWrite, sim::kPartition, Owner{"general"});
    require(!sim::verifyGraph(graph).empty(), "parent container accepted direct member");
    graph.setAttr(smallWrite, sim::kPartition, Owner{"general", "small-sink"});
    const auto input = graph.opOperands(largeWrite)[2];
    const auto def = graph.valueDef(input);
    require(def.valid(), "large data has no computation");
    graph.setAttr(def, sim::kPartition, Owner{"output"});
    require(!sim::verifyGraph(graph).empty(), "ordinary Output-to-General dependency accepted");
    for (const std::string_view name : {"grhsim-split-event-and-output", "grhsim-split-nonsink-and-sink", "grhsim-partition-nonsink", "grhsim-partition-sink"}) {
        std::string error;
        const std::array<std::string_view, 1> lines{"-max-function-lines=4"};
        require(!tx::makePass(name, lines, error), "chunk parameter accepted by partition pass");
    }
}

void testActivityAndCloneIsolation() {
    Fixture fixture;
    const auto x = fixture.input("x", 8), a = fixture.input("a", 8), b = fixture.input("b", 8);
    const auto one = fixture.constant(1, "1'h1");
    const auto inverted = fixture.compute(OperationKind::kNot, Fixture::type(8), {x});
    fixture.graph.setAttr(fixture.graph.valueDef(inverted), "originModules", std::vector<std::string>{"top$shared"});
    const auto left = fixture.compute(OperationKind::kAdd, Fixture::type(8), {inverted, a});
    const auto right = fixture.compute(OperationKind::kXor, Fixture::type(8), {inverted, b});
    fixture.task("display", {one, left}); fixture.task("display", {one, right});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    const auto split = json(fixture.design);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=1"});
    std::set<Owner> owners;
    std::size_t nots = 0;
    for (const auto op : fixture.graph.operations()) if (fixture.graph.opKind(op) == OperationKind::kNot) {
        ++nots; owners.insert(owner(fixture.graph, op));
        require(sim::attribute<std::vector<std::string>>(fixture.graph, op, "originModules") == std::vector<std::string>{"top$shared"}, "boundary clone lost source annotation");
    }
    require(nots >= 3 && owners.size() == nots, "shared boundary was not cloned into separate owners");
    run(fixture.design, "grhsim-simplify", fixture.session);
    require(count(fixture.graph, OperationKind::kNot) >= 2, "partition simplify merged clones back together");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned i = 0; i < 128; ++i) inputs.push_back({{"x", i}, {"a", i * 17}, {"b", i * 11}});
    compare(reference, fixture.design, {}, inputs, "activity-clones", &schedule);
    auto noClone = grh::Design::fromJsonString(split);
    run(noClone, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=1", "-clone-shared=0"});
    require(count(*noClone.findGraph("top"), OperationKind::kNot) == 1, "clone disable option ignored");
}

void testEffectSampling() {
    Fixture fixture;
    const auto input = fixture.input("input", 8), one = fixture.constant(1, "1'h1");
    const auto import = fixture.graph.createOperation(OperationKind::kDpicImport, fixture.graph.internSymbol("dpi"));
    fixture.graph.setAttr(import, "argsName", std::vector<std::string>{"in"});
    fixture.graph.setAttr(import, "argsDirection", std::vector<std::string>{"input"});
    fixture.graph.setAttr(import, "argsWidth", std::vector<int64_t>{8}); fixture.graph.setAttr(import, "argsSigned", std::vector<bool>{false});
    fixture.graph.setAttr(import, "hasReturn", true); fixture.graph.setAttr(import, "returnWidth", int64_t{8}); fixture.graph.setAttr(import, "returnSigned", false);
    const auto call = fixture.graph.createOperation(OperationKind::kDpicCall, fixture.graph.internSymbol("call"));
    const auto result = fixture.graph.createValue(Fixture::type(8));
    fixture.graph.addOperand(call, one); fixture.graph.addOperand(call, input); fixture.graph.addResult(call, result);
    fixture.graph.setAttr(call, "targetImportSymbol", std::string("dpi"));
    fixture.graph.bindOutputPort("dpi", result);
    const auto random = fixture.compute(OperationKind::kSystemFunction, Fixture::type(32), {});
    fixture.graph.setAttr(fixture.graph.valueDef(random), "name", std::string("random"));
    fixture.graph.bindOutputPort("random", random);
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split(); fixture.partition();
    require(count(fixture.graph, sim::kDpiCall) == 1 && count(fixture.graph, sim::kSystemFunction) == 1, "effectful call/sample cloned for Output");
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {}, {{{"input", 0}}, {{"input", 1}}, {{"input", 1}}, {{"input", 255}}}, "effect-sampling", &schedule);
}

void testActivityWeightsAndQuotientValidation() {
    Fixture fixture;
    const auto c0 = fixture.input("c0"), c1 = fixture.input("c1"), en = fixture.input("en"), data = fixture.input("data");
    const auto q0 = fixture.state("q0", 1), q1 = fixture.state("q1", 1);
    fixture.write(OperationKind::kRegisterWritePort, q0, {en, data, c0});
    fixture.write(OperationKind::kRegisterWritePort, q1, {en, data, c1});
    fixture.convert();
    const auto detectors = [&] {
        std::vector<grh::OperationId> ops;
        for (const auto op : fixture.graph.operations()) if (fixture.graph.opKind(op) == sim::kEdgeDetect) ops.push_back(op);
        return ops;
    }();
    require(detectors.size() == 2, "activity fixture lost its two clocks");
    const auto f0 = fixture.graph.opResults(detectors[0])[0], f1 = fixture.graph.opResults(detectors[1])[0];
    const auto a = fixture.compute(OperationKind::kNot, Fixture::type(1), {f0});
    const auto b = fixture.compute(OperationKind::kXor, Fixture::type(1), {f0, f1});
    const auto c = fixture.compute(OperationKind::kNot, Fixture::type(1), {f1});
    for (const auto value : {a, b, c}) fixture.graph.addDeclaredSymbol(fixture.graph.valueSymbol(value));
    fixture.split();
    const auto checkpoint = json(fixture.design);
    const auto sameSupernode = [](const grh::Graph& graph, grh::ValueId x, grh::ValueId y) {
        const auto left = owner(graph, graph.valueDef(x)), right = owner(graph, graph.valueDef(y));
        return left.size() == 4 && right.size() == 4 && left[2] == right[2];
    };
    for (unsigned hot = 0; hot < 2; ++hot) {
        auto candidate = grh::Design::fromJsonString(checkpoint);
        auto& graph = *candidate.findGraph("top");
        graph.setAttr(detectors[hot], "grhsim.activity", int64_t{100});
        run(candidate, "grhsim-partition-nonsink", fixture.session,
            {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=0"});
        require(sameSupernode(graph, b, hot == 0 ? a : c) && !sameSupernode(graph, a, c),
                "activity weights did not change the cut at the shared input");
    }
    for (const int64_t weight : {int64_t{-1}, INT64_MAX}) {
        auto invalid = grh::Design::fromJsonString(checkpoint);
        auto& invalidGraph = *invalid.findGraph("top");
        for (const auto detector : detectors) invalidGraph.setAttr(detector, "grhsim.activity", weight);
        const auto unchanged = json(invalid);
        require(!run(invalid, "grhsim-partition-nonsink", fixture.session,
            {"-max-node-work=1", "-max-supernode-work=2", "-segment-penalty=2", "-clone-shared=0"}, false) &&
            unchanged == json(invalid), "invalid/overflowing activity cost mutated the graph");
    }
    // Acyclic ops can still form a cyclic partition quotient: a -> b -> c
    // cannot put a/c in one leaf or supernode while b belongs to another.
    auto& graph = fixture.graph;
    graph.replaceOperand(graph.valueDef(b), 0, a);
    graph.replaceOperand(graph.valueDef(c), 0, b);
    graph.setAttr(graph.valueDef(a), sim::kPartition, Owner{"general", "nonsink", "s0", "n0"});
    graph.setAttr(graph.valueDef(b), sim::kPartition, Owner{"general", "nonsink", "s1", "n1"});
    graph.setAttr(graph.valueDef(c), sim::kPartition, Owner{"general", "nonsink", "s0", "n0"});
    graph.setMetadata(sim::kStage, std::string("nonsink-partitioned"));
    require(sim::verifyGraph(graph).find("Cyclic virtual partition") != std::string::npos, "cyclic leaf quotient accepted");
    graph.setAttr(graph.valueDef(c), sim::kPartition, Owner{"general", "nonsink", "s0", "n2"});
    require(sim::verifyGraph(graph).find("Cyclic virtual partition") != std::string::npos, "cyclic supernode quotient accepted");
}

void testPulseOutputAndFourStateHistories() {
    Fixture pulse;
    const auto clock = pulse.input("clock"), enable = pulse.input("enable"), data = pulse.input("data");
    const auto q = pulse.state("q", 1);
    pulse.write(OperationKind::kRegisterWritePort, q, {enable, data, clock});
    pulse.convert();
    for (const auto op : pulse.graph.operations()) if (pulse.graph.opKind(op) == sim::kEdgeDetect)
        pulse.graph.bindOutputPort("pulse", pulse.graph.opResults(op)[0]);
    const auto reference = pulse.design.clone();
    pulse.split(); pulse.partition();
    const auto& schedule = sim::requireSinkSchedule(&pulse.session, pulse.graph);
    compare(reference, pulse.design, {"q"}, {{{"clock", 1}, {"enable", 1}, {"data", 1}},
        {{"clock", 0}}, {{"clock", 1}}, {{"clock", 1}}}, "pulse-output", &schedule);

    Fixture fourState;
    const auto cond = fourState.input("cond");
    const auto x = fourState.constant(8, "8'hx");
    const auto real = fourState.graph.createValue(grh::LogicalType{grh::ValueType::Real, 64, false, grh::LogicDomain::FourState, {}});
    const auto string = fourState.graph.createValue(grh::LogicalType{grh::ValueType::String, 0, false, grh::LogicDomain::FourState, {}});
    fourState.graph.bindInputPort("real", real); fourState.graph.bindInputPort("string", string);
    fourState.task("monitor", {cond, x, real, string});
    run(fourState.design, "grh-to-grhsim", fourState.session, {"-logic-domain=4state"});
    fourState.split(); fourState.partition();
    require(count(fourState.graph, sim::kHistoryWrite) == 4, "mixed-type monitor did not get complete history");
    require(fourState.graph.valueDomain(x) == grh::LogicDomain::FourState, "partitioning projected four-state values");
    for (const auto op : fourState.graph.operations()) if (fourState.graph.opKind(op) == sim::kHistoryWrite)
        require(grh::stateLogicalType(fourState.graph, sim::writerTarget(fourState.graph, op)) ==
            fourState.graph.valueLogicalType(fourState.graph.opOperands(op)[0]), "history lost its complete type");
    auto restored = grh::Design::fromJsonString(json(fourState.design));
    run(restored, "grhsim-partition-sink", fourState.session);
}
}

int main() {
    try {
        testConesHistoriesAndRestart(); testSizePolicyAndFailures(); testActivityAndCloneIsolation();
        testEffectSampling(); testActivityWeightsAndQuotientValidation();
        testPulseOutputAndFourStateHistories();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
