#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/reg_to_mem.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

void regToMemSemanticsTests();
void regToMemEmitChecks(const std::filesystem::path &directory);

namespace {
using namespace wolvrix::lib;
using namespace grhsim;
void require(bool b, const char *s) { if (!b) throw std::runtime_error(s); }
void compactTest() {
    GrhSimModel m("compact"); m.addDialect("core", "1", "wolvrix.grhsim.core.v1");
    auto bit = m.logicType(1, false, LogicDomain::TwoState);
    auto q = m.addState("q", bit);
    const std::array params{Parameter{m.intern("value"), std::string("1'b0")}};
    const std::array steps{InitStep{m.intern("core.init.const"), {0, 1}}};
    m.addInit(q, steps, params);
    auto a = m.addValue(bit), b = m.addValue(bit);
    const std::array ar{a}, br{b};
    const std::array refs{ObjectRef::state(q)};
    auto read = m.addOperation("core.state.read", {}, ar, refs);
    m.addOperation("core.compute.not", ar, br);
    std::vector<uint8_t> ops(m.operations().size()+1), states(m.states().size()+1);
    ops[read.index] = 1; states[q.index] = 1;
    bool rejected = false;
    try { m.compact(ops, states); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected && m.operations().size() == 2, "compact accepted dangling reference or changed input on failure");
    m.replaceOperation({2,0}, "core.compute.constant", {}, br, {}, params);
    const auto identity=m.identity(); const auto revision=m.semanticRevision();
    m.compact(ops, states);
    diag::Diagnostics d;
    require(verifyGrhSimModel(m, defaultDialectRegistry(), d), "compacted model invalid");
    require(m.states().empty() && m.values().size()==1 && m.values()[0].id.index==1 &&
            m.identity()==identity && m.semanticRevision()==revision, "compact metadata/remap");
}

void updateConditionTest() {
    GrhSimModel m("update-condition"); m.addDialect("core", "1", "wolvrix.grhsim.core.v1");
    const auto bit = m.logicType(1, false, LogicDomain::TwoState);
    const auto addrType = m.logicType(2, false, LogicDomain::TwoState);
    const auto uInput = m.addInput("update", bit);
    const auto aInput = m.addInput("address", addrType);
    const auto dInput = m.addInput("data", bit);
    const auto d2Input = m.addInput("data2", bit);
    const auto eInput = m.addInput("event", bit);
    auto input = [&](InputId id, TypeId type) {
        const auto value = m.addValue(type);
        m.addOperation("core.input.read", {}, std::array{value}, std::array<ObjectRef, 1>{ObjectRef::input(id)});
        return value;
    };
    const auto update = input(uInput, bit);
    const auto address = input(aInput, addrType);
    const auto data = input(dInput, bit);
    const auto data2 = input(d2Input, bit);
    const auto event = input(eInput, bit);
    const auto zero = m.addValue(bit);
    m.addOperation("core.compute.constant", {}, std::array{zero}, {},
                   std::array{Parameter{m.intern("value"), std::string("1'b0")}});
    const auto mask = m.addValue(bit);
    m.addOperation("core.compute.constant", {}, std::array{mask}, {},
                   std::array{Parameter{m.intern("value"), std::string("1'b1")}});
    for (uint32_t row = 0; row < 2; ++row) {
        const auto q = m.addState("q" + std::to_string(row), bit);
        const std::array initSteps{InitStep{m.intern("core.init.const"), {0, 1}}};
        const std::array initParams{Parameter{m.intern("value"), std::string("1'b0")}};
        m.addInit(q, initSteps, initParams);
        const auto old = m.addValue(bit);
        m.addOperation("core.state.read", {}, std::array{old}, std::array<ObjectRef, 1>{ObjectRef::state(q)});
        const auto rowConstant = m.addValue(addrType);
        m.addOperation("core.compute.constant", {}, std::array{rowConstant}, {},
                       std::array{Parameter{m.intern("value"), std::to_string(2) + "'d" + std::to_string(row)}});
        const auto hit = m.addValue(bit);
        m.addOperation("core.compute.eq", std::array{address, rowConstant}, std::array{hit});
        const auto lowNext = m.addValue(bit);
        m.addOperation("core.compute.mux", std::array{hit, data, old}, std::array{lowNext});
        const auto highNext = m.addValue(bit);
        m.addOperation("core.compute.mux", std::array{hit, data2, lowNext}, std::array{highNext});
        m.addOperation("core.state.regWrite", std::array{update, highNext, mask, event}, {},
                       std::array<ObjectRef, 1>{ObjectRef::state(q)},
                       std::array{Parameter{m.intern("event_edges"), std::vector<std::string>{"posedge"}}});
    }
    diag::Diagnostics d;
    RegToMemOptions options; options.minElementCount = 2;
    options.enableCostSelection = false;
    options.enableSameAddressFusion = false;
    RegToMemPass pass(options);
    const auto result = pass.run(m, d);
    require(result.success && result.changed, "reg-to-mem did not rewrite update-guarded rows");
    require(!d.hasError(), "reg-to-mem emitted diagnostics for update-guarded rows");
    require(m.states().size() == 1, "reg-to-mem did not compact the scalar rows");
    bool foundArray = false, foundWrite = false, foundSequence = false, guardConjoined = false;
    for (const auto &state : m.states())
        foundArray |= m.types()[state.type.index - 1].kind == TypeKind::Array;
    std::vector<OpId> producers(m.values().size() + 1);
    for (const auto &op : m.operations()) {
        if (m.text(op.opType) == "core.state.memWrite" || m.text(op.opType) == "core.state.memWriteSeq") {
            foundWrite = true;
            const auto operands = m.operands(op);
            if (!operands.empty() && m.text(op.opType) == "core.state.memWriteSeq") {
                for (std::size_t i = 0; i + 2 < operands.size(); i += 3) {
                    if (producers[operands[i].index]) {
                        const auto &guard = m.operations()[producers[operands[i].index].index - 1];
                        guardConjoined |= m.text(guard.opType) == "core.compute.logicAnd";
                    }
                }
            }
        }
        foundSequence |= m.text(op.opType) == "core.state.memWriteSeq";
        for (auto value : m.results(op)) producers[value.index] = op.id;
    }
    require(foundArray && foundWrite && foundSequence && guardConjoined,
            (std::cerr << "array=" << foundArray << " write=" << foundWrite << " seq=" << foundSequence
                        << " guard=" << guardConjoined << '\n',
             "reg-to-mem lost the update condition on the recovered write"));
    for (const auto &op : m.operations())
        if (m.text(op.opType) == "core.state.memWriteSeq")
            require(m.operands(op).size() == 7,
                    "reg-to-mem duplicated scalar rows instead of recovering one dynamic write source");
    require(verifyGrhSimModel(m, defaultDialectRegistry(), d), "rewritten update-guarded model invalid");
}
void inspect(const GrhSimModel &m, std::string_view pattern) {
    std::vector<OpId> defs(m.values().size()+1);
    for (const auto &op:m.operations()) for (auto v:m.results(op)) defs[v.index]=op.id;
    std::set<uint32_t> targets;
    for (const auto &s:m.states()) if (m.text(s.name).find(pattern)!=std::string_view::npos) {
        std::cout << "STATE "<<s.id.index<<' '<<m.text(s.name)<<" type="<<s.type.index<<'\n';
        if (targets.size()<2) targets.insert(s.id.index);
    }
    for(auto id:targets) {
        const auto &state=m.states()[id-1];
        const auto &type=m.types()[state.type.index-1];
        if(type.kind!=TypeKind::Array) continue;
        std::size_t reads=0,writes=0,fills=0,sequences=0,triples=0;
        for(const auto &op:m.operations()) {
            const auto refs=m.objectRefs(op);
            if(refs.empty() || refs[0]!=ObjectRef::state(state.id)) continue;
            const auto k=m.text(op.opType);
            reads+=k=="core.state.memRead";
            writes+=k=="core.state.memWrite";
            fills+=k=="core.state.memFill";
            if(k=="core.state.memWriteSeq") {
                ++sequences;
                std::size_t events=0;
                for(const auto &p:m.parameters(op)) if(m.text(p.name)=="event_edges")
                    events=std::get<std::vector<std::string>>(p.value).size();
                triples+=(m.operands(op).size()-events)/3;
            }
        }
        std::cout<<"ARRAY rows="<<type.count<<" reads="<<reads<<" writes="<<writes
            <<" fills="<<fills<<" sequences="<<sequences<<" triples="<<triples<<'\n';
    }
    std::set<uint32_t> seen;
    std::function<void(ValueId,unsigned)> dump=[&](ValueId v,unsigned depth) {
        if (!defs[v.index] || depth>7 || seen.size()>180 || !seen.insert(v.index).second) return;
        const auto &op=m.operations()[defs[v.index].index-1];
        std::cout<<std::string(depth*2,' ')<<'%'<<v.index<<" = "<<m.text(op.opType)<<" (";
        unsigned shown=0;
        for(auto x:m.operands(op)) {
            if(shown++==12) {std::cout<<"...";break;}
            std::cout<<x.index<<',';
        }
        std::cout<<")";
        for(auto ref:m.objectRefs(op)) std::cout<<" @"<<ref.index;
        for(const auto &p:m.parameters(op)) {
            std::cout<<' '<<m.text(p.name)<<'=';
            if(auto s=std::get_if<std::string>(&p.value)) std::cout<<*s;
            if(auto n=std::get_if<int64_t>(&p.value)) std::cout<<*n;
        }
        std::cout<<'\n';
        if(m.text(op.opType)=="core.compute.concat") return;
        for(auto x:m.operands(op)) dump(x,depth+1);
    };
    if(pattern.starts_with("%")) {
        dump({static_cast<uint32_t>(std::stoul(std::string(pattern.substr(1)))),0},0);
        return;
    }
    for(const auto &op:m.operations()) if(m.text(op.opType)=="core.state.regWrite" &&
        targets.contains(m.objectRefs(op)[0].index)) {
        std::cout<<"WRITE "<<op.id.index<<'\n';
        auto current=m.operands(op)[1];
        for(unsigned i=0;i<600;++i) {
            const auto &d=m.operations()[defs[current.index].index-1];
            if(m.text(d.opType)!="core.compute.mux") { std::cout<<"FALLBACK %"<<current.index<<'\n';break; }
            auto a=m.operands(d); std::cout<<"BRANCH "<<i<<" guard="<<a[0].index<<" data="<<a[1].index<<'\n';
            current=a[2];
        }
        for(auto v:m.operands(op)) dump(v,0);
    }
}
void summarize(const GrhSimModel &m) {
    struct Counts { uint64_t fixed=0,dynamic=0,writes=0,sequences=0,triples=0,fills=0; };
    std::vector<Counts> counts(m.states().size()+1);
    std::vector<bool> constant(m.values().size()+1);
    for(const auto &op:m.operations()) if(m.text(op.opType)=="core.compute.constant")
        for(auto v:m.results(op)) constant[v.index]=true;
    for(const auto &op:m.operations()) {
        auto refs=m.objectRefs(op);auto args=m.operands(op);
        if(refs.empty() || refs[0].kind!=ObjectKind::State) continue;
        auto &c=counts[refs[0].index];const auto k=m.text(op.opType);
        if(k=="core.state.memRead") {
            if(constant[args[0].index]) ++c.fixed; else ++c.dynamic;
        }
        if(k=="core.state.memWrite") ++c.writes;
        if(k=="core.state.memFill") ++c.fills;
        if(k=="core.state.memWriteSeq") {
            ++c.sequences;
            std::size_t events=0;
            for(const auto &p:m.parameters(op)) if(m.text(p.name)=="event_edges")
                events=std::get<std::vector<std::string>>(p.value).size();
            c.triples+=(args.size()-events)/3;
        }
    }
    std::cout<<"state\trows\twidth\tfixed_reads\tdynamic_reads\twrites\tsequences\ttriples\tfills\n";
    for(const auto &state:m.states()) {
        const auto &t=m.types()[state.type.index-1];
        if(t.kind!=TypeKind::Array || !m.text(state.name).starts_with("__reg_to_mem_")) continue;
        const auto &c=counts[state.id.index];
        std::cout<<m.text(state.name)<<'\t'<<t.count<<'\t'<<m.types()[t.elementType.index-1].width
            <<'\t'<<c.fixed<<'\t'<<c.dynamic<<'\t'<<c.writes<<'\t'<<c.sequences<<'\t'<<c.triples<<'\t'<<c.fills<<'\n';
    }
}
}
int main(int argc,char **argv) {
    try {
        if(argc==3 && std::string_view(argv[1])=="--summarize") {
            diag::Diagnostics d; auto m=loadGrhSimModel(argv[2],defaultDialectRegistry(),d);
            require(bool(m),"load failed"); summarize(*m);return 0;
        }
        if(argc==3 && std::string_view(argv[1])=="--emit-checks") {
            regToMemEmitChecks(argv[2]); return 0;
        }
        if(argc==4 && (std::string_view(argv[1])=="--analyze" || std::string_view(argv[1])=="--rewrite")) {
            diag::Diagnostics d; auto m=loadGrhSimModel(argv[2],defaultDialectRegistry(),d);
            require(bool(m),"load failed");
            const auto oldStates=m->states().size(), oldOps=m->operations().size();
            RegToMemOptions options; options.analysisOnly=std::string_view(argv[1])=="--analyze"; options.report=argv[3];
            RegToMemPass pass(options); require(pass.run(*m,d).success,"reg-to-mem failed");
            require(verifyGrhSimModel(*m,defaultDialectRegistry(),d),"reg-to-mem produced invalid model");
            std::cout<<"states "<<oldStates<<" -> "<<m->states().size()<<" ops "<<oldOps<<" -> "<<m->operations().size()<<'\n';
            return 0;
        }
        if(argc==4 && std::string_view(argv[1])=="--inspect") {
            diag::Diagnostics d; auto m=loadGrhSimModel(argv[2],defaultDialectRegistry(),d);
            require(bool(m),"load failed"); inspect(*m,argv[3]); return 0;
        }
        compactTest(); updateConditionTest(); regToMemSemanticsTests(); std::cout<<"reg-to-mem tests passed\n"; return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
