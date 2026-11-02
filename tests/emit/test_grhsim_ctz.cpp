#include "core/store.hpp"
#include "emit/grhsim_activity_model.hpp"
#include "emit/grhsim_cpp.hpp"
#include "emit/system_verilog.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/simplify.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace wolvrix::lib;
using namespace grh;
using namespace emit;
using namespace transform;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}};
}
template<class T> void slot(SessionStore &s, const char *name, T value) {
    s.insert_or_assign(std::string("top.activity_schedule.")+name,
        std::make_unique<SessionSlotValue<T>>(std::move(value), "ctz-test"));
}
void structure(const std::filesystem::path &root) {
    for (int width : {1, 7, 16, 65, 129, 1024}) {
        Design d; auto &g=d.createGraph("top");d.markAsTop(g.symbol());
        auto value=[&](const std::string &n,int w){return g.createValue(g.internSymbol(n),w,false);};
        auto x=value("x",width);g.bindInputPort("x",x);
        auto unused=value("unused",width);g.bindInputPort("unused",unused);
        auto op=[&](OperationKind kind,const std::string &n,std::vector<ValueId> args,int w){
            auto id=g.createOperation(kind,g.internSymbol(n+"_op"));
            for(auto a:args)g.addOperand(id,a);
            auto v=value(n,w);g.addResult(id,v);return v;
        };
        int rw=std::bit_width(unsigned(width));
        auto a=op(OperationKind::kCountTrailingZeros,"_val_a",{x},rw);
        auto b=op(OperationKind::kCountTrailingZeros,"_val_b",{x},rw);
        op(OperationKind::kCountTrailingZeros,"_val_dead",{unused},rw);
        auto zero=op(OperationKind::kConstant,"_val_zero",{},width);
        g.setAttr(g.valueDef(zero),"constValue",std::to_string(width)+"'h0");
        auto c=op(OperationKind::kCountTrailingZeros,"_val_const",{zero},rw);
        auto sum=op(OperationKind::kAdd,"sum",{a,b},rw+1);g.bindOutputPort("sum",sum);
        g.bindOutputPort("constant",c);
        g.setOpSrcLoc(g.valueDef(a),SrcLoc{"Encoder.scala",12,3,12,30,"utility","","CTZ"});
        store::StoreOptions so;so.outputFilename=(root/"roundtrip.json").string();
        check(store::StoreJson().store(d,so).success,"store failed");
        auto json=read(*so.outputFilename);
        check(json.find("kCountTrailingZeros")!=std::string::npos && json.find("Encoder.scala")!=std::string::npos,
              "operation or provenance lost in serialization");
        auto restored=Design::fromJsonString(json);(void)restored;
        GrhSimActivitySeeds seeds;seeds.inputs[x]={{0,unsigned(width),1}};
        seeds.inputs[unused]={{0,unsigned(width),2}};
        GrhSimExecutionPlan plan;plan.graph=g.id();plan.inputs={{x,{}},{unused,{}}};
        auto activity=analyzeGrhSimActivity(g,plan,seeds,GrhSimActivityOptions{});
        check(activity.operationMasks[g.valueDef(a).index]==1,"CTZ acquired unknown/unrelated activity");
        SimplifyOptions opts;opts.semantics=ConstantFoldOptions::Semantics::TwoState;
        PassManager pm;pm.addPass(std::make_unique<SimplifyPass>(opts));PassDiagnostics diag;
        check(pm.run(d,diag).success && !diag.hasError(),"simplify failed");
        unsigned counts=0;
        for(auto id:g.operations())counts+=g.getOperation(id).kind()==OperationKind::kCountTrailingZeros;
        check(counts==1,"CSE, constant folding or DCE failed");
        EmitOptions svOptions;svOptions.outputDir=(root/"unsupported_sv").string();
        EmitDiagnostics svDiag;
        check(!EmitSystemVerilog(&svDiag).emit(d,svOptions).success && svDiag.hasError(),
              "SV emitter silently dropped CTZ");
    }
}
void runtime(const std::filesystem::path &root) {
    Design d;auto &g=d.createGraph("top");d.markAsTop(g.symbol());
    ActivityScheduleSupernodeToOps nodes(3);
    auto val=[&](const std::string &n,int w){return g.createValue(g.internSymbol(n),w,false);};
    auto x=val("x",65),other=val("other",1);g.bindInputPort("x",x);g.bindInputPort("other",other);
    auto op=[&](unsigned node,OperationKind kind,const std::string &n,std::vector<ValueId> args,int w){
        auto id=g.createOperation(kind,g.internSymbol(n+"_op"));for(auto a:args)g.addOperand(id,a);
        auto v=val(n,w);g.addResult(id,v);nodes[node].push_back(id);return v;
    };
    auto count=op(0,OperationKind::kCountTrailingZeros,"count",{x},7);
    auto one=op(1,OperationKind::kConstant,"one",{},8);g.setAttr(g.valueDef(one),"constValue",std::string("8'd1"));
    auto sum=op(1,OperationKind::kAdd,"sum",{count,one},8);g.bindOutputPort("sum",sum);
    auto inverse=op(2,OperationKind::kNot,"inverse",{other},1);g.bindOutputPort("inverse",inverse);
    ActivityScheduleValueFanout fanout(g.values().size());
    fanout[x.index-1]={0};fanout[count.index-1]={1};fanout[other.index-1]={2};
    SessionStore session;slot(session,"supernode_to_ops",nodes);
    slot(session,"value_fanout",fanout);slot(session,"topo_order",ActivityScheduleTopoOrder{0,1,2});
    slot(session,"state_read_supernodes",ActivityScheduleStateReadSupernodes{});
    EmitOptions opts;opts.outputDir=(root/"runtime").string();opts.session=&session;opts.sessionPathPrefix="top";
    opts.attributes["waveform"]="off";opts.attributes["perf"]="off";
    EmitDiagnostics diag;check(EmitGrhSimCpp(&diag).emit(d,opts).success && !diag.hasError(),"runtime emit failed");
    const auto dir=std::filesystem::path(*opts.outputDir);
    unsigned instrumented=0;
    for(const auto &f:std::filesystem::directory_iterator(dir)) {
        if(f.path().extension()!=".cpp")continue;
        auto s=read(f.path());bool edited=false;
        for(unsigned n=0;n<3;++n) {
            auto pos=s.find("// Supernode "+std::to_string(n)+":");
            if(pos==std::string::npos)continue;
            auto brace=s.find('{',pos);check(brace!=std::string::npos,"missing dispatch body");
            s.insert(brace+1,"\n ++ctz_test_visits["+std::to_string(n)+"];\n");
            edited=true;++instrumented;
        }
        if(edited){std::ofstream out(f.path());out<<"extern unsigned ctz_test_visits[3];\n"<<s;}
    }
    check(instrumented==3,"incomplete diagnostic hooks");
    std::ofstream h(dir/"harness.cpp");h<<R"CPP(#include "grhsim_top.hpp"
#include <cassert>
unsigned ctz_test_visits[3]={};
void reset(){for(auto &v:ctz_test_visits)v=0;}
int main(){GrhSIM_top m;m.x={1,0};m.eval();assert(m.sum==1);reset();
m.eval();assert(!ctz_test_visits[0]&&!ctz_test_visits[1]&&!ctz_test_visits[2]);
m.other=1;m.eval();assert(!ctz_test_visits[0]&&!ctz_test_visits[1]&&ctz_test_visits[2]);reset();
m.x[1]=1;m.eval();assert(m.sum==1&&ctz_test_visits[0]&&!ctz_test_visits[1]);reset();
m.x[0]=0;m.eval();assert(m.sum==65&&ctz_test_visits[0]&&ctz_test_visits[1]);reset();
m.x[1]=0;m.eval();assert(m.sum==66&&ctz_test_visits[0]&&ctz_test_visits[1]);}
)CPP";h.close();
    auto path=dir.string();check(path.find('\'')==std::string::npos,"test path contains quote");
    std::string cmd="c++ -std=c++20 -O2 -I '"+path+"' '"+path+"'/*.cpp -o '"+path+"/check' && '"+path+"/check'";
    check(std::system(cmd.c_str())==0,"activity runtime check failed");
}
}
int main(){try{
    auto root=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/"grhsim_ctz";
    std::filesystem::create_directories(root);structure(root);runtime(root);
    std::cout<<"PASS CTZ serialization, provenance, CSE, DCE, folding, SSA boundary and activity\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
