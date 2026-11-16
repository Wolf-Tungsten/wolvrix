#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::emit;

namespace {
std::string read(const std::filesystem::path &p) {
    std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}};
}
std::string quote(const std::string &s) {
    std::string r="'"; for (char c:s) r += c=='\'' ? "'\\''" : std::string(1,c); return r+"'";
}
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Design fixture(int width, int rounds=8, bool observed=false, bool effect=false, int cacheSeeds=0) {
    Design d; auto &g=d.createGraph("top"); d.markAsTop("top");
    auto value=[&](std::string name, int w, bool sign=false) {
        return g.createValue(g.internSymbol(name),w,sign,ValueType::Logic);
    };
    auto input=[&](std::string name, int w, bool sign=false) {
        auto v=value(name,w,sign); g.bindInputPort(name,v); return v;
    };
    auto op=[&](OperationKind kind, std::string name, int w, bool sign, std::vector<ValueId> args) {
        auto id=g.createOperation(kind,g.internSymbol(name+"_op"));
        for(auto v:args) g.addOperand(id,v);
        auto v=value(name,w,sign);g.addResult(id,v);return v;
    };
    auto constant=[&](std::string name,int w,std::string literal) {
        auto v=op(OperationKind::kConstant,name,w,false,{});
        g.setAttr(g.valueDef(v),"constValue",literal);return v;
    };
    auto clk=input("clk",1), sel=input("sel",3), x=input("x",width,true), y=input("y",width,true);
    auto shift=input("shift",8);
    if(cacheSeeds) {
        auto noise=input("noise",1);
        auto out=op(OperationKind::kNot,"noise_out",1,false,{noise});g.bindOutputPort("noise_out",out);
    }
    auto reg=g.createOperation(OperationKind::kRegister,g.internSymbol("q"));
    g.setAttr(reg,"width",int64_t{width});g.setAttr(reg,"isSigned",true);
    g.setAttr(reg,"initValue",std::to_string(width)+"'d0");
    auto q=op(OperationKind::kRegisterReadPort,"q_read",width,true,{});
    g.setAttr(g.valueDef(q),"regSymbol",std::string("q"));g.bindOutputPort("q_out",q);
    auto chainY=y;
    auto chainShift=shift,chainSel=sel;
    auto extraValue=ValueId::invalid();
    auto groupedSeed=q;
    if(cacheSeeds>=5) {
        groupedSeed=op(OperationKind::kXor,"group_seed",width,true,{q,x});
        chainY=op(OperationKind::kNot,"group_y",width,true,{y});
        auto aux=input("aux_clk",1);
        auto auxReg=g.createOperation(OperationKind::kRegister,g.internSymbol("aux"));
        g.setAttr(auxReg,"width",int64_t{width});g.setAttr(auxReg,"isSigned",true);
        g.setAttr(auxReg,"initValue",std::to_string(width)+"'d0");
        auto auxRead=op(OperationKind::kRegisterReadPort,"aux_read",width,true,{});
        g.setAttr(g.valueDef(auxRead),"regSymbol",std::string("aux"));g.bindOutputPort("aux_out",auxRead);
        auto data=op(OperationKind::kXor,"aux_data",width,true,{groupedSeed,chainY});
        if(cacheSeeds==6) {
            auto extra=input("extra",width), shiftMask=input("shift_mask",8), selMask=input("sel_mask",3);
            extraValue=op(OperationKind::kNot,"extra_value",width,true,{extra});
            chainShift=op(OperationKind::kXor,"shared_shift",8,false,{shift,shiftMask});
            chainSel=op(OperationKind::kXor,"shared_sel",3,false,{sel,selMask});
            data=op(OperationKind::kXor,"aux_shift_data",width,true,{data,chainShift});
            data=op(OperationKind::kXor,"aux_sel_data",width,true,{data,chainSel});
            data=op(OperationKind::kXor,"aux_extra_data",width,true,{data,extraValue});
        }
        auto en=constant("aux_en",1,"1'b1"),mask=constant("aux_mask",width,std::to_string(width)+"'hffffffff");
        auto write=g.createOperation(OperationKind::kRegisterWritePort,g.internSymbol("write_aux"));
        for(auto v:{en,data,mask,aux})g.addOperand(write,v);
        g.setAttr(write,"regSymbol",std::string("aux"));g.setAttr(write,"eventEdge",std::vector<std::string>{"posedge"});
    }
    auto chain=[&](std::string prefix, ValueId seed, unsigned salt) {
        for(int i=0;i<rounds;++i) {
            auto k=constant(prefix+"_k"+std::to_string(i),width,std::to_string(width)+"'d"+std::to_string(salt+i));
            seed=op(OperationKind::kAdd,prefix+"_add"+std::to_string(i),width,true,{seed,k});
            seed=op(OperationKind::kXor,prefix+"_xor"+std::to_string(i),width,true,{seed,chainY});
            seed=op(OperationKind::kAShr,prefix+"_ashr"+std::to_string(i),width,true,{seed,chainShift});
            if(effect && i==0) g.setAttr(g.valueDef(seed),"hasSideEffects",true);
        }
        return seed;
    };
    auto other=q;
    if(cacheSeeds>=3) {
        auto r=g.createOperation(OperationKind::kRegister,g.internSymbol("other"));
        g.setAttr(r,"width",int64_t{width});g.setAttr(r,"isSigned",true);
        g.setAttr(r,"initValue",std::to_string(width)+"'d7");
        other=op(OperationKind::kRegisterReadPort,"other_read",width,true,{});
        g.setAttr(g.valueDef(other),"regSymbol",std::string("other"));
    }
    auto seed=cacheSeeds>=5?groupedSeed:cacheSeeds>=2?q:x;
    if(cacheSeeds==4) {
        seed=op(OperationKind::kXor,"stored_seed",width,true,{q,x});
        g.bindOutputPort("seed_watch",seed);
    }
    auto a=chain("a",seed,17),b=chain("b",cacheSeeds==1?x:other,83);
    // An unreachable IR user must not prevent delaying a value whose only
    // emitted consumer is the mux. The scheduler omits this dead expression.
    op(OperationKind::kXor,"dead_user",width,true,{a,b});
    if(observed) {g.bindOutputPort("a_watch",a);g.bindOutputPort("b_watch",b);}
    auto mux=op(OperationKind::kMux,"select",width,true,{chainSel,a,b});
    g.bindOutputPort("out",mux);
    auto write=g.createOperation(OperationKind::kRegisterWritePort,g.internSymbol("write_q"));
    auto en=constant("one",1,"1'b1"), mask=constant("mask",width,std::to_string(width)+"'hffffffffffffffff");
    auto writeData=cacheSeeds==6?op(OperationKind::kXor,"write_extra_data",width,true,{mux,extraValue}):mux;
    for(auto v:{en,writeData,mask,clk})g.addOperand(write,v);
    g.setAttr(write,"regSymbol",std::string("q"));g.setAttr(write,"eventEdge",std::vector<std::string>{"posedge"});
    return d;
}

const char *harness=R"CPP(
#include "grhsim_top.hpp"
#include <cstdint>
#include <iostream>
constexpr unsigned width=TEST_WIDTH;
constexpr uint64_t mask=~uint64_t(0)>>(64-width);
uint64_t chain(uint64_t x,uint64_t y,unsigned shift,unsigned salt) {
    for(unsigned i=0;i<8;++i) {
        x=((x+salt+i)^y)&mask;
        const bool negative=(x>>(width-1))!=0;
        if(shift>=width) x=negative?mask:0;
        else if(shift) x=(x>>shift) | (negative ? (mask^(mask>>shift)) : 0);
    }
    return x;
}
int main() {
    GrhSIM_top m; m.init(); m.eval();
    uint64_t q=0,random=0x1234567812345678ULL; bool previous=false;
    for(unsigned i=0;i<600;++i) {
        random^=random<<13;random^=random>>7;random^=random<<17;
        const uint64_t x=random&mask,y=(random>>17)&mask;
        const unsigned sh=i%7==0?255:i%7==1?width:i%7==2?0:i%3;
        const unsigned sel=(i/2)%8;const bool clk=(i%5)==0;
        m.x=x;m.y=y;m.shift=sh;m.sel=sel;m.clk=clk;
        if(clk&&!previous) q=sel?chain(x,y,sh,17):chain(q,y,sh,83);
        m.eval();previous=clk;
        auto expected=sel?chain(x,y,sh,17):chain(q,y,sh,83);
        if((uint64_t(m.out)&mask)!=expected || (uint64_t(m.q_out)&mask)!=q) {
            std::cerr<<"mismatch "<<width<<" step "<<i<<'\n';return 1;
        }
        // Change only the condition with the clock held. This catches stale
        // skipped arms even when all arithmetic inputs remain unchanged.
        m.sel=sel?0:7;m.eval();
        expected=sel?chain(q,y,sh,83):chain(x,y,sh,17);
        if((uint64_t(m.out)&mask)!=expected) return 2;
    }
}
)CPP";

std::map<std::string,std::string> emit(Design &d,const std::filesystem::path &dir,
                                      std::string mode, bool split=false, std::string cacheMode={}, bool jointTesting=false) {
    SessionStore session;PassManager pm;pm.options().session=&session;
    ActivityScheduleOptions schedule;schedule.path="top";
    schedule.maxOpInComputeNode=split?4:4096;schedule.maxOpInComputeSupernode=split?4:8192;
    schedule.eventRegions=!split;
    schedule.eventRegionsShared=cacheMode.rfind("group-",0)==0;
    if(schedule.eventRegionsShared){schedule.maxOpInComputeNode=4;schedule.maxOpInComputeSupernode=8;}
    pm.addPass(std::make_unique<ActivitySchedulePass>(schedule));
    PassDiagnostics pd;require(pm.run(d,pd).success,"schedule failed");
    EmitOptions opts;opts.outputDir=dir;opts.topOverrides={"top"};opts.session=&session;
    opts.attributes["waveform"]="off";
    if(mode!="default")opts.attributes["conditional_mux_cones"]=mode;
    if(cacheMode.rfind("group-",0)==0) {
        opts.attributes["conditional_mux_change_cache"]=cacheMode=="group-change-off"?"0":"1";
        opts.attributes["conditional_mux_input_cache"]="0";
        if(cacheMode!="group-default" && cacheMode!="group-env")
            opts.attributes["conditional_mux_change_grouped"]=cacheMode=="group-off" || cacheMode=="group-precedence"?"0":"1";
    } else if(cacheMode.rfind("change-",0)==0) {
        if(cacheMode!="change-default" && cacheMode!="change-env")
            opts.attributes["conditional_mux_change_cache"]=cacheMode=="change-off" || cacheMode=="change-precedence"?"0":"1";
        opts.attributes["conditional_mux_input_cache"]="0";
    } else if(!cacheMode.empty() && cacheMode!="default" && cacheMode!="env")
        opts.attributes["conditional_mux_input_cache"]=cacheMode=="off" || cacheMode=="precedence"?"0":"1";
    if(cacheMode=="fullpass" || cacheMode=="change-fullpass") {
        opts.attributes["input_fullpass_specialization"]="1";
        opts.attributes["posedge_fullpass_specialization"]="1";
    }
    if(jointTesting) {
        opts.attributes["conditional_mux_change_grouped"]="1";
        if(cacheMode!="group-default" && cacheMode!="group-env")
            opts.attributes["conditional_mux_change_joint"]=cacheMode=="group-off" || cacheMode=="group-precedence"?"0":"1";
        if(cacheMode=="group-grouped-off")opts.attributes["conditional_mux_change_grouped"]="0";
    }
    EmitDiagnostics ed;
    if(!EmitGrhSimCpp(&ed).emit(d,opts).success) {
        for(const auto &e:ed.messages())std::cerr<<e.message<<'\n';throw std::runtime_error("emit failed");
    }
    GrhSimExecutionPlan plan;
    require(EmitGrhSimCpp(&ed).analyzeExecutionPlan(d,opts,plan).success,"execution plan failed");
    for (const auto &entry : plan.computeLowering) {
        if (entry.features.contains("conditional_mux_roots"))
            require(!entry.unmodeledWork.empty(),"conditional work was priced as unconditional");
        if(entry.features.contains("conditional_mux_input_cache_roots"))
            require(entry.features.at("conditional_mux_input_cache_keys")<=4*entry.features.at("conditional_mux_input_cache_roots"),"cache key budget mismatch");
    }
    std::map<std::string,std::string> files;
    for(const auto &p:std::filesystem::directory_iterator(dir))
        if(p.path().extension()==".cpp" || p.path().extension()==".hpp")
            if(p.path().filename()!="harness.cpp")files[p.path().filename().string()]=read(p.path());
    return files;
}
std::size_t cones(const std::map<std::string,std::string> &files) {
    std::size_t n=0;for(const auto &[name,s]:files)for(std::size_t at=0;(at=s.find("// Conditional mux cone:",at))!=std::string::npos;++at)++n;return n;
}

int runInputCacheTests() {
    constexpr const char *key="WOLVRIX_GRHSIM_CONDITIONAL_MUX_INPUT_CACHE";
    const char *old=std::getenv(key);const std::string saved=old?old:"";const bool had=old;
    ::unsetenv(key);
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"grhsim_mux_input_cache";
    std::filesystem::remove_all(root);
    for(int seeds:{1,2}) {
        std::map<std::string,std::string> defaults;
        for(const std::string mode:{"default","off","on","fullpass","env","precedence","parent-off"}) {
            const auto dir=root/(std::to_string(seeds)+mode);auto d=fixture(32,8,false,false,seeds);
            if(mode=="env" || mode=="precedence")::setenv(key,"1",1);
            auto files=emit(d,dir,mode=="parent-off"?"off":"on",false,mode);
            ::unsetenv(key);
            const bool enabled=mode=="on" || mode=="fullpass" || mode=="env";
            std::size_t cached=0;
            for(const auto &[name,text]:files)for(std::size_t at=0;(at=text.find("// Conditional mux input cache:",at))!=std::string::npos;++at)++cached;
            require(enabled?cached>=1:cached==0,"input cache coverage mismatch");
            if(mode=="default") {defaults=files;continue;}
            if(mode=="off" || mode=="precedence")require(files==defaults,"input cache default/off drift");
            if(mode=="env" || mode=="precedence" || mode=="parent-off")continue;
            std::ofstream(dir/"memo_test_counters.hpp")<<"#pragma once\nnamespace memo_test { inline unsigned visits=0, misses=0; }\n";
            for(const auto &[name,text]:files)if(std::filesystem::path(name).extension()==".cpp") {
                auto instrumented=std::regex_replace(text,std::regex("(if \\(grhsim_mux_cache_miss_[0-9]+\\) \\{)"),
                    "++memo_test::visits;\n$1\n++memo_test::misses;");
                std::ofstream(dir/name)<<"#include \"memo_test_counters.hpp\"\n"<<instrumented;
            }
            std::string code=harness;
            const auto begin=code.find("int main()");require(begin!=std::string::npos,"missing oracle entry");code.resize(begin);
            code+="int main(){GrhSIM_top m;for(unsigned reset=0;reset<3;++reset){m.init();uint64_t q=0;bool previous=false;"
                  "for(unsigned i=0;i<300;++i){m.x=(i/4)*37;m.y=(i/8)*11;m.shift=i%11==0?255:i%7==0?32:i%3;"
                  "m.sel=(i/2)%8;m.clk=i%5==0;uint64_t seed=SEEDS==1?m.x:q;"
                  "if(m.clk&&!previous)q=chain(seed,m.y,m.shift,m.sel?17:83);previous=m.clk;"
                  "for(unsigned n=0;n<4;++n){m.noise=n%2;m.eval();seed=SEEDS==1?m.x:q;"
                  "if(m.out!=chain(seed,m.y,m.shift,m.sel?17:83)||m.q_out!=q)return 1;}}"
                  "m.clk=0;m.x=0;m.y=0;m.shift=0;m.sel=1;m.eval();"
                  "for(unsigned n=0;n<4;++n){m.noise=n%2;m.eval();uint64_t seed=SEEDS==1?0:q;"
                  "if(m.out!=chain(seed,0,0,17))return 2;} }"
                  "if(EXPECT_CACHE && (!memo_test::visits || !memo_test::misses))return 3;"
                  "if(EXPECT_HITS && memo_test::visits<=memo_test::misses)return 4;}\n";
            std::ofstream(dir/"harness.cpp")<<"#define TEST_WIDTH 32\n#define SEEDS "<<seeds
                <<"\n#define EXPECT_CACHE "<<enabled<<"\n#define EXPECT_HITS "<<(mode=="fullpass")
                <<"\n#include \"memo_test_counters.hpp\"\n"<<code;
            const char *cxx=std::getenv("CXX");
            for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"}) {
                std::string cmd=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
                for(const auto &[name,text]:files)if(std::filesystem::path(name).extension()==".cpp")cmd+=" "+quote((dir/name).string());
                cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
                require(std::system(cmd.c_str())==0,"cache generated compile failed");
                require(std::system(quote((dir/"check").string()).c_str())==0,"cache oracle or hit check failed");
            }
        }
    }
    // Five independent leaves exceed the fixed budget. Observable intermediate
    // values, effects and small cones retain existing conditional fallback.
    for(const std::string test:{"five-inputs","distinct-state","observed","effect","small","split"}) {
        auto d=fixture(32,test=="small"?3:8,test=="observed",test=="effect",
            test=="five-inputs"?0:test=="distinct-state"?3:1);
        const auto files=emit(d,root/test,"on",test=="split","on");
        for(const auto &[name,text]:files)require(text.find("// Conditional mux input cache:")==std::string::npos,"unsafe or expensive cache accepted");
    }
    if(had)::setenv(key,saved.c_str(),1);
    std::cout<<"mux input cache oracle, hits and fallbacks passed\n";return 0;
}

int runChangeTriggerCacheTests() {
    constexpr const char *key="WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_CACHE";
    const char *old=std::getenv(key);const std::string saved=old?old:"";const bool had=old;
    ::unsetenv(key);
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"grhsim_mux_change_cache";
    std::filesystem::remove_all(root);
    // Case 4 deliberately lacks a tracked change boundary: require fallback.
    for(int seeds:{3,4}) {
    std::map<std::string,std::string> disabled;
    for(const std::string mode:{"change-default","change-off","change-on","change-env","change-precedence","change-parent-off","change-fullpass"}) {
        const auto dir=root/(std::to_string(seeds)+mode);auto d=fixture(32,8,false,false,seeds);
        if(mode=="change-env" || mode=="change-precedence")::setenv(key,"1",1);
        auto files=emit(d,dir,mode=="change-parent-off"?"off":"on",false,mode);
        ::unsetenv(key);
        std::size_t sites=0;
        for(const auto &[name,text]:files)for(std::size_t at=0;(at=text.find("// Conditional mux change-trigger cache:",at))!=std::string::npos;++at)++sites;
        const bool enabled=seeds==3 && (mode=="change-on" || mode=="change-env");
        require(enabled?sites>=1:sites==0,"change-trigger cache coverage mismatch");
        if(mode=="change-default") {disabled=files;continue;}
        if(mode=="change-off" || mode=="change-precedence")require(files==disabled,"change-trigger default/off drift");
        if(mode!="change-on" && mode!="change-env")continue;
        std::ofstream(dir/"memo_test_counters.hpp")<<"#pragma once\nnamespace memo_test { inline unsigned visits=0, misses=0; }\n";
        for(const auto &[name,text]:files)if(std::filesystem::path(name).extension()==".cpp") {
            auto instrumented=std::regex_replace(text,std::regex("(if \\(conditional_mux_change_dirty_\\[[0-9]+u\\]\\) \\{)"),
                "++memo_test::visits;\n$1\n++memo_test::misses;");
            std::ofstream(dir/name)<<"#include \"memo_test_counters.hpp\"\n"<<instrumented;
        }
        std::string code=harness;const auto begin=code.find("int main()");require(begin!=std::string::npos,"missing change-cache oracle entry");code.resize(begin);
        code+="int main(){GrhSIM_top m;for(unsigned reset=0;reset<3;++reset){m.init();m.clk=0;m.noise=0;m.x=0;m.y=0;m.shift=0;m.sel=0;m.eval();uint64_t q=0;bool previous=false;"
              "auto expected=[&](){return m.sel?chain(SEEDS==4?(q^m.x):q,m.y,m.shift,17):chain(7,m.y,m.shift,83);};"
              "auto step=[&](){if(m.clk&&!previous)q=expected();previous=m.clk;m.eval();return m.out==expected()&&m.q_out==q;};"
              "for(unsigned i=0;i<100;++i){m.x=(i/4)*37;m.y=(i/8)*11;m.shift=i%9==0?255:i%3;m.sel=(i/4)%8;m.clk=i%13==0;"
              "if(!step())return 1;for(unsigned n=0;n<4;++n){m.noise=n%2;if(!step())return 2;}"
              "m.clk=0;if(!step())return 3;m.clk=1;if(!step())return 4;"
              "m.clk=0;if(!step())return 5;m.x^=0x12345678;if(!step())return 6;"
              "m.sel=m.sel?0:7;if(!step())return 7;} }"
              "if(EXPECT_CACHE && (!memo_test::visits||!memo_test::misses||memo_test::misses>=memo_test::visits))return 8;}\n";
        std::ofstream(dir/"harness.cpp")<<"#define TEST_WIDTH 32\n#define SEEDS "<<seeds<<"\n#define EXPECT_CACHE "<<enabled<<"\n#include \"memo_test_counters.hpp\"\n"<<code;
        const char *cxx=std::getenv("CXX");
        for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"}) {
            std::string cmd=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
            for(const auto &[name,text]:files)if(std::filesystem::path(name).extension()==".cpp")cmd+=" "+quote((dir/name).string());
            cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
            require(std::system(cmd.c_str())==0,"change-cache generated compile failed");
            require(std::system(quote((dir/"check").string()).c_str())==0,"change-cache oracle/hit check failed");
        }
    }
    }
    for(const std::string test:{"observed","effect","small","split"}) {
        auto d=fixture(32,test=="small"?3:8,test=="observed",test=="effect",3);
        const auto files=emit(d,root/test,"on",test=="split","change-on");
        for(const auto &[name,text]:files)require(text.find("// Conditional mux change-trigger cache:")==std::string::npos,"unsafe change cache accepted");
    }
    if(had)::setenv(key,saved.c_str(),1);
    std::cout<<"mux change-trigger cache oracle and invalidation passed\n";return 0;
}
int runGroupedNotifyTests(bool joint=false) {
    const char *key=joint?"WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_JOINT":"WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_GROUPED";
    const char *old=std::getenv(key);const std::string saved=old?old:"";const bool had=old;
    ::unsetenv(key);
    const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/(joint?"grhsim_mux_joint_notify":"grhsim_mux_grouped_notify");
    std::filesystem::remove_all(root);std::map<std::string,std::string> defaults;
    std::vector<std::string> modes={"group-default","group-off","group-on","group-env","group-precedence","group-parent-off","group-change-off"};
    if(joint)modes.push_back("group-grouped-off");
    for(const std::string mode:modes) {
        const auto dir=root/mode;auto design=fixture(32,8,false,false,joint?6:5);
        if(mode=="group-env" || mode=="group-precedence")::setenv(key,"1",1);
        const auto files=emit(design,dir,mode=="group-parent-off"?"off":"on",false,mode,joint);
        ::unsetenv(key);
        std::size_t groups=0,caches=0;
        for(const auto &[name,source]:files) {
            for(std::size_t i=0;(i=source.find(joint?"// Joint notify plan:":"// Conditional mux notify group:",i))!=std::string::npos;++i)++groups;
            for(std::size_t i=0;(i=source.find("// Conditional mux change-trigger cache:",i))!=std::string::npos;++i)++caches;
        }
        const bool enabled=mode=="group-on" || mode=="group-env";
        if(enabled)require(groups>=1 && caches>=1,"grouped notify fixture did not cover buffering");
        else require(groups==0,"grouped notify bypass ignored");
        if(mode=="group-default")defaults=files;
        if(mode=="group-off" || mode=="group-precedence")require(files==defaults,"grouped notify default/off drift");
        if(mode!="group-on" && mode!="group-off")continue;
        if(joint) {
            std::ofstream(dir/"joint_test_count.hpp")<<"#pragma once\ninline unsigned joint_test_misses=0;\n";
            for(const auto &[name,source]:files)if(std::filesystem::path(name).extension()==".cpp") {
                auto instrumented=std::regex_replace(source,std::regex("(if \\(conditional_mux_change_dirty_\\[[0-9]+u\\]\\) \\{)"),"$1\n++joint_test_misses;");
                std::ofstream(dir/name)<<"#include \"joint_test_count.hpp\"\n"<<instrumented;
            }
        }
        std::string code=harness;code.resize(code.find("int main()"));
        if(joint)code="#include \"joint_test_count.hpp\"\n"+code;
        code+=R"CPP(
int main(){GrhSIM_top m;
for(unsigned reset=0;reset<3;++reset){m.init();
#if TEST_JOINT
m.extra=0;m.shift_mask=0;m.sel_mask=0;
#endif
m.clk=0;m.aux_clk=0;m.x=0;m.y=0;m.sel=0;m.shift=0;m.noise=0;m.eval();
uint64_t q=0,aux=0;bool previous=false,previous_aux=false;
auto select=[&](){unsigned v=m.sel;
#if TEST_JOINT
v^=m.sel_mask;
#endif
return v;};
auto shift_value=[&](){unsigned v=m.shift;
#if TEST_JOINT
v^=m.shift_mask;
#endif
return v;};
auto value=[&](){return select()?chain((q^m.x)&mask,(~uint64_t(m.y))&mask,shift_value(),17):chain(7,(~uint64_t(m.y))&mask,shift_value(),83);};
auto step=[&](){auto next_q=q,next_aux=aux;if(m.clk&&!previous){next_q=value();
#if TEST_JOINT
next_q^=(~uint64_t(m.extra))&mask;
#endif
}
if(m.aux_clk&&!previous_aux) {
next_aux=(q^m.x^~uint64_t(m.y))&mask;
#if TEST_JOINT
next_aux^=((~uint64_t(m.extra))&mask)^shift_value()^select();
#endif
}
q=next_q;aux=next_aux;previous=m.clk;previous_aux=m.aux_clk;m.eval();
return m.out==value()&&m.q_out==q&&m.aux_out==aux;};
for(unsigned i=0;i<160;++i){m.x=(i/3)*31;m.y=(i/7)*83;m.sel=(i/4)%8;m.shift=i%7==0?255:i%3;m.clk=i%5==0;m.aux_clk=i%3==0;
if(!step())return 1;for(unsigned n=0;n<4;++n){m.noise=n%2;if(!step())return 2;}
m.clk=0;m.aux_clk=0;if(!step())return 3;m.clk=1;if(!step())return 4;m.aux_clk=1;if(!step())return 5;
m.x^=0x12345678;if(!step())return 6;m.y^=0x56781234;if(!step())return 7;m.sel=m.sel?0:7;if(!step())return 8;
#if TEST_JOINT
const auto old_misses=joint_test_misses;
m.extra^=0x87654321;if(!step())return 9;
if(joint_test_misses!=old_misses)return 12;
m.shift_mask^=3;if(!step())return 10;m.sel_mask^=5;if(!step())return 11;
#endif
}}
}
)CPP";
        std::ofstream(dir/"harness.cpp")<<"#define TEST_WIDTH 32\n#define TEST_JOINT "<<joint<<"\n"<<code;
        const char *cxx=std::getenv("CXX");
        for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"}) {
            std::string cmd=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
            for(const auto &[name,source]:files)if(std::filesystem::path(name).extension()==".cpp")cmd+=" "+quote((dir/name).string());
            cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
            require(std::system(cmd.c_str())==0,"grouped notify generated compile failed");
            require(std::system(quote((dir/"check").string()).c_str())==0,"grouped notify oracle failed");
        }
    }
    if(joint) {
        auto design=fixture(32,8,false,false,5);
        const auto files=emit(design,root/"no-static-saving","on",false,"group-on",true);
        for(const auto &[name,source]:files)require(source.find("// Joint notify plan:")==std::string::npos,"joint plan ignored static cost gate");
    }
    if(had)::setenv(key,saved.c_str(),1);
    std::cout<<"grouped notify source identity and independent clock oracle passed\n";return 0;
}

}

int main() {
    try {
        if(std::getenv("WOLVRIX_TEST_MUX_JOINT_NOTIFY"))return runGroupedNotifyTests(true);
        if(std::getenv("WOLVRIX_TEST_MUX_GROUPED_NOTIFY"))return runGroupedNotifyTests();
        if(std::getenv("WOLVRIX_TEST_MUX_CHANGE_CACHE"))return runChangeTriggerCacheTests();
        if(std::getenv("WOLVRIX_TEST_MUX_INPUT_CACHE"))return runInputCacheTests();
        const auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"grhsim_conditional_mux";
        std::filesystem::remove_all(root);
        for(int width:{17,32,64}) {
            std::map<std::string,std::string> defaults;
            for(const std::string mode:{"default","off","on"}) {
                auto d=fixture(width);const auto dir=root/(std::to_string(width)+mode);
                auto files=emit(d,dir,mode);
                if(mode=="default") {defaults=files;continue;}
                if(mode=="off") require(files==defaults,"default/off source drift");
                require(cones(files)==(mode=="on"?1u:0u),"wrong conditional cone coverage");
                std::ofstream(dir/"harness.cpp")<<"#define TEST_WIDTH "<<width<<'\n'<<harness;
                const char *cxx=std::getenv("CXX");
                for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"}) {
                    std::string cmd=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
                    for(const auto &[name,text]:files)if(std::filesystem::path(name).extension()==".cpp")cmd+=" "+quote((dir/name).string());
                    cmd+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
                    require(std::system(cmd.c_str())==0,"generated compile failed");
                    require(std::system(quote((dir/"check").string()).c_str())==0,"generated oracle failed");
                }
            }
        }
        for(const std::string test:{"observed","effect","oversize","small","split"}) {
            auto d=fixture(32,test=="oversize"?90:test=="small"?3:8,test=="observed",test=="effect");
            require(cones(emit(d,root/test,"on",test=="split"))==0,"protected cone did not fall back");
        }
        std::cout<<"conditional mux oracle and fallback tests passed\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
