#include "node_test_fixture.hpp"

#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/recover-masked-or.hpp"
#include "transform/grhsim/simplify.hpp"
#include "transform/simplify.hpp"
#include "slang/numeric/SVInt.h"

#include <map>
#include <random>

using namespace node_test;
namespace sim = tx::grhsim;

namespace {
// Independent, small two-state oracle for the combinational fixtures below.
uint64_t evaluate(const grh::Graph& graph, grh::ValueId value, const std::map<std::string, uint64_t>& inputs,
                  std::unordered_map<grh::ValueId, uint64_t, grh::ValueIdHash>& cache) {
    if (const auto found = cache.find(value); found != cache.end()) return found->second;
    const auto op = graph.valueDef(value);
    uint64_t bits = 0;
    if (!op) bits = inputs.at(std::string(graph.symbolText(graph.valueSymbol(value))));
    else {
        std::vector<uint64_t> args;
        for (const auto arg : graph.opOperands(op)) args.push_back(evaluate(graph, arg, inputs, cache));
        switch (graph.opKind(op)) {
        case OperationKind::kAssign: bits = args.at(0); break;
        case OperationKind::kAnd: bits = args.at(0) & args.at(1); break;
        case OperationKind::kOr: bits = args.at(0) | args.at(1); break;
        case OperationKind::kXor: bits = args.at(0) ^ args.at(1); break;
        case OperationKind::kXnor: bits = ~(args.at(0) ^ args.at(1)); break;
        case OperationKind::kNot: bits = ~args.at(0); break;
        case OperationKind::kAdd: bits = args.at(0) + args.at(1); break;
        case OperationKind::kMux: bits = args.at(0) ? args.at(1) : args.at(2); break;
        case OperationKind::kConcat:
            for (std::size_t i = 0; i < args.size(); ++i)
                bits = (bits << graph.valueWidth(graph.opOperands(op)[i])) | args[i];
            break;
        case OperationKind::kSliceStatic:
            bits = args.at(0) >> sim::attribute<int64_t>(graph, op, "sliceStart").value(); break;
        case OperationKind::kArrayCreate: {
            int32_t offset = 0;
            for (std::size_t i = 0; i < args.size(); ++i) {
                bits |= args[i] << offset; offset += graph.valueWidth(graph.opOperands(op)[i]);
            }
            break;
        }
        case OperationKind::kSliceArray:
            bits = args.at(0) >> (args.at(1) * graph.valueWidth(value)); break;
        case sim::kMaskedOr: {
            const auto views = sim::attribute<std::vector<int64_t>>(graph, op, "views").value();
            for (std::size_t i = 0; i < views.size(); i += 4)
                if ((args.at(views[i]) >> views[i + 1]) & 1) bits |= args.at(views[i + 2]) >> views[i + 3];
            break;
        }
        case OperationKind::kConstant:
            bits = slang::SVInt::fromString(sim::attribute<std::string>(graph, op, "constValue").value()).getRawPtr()[0]; break;
        default: throw std::runtime_error("oracle fixture operation is not supported");
        }
    }
    bits &= (uint64_t{1} << graph.valueWidth(value)) - 1;
    cache.emplace(value, bits); return bits;
}
uint64_t evaluate(const grh::Graph& graph, grh::ValueId value, const std::map<std::string, uint64_t>& inputs) {
    std::unordered_map<grh::ValueId, uint64_t, grh::ValueIdHash> cache;
    return evaluate(graph, value, inputs, cache);
}
void compare(const grh::Design& reference, const grh::Design& candidate) {
    const auto& before = *reference.findGraph("top");
    const auto& after = *candidate.findGraph("top");
    std::mt19937_64 rng(917);
    for (unsigned trial = 0; trial < 512; ++trial) {
        std::map<std::string, uint64_t> inputs;
        for (const auto& port : before.inputPorts()) inputs.emplace(port.name, rng());
        for (const auto& port : before.outputPorts()) {
            const auto found = std::find_if(after.outputPorts().begin(), after.outputPorts().end(),
                                           [&](const auto& item) { return item.name == port.name; });
            require(found != after.outputPorts().end(), "optimizer dropped an observable output");
            require(evaluate(before, port.value, inputs) == evaluate(after, found->value, inputs),
                    "node scope optimization changed a combinational output");
        }
    }
}
void sameStructure(const grh::Graph& left, const grh::Graph& right) {
    require(left.operations().size() == right.operations().size(), "annotations changed optimized operation count");
    std::unordered_map<grh::OperationId, grh::OperationId, grh::OperationIdHash> forward, reverse;
    std::function<void(grh::ValueId, grh::ValueId)> match = [&](auto a, auto b) {
        require(left.valueLogicalType(a) == right.valueLogicalType(b), "annotations changed optimized type");
        const auto x = left.valueDef(a), y = right.valueDef(b);
        require(bool(x) == bool(y), "annotations retained an otherwise removable producer");
        if (!x) {
            require(left.symbolText(left.valueSymbol(a)) == right.symbolText(right.valueSymbol(b)), "optimized input differs");
            return;
        }
        if (forward.contains(x)) { require(forward.at(x) == y, "annotations prevented sharing"); return; }
        require(!reverse.contains(y), "annotations introduced different sharing");
        forward.emplace(x, y); reverse.emplace(y, x);
        require(left.opKind(x) == right.opKind(y), "annotations changed optimized operation");
        const auto attributes = [](const auto& graph, auto op) {
            std::vector<grh::AttrKV> result;
            for (const auto& entry : graph.opAttrs(op))
                if (!node::isAttribute(entry.key) && entry.key != "grhsim.nodeClone" && entry.key != "originModules") result.push_back(entry);
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
            return result;
        };
        const auto aa = attributes(left, x), bb = attributes(right, y);
        require(aa.size() == bb.size(), "functional attribute count differs");
        for (std::size_t i = 0; i < aa.size(); ++i)
            require(aa[i].key == bb[i].key && aa[i].value == bb[i].value, "functional attributes differ");
        const auto ax = left.opOperands(x), by = right.opOperands(y);
        require(ax.size() == by.size(), "optimized arity differs");
        for (std::size_t i = 0; i < ax.size(); ++i) match(ax[i], by[i]);
    };
    for (const auto& port : left.outputPorts()) {
        const auto found = std::find_if(right.outputPorts().begin(), right.outputPorts().end(),
                                      [&](const auto& other) { return other.name == port.name; });
        require(found != right.outputPorts().end(), "output lost"); match(port.value, found->value);
    }
}
void optimize(Fixture& f, bool grhsim) {
    auto unannotated = f.design.clone();
    for (auto& [name, graph] : unannotated.graphs()) {
        for (const auto op : graph->operations()) node::write(*graph, op, {});
        graph->eraseMetadata(node::kSchema);
    }
    const auto apply = [&](auto& design) {
        if (grhsim) {
            run(design, std::make_unique<sim::GrhToGrhsimPass>());
            run(design, std::make_unique<sim::SimplifyPass>());
        } else run(design, std::make_unique<tx::SimplifyPass>());
    };
    apply(f.design); apply(unannotated);
    sameStructure(f.graph, *unannotated.findGraph("top"));
    valid(f.design);
    run(f.design, std::make_unique<tx::GrhVerifyPass>());
}

void localAndCrossNode(bool grhsim, OperationKind primitive) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b"), c = f.input("c");
    const auto n1 = key("n1"), n2 = key("n2");
    const auto first = f.compute(primitive, {a, b}, n1);
    const auto duplicate = f.compute(primitive, {a, b}, n1);
    auto provenance = node::read(f.graph, f.graph.valueDef(duplicate));
    provenance.origins = {key("diagnostic-only")};
    node::write(f.graph, f.graph.valueDef(duplicate), std::move(provenance));
    const auto local = f.compute(OperationKind::kAdd, {first, duplicate}, n1, true);
    const auto second = f.compute(primitive, {a, b}, n2);
    const auto other = f.compute(OperationKind::kXor, {second, c}, n2, true);
    const auto sv1 = f.compute(primitive, {a, b});
    const auto sv2 = f.compute(primitive, {a, b});
    const auto svOut = f.compute(OperationKind::kAdd, {sv1, sv2});
    f.output("local", local); f.output("other", other); f.output("sv", svOut);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    require(count(f.graph, primitive) <= (primitive == OperationKind::kAdd ? 3 : 1),
            "ordinary CSE did not share across node and SV sources");
    compare(reference, f.design);
    auto loaded = roundTrip(f.design);
    valid(loaded);
    compare(reference, loaded);
}

void rootsAndConstants(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto one = f.constant("8'h3", key("literal"));
    const auto two = f.constant("8'h3", key("duplicate-literal"));
    const auto folded = f.compute(OperationKind::kAdd, {one, two}, key("folded"), true);
    const auto x = f.compute(OperationKind::kAnd, {a, b}, key("x"), true);
    const auto alias = f.compute(OperationKind::kAssign, {x}, key("alias"), true);
    const auto inputAlias = f.compute(OperationKind::kAssign, {a}, key("input-alias"), true);
    f.compute(OperationKind::kOr, {a, b}, key("dead"), true);
    f.output("folded", folded); f.output("x", x); f.output("alias", alias); f.output("input", inputAlias);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    require(count(f.graph, OperationKind::kAdd) == 0, "node attrs prevented constant folding");
    for (const auto op : f.graph.operations()) {
        const auto attrs = node::read(f.graph, op);
        require(std::find(attrs.members.begin(), attrs.members.end(), key("dead")) == attrs.members.end(),
                "node attrs prevented dead code removal");
    }
    compare(reference, f.design);
}

void rewrittenRoot(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto inner = f.compute(OperationKind::kXor, {a, b}, key("n"));
    const auto outer = f.compute(OperationKind::kNot, {inner}, key("n"), true);
    f.output("y", outer);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    compare(reference, f.design);
}

void rootBecomesInput(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto joined = f.compute(OperationKind::kConcat, {a, b}, key("n"), false, 16);
    const auto slice = f.compute(OperationKind::kSliceStatic, {joined}, key("n"), true);
    const auto op = f.graph.valueDef(slice);
    f.graph.setAttr(op, "sliceStart", int64_t{8}); f.graph.setAttr(op, "sliceEnd", int64_t{15});
    f.output("y", slice);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    require(count(f.graph, OperationKind::kConcat) == 0 && count(f.graph, OperationKind::kSliceStatic) == 0,
            "root tracking prevented structural simplification");
    compare(reference, f.design);
}

void crossNodeFusion(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto inner = f.compute(OperationKind::kXor, {a, b}, key("x"), true);
    const auto outer = f.compute(OperationKind::kNot, {inner}, key("n"), true);
    f.output("y", outer);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    // optimize() compares complete sharing structure with an unannotated copy;
    // neither pipeline is required to choose a particular XOR spelling.
    compare(reference, f.design);
}

void maskedOrBoundary(bool sameNode) {
    Fixture f;
    const auto s1 = f.input("s1", 1), s2 = f.input("s2", 1), a = f.input("a"), b = f.input("b");
    const auto zero = f.constant("8'h0");
    const auto x = f.compute(OperationKind::kMux, {s1, a, zero}, key(sameNode ? "n" : "x"), !sameNode);
    const auto y = f.compute(OperationKind::kMux, {s2, b, zero}, key(sameNode ? "n" : "y"), !sameNode);
    const auto result = f.compute(OperationKind::kOr, {x, y}, key("n"), true);
    f.output("result", result);
    const auto reference = f.design.clone();
    run(f.design, std::make_unique<sim::GrhToGrhsimPass>());
    sim::RecoverMaskedOrOptions options; options.minTerms = 2;
    run(f.design, std::make_unique<sim::RecoverMaskedOrPass>(options));
    valid(f.design);
    require(count(f.graph, sim::kMaskedOr) == 1, "node hint blocked masked-OR recovery");
    compare(reference, f.design);
}

void arrays(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto concat = f.compute(OperationKind::kConcat, {a, b}, key("array"), false, 16);
    const auto pack = f.graph.createOperation(OperationKind::kAssign);
    const auto array = f.graph.createValue(Fixture::type(16, {2}));
    f.graph.addOperand(pack, concat); f.graph.addResult(pack, array);
    f.tag(pack, key("array"), true);
    const auto index = f.constant("8'h1");
    const auto element = f.compute(OperationKind::kSliceArray, {array, index}, key("element"), true);
    f.output("array", array); f.output("element", element);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    require(count(f.graph, OperationKind::kArrayCreate) == 1, "node attrs prevented array shape normalization");
    require(count(f.graph, OperationKind::kSliceArray) == 0, "node hint blocked array element recovery");
    compare(reference, f.design);
    auto loaded = roundTrip(f.design);
    valid(loaded);
    run(loaded, std::make_unique<tx::GrhVerifyPass>());
    compare(reference, loaded);
}

void isolatedFragments(bool grhsim) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto left = f.compute(OperationKind::kAnd, {a, b});
    const auto right = f.compute(OperationKind::kAnd, {a, b});
    for (const auto value : {left, right}) {
        node::Attributes attrs; attrs.origins = {key("lost-members")};
        node::write(f.graph, f.graph.valueDef(value), std::move(attrs));
    }
    const auto result = f.compute(OperationKind::kAdd, {left, right});
    f.output("y", result);
    const auto reference = f.design.clone();
    optimize(f, grhsim);
    require(count(f.graph, OperationKind::kAnd) == 1, "diagnostic-only provenance blocked CSE");
    require(node::inspect(f.graph).isolatedFragments.size() == 1, "diagnostic provenance lost from representative");
    compare(reference, f.design);
}

void leafSharing(bool sameLeaf, std::string kind) {
    Fixture f;
    const auto a = f.input("a"), b = f.input("b");
    const auto left = f.compute(OperationKind::kAnd, {a, b}, key("left"), true);
    const auto right = f.compute(OperationKind::kAnd, {a, b}, key("right"), true);
    // Anchors keep both observations live without assigning these values to Output.
    f.graph.addDeclaredSymbol(f.graph.valueSymbol(left));
    f.graph.addDeclaredSymbol(f.graph.valueSymbol(right));
    f.output("input", a);
    run(f.design, std::make_unique<sim::GrhToGrhsimPass>());
    f.graph.setMetadata(sim::kStage, std::string(kind == "nonsink" ? "nonsink-partitioned" : "sink-partitioned"));
    const std::vector<std::string> owner = kind == "nonsink"
        ? std::vector<std::string>{"general", "nonsink", "sn_0", "node_0"}
        : std::vector<std::string>{"general", kind, "group_0"};
    for (const auto op : f.graph.operations()) f.graph.setAttr(op, sim::kPartition, owner);
    if (!sameLeaf) {
        auto other = owner; other.back() += "_other";
        f.graph.setAttr(f.graph.valueDef(right), sim::kPartition, std::move(other));
    }
    const bool expectedScope = node::sameScope(f.graph, f.graph.valueDef(left), f.graph.valueDef(right));
    require(expectedScope == sameLeaf, "node CSE scope ignored the final execution leaf");
    run(f.design, std::make_unique<sim::SimplifyPass>());
    valid(f.design);
    require(count(f.graph, OperationKind::kAnd) == (sameLeaf ? 1 : 2), "CSE did not follow final execution ownership");
    for (const auto& record : node::inspect(f.graph).nodes) {
        require(record.roots.size() == 1, "leaf CSE lost a live node root");
        require(evaluate(f.graph, record.roots[0].value, {{"a", 0x5a}, {"b", 0x3c}}) == (0x5a & 0x3c),
                "leaf CSE root no longer computes the expected result");
    }
}
}

int main() {
    try {
        for (const bool grhsim : {false, true}) {
            localAndCrossNode(grhsim, OperationKind::kAnd);
            localAndCrossNode(grhsim, OperationKind::kAdd);
            rootsAndConstants(grhsim); rewrittenRoot(grhsim);
            rootBecomesInput(grhsim); crossNodeFusion(grhsim);
            arrays(grhsim);
            isolatedFragments(grhsim);
        }
        for (const auto kind : {"nonsink", "small-sink", "large-sink"}) {
            leafSharing(false, kind); leafSharing(true, kind);
        }
        maskedOrBoundary(false); maskedOrBoundary(true);
    } catch (const std::exception& error) { std::cerr << "[firrtl-cse-scope] " << error.what() << '\n'; return 1; }
    return 0;
}
