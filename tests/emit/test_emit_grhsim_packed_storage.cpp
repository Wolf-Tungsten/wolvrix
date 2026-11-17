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
    std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}};
}
std::string quote(const std::string &s) {
    std::string r="'"; for(char c:s) r+=c=='\''?"'\\''":std::string(1,c); return r+"'";
}
struct Fixture {
    Design design; Graph &g; SessionStore session;
    ActivityScheduleSupernodeToOps nodes{2};
    Fixture():g(design.createGraph("top")) { design.markAsTop("top"); }
    ValueId value(const std::string &n, int w, bool sign=false) {
        return g.createValue(g.internSymbol(n),w,sign,ValueType::Logic);
    }
    ValueId input(const std::string &n,int w) { auto v=value(n,w);g.bindInputPort(n,v);return v; }
    ValueId op(OperationKind k,const std::string &n,int w,std::vector<ValueId> args,bool sign=false) {
        auto id=g.createOperation(k,g.internSymbol(n+"_op"));for(auto a:args)g.addOperand(id,a);
        auto v=value(n,w,sign);g.addResult(id,v);nodes[0].push_back(id);return v;
    }
    ValueId constant(const std::string &n,int w,const std::string &literal) {
        auto v=op(OperationKind::kConstant,n,w,{});g.setAttr(g.valueDef(v),"constValue",literal);return v;
    }
    ValueId packed(const std::string &name,int width,const std::vector<ValueId> &lanes) {
        auto operands=lanes;std::reverse(operands.begin(),operands.end());
        auto v=op(OperationKind::kConcat,name,width*static_cast<int>(lanes.size()),operands);
        auto id=g.valueDef(v);g.setAttr(id,"svPackedArray.version",int64_t{1});
        g.setAttr(id,"svPackedArray.elementWidth",int64_t{width});
        g.setAttr(id,"svPackedArray.elementCount",int64_t{static_cast<int64_t>(lanes.size())});
        g.setAttr(id,"svPackedArray.indexLow",int64_t{0});
        g.setAttr(id,"svPackedArray.indexHigh",int64_t{static_cast<int64_t>(lanes.size()-1)});
        g.setAttr(id,"svPackedArray.laneOrder",std::string("lsb_index_low"));
        g.setAttr(id,"svPackedArray.concat.operand0Index",int64_t{static_cast<int64_t>(lanes.size()-1)});
        g.setAttr(id,"svPackedArray.concat.operandStride",int64_t{-1});return v;
    }
    template<class T>void slot(const std::string &n,T v) {
        session.insert_or_assign("top.activity_schedule."+n,
            std::make_unique<SessionSlotValue<T>>(std::move(v),"packed-storage-test"));
    }
    void schedule() {
        std::map<uint32_t,unsigned> owner;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n])owner[id.index]=n;
        ActivityScheduleValueFanout fanout(g.values().size());ActivityScheduleStateReadSupernodes readers;
        for(unsigned n=0;n<nodes.size();++n)for(auto id:nodes[n]) {
            const auto operation=g.getOperation(id);
            for(auto v:operation.operands()) {
                auto def=g.valueDef(v);
                if(!def.valid()||!owner.contains(def.index)||owner.at(def.index)!=n) {
                    auto &f=fanout[v.index-1];if(std::find(f.begin(),f.end(),n)==f.end())f.push_back(n);
                }
            }
            if(operation.kind()==OperationKind::kRegisterReadPort)
                readers[std::get<std::string>(*operation.attr("regSymbol"))].push_back(n);
        }
        ActivityScheduleTopoOrder order(nodes.size());std::iota(order.begin(),order.end(),0u);
        slot("supernode_to_ops",nodes);slot("value_fanout",fanout);slot("topo_order",order);slot("state_read_supernodes",readers);
    }
};
using Files=std::map<std::string,std::string>;
Files emit(Fixture &f,const std::filesystem::path &dir,const std::string &feature,const std::string &mode) {
    f.schedule();EmitOptions options;options.outputDir=dir;options.session=&f.session;options.sessionPathPrefix="top";
    options.attributes["waveform"]="off";options.topOverrides={"top"};
    if(mode!="default"&&mode!="env")options.attributes[feature]=mode=="off"||mode=="precedence"?"0":"1";
    EmitDiagnostics diag;EmitGrhSimCpp emitter(&diag);
    if(!emitter.emit(f.design,options).success) {
        for(const auto &d:diag.messages())std::cerr<<d.message<<'\n';throw std::runtime_error("emit failed");
    }
    Files result;for(const auto &entry:std::filesystem::directory_iterator(dir))
        if(entry.path().extension()==".cpp"||entry.path().extension()==".hpp")
            result[entry.path().filename().string()]=read(entry.path());
    return result;
}
void layoutFixture(Fixture &f,int width,const std::string &kind) {
    auto clk=f.input("clk",1),aux=f.input("aux",1),rst=f.input("rst",1),index=f.input("index",8);
    auto enable=f.constant("enable",1,"1'b1"),mask=f.input("mask",width);
    auto all=f.constant("all",width,std::to_string(width)+"'hffffffffffffffff");
    auto zero=f.constant("zero",width,std::to_string(width)+"'d0");
    std::vector<ValueId> lanes[2];
    const auto originalIndex=index;
    if(kind=="local-index"||kind=="stored-index")index=f.op(OperationKind::kAssign,"stored_index",8,{index});
    for(unsigned lane=0;lane<10;++lane)for(unsigned bank=0;bank<2;++bank) {
        auto n="q_"+std::to_string(bank)+"_"+std::to_string(lane);
        auto reg=f.g.createOperation(OperationKind::kRegister,f.g.internSymbol(n));
        f.g.setAttr(reg,"width",int64_t{width});f.g.setAttr(reg,"isSigned",width==17);
        f.g.setAttr(reg,"initValue",std::to_string(width)+"'d0");
        auto data=f.input("d_"+std::to_string(bank)+"_"+std::to_string(lane),width);
        auto r=f.op(OperationKind::kRegisterReadPort,"r_"+n,width,{},width==17);
        f.g.setAttr(f.g.valueDef(r),"regSymbol",n);lanes[bank].push_back(r);
        auto next=f.op(OperationKind::kMux,"next_"+n,width,{rst,data,zero});
        auto bits=f.op(OperationKind::kMux,"mask_"+n,width,{rst,mask,all});
        auto write=f.g.createOperation(OperationKind::kRegisterWritePort,f.g.internSymbol("write_"+n));
        for(auto v:{enable,next,bits,bank==0?clk:aux,rst})f.g.addOperand(write,v);
        f.g.setAttr(write,"regSymbol",n);f.g.setAttr(write,"eventEdge",std::vector<std::string>{"posedge","negedge"});
        f.nodes[1].push_back(write);
    }
    for(unsigned bank=0;bank<2;++bank) {
        if(kind=="duplicate")lanes[bank][1]=lanes[bank][0];
        if(kind=="padding")lanes[bank][8]=lanes[bank][9]=lanes[bank][0];
        if(kind=="expression")lanes[bank][1]=f.op(OperationKind::kNot,"inverted_"+std::to_string(bank),width,{lanes[bank][1]});
        auto packed=f.packed("packed_"+std::to_string(bank),width,lanes[bank]);
        if(kind=="observed")f.g.bindOutputPort("watch"+std::to_string(bank),packed);
        auto out=f.op(OperationKind::kSliceArray,"selected_"+std::to_string(bank),width,{packed,index});
        f.g.setAttr(f.g.valueDef(out),"sliceWidth",int64_t{width});f.g.bindOutputPort("y"+std::to_string(bank),out);
        if(kind=="effect")f.g.setAttr(f.g.valueDef(out),"hasSideEffects",true);
        if(kind=="multiple") {
            auto extra=f.op(OperationKind::kSliceArray,"extra_"+std::to_string(bank),width,{packed,originalIndex});
            f.g.setAttr(f.g.valueDef(extra),"sliceWidth",int64_t{width});f.g.bindOutputPort("extra"+std::to_string(bank),extra);
        }
        if(kind=="conflict") {
            auto reversed=lanes[bank];std::reverse(reversed.begin(),reversed.end());
            auto p=f.packed("reversed_"+std::to_string(bank),width,reversed);
            auto y=f.op(OperationKind::kSliceArray,"other_"+std::to_string(bank),width,{p,index});
            f.g.setAttr(f.g.valueDef(y),"sliceWidth",int64_t{width});f.g.bindOutputPort("z"+std::to_string(bank),y);
        }
    }
    if(kind=="stored-index") {
        auto compute=std::move(f.nodes[0]),commit=std::move(f.nodes[1]);
        const auto def=f.g.valueDef(index);
        compute.erase(std::remove(compute.begin(),compute.end(),def),compute.end());
        f.nodes={{def},std::move(compute),std::move(commit)};
    }
    if(kind=="persistent") {
        auto compute=std::move(f.nodes[0]),commit=std::move(f.nodes[1]);
        std::vector<OperationId> producer,consumer;
        for(auto id:compute)(f.g.getOperation(id).kind()==OperationKind::kSliceArray?consumer:producer).push_back(id);
        f.nodes={std::move(producer),std::move(consumer),std::move(commit)};
    }
}
void runLayout(const std::filesystem::path &dir,const Files &files,int width,bool packed,const std::string &kind) {
    std::string generated;for(const auto &[n,s]:files)generated+=s;
    std::map<std::string,std::string> fields;
    std::map<std::string,std::string> aliases;
    const std::regex alias(R"(auto &(\w+) = state_logic_storage_\.(\w+);)");
    for(auto it=std::sregex_iterator(generated.begin(),generated.end(),alias);it!=std::sregex_iterator();++it)
        aliases[(*it)[1]]=(*it)[2];
    const std::regex writer(R"(\[kRegisterWritePort\] reg=(q_\d+_\d+)\n[\s\S]*?if \(unlikely\((\w+(?:\.\w+)?) != next_value)");
    for(auto it=std::sregex_iterator(generated.begin(),generated.end(),writer);it!=std::sregex_iterator();++it)
    {
        const std::string ref=(*it)[2];
        fields[(*it)[1]]=ref.starts_with("state_logic_storage_.")?ref.substr(21):aliases.at(ref);
    }
    require(fields.size()==20,"missing state field mapping");
    std::ofstream out(dir/"harness.cpp");
    out<<"#include \"grhsim_top.hpp\"\n#include <cstddef>\n#include <cstdint>\n";
    if(packed)for(unsigned bank=0;bank<2;++bank)for(unsigned lane=1;lane<10;++lane)
        out<<"static_assert(offsetof(GrhSIM_top,state_logic_storage_."<<fields.at("q_"+std::to_string(bank)+"_"+std::to_string(lane))
           <<") == offsetof(GrhSIM_top,state_logic_storage_."<<fields.at("q_"+std::to_string(bank)+"_0")<<") + "
           <<lane*(width==8?1:4)<<");\n";
    out<<"int main(){GrhSIM_top m;constexpr uint64_t limit="
       <<(width==64?"UINT64_MAX":"((UINT64_C(1)<<"+std::to_string(width)+")-1)")<<";\n"
       <<"for(unsigned init=0;init<2;++init){m.init();uint64_t q[2][10]{};bool c=0,a=0,r=1;m.clk=0;m.aux=0;m.rst=1;m.eval();\n"
       <<"for(unsigned i=0;i<400;++i){m.clk=i%4==1;m.aux=i%7==1;m.rst=i%29!=2;m.mask=i%5==0?limit:(uint64_t(i)*1977)&limit;m.index=i%11==0?255:(i*3)%13;\n";
    for(unsigned bank=0;bank<2;++bank)for(unsigned lane=0;lane<10;++lane) {
        auto name=std::to_string(bank)+"_"+std::to_string(lane);
        out<<"m.d_"<<name<<"=((uint64_t(i)*7301+"<<bank*113+lane*997<<")^UINT64_C(0xfedcba9876543210))&limit;\n"
           <<"if((m."<<(bank==0?"clk&&!c":"aux&&!a")<<")||(!m.rst&&r))q["<<bank<<"]["<<lane<<"]="
           <<"m.rst?((q["<<bank<<"]["<<lane<<"]&~uint64_t(m.mask))|(uint64_t(m.d_"<<name<<")&m.mask)):0;\n";
    }
    out<<"c=m.clk;a=m.aux;r=m.rst;for(unsigned repeat=0;repeat<3;++repeat){m.eval();\n";
    for(unsigned bank=0;bank<2;++bank) {
        const auto lane=kind=="duplicate"?"(m.index==1?0:m.index)":kind=="padding"?"(m.index>=8?0:m.index)":"m.index";
        out<<"if(m.y"<<bank<<"!=(m.index<10?q["<<bank<<"]["<<lane<<"]:0))return 1;\n";
        if(kind=="conflict")out<<"if(m.z"<<bank<<"!=(m.index<10?q["<<bank<<"][9-m.index]:0))return 2;\n";
        if(kind=="observed")out<<"for(unsigned b=0;b<"<<width*10<<";++b)if(((m.watch"<<bank
            <<"[b/64]>>(b%64))&1)!=((q["<<bank<<"][b/"<<width<<"]>>(b%"<<width<<"))&1))return 3;\n";
    }
    out<<"m.index=(m.index+5)%13;}}}return 0;}\n";out.close();
    const char *cxx=std::getenv("CXX");
    for(const char *flags:{"-O0","-O3 -fsanitize=address,undefined -fno-sanitize-recover=all"}) {
        std::string command=quote(cxx?cxx:WOLF_TEST_CXX_COMPILER)+" -std=c++20 -fno-access-control -Wno-invalid-offsetof "+flags+" -I"+quote(dir.string());
        for(const auto &[name,text]:files)if(name.ends_with(".cpp"))command+=" "+quote((dir/name).string());
        command+=" "+quote((dir/"harness.cpp").string())+" -o "+quote((dir/"check").string());
        require(std::system(command.c_str())==0,"harness compile failed");
        require(std::system(quote((dir/"check").string()).c_str())==0,"layout oracle failed");
    }
}
void layoutTests(const std::filesystem::path &base) {
    const char *env="WOLVRIX_GRHSIM_STATE_LAYOUT_PACKED_ACCESS";::unsetenv(env);
    for(int width:{8,17})for(const std::string kind:{"ordinary","conflict","duplicate"}) {
        Files defaults;
        for(const std::string mode:{"default","off","on","env","precedence"}) {
            Fixture f;layoutFixture(f,width,kind);
            const auto dir=base/(std::to_string(width)+"-"+kind+"-"+mode);
            if(mode=="env"||mode=="precedence")::setenv(env,"1",1);
            auto files=emit(f,dir,"state_layout_packed_access",mode);::unsetenv(env);
            if(mode=="default")defaults=files;
            if(mode=="off"||mode=="precedence"||kind=="duplicate")require(files==defaults,"layout fallback/default drift");
            if(kind!="duplicate"&&(mode=="on"||mode=="off"))runLayout(dir,files,width,mode=="on",kind);
            if(mode=="env") {
                Fixture explicitFixture;layoutFixture(explicitFixture,width,kind);
                require(files==emit(explicitFixture,base/(std::to_string(width)+kind+"-explicit"),"state_layout_packed_access","on"),"env precedence mismatch");
            }
        }
    }
}
void scopeTests(const std::filesystem::path &base) {
    const char *env="WOLVRIX_GRHSIM_PACKED_ARRAY_LOCAL_SCOPE";::unsetenv(env);
    for(int width:{8,17,64})for(const std::string kind:{"ordinary","stored-index"}) {
        Files defaults;
        for(const std::string mode:{"default","off","on","env","precedence"}) {
            Fixture f;layoutFixture(f,width,kind);
            const auto dir=base/(std::to_string(width)+kind+mode);
            if(mode=="env"||mode=="precedence")::setenv(env,"1",1);
            auto files=emit(f,dir,"packed_array_local_scope",mode);::unsetenv(env);
            if(mode=="default")defaults=files;
            if(mode=="off"||mode=="precedence")require(files==defaults,"scope off/default drift");
            std::string text;for(const auto &[n,s]:files)text+=s;
            unsigned sites=0;for(std::size_t i=0;(i=text.find("// Scoped packed array:",i))!=std::string::npos;++i)++sites;
            require(sites==((mode=="on"||mode=="env")?2u:0u),"scope sites: "+kind+mode);
            if(mode=="on"||mode=="off")runLayout(dir,files,width,false,kind);
        }
    }
    for(const std::string kind:{"multiple","observed","local-index","effect"}) {
        Fixture f;layoutFixture(f,17,kind);
        auto off=emit(f,base/(kind+"off"),"packed_array_local_scope","off");
        auto on=emit(f,base/(kind+"on"),"packed_array_local_scope","on");
        require(off==on,"scope protected fallback: "+kind);
    }
}
void demandTests(const std::filesystem::path &base, bool select) {
    const std::string feature=select?"packed_array_state_select":"packed_array_no_init";
    const char *env=select?"WOLVRIX_GRHSIM_PACKED_ARRAY_STATE_SELECT":"WOLVRIX_GRHSIM_PACKED_ARRAY_NO_INIT";
    ::unsetenv(env);
    for(int width:{8,17,64}) {
        const std::string kind=select?(width==8?"ordinary":width==17?"stored-index":"padding"):
                                      (width==8?"ordinary":width==17?"persistent":"duplicate");
        Files defaults;
        for(const std::string mode:{"default","off","on","env","precedence"}) {
            Fixture f;layoutFixture(f,width,kind);
            const auto dir=base/(std::to_string(width)+kind+mode);
            if(mode=="env"||mode=="precedence")::setenv(env,"1",1);
            const auto files=emit(f,dir,feature,mode);::unsetenv(env);
            if(mode=="default")defaults=files;
            if(mode=="off"||mode=="precedence")require(files==defaults,"demand default/precedence drift");
            std::string text;for(const auto &[n,s]:files)text+=s;
            const std::string marker=select?"// Selected packed state:":"// Packed array full overwrite:";
            unsigned sites=0;for(std::size_t i=0;(i=text.find(marker,i))!=std::string::npos;++i)++sites;
            require(sites==((mode=="on"||mode=="env")?2u:0u),"demand sites: "+kind+mode);
            if(mode=="on"||mode=="off")runLayout(dir,files,width,false,kind);
        }
    }
    const std::vector<std::string> fallbacks=select?
        std::vector<std::string>{"multiple","observed","persistent","local-index","effect","expression"}:
        std::vector<std::string>{"expression","observed"};
    for(const auto &kind:fallbacks) {
        Fixture f;layoutFixture(f,17,kind);
        require(emit(f,base/(kind+"off"),feature,"off")==emit(f,base/(kind+"on"),feature,"on"),
                "demand protected fallback: "+kind);
    }
    if(select) {
        Fixture f;layoutFixture(f,17,"ordinary");
        ::setenv("WOLVRIX_GRHSIM_STATE_LAYOUT_PACKED_ACCESS","1",1);
        ::setenv("WOLVRIX_GRHSIM_PACKED_ARRAY_NO_INIT","1",1);
        ::setenv("WOLVRIX_GRHSIM_PACKED_ARRAY_LOCAL_SCOPE","1",1);
        const auto dir=base/"combined";
        auto files=emit(f,dir,feature,"on");
        ::unsetenv("WOLVRIX_GRHSIM_STATE_LAYOUT_PACKED_ACCESS");
        ::unsetenv("WOLVRIX_GRHSIM_PACKED_ARRAY_NO_INIT");
        ::unsetenv("WOLVRIX_GRHSIM_PACKED_ARRAY_LOCAL_SCOPE");
        std::string text;for(const auto &[n,s]:files)text+=s;
        require(text.find("// Selected packed state:")!=std::string::npos &&
                text.find("// Scoped packed array:")==std::string::npos,"selected read must precede scope");
        runLayout(dir,files,17,true,"ordinary");
    }
}
}
int main(int argc,char **argv) {
    try {
        const std::string mode=argc==2?argv[1]:"layout";
        const auto base=std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR)/("packed-storage-"+mode);
        std::filesystem::remove_all(base);
        if(mode=="scope")scopeTests(base);
        else if(mode=="select"||mode=="no-init")demandTests(base,mode=="select");
        else layoutTests(base);
        std::cout<<"packed storage structure, fallback and O0/O3 sanitizer oracles passed\n";
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
