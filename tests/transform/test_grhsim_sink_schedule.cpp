#include "grhsim_partition_fixture.hpp"

namespace {
using namespace partition_test;

void testOrderedRunsAndSnapshots() {
    Fixture fixture;
    const auto clock0 = fixture.input("clock0"), clock1 = fixture.input("clock1");
    const auto enable = fixture.input("enable"), address = fixture.input("address", 2), data = fixture.input("data", 8);
    const auto one = fixture.constant(1, "1'h1"), mask = fixture.constant(8, "8'h0f");
    const auto q = fixture.state("q", 8), memory = fixture.state("memory", 32, {4});
    const auto peer = fixture.state("peer", 8), extra = fixture.state("extra", 8);
    const auto qValue = fixture.read(q);
    const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {qValue, data});
    fixture.task("display", {one, sum}); // Keep the shared parameter in nonsink.
    std::vector<grh::OperationId> writers;
    writers.push_back(fixture.write(OperationKind::kRegisterWritePort, q, {enable, data, clock0}));
    writers.push_back(fixture.write(OperationKind::kRegisterWritePort, peer, {enable, sum, clock1}));
    writers.push_back(fixture.write(OperationKind::kRegisterWritePort, extra, {enable, sum, clock0}));
    writers.push_back(fixture.write(OperationKind::kRegisterWriteMaskPort, q, {enable, sum, mask, clock1}));
    writers.push_back(fixture.write(OperationKind::kRegisterWritePort, q, {enable, sum, clock0, clock1}, {"posedge", "negedge"}));
    writers.push_back(fixture.write(OperationKind::kRegisterFillPort, memory, {enable, data, clock0}));
    writers.push_back(fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {enable, address, sum, mask, clock1}));
    writers.push_back(fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {enable, address, qValue, clock0}));
    fixture.graph.bindOutputPort("q", qValue); fixture.graph.bindOutputPort("memory", fixture.read(memory));
    fixture.graph.bindOutputPort("peer", fixture.read(peer)); fixture.graph.bindOutputPort("extra", fixture.read(extra));
    fixture.convert(); const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=4"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=2", "-max-supernode-work=4"});
    const auto nonsinkCheckpoint = json(fixture.design);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    auto& graph = fixture.graph;
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    std::vector<grh::OperationId> order;
    for (const auto& run : schedule.generalRuns) order.insert(order.end(), run.orderedWriters.begin(), run.orderedWriters.end());
    require(order == writers, "clock grouping reordered the original writers");
    require(!schedule.largeCommitRuns.empty(), "large commits omitted");
    require(std::any_of(schedule.generalRuns.begin(), schedule.generalRuns.end(), [](const auto& run) { return !run.requiredNonsinkKeys.empty(); }), "sink nonsink dependencies omitted");
    require(std::any_of(schedule.sinkGroups.begin(), schedule.sinkGroups.end(), [](const auto& group) { return !group.guards.empty(); }), "shared enable guards not grouped");
    // Independent states at priority zero share clock0 but are interleaved by
    // clock1; the same group must recur instead of moving the third writer.
    require(schedule.generalRuns.size() >= 3 && schedule.generalRuns[0].partitionKey == schedule.generalRuns[2].partitionKey, "interleaved sink group was not represented by repeated runs");
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned i = 0; i < 128; ++i) inputs.push_back({{"clock0", (i / 2) & 1}, {"clock1", (i / 3) & 1}, {"enable", i % 9 != 0}, {"address", i & 3}, {"data", (i * 13) & 255}});
    compare(reference, fixture.design, {"q", "memory", "peer", "extra"}, inputs, "ordered-snapshots", &schedule);
    auto forged = schedule;
    forged.generalRuns[0].orderedWriters.clear();
    require(!sim::verifySinkSchedule(graph, forged).empty(), "incomplete schedule passed verification");
    forged = schedule; std::swap(forged.generalRuns[0], forged.generalRuns[1]);
    require(!sim::verifySinkSchedule(graph, forged).empty(), "reordered schedule passed verification");
    const auto final = json(fixture.design);
    const auto stamp = tx::DesignStamp::capture(graph);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    require(stamp.matches(graph) && final == json(fixture.design), "schedule rebuild mutated graph");
    require(!run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=3"}, false), "changed config accepted without restart");
    rejects([&] { sim::requireSinkSchedule(&fixture.session, graph); }, "failed reconfiguration retained old complete schedule");
    auto restarted = grh::Design::fromJsonString(nonsinkCheckpoint);
    run(restarted, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=3"});
    require(sim::requireSinkSchedule(&fixture.session, *restarted.findGraph("top")).complete, "prior checkpoint cannot select new sink config");
}

void testLifecycleAndNegativeContracts() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), data = fixture.input("data", 8);
    const auto unused = fixture.graph.createValue(Fixture::type(8));
    const auto state = fixture.state("q", 8);
    fixture.write(OperationKind::kRegisterWritePort, state, {enable, data, clock});
    fixture.graph.bindOutputPort("q", fixture.read(state));
    fixture.convert(); fixture.split(); fixture.partition();
    auto& graph = fixture.graph;
    const auto checkpoint = json(fixture.design);
    const auto initial = sim::requireSinkSchedule(&fixture.session, graph);
    rejects([&] { sim::requireSinkSchedule(nullptr, graph); }, "missing session accepted");
    tx::SessionStore other;
    rejects([&] { sim::requireSinkSchedule(&other, graph); }, "wrong session accepted");
    const auto key = sim::sinkScheduleKey(graph);
    other[key] = std::make_unique<tx::SessionSlotValue<int>>(7);
    rejects([&] { sim::requireSinkSchedule(&other, graph); }, "wrong slot type accepted");
    auto clone = fixture.design.clone();
    rejects([&] { sim::requireSinkSchedule(&fixture.session, *clone.findGraph("top")); }, "clone reused old instance schedule");
    const auto sourceStamp = tx::DesignStamp::capture(graph);
    tx::SessionStore isolated;
    for (const auto& [name, slot] : fixture.session) isolated.emplace(name, slot->clone());
    run(clone, "grhsim-partition-sink", isolated);
    require(sim::requireSinkSchedule(&isolated, *clone.findGraph("top")).complete && sourceStamp.matches(graph) &&
        checkpoint == json(fixture.design), "isolated dry run changed the source graph/session");
    require(sim::requireSinkSchedule(&fixture.session, graph).complete, "dry run invalidated source schedule");
    graph.eraseValue(unused); graph.freeze();
    require(!sim::verifySinkSchedule(graph, initial).empty(), "freeze reused remapped schedule IDs");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    auto refreshed = sim::requireSinkSchedule(&fixture.session, graph);
    const auto writer = refreshed.generalRuns[0].orderedWriters[0];
    const auto savedOwner = owner(graph, writer);
    graph.setOpSymbol(writer, graph.internSymbol("renamed_writer"));
    rejects([&] { sim::requireSinkSchedule(&fixture.session, graph); }, "rename did not invalidate schedule");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    require(owner(graph, writer) == savedOwner, "rename changed stable partition identity");
    grh::ValueId live;
    for (const auto& port : graph.inputPorts()) if (port.name == "data") live = port.value;
    const auto released = graph.valueSymbol(live);
    graph.setValueSymbol(live, graph.internSymbol("renamed_data"));
    const auto replacement = graph.createValue(Fixture::type(8));
    graph.setValueSymbol(replacement, released);
    rejects([&] { sim::requireSinkSchedule(&fixture.session, graph); }, "symbol transfer reused resolved schedule");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    require(owner(graph, writer) == savedOwner, "symbol transfer changed partition identity");
    graph.eraseAttr(writer, sim::kPartition);
    require(!run(fixture.design, "grhsim-partition-sink", fixture.session, {}, false), "partly attributed graph accepted");
    rejects([&] { sim::requireSinkSchedule(&fixture.session, graph); }, "failed partition left a consumable schedule");
    graph.setAttr(writer, sim::kPartition, savedOwner);
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    fixture.session.clear();
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    require(sim::requireSinkSchedule(&fixture.session, graph).complete, "cleared scratchpad could not be rebuilt");
    auto restored = grh::Design::fromJsonString(checkpoint);
    run(restored, "grhsim-partition-sink", fixture.session);
    auto& restoredGraph = *restored.findGraph("top");
    require(sim::requireSinkSchedule(&fixture.session, restoredGraph).stamp.instance != initial.stamp.instance, "load kept old graph identity");
    const auto restoredSchedule = sim::requireSinkSchedule(&fixture.session, restoredGraph);
    restoredGraph.setMetadata("test.invalidates", true);
    require(!sim::verifySinkSchedule(restoredGraph, restoredSchedule).empty(), "metadata mutation reused schedule");
    // Same session can retain valid schedules for multiple designs/tops.
    require(sim::requireSinkSchedule(&fixture.session, graph).complete, "other design overwrote schedule slot");
}

void testMultidimensionalSamplingAndLatch() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto i = fixture.input("i", 2), j = fixture.input("j", 3), data = fixture.input("data", 8);
    const auto memory = fixture.state("memory", 64, {2, 4});
    const auto latch = fixture.state("latch", 8);
    fixture.graph.setOpKind(latch, OperationKind::kLatch);
    const auto partial = fixture.compute(OperationKind::kReplicate, Fixture::type(32), {data});
    fixture.graph.setAttr(fixture.graph.valueDef(partial), "rep", int64_t{4});
    const auto mask = fixture.constant(8, "8'h5a");
    fixture.write(OperationKind::kRegisterFillPort, memory, {enable, data, clock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {enable, i, partial, clock});
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {enable, i, j, data, mask, clock});
    const auto read = fixture.read(memory, {i, j});
    fixture.write(OperationKind::kLatchWritePort, latch, {enable, read, mask});
    fixture.graph.bindOutputPort("memory", fixture.read(memory));
    fixture.graph.bindOutputPort("latch", fixture.read(latch));
    fixture.graph.bindOutputPort("read", read);
    fixture.convert();
    const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=8"});
    fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    require(std::any_of(schedule.sinkGroups.begin(), schedule.sinkGroups.end(), [](const auto& group) { return !group.fire; }),
            "level-sensitive sink did not retain its distinct clock policy");
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned step = 0; step < 128; ++step)
        inputs.push_back({{"clock", step & 1}, {"enable", step % 5 != 0}, {"i", (step / 3) & 3},
            {"j", (step / 7) & 7}, {"data", (step * 17) & 255}});
    compare(reference, fixture.design, {"memory", "latch"}, inputs, "partial-array-latch", &schedule);
}

void testForgedGroupsAndEffectDependencies() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), data = fixture.input("data", 8);
    const auto one = fixture.constant(1, "1'h1");
    const auto q = fixture.state("q", 8), peer = fixture.state("peer", 8);
    const auto task = fixture.task("display", {one, data});
    const auto sample = fixture.compute(OperationKind::kSystemFunction, Fixture::type(32), {});
    fixture.graph.setAttr(fixture.graph.valueDef(sample), "name", std::string("random"));
    const auto low = fixture.compute(OperationKind::kSliceStatic, Fixture::type(8), {sample});
    fixture.graph.setAttr(fixture.graph.valueDef(low), "sliceStart", int64_t{0});
    fixture.graph.setAttr(fixture.graph.valueDef(low), "sliceEnd", int64_t{7});
    const auto first = fixture.write(OperationKind::kRegisterWritePort, q, {enable, data, clock});
    fixture.write(OperationKind::kRegisterWritePort, peer, {enable, low, clock});
    const auto last = fixture.write(OperationKind::kRegisterWritePort, q, {enable, low, clock});
    const auto lateProducer = fixture.compute(OperationKind::kNot, Fixture::type(8), {data});
    fixture.graph.replaceOperand(task, 1, lateProducer);
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-node-work=1", "-max-supernode-work=1", "-clone-shared=0"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    auto& graph = fixture.graph;
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    const auto samplePart = owner(graph, graph.valueDef(sample));
    Owner taskPart;
    for (const auto op : graph.operations()) if (graph.opKind(op) == sim::kSystemTask) taskPart = owner(graph, op);
    const auto& dependencies = schedule.generalRuns.back().requiredNonsinkKeys;
    const auto taskPosition = std::find(dependencies.begin(), dependencies.end(), taskPart);
    const auto samplePosition = std::find(dependencies.begin(), dependencies.end(), samplePart);
    require(taskPosition != dependencies.end() && samplePosition != dependencies.end() && taskPosition < samplePosition,
            "sink dependency closure lost effect-order prerequisites");
    compare(reference, fixture.design, {"q", "peer"}, {{{"clock", 0}, {"enable", 1}, {"data", 9}},
        {{"clock", 1}}, {{"clock", 0}}, {{"clock", 1}, {"enable", 0}}}, "mixed-effects", &schedule);
    const auto checkpoint = json(fixture.design);
    // All operands are ambient or nonsink; only the forged grouping is wrong.
    graph.setAttr(last, sim::kPartition, owner(graph, first));
    require(!run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"}, false),
            "group mixing writer priority ranks accepted");
    auto restored = grh::Design::fromJsonString(checkpoint);
    auto& restoredGraph = *restored.findGraph("top");
    restoredGraph.setAttr(first, "grhsim.hoistEnable", false);
    require(!run(restored, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"}, false),
            "group mixing local and hoisted guards accepted");
}

void testEqualPriorityDeclarationOrder() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto q = fixture.state("q", 8);
    const auto first = fixture.write(OperationKind::kRegisterWritePort, q, {enable, fixture.constant(8, "8'h11"), clock});
    const auto last = fixture.write(OperationKind::kRegisterWritePort, q, {enable, fixture.constant(8, "8'hee"), clock});
    fixture.graph.setOpSymbol(first, fixture.graph.internSymbol("z_first"));
    fixture.graph.setOpSymbol(last, fixture.graph.internSymbol("a_last"));
    fixture.graph.setAttr(first, "writerOrder", int64_t{0}); fixture.graph.setAttr(last, "writerOrder", int64_t{0});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {"q"}, {{{"clock", 0}, {"enable", 1}}, {{"clock", 1}}, {{"clock", 0}}, {{"clock", 1}}},
        "equal-priority", &schedule);
    const auto firstOwner = owner(fixture.graph, first), lastOwner = owner(fixture.graph, last);
    fixture.graph.setOpSymbol(first, fixture.graph.internSymbol("zz_renamed_first"));
    fixture.graph.setOpSymbol(last, fixture.graph.internSymbol("aa_renamed_last"));
    require(owner(fixture.graph, first) == firstOwner && owner(fixture.graph, last) == lastOwner, "writer rename changed partition keys");
    fixture.graph.freeze();
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& rebuilt = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {"q"}, {{{"clock", 0}, {"enable", 1}}, {{"clock", 1}}}, "equal-priority-freeze", &rebuilt);
}

void testSharedSinkConeAfterSimplify() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto q = fixture.state("q", 8);
    const auto a = fixture.constant(8, "8'h11"), b = fixture.constant(8, "8'h22");
    const auto first = fixture.write(OperationKind::kRegisterWritePort, q, {enable, a, clock});
    const auto second = fixture.write(OperationKind::kRegisterWritePort, q, {enable, b, clock});
    fixture.graph.bindOutputPort("q", fixture.read(q));
    fixture.convert();
    fixture.graph.setAttr(fixture.graph.valueDef(b), "constValue", std::string("8'h11"));
    const auto reference = fixture.design.clone();
    fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-simplify", fixture.session);
    require(fixture.graph.opOperands(first)[2] == fixture.graph.opOperands(second)[2],
            "fixture did not exercise CSE sharing within the broad sink region");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto firstData = fixture.graph.opOperands(first)[2], secondData = fixture.graph.opOperands(second)[2];
    require(firstData != secondData && owner(fixture.graph, fixture.graph.valueDef(firstData)) == owner(fixture.graph, first) &&
            owner(fixture.graph, fixture.graph.valueDef(secondData)) == owner(fixture.graph, second),
            "sink partitioning did not clone shared computation into final groups");
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {"q"}, {{{"clock", 0}, {"enable", 1}}, {{"clock", 1}},
        {{"clock", 0}}, {{"clock", 1}, {"enable", 0}}}, "sink-cse-clones", &schedule);
}

void testSharedRunDependencyClosure() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), input = fixture.input("input", 8);
    const auto one = fixture.constant(8, "8'd1");
    auto data = input;
    for (unsigned i = 0; i < 32; ++i) data = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, one});
    fixture.task("display", {enable, data}); // Retain the shared chain in General.
    for (unsigned i = 0; i < 128; ++i) {
        const auto state = fixture.state("state" + std::to_string(i), 8);
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, data, clock});
    }
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=8", "-clone-shared=0"});
    std::set<Owner> expected;
    for (const auto op : fixture.graph.operations()) {
        const auto path = owner(fixture.graph, op);
        if (path.size() == 4 && fixture.graph.opKind(op) == OperationKind::kAdd) expected.insert(path);
    }
    require(expected.size() > 8, "Shared dependency fixture lost its leaf chain");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    require(schedule.generalRuns.size() == 1 && schedule.generalRuns[0].orderedWriters.size() == 128,
            "Independent same-clock writers did not form one ordered run");
    const auto& keys = schedule.generalRuns[0].requiredNonsinkKeys;
    const std::set<Owner> unique(keys.begin(), keys.end());
    require(unique.size() == keys.size(), "Repeated writers duplicated run prerequisites");
    for (const auto& key : expected) require(unique.contains(key), "Shared run omitted a transitive leaf prerequisite");
}
}

int main() {
    try {
        testOrderedRunsAndSnapshots(); testLifecycleAndNegativeContracts();
        testMultidimensionalSamplingAndLatch(); testForgedGroupsAndEffectDependencies();
        testEqualPriorityDeclarationOrder();
        testSharedSinkConeAfterSimplify();
        testSharedRunDependencyClosure();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
