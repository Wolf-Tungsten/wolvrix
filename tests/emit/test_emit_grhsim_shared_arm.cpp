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
#include <sstream>
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
        session.insert_or_assign("top.activity_schedule."+n,std::make_unique<SessionSlotValue<T>>(std::move(v),"shared-arm-test"));
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
struct CaseInfo { ValueId shared; OperationId root; unsigned rounds; };
CaseInfo build(Fixture &f, int width, const std::string &kind) {
    auto clk=f.input("clk",1),aux=f.input("aux",1),rst=f.input("rst",1),en=f.input("en",1);
    auto x=f.input("x",width),y=f.input("y",width),sel=f.input("sel",1),shift=f.input("shift",8),mask=f.input("mask",width);
    f.reg("q",width,true);f.reg("r",width,true);
    auto q=f.regRead("q_read","q",width,0,true),r=f.regRead("r_read","r",width,0,true);
    f.g.bindOutputPort("q_out",q);f.g.bindOutputPort("r_out",r);
    auto one=f.constant("one",1,"1'b1"),zero=f.constant("zero",width,std::to_string(width)+"'d0");
    auto all=f.constant("all",width,std::to_string(width)+"'hffffffffffffffff");
    if(kind=="cached") {y=zero;shift=f.constant("shift3",8,"8'd3");r=q;}
    const auto chain=[&](std::string prefix,ValueId seed,unsigned rounds,unsigned salt) {
        for(unsigned i=0;i<rounds;++i) {
            auto c=f.constant(prefix+"c"+std::to_string(i),width,std::to_string(width)+"'d"+std::to_string(salt+i));
            seed=f.op(OperationKind::kAdd,prefix+"a"+std::to_string(i),width,{seed,c},0,true);
            seed=f.op(OperationKind::kXor,prefix+"x"+std::to_string(i),width,{seed,y},0,true);
            seed=f.op(OperationKind::kAShr,prefix+"s"+std::to_string(i),width,{seed,shift},0,true);
        }
        return seed;
    };
    auto seed=f.op(OperationKind::kXor,"seed",width,{q,x},0,true);
    const unsigned rounds=kind=="oversize"?90:4;
    auto shared=chain("shared",seed,rounds,3);
    ValueId trueValue;
    if(kind=="duplicate") {
        auto twice=f.op(OperationKind::kAdd,"twice",width,{shared,shared},0,true);
        trueValue=chain("dup",twice,8,41);
    } else {
        auto left=chain("left",shared,4,17),right=chain("right",shared,4,83);
        trueValue=f.op(OperationKind::kXor,"true_value",width,{left,right},0,true);
    }
    auto falseValue=chain("false",kind=="other-arm"?shared:r,8,101);
    if(kind=="observed") f.g.bindOutputPort("watch",shared);
    if(kind=="cross-group") {
        auto watch=f.op(OperationKind::kNot,"watch",width,{shared},1,true);f.g.bindOutputPort("watch",watch);
    }
    if(kind=="selector") sel=f.op(OperationKind::kReduceOr,"shared_select",1,{shared});
    if(kind=="effect") f.g.setAttr(f.g.valueDef(shared),"hasSideEffects",true);
    auto result=f.op(OperationKind::kMux,"result",width,{sel,trueValue,falseValue},0,true);
    f.g.bindOutputPort("out",result);
    auto nextR=f.op(OperationKind::kXor,"next_r",width,{q,x},2,true);
    auto writeEn=f.op(OperationKind::kMux,"write_en",1,{rst,en,one},2);
    auto writeMask=f.op(OperationKind::kMux,"write_mask",width,{rst,mask,all},2);
    auto qData=f.op(OperationKind::kMux,"q_data",width,{rst,result,zero},2,true);
    auto rData=f.op(OperationKind::kMux,"r_data",width,{rst,nextR,zero},2,true);
    f.write("write_q","q",{writeEn,qData,writeMask,clk,rst},3);
    f.write("write_r","r",{writeEn,rData,writeMask,aux,rst},4);
    // Unscheduled IR uses do not make an emitted local value escape.
    auto dead=f.g.createOperation(OperationKind::kXor,f.g.internSymbol("dead_user"));
    f.g.addOperand(dead,shared);f.g.addOperand(dead,x);f.g.addResult(dead,f.value("dead",width));
    return {shared,f.g.valueDef(result),rounds};
}
using Files=std::map<std::string,std::string>;
Files emit(Fixture &f,const std::filesystem::path &dir,const std::string &mode,bool cached) {
    f.schedule();EmitOptions opts;opts.outputDir=dir;opts.topOverrides={"top"};opts.session=&f.session;opts.sessionPathPrefix="top";
    opts.attributes["waveform"]="off";opts.attributes["conditional_mux_cones"]="1";
    opts.attributes["conditional_mux_input_cache"]=cached?"1":"0";
    if(mode!="default"&&mode!="env")opts.attributes["conditional_mux_shared_arm"]=mode=="on"?"1":"0";
    if(mode=="audit")opts.attributes["conditional_mux_shared_arm_audit"]="1";
    EmitDiagnostics diag;EmitGrhSimCpp emitter(&diag);
    if(!emitter.emit(f.d,opts).success) {
        for(const auto &m:diag.messages())std::cerr<<m.message<<'\n';throw std::runtime_error("emit failed");
    }
    GrhSimExecutionPlan plan;require(emitter.analyzeExecutionPlan(f.d,opts,plan).success,"planner failed");
    Files files;for(const auto &p:std::filesystem::directory_iterator(dir))
        if(p.path().extension()==".cpp"||p.path().extension()==".hpp")files[p.path().filename().string()]=read(p.path());
    return files;
}
std::string strip(const std::string &s) {
    return std::regex_replace(s,std::regex(R"(        // Conditional mux shared arm:[^\n]*\n)"),"");
}
std::map<std::pair<unsigned,unsigned>,std::vector<unsigned>> additions(const Files &files) {
    std::map<std::pair<unsigned,unsigned>,std::vector<unsigned>> result;
    std::regex pattern(R"(Conditional mux shared arm: root=(\d+) arm=(\d+)[^\n]* ops=([^\n]*))");
    for(const auto &[name,s]:files)for(auto it=std::sregex_iterator(s.begin(),s.end(),pattern);it!=std::sregex_iterator();++it) {
        std::vector<unsigned> members;std::stringstream stream((*it)[3].str());std::string n;
        while(std::getline(stream,n,','))if(!n.empty())members.push_back(std::stoul(n));
        result[{std::stoul((*it)[1]),std::stoul((*it)[2])}]=std::move(members);
    }
    return result;
}
void run(const std::filesystem::path &dir,const Files &files,int width,const std::string &kind,unsigned rounds) {
    std::ofstream out(dir/"oracle.cpp");
    out<<"#define W "<<width<<"\n#define ROUNDS "<<rounds<<"\n";
    for(const auto &mode:{"cached","other-arm","selector","duplicate","observed","cross-group"})
        out<<"#define "<<std::regex_replace(std::string(mode),std::regex("-"),"_")<<"_case "<<(kind==mode)<<"\n";
    out<<R"CPP(
#include "grhsim_top.hpp"
#include <cstdint>
constexpr uint64_t limit=~uint64_t(0)>>(64-W);
uint64_t chain(uint64_t a,uint64_t y,unsigned shift,unsigned rounds,unsigned salt){
 for(unsigned i=0;i<rounds;++i){a=((a+salt+i)^y)&limit;bool sign=a>>(W-1);
 if(shift>=W)a=sign?limit:0;else if(shift)a=(a>>shift)|(sign?(limit^(limit>>shift)):0);}
 return a;
}
int main(){GrhSIM_top m;uint64_t random=1231;
for(unsigned init=0;init<2;++init){m.init();m.clk=0;m.aux=0;m.rst=1;m.en=0;m.x=0;m.y=0;m.sel=0;m.shift=0;m.mask=0;m.eval();
uint64_t q=0,r=0;bool prev=false,prevAux=false,prevRst=true;
auto shared=[&](){return chain(q^m.x,cached_case?0:m.y,cached_case?3:m.shift,ROUNDS,3);};
auto value=[&](){uint64_t a=shared();unsigned sh=cached_case?3:m.shift;uint64_t y=cached_case?0:m.y;
uint64_t yes=duplicate_case?chain((a+a)&limit,y,sh,8,41):(chain(a,y,sh,4,17)^chain(a,y,sh,4,83));
uint64_t no=chain(other_arm_case?a:(cached_case?q:r),y,sh,8,101);
return (selector_case?a!=0:bool(m.sel))?yes:no;};
auto step=[&](){uint64_t nextQ=q,nextR=r;bool reset=!m.rst&&prevRst;
if((m.clk&&!prev)||reset){if(!m.rst)nextQ=0;else if(m.en)nextQ=((q&~uint64_t(m.mask))|(value()&m.mask))&limit;}
if((m.aux&&!prevAux)||reset){if(!m.rst)nextR=0;else if(m.en)nextR=((r&~uint64_t(m.mask))|((q^m.x)&m.mask))&limit;}
q=nextQ;r=nextR;prev=m.clk;prevAux=m.aux;prevRst=m.rst;m.eval();
if(uint64_t(m.out)!=value()||uint64_t(m.q_out)!=q||uint64_t(m.r_out)!=r)return false;
#if observed_case
if(uint64_t(m.watch)!=shared())return false;
#endif
#if cross_group_case
if(uint64_t(m.watch)!=((~shared())&limit))return false;
#endif
return true;};
for(unsigned i=0;i<360;++i){random^=random<<13;random^=random>>7;random^=random<<17;
m.x=random&limit;m.y=(random>>11)&limit;m.mask=(random>>7)&limit;m.shift=i%6==0?255:i%6==1?W:i%3;
m.en=i%3!=0;m.rst=i%31!=2;m.clk=i%5==0;m.aux=i%7==0;m.sel=i%2;
if(!step())return 1;for(unsigned repeat=0;repeat<3;++repeat)if(!step())return 2;
m.sel=!m.sel;if(!step())return 3;m.x^=17;if(!step())return 4;}}
return 0;}
)CPP";
    out.close();const char *cxx=std::getenv("CXX");
    for(const char *flags:{"-O0","-O3 -fsanitize=address,undefined -fno-sanitize-recover=all"}) {
        std::string cmd=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
        for(const auto &[name,s]:files)if(name.ends_with(".cpp"))cmd+=" "+quote((dir/name).string());
        cmd+=" "+quote((dir/"oracle.cpp").string())+" -o "+quote((dir/"check").string());
        require(std::system(cmd.c_str())==0,"oracle compile failed");
        require(std::system(quote((dir/"check").string()).c_str())==0,"independent state oracle failed");
    }
}
}
int main(){try{
 const auto base=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"shared-arm";std::filesystem::remove_all(base);
 constexpr const char *env="WOLVRIX_GRHSIM_CONDITIONAL_MUX_SHARED_ARM";::unsetenv(env);
 for(int width:{17,32,64})for(const std::string kind:{"ordinary","duplicate","observed","other-arm","selector","cross-group","oversize","cached","effect"}) {
    if(width!=17&&kind!="ordinary")continue;
    Files off;std::map<std::pair<unsigned,unsigned>,std::vector<unsigned>> audited;
    for(const std::string mode:{"off","audit","on"}) {
        Fixture f;auto info=build(f,width,kind);const auto dir=base/(std::to_string(width)+kind+mode);
        auto files=emit(f,dir,mode,kind=="cached");
        if(mode=="off")off=files;
        if(mode=="audit") {audited=additions(files);for(const auto &[name,s]:files)require(strip(s)==off.at(name),"audit changed generated code");}
        if(mode=="on") {
            const auto added=additions(files);require(added==audited,"audit/implementation membership mismatch");
            const auto key=std::make_pair(info.root.index,0u);
            const auto it=added.find(key);bool found=false;
            if(it!=added.end())found=std::find(it->second.begin(),it->second.end(),f.g.valueDef(info.shared).index)!=it->second.end();
            require(found==(kind=="ordinary"||kind=="duplicate"),"wrong closure/fallback decision "+kind);
            std::string code;for(const auto &[name,s]:files)code+=s;
            if(kind=="cached")require(code.find("cached=1")!=std::string::npos,"cached-root test missed cache");
        }
        if(mode!="audit"&&kind!="effect")run(dir,files,width,kind,info.rounds);
    }
 }
 Fixture def;build(def,17,"ordinary");auto defaults=emit(def,base/"default","default",false);
 Fixture disabled;build(disabled,17,"ordinary");require(defaults==emit(disabled,base/"off","off",false),"default drift");
 ::setenv(env,"1",1);Fixture fromEnv;build(fromEnv,17,"ordinary");auto enabled=emit(fromEnv,base/"env","env",false);
 Fixture explicitOn;build(explicitOn,17,"ordinary");require(enabled==emit(explicitOn,base/"on","on",false),"environment mismatch");
 Fixture explicitOff;build(explicitOff,17,"ordinary");require(defaults==emit(explicitOff,base/"precedence","off",false),"attribute precedence failed");::unsetenv(env);
 std::cout<<"shared-arm closure, snapshots, cache protection and O0/O3 sanitizer oracles passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
