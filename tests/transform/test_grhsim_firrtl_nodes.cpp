#include "grhsim_partition_fixture.hpp"
#include "ingest/node_attributes.hpp"

namespace {
using namespace partition_test;
namespace node = wolvrix::lib::ingest::node;

std::string key(std::string name, std::vector<std::string> fields = {}, std::vector<std::string> instance = {}) {
    return node::key({"test", "top", "top", std::move(instance), std::move(name), std::move(fields)});
}
void tag(grh::Graph& graph, grh::OperationId op, std::vector<std::string> members) {
    node::write(graph, op, {members, {}, members});
}
uint64_t stat(const grh::Graph& graph, std::string_view key) {
    const auto values = std::get<std::vector<std::string>>(graph.metadata("grhsim.nonsinkHintStats").value());
    for (const auto& value : values) if (value.starts_with(std::string(key) + "=")) return std::stoull(value.substr(key.size() + 1));
    throw std::runtime_error("Missing statistic: " + std::string(key));
}
void capacity(const grh::Graph& graph, unsigned limit) {
    std::map<Owner, std::set<std::string>> nodes;
    for (const auto op : graph.operations()) {
        auto path = owner(graph, op);
        if (path.size() != 4 || path[1] != "nonsink") continue;
        const auto leaf = path.back(); path.resize(3); nodes[path].insert(leaf);
    }
    for (const auto& [path, members] : nodes) require(members.size() <= limit, "supernode exceeded node capacity");
}
std::vector<grh::OperationId> computation(const grh::Graph& graph) {
    std::vector<grh::OperationId> result;
    for (const auto op : graph.operations()) if (owner(graph, op) == Owner{"general", "nonsink"} &&
        graph.opKind(op) != OperationKind::kConstant && grh::operationEffect(graph, op) == grh::OpEffect::Pure) result.push_back(op);
    return result;
}
void chain(Fixture& fixture, unsigned count) {
    const auto x = fixture.input("x", 8), y = fixture.input("y", 8), enable = fixture.input("enable");
    auto value = x;
    for (unsigned i = 0; i < count; ++i)
        value = fixture.compute(i % 2 ? OperationKind::kAdd : OperationKind::kXor, Fixture::type(8), {value, y});
    fixture.task("display", {enable, value});
    fixture.convert(); fixture.split();
}

void largeNodes() {
    for (const unsigned length : {3u, 180u}) for (const bool hints : {false, true}) {
        Fixture fixture; chain(fixture, length);
        const auto reference = fixture.design.clone();
        const auto ops = computation(fixture.graph);
        require(ops.size() == length, "large chain lost operations");
        if (hints) {
            for (const auto op : ops) tag(fixture.graph, op, {key("large")});
            node::establishPolicy(fixture.graph);
        }
        const auto source = json(fixture.design);
        for (const std::string cost : {"legacy", "bytes"}) {
            auto candidate = grh::Design::fromJsonString(source); auto& graph = *candidate.findGraph("top");
            const auto model = "-cost-model=" + cost;
            run(candidate, "grhsim-partition-nonsink", fixture.session,
                {"-max-supernode-nodes=1", "-clone-shared=0", "-firrtl-nodes=auto", model});
            const auto first = owner(graph, ops.front());
            for (const auto op : ops) require(owner(graph, op) == first, "op count split an indivisible node");
            require(stat(graph, hints ? "firrtl-nodes" : "mffc-nodes") == (hints ? 1 : 2),
                "chain did not form one node: length=" + std::to_string(length) + " hints=" + std::to_string(hints) +
                " firrtl=" + std::to_string(stat(graph, "firrtl-nodes")) + " mffc=" + std::to_string(stat(graph, "mffc-nodes")));
            require(stat(graph, "node-max-work") >= length, "large node was cut by work"); capacity(graph, 1);
            require(graph.operations().size() == fixture.graph.operations().size(), "node builder created operations");
            run(candidate, "grhsim-partition-sink", fixture.session);
            const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
            compare(reference, candidate, {}, {{{"x", 3}, {"y", 5}, {"enable", 1}}, {{"x", 7}, {"y", 2}, {"enable", 0}}},
                "large-" + std::to_string(length) + (hints ? "-hint-" : "-mffc-") + cost, &schedule);
        }
    }
}

void nodeCountsAndReplay() {
    for (const unsigned count : {4u, 5u}) {
        Fixture fixture;
        const auto x = fixture.input("x", 8), y = fixture.input("y", 8);
        fixture.convert(); fixture.split();
        auto value = x;
        for (unsigned i = 0; i < count; ++i) {
            value = fixture.compute(i % 2 ? OperationKind::kAdd : OperationKind::kXor, Fixture::type(8), {value, y});
            fixture.graph.setAttr(fixture.graph.valueDef(value), sim::kPartition, Owner{"general", "nonsink"});
        }
        const auto ops = computation(fixture.graph);
        for (unsigned i = 0; i < ops.size(); ++i) tag(fixture.graph, ops[i], {key("n" + std::to_string(i))});
        node::establishPolicy(fixture.graph);
        const auto source = json(fixture.design);
        for (const std::string cost : {"legacy", "bytes"}) {
            auto candidate = grh::Design::fromJsonString(source); auto& graph = *candidate.findGraph("top");
            const auto model = "-cost-model=" + cost;
            run(candidate, "grhsim-partition-nonsink", fixture.session,
                {"-max-supernode-nodes=4", "-clone-shared=0", "-firrtl-nodes=prefer", model});
            require(stat(graph, "nodes") == count && stat(graph, "firrtl-nodes") == count, "source node count changed");
            require(stat(graph, "supernodes") == (count + 3) / 4, "K/K+1 capacity is not measured in nodes"); capacity(graph, 4);
            std::set<Owner> owners;
            for (const auto op : ops) owners.insert(owner(graph, op));
            require(owners.size() == count, "supernode merging fused node identities");
            const auto partitioned = json(candidate);
            const auto reportPath = artifact("node-count-report.txt"); const auto reportOption = "-report=" + reportPath.string();
            run(candidate, "grhsim-partition-nonsink", fixture.session,
                {"-max-supernode-nodes=4", "-clone-shared=0", "-firrtl-nodes=prefer", model, reportOption});
            require(json(candidate) == partitioned && std::filesystem::file_size(reportPath) > 0, "replay changed partition or lost report");
            require(!run(candidate, "grhsim-partition-nonsink", fixture.session, {"-max-supernode-nodes=3"}, false), "changed capacity resumed checkpoint");
        }
    }
    for (const std::string_view option : {"-max-node-work=1", "-max-supernode-work=1", "-fuse-leaves=0"}) {
        std::string error; const std::array args{option};
        require(!tx::makePass("grhsim-partition-nonsink", args, error) && error.find("Retired") != std::string::npos,
            "retired work/fusion option silently accepted");
    }
}

void groupingAndFallback() {
    Fixture fixture; chain(fixture, 7); const auto reference = fixture.design.clone();
    const auto ops = computation(fixture.graph);
    // Bundle fields belong to one source node; a different instance does not.
    tag(fixture.graph, ops[0], {key("bundle", {"a"})}); tag(fixture.graph, ops[1], {key("bundle", {"b"})});
    tag(fixture.graph, ops[2], {key("bundle", {}, {"other"})});
    node::establishPolicy(fixture.graph);
    const auto source = json(fixture.design);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=1", "-clone-shared=0", "-firrtl-nodes=prefer"});
    require(owner(fixture.graph, ops[0]) == owner(fixture.graph, ops[1]), "Bundle fields split source node");
    require(owner(fixture.graph, ops[1]) != owner(fixture.graph, ops[2]), "distinct instance nodes merged");
    for (unsigned i = 4; i < ops.size(); ++i) require(owner(fixture.graph, ops[i]) == owner(fixture.graph, ops[3]), "unannotated glue did not form MFFC");
    require(stat(fixture.graph, "firrtl-nodes") == 2 && stat(fixture.graph, "mffc-nodes") == 2, "mixed node sources missing");
    auto off = grh::Design::fromJsonString(source);
    run(off, "grhsim-partition-nonsink", fixture.session, {"-max-supernode-nodes=1", "-clone-shared=0", "-firrtl-nodes=off"});
    require(stat(*off.findGraph("top"), "firrtl-nodes") == 0 && stat(*off.findGraph("top"), "mffc-nodes") == 2, "off did not use MFFC");
    run(fixture.design, "grhsim-simplify", fixture.session); run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {}, {{{"x", 3}, {"y", 5}, {"enable", 1}}, {{"x", 7}, {"y", 2}, {"enable", 1}}}, "mixed-node-sources", &schedule);
}

void sharedAndNonconvex() {
    for (const bool cycle : {false, true}) {
        Fixture fixture; chain(fixture, 7); const auto reference = fixture.design.clone();
        const auto ops = computation(fixture.graph);
        if (cycle) {
            tag(fixture.graph, ops[0], {key("a")}); tag(fixture.graph, ops[2], {key("a")});
            tag(fixture.graph, ops[4], {key("valid")}); tag(fixture.graph, ops[5], {key("valid")});
        } else {
            tag(fixture.graph, ops[0], {key("a"), key("b")}); tag(fixture.graph, ops[1], {key("a"), key("b")});
            tag(fixture.graph, ops[2], {key("a")}); tag(fixture.graph, ops[3], {key("a")});
            tag(fixture.graph, ops[4], {key("b")}); tag(fixture.graph, ops[5], {key("b")});
        }
        node::establishPolicy(fixture.graph); const auto count = fixture.graph.operations().size();
        run(fixture.design, "grhsim-partition-nonsink", fixture.session,
            {"-max-supernode-nodes=1", "-clone-shared=0", "-firrtl-nodes=prefer"});
        require(fixture.graph.operations().size() == count, "source overlap cloned operations"); capacity(fixture.graph, 1);
        if (cycle) {
            require(stat(fixture.graph, "hint-conflict-groups") == 1, "nonconvex hint not diagnosed");
            require(owner(fixture.graph, ops[4]) == owner(fixture.graph, ops[5]), "unrelated valid hint lost during conflict fallback");
        } else {
            require(stat(fixture.graph, "shared-nodes") == 1, "shared associations lack unique owner");
            require(owner(fixture.graph, ops[0]) == owner(fixture.graph, ops[1]), "shared region fragmented");
            require(owner(fixture.graph, ops[1]) != owner(fixture.graph, ops[2]) && owner(fixture.graph, ops[3]) != owner(fixture.graph, ops[4]), "overlap caused transitive union");
        }
        run(fixture.design, "grhsim-simplify", fixture.session); run(fixture.design, "grhsim-partition-sink", fixture.session);
        const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
        compare(reference, fixture.design, {}, {{{"x", 3}, {"y", 5}, {"enable", 1}}, {{"x", 7}, {"y", 2}, {"enable", 1}}}, cycle ? "nonconvex-nodes" : "shared-nodes", &schedule);
    }
}

void reconvergentMffc() {
    Fixture fixture; const auto x = fixture.input("x", 8), y = fixture.input("y", 8), en = fixture.input("enable");
    const auto shared = fixture.compute(OperationKind::kXor, Fixture::type(8), {x, y});
    const auto left = fixture.compute(OperationKind::kAdd, Fixture::type(8), {shared, x});
    const auto right = fixture.compute(OperationKind::kAnd, Fixture::type(8), {shared, y});
    const auto root = fixture.compute(OperationKind::kOr, Fixture::type(8), {left, right});
    fixture.task("display", {en, root}); fixture.convert(); fixture.split();
    const auto source = json(fixture.design);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-supernode-nodes=1", "-clone-shared=0"});
    require(stat(fixture.graph, "mffc-nodes") == 2, "reconvergent fanout within cone split MFFC");
    auto external = grh::Design::fromJsonString(source); auto& graph = *external.findGraph("top");
    // A real fanout outside the cone must keep the shared producer separate.
    const auto sharedOp = graph.valueDef(shared); require(bool(sharedOp), "shared producer missing");
    const auto branch = graph.createOperation(OperationKind::kNot);
    graph.addOperand(branch, graph.opResults(sharedOp).front());
    graph.addResult(branch, graph.createValue(Fixture::type(8)));
    graph.setAttr(branch, sim::kPartition, Owner{"general", "nonsink"});
    run(external, "grhsim-partition-nonsink", fixture.session, {"-max-supernode-nodes=1", "-clone-shared=0"});
    require(stat(graph, "mffc-nodes") == 4 && owner(graph, sharedOp) != owner(graph, graph.valueDef(root)),
        "real shared fanout boundary was absorbed");
}

void cloneIntoExistingNode() {
    Fixture fixture; const auto x = fixture.input("x", 32), en = fixture.input("enable");
    const auto inverted = fixture.compute(OperationKind::kNot, Fixture::type(32), {x});
    const auto left = fixture.compute(OperationKind::kMul, Fixture::type(32), {inverted, x});
    const auto right = fixture.compute(OperationKind::kAdd, Fixture::type(32), {inverted, x});
    fixture.task("display", {en, left, right});
    fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
    for (const auto op : computation(fixture.graph)) tag(fixture.graph, op,
        {key(std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(op))))});
    node::establishPolicy(fixture.graph);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=1", "-firrtl-nodes=prefer", "-cost-model=bytes", "-max-clones=1"});
    require(stat(fixture.graph, "clone-ops") == 1 && stat(fixture.graph, "clone-extra-work") == 1,
        "profitable clone still constrained by op work");
    require(stat(fixture.graph, "nodes") == stat(fixture.graph, "firrtl-nodes") + stat(fixture.graph, "mffc-nodes") +
        stat(fixture.graph, "boundary-nodes"), "clone changed node count");
    capacity(fixture.graph, 1);
    run(fixture.design, "grhsim-simplify", fixture.session); run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {}, {{{"x", 3}, {"enable", 1}}, {{"x", 7}, {"enable", 1}},
        {{"x", 7}, {"enable", 0}}}, "node-count-clone", &schedule);
}

void arrayAndEffectBoundaries() {
    Fixture fixture;
    const auto x = fixture.graph.createValue(fixture.graph.internSymbol("x"), Fixture::type(32, {4}));
    const auto y = fixture.graph.createValue(fixture.graph.internSymbol("y"), Fixture::type(32, {4}));
    fixture.graph.bindInputPort("x", x); fixture.graph.bindInputPort("y", y);
    const auto en = fixture.input("enable");
    const auto a = fixture.compute(OperationKind::kXor, Fixture::type(32, {4}), {x, y});
    const auto b = fixture.compute(OperationKind::kAnd, Fixture::type(32, {4}), {a, y});
    const auto effect = fixture.task("display", {en, b});
    fixture.convert(); fixture.split();
    const auto ops = computation(fixture.graph);
    for (const auto op : ops) tag(fixture.graph, op, {key("array")});
    tag(fixture.graph, effect, {key("array")});
    node::establishPolicy(fixture.graph);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=1", "-clone-shared=0", "-firrtl-nodes=prefer"});
    require(ops.size() == 2 && owner(fixture.graph, ops[0]) == owner(fixture.graph, ops[1]), "array shape split source node");
    require(owner(fixture.graph, ops[0]) != owner(fixture.graph, effect) && stat(fixture.graph, "hint-boundary-ops") == 1,
        "source hint absorbed effect boundary");
    for (const auto op : ops) require(fixture.graph.valueLogicalType(fixture.graph.opResults(op).front()).shape ==
        std::vector<int64_t>{4}, "node construction flattened array shape");
    run(fixture.design, "grhsim-partition-sink", fixture.session);
}
}

int main() {
    try { largeNodes(); nodeCountsAndReplay(); groupingAndFallback(); sharedAndNonconvex(); reconvergentMffc(); cloneIntoExistingNode(); arrayAndEffectBoundaries(); }
    catch (const std::exception& error) { std::cerr << "[grhsim-firrtl-nodes] " << error.what() << '\n'; return 1; }
}
