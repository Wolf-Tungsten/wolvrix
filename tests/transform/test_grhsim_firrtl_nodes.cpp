#include "grhsim_partition_fixture.hpp"
#include "ingest/node_attributes.hpp"

namespace {
using namespace partition_test;
namespace node = wolvrix::lib::ingest::node;

std::string key(std::string name) { return node::key({"test", "top", "top", {}, std::move(name), {}}); }
void tag(grh::Graph& graph, grh::OperationId op, std::vector<std::string> members) {
    node::write(graph, op, {members, {}, members});
}
std::map<std::string, Owner> owners(const grh::Graph& graph) {
    std::map<std::string, Owner> result;
    for (const auto op : graph.operations()) result.emplace(graph.symbolText(graph.operationSymbol(op)), owner(graph, op));
    return result;
}
std::vector<std::string> stats(const grh::Graph& graph) {
    return std::get<std::vector<std::string>>(graph.metadata("grhsim.nonsinkHintStats").value());
}
void hasStat(const grh::Graph& graph, std::string expected) {
    const auto values = stats(graph);
    require(std::find(values.begin(), values.end(), expected) != values.end(), "Missing statistic: " + expected);
}
void budgets(const grh::Graph& graph, unsigned leafLimit, unsigned executionLimit) {
    std::map<Owner, unsigned> leaves, groups;
    for (const auto op : graph.operations()) {
        auto path = owner(graph, op);
        if (path.size() != 4 || path[1] != "nonsink") continue;
        ++leaves[path]; path.resize(3); ++groups[path];
    }
    for (const auto& [path, count] : leaves) require(count <= leafLimit, "leaf exceeded work budget");
    for (const auto& [path, count] : groups) require(count <= executionLimit, "supernode exceeded work budget");
}

void softHints() {
    Fixture fixture;
    const auto x = fixture.input("x", 8), y = fixture.input("y", 8), one = fixture.constant(1, "1'h1");
    auto value = x;
    for (unsigned i = 0; i < 8; ++i) value = fixture.compute(i % 2 ? OperationKind::kAnd : OperationKind::kXor, Fixture::type(8), {value, y});
    fixture.task("display", {one, value});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    const auto split = json(fixture.design);
    auto baseline = grh::Design::fromJsonString(split);
    run(baseline, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=0"});
    const auto baselineOwners = owners(*baseline.findGraph("top"));
    // Pick an actual baseline cut on a pure chain. Giving just these adjacent
    // ops a common association should change an equal-cost coarsening choice.
    bool changed = false;
    for (const auto op : fixture.graph.operations()) {
        if (!grh::canCloneOperation(fixture.graph, op) || owner(fixture.graph, op) != Owner{"general", "nonsink"}) continue;
        for (const auto input : fixture.graph.opOperands(op)) {
            const auto producer = fixture.graph.valueDef(input);
            if (!producer || owner(fixture.graph, producer) != Owner{"general", "nonsink"}) continue;
            const auto sourceName = std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(producer)));
            const auto targetName = std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(op)));
            if (baselineOwners.at(sourceName)[2] == baselineOwners.at(targetName)[2]) continue;
            auto annotated = grh::Design::fromJsonString(split); auto& graph = *annotated.findGraph("top");
            tag(graph, graph.findOperation(sourceName), {key("shared")});
            tag(graph, graph.findOperation(targetName), {key("shared"), key("overlap")});
            node::establishPolicy(graph);
            const auto sourceCheckpoint = json(annotated);
            auto off = grh::Design::fromJsonString(sourceCheckpoint);
            run(off, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=0", "-firrtl-nodes=off"});
            require(owners(*off.findGraph("top")) == baselineOwners, "off changed baseline owners when attrs were present");
            run(annotated, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=0", "-firrtl-nodes=prefer"});
            if (owners(graph) == baselineOwners) continue;
            changed = true; budgets(graph, 1, 2); hasStat(graph, "multi-member-ops=1");
            require(graph.operations().size() == fixture.graph.operations().size(), "hint created executable scaffolding");
            const auto partitioned = json(annotated);
            run(annotated, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=0", "-firrtl-nodes=prefer"});
            require(json(annotated) == partitioned, "hint partition was not idempotent");
            const auto reportPath = artifact("soft-hints-report.txt");
            const auto reportOption = "-report=" + reportPath.string();
            run(annotated, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2",
                "-clone-shared=0", "-firrtl-nodes=prefer", reportOption});
            std::ifstream report(reportPath);
            const std::string actualReport((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
            std::string expectedReport;
            for (const auto& entry : stats(graph)) expectedReport += entry + '\n';
            require(actualReport == expectedReport && json(annotated) == partitioned, "idempotent report changed snapshot");
            require(!run(annotated, "grhsim-partition-nonsink", fixture.session, {"-firrtl-nodes=off"}, false), "changed policy resumed completed checkpoint");
            run(annotated, "grhsim-simplify", fixture.session);
            run(annotated, "grhsim-partition-sink", fixture.session);
            const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
            std::vector<std::map<std::string, uint64_t>> inputs;
            for (unsigned i = 0; i < 96; ++i) inputs.push_back({{"x", i * 13}, {"y", i * 7}});
            compare(reference, annotated, {}, inputs, "firrtl-soft-hints", &schedule);
            break;
        }
        if (changed) break;
    }
    require(changed, "soft hints never influenced an equal-cost partition choice");

    // Rootless, nonconvex, fully overlapping sets are valid hints. Oversized
    // source nodes split normally; unannotated ops may share their execution.
    for (const auto op : fixture.graph.operations()) if (owner(fixture.graph, op) == Owner{"general", "nonsink"})
        tag(fixture.graph, op, {key("all"), key("overlapping")});
    node::establishPolicy(fixture.graph);
    const auto full = json(fixture.design);
    for (const std::string mode : {"off", "prefer", "auto"}) for (const std::string cost : {"legacy", "bytes"}) {
        auto candidate = grh::Design::fromJsonString(full); auto& graph = *candidate.findGraph("top");
        const auto hints = "-firrtl-nodes=" + mode, model = "-cost-model=" + cost;
        run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2", "-clone-shared=1", hints, model});
        budgets(graph, 1, 2); hasStat(graph, "split-hints=2");
        run(candidate, "grhsim-partition-sink", fixture.session);
        require(!sim::verifyGraph(graph).size(), "hint introduced a quotient cycle");
    }
    auto noHints = grh::Design::fromJsonString(split);
    run(noHints, "grhsim-partition-nonsink", fixture.session, {"-firrtl-nodes=auto"});
    hasStat(*noHints.findGraph("top"), "resolved=off");
}

void nonconvexHints() {
    Fixture fixture;
    const auto x = fixture.input("x", 8), y = fixture.input("y", 8), one = fixture.constant(1, "1'h1");
    auto value = x;
    for (unsigned i = 0; i < 6; ++i)
        value = fixture.compute(i % 2 ? OperationKind::kAdd : OperationKind::kXor, Fixture::type(8), {value, y});
    fixture.task("display", {one, value});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    // The middle of each source set lies outside that set, and their shared
    // member does not authorize a transitive must-link across this chain.
    unsigned position = 0;
    for (const auto op : fixture.graph.operations()) {
        if (owner(fixture.graph, op) != Owner{"general", "nonsink"} || !grh::canCloneOperation(fixture.graph, op)) continue;
        if (position == 0) tag(fixture.graph, op, {key("a")});
        if (position == 2) tag(fixture.graph, op, {key("a"), key("b")});
        if (position == 4) tag(fixture.graph, op, {key("b")});
        ++position;
    }
    require(position >= 5, "nonconvex fixture lost its chain");
    node::establishPolicy(fixture.graph);
    const auto split = json(fixture.design);
    for (const std::string cost : {"legacy", "bytes"}) {
        auto candidate = grh::Design::fromJsonString(split); auto& graph = *candidate.findGraph("top");
        const auto costOption = "-cost-model=" + cost;
        run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=2",
            "-clone-shared=0", "-firrtl-nodes=prefer", costOption});
        budgets(graph, 1, 2); hasStat(graph, "hint-sets=2"); hasStat(graph, "multi-member-ops=1");
        run(candidate, "grhsim-simplify", fixture.session); run(candidate, "grhsim-partition-sink", fixture.session);
        const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
        compare(reference, candidate, {}, {{{"x", 3}, {"y", 5}}, {{"x", 7}, {"y", 2}}}, "nonconvex-" + cost, &schedule);
    }
}

void fusionAndClones() {
    Fixture fixture;
    const auto x = fixture.input("x", 8), a = fixture.input("a", 8), b = fixture.input("b", 8);
    const auto one = fixture.constant(1, "1'h1");
    const auto inverted = fixture.compute(OperationKind::kNot, Fixture::type(8), {x});
    const auto left = fixture.compute(OperationKind::kAdd, Fixture::type(8), {inverted, a});
    const auto right = fixture.compute(OperationKind::kXor, Fixture::type(8), {inverted, b});
    fixture.task("display", {one, left}); fixture.task("display", {one, right});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    for (const auto op : fixture.graph.operations()) if (owner(fixture.graph, op) == Owner{"general", "nonsink"}) tag(fixture.graph, op, {key("same")});
    node::establishPolicy(fixture.graph);
    const auto split = json(fixture.design);
    for (bool fuse : {false, true}) {
        auto candidate = grh::Design::fromJsonString(split); auto& graph = *candidate.findGraph("top");
        run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=2", "-max-supernode-work=3",
            "-firrtl-nodes=prefer", fuse ? "-fuse-leaves=1" : "-fuse-leaves=0"});
        budgets(graph, 2, 3);
        run(candidate, "grhsim-simplify", fixture.session); run(candidate, "grhsim-partition-sink", fixture.session);
        const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
        compare(reference, candidate, {}, {{{"x", 5}, {"a", 7}, {"b", 9}}, {{"x", 0}, {"a", 5}, {"b", 2}}}, fuse ? "hint-fused" : "hint-leaves", &schedule);
    }
    auto bounded = grh::Design::fromJsonString(split);
    run(bounded, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=1", "-max-supernode-work=1", "-firrtl-nodes=prefer"});
    budgets(*bounded.findGraph("top"), 1, 1); hasStat(*bounded.findGraph("top"), "clone-ops=0");
}

void profitableClone() {
    Fixture fixture; const auto x = fixture.input("x", 32), y = fixture.input("y", 32);
    const auto one = fixture.constant(1, "1'h1");
    const auto inverted = fixture.compute(OperationKind::kNot, Fixture::type(32), {x});
    const auto left = fixture.compute(OperationKind::kMul, Fixture::type(32), {inverted, x});
    const auto right = fixture.compute(OperationKind::kMul, Fixture::type(32), {inverted, y});
    // Both destinations already depend on x, so copying NOT adds work but
    // removes a separate result boundary without adding an activation source.
    fixture.task("display", {one, left, x}); fixture.task("display", {one, right, x});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=6", "-max-supernode-work=6",
        "-cost-model=bytes", "-max-clones=1"});
    hasStat(fixture.graph, "clone-ops=1"); hasStat(fixture.graph, "clone-extra-work=1");
    run(fixture.design, "grhsim-simplify", fixture.session); run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {}, {{{"x", 3}, {"y", 5}}, {{"x", 7}, {"y", 2}}, {{"x", 3}, {"y", 9}}}, "hint-bounded-clone", &schedule);
}
}

int main() {
    try { softHints(); nonconvexHints(); fusionAndClones(); profitableClone(); }
    catch (const std::exception& error) { std::cerr << "[grhsim-firrtl-nodes] " << error.what() << '\n'; return 1; }
}
