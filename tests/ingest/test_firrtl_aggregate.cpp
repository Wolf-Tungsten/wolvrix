#include "firrtl_test_fixture.hpp"
#include "transform/comb_loop_elim.hpp"
#include "transform/const_fold.hpp"
#include "transform/hier_flatten.hpp"
#include "slang/numeric/SVInt.h"

namespace {
using namespace firrtl_test;
void shape(const grh::Graph& graph, std::string_view name, int width, std::vector<int64_t> expected) {
    const auto value = graph.findValue(name); require(bool(value), "missing flattened leaf: "+std::string(name));
    require(graph.valueWidth(value)==width && std::vector<int64_t>(graph.valueShape(value).begin(),graph.valueShape(value).end())==expected, "wrong aggregate geometry: "+std::string(name));
}
void requireCycleRejectedAfterFlatten(ingest::IngestResult& result, std::string_view message) {
    require(result.success(), diagnostics(result));
    tx::PassManager manager;
    manager.addPass(std::make_unique<tx::HierFlattenPass>());
    tx::CombLoopElimOptions options;
    options.failOnTrueLoop = true;
    manager.addPass(std::make_unique<tx::CombLoopElimPass>(options));
    tx::PassDiagnostics diagnostics;
    const auto outcome = manager.run(*result.design, diagnostics);
    require(!outcome.success, std::string(message));
}
void rectangularMixedIndexBounds() {
    // FIRRTL's final suffix is the outer dimension: this is two rows of three.
    const std::string body = R"FIR(    input a : UInt<4>[3][2]
    input i : UInt<2>
    input data : UInt<4>
    output rowOut : UInt<4>[3]
    output selectedOut : UInt<4>
    output typedOut : UInt<4>
    output missingOut : UInt<4>
    output matrixOut : UInt<4>[3][2]
    output noOpOut : UInt<4>[3][2]
    node row = a[i]
    node selected = a[i][2]
    node typed = a[i][UInt<2>(2)]
    node missing = a[i][UInt<2>(3)]
    wire w : UInt<4>[3][2]
    connect w, a
    connect w[i][UInt<2>(2)], data
    connect w[i][UInt<2>(3)], UInt<4>(15)
    wire noOp : UInt<4>[3][2]
    connect noOp, a
    connect noOp[i][UInt<2>(3)], UInt<4>(15)
    connect rowOut, row
    connect selectedOut, selected
    connect typedOut, typed
    connect missingOut, missing
    connect matrixOut, w
    connect noOpOut, noOp
)FIR";
    for (const auto policy : {ingest::FirrtlInvalidPolicy::Unknown, ingest::FirrtlInvalidPolicy::Zero}) {
        auto result = text("rectangular-mixed-index-bounds", body, policy);
        require(result.success(), diagnostics(result));
        run(*result.design, std::make_unique<tx::GrhVerifyPass>());
        auto restored = roundTrip(*result.design);
        for (const auto* candidate : {result.design->findGraph("top"), restored.findGraph("top")}) {
            shape(*candidate, "a", 24, {2, 3});
            shape(*candidate, "row", 12, {3}); shape(*candidate, "rowOut", 12, {3});
            shape(*candidate, "selected", 4, {}); shape(*candidate, "typed", 4, {});
            shape(*candidate, "missing", 4, {});
            shape(*candidate, "matrixOut", 24, {2, 3}); shape(*candidate, "noOpOut", 24, {2, 3});
            Oracle oracle(*candidate);
            for (const uint64_t value : {UINT64_C(0x654321), UINT64_C(0xabcdef)})
                for (unsigned i = 0; i < 4; ++i) for (unsigned data : {0u, 9u, 15u}) {
                    const std::map<std::string, uint64_t> inputs{{"a", value}, {"i", i}, {"data", data}};
                    const auto offset = 12*i + 8;
                    const auto expectedSelected = i < 2 ? (value >> offset) & 15 : 0;
                    const auto expectedMatrix = i < 2 ?
                        (value & ~(UINT64_C(15) << offset)) | (uint64_t(data) << offset) : value;
                    require(oracle.output("rowOut", inputs) == (i < 2 ? (value >> (12*i)) & 4095 : 0),
                        "dynamic first index changed the rectangular suffix shape or row stride");
                    require(oracle.output("selectedOut", inputs) == expectedSelected &&
                        oracle.output("typedOut", inputs) == expectedSelected &&
                        oracle.output("missingOut", inputs) == 0,
                        "static second index used the wrong extent or changed invalid read behavior");
                    require(oracle.output("matrixOut", inputs) == expectedMatrix &&
                        oracle.output("noOpOut", inputs) == value,
                        "UInt second-index out-of-bounds write changed a rectangular array");
                }
        }
    }
}
void geometry() {
    rectangularMixedIndexBounds();
    auto source = design(std::filesystem::path(WOLVRIX_FIRRTL_TEST_DATA_DIR)/"comb/aggregate.fir");
    auto& graph = *source.findGraph("top");
    shape(graph,"packet__DOT__valid",2,{2}); shape(graph,"packet__DOT__data",16,{2,2});
    shape(graph,"packet__DOT__meta__DOT__flag",2,{2}); shape(graph,"result__DOT__data",20,{2,2});
    shape(graph,"one",1,{1,1}); shape(graph,"single",1,{1,1}); shape(graph,"sub",8,{2});
    const auto inspection = node::inspect(graph); require(inspection.errors.empty(),inspection.report(graph));
    require(inspection.nodes.size()==6,"Vector node identity was expanded per element or Bundle field omitted");
    for (const auto& record : inspection.nodes) {
        const auto identity = node::parseKey(record.key);
        require(identity.fieldPath==std::vector<std::string>{"valid"} || identity.fieldPath==std::vector<std::string>{"data"} || identity.fieldPath==std::vector<std::string>{"meta","flag"},"node identity field geometry is wrong");
        if (identity.name=="aliasPacket") require(record.members.size()==1,"aggregate alias relabeled its producer");
    }
    require(count(graph,OperationKind::kArrayCreate)>0,"per-element width adaptation lost array construction");
    bool multiIndex = false;
    for (const auto op : graph.operations()) if (graph.opKind(op)==OperationKind::kSliceArray && graph.opOperands(op).size()==3) multiIndex=true;
    require(multiIndex,"nested Vector access was not emitted with multiple indices");
    run(source,std::make_unique<tx::GrhVerifyPass>());
    auto stored=roundTrip(source); valid(stored);
    const auto check = [&](const grh::Graph& candidate) {
        Oracle oracle(candidate);
        for (unsigned n=0;n<64;++n) for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) {
            const uint64_t data=(n*149+0x3579)&65535, small=(n*17+0x53)&255;
            const unsigned sel=n&1, sel2=(n>>1)&1;
            const std::map<std::string,uint64_t> inputs{{"packet__DOT__valid",n&3},{"packet__DOT__data",data},{"packet__DOT__meta__DOT__flag",(n>>2)&3},
                {"other__DOT__valid",(n>>1)&3},{"other__DOT__data",small},{"other__DOT__meta__DOT__flag",(n>>3)&3},
                {"one",n&1},{"sel",sel},{"sel2",sel2},{"i",i},{"j",j},{"data",n&15}};
            uint64_t selected=0, table=data;
            for(unsigned cell=0;cell<4;++cell) selected |= ((sel ? (data>>(cell*4))&15 : (small>>(cell*2))&3) << (cell*5));
            const auto write=[&](unsigned row,unsigned col,unsigned value) { const auto offset=(row*2+col)*4; table=(table&~(UINT64_C(15)<<offset))|(uint64_t(value)<<offset); };
            if(i<2 && j<2) { if(sel) write(i,j,n&15); if(sel2) write(j,i,15); }
            require(oracle.output("result__DOT__data",inputs)==selected,"aggregate mux/connect did not extend each element");
            require(oracle.output("result__DOT__valid",inputs)==(sel?(n&3):((n>>1)&3)),"Bundle mux selection differs");
            require(oracle.output("tableOut",inputs)==table && oracle.output("packed",inputs)==table,"static/dynamic writes, priority or packed layout differs");
            require(oracle.output("readOut",inputs)==(i<2 && j<2 ? (table>>((i*2+j)*4))&15 : 0),"dynamic multi-index access differs");
            require(oracle.output("sub",inputs)==(i<2 ? (table>>(i*8))&255 : 0),"partial indexing dropped suffix geometry");
            require(oracle.output("equal",inputs)==1 && oracle.output("single",inputs)==(n&1),"aggregate equality or singleton layout differs");
            require(oracle.output("lastOut",inputs)==(data&255),"whole connect did not override earlier partial write");
        }
    };
    check(graph); check(*stored.findGraph("top")); run(source,std::make_unique<tx::SimplifyPass>()); valid(source); check(graph);
}
void flowsAndCoverage() {
    auto result=text("flips",R"FIR(    input in : {d : UInt<4>, flip ready : UInt<1>}
    output out : {d : UInt<4>, flip ready : UInt<1>}
    connect out,in
)FIR");
    require(result.success(),diagnostics(result)); auto& graph=*result.design->findGraph("top");
    require(graph.inputPortValue("in__DOT__d") && graph.outputPortValue("in__DOT__ready") && graph.outputPortValue("out__DOT__d") && graph.inputPortValue("out__DOT__ready"),"Bundle flip did not reverse port directions");
    Oracle oracle(graph);
    const std::map<std::string,uint64_t> inputs{{"in__DOT__d",13},{"out__DOT__ready",1}};
    require(oracle.output("out__DOT__d",inputs)==13 && oracle.output("in__DOT__ready",inputs)==1,"flipped whole connect reversed the wrong driver");
    auto coverage=text("coverage",R"FIR(    input sel : UInt<1>
    output y : UInt<4>[2]
    when sel :
      connect y[0],UInt<4>(1)
      connect y[1],UInt<4>(2)
    else :
      connect y[0],UInt<4>(3)
      connect y[1],UInt<4>(4)
)FIR");
    require(coverage.success(),diagnostics(coverage)); Oracle paths(*coverage.design->findGraph("top"));
    require(paths.output("y",{{"sel",1}})==0x21 && paths.output("y",{{"sel",0}})==0x43,"when/else coverage or array element order differs");
    auto local=text("local-wire",R"FIR(    input sel : UInt<1>
    output y : UInt<4>
    when sel :
      wire w : UInt<4>
      connect w,UInt<4>(7)
      connect y,w
    else : connect y,UInt<4>(9)
)FIR");
    require(local.success(),diagnostics(local)); Oracle localOracle(*local.design->findGraph("top"));
    require(localOracle.output("y",{{"sel",1}})==7 && localOracle.output("y",{{"sel",0}})==9,"conditional declaration was required outside its scope");
    auto boolean=text("boolean-coverage",R"FIR(    input sel : UInt<1>
    output y : UInt<4>
    output typed : UInt<4>
    when sel : connect y,UInt<4>(3)
    when not(sel) : connect y,UInt<4>(7)
    when head(UInt<2>(2),1) : connect typed,UInt<4>(1)
    else : connect typed,UInt<4>(2)
    when head(UInt<3>(2),1) : connect typed,UInt<4>(3)
    else : connect typed,UInt<4>(4)
)FIR");
    require(boolean.success(),diagnostics(boolean)); Oracle booleanOracle(*boolean.design->findGraph("top"));
    require(booleanOracle.output("y",{{"sel",1}})==3 && booleanOracle.output("y",{{"sel",0}})==7,"complementary predicates did not cover a complete driver");
    require(booleanOracle.output("typed",{{"sel",0}})==4,"guard identity ignored FIRRTL literal width");
    error("shape-mismatch","    input a : UInt<2>[2]\n    output b : UInt<2>[3]\n    connect b,a\n","lengths do not match",6);
    error("dynamic-sign","    input a : UInt<2>[2]\n    input i : SInt<1>\n    node n = a[i]\n","index must have UInt",6);
    error("static-oob","    input a : UInt<2>[2]\n    node n = a[2]\n","static Vector index",5);
    error("nonpassive-node","    input a : {flip x : UInt<2>}\n    node n = a\n","passive type",5);
    error("name-collision","    input a : {x : UInt<2>}\n    wire a__DOT__x : UInt<2>\n    connect a__DOT__x,UInt<2>(0)\n","name collision",5);
    error("missing-element","    output a : UInt<2>[2]\n    connect a[0],UInt<2>(0)\n","not fully initialized",4);
    error("unknown-field","    input a : {x : UInt<2>}\n    node n = a.z\n","unknown Bundle field",5);
    error("bundle-order","    input a : {x : UInt<2>, y : UInt<2>}\n    output b : {y : UInt<2>, x : UInt<2>}\n    connect b,a\n","field order",6);
    error("aggregate-ground-op","    input a : UInt<2>[2]\n    node n = eq(a,a)\n","ground operands",5);
}
void accessSize() {
    std::size_t previous=0;
    for(const auto extent : {32,4096}) {
        auto result=text("access-"+std::to_string(extent),"    input a : UInt<1>["+std::to_string(extent)+"]\n    input i : UInt<13>\n    output y : UInt<1>\n    node n = a[i]\n    connect y,n\n");
        require(result.success(),diagnostics(result)); auto& graph=*result.design->findGraph("top");
        require(count(graph,OperationKind::kSliceArray)==1 && count(graph,OperationKind::kEq)==0,"dynamic read expanded into per-slot decoding");
        if(previous) require(graph.operations().size()==previous,"dynamic read operation count scales with array extent");
        previous=graph.operations().size(); run(*result.design,std::make_unique<tx::GrhVerifyPass>());
    }
}
void constantIndexBounds() {
    const std::string body = R"FIR(    input a : UInt<4>[2][2]
    input i : UInt<2>
    input data : UInt<4>
    output exactOut : UInt<4>
    output rowOut : UInt<4>[2]
    output selectedOut : UInt<4>
    output missingOut : UInt<4>
    output matrixOut : UInt<4>[2][2]
    node exact = a[1][0]
    node row = a[1]
    node selected = a[UInt<2>(1)][i]
    node missing = a[UInt<2>(2)][i]
    wire w : UInt<4>[2][2]
    connect w, a
    connect w[UInt<2>(2)][i], UInt<4>(15)
    connect w[UInt<2>(1)][i], data
    connect exactOut, exact
    connect rowOut, row
    connect selectedOut, selected
    connect missingOut, missing
    connect matrixOut, w
)FIR";
    for (const auto policy : {ingest::FirrtlInvalidPolicy::Unknown, ingest::FirrtlInvalidPolicy::Zero}) {
        auto result = text("constant-index-bounds", body, policy);
        require(result.success(), diagnostics(result));
        auto& graph = *result.design->findGraph("top");
        require(count(graph, OperationKind::kLt) == 2, "constant indices created runtime bounds checks");
        const auto missingRoot = graph.valueDef(graph.findValue("missing"));
        const auto missingValue = graph.opOperands(missingRoot)[0];
        require(graph.opKind(graph.valueDef(missingValue)) == OperationKind::kConstant &&
            std::get<std::string>(*graph.getOperation(graph.valueDef(missingValue)).attr("constValue")) ==
                (policy == ingest::FirrtlInvalidPolicy::Unknown ? "4'bx" : "4'b0"),
            "constant out-of-bounds read did not use the selected invalid policy");
        run(*result.design, std::make_unique<tx::GrhVerifyPass>()); node::requireValid(graph);
        auto restored = roundTrip(*result.design);
        for (const auto* candidate : {&graph, restored.findGraph("top")}) {
            Oracle oracle(*candidate);
            for (unsigned value : {0x1234u, 0xabcdu}) for (unsigned i = 0; i < 4; ++i) {
                const auto outputs = std::map<std::string, uint64_t>{{"a", value}, {"i", i}, {"data", 9}};
                const auto expected = i < 2 ? (value & ~(15u << (8 + 4*i))) | (9u << (8 + 4*i)) : value;
                require(oracle.output("exactOut", outputs) == ((value >> 8) & 15) &&
                    oracle.output("rowOut", outputs) == (value >> 8) &&
                    oracle.output("selectedOut", outputs) == (i < 2 ? (value >> (8 + 4*i)) & 15 : 0) &&
                    oracle.output("missingOut", outputs) == 0 && oracle.output("matrixOut", outputs) == expected,
                    "constant/mixed indices changed partial shape, invalid reads or no-op writes");
            }
        }
    }
    auto assignment = text("unmasked-wire", "    input a : UInt<4>\n    output y : UInt<4>\n    connect y, a\n");
    require(assignment.success(), diagnostics(assignment));
    const auto& graph = *assignment.design->findGraph("top");
    for (const auto op : graph.operations()) if (graph.opKind(op) == OperationKind::kConstant)
        require(std::get<std::string>(*graph.getOperation(op).attr("constValue")) != "4'b1111",
            "ordinary wire assignment created an unused memory write mask");
}
void inferredAlias() {
    const auto path=artifact("firrtl-fixtures")/"inferred-alias.fir";
    std::ofstream(path)<<R"FIR(FIRRTL version 6.0.0
circuit top :
  type Word = UInt
  public module top :
    input a : UInt<3>
    input b : UInt<7>
    output y : UInt[2]
    wire w : Word[2]
    connect w[0],a
    connect w[1],b
    connect y,w
)FIR";
    auto result=read(path); require(result.success(),diagnostics(result)); auto& graph=*result.design->findGraph("top");
    shape(graph,"w",14,{2}); shape(graph,"y",14,{2});
    require(result.modules[0].ports[2].elementWidth==7 && !result.modules[0].ports[2].requiresTypeResolution,"inferred array signature used total width as element width");
    Oracle oracle(graph); require(oracle.output("y",{{"a",7},{"b",79}})==((79<<7)|7),"alias/vector element widths were not shared and inferred");
}
void elementDependencies() {
    auto result = text("array-element-dependencies", R"FIR(    input a : UInt<4>
    input b : UInt<4>
    output y : UInt<1>[2][2]
    wire matrix : UInt<1>[2][2]
    connect matrix[0][0], UInt<1>(0)
    connect matrix[0][1], lt(a,b)
    connect matrix[1][0], not(matrix[0][1])
    connect matrix[1][1], UInt<1>(0)
    connect y, matrix
)FIR");
    require(result.success(), diagnostics(result));
    auto restored = roundTrip(*result.design);
    const auto& graph = *restored.findGraph("top");
    shape(graph, "y", 4, {2, 2});
    require(graph.metadata("firrtl.pendingCombLoopCheck").has_value(),
            "post-flatten cycle-analysis deferral was lost before publication");
    Oracle oracle(graph);
    for (unsigned a = 0; a < 16; ++a) for (unsigned b = 0; b < 16; ++b)
        require(oracle.output("y", {{"a", a}, {"b", b}}) == (a < b ? 2 : 4),
            "acyclic element dependencies changed while preserving arrays");
    valid(restored);
    auto cycle = text("real-array-cycle", R"FIR(    output y : UInt<1>[2]
    connect y[0], not(y[1])
    connect y[1], not(y[0])
)FIR");
    requireCycleRejectedAfterFlatten(cycle,
        "range analysis accepted a real array element cycle");
}
void conditionalElementDependencies() {
    auto result = text("conditional-array-element-dependencies", R"FIR(    input initial : UInt<1>[24]
    input outer : UInt<1>
    input a : UInt<1>
    input b : UInt<1>
    input c : UInt<1>
    output y : UInt<1>[24]
    wire flags : UInt<1>[24]
    connect flags, initial
    when outer :
      connect flags[13], a
      connect flags[21], b
      connect flags[5], c
      node selected = or(or(flags[13],flags[21]),flags[5])
      when selected : connect flags[4], UInt<1>(0)
    else :
      connect flags[13], UInt<1>(0)
      connect flags[21], UInt<1>(0)
      connect flags[4], UInt<1>(0)
      connect flags[5], UInt<1>(0)
      connect flags[19], a
    connect flags[3], b
    connect y, flags
)FIR");
    require(result.success(), diagnostics(result));
    auto restored = roundTrip(*result.design);
    const auto check = [&](const grh::Design& design) {
        const auto& graph = *design.findGraph("top");
        shape(graph, "y", 24, {24});
        Oracle oracle(graph);
        for (unsigned initial : {0u, 0xffffffu, 0xa5c39fu, 0x1b72e4u})
            for (unsigned bits = 0; bits < 16; ++bits) {
                const unsigned outer = bits & 1, a = (bits >> 1) & 1;
                const unsigned b = (bits >> 2) & 1, c = (bits >> 3) & 1;
                unsigned expected = initial;
                const auto set = [&](unsigned index, unsigned value) {
                    expected = (expected & ~(1u << index)) | (value << index);
                };
                if (outer) {
                    set(13, a); set(21, b); set(5, c);
                    if (a || b || c) set(4, 0);
                } else {
                    set(13, 0); set(21, 0); set(4, 0); set(5, 0); set(19, a);
                }
                set(3, b);
                require(oracle.output("y", {{"initial", initial}, {"outer", outer}, {"a", a}, {"b", b}, {"c", c}}) == expected,
                    "conditional partial connect introduced dependencies on untouched array elements");
            }
    };
    check(*result.design); check(restored);
    run(restored, std::make_unique<tx::SimplifyPass>()); valid(restored); check(restored);
    for (unsigned high : {5u, 10u}) {
        auto unknown = text("unknown-conditional-element", std::string(R"FIR(    output y : UInt<4>[2]
    wire condition : UInt<1>
    invalidate condition
    connect y[0], UInt<4>(5)
    connect y[1], UInt<4>()FIR") + std::to_string(high) + R"FIR()
    when condition : connect y[1], UInt<4>(5)
)FIR", ingest::FirrtlInvalidPolicy::Unknown);
        require(unknown.success(), diagnostics(unknown));
        tx::ConstantFoldOptions options;
        options.xFold = tx::ConstantFoldOptions::XFoldMode::Propagate;
        run(*unknown.design, std::make_unique<tx::ConstantFoldPass>(options));
        const auto& graph = *unknown.design->findGraph("top");
        const auto output = graph.outputPortValue("y");
        const auto operation = graph.valueDef(output);
        require(graph.opKind(operation) == OperationKind::kConstant, "four-state conditional fixture did not fold");
        const auto merged = slang::SVInt::fromString(std::get<std::string>(*graph.getOperation(operation).attr("constValue")));
        require(merged.slice(3, 0).as<uint64_t>() == 5 &&
            (high == 5 ? merged.as<uint64_t>() == 0x55 : merged.slice(7, 4).hasUnknown()),
            "unknown partial-write guard changed untouched bits or lost the written slice's merge");
    }
    auto cycle = text("real-conditional-array-cycle", R"FIR(    output y : UInt<1>[2]
    connect y[0], UInt<1>(1)
    connect y[1], UInt<1>(0)
    when y[0] : connect y[0], UInt<1>(0)
)FIR");
    requireCycleRejectedAfterFlatten(cycle,
        "conditional partial connect hid a real dependency on its own destination");
}
}
int main() {
    try { geometry(); flowsAndCoverage(); accessSize(); constantIndexBounds(); inferredAlias(); elementDependencies(); conditionalElementDependencies(); return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
