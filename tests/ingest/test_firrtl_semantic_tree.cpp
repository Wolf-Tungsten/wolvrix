#include "firrtl_test_fixture.hpp"
#include "core/load.hpp"
#include "core/store.hpp"
#include "transform/hier_flatten.hpp"

#include <fstream>

namespace {
using namespace firrtl_test;

bool hasPath(const grh::Graph& graph, std::span<const grh::SemanticId> ids,
             const std::vector<std::string>& path) {
    return std::any_of(ids.begin(), ids.end(), [&](grh::SemanticId id) {
        return graph.semanticTree()->path(id) == path;
    });
}

std::size_t countValues(const grh::Graph& graph, const std::vector<std::string>& path) {
    return std::count_if(graph.values().begin(), graph.values().end(), [&](grh::ValueId value) {
        return hasPath(graph, graph.valueSemanticIds(value), path);
    });
}

std::size_t countOperations(const grh::Graph& graph, const std::vector<std::string>& path) {
    return std::count_if(graph.operations().begin(), graph.operations().end(), [&](grh::OperationId op) {
        return hasPath(graph, graph.opSemanticIds(op), path);
    });
}

grh::Design roundtrip(const grh::Design& design) {
    wolvrix::lib::store::StoreOptions options;
    options.jsonMode = wolvrix::lib::store::JsonPrintMode::PrettyCompact;
    const auto json = wolvrix::lib::store::StoreJson().storeToString(design, options);
    require(json.has_value(), "failed to store SemanticTree checkpoint");
    auto restored = wolvrix::lib::load::LoadJson().load(*json);
    const auto second = wolvrix::lib::store::StoreJson().storeToString(restored, options);
    require(second && *second == *json, "SemanticTree checkpoint is not byte-stable");
    return restored;
}

void requireCovered(const grh::Graph& graph);

void checkFlattened(grh::Design& design, bool globalPlan) {
    tx::HierFlattenOptions options;
    options.useGlobalPlan = globalPlan;
    require(node_test::run(design, std::make_unique<tx::HierFlattenPass>(options)),
            globalPlan ? "global flatten failed" : "legacy flatten failed");
    require(design.graphs().size() == 1, "flatten did not delete the source definition");
    const auto& top = *design.findGraph("top");
    require(top.semanticTree(), "flatten lost SemanticTree");
    requireCovered(top);
    require(countValues(top, {"top", "c0", "n"}) == 1 &&
            countValues(top, {"top", "c1", "n"}) == 1,
            "deep-copied instances did not retain independent instance paths");
    require(countOperations(top, {"top", "c0", "n"}) >= 1 &&
            countOperations(top, {"top", "c1", "n"}) >= 1,
            "flatten did not remap operation sources independently per copy");
    require(countValues(top, {"top", "c0", "l0", "inner"}) == 1 &&
            countValues(top, {"top", "c1", "l0", "inner"}) == 1,
            "nested flatten did not preserve the complete instance path");
    require(countValues(top, {"top", "a0"}) == 1 && countValues(top, {"top", "a1"}) == 1,
            "top input sources disappeared");
    require(countValues(top, {"top", "c0", "a"}) == 1 &&
            countValues(top, {"top", "c1", "a"}) == 1,
            "child input sources disappeared from their boundary values");
    const bool mergedPortSources = std::any_of(top.values().begin(), top.values().end(), [&](grh::ValueId value) {
        const auto ids = top.valueSemanticIds(value);
        return ids.size() > 1 && hasPath(top, ids, {"top", "c0", "a"});
    });
    require(mergedPortSources, "port aliases did not retain multiple direct source associations");
    require(countValues(top, {"top", "w", "bits", "data"}) == 1,
            "bundle field path changed during flatten");
    for (const auto op : top.operations()) {
        for (const auto attr : top.opAttrRange(op)) {
            require(!node::isAttribute(attr.key()) && attr.key() != "originModules",
                    "flatten emitted a legacy source attribute");
        }
    }
    auto restored = roundtrip(design);
    const auto& restoredTop = *restored.findGraph("top");
    require(countValues(restoredTop, {"top", "c0", "l0", "inner"}) == 1 &&
            countValues(restoredTop, {"top", "c1", "l0", "inner"}) == 1 &&
            countOperations(restoredTop, {"top", "c0", "n"}) >= 1 &&
            countOperations(restoredTop, {"top", "c1", "n"}) >= 1,
            "JSON roundtrip lost flattened SemanticId associations");
}

void requireCovered(const grh::Graph& graph) {
    require(graph.semanticTree(), "SemanticId coverage requires a SemanticTree");
    for (const auto op : graph.operations()) {
        const auto ids = graph.opSemanticIds(op);
        require(!ids.empty(), "operation has no SemanticId: " + std::string(grh::toString(graph.opKind(op))));
        for (const auto id : ids) require(graph.semanticTree()->contains(id), "operation has an invalid SemanticId");
    }
}

void checkMemoryAnnotations() {
    const auto path = artifact("firrtl-semantic-tree") / "memory.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module child :
    input clock : Clock
    input addr : UInt<2>
    output y : UInt<8>
    smem mem : UInt<8>[4]
    read mport r = mem[addr], clock
    connect y, r
  public module top :
    input clock : Clock
    input addr : UInt<2>
    output y0 : UInt<8>
    output y1 : UInt<8>
    inst c0 of child
    inst c1 of child
    connect c0.clock, clock
    connect c1.clock, clock
    connect c0.addr, addr
    connect c1.addr, addr
    connect y0, c0.y
    connect y1, c1.y
)FIR";
    auto design = firrtl_test::design(path);
    const auto& child = *design.findGraph("child");
    requireCovered(child);
    require(!child.findValue("mem").valid(), "SemanticTree-only memory declaration created an ordinary value");
    std::size_t storage = 0, pipelines = 0;
    for (const auto op : child.operations()) {
        bool memory = false, pipeline = false;
        for (const auto attr : child.opAttrRange(op)) {
            memory |= attr.key() == "firrtl.memory";
            pipeline |= attr.key() == "firrtl.memoryPipeline";
        }
        if (memory) {
            ++storage;
            require(hasPath(child, child.opSemanticIds(op), {"child", "mem"}),
                    "memory storage is not attributed to the memory declaration");
        }
        if (pipeline) {
            ++pipelines;
            require(hasPath(child, child.opSemanticIds(op), {"child", "r"}),
                    "memory pipeline is not attributed to its read port");
        }
    }
    require(storage == 1 && pipelines > 0, "synchronous read fixture did not create storage and pipeline state");
    auto restored = roundtrip(design);
    requireCovered(*restored.findGraph("child"));
    tx::HierFlattenOptions options;
    options.useGlobalPlan = true;
    require(node_test::run(restored, std::make_unique<tx::HierFlattenPass>(options)), "memory flatten failed");
    const auto& top = *restored.findGraph("top");
    requireCovered(top);
    require(countOperations(top, {"top", "c0", "mem"}) >= 1 &&
            countOperations(top, {"top", "c1", "mem"}) >= 1 &&
            countOperations(top, {"top", "c0", "r"}) >= 1 &&
            countOperations(top, {"top", "c1", "r"}) >= 1,
            "flatten did not preserve independent memory and mport paths");
}

void checkAggregateMemoryAnnotations() {
    const auto path = artifact("firrtl-semantic-tree") / "memory-aggregate.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clk : Clock
    input addr : UInt<2>
    input wa : UInt<1>
    input wb : UInt<1>
    input a : UInt<8>
    input b : UInt<8>
    output pa : UInt<8>
    output pb : UInt<8>
    output qa : UInt<8>
    output qb : UInt<8>
    cmem m : {a : UInt<8>, b : UInt<8>}[3]
    rdwr mport p = m[addr], clk
    read mport q = m[addr], clk
    when wa :
      connect p.a, a
    when wb :
      connect p.b, b
    connect pa, p.a
    connect pb, p.b
    connect qa, q.a
    connect qb, q.b
)FIR";
    auto design = firrtl_test::design(path);
    const auto& top = *design.findGraph("top");
    requireCovered(top);
    require(countOperations(top, {"top", "m", "a"}) >= 1 &&
            countOperations(top, {"top", "m", "b"}) >= 1,
            "aggregate memory leaves do not have independent storage sources");
    bool sharedPortLeaves = false;
    for (const auto op : top.operations()) {
        const auto ids = top.opSemanticIds(op);
        sharedPortLeaves |= hasPath(top, ids, {"top", "p", "a"}) && hasPath(top, ids, {"top", "p", "b"});
    }
    require(sharedPortLeaves, "shared rdwr mode logic does not carry every aggregate port leaf");
    auto restored = roundtrip(design);
    requireCovered(*restored.findGraph("top"));
}

void checkGeneratedAnnotations() {
    const auto path = artifact("firrtl-semantic-tree") / "generated.fir";
    std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input reset : UInt<1>
    input cond : UInt<1>
    input a : UInt<8>
    output y0 : UInt<8>
    output y1 : UInt<8>
    regreset r : UInt<8>, clock, reset, UInt<8>(0)
    wire w0 : UInt<8>
    wire w1 : UInt<8>
    connect w0, a
    connect w1, a
    when cond :
      connect w0, r
      connect w1, r
      connect r, a
    connect y0, w0
    connect y1, w1
    printf(clock, cond, "r=%x", r)
    assert(clock, cond, cond, "unexpected r")
)FIR";
    auto design = firrtl_test::design(path);
    const auto& top = *design.findGraph("top");
    requireCovered(top);
    std::size_t guardOperations = 0, effectOperations = 0, registerWrites = 0;
    for (const auto op : top.operations()) {
        for (const auto id : top.opSemanticIds(op)) {
            const auto path = top.semanticTree()->path(id);
            if (path.size() < 2 || path.front() != "top") continue;
            if (path[1].starts_with("guard_")) {
                ++guardOperations;
                require(path[1] != "w0" && path[1] != "w1", "shared guard was attributed to its first driver");
            }
            if (path[1].starts_with("effect@")) ++effectOperations;
        }
        if (!grh::isRegisterWritePortKind(top.opKind(op))) continue;
        for (const auto attr : top.opAttrRange(op)) {
            if (attr.key() != "regSymbol") continue;
            const auto symbol = attr.value().get<std::string>();
            if (symbol && *symbol == "r") {
                ++registerWrites;
                require(hasPath(top, top.opSemanticIds(op), {"top", "r"}),
                        "register write is not attributed to the written register");
            }
        }
    }
    require(guardOperations > 0 && effectOperations >= 2 && registerWrites >= 2,
            "guard, effect, or reset generated operations were not covered");
    auto restored = roundtrip(design);
    requireCovered(*restored.findGraph("top"));
}

void checkFlattenFallbackAndBoundaries() {
    for (const bool nested : {false, true}) {
        grh::Design base;
        auto& child = base.createGraph("child");
        const grh::LogicalType signedByte{grh::ValueType::Logic, 8, true};
        const auto childA = child.createValue(child.internSymbol("a"), signedByte);
        const auto childY = child.createValue(child.internSymbol("y"), signedByte);
        child.bindInputPort("a", childA);
        child.bindOutputPort("y", childY);
        const auto inverted = child.createTemporaryOperation(grh::OperationKind::kNot);
        child.addOperand(inverted, childA);
        child.addResult(inverted, childY);

        if (nested) {
            auto& middle = base.createGraph("middle");
            auto& tree = middle.getOrCreateSemanticTree("unit", "circuit", "middle");
            const auto aSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "a");
            const auto ySemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "y");
            const auto instanceSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "u");
            const auto a = middle.createValue(middle.internSymbol("a"), signedByte);
            const auto y = middle.createValue(middle.internSymbol("y"), signedByte);
            middle.bindInputPort("a", a);
            middle.bindOutputPort("y", y);
            middle.setValueSemanticIds(a, std::span<const grh::SemanticId>(&aSemantic, 1));
            middle.setValueSemanticIds(y, std::span<const grh::SemanticId>(&ySemantic, 1));
            const auto instance = middle.createOperation(grh::OperationKind::kInstance);
            middle.addOperand(instance, a);
            middle.addResult(instance, y);
            middle.setAttr(instance, "moduleName", std::string("child"));
            middle.setAttr(instance, "instanceName", std::string("u"));
            middle.setAttr(instance, "inputPortName", std::vector<std::string>{"a"});
            middle.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
            middle.setAttr(instance, "inoutPortName", std::vector<std::string>{});
            middle.setOpSemanticIds(instance, std::span<const grh::SemanticId>(&instanceSemantic, 1));
        }

        auto& top = base.createGraph("top");
        auto& tree = top.getOrCreateSemanticTree("unit", "circuit", "top");
        const auto aSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "a");
        const auto ySemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, "y");
        const std::string instanceName = nested ? "m" : "u";
        const auto instanceSemantic = tree.addNode(tree.root(), grh::SemanticNodeKind::Declaration, instanceName);
        const grh::LogicalType unsignedByte{grh::ValueType::Logic, 8, false};
        const auto a = top.createValue(top.internSymbol("a"), unsignedByte);
        const auto y = top.createValue(top.internSymbol("y"), unsignedByte);
        top.bindInputPort("a", a);
        top.bindOutputPort("y", y);
        top.setValueSemanticIds(a, std::span<const grh::SemanticId>(&aSemantic, 1));
        top.setValueSemanticIds(y, std::span<const grh::SemanticId>(&ySemantic, 1));
        const auto instance = top.createOperation(grh::OperationKind::kInstance);
        top.addOperand(instance, a);
        top.addResult(instance, y);
        top.setAttr(instance, "moduleName", std::string(nested ? "middle" : "child"));
        top.setAttr(instance, "instanceName", instanceName);
        top.setAttr(instance, "inputPortName", std::vector<std::string>{"a"});
        top.setAttr(instance, "outputPortName", std::vector<std::string>{"y"});
        top.setAttr(instance, "inoutPortName", std::vector<std::string>{});
        top.setOpSemanticIds(instance, std::span<const grh::SemanticId>(&instanceSemantic, 1));
        base.markAsTop("top");

        const std::vector<std::string> expected = nested ?
            std::vector<std::string>{"top", "m", "u"} : std::vector<std::string>{"top", "u"};
        const auto check = [&](grh::Design design, bool globalPlan) {
            tx::HierFlattenOptions options;
            options.useGlobalPlan = globalPlan;
            require(node_test::run(design, std::make_unique<tx::HierFlattenPass>(options)),
                    globalPlan ? "global mixed-source flatten failed" : "legacy mixed-source flatten failed");
            const auto& flattened = *design.findGraph("top");
            requireCovered(flattened);
            require(countOperations(flattened, expected) == 1,
                    "source graph without a SemanticTree did not inherit the relocated instance declaration");
            require(countOperations(flattened, {"top", "a"}) >= 1 &&
                    countOperations(flattened, {"top", "y"}) >= 1,
                    "flatten boundary assigns are not attributed to their port leaves");
        };
        check(base.clone(), false);
        check(base.clone(), true);
    }
}

} // namespace

int main() {
    try {
        const auto path = artifact("firrtl-semantic-tree") / "hierarchy.fir";
        std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt<8>
    output y : UInt<8>
    node inner = add(a, UInt<8>(2))
    connect y, inner
  module child :
    input a : UInt<8>
    output y : UInt<8>
    node n = add(a, UInt<8>(1))
    inst l0 of leaf
    connect l0.a, n
    connect y, l0.y
  public module top :
    input a0 : UInt<8>
    input a1 : UInt<8>
    output y0 : UInt<8>
    output y1 : UInt<8>
    output wy : UInt<8>
    wire w : {bits : {data : UInt<8>}}
    connect w.bits.data, a0
    connect wy, w.bits.data
    inst c0 of child
    inst c1 of child
    connect c0.a, a0
    connect c1.a, a1
    connect y0, c0.y
    connect y1, c1.y
)FIR";
        auto base = firrtl_test::design(path);
        for (const auto& [name, graph] : base.graphs()) requireCovered(*graph);
        const auto& lowered = *base.findGraph("top");
        const auto aggregate = lowered.findValue("w__DOT__bits__DOT__data");
        require(aggregate.valid(), "lowered bundle value is missing");
        require(hasPath(lowered, lowered.valueSemanticIds(aggregate), {"top", "w", "bits", "data"}),
                "lowering did not retain the bundle field path");
        const auto& child = *base.findGraph("child");
        const auto childInput = child.findValue("a");
        require(childInput.valid() && hasPath(child, child.valueSemanticIds(childInput), {"child", "a"}),
                "lowering did not retain the child input source");
        auto ingested = roundtrip(base);
        const auto& restoredChild = *ingested.findGraph("child");
        const auto restoredInput = restoredChild.findValue("a");
        require(restoredInput.valid() && hasPath(restoredChild, restoredChild.valueSemanticIds(restoredInput),
                                                 {"child", "a"}),
                "JSON roundtrip lost ingest SemanticId associations");
        auto legacy = base.clone();
        checkFlattened(legacy, false);
        auto global = base.clone();
        checkFlattened(global, true);
        ingest::IngestRequest request;
        request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
        request.firrtl.nodeAnnotations = true;
        request.pipeline = ingest::IngestPipeline::GrhPreFlatten;
        auto prefixed = ingest::readSources(request);
        require(prefixed.success() && prefixed.design, diagnostics(prefixed));
        checkFlattened(*prefixed.design, true);
        checkMemoryAnnotations();
        checkAggregateMemoryAnnotations();
        checkGeneratedAnnotations();
        checkFlattenFallbackAndBoundaries();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[firrtl-semantic-tree] " << error.what() << '\n';
        return 1;
    }
}
