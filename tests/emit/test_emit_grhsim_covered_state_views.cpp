#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <regex>
#include <stdexcept>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::emit;

namespace {
void require(bool ok, const std::string &s) { if (!ok) throw std::runtime_error(s); }
std::string read(const std::filesystem::path &p) {
    std::ifstream in(p);return {std::istreambuf_iterator<char>(in),{}};
}
std::string quote(const std::string &s) {
    std::string r="'";for(char c:s)r+=c=='\''?"'\\''":std::string(1,c);return r+"'";
}
struct Fixture {
    Design d;Graph &g;SessionStore session;ActivityScheduleSupernodeToOps nodes{5};
    std::vector<ValueId> views;
    ActivityScheduleTopoOrder orderOverride;
    Fixture():g(d.createGraph("top")){d.markAsTop("top");}
    ValueId value(std::string n,int w,bool sign=false){return g.createValue(g.internSymbol(n),w,sign,ValueType::Logic);}
    ValueId input(std::string n,int w){auto v=value(n,w);g.bindInputPort(n,v);return v;}
    ValueId op(OperationKind k,std::string n,int w,std::vector<ValueId> args,unsigned group=0,bool sign=false) {
        auto id=g.createOperation(k,g.internSymbol(n+"_op"));for(auto a:args)g.addOperand(id,a);
        auto v=value(n,w,sign);g.addResult(id,v);nodes[group].push_back(id);return v;
    }
    ValueId constant(std::string n,int w,std::string literal,unsigned group=0) {
        auto v=op(OperationKind::kConstant,n,w,{},group);g.setAttr(g.valueDef(v),"constValue",literal);return v;
    }
    void reg(std::string name,int w,bool sign=false) {
        auto id=g.createOperation(OperationKind::kRegister,g.internSymbol(name));g.setAttr(id,"width",int64_t{w});
        g.setAttr(id,"isSigned",sign);g.setAttr(id,"initValue",std::to_string(w)+"'d0");
    }
    ValueId regRead(std::string name,std::string symbol,int w,unsigned group,bool sign=false) {
        auto v=op(OperationKind::kRegisterReadPort,name,w,{},group,sign);g.setAttr(g.valueDef(v),"regSymbol",symbol);return v;
    }
    void write(std::string name,std::string symbol,std::vector<ValueId> args,unsigned group) {
        auto id=g.createOperation(OperationKind::kRegisterWritePort,g.internSymbol(name));
        for(auto v:args)g.addOperand(id,v);g.setAttr(id,"regSymbol",symbol);
        g.setAttr(id,"eventEdge",std::vector<std::string>{"posedge","negedge"});nodes[group].push_back(id);
    }
    template<class T>void slot(std::string n,T v) {
        session.insert_or_assign("top.activity_schedule."+n,std::make_unique<SessionSlotValue<T>>(std::move(v),"covered-view-test"));
    }
    void schedule() {
        std::map<uint32_t,unsigned> owner;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n])owner[id.index]=n;
        ActivityScheduleValueFanout fanout(g.values().size());ActivityScheduleStateReadSupernodes readers;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n]) {
            const auto operation=g.getOperation(id);
            for(auto v:operation.operands()) {
                const auto def=g.valueDef(v);
                if(!def.valid()||!owner.contains(def.index)||owner.at(def.index)!=n) {
                    auto &f=fanout[v.index-1];if(std::find(f.begin(),f.end(),n)==f.end())f.push_back(n);
                }
            }
            if(operation.kind()==OperationKind::kRegisterReadPort) {
                auto attr=operation.attr("regSymbol");readers[std::get<std::string>(*attr)].push_back(n);
            }
        }
        ActivityScheduleTopoOrder order(nodes.size());std::iota(order.begin(),order.end(),0u);
        if(!orderOverride.empty())order=orderOverride;
        slot("supernode_to_ops",nodes);slot("value_fanout",fanout);slot("topo_order",order);slot("state_read_supernodes",readers);
    }
};
void build(Fixture &f,int width,const std::string &kind="ordinary") {
    const bool sign=width==17;
    auto clk=f.input("clk",1),aux=f.input("aux",1),rst=f.input("rst",1),en=f.input("en",1);
    auto data=f.input("data",width),mask=f.input("mask",width),noise=f.input("noise",width);
    auto zero=f.constant("zero",width,std::to_string(width)+"'d0"),one=f.constant("one",1,"1'b1");
    auto all=f.constant("all",width,std::to_string(width)+"'hffffffffffffffff");
    f.reg("q",width,sign);f.reg("r",width);
    auto q=f.regRead("q_producer","q",width,0,sign);
    auto bits=width==1?1:std::min(width-1,8);
    auto slice=f.op(OperationKind::kSliceStatic,"view_slice",bits,{q});
    f.g.setAttr(f.g.valueDef(slice),"sliceStart",int64_t{width==1?0:1});
    f.g.setAttr(f.g.valueDef(slice),"sliceEnd",int64_t{width==1?0:bits});
    auto invert=f.op(OperationKind::kNot,"view_not",width,{q});
    auto logical=f.op(OperationKind::kLogicNot,"view_logicnot",1,{q});
    auto assign=f.op(OperationKind::kAssign,"view_assign",width,{q});
    f.views={slice,invert,logical,assign};
    if(kind=="observed")for(unsigned i=0;i<f.views.size();++i)f.g.bindOutputPort("watch"+std::to_string(i),f.views[i]);
    auto enable=f.op(OperationKind::kLogicOr,"enable",1,{en,f.op(OperationKind::kLogicNot,"reset",1,{rst})});
    auto next=f.op(OperationKind::kMux,"next_q",width,{rst,data,zero});
    auto selectedMask=f.op(OperationKind::kMux,"selected_mask",width,{rst,mask,all});
    f.write("write_q","q",{enable,next,selectedMask,clk,rst},3);
    if(kind=="two-writers")f.write("write_q_again","q",{enable,next,selectedMask,aux,rst},4);
    auto direct=kind=="missing-reader"?noise:f.regRead("q_consumer","q",width,1,sign);
    auto sum=f.op(OperationKind::kXor,"sum",width,{direct,noise},1);f.g.bindOutputPort("sum",sum);
    if(kind=="effect")f.g.setAttr(f.g.valueDef(sum),"hasSideEffects",true);
    for(unsigned i=0;i<f.views.size();++i) {
        auto used=f.op(OperationKind::kXor,"used"+std::to_string(i),width,{f.views[i],noise},1);
        f.g.bindOutputPort("y"+std::to_string(i),used);
    }
    if(kind=="decode-key")for(unsigned i=0;i<16;++i) {
        auto k=f.constant("decode_k"+std::to_string(i),bits,std::to_string(bits)+"'d"+std::to_string(i),1);
        auto p=f.op(OperationKind::kEq,"decode_p"+std::to_string(i),1,{slice,k},1);
        f.g.bindOutputPort("decode"+std::to_string(i),p);
    }
    auto r=f.regRead("r_read","r",width,2);f.g.bindOutputPort("r_out",r);
    auto nextR=f.op(OperationKind::kMux,"next_r",width,{rst,sum,zero},1);
    f.write("write_r","r",{one,nextR,all,aux,rst},4);
    if(kind=="commit-use") {
        // One of the views also supplies a commit operand and must be retained.
        auto extra=f.g.createOperation(OperationKind::kRegister,f.g.internSymbol("extra"));
        f.g.setAttr(extra,"width",int64_t{width});f.g.setAttr(extra,"isSigned",false);
        f.write("write_extra","extra",{one,assign,all,aux,rst},4);
    }
    if(kind=="event-use") {
        auto extra=f.g.createOperation(OperationKind::kRegister,f.g.internSymbol("extra"));
        f.g.setAttr(extra,"width",int64_t{width});f.g.setAttr(extra,"isSigned",false);
        f.write("write_extra","extra",{one,data,all,logical,rst},4);
    }
}
std::map<std::string,std::string> emit(Fixture &f,const std::filesystem::path &dir,const std::string &mode,
    GrhSimExecutionPlan &plan,const std::map<std::string,std::string> &extra={}) {
    f.schedule();EmitOptions opts;opts.outputDir=dir;opts.session=&f.session;opts.sessionPathPrefix="top";
    opts.topOverrides={"top"};opts.attributes["waveform"]=mode=="waveform"?"declared-symbols":"off";
    if(mode!="default"&&mode!="env")opts.attributes["covered_state_view_notifications"]=mode=="off"||mode=="precedence"||mode=="inline-only"?"0":"1";
    opts.attributes["covered_state_view_inline"]=mode=="notify"?"0":"1";
    opts.attributes["shared_equality_families"]="1";
    if(mode=="fullpass")opts.attributes["input_fullpass_specialization"]="1";
    if(mode=="change-cache")opts.attributes["conditional_mux_change_cache"]="1";
    if(mode=="dirty")opts.attributes["producer_dirty_words"]="1";
    for(const auto &[key,value]:extra)opts.attributes[key]=value;
    EmitDiagnostics diag;EmitGrhSimCpp emitter(&diag);
    if(!emitter.emit(f.d,opts).success) {
        // Builds without libfst must reject a real waveform request before
        // lowering; builds with libfst exercise the conservative fallback.
        if(mode=="waveform")for(const auto &d:diag.messages())
            if(d.message.find("built without libfst support")!=std::string::npos)return {};
        for(const auto &d:diag.messages())std::cerr<<d.message<<'\n';throw std::runtime_error("emit failed");
    }
    require(emitter.analyzeExecutionPlan(f.d,opts,plan).success,"planner failed");
    std::map<std::string,std::string> files;
    for(const auto &p:std::filesystem::directory_iterator(dir))
        if(p.path().extension()==".cpp"||p.path().extension()==".hpp")files[p.path().filename().string()]=read(p.path());
    return files;
}
std::size_t countViews(const std::map<std::string,std::string> &files) {
    std::size_t result=0;for(const auto &[n,s]:files)for(std::size_t p=0;(p=s.find("// Covered state view:",p))!=std::string::npos;++p)++result;return result;
}
std::map<std::string,std::vector<uint32_t>> states(const GrhSimExecutionPlan &p) {
    std::map<std::string,std::vector<uint32_t>> result;for(const auto &s:p.states)result[s.symbol]=s.targets;return result;
}
const char *oracle=R"CPP(#include "grhsim_top.hpp"
#include "visits.hpp"
#include <cstdint>
#include <cstdio>
int main(){GrhSIM_top m;constexpr uint64_t mask=WIDTH==64?UINT64_MAX:((UINT64_C(1)<<WIDTH)-1);
 uint64_t seed=123;auto rand=[&](){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
 for(unsigned reset=0;reset<3;++reset){m.init();m.clk=0;m.aux=0;m.rst=1;m.en=0;m.noise=0;m.data=0;m.mask=mask;m.eval();
 uint64_t q=0,r=0;bool oldClk=0,oldAux=0,oldRst=1;
 for(unsigned i=0;i<1024;++i){m.clk=i%4==1;m.aux=i%7==1;m.rst=i%31!=2;m.en=i%3!=0;
 m.data=rand()&mask;m.mask=rand()&mask;m.noise=rand()&mask;
 if((m.aux&&!oldAux)||(!m.rst&&oldRst))r=m.rst?(q^m.noise):0;
 if((m.clk&&!oldClk)||(!m.rst&&oldRst)) {if(!m.rst)q=0;else if(m.en)q=(q&~uint64_t(m.mask))|(uint64_t(m.data)&m.mask);}
 oldClk=m.clk;oldAux=m.aux;oldRst=m.rst;
 for(unsigned again=0;again<3;++again){m.eval();const unsigned n=WIDTH==1?1:WIDTH-1<8?WIDTH-1:8;
 uint64_t slice=(q>>(WIDTH==1?0:1))&((UINT64_C(1)<<n)-1);
 if(m.y0!=(slice^m.noise)||m.y1!=((~q&mask)^m.noise)||m.y2!=((q==0)^m.noise)||m.y3!=(q^m.noise))return 1;
 if(m.sum!=(q^m.noise)||m.r_out!=r)return 2;m.noise=rand()&mask;}
 }}for(auto n:visits::calls)std::printf("%llu ",static_cast<unsigned long long>(n));std::puts("");})CPP";
std::string run(const std::filesystem::path &dir,const std::map<std::string,std::string> &files,int width,
    const std::string &extraOracle={},const std::string &replacementOracle={},unsigned visitCount=5) {
    std::ofstream(dir/"visits.hpp")<<"#pragma once\nnamespace visits {inline unsigned long long calls["<<visitCount<<"]{};}\n";
    for(const auto &[name,s]:files)if(name.ends_with(".cpp")) {
        auto text=std::regex_replace(s,std::regex("(// Supernode ([0-9]+):[^\\n]*\\n\\s*if \\([^\\n]*\\) \\{)"),
            "$1\n++visits::calls[$2];");
        std::ofstream(dir/name)<<"#include \"visits.hpp\"\n"<<text;
    }
    std::string program=replacementOracle.empty()?oracle:replacementOracle;
    program.insert(program.find(" if(m.sum"),extraOracle);
    std::ofstream(dir/"harness.cpp")<<"#define WIDTH "<<width<<'\n'<<program;
    const auto compiler=std::getenv("CXX");std::string result;
    for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-sanitize-recover=all"}) {
        std::string cmd=quote(compiler?compiler:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
        for(const auto &[name,s]:files)if(name.ends_with(".cpp"))cmd+=" "+quote((dir/name).string());
        cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
        require(std::system(cmd.c_str())==0,"compile failed");
        require(std::system((quote((dir/"check").string())+" > "+quote((dir/"visits.txt").string())).c_str())==0,"oracle failed: "+dir.string());
        auto observed=read(dir/"visits.txt");if(!result.empty())require(result==observed,"O0/O3 execution counts differ");result=observed;
    }
    return result;
}

std::vector<ValueId> addExpressions(Fixture &f,int width,const std::string &kind="ordinary") {
    const auto assign=f.g.getOperation(f.g.valueDef(f.views[3]));const auto q=assign.operands()[0];
    const unsigned bits=width==1?1:std::min(width-1,8);
    auto complement=f.op(OperationKind::kNot,"complement",bits,{f.views[0]});
    auto slicedNot=f.op(OperationKind::kSliceStatic,"sliced_not",bits,{f.views[1]});
    f.g.setAttr(f.g.valueDef(slicedNot),"sliceStart",int64_t{0});
    f.g.setAttr(f.g.valueDef(slicedNot),"sliceEnd",int64_t{bits-1});
    auto k=f.constant("eq_constant",width,std::to_string(width)+(kind=="unknown"?"'bx":"'d3"));
    auto equal=f.op(OperationKind::kEq,"equal",1,{q,k});
    auto again=f.regRead("another_q","q",width,0,width==17);
    auto low=f.op(OperationKind::kSliceStatic,"low",bits,{again});
    f.g.setAttr(f.g.valueDef(low),"sliceStart",int64_t{0});
    f.g.setAttr(f.g.valueDef(low),"sliceEnd",int64_t{bits-1});
    auto concat=f.op(OperationKind::kConcat,"parts",bits*2,{f.views[0],low});
    auto reduction=f.op(OperationKind::kReduceOr,"reduction",1,{concat});
    auto deep=f.views[3];
    for(unsigned i=0;i<5;++i)deep=f.op(OperationKind::kNot,"deep"+std::to_string(i),width,{deep});
    std::vector<ValueId> roots{complement,slicedNot,equal,reduction,deep};
    if(kind=="public")f.g.bindOutputPort("observed_expression",complement);
    if(kind=="multiple-states") {
        auto other=f.regRead("other_r","r",width,0);
        f.regRead("other_r_consumer","r",width,1);
        roots.push_back(f.op(OperationKind::kAnd,"mixed_states",width,{q,other}));
    }
    // The independent unrelated input makes consumers execute while q is held.
    ValueId noise;
    for(const auto &port:f.g.inputPorts())if(f.g.symbolText(f.g.valueSymbol(port.value))=="noise")noise=port.value;
    require(noise.valid(),"noise input missing");
    for(unsigned i=0;i<roots.size();++i) {
        auto used=f.op(OperationKind::kXor,"expression_use"+std::to_string(i),width,{roots[i],noise},1);
        f.g.bindOutputPort("z"+std::to_string(i),used);
    }
    return roots;
}

void testExpressions() {
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"state-expressions";
    std::filesystem::remove_all(root);
    const char *key="WOLVRIX_GRHSIM_COVERED_STATE_VIEW_COMPOSITION";::unsetenv(key);
    const std::string oracleExtra=R"CPP(
 uint64_t bitMask=(UINT64_C(1)<<n)-1;
 if(m.z0!=((~slice&bitMask)^m.noise)||m.z1!=(((~q)&bitMask)^m.noise)||
    m.z2!=((q==(UINT64_C(3)&mask))^m.noise)||m.z3!=(((slice|(q&bitMask))!=0)^m.noise)||
    m.z4!=((~q&mask)^m.noise))return 3;
)CPP";
    for(int width:{1,17,64}) {
        std::string baselineCounts;GrhSimExecutionPlan baseline;
        std::map<std::string,std::map<std::string,std::string>> sources;
        for(const std::string stage:{"base","notify","inline","env","precedence","parent-off"}) {
            Fixture f;build(f,width);const auto roots=addExpressions(f,width);GrhSimExecutionPlan plan;
            std::map<std::string,std::string> options{{"covered_state_view_composition",stage=="base"||stage=="precedence"?"0":"1"},
                {"covered_state_view_composition_inline",stage=="notify"?"0":"1"}};
            if(stage=="env"||stage=="precedence")::setenv(key,"1",1);
            if(stage=="env")options.erase("covered_state_view_composition");
            if(stage=="parent-off")options["covered_state_view_notifications"]="0";
            const auto dir=root/(std::to_string(width)+stage);
            auto files=emit(f,dir,"inline",plan,options);::unsetenv(key);
            if(stage=="base")baseline=plan;
            require(states(plan)==states(baseline),"composition changed state frontier");
            uint64_t composed=0,inlined=0;
            for(const auto &entry:plan.computeLowering) {
                if(entry.features.contains("covered_state_view_composition"))composed+=entry.features.at("covered_state_view_composition");
                if(entry.features.contains("covered_state_view_inlined_definitions"))inlined+=entry.features.at("covered_state_view_inlined_definitions");
            }
            const bool enabled=stage=="notify"||stage=="inline"||stage=="env";
            require(composed==(enabled?5u:0u),"composition coverage: "+stage);
            require(inlined==(stage=="parent-off"?0u:stage=="inline"||stage=="env"?8u:4u),"composition inline budget: "+stage);
            sources[stage]=files;
            if(stage=="base"||stage=="notify"||stage=="inline") {
                const auto calls=run(dir,files,width,oracleExtra);
                if(stage=="base")baselineCounts=calls;else require(calls==baselineCounts,"composition changed execution counts");
            }
        }
        require(sources["base"]==sources["precedence"],"explicit composition off lost precedence");
        require(sources["inline"]==sources["env"],"composition environment drift");
    }
    for(const std::string kind:{"unknown","public","multiple-states","decode-key"}) {
        Fixture f;build(f,17,kind=="decode-key"?kind:"ordinary");auto roots=addExpressions(f,17,kind);GrhSimExecutionPlan plan;
        auto files=emit(f,root/kind,"inline",plan,{{"covered_state_view_composition","1"},{"covered_state_view_composition_inline","1"}});
        uint64_t composed=0;for(const auto &entry:plan.computeLowering)
            if(entry.features.contains("covered_state_view_composition"))composed+=entry.features.at("covered_state_view_composition");
        require(composed==(kind=="multiple-states"?5u:kind=="decode-key"?3u:4u),"unsafe composition accepted: "+kind);
    }
    std::cout<<"state expressions: oracle, ownership, budget and unchanged counts passed\n";
}

void testCrossRegion() {
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"cross-region-state-views";
    std::filesystem::remove_all(root);
    const std::string oracleExtra=R"CPP(
 uint64_t bitMask=(UINT64_C(1)<<n)-1;
 if(m.z0!=((~slice&bitMask)^m.noise)||m.z1!=(((~q)&bitMask)^m.noise)||
    m.z2!=((q==(UINT64_C(3)&mask))^m.noise)||m.z3!=(((slice|(q&bitMask))!=0)^m.noise)||
    m.z4!=((~q&mask)^m.noise))return 3;
)CPP";
    auto arrange=[](Fixture &f,int width,const std::string &kind) {
        build(f,width);auto roots=addExpressions(f,width,kind=="unknown"?kind:"ordinary");
        // Original producer 0 -> extracted roots 1 -> original consumer 2.
        // Independent register reads in 1 and 2 are real graph users, not
        // synthetic activation metadata supplied only to make the proof pass.
        auto old=f.nodes;f.nodes.assign(6,{});
        f.nodes[0]=old[0];f.nodes[2]=old[1];f.nodes[3]=old[2];f.nodes[4]=old[3];f.nodes[5]=old[4];
        for(auto value:roots) {
            if(value==roots[2])continue; // Keep raw register read local; stored read views remain protected.
            auto id=f.g.valueDef(value);auto &group=f.nodes[0];group.erase(std::find(group.begin(),group.end(),id));
            f.nodes[1].push_back(id);
        }
        if(kind!="missing-reader") {
            auto q=f.regRead("q_middle","q",width,1,width==17);
            ValueId noise;for(const auto &port:f.g.inputPorts())
                if(f.g.symbolText(f.g.valueSymbol(port.value))=="noise")noise=port.value;
            auto used=f.op(OperationKind::kXor,"middle_use",width,{q,noise},1);
            f.g.bindOutputPort("middle_read",used);
        }
        if(kind=="public")f.g.bindOutputPort("public_root",roots[0]);
        if(kind=="effect")f.g.setAttr(f.g.valueDef(f.views[0]),"hasSideEffects",true);
        if(kind=="decode-key") {
            auto bits=f.g.valueWidth(f.views[0]);
            for(unsigned i=0;i<16;++i) {
                auto k=f.constant("protect_k"+std::to_string(i),bits,std::to_string(bits)+"'d"+std::to_string(i),1);
                auto p=f.op(OperationKind::kEq,"protect_p"+std::to_string(i),1,{f.views[0],k},1);
                f.g.bindOutputPort("protect"+std::to_string(i),p);
            }
        }
        return roots;
    };
    auto crosses=[](const std::map<std::string,std::string> &files) {
        std::map<unsigned,std::string> sites;
        const std::regex pattern("Cross-region state expression: value=([0-9]+) ops=[0-9]+ mode=(inline|notify)");
        for(const auto &[name,text]:files)
            for(auto i=std::sregex_iterator(text.begin(),text.end(),pattern);i!=std::sregex_iterator();++i)
                sites.emplace(std::stoul((*i)[1]),(*i)[2]);
        return sites;
    };
    for(int width:{1,17,64}) {
        std::string baselineCalls;GrhSimExecutionPlan baseline;
        std::map<std::string,std::string> baselineSource;
        for(const std::string stage:{"off","notify","inline","parent-off"}) {
            Fixture f;auto roots=arrange(f,width,"ordinary");GrhSimExecutionPlan plan;
            auto dir=root/(std::to_string(width)+stage);
            std::map<std::string,std::string> options{{"covered_state_view_composition","1"},
                {"covered_state_view_composition_inline",stage=="notify"?"0":"1"},
                {"covered_state_view_cross_region",stage=="off"?"0":"1"}};
            if(stage=="parent-off")options["covered_state_view_composition"]="0";
            auto files=emit(f,dir,"inline",plan,options);const auto selected=crosses(files);
            if(stage=="off"){baseline=plan;baselineSource=files;}
            require(states(plan)==states(baseline),"cross-region changed state activation frontier");
            if(stage=="off"||stage=="parent-off")require(selected.empty(),"disabled cross-region proof applied");
            else {
                // complement, sliced-not, reduction and deep chain cross a group.
                // Equal-to-constant also has its register read upstream, and may
                // conservatively fall back if direct-read lowering stores it.
                for(unsigned i:{0u,1u,3u,4u})require(selected.contains(roots[i].index),"cross-region fixture missing proof");
                require(selected.at(roots[4].index)=="notify","deep cross-region expression bypassed inline budget");
            }
            if(stage!="parent-off") {
                auto calls=run(dir,files,width,oracleExtra,{},6);
                if(stage=="off")baselineCalls=calls;else require(calls==baselineCalls,"cross-region changed execution counts");
            }
        }
    }
    for(const std::string kind:{"missing-reader","effect","public","unknown","decode-key"}) {
        Fixture f;auto roots=arrange(f,17,kind);GrhSimExecutionPlan plan;
        auto files=emit(f,root/kind,"inline",plan,{{"covered_state_view_composition","1"},
            {"covered_state_view_composition_inline","1"},{"covered_state_view_cross_region","1"}});
        auto selected=crosses(files);
        if(kind=="missing-reader"||kind=="effect")require(selected.empty(),"unsafe producer/frontier accepted");
        if(kind=="public"||kind=="decode-key")require(!selected.contains(roots[0].index),"protected cross-region value accepted");
        if(kind=="unknown")require(!selected.contains(roots[2].index),"unknown literal proof accepted");
    }
    std::cout<<"cross-region expressions: snapshots, activation, ownership and budget passed\n";
}

void testEntryGuard() {
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"compute-entry-guard";
    std::filesystem::remove_all(root);
    std::string program=oracle;
    auto replace=[&](const std::string &before,const std::string &after){
        const auto at=program.find(before);require(at!=std::string::npos,"oracle marker missing");program.replace(at,before.size(),after);
    };
    // No independent state reader at the consumer: it must observe activation
    // produced by the preceding batch in the same round, not a stale snapshot.
    replace("r=m.rst?(q^m.noise):0","r=0");replace("m.sum!=(q^m.noise)","m.sum!=0");
    replace("}}for(auto n",R"CPP(}}
 m.init();m.clk=0;m.aux=0;m.rst=1;m.en=1;m.data=mask;m.mask=mask;m.noise=0;m.eval();
 m.clk=1;m.eval();if(m.y3!=mask)return 4;
 m.clk=0;m.eval();m.data=0;m.eval();if(m.y3!=mask)return 5;
 m.clk=1;m.eval();if(m.y3!=0)return 6;
 for(auto n)CPP");
    std::string counts;
    for(const std::string mode:{"off","on"}) {
        Fixture f;build(f,17,"missing-reader");GrhSimExecutionPlan plan;
        // A dispatch word is indivisible even with max_ops=1. Put the receiver
        // in a second word, and keep the supplied topo order phase-correct.
        f.nodes.resize(14);std::swap(f.nodes[1],f.nodes[13]);
        for(unsigned group:{1u,5u,6u,7u,8u,9u,10u,11u,12u}) {
            auto value=f.constant("padding"+std::to_string(group),1,"1'b1",group);
            f.g.bindOutputPort("padding"+std::to_string(group),value);
        }
        f.orderOverride={0,1,2,5,6,7,8,9,10,11,12,13,3,4};
        auto files=emit(f,root/mode,"off",plan,{{"compute_batch_dispatch_guard",mode=="on"?"1":"0"},
            {"sched_batch_max_ops","1"},{"sched_batch_max_estimated_lines","1"}});
        std::size_t guards=0;
        for(const auto &[name,text]:files)if(name.ends_with("_eval.cpp"))
            for(std::size_t at=0;(at=text.find("if ((supernode_active_curr_[",at))!=std::string::npos;++at)++guards;
        require(mode=="on"?guards>=2:guards==0,"compute call guard emission mismatch");
        const auto observed=run(root/mode,files,17,{},program,14);
        if(mode=="off")counts=observed;else require(observed==counts,"entry guard changed supernode executions");
    }
    std::cout<<"compute entry guard: same-round propagation, two clocks, reset and held-input oracle passed\n";
}
}
int main() {
    try {
        if(std::getenv("WOLVRIX_TEST_CROSS_REGION_VIEWS")){testCrossRegion();return 0;}
        if(std::getenv("WOLVRIX_TEST_COMPUTE_ENTRY_GUARD")){testEntryGuard();return 0;}
        if(std::getenv("WOLVRIX_TEST_STATE_EXPRESSIONS")){testExpressions();return 0;}
        const char *key="WOLVRIX_GRHSIM_COVERED_STATE_VIEW_NOTIFICATIONS";::unsetenv(key);
        const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"covered-state-views";
        std::filesystem::remove_all(root);
        for(int width:{1,17,64}) {
            std::map<std::string,std::string> defaults;GrhSimExecutionPlan baseline;std::string counts;
            for(const std::string mode:{"default","off","notify","inline","env","precedence","inline-only","fullpass","change-cache","dirty","waveform"}) {
                Fixture f;build(f,width);GrhSimExecutionPlan plan;const auto dir=root/(std::to_string(width)+mode);
                if(mode=="env"||mode=="precedence")::setenv(key,"1",1);
                const auto files=emit(f,dir,mode,plan);::unsetenv(key);
                if(mode=="waveform"&&files.empty())continue;
                bool enabled=mode=="notify"||mode=="inline"||mode=="env";
                require(countViews(files)==(enabled?4u:0u),"coverage mismatch: "+mode);
                if(mode=="default"){defaults=files;baseline=plan;}
                if(mode=="off"||mode=="precedence"||mode=="inline-only")require(files==defaults,"default/off mismatch");
                require(states(plan)==states(baseline),"register activation frontier changed");
                uint64_t covered=0,inlined=0;for(const auto &entry:plan.computeLowering) {
                    if(entry.features.contains("covered_state_view_notifications"))covered+=entry.features.at("covered_state_view_notifications");
                    if(entry.features.contains("covered_state_view_inlined_definitions"))inlined+=entry.features.at("covered_state_view_inlined_definitions");
                }
                require(covered==(enabled?4u:0u),"planner coverage mismatch");
                require(inlined==((mode=="inline"||mode=="env")?4u:0u),"planner inline mismatch");
                if(mode=="off"||mode=="notify"||mode=="inline") {
                    const auto observed=run(dir,files,width);
                    if(mode=="off")counts=observed;else require(counts==observed,"consumer/group execution count changed");
                }
            }
        }
        for(const std::string kind:{"missing-reader","observed","effect","two-writers","commit-use","event-use","decode-key"}) {
            Fixture f;build(f,32,kind);GrhSimExecutionPlan plan;
            const auto files=emit(f,root/kind,"inline",plan);
            require(countViews(files)==((kind=="commit-use"||kind=="event-use"||kind=="decode-key")?3u:0u),"unsafe boundary accepted: "+kind);
        }
        std::cout<<"covered state views: identities, unchanged activation counts and independent oracle passed\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
