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
#include <sstream>
#include <stdexcept>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::emit;

namespace {
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
std::string quote(const std::string &s) {
    std::string result="'"; for (char c:s) result+=c=='\''?"'\\''":std::string(1,c); return result+"'";
}
std::string read(const std::filesystem::path &p) {
    std::ifstream in(p); return {std::istreambuf_iterator<char>(in),{}};
}
struct Fixture {
    Design design;
    Graph &g;
    SessionStore session;
    ActivityScheduleSupernodeToOps nodes{3};
    Fixture():g(design.createGraph("top")) { design.markAsTop("top"); }
    ValueId value(std::string name,int width,bool sign=false) {
        return g.createValue(g.internSymbol(name),width,sign,ValueType::Logic);
    }
    ValueId input(std::string name,int width,bool sign=false) {
        auto v=value(name,width,sign);g.bindInputPort(name,v);return v;
    }
    ValueId op(OperationKind kind,std::string name,int width,std::vector<ValueId> args,unsigned node=0,bool sign=false) {
        auto id=g.createOperation(kind,g.internSymbol(name+"_op"));
        for(auto v:args)g.addOperand(id,v);
        auto v=value(name,width,sign);g.addResult(id,v);nodes[node].push_back(id);return v;
    }
    ValueId constant(std::string name,int width,std::string literal,unsigned node=0) {
        auto v=op(OperationKind::kConstant,name,width,{},node);g.setAttr(g.valueDef(v),"constValue",literal);return v;
    }
    template<class T>void slot(const std::string &name,T v) {
        session.insert_or_assign("top.activity_schedule."+name,std::make_unique<SessionSlotValue<T>>(std::move(v),"consumer-test"));
    }
    void schedule() {
        std::map<uint32_t,unsigned> owner;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n])owner[id.index]=n;
        ActivityScheduleValueFanout fanout(g.values().size());
        ActivityScheduleStateReadSupernodes reads;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n]) {
            const auto operation=g.getOperation(id);
            for(auto v:operation.operands()) {
                auto def=g.valueDef(v);
                if(!def.valid() || !owner.contains(def.index) || owner.at(def.index)!=n) {
                    auto &targets=fanout[v.index-1];
                    if(std::find(targets.begin(),targets.end(),n)==targets.end())targets.push_back(n);
                }
            }
            if(operation.kind()==OperationKind::kRegisterReadPort)reads["q"].push_back(n);
        }
        ActivityScheduleTopoOrder order(nodes.size());std::iota(order.begin(),order.end(),0u);
        slot("supernode_to_ops",nodes);slot("value_fanout",fanout);slot("topo_order",order);slot("state_read_supernodes",reads);
    }
};
struct Emitted {std::map<std::string,std::string> files;std::string text;uint64_t wide=0,sets=0;};
Emitted emit(Fixture &f,const std::filesystem::path &dir,const std::string &feature,const std::string &mode) {
    f.schedule();EmitOptions options;options.outputDir=dir;options.session=&f.session;options.sessionPathPrefix="top";
    options.attributes["waveform"]="off";options.topOverrides={"top"};
    options.attributes["conditional_mux_cones"]=feature=="conditional_mux_wide" && mode!="parent-off"?"1":"0";
    if(mode!="default" && mode!="env")options.attributes[feature]=mode=="off" || mode=="precedence"?"0":"1";
    if(mode=="fullpass")options.attributes["input_fullpass_specialization"]="1";
    EmitDiagnostics diagnostics;EmitGrhSimCpp emitter(&diagnostics);
    if(!emitter.emit(f.design,options).success) {
        for(const auto &d:diagnostics.messages())std::cerr<<d.message<<'\n';throw std::runtime_error("emit failed");
    }
    GrhSimExecutionPlan plan;require(emitter.analyzeExecutionPlan(f.design,options,plan).success,"planner failed");
    Emitted result;
    for(const auto &entry:plan.computeLowering)for(const auto &[key,count]:entry.features) {
        if(key=="conditional_mux_wide_operations_upper")result.wide+=count;
        if(key=="local_predicate_set_roots")result.sets+=count;
    }
    for(const auto &p:std::filesystem::directory_iterator(dir))
        if(p.path().extension()==".cpp" || p.path().extension()==".hpp") {
            auto content=read(p.path());result.text+=content;result.files[p.path().filename().string()]=content;
        }
    return result;
}
void run(const std::filesystem::path &dir,const Emitted &files,const std::string &harness) {
    std::ofstream(dir/"harness.cpp")<<harness;
    const char *compiler=std::getenv("CXX");
    for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-sanitize-recover=all"}) {
        std::string cmd=quote(compiler?compiler:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
        for(const auto &[name,text]:files.files)if(name.ends_with(".cpp"))cmd+=" "+quote((dir/name).string());
        cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
        require(std::system(cmd.c_str())==0,"compile failed: "+dir.string());
        require(std::system(quote((dir/"check").string()).c_str())==0,"oracle failed: "+dir.string());
    }
}
void wideFixture(Fixture &f,int width,bool observed=false,bool effect=false) {
    auto a=f.input("a",64),b=f.input("b",64),index=f.input("index",32),sel=f.input("sel",1),clk=f.input("clk",1);
    auto tail=f.op(OperationKind::kConcat,"tail",128,{a,b});
    auto wide=tail;
    if(width>128)wide=f.op(OperationKind::kConcat,"wide",width,
        {f.constant("padding",width-128,std::to_string(width-128)+"'d0"),tail});
    auto sliced=f.op(OperationKind::kSliceDynamic,"slice",32,{wide,index});
    f.g.setAttr(f.g.valueDef(sliced),"sliceWidth",int64_t{32});
    if(observed)f.g.bindOutputPort("watch",wide);
    auto data=sliced;
    for(int i=0;i<20;++i) {
        data=f.op(OperationKind::kAdd,"step"+std::to_string(i),32,
            {data,f.constant("salt"+std::to_string(i),32,"32'd"+std::to_string(i+1))});
        if(effect && i==3)f.g.setAttr(f.g.valueDef(data),"hasSideEffects",true);
    }
    auto other=f.constant("other",32,"32'd57");
    auto out=f.op(OperationKind::kMux,"result",32,{sel,data,other});f.g.bindOutputPort("y",out);
    auto reg=f.g.createOperation(OperationKind::kRegister,f.g.internSymbol("q"));
    f.g.setAttr(reg,"width",int64_t{32});f.g.setAttr(reg,"isSigned",false);f.g.setAttr(reg,"initValue",std::string("32'd0"));
    auto q=f.op(OperationKind::kRegisterReadPort,"q_read",32,{},1);f.g.setAttr(f.g.valueDef(q),"regSymbol",std::string("q"));
    f.g.bindOutputPort("q_out",q);
    auto en=f.constant("en",1,"1'b1"),mask=f.constant("mask",32,"32'hffffffff");
    auto write=f.g.createOperation(OperationKind::kRegisterWritePort,f.g.internSymbol("write_q"));
    for(auto v:{en,out,mask,clk})f.g.addOperand(write,v);
    f.g.setAttr(write,"regSymbol",std::string("q"));f.g.setAttr(write,"eventEdge",std::vector<std::string>{"posedge"});f.nodes[2].push_back(write);
}
const char *wideHarness=R"CPP(#include "grhsim_top.hpp"
#include <cstdint>
int main(){GrhSIM_top m;for(unsigned init=0;init<3;++init){m.init();m.clk=0;m.eval();uint32_t q=0;bool old=false;
 for(unsigned i=0;i<600;++i){m.a=UINT64_C(0xd5ea0f531b923c76)^(uint64_t(i)*237);m.b=UINT64_C(0x178acdead1234567)+i;
 const unsigned indices[]={0,1,31,32,63,64,65,95,96,127,128,2047,2048,UINT32_MAX};m.index=indices[i%14];m.sel=(i/3)%2;m.clk=(i%4)==1;
 auto bits=(__uint128_t(m.a)<<64)|m.b;uint32_t expected=m.sel?(m.index>=128?0:uint32_t(bits>>m.index))+210:57;
 if(m.clk&&!old)q=expected;old=m.clk;m.eval();if(m.y!=expected||m.q_out!=q)return 1;
 m.sel=!m.sel;expected=m.sel?(m.index>=128?0:uint32_t(bits>>m.index))+210:57;m.eval();if(m.y!=expected||m.q_out!=q)return 2;
 }}return 0;})CPP";
void wideTests(const std::filesystem::path &base) {
    const char *key="WOLVRIX_GRHSIM_CONDITIONAL_MUX_WIDE";::unsetenv(key);
    for(int width:{128,2048}) {
        std::map<std::string,std::string> defaults;
        for(const std::string mode:{"default","off","on","env","precedence","parent-off","fullpass"}) {
            Fixture f;wideFixture(f,width);const auto dir=base/(std::to_string(width)+mode);
            if(mode=="env"||mode=="precedence")::setenv(key,"1",1);
            auto files=emit(f,dir,"conditional_mux_wide",mode);::unsetenv(key);
            const bool enabled=mode=="on"||mode=="env"||mode=="fullpass";
            require((files.text.find("// Conditional wide operation:")!=std::string::npos)==enabled,"wide coverage: "+mode);
            if(mode!="fullpass")require((files.wide>0)==enabled,"wide planner drift");
            if(mode=="default")defaults=files.files;
            if(mode=="off"||mode=="precedence")require(files.files==defaults,"wide off/default drift");
            if(mode=="on"||mode=="off"||mode=="fullpass")run(dir,files,wideHarness);
        }
    }
    for(const std::string mode:{"effect","observed","oversize"}) {
        Fixture f;wideFixture(f,mode=="oversize"?8192:2048,mode=="observed",mode=="effect");
        auto files=emit(f,base/mode,"conditional_mux_wide","on");
        require(files.wide==(mode=="observed"?1u:0u),"wide escape/effect/budget fallback");
        if(mode=="observed")run(base/mode,files,wideHarness);
    }
}
void predicateFixture(Fixture &f,const std::string &kind) {
    int width=kind=="u64"?64:kind=="full"?4:12;
    auto a=f.input("a",width,kind=="signed"),b=f.input("b",width,kind=="signed"),residual=f.input("residual",1);
    // Unrelated live two-input logic must survive local DCE. In particular,
    // never take an operand span from a destroyed temporary Operation.
    auto noise=f.input("noise",1);
    auto untouched=f.op(OperationKind::kLogicOr,"untouched_or",1,{noise,residual},1);
    untouched=f.op(OperationKind::kLogicNot,"untouched_not",1,{untouched},1);
    f.g.bindOutputPort("untouched",untouched);
    auto source=a;
    if(kind=="stored") {
        source=f.op(OperationKind::kXor,"stored",width,{a,f.constant("salt",width,std::to_string(width)+"'d3")},0);
        f.g.bindOutputPort("source_watch",source);
    }
    std::vector<ValueId> leaves;
    unsigned count=kind=="small"?15:kind=="full"?16:32;
    for(unsigned family=0;family<(kind=="u64"||kind=="full"?1u:2u);++family)
    for(unsigned i=0;i<count;++i) {
        uint64_t k=kind=="u64"?(i<16?i:UINT64_MAX-(i-16)):kind=="full"?i:(100+family*100)+(kind=="sparse"?i*7:i);
        auto name="p"+std::to_string(family)+"_"+std::to_string(i);
        auto constant=f.constant(name+"k",width,std::to_string(width)+"'d"+std::to_string(k),1);
        auto eq=f.op(OperationKind::kEq,name,1,i%2?std::vector<ValueId>{constant,family?b:source}:
            std::vector<ValueId>{family?b:source,constant},1);
        if(kind=="effect"&&i==3)f.g.setAttr(f.g.valueDef(eq),"hasSideEffects",true);
        if(kind=="shared"&&family==0&&i==0)f.g.bindOutputPort("watch",eq);
        leaves.push_back(eq);
    }
    ValueId result;
    if(kind=="concat") {
        auto packed=f.op(OperationKind::kConcat,"packed",static_cast<int>(leaves.size()),leaves,1);
        result=f.op(OperationKind::kReduceOr,"reduce",1,{packed},1);
    } else {
        result=leaves.front();for(unsigned i=1;i<leaves.size();++i)
            result=f.op(OperationKind::kLogicOr,"union"+std::to_string(i),1,{result,leaves[i]},1);
    }
    result=f.op(OperationKind::kOr,"with_residual",1,{result,residual},1);f.g.bindOutputPort("y",result);
}
void predicateTests(const std::filesystem::path &base) {
    const char *key="WOLVRIX_GRHSIM_LOCAL_PREDICATE_SETS";::unsetenv(key);
    for(const std::string kind:{"dense","concat","shared","stored","full","u64","sparse","small","signed","effect"}) {
        const bool positive=kind!="sparse"&&kind!="small"&&kind!="signed"&&kind!="effect";
        std::map<std::string,std::string> defaults;
        for(const std::string mode:{"default","off","on","env","precedence","fullpass"}) {
            if(!positive&&mode!="on")continue;
            Fixture f;predicateFixture(f,kind);const auto dir=base/(kind+mode);
            if(mode=="env"||mode=="precedence")::setenv(key,"1",1);
            auto files=emit(f,dir,"local_predicate_sets",mode);::unsetenv(key);
            const bool enabled=positive&&(mode=="on"||mode=="env"||mode=="fullpass");
            require((files.text.find("// Local predicate set:")!=std::string::npos)==enabled,"set coverage: "+kind+mode);
            if(mode!="fullpass")require((files.sets>0)==enabled,"set planner drift");
            if(mode=="default")defaults=files.files;
            if(mode=="off"||mode=="precedence")require(files.files==defaults,"predicate off/default drift");
            if(!positive||(mode!="off"&&mode!="on"&&mode!="fullpass"))continue;
            std::string harness="#include \"grhsim_top.hpp\"\n#include <cstdint>\nint main(){GrhSIM_top m;for(unsigned init=0;init<2;++init){m.init();for(unsigned i=0;i<8192;++i){";
            if(kind=="u64")harness+="m.a=i<4096?uint64_t(i):UINT64_MAX-(i-4096);m.b=0;";
            else if(kind=="stored")harness+="m.a=i%4096;m.b=(i*13)%8192;";
            else harness+="m.a=i;m.b=(i*13)%8192;";
            harness+="for(unsigned r=0;r<2;++r){m.residual=r;m.noise=i%3==0;m.eval();if(m.untouched!=!(m.noise||m.residual))return 4;";
            if(kind=="u64")harness+="bool expected=m.a<=15||m.a>=UINT64_MAX-15||r;";
            else if(kind=="full")harness+="bool expected=m.a<=15||r;";
            else harness+="auto source="+std::string(kind=="stored"?"((m.a^3u)&4095u)":"m.a")+";bool expected=(source>=100&&source<=131)||(m.b>=200&&m.b<=231)||r;";
            harness+="if(m.y!=expected)return 1;";
            if(kind=="shared")harness+="if(m.watch!=(m.a==100))return 2;";
            harness+="m.eval();if(m.y!=expected)return 3;}}}return 0;}";
            run(dir,files,harness);
        }
    }
}
}
int main(int argc,char **argv) {
    try {
        require(argc==2,"usage: consumer-structure wide|predicates");
        const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/(std::string("consumer-structure-")+argv[1]);
        std::filesystem::remove_all(root);
        if(std::string(argv[1])=="wide")wideTests(root);else predicateTests(root);
        std::cout<<argv[1]<<" structure/oracle/fallback checks passed\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
