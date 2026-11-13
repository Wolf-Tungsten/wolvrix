#include "firrtl_test_fixture.hpp"
#include "core/load.hpp"
#include "core/store.hpp"
#include "transform/hier_flatten.hpp"

#include <fstream>
#include <unordered_set>

namespace {
using namespace firrtl_test;

grh::SemanticId nodeAt(const grh::Graph& graph, const std::vector<std::string>& path) {
    const auto* tree = graph.semanticTree();
    require(tree, "missing SemanticTree");
    grh::SemanticId found = 0;
    for (const auto id : tree->nodeIds()) {
        if (tree->path(id) != path) continue;
        require(!found, "duplicate semantic Node path");
        found = id;
    }
    require(found, "missing semantic Node: " + grh::SemanticTree::qualifiedName(path));
    return found;
}

grh::SemanticId flatNode(const grh::Graph& graph, const std::vector<std::string>& path) {
    const auto* tree = graph.semanticTree();
    const auto qualified = grh::SemanticTree::qualifiedName(path);
    for (const auto id : tree->nodeIds())
        if (tree->node(id).parent == tree->root() && tree->name(id) == qualified) return id;
    throw std::runtime_error("missing flattened semantic Node: " + qualified);
}

bool owns(const grh::Graph& graph, grh::OperationId operation, grh::SemanticId node) {
    const auto ids = graph.opSemanticIds(operation);
    return std::find(ids.begin(), ids.end(), node) != ids.end();
}

std::size_t membersOfKind(const grh::Graph& graph, grh::SemanticId node, grh::OperationKind kind) {
    const auto members = graph.semanticNodeOperations(node);
    return std::count_if(members.begin(), members.end(), [&](const auto op) { return graph.opKind(op) == kind; });
}

void requireCovered(const grh::Graph& graph) {
    const auto* tree = graph.semanticTree();
    require(tree, "semantic coverage requires a SemanticTree");
    const auto errors = graph.validateSemanticTree();
    require(errors.empty(), errors.empty() ? "" : errors.front());
    require(graph.semanticBindings().empty(), "FIRRTL still creates the retired binding schema");
    std::unordered_set<grh::OperationId, grh::OperationIdHash> covered;
    for (const auto id : tree->nodeIds()) {
        require(tree->pathContains(tree->node(id).parent), "Node parent is not a Path");
        for (const auto op : graph.semanticNodeOperations(id)) {
            require(owns(graph, op, id), "Node membership and op reverse index disagree");
            covered.insert(op);
        }
        for (const auto value : graph.semanticNodeAnchors(id)) {
            const auto ids = graph.valueSemanticIds(value);
            require(std::find(ids.begin(), ids.end(), id) != ids.end(), "anchor reverse index disagrees");
        }
    }
    for (const auto op : graph.operations())
        require(covered.contains(op), "unowned FIRRTL op: " + std::string(grh::toString(graph.opKind(op))));
}

grh::Design roundtrip(const grh::Design& design) {
    wolvrix::lib::store::StoreOptions options;
    options.jsonMode = wolvrix::lib::store::JsonPrintMode::PrettyCompact;
    const auto json = wolvrix::lib::store::StoreJson().storeToString(design, options);
    require(json.has_value(), "failed to store SemanticTree checkpoint");
    auto restored = wolvrix::lib::load::LoadJson().load(*json);
    const auto second = wolvrix::lib::store::StoreJson().storeToString(restored, options);
    require(second && *second == *json, "SemanticTree checkpoint is not byte-stable");
    for (const auto& [name, graph] : restored.graphs()) requireCovered(*graph);
    return restored;
}

grh::Design fixture(std::string_view name, std::string_view source) {
    const auto path = artifact("firrtl-semantic-tree") / (std::string(name) + ".fir");
    std::ofstream(path) << source;
    auto result = firrtl_test::design(path);
    for (const auto& [module, graph] : result.graphs()) requireCovered(*graph);
    return result;
}

void checkNamedNodesAndPaths() {
    auto design = fixture("nodes-and-paths", R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input a : {payload : SInt<8>[2], zero : UInt<0>}[2]
    output y : {payload : SInt<8>[2], zero : UInt<0>}[2]
    output q : UInt<9>
    wire empty : UInt<8>[0]
    node n = add(UInt<8>(1), UInt<8>(2))
    node alias = n
    connect y, a
    connect q, alias
)FIR");
    const auto& top = *design.findGraph("top");
    const auto* tree = top.semanticTree();
    require(tree->size() == 8, "Bundle paths or vector elements became semantic Nodes");
    require(tree->pathCount() == 3, "Bundle paths were not interned independently of Nodes");
    const auto input = nodeAt(top, {"top", "a", "payload"});
    require(top.semanticNodeOperations(input).empty(), "an input Node acquired fabricated circuitry");
    const auto anchors = top.semanticNodeAnchors(input);
    require(anchors.size() == 1 && top.valueShape(anchors.front()).size() == 2 &&
            top.valueShape(anchors.front())[0] == 2 && top.valueShape(anchors.front())[1] == 2,
            "array input is not one Node with its actual shaped value anchor");
    for (const auto& path : {std::vector<std::string>{"top", "a", "zero"},
                             std::vector<std::string>{"top", "y", "zero"},
                             std::vector<std::string>{"top", "empty"}}) {
        const auto id = nodeAt(top, path);
        require(top.semanticNodeOperations(id).empty() && top.semanticNodeAnchors(id).empty(),
                "zero-width or empty-array Node acquired a fabricated op/value");
    }
    const auto n = nodeAt(top, {"top", "n"});
    const auto alias = nodeAt(top, {"top", "alias"});
    require(membersOfKind(top, n, grh::OperationKind::kAdd) == 1 &&
            membersOfKind(top, n, grh::OperationKind::kConstant) >= 2,
            "named expression Node does not own its actual expression implementation");
    require(top.semanticNodeOperations(alias).size() == 1 &&
            membersOfKind(top, alias, grh::OperationKind::kAssign) == 1,
            "alias Node swallowed its named input's operations");
    roundtrip(design);
}

void checkGuardsStateAndEffects() {
    auto design = fixture("guards-state-effects", R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input reset : AsyncReset
    input c : UInt<1>
    input d : UInt<1>
    input a : UInt<8>
    output y : UInt<8>
    output z : UInt<8>
    regreset r : UInt<8>, clock, reset, UInt<8>(7)
    reg rv : UInt<8>[2], clock
    reg idle : UInt<8>, clock
    reg idleArray : UInt<8>[2], clock
    node guard_2 = and(c, d)
    connect y, a
    connect z, a
    connect rv[1], UInt<8>(3)
    when c :
      when guard_2 :
        connect y, r
        connect z, r
        connect r, UInt<8>(1)
    when d :
      connect r, UInt<8>(2)
    when UInt<1>(0) :
      skip
    printf(clock, c, "r=%x", r)
    assert(clock, d, c, "unexpected r")
)FIR");
    const auto& top = *design.findGraph("top");
    const auto* tree = top.semanticTree();
    const auto current = nodeAt(top, {"top", "r"});
    const auto next = nodeAt(top, {"top", "r$NEXT"});
    const auto arrayNext = nodeAt(top, {"top", "rv$NEXT"});
    const auto y = nodeAt(top, {"top", "y"}), z = nodeAt(top, {"top", "z"});
    require(current != next && tree->node(current).parent == tree->node(next).parent,
            "state current/next are not distinct leaf Nodes");
    require(top.semanticNodeOperations(current).size() == 2 &&
            membersOfKind(top, current, grh::OperationKind::kRegister) == 1 &&
            membersOfKind(top, current, grh::OperationKind::kRegisterReadPort) == 1,
            "current-state Node does not own exactly the actual declaration/read");
    require(membersOfKind(top, next, grh::OperationKind::kRegisterWritePort) == 3,
            "next-state Node omits an ordinary/reset ordered writer");
    require(membersOfKind(top, arrayNext, grh::OperationKind::kRegisterWriteArrayPort) == 1,
            "array next-state Node omits the indexed writer");
    for (const auto& name : {"idle", "idleArray"}) {
        const auto heldCurrent = nodeAt(top, {"top", name});
        const auto heldNext = nodeAt(top, {"top", std::string(name) + "$NEXT"});
        const auto currentAnchors = top.semanticNodeAnchors(heldCurrent);
        const auto nextAnchors = top.semanticNodeAnchors(heldNext);
        require(top.semanticNodeOperations(heldNext).empty() && currentAnchors.size() == 1 &&
                nextAnchors == currentAnchors,
                "writer-free next Node does not anchor the actual current value hold boundary");
    }
    for (const auto op : top.semanticNodeOperations(next)) {
        if (top.opKind(op) != grh::OperationKind::kRegisterWritePort) continue;
        require(!owns(top, op, current) && top.opOperands(op).size() == 4 &&
                top.getOperation(op).attr("writerOrder") && top.getOperation(op).attr("eventEdge"),
                "next-state membership lost actual enable/data/clock/reset/order context");
    }
    std::size_t shared = 0, effects = 0;
    for (const auto op : top.operations()) {
        if (top.opKind(op) == grh::OperationKind::kMux && owns(top, op, y) && owns(top, op, z)) {
            ++shared;
            require(owns(top, op, next), "cached nested guard lost a later next-state consumer");
        }
        if (top.opKind(op) != grh::OperationKind::kSystemTask) continue;
        ++effects;
        const auto ids = top.opSemanticIds(op);
        require(ids.size() == 1 && tree->name(ids.front()).starts_with("effect@"),
                "effect task is not a member of its ordinary named leaf");
    }
    require(shared >= 2 && effects == 2, "guard sharing or effect Nodes are missing");
    require(membersOfKind(top, nodeAt(top, {"top", "guard_2"}), grh::OperationKind::kAnd) == 1,
            "generated guards replaced a legal named computation");
    for (const auto id : tree->nodeIds())
        require(!tree->name(id).starts_with("when@"), "pure control occurrence became a redundant Node");
    roundtrip(design);
}

void checkMemoryNodes() {
    auto design = fixture("memory", R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input addr : UInt<2>
    output y : UInt<8>
    smem mem : UInt<8>[4]
    read mport r = mem[addr], clock
    cmem emptyMemory : UInt<0>[2]
    connect y, r
)FIR");
    const auto& top = *design.findGraph("top");
    const auto storage = nodeAt(top, {"top", "mem", "$storage"});
    require(membersOfKind(top, storage, grh::OperationKind::kRegister) == 1 &&
            top.semanticNodeAnchors(storage).empty(), "memory storage does not directly own its actual state op");
    const auto empty = nodeAt(top, {"top", "emptyMemory", "$storage"});
    require(top.semanticNodeOperations(empty).empty(), "empty memory acquired fabricated storage");
    const auto address = nodeAt(top, {"top", "r$ADDR0"});
    const auto addressNext = nodeAt(top, {"top", "r$ADDR0$NEXT"});
    require(membersOfKind(top, address, grh::OperationKind::kRegister) == 1 &&
            membersOfKind(top, address, grh::OperationKind::kRegisterReadPort) == 1 &&
            top.semanticNodeAnchors(address).size() == 1 &&
            membersOfKind(top, addressNext, grh::OperationKind::kRegisterWritePort) == 1,
            "read-address pipeline current/next do not own their actual state implementation");
    roundtrip(design);

    auto aggregate = fixture("memory-aggregate", R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input addr : UInt<2>
    input wa : UInt<1>
    input wb : UInt<1>
    input a : UInt<8>
    input b : UInt<8>
    output y : UInt<8>
    cmem m : {a : UInt<8>, b : UInt<8>}[3]
    rdwr mport p = m[addr], clock
    when wa :
      connect p.a, a
    when wb :
      connect p.b, b
    connect y, p.a
)FIR");
    const auto& graph = *aggregate.findGraph("top");
    require(membersOfKind(graph, nodeAt(graph, {"top", "m", "$storage", "a"}), grh::OperationKind::kRegister) == 1 &&
            membersOfKind(graph, nodeAt(graph, {"top", "m", "$storage", "b"}), grh::OperationKind::kRegister) == 1,
            "aggregate storage field Nodes are missing");
    const auto a = nodeAt(graph, {"top", "p", "a"}), b = nodeAt(graph, {"top", "p", "b"});
    require(std::any_of(graph.operations().begin(), graph.operations().end(), [&](const auto op) {
        return owns(graph, op, a) && owns(graph, op, b);
    }), "shared read/write mode implementation is not shared by its actual port Nodes");
    roundtrip(aggregate);

    auto standard = fixture("memory-standard", R"FIR(FIRRTL version 6.0.0
circuit top :
  public module top :
    input clock : Clock
    input addr : UInt<2>
    input en : UInt<1>
    input data : UInt<8>
    output y : UInt<8>
    mem m :
      data-type => UInt<8>
      depth => 4
      reader => r
      writer => w
      read-latency => 1
      write-latency => 2
      read-under-write => old
    connect m.r.addr, addr
    connect m.r.en, en
    connect m.r.clk, clock
    connect m.w.addr, addr
    connect m.w.en, en
    connect m.w.clk, clock
    connect m.w.data, data
    connect m.w.mask, en
    connect y, m.r.data
)FIR");
    const auto& standardGraph = *standard.findGraph("top");
    require(membersOfKind(standardGraph, nodeAt(standardGraph, {"top", "m", "$storage"}),
                          grh::OperationKind::kRegister) == 1 &&
            membersOfKind(standardGraph, nodeAt(standardGraph, {"top", "m", "w", "data"}),
                          grh::OperationKind::kRegisterWriteArrayMaskPort) == 1,
            "standard memory ports/storage lost their actual members");
    nodeAt(standardGraph, {"top", "m", "w", "data$PIPE0$NEXT"});
    roundtrip(standard);
}

void checkHierarchy() {
    auto base = fixture("hierarchy", R"FIR(FIRRTL version 6.0.0
circuit top :
  module leaf :
    input a : UInt<8>
    output y : UInt<8>
    node inner = add(a, UInt<8>(2))
    connect y, inner
  module child :
    input a : UInt<8>
    input zero : UInt<0>
    input empty : UInt<8>[0]
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
)FIR");
    const auto& top = *base.findGraph("top");
    const auto c0a = nodeAt(top, {"top", "c0", "a"});
    const auto c0y = nodeAt(top, {"top", "c0", "y"});
    const auto& child = *base.findGraph("child");
    const auto formals = child.metadata("firrtl.semanticPortNames");
    const auto formalNodes = child.metadata("firrtl.semanticPortNodes");
    require(formals && formalNodes && std::get<std::vector<std::string>>(*formals).size() == 4 &&
            std::get<std::vector<int64_t>>(*formalNodes).size() == 4,
            "formal semantic port map omitted elided endpoints");
    for (const auto op : top.operations()) {
        if (top.opKind(op) != grh::OperationKind::kInstance || !owns(top, op, c0a)) continue;
        require(owns(top, op, c0y) && top.opSemanticIds(op).size() == 4 &&
                std::get<std::vector<std::string>>(*top.getOperation(op).attr("semanticPortNames")).size() == 4 &&
                std::get<std::vector<int64_t>>(*top.getOperation(op).attr("semanticPortNodes")).size() == 4,
                "instance membership is not its concrete port leaves");
    }
    auto restored = roundtrip(base);
    for (const bool globalPlan : {false, true}) {
        auto design = restored.clone();
        tx::HierFlattenOptions options;
        options.useGlobalPlan = globalPlan;
        require(node_test::run(design, std::make_unique<tx::HierFlattenPass>(options)), "flatten failed");
        require(design.graphs().size() == 1, "flatten retained internal module graphs");
        const auto& flat = *design.findGraph("top");
        requireCovered(flat);
        const auto* tree = flat.semanticTree();
        require(tree->pathCount() == 1 && tree->size() == 22,
                "flatten retained paths or duplicated instance port placeholders");
        for (const auto id : tree->nodeIds())
            require(tree->node(id).parent == tree->root(), "flatten did not attach every Node directly to root");
        for (const auto& instance : {"c0", "c1"}) {
            require(membersOfKind(flat, flatNode(flat, {instance, "n"}), grh::OperationKind::kAdd) == 1 &&
                    membersOfKind(flat, flatNode(flat, {instance, "l0", "inner"}), grh::OperationKind::kAdd) == 1,
                    "instance op members were not independently relocated");
            require(flat.semanticNodeAnchors(flatNode(flat, {instance, "a"})).size() == 1,
                    "flatten lost an empty-op input/alias Node anchor");
            for (const auto& name : {"zero", "empty"}) {
                const auto node = flatNode(flat, {instance, name});
                require(flat.semanticNodeOperations(node).empty() && flat.semanticNodeAnchors(node).empty(),
                        "flatten duplicated or fabricated elided instance-port members");
            }
        }
        flatNode(flat, {"w", "bits", "data"});
        roundtrip(design);
    }
}
} // namespace

int main() {
    try {
        checkNamedNodesAndPaths();
        checkGuardsStateAndEffects();
        checkMemoryNodes();
        checkHierarchy();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[firrtl-semantic-tree] " << error.what() << '\n';
        return 1;
    }
}
