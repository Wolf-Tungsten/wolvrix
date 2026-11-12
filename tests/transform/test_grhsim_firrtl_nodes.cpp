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

void semanticTreeNodes() {
    for (const bool anchors : {false, true}) {
        Fixture fixture; chain(fixture, 7);
        const auto ops = computation(fixture.graph);
        auto& tree = fixture.graph.getOrCreateSemanticTree("test", "top", "top");
        const auto declaration = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "bundle");
        const auto fieldA = tree.addNode(declaration, grh::SemanticNodeKind::Field, "a");
        const auto fieldB = tree.addNode(declaration, grh::SemanticNodeKind::Field, "b");
        const auto instance = tree.addNode(tree.root(), grh::SemanticNodeKind::Module, "other");
        const auto separate = tree.addNode(instance, grh::SemanticNodeKind::Expression, "bundle");
        for (unsigned i = 0; i < ops.size(); ++i) {
            const auto semantic = i < 6 ? (i % 2 ? fieldA : fieldB) : separate;
            fixture.graph.setOpSemanticIds(ops[i], std::array{semantic});
        }
        fixture.graph.setValueSemanticIds(fixture.graph.opResults(ops[5])[0], std::array{fieldA});
        run(fixture.design, "partition-nonsink", fixture.session,
            {"-max-supernode-nodes=1", "-clone-shared=0", anchors ? "-semantic-nodes=1" : "-semantic-nodes=0"});
        for (unsigned i = 1; i < 6; ++i)
            require(owner(fixture.graph, ops[0]) == owner(fixture.graph, ops[i]), "SemanticTree source expression split into op nodes");
        require(owner(fixture.graph, ops[5]) != owner(fixture.graph, ops[6]), "different semantic instance families merged");
        require(stat(fixture.graph, "semantic-covered-ops") == ops.size() && stat(fixture.graph, "firrtl-nodes") == 2,
            "auto mode did not consume SemanticTree families");
    }
}

void dagCensus() {
    Fixture fixture;
    const auto x = fixture.input("x", 8), y = fixture.input("y", 8), enable = fixture.input("enable");
    const auto a = fixture.compute(OperationKind::kXor, Fixture::type(8), {x, y});
    const auto a2 = fixture.compute(OperationKind::kAnd, Fixture::type(8), {x, y});
    const auto b = fixture.compute(OperationKind::kAdd, Fixture::type(8), {a, a});
    const auto c = fixture.compute(OperationKind::kOr, Fixture::type(8), {a, a2});
    const auto d = fixture.compute(OperationKind::kXor, Fixture::type(8), {b, c});
    fixture.task("display", {enable, d}); fixture.convert(); fixture.split();
    for (const auto value : {a, a2}) tag(fixture.graph, fixture.graph.valueDef(value), {key("a")});
    for (const auto value : {b, c, d}) tag(fixture.graph, fixture.graph.valueDef(value), {key(std::to_string(value.index))});
    node::establishPolicy(fixture.graph);
    run(fixture.design, "partition-nonsink", fixture.session, {"-max-supernode-nodes=1", "-clone-shared=0"});
    checkpoint(fixture.design, "dag-census");
    // Translation adds one constant fire operand to the task. Its explicit
    // node contributes one data edge and one structural value-target pair.
    require(stat(fixture.graph, "node-data-edges") == 6 && stat(fixture.graph, "supernode-data-edges") == 6,
        "DAG census counted repeated uses or boundary values as node pairs: node=" +
        std::to_string(stat(fixture.graph, "node-data-edges")) + " supernode=" +
        std::to_string(stat(fixture.graph, "supernode-data-edges")));
    require(stat(fixture.graph, "node-dependency-edges") == 6 && stat(fixture.graph, "supernode-dependency-edges") == 6,
        "dependency census changed pure data edges");
    require(stat(fixture.graph, "nonsink-internal-boundary-value-targets") == 7 &&
            stat(fixture.graph, "nonsink-external-boundary-value-targets") == 3 &&
            stat(fixture.graph, "nonsink-boundary-value-targets") == 10,
        "boundary census did not deduplicate (value, target supernode)");
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
    for (const std::string_view name : {"grhsim-dialect-translate", "split-event-and-output",
                                        "split-nonsink-and-sink", "partition-nonsink",
                                        "partition-sink", "comm-expr"}) {
        std::string error;
        require(static_cast<bool>(tx::makePass(name, {}, error)),
            "canonical F24 pass name is not registered: " + std::string(name));
    }
}

void coarseningOrderGolden() {
    const auto execution = [](const grh::Graph& graph, grh::OperationId op) {
        auto path = owner(graph, op);
        require(path.size() == 4, "golden partition lost its node owner");
        path.resize(3);
        return path;
    };
    const auto separateNodes = [](Fixture& fixture, const std::vector<grh::OperationId>& ops) {
        for (unsigned i = 0; i < ops.size(); ++i) {
            fixture.graph.setAttr(ops[i], sim::kPartition, Owner{"general", "nonsink"});
            tag(fixture.graph, ops[i], {key("golden-" + std::to_string(i))});
        }
        node::establishPolicy(fixture.graph);
    };
    {
        Fixture fixture;
        const auto x = fixture.input("x", 8), y = fixture.input("y", 8);
        fixture.convert(); fixture.split();
        const auto root = fixture.compute(OperationKind::kXor, Fixture::type(8), {x, y});
        std::vector<grh::OperationId> ops{fixture.graph.valueDef(root)};
        for (unsigned i = 0; i < 5; ++i) {
            const auto value = fixture.compute(OperationKind::kAdd, Fixture::type(8), {root, x});
            ops.push_back(fixture.graph.valueDef(value));
        }
        separateNodes(fixture, ops);
        run(fixture.design, "partition-nonsink", fixture.session,
            {"-max-supernode-nodes=2", "-clone-shared=0", "-firrtl-nodes=prefer"});
        // Equal links merge the first child with root. The remaining equal
        // predecessor groups fill consecutive pairs; a full anchor advances.
        for (unsigned i = 0; i < ops.size(); ++i) {
            const auto first = ops[(i / 2) * 2];
            require(execution(fixture.graph, ops[i]) == execution(fixture.graph, first),
                "same-predecessor capacity changed the golden pairs");
            require(execution(fixture.graph, ops[i]).back() ==
                "sn_" + std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(first))),
                "same-predecessor merging changed first-member order");
        }
        require(stat(fixture.graph, "nodes") == 6 && stat(fixture.graph, "supernodes") == 3,
            "same-predecessor capacity changed the golden group count");
        capacity(fixture.graph, 2);
    }
    {
        Fixture fixture;
        const auto x = fixture.input("x", 8), y = fixture.input("y", 8);
        fixture.convert(); fixture.split();
        const auto a = fixture.compute(OperationKind::kXor, Fixture::type(8), {x, y});
        const auto b = fixture.compute(OperationKind::kAnd, Fixture::type(8), {x, y});
        const auto c = fixture.compute(OperationKind::kAdd, Fixture::type(8), {a, b});
        const std::vector<grh::OperationId> ops{
            fixture.graph.valueDef(a), fixture.graph.valueDef(b), fixture.graph.valueDef(c)};
        separateNodes(fixture, ops);
        run(fixture.design, "partition-nonsink", fixture.session,
            {"-max-supernode-nodes=2", "-clone-shared=0", "-firrtl-nodes=prefer"});
        // Pair order chooses A/C before B/C; quotient ordering must then put
        // B before A/C without changing the first member of the A/C cluster.
        require(execution(fixture.graph, ops[0]) == execution(fixture.graph, ops[2]) &&
                execution(fixture.graph, ops[0]) != execution(fixture.graph, ops[1]),
            "equal-weight link order changed the golden merge");
        require(execution(fixture.graph, ops[0]).back() ==
            "sn_" + std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(ops[0]))),
            "quotient reordering changed merged member order");
        require(stat(fixture.graph, "nodes") == 3 && stat(fixture.graph, "supernodes") == 2,
            "quotient reordering changed golden group count");
        capacity(fixture.graph, 2);
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
    run(fixture.design, "grhsim-partition-sink", fixture.session);
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
        run(fixture.design, "grhsim-partition-sink", fixture.session);
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
    auto& tree = fixture.graph.getOrCreateSemanticTree("test", "top", "top");
    for (const auto op : computation(fixture.graph)) {
        const auto name = std::string(fixture.graph.symbolText(fixture.graph.operationSymbol(op)));
        tag(fixture.graph, op, {key(name)});
        const auto source = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, name);
        fixture.graph.setOpSemanticIds(op, std::array{source});
        fixture.graph.setValueSemanticIds(fixture.graph.opResults(op)[0], std::array{source});
    }
    node::establishPolicy(fixture.graph);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=1", "-firrtl-nodes=prefer", "-cost-model=bytes", "-max-clones=1"});
    require(stat(fixture.graph, "clone-ops") == 1 && stat(fixture.graph, "clone-extra-work") == 1,
        "profitable clone still constrained by op work");
    require(stat(fixture.graph, "nodes") == stat(fixture.graph, "firrtl-nodes") + stat(fixture.graph, "mffc-nodes") +
        stat(fixture.graph, "boundary-nodes"), "clone changed node count");
    capacity(fixture.graph, 1);
    for (const auto op : fixture.graph.operations())
        if (sim::attribute<bool>(fixture.graph, op, "grhsim.nodeClone").value_or(false)) {
            require(!fixture.graph.opSemanticIds(op).empty(), "partition clone lost operation SemanticId");
            for (const auto value : fixture.graph.opResults(op))
                require(!fixture.graph.valueSemanticIds(value).empty(), "partition clone lost value SemanticId");
        }
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, fixture.graph);
    compare(reference, fixture.design, {}, {{{"x", 3}, {"enable", 1}}, {{"x", 7}, {"enable", 1}},
        {{"x", 7}, {"enable", 0}}}, "node-count-clone", &schedule);
}

void clockDomainIsolation() {
    for (const std::string association : {"off", "legacy", "semantic"}) {
        Fixture fixture; auto& graph = fixture.graph;
        const auto clockA = fixture.input("clock_a"), clockB = fixture.input("clock_b");
        const auto data = fixture.input("data", 8), enable = fixture.input("enable");
        const auto index = fixture.input("index", 2);
        const auto state = fixture.state("delayed_data", 8);
        const auto memory = fixture.state("data_array", 32, {4});
        const auto read = fixture.read(state), arrayRead = fixture.read(memory, {index});
        const auto invertedClock = fixture.compute(OperationKind::kNot, Fixture::type(1), {clockA});
        const auto invertedData = fixture.compute(OperationKind::kNot, Fixture::type(8), {data});
        const auto plain = fixture.compute(OperationKind::kAdd, Fixture::type(8), {read, arrayRead});
        const auto selected = fixture.compute(OperationKind::kMux, Fixture::type(8), {clockA, invertedData, data});
        const auto first = fixture.task("display", {enable, selected, plain, clockA, clockB}, {"posedge", "posedge"});
        const auto second = fixture.task("display", {enable, selected, clockA, clockB}, {"posedge", "posedge"});
        const auto coldEffect = fixture.task("display", {enable, plain, invertedData});
        fixture.task("display", {enable, invertedClock});
        // Reading a state written by a clock must remain a data dependency.
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, data, clockA});
        fixture.convert();
        const auto fire = graph.opOperands(first)[0];
        require(graph.opKind(graph.valueDef(fire)) == OperationKind::kOr,
                "Clock isolation lost its multi-event fire join");
        const auto alias = fixture.compute(OperationKind::kAssign, Fixture::type(1), {fire});
        graph.replaceOperand(first, 0, alias); graph.replaceOperand(second, 0, alias);
        const auto reference = fixture.design.clone();
        fixture.split();
        const std::array sameFamily{graph.valueDef(invertedClock), graph.valueDef(plain), graph.valueDef(alias)};
        if (association == "legacy") {
            for (const auto op : sameFamily) tag(graph, op, {key("mixed_clock_and_data")});
            tag(graph, graph.valueDef(invertedData), {key("plain_transform")});
            node::establishPolicy(graph);
        } else if (association == "semantic") {
            auto& tree = graph.getOrCreateSemanticTree("test", "top", "top");
            const auto family = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "mixed_clock_and_data");
            for (const auto op : sameFamily) graph.setOpSemanticIds(op, std::array{family});
            const auto separate = tree.addNode(tree.root(), grh::SemanticNodeKind::Expression, "plain_transform");
            graph.setOpSemanticIds(graph.valueDef(invertedData), std::array{separate});
        }
        const auto source = json(fixture.design);
        for (const std::string cost : {"legacy", "bytes"}) for (const bool clone : {false, true}) {
            auto candidate = grh::Design::fromJsonString(source); auto& partitioned = *candidate.findGraph("top");
            const auto execution = [&](grh::OperationId op) {
                auto path = owner(partitioned, op);
                require(path.size() == 4 && path[1] == "nonsink", "Clock isolation lost a nonsink operation");
                path.resize(3); return path;
            };
            const auto costOption = "-cost-model=" + cost;
            const auto hintOption = std::string("-firrtl-nodes=") + (association == "off" ? "off" : "prefer");
            run(candidate, "grhsim-partition-nonsink", fixture.session,
                {"-max-supernode-nodes=128", costOption, hintOption, clone ? "-clone-shared=1" : "-clone-shared=0"});
            const auto clockOwner = execution(partitioned.valueDef(invertedClock));
            const auto fireOwner = execution(partitioned.valueDef(alias));
            const auto plainOwner = execution(partitioned.valueDef(plain));
            require(clockOwner != plainOwner && fireOwner != plainOwner,
                    "Clock/event source families or packing dragged unrelated data into the same owner");
            require(execution(partitioned.valueDef(read)) != clockOwner &&
                    execution(partitioned.valueDef(arrayRead)) != fireOwner,
                    "State reads inherited a writer's clock domain");
            require(execution(coldEffect) != clockOwner && execution(coldEffect) != fireOwner,
                    "Effect-order scheduling arcs propagated the clock domain");
            for (const auto op : partitioned.operations())
                if (partitioned.opKind(op) == OperationKind::kNot && partitioned.opOperands(op)[0] == data)
                    require(execution(op) != execution(partitioned.valueDef(selected)),
                            "Boundary cloning put a plain data transform back in a clock owner");
            require(stat(partitioned, "clock-roots") >= 4 && stat(partitioned, "clock-dependent-ops") >= 5 &&
                    stat(partitioned, "clock-dependent-supernodes") > 0,
                    "Clock isolation lacks its structural domain census");
            if (clone) require(stat(partitioned, "clone-skip-clock") > 0,
                               "Clock isolation fixture did not exercise a rejected cross-domain clone");
            capacity(partitioned, 128);
            const auto checkpoint = json(candidate);
            run(candidate, "grhsim-partition-nonsink", fixture.session,
                {"-max-supernode-nodes=128", costOption, hintOption, clone ? "-clone-shared=1" : "-clone-shared=0"});
            require(json(candidate) == checkpoint, "Clock isolation replay changed the partition");
            run(candidate, "grhsim-partition-sink", fixture.session);
            const auto& schedule = sim::requireSinkSchedule(&fixture.session, partitioned);
            compare(reference, candidate, {}, {
                {{"clock_a", 0}, {"clock_b", 0}, {"data", 3}, {"enable", 1}, {"index", 0}},
                {{"clock_a", 1}, {"clock_b", 0}, {"data", 3}, {"enable", 1}, {"index", 1}},
                {{"clock_a", 0}, {"clock_b", 1}, {"data", 7}, {"enable", 1}, {"index", 2}},
                {{"clock_a", 1}, {"clock_b", 1}, {"data", 7}, {"enable", 0}, {"index", 3}}},
                "clock-isolation-" + association + "-" + cost + (clone ? "-clone" : "-no-clone"), &schedule);
        }
    }
}

void systemTaskCapacity() {
    const auto execution = [](const grh::Graph& graph, grh::OperationId op) {
        auto path = owner(graph, op);
        require(path.size() == 4 && path[1] == "nonsink", "Task capacity lost an execution owner");
        path.resize(3); return path;
    };
    const auto weightedCapacity = [](const grh::Graph& graph, unsigned limit) {
        std::map<Owner, uint64_t> taskArgs;
        for (const auto op : graph.operations()) {
            const auto path = owner(graph, op);
            if (path.size() != 4 || path[1] != "nonsink") continue;
            auto& count = taskArgs[path];
            if (graph.opKind(op) == sim::kSystemTask)
                count += graph.opOperands(op).size() - 2 + bool(graph.getOperation(op).attr(sim::kLiteralFormat));
        }
        std::map<Owner, uint64_t> capacities;
        for (const auto& [node, args] : taskArgs) {
            auto execution = node; execution.resize(3);
            capacities[execution] += std::min<uint64_t>(limit, std::max<uint64_t>(1, args / 16 + (args % 16 != 0)));
        }
        for (const auto& [execution, count] : capacities)
            require(count <= limit, "SystemTask expansion exceeded weighted capacity");
        capacity(graph, limit);
    };
    {
        Fixture fixture; auto& graph = fixture.graph;
        const auto enable = fixture.input("enable"), data = fixture.input("data", 8);
        const std::array<unsigned, 5> counts{0, 16, 17, 33, 257};
        std::vector<grh::OperationId> tasks;
        for (const auto count : counts) {
            const auto task = fixture.task("display", {enable});
            for (unsigned i = 0; i < count; ++i) graph.addOperand(task, data);
            tasks.push_back(task);
        }
        fixture.convert(); const auto reference = fixture.design.clone(); fixture.split();
        const auto source = json(fixture.design);
        const auto operationCount = graph.operations().size();
        for (const std::string cost : {"legacy", "bytes"}) {
            auto candidate = grh::Design::fromJsonString(source); auto& partitioned = *candidate.findGraph("top");
            const auto model = "-cost-model=" + cost;
            run(candidate, "partition-nonsink", fixture.session,
                {"-max-supernode-nodes=4", "-clone-shared=0", model});
            require(partitioned.operations().size() == operationCount, "Task budget changed the operation inventory");
            require(stat(partitioned, "nodes") == 6 && stat(partitioned, "task-nodes") == 5 &&
                    stat(partitioned, "task-args") == 323 && stat(partitioned, "task-weighted-nodes") == 3 &&
                    stat(partitioned, "task-clamped-nodes") == 1 && stat(partitioned, "node-max-task-args") == 257 &&
                    stat(partitioned, "node-capacity-units") == 12 && stat(partitioned, "supernode-max-capacity") == 4,
                    "Task argument boundary or actual-node census changed");
            weightedCapacity(partitioned, 4);
            for (unsigned i = 0; i < tasks.size(); ++i) {
                require(partitioned.opOperands(tasks[i]).size() == counts[i] + 2,
                        "Task budget changed normalized operands");
                if (i + 1 < tasks.size())
                    require(execution(partitioned, tasks[i]) != execution(partitioned, tasks.back()),
                            "Oversized task was not an indivisible exclusive node");
            }
            if (cost == "legacy") require(stat(partitioned, "coarsen-mode0-accepted") > 0,
                                           "Task budget did not exercise chain coarsening");
            else require(stat(partitioned, "coarsen-mode0-accepted") == 0 &&
                         stat(partitioned, "coarsen-mode1-accepted") == 0 &&
                         stat(partitioned, "coarsen-mode2-accepted") == 0,
                         "Bytes task budget unexpectedly used greedy coarsening");
            const auto checkpoint = json(candidate);
            run(candidate, "partition-nonsink", fixture.session,
                {"-max-supernode-nodes=4", "-clone-shared=0", model});
            require(json(candidate) == checkpoint, "Task capacity replay changed its checkpoint");
            auto old = grh::Design::fromJsonString(checkpoint);
            old.findGraph("top")->setMetadata("grhsim.nonsinkConfig", std::string("v5/task-budget-absent"));
            require(!run(old, "partition-nonsink", fixture.session,
                {"-max-supernode-nodes=4", "-clone-shared=0", model}, false), "Old task-free configuration resumed");
            run(candidate, "partition-sink", fixture.session);
            const auto& schedule = sim::requireSinkSchedule(&fixture.session, partitioned);
            compare(reference, candidate, {}, {{{"enable", 1}, {"data", 3}},
                {{"enable", 0}, {"data", 7}}, {{"enable", 1}, {"data", 5}}}, "task-capacity-" + cost, &schedule);
        }
    }
    {
        // The sole task keeps its original effect order. Pure siblings share
        // its fire producer, so mode 1 fills that producer and mode 2 must
        // account for the task's weight when merging the remaining siblings.
        Fixture fixture; auto& graph = fixture.graph;
        const auto enable = fixture.input("enable"), data = fixture.input("data", 8);
        const auto task = fixture.task("display", {enable});
        for (unsigned i = 0; i < 33; ++i) graph.addOperand(task, data);
        fixture.convert(); fixture.split();
        const auto fire = graph.opOperands(task)[0];
        std::vector<grh::OperationId> pure{graph.valueDef(fire)};
        for (unsigned i = 0; i < 4; ++i) {
            const auto value = fixture.compute(OperationKind::kNot, Fixture::type(1), {fire});
            graph.setAttr(graph.valueDef(value), sim::kPartition, Owner{"general", "nonsink"});
            pure.push_back(graph.valueDef(value));
        }
        const auto reference = fixture.design.clone();
        for (unsigned i = 0; i < pure.size(); ++i) tag(graph, pure[i], {key("task-sibling-" + std::to_string(i))});
        node::establishPolicy(graph);
        run(fixture.design, "partition-nonsink", fixture.session,
            {"-max-supernode-nodes=4", "-clone-shared=0", "-firrtl-nodes=prefer"});
        require(stat(graph, "coarsen-mode1-accepted") > 0 && stat(graph, "coarsen-mode2-accepted") > 0,
                "Task capacity did not exercise input and sibling coarsening");
        require(stat(graph, "nodes") == 6 && stat(graph, "task-weighted-nodes") == 1 &&
                stat(graph, "node-capacity-units") == 8, "Sibling coarsening changed activity nodes");
        weightedCapacity(graph, 4);
        require(execution(graph, task) != execution(graph, pure.front()), "Task overflowed its full predecessor cluster");
        run(fixture.design, "partition-sink", fixture.session);
        const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
        compare(reference, fixture.design, {}, {{{"enable", 1}, {"data", 3}},
            {{"enable", 0}, {"data", 7}}}, "task-capacity-siblings", &schedule);
    }
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
    try { largeNodes(); semanticTreeNodes(); dagCensus(); nodeCountsAndReplay(); coarseningOrderGolden(); groupingAndFallback(); sharedAndNonconvex(); reconvergentMffc(); cloneIntoExistingNode(); clockDomainIsolation(); systemTaskCapacity(); arrayAndEffectBoundaries(); }
    catch (const std::exception& error) { std::cerr << "[grhsim-firrtl-nodes] " << error.what() << '\n'; return 1; }
}
