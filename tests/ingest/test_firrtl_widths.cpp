#include "firrtl_test_fixture.hpp"

namespace {
using namespace firrtl_test;
struct Case {
    std::string name, expression;
    int width = 1;
    bool sign = false;
    std::function<int64_t(int64_t,int64_t,unsigned)> expected;
};
int64_t right(int64_t a, unsigned n) {
    const int64_t scale = int64_t{1} << n;
    return a >= 0 ? a / scale : -((-a + scale - 1) / scale);
}
void primitives(bool sign) {
    std::vector<Case> cases{
        {"add", "add(a,b)", 6, sign, [](auto a,auto b,auto) { return a+b; }},
        {"sub", "sub(a,b)", 6, sign, [](auto a,auto b,auto) { return a-b; }},
        {"mul", "mul(a,b)", 8, sign, [](auto a,auto b,auto) { return a*b; }},
        {"div", "div(a,b)", sign?6:5, sign, [](auto a,auto b,auto) { return b?a/b:0; }},
        {"rem", "rem(a,b)", 3, sign, [](auto a,auto b,auto) { return b?a%b:0; }},
        {"eq", "eq(a,b)", 1, false, [](auto a,auto b,auto) { return a==b; }},
        {"neq", "neq(a,b)", 1, false, [](auto a,auto b,auto) { return a!=b; }},
        {"lt", "lt(a,b)", 1, false, [](auto a,auto b,auto) { return a<b; }},
        {"leq", "leq(a,b)", 1, false, [](auto a,auto b,auto) { return a<=b; }},
        {"gt", "gt(a,b)", 1, false, [](auto a,auto b,auto) { return a>b; }},
        {"geq", "geq(a,b)", 1, false, [](auto a,auto b,auto) { return a>=b; }},
        {"and", "and(a,b)", 5, false, [](auto a,auto b,auto) { return a&b; }},
        {"or", "or(a,b)", 5, false, [](auto a,auto b,auto) { return a|b; }},
        {"xor", "xor(a,b)", 5, false, [](auto a,auto b,auto) { return a^b; }},
        {"not", "not(a)", 5, false, [](auto a,auto,auto) { return ~a; }},
        {"pad", "pad(a,8)", 8, sign, [](auto a,auto,auto) { return a; }},
        {"cvt", "cvt(a)", sign?5:6, true, [](auto a,auto,auto) { return a; }},
        {"neg", "neg(a)", 6, true, [](auto a,auto,auto) { return -a; }},
        {"shl", "shl(a,2)", 7, sign, [](auto a,auto,auto) { return a*4; }},
        {"shr", "shr(a,2)", 3, sign, [](auto a,auto,auto) { return right(a,2); }},
        {"dshl", "dshl(a,sh)", 8, sign, [](auto a,auto,auto sh) { return a*(int64_t{1}<<sh); }},
        {"dshr", "dshr(a,sh)", 5, sign, [](auto a,auto,auto sh) { return right(a,sh); }},
        {"cat", "cat(a,b)", 8, false, [](auto a,auto b,auto) { return ((a&31)<<3)|(b&7); }},
        {"bits", "bits(a,3,1)", 3, false, [](auto a,auto,auto) { return (a&31)>>1; }},
        {"head", "head(a,3)", 3, false, [](auto a,auto,auto) { return (a&31)>>2; }},
        {"tail", "tail(a,3)", 2, false, [](auto a,auto,auto) { return a&3; }},
        {"andr", "andr(a)", 1, false, [](auto a,auto,auto) { return (a&31)==31; }},
        {"orr", "orr(a)", 1, false, [](auto a,auto,auto) { return a!=0; }},
        {"xorr", "xorr(a)", 1, false, [](auto a,auto,auto) { return std::popcount(uint64_t(a)&31)&1; }},
        {"asUInt", "asUInt(a)", 5, false, [](auto a,auto,auto) { return a&31; }},
        {"asSInt", "asSInt(a)", 5, true, [](auto a,auto,auto) { return a&31; }},
        {"mux", "mux(sel,a,b)", 5, sign, [](auto a,auto b,auto sh) { return (sh&1)?a:b; }},
    };
    const std::string ground = sign ? "SInt" : "UInt";
    std::string body = "    input a : " + ground + "<5>\n    input b : " + ground + "<3>\n    input sh : UInt<2>\n    input sel : UInt<1>\n";
    for (const auto& entry : cases) body += "    output o_"+entry.name+" : "+(entry.sign?"SInt":"UInt")+"<"+std::to_string(entry.width)+">\n";
    for (const auto& entry : cases) body += "    node n_"+entry.name+" = "+entry.expression+"\n    connect o_"+entry.name+", n_"+entry.name+"\n";
    auto result = text(sign?"signed-primitives":"unsigned-primitives", body);
    require(result.success(), diagnostics(result)); auto& graph = *result.design->findGraph("top");
    run(*result.design, std::make_unique<tx::GrhVerifyPass>()); valid(*result.design);
    for (const auto& entry : cases) {
        const auto value = graph.findValue("n_"+entry.name);
        require(graph.valueWidth(value) == entry.width && graph.valueSigned(value) == entry.sign, "primitive result type differs: "+entry.name);
    }
    const auto check = [&] {
        Oracle oracle(graph);
        for (int64_t a=sign?-16:0; a<(sign?16:32); ++a) for (int64_t b=sign?-4:0; b<(sign?4:8); ++b) for (unsigned sh=0; sh<4; ++sh) {
            const std::map<std::string,uint64_t> inputs{{"a",uint64_t(a)&31},{"b",uint64_t(b)&7},{"sh",sh},{"sel",sh&1}};
            for (const auto& entry : cases) {
                const auto actual = oracle.output("o_"+entry.name, inputs);
                const auto expected = uint64_t(entry.expected(a,b,sh)) & Oracle::mask(entry.width);
                require(actual == expected, entry.name+" a="+std::to_string(a)+" b="+std::to_string(b)+" sh="+std::to_string(sh)+" expected="+std::to_string(expected)+" actual="+std::to_string(actual));
            }
        }
    };
    check(); run(*result.design, std::make_unique<tx::SimplifyPass>()); valid(*result.design); check();
}
void zeroAndConstants() {
    auto result = text("zero-widths", R"FIR(    input z : UInt<0>
    input empty : UInt<8>[0]
    input index : UInt<2>
    output oempty : UInt<8>[0]
    output a : UInt<1>
    output b : UInt<1>
    output c : UInt<1>
    output d : UInt<8>
    output e : UInt<8>
    output f : SInt<8>
    output g : UInt<8>
    node nothing = cat()
    node singleton = cat(UInt<6>(42))
    node signedZero = SInt<0>(-0)
    connect a, andr(nothing)
    connect b, orr(z)
    connect c, xorr(nothing)
    connect d, cat(nothing, singleton)
    connect e, pad(z,8)
    connect f, signedZero
    connect g, empty[index]
    connect oempty, empty
)FIR");
    require(result.success(), diagnostics(result)); auto& graph = *result.design->findGraph("top");
    for (const auto value : graph.values()) {
        require(graph.valueWidth(value)>0, "zero-width GRH materialized");
        for (const auto extent : graph.valueShape(value)) require(extent>0, "zero-extent GRH materialized");
    }
    require(!graph.inputPortValue("z") && !graph.inputPortValue("empty") && !graph.outputPortValue("oempty"), "zero ABI leaf was materialized");
    Oracle oracle(graph); const std::map<std::string,uint64_t> inputs{{"index",3}};
    require(oracle.output("a",inputs)==1 && oracle.output("b",inputs)==0 && oracle.output("c",inputs)==0 && oracle.output("d",inputs)==42 && oracle.output("e",inputs)==0 && oracle.output("f",inputs)==0 && oracle.output("g",inputs)==0, "zero-width identity/layout differs");
    auto wide = text("wide-literals", R"FIR(    output a : UInt<130>
    output b : SInt<130>
    output c : SInt<8>
    node pos = UInt(1361129467683753853853498429727072845823)
    node neg = SInt(-1361129467683753853853498429727072845824)
    connect a, pos
    connect b, neg
    connect c, SInt(-42)
)FIR");
    require(wide.success(), diagnostics(wide)); auto& wideGraph = *wide.design->findGraph("top");
    require(wideGraph.valueWidth(wideGraph.findValue("pos"))==130 && wideGraph.valueWidth(wideGraph.findValue("neg"))==131, "arbitrary precision literal inference truncated");
    run(*wide.design, std::make_unique<tx::GrhVerifyPass>());
    auto invalid = text("invalid-policy", "    output y : UInt<8>\n    invalidate y\n", ingest::FirrtlInvalidPolicy::Unknown);
    require(invalid.success(), diagnostics(invalid));
    const auto& invalidGraph = *invalid.design->findGraph("top");
    bool unknown = false;
    for (const auto op : invalidGraph.operations()) if (invalidGraph.opKind(op)==OperationKind::kConstant)
        unknown |= std::get<std::string>(*invalidGraph.getOperation(op).attr("constValue")).find('x') != std::string::npos;
    require(unknown && std::get<std::string>(*invalidGraph.metadata("firrtl.invalidPolicy"))=="unknown", "explicit invalid policy lost");
}
void resets() {
    auto result = text("reset-inference", R"FIR(    input a : UInt<1>
    input async : AsyncReset
    output x : AsyncReset
    output y : UInt<1>
    wire r : Reset
    wire q : Reset
    connect r, asReset(a)
    connect q, asReset(a)
    connect x, r
    connect y, q
)FIR");
    require(result.success(), diagnostics(result));
    run(*result.design, std::make_unique<tx::GrhVerifyPass>());
    auto port = text("reset-port-inference", "    input r : Reset\n    output y : AsyncReset\n    connect y,r\n");
    require(port.success(),diagnostics(port));
    require(port.modules[0].ports[0].sourceType=="AsyncReset" && !port.modules[0].ports[0].requiresTypeResolution,"resolved Reset signature was not published");
    error("conflicting-reset", "    input a : UInt<1>\n    input b : AsyncReset\n    wire r : Reset\n    connect r,a\n    connect r,b\n", "both synchronous and asynchronous", 8);
    error("reset-width", "    input a : UInt<2>\n    wire r : Reset\n    connect r,a\n", "one-bit UInt", 6);
    error("reset-cast", "    input a : UInt<2>\n    node r = asReset(a)\n", "asReset requires UInt<1>", 5);
}
void errors() {
    error("mixed-sign", "    input a : UInt<2>\n    input b : SInt<2>\n    node n = add(a,b)\n", "same signedness", 6);
    error("mixed-cat", "    node n = cat(UInt<1>(0),SInt<1>(0))\n", "same signedness");
    error("bad-bit-range", "    input a : UInt<2>\n    node n = bits(a,3,0)\n", "outside its operand", 5);
    error("overflow-shift", "    input a : UInt<2>\n    node n = shl(a,9223372036854775807)\n", "GRH range", 5);
    error("overflow-dshift", "    input a : UInt<2>\n    input sh : UInt<32>\n    node n = dshl(a,sh)\n", "exceeds GRH range", 6);
    error("unresolved", "    wire a : UInt\n", "width cannot be inferred", 4);
    error("uninitialized", "    output a : UInt<1>\n", "not fully initialized", 4);
    error("partial-path", "    input sel : UInt<1>\n    output a : UInt<1>\n    when sel :\n      connect a,UInt<1>(1)\n", "not fully initialized", 5);
    error("bad-when", "    input sel : UInt<2>\n    when sel : skip\n", "at most one bit", 5);
    error("input-sink", "    input a : UInt<1>\n    connect a,UInt<1>(1)\n", "sink or duplex", 5);
    error("literal-fit", "    node a = UInt<2>(4)\n", "does not fit", 4);
    error("negative-uint", "    node a = UInt(-1)\n", "cannot be negative", 4);
    error("const-source", "    input a : UInt<1>\n    output y : const UInt<1>\n    connect y,a\n", "non-constant source", 6);
    error("const-condition", "    input sel : UInt<1>\n    output y : const UInt<1>\n    connect y,UInt(0)\n    when sel : connect y,UInt(1)\n", "non-constant when", 7);
    error("cycle", "    output y : UInt<4>\n    wire w : UInt<4>\n    connect w,add(w,UInt<1>(1))\n    connect y,w\n", "combinational cycle", 5);
    error("memory-stage", "    input clk : Clock\n    cmem r : UInt<4>[4]\n", "not supported yet", 5);
    auto bounded=text("bounded-width-cycle",R"FIR(    output y : UInt<16>
    wire w : UInt
    node t = rem(add(w,UInt<1>(1)),UInt<16>(1))
    connect w,t
    connect w,UInt<1>(0)
    connect y,w
)FIR");
    require(bounded.success(),diagnostics(bounded));
    require(bounded.design->findGraph("top")->valueWidth(bounded.design->findGraph("top")->findValue("w"))==16,"bounded width cycle was mistaken for an unbounded constraint");
    error("unbounded-width-cycle", "    wire w : UInt\n    connect w,add(w,UInt<1>(1))\n    connect w,UInt<1>(0)\n", "resource limit exceeded");
}
}
int main() {
    try { primitives(false); primitives(true); zeroAndConstants(); resets(); errors(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
