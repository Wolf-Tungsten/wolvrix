#include "firrtl_test_fixture.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/node-cse.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/simplify.hpp"
#include "../../lib/transform/grhsim/node-cse-internal.hpp"

using namespace firrtl_test;
namespace sim = tx::grhsim;
namespace {
void convert(Fixture& f) { node::establishPolicy(f.graph); run(f.design, std::make_unique<sim::GrhToGrhsimPass>()); }
void cse(Fixture& f, std::string mode = "gsim") {
    run(f.design, std::make_unique<sim::NodeCsePass>(sim::NodeCseOptions{"", mode, {}})); valid(f.design);
}
void threshold(unsigned size, int width, unsigned fanout, bool share, std::string mode = "gsim") {
    Fixture f; const auto a = f.input("a", width), b = f.input("b", width), c = f.input("c", width);
    for (unsigned i = 0; i < size; ++i) {
        const auto identity = key("n" + std::to_string(i));
        const auto inner = f.compute(OperationKind::kAnd, {a,b}, identity, false, width);
        const auto value = f.compute(OperationKind::kXor, {inner,c}, identity, true, width);
        f.output("y" + std::to_string(i), value);
        if (!i && fanout > 1) {
            // Repeated arguments within the same consumer count once.
            const auto use = f.compute(OperationKind::kAdd, {value,value}, key("use"), true, width);
            f.output("use", use);
        }
    }
    convert(f); cse(f, mode);
    require(count(f.graph, OperationKind::kXor) == (share ? 1 : size), "whole-node threshold mismatch");
    require(count(f.graph, OperationKind::kAnd) == (share ? 1 : size), "whole node interiors did not follow threshold");
    const auto first = f.graph.outputPortValue("y0");
    require((first == f.graph.outputPortValue("y1")) == share, "root outputs not redirected");
    auto before = roundTrip(f.design); cse(f, mode);
    wolvrix::lib::store::StoreJson store;
    require(store.storeToString(before) == store.storeToString(f.design), "node CSE is not idempotent");
    const auto attrs = node::read(f.graph, f.graph.valueDef(first));
    require(attrs.roots.size() == (share ? size : 1), "merged source identities were lost");
}
void downstreamEquivalence() {
    Fixture f; const auto a = f.input("a"), b = f.input("b"), c = f.input("c", 257);
    const auto x = f.compute(OperationKind::kAnd, {a,b}, key("x"), true);
    const auto y = f.compute(OperationKind::kAnd, {a,b}, key("y"), true);
    // Each upstream node has one successor and stays physically independent.
    const auto p = f.compute(OperationKind::kAdd, {x,c}, key("p"), true, 257);
    const auto q = f.compute(OperationKind::kAdd, {y,c}, key("q"), true, 257);
    f.output("p",p); f.output("q",q); convert(f); cse(f);
    require(count(f.graph, OperationKind::kAdd) == 1, "downstream missed retained upstream equivalence");
    // y may become dead after the dependent merge, but x/y were not themselves
    // physically merged: the surviving AND must retain its original scope.
    for (const auto op : f.graph.operations()) if (f.graph.opKind(op) == OperationKind::kAnd)
        require(node::read(f.graph,op).members.size() == 1, "equivalence alone merged upstream owners");
}
void boundariesAndSubtrees() {
    Fixture f; const auto a=f.input("a"), b=f.input("b"), c=f.input("c"), d=f.input("d");
    for (unsigned i=0;i<5;++i) {
        const auto identity=key("x"+std::to_string(i));
        const auto inner=f.compute(OperationKind::kAnd,{a,b},identity);
        const auto value=f.compute(i%2 ? OperationKind::kXor : OperationKind::kOr,{inner,c},identity,true);
        f.output("x"+std::to_string(i),value);
    }
    const auto different=f.compute(OperationKind::kOr,{a,d},key("different"),true); f.output("d",different);
    convert(f); cse(f);
    require(count(f.graph,OperationKind::kAnd)==5,"cross-node internal subtree was extracted");
    require(f.graph.outputPortValue("x0")!=f.graph.outputPortValue("x1"),"different functions merged");
    require(f.graph.outputPortValue("x0")!=f.graph.outputPortValue("d"),"different inputs merged");
}
void repeatedSuccessor() {
    Fixture f; const auto a=f.input("a",256), b=f.input("b",256);
    for(unsigned i=0;i<2;++i){const auto n=f.compute(OperationKind::kAnd,{a,b},key("n"+std::to_string(i)),true,256);
        const auto out=f.compute(OperationKind::kXor,{n,n},key("out"+std::to_string(i)),true,256);f.output("o"+std::to_string(i),out);}
    convert(f); cse(f); require(count(f.graph,OperationKind::kAnd)==2,"repeated argument counted as two successors");
}
void typeAndAttrs() {
    Fixture f; const auto a=f.input("a",257), b=f.input("b",257);
    const auto x=f.compute(OperationKind::kSliceStatic,{a},key("x"),true,8);
    const auto y=f.compute(OperationKind::kSliceStatic,{a},key("y"),true,8);
    f.graph.setAttr(f.graph.valueDef(x),"sliceStart",int64_t{0});f.graph.setAttr(f.graph.valueDef(x),"sliceEnd",int64_t{7});
    f.graph.setAttr(f.graph.valueDef(y),"sliceStart",int64_t{1});f.graph.setAttr(f.graph.valueDef(y),"sliceEnd",int64_t{8});
    f.output("x",x);f.output("y",y);
    const auto p=f.compute(OperationKind::kAnd,{a,b},key("p"),true,257);
    const auto q=f.compute(OperationKind::kAnd,{b,a},key("q"),true,257);
    f.output("p",p);f.output("q",q);convert(f);cse(f);
    require(f.graph.outputPortValue("p")!=f.graph.outputPortValue("q"),"ordered operands were ignored");
    require(count(f.graph,OperationKind::kSliceStatic)==2,"functional attributes were ignored");
}
void configuration() {
    Fixture f;const auto a=f.input("a"),b=f.input("b"); f.output("y",f.compute(OperationKind::kAnd,{a,b},key("n"),true));
    convert(f);cse(f,"off");
    require(!run(f.design,std::make_unique<sim::NodeCsePass>(),false),"checkpoint policy change accepted");
    std::string error; const std::string_view args[]={"-firrtl-node-cse=off"};
    require(bool(tx::makePass("grhsim-node-cse",args,error)),error);
}
void collisions() {
    sim::detail::NodeTerms terms([](std::string_view) { return 0; });
    const auto a = terms.intern("and:8:input-a:input-b");
    const auto b = terms.intern("or:8:input-a:input-b");
    require(a != b && terms.intern("and:8:input-a:input-b") == a, "hash collision proved false equivalence");
}
void behaviorAndIdentity() {
    Fixture f;
    // Leave holes so freeze/clone actually renumbers IDs.
    const auto unused = f.compute(OperationKind::kNot, {f.input("a")});
    const auto unusedOp = f.graph.valueDef(unused);
    const auto b = f.input("b");
    for (unsigned i = 0; i < 5; ++i) {
        const auto identity = key("n" + std::to_string(i));
        const auto inner = f.compute(OperationKind::kAnd, {f.graph.inputPortValue("a"), b}, identity);
        f.output("y" + std::to_string(i), f.compute(OperationKind::kNot, {inner}, identity, true));
    }
    f.graph.eraseOp(unusedOp); f.graph.eraseValue(unused);
    convert(f);
    auto clone = f.design.clone();
    auto& graph = *clone.findGraph("top");
    for (const auto op : graph.operations()) graph.setOpSymbol(op, graph.makeInternalOpSym());
    for (const auto value : graph.values()) if (!graph.valueIsInput(value)) graph.setValueSymbol(value, graph.makeInternalValSym());
    auto loaded = roundTrip(clone);
    const auto original = f.design.clone();
    cse(f); run(clone, std::make_unique<sim::NodeCsePass>()); run(loaded, std::make_unique<sim::NodeCsePass>());
    for (const auto* candidate : {&f.design, &clone, &loaded}) {
        const auto& current = *candidate->findGraph("top");
        require(count(current, OperationKind::kAnd) == 1 && count(current, OperationKind::kNot) == 1, "identity reconstruction changed sharing");
        Oracle oracle(current);
        for (unsigned a = 0; a < 256; ++a) for (unsigned b = 0; b < 256; b += 17)
            for (unsigned i = 0; i < 5; ++i)
                require(oracle.output("y" + std::to_string(i), {{"a", a}, {"b", b}}) == ((~(a & b)) & 255), "whole-node CSE changed behavior");
    }
    require(node::inspect(*original.findGraph("top")).nodes.size() == node::inspect(f.graph).nodes.size(), "identity count changed");
}
void incompleteAndInstances() {
    Fixture f; const auto a = f.input("left_a",257), b = f.input("right_a",257), c = f.input("b",257);
    for (unsigned i = 0; i < 2; ++i) {
        auto identity = node::parseKey(key("n")); identity.instancePath = {i ? "right" : "left"};
        const auto inner = f.compute(OperationKind::kAnd, {i ? b : a,c}, node::key(identity), false,257);
        f.output("inner" + std::to_string(i), inner);
        f.output("out" + std::to_string(i), f.compute(OperationKind::kNot, {inner}, node::key(identity), true,257));
    }
    convert(f); cse(f);
    require(count(f.graph, OperationKind::kAnd) == 2, "different instance inputs shared");
    const auto stats = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeCseStats"));
    require(std::find(stats.begin(),stats.end(),"skip-incomplete-region=2") != stats.end(), "escaping interior was accepted");
    auto legacy = f.design.clone(); legacy.findGraph("top")->eraseMetadata(node::kScopePolicy);
    require(!run(legacy, std::make_unique<sim::NodeCsePass>(), false), "legacy provenance upgraded silently");
}
void distinctInstanceInputs() {
    Fixture f;
    const auto left = f.input("left", 257), right = f.input("right", 257), b = f.input("b", 257);
    for (unsigned i = 0; i < 2; ++i) {
        auto identity = node::parseKey(key("n"));
        identity.instancePath = {i ? "right_instance" : "left_instance"};
        const auto inner = f.compute(OperationKind::kAnd, {i ? right : left, b}, node::key(identity), false, 257);
        f.output("out" + std::to_string(i), f.compute(OperationKind::kNot, {inner}, node::key(identity), true, 257));
    }
    convert(f); cse(f);
    require(count(f.graph, OperationKind::kAnd) == 2, "complete nodes with distinct instance inputs merged");
    const auto stats = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeCseStats"));
    require(std::find(stats.begin(), stats.end(), "equivalent=0") != stats.end(), "distinct boundary values compared equal");
}
void fullResultTypes() {
    Fixture f; const auto a = f.input("a", 257), b = f.input("b", 257);
    for (unsigned i = 0; i < 4; ++i)
        f.output("out" + std::to_string(i), f.compute(OperationKind::kAnd, {a, b}, key("n" + std::to_string(i)), true, 257));
    convert(f);
    for (unsigned i = 1; i < 4; ++i) {
        const auto value = f.graph.outputPortValue("out" + std::to_string(i));
        auto type = f.graph.valueLogicalType(value);
        if (i == 1) type.isSigned = true;
        if (i == 2) type.domain = grh::LogicDomain::FourState;
        if (i == 3) type.shape = {257};
        f.graph.setValueLogicalType(value, type);
    }
    cse(f);
    require(count(f.graph, OperationKind::kAnd) == 4, "result sign/domain/shape were ignored");
    const auto stats = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeCseStats"));
    require(std::find(stats.begin(), stats.end(), "skip-array-or-nonlogic=1") != stats.end(), "array node was not rejected");
}
void constantPayloads() {
    Fixture f; const auto a = f.input("a", 257);
    for (unsigned i = 0; i < 2; ++i) {
        const auto literal = f.constant(i ? "257'h2" : "257'h1", {}, 257);
        f.output("out" + std::to_string(i), f.compute(OperationKind::kAnd, {a, literal}, key("n" + std::to_string(i)), true, 257));
    }
    convert(f); cse(f);
    require(count(f.graph, OperationKind::kAnd) == 2, "different constant payloads merged");
}
void stateAndEffects() {
    auto input = text("node-state-effect-rejection",
        "    input clk : Clock\n    input a : UInt<8>\n    output y : UInt<8>\n"
        "    reg r : UInt<8>, clk\n    connect r, a\n    node n = not(r)\n    connect y, n\n"
        "    printf(clk, UInt<1>(1), \"a=%d\\n\", a)\n");
    require(input.success(), diagnostics(input));
    auto& graph = *input.design->findGraph("top");
    for (const auto op : graph.operations()) {
        if (graph.opKind(op) == OperationKind::kRegisterReadPort) {
            node::Attributes attrs; attrs.members = {key("read")}; attrs.roots = {{key("read"), 0}};
            node::write(graph, op, std::move(attrs));
        }
    }
    node::establishPolicy(graph);
    run(*input.design, std::make_unique<sim::GrhToGrhsimPass>());
    bool taggedHistory = false;
    for (const auto op : graph.operations()) if (graph.opKind(op) == sim::kEdgeDetect && !taggedHistory) {
        node::Attributes attrs; attrs.members = {key("history")}; attrs.roots = {{key("history"), 0}};
        node::write(graph, op, std::move(attrs)); taggedHistory = true;
    }
    require(taggedHistory, "state fixture lacks an effectful history operation");
    node::establishPolicy(graph);
    run(*input.design, std::make_unique<sim::NodeCsePass>());
    const auto stats = std::get<std::vector<std::string>>(*graph.metadata("firrtl.nodeCseStats"));
    require(std::find(stats.begin(), stats.end(), "skip-state-or-effect=2") != stats.end(), "state/history node entered whole-node CSE");
    require(count(graph, sim::kSystemTask) == 1, "effect operation changed during node CSE");
}
void multipleResults() {
    constexpr auto pairKind = static_cast<OperationKind>(0x30000);
    static const grh::OpDescriptor descriptors[]{{pairKind, "grhsim.node-cse-test-pair", "grhsim", 1,
        grh::OpEffect::Pure, true, true, 1, 1, 2, 2}};
    grh::registerOpDescriptors(descriptors);
    Fixture f; const auto a = f.input("a"); convert(f);
    for (unsigned i = 0; i < 5; ++i) {
        const auto op = f.graph.createOperation(pairKind); f.graph.addOperand(op, a);
        node::Attributes attrs;
        for (unsigned lane = 0; lane < 2; ++lane) {
            const auto identity = key("pair" + std::to_string(i), {std::to_string(lane)});
            const auto value = f.graph.createValue(Fixture::type()); f.graph.addResult(op, value);
            attrs.members.push_back(identity); attrs.roots.push_back({identity, lane});
            f.output("out" + std::to_string(i) + std::to_string(lane), value);
        }
        node::write(f.graph, op, std::move(attrs));
    }
    node::establishPolicy(f.graph); cse(f);
    require(count(f.graph, pairKind) == 5, "multi-result operations entered whole-node sharing");
    const auto stats = std::get<std::vector<std::string>>(*f.graph.metadata("firrtl.nodeCseStats"));
    require(std::find(stats.begin(), stats.end(), "skip-multiple-results=10") != stats.end(), "multi-result rejection was not reported");
}
}
int main(){try{threshold(4,256,1,false);threshold(5,256,1,true);threshold(2,257,1,true);
threshold(2,256,2,true);threshold(5,257,2,false,"off");downstreamEquivalence();boundariesAndSubtrees();
repeatedSuccessor();typeAndAttrs();configuration();collisions();behaviorAndIdentity();incompleteAndInstances();
distinctInstanceInputs();fullResultTypes();constantPayloads();stateAndEffects();multipleResults();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
