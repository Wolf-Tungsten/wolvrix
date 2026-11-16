#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/dead_code_elim.hpp"
#include "transform/slice_demand.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;
using namespace grh;
using namespace transform;

namespace
{
    void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
    ValueId operation(Graph &g, OperationKind kind, int width, const std::vector<ValueId> &args)
    {
        auto value = g.createValue(width, false); auto op = g.createOperation(kind);
        for (auto arg : args) g.addOperand(op, arg);
        g.addResult(op, value); return value;
    }
    ValueId input(Graph &g, const char *name, int width, bool sign = false)
    {
        auto v = g.createValue(g.internSymbol(name), width, sign); g.bindInputPort(name, v); return v;
    }
    ValueId constant(Graph &g, int width, const std::string &literal)
    {
        auto v = operation(g, OperationKind::kConstant, width, {});
        g.setAttr(g.valueDef(v), "constValue", literal); return v;
    }
    ValueId zero(Graph &g, int width) { return constant(g, width, std::to_string(width) + "'d0"); }
    ValueId slice(Graph &g, ValueId v, int low, int width)
    {
        auto result = operation(g, OperationKind::kSliceStatic, width, {v});
        g.setAttr(g.valueDef(result), "sliceStart", int64_t(low));
        g.setAttr(g.valueDef(result), "sliceEnd", int64_t(low + width - 1)); return result;
    }
    ValueId padded(Graph &g, ValueId data, int width, int offset)
    {
        std::vector<ValueId> args;
        if (width > offset + g.valueWidth(data)) args.push_back(zero(g, width - offset - g.valueWidth(data)));
        args.push_back(data); if (offset) args.push_back(zero(g, offset));
        return operation(g, OperationKind::kConcat, width, args);
    }
    ValueId signedView(Graph &g, ValueId value)
    {
        auto result = g.createValue(g.valueWidth(value), true);
        auto op = g.createOperation(OperationKind::kAssign);g.addOperand(op,value);g.addResult(op,result);return result;
    }
    ValueId directionalShift(Graph &g, ValueId data, ValueId magnitude, ValueId otherMagnitude={})
    {
        const int width = g.valueWidth(magnitude);
        auto negative = operation(g,OperationKind::kSub,width,{zero(g,width),magnitude});
        auto bitView = operation(g,OperationKind::kConcat,width,{negative});
        auto sign = operation(g,OperationKind::kLt,1,{signedView(g,bitView),signedView(g,zero(g,width))});
        auto positive = operation(g,OperationKind::kSub,width,{zero(g,width),bitView});
        auto left = operation(g,OperationKind::kShl,g.valueWidth(data),{data,otherMagnitude.valid()?otherMagnitude:positive});
        auto right = operation(g,OperationKind::kLShr,g.valueWidth(data),
                               {data,bitView});
        return operation(g,OperationKind::kMux,g.valueWidth(data),{sign,left,right});
    }
    bool applyRule(Design &design, bool enabled)
    {
        PassManager manager; manager.addPass(std::make_unique<SliceDemandPass>(SliceDemandOptions{enabled}));
        PassDiagnostics diagnostics; const auto result = manager.run(design, diagnostics);
        require(result.success && !diagnostics.hasError(), "slice-demand failed"); return result.changed;
    }
    void fallbacks()
    {
        for (int mode = 0; mode < 12; ++mode)
        {
            Design d; auto &g = d.createGraph("top");
            auto x = input(g, "x", mode == 2 ? 65 : 32), amount = input(g, "amount", mode == 3 ? 65 : 33);
            int width = mode == 7 ? 8192 : 128;
            auto source = padded(g, x, width, 0);
            if (mode == 4) source = constant(g, width, "128'bx");
            auto shifted = operation(g, mode == 1 ? OperationKind::kAShr : OperationKind::kShl, width, {source, amount});
            if (mode == 5) g.setAttr(g.valueDef(shifted), "hasSideEffects", true);
            if (mode == 8)
            {
                auto cyclic = operation(g, OperationKind::kNot, width, {shifted});
                g.replaceOperand(g.valueDef(cyclic), 0, cyclic); shifted = cyclic;
            }
            if (mode == 9)
                for (unsigned i = 0; i < 40; ++i) shifted = operation(g, OperationKind::kNot, width, {shifted});
            if (mode == 11) shifted = operation(g, OperationKind::kConcat, width, {zero(g, 16), shifted});
            const auto output = slice(g, shifted, 96, mode == 6 ? 1 : 16); g.bindOutputPort("out", output);
            if (mode == 10) g.setAttr(g.valueDef(output), "sliceEnd", int64_t{130});
            const auto count = g.operations().size();
            require(!applyRule(d, mode != 0), "unsupported case transformed");
            require(g.operations().size() == count && g.getOperation(g.valueDef(output)).kind() == OperationKind::kSliceStatic,
                    "failed planning mutated graph");
        }
        std::string error;
        const std::vector<std::string_view> good{"-semantics", "2state"}, bad{"-semantics", "anything"};
        require(makePass("slice-demand", good, error) != nullptr, "CLI rejected two-state");
        require(makePass("slice-demand", bad, error) == nullptr && !error.empty(), "CLI accepted invalid semantics");
    }

    Design fixture()
    {
        Design d; auto &g = d.createGraph("top"); d.markAsTop(g.symbol());
        auto x = input(g, "x", 64), amount = input(g, "amount", 64);
        auto shortAmount = input(g, "short_amount", 33, true);
        auto signedData = input(g, "signed_data", 32, true);
        auto select = input(g, "select", 1), clk = input(g, "clk", 1);
        const std::vector<int> widths{1024,1024,192,192,128,128,256,1024};
        const std::vector<int> payloads{32,32,64,64,64,32,32,32};
        const std::vector<int> offsets{0,0,80,80,0,64,80,80};
        const std::vector<int> lows{96,96,64,64,0,0,96,96};
        const std::vector<int> results{16,16,64,64,64,2,16,64};
        std::vector<ValueId> outputs;
        for (int i = 0; i < 8; ++i)
        {
            auto data = payloads[i] == 64 ? x : slice(g, x, 0, payloads[i]);
            auto source = padded(g, data, widths[i], offsets[i]);
            if (i == 7)
            {
                source = constant(g, 1024, "1024'hffffffff00000000000000000000");
            }
            auto shifted = operation(g, (i == 1 || i == 3 || i == 5) ? OperationKind::kLShr : OperationKind::kShl,
                                     widths[i], {source, i < 2 ? shortAmount : amount});
            if (i == 0) g.bindOutputPort("wide_tap", shifted); // Preserve other users of the original wide result.
            if (i == 6)
            {
                auto right = operation(g, OperationKind::kLShr, widths[i], {source, amount});
                auto chosen = operation(g, OperationKind::kMux, widths[i], {select, shifted, right});
                auto inverted = operation(g, OperationKind::kNot, widths[i], {chosen});
                auto salt = constant(g, widths[i], "256'h13579bdf2468ace0fedcba9876543210");
                auto both = operation(g, OperationKind::kAnd, widths[i], {inverted, salt});
                auto either = operation(g, OperationKind::kOr, widths[i], {both, source});
                shifted = operation(g, OperationKind::kXor, widths[i], {either, salt});
            }
            outputs.push_back(slice(g, shifted, lows[i], results[i]));
            g.bindOutputPort("c" + std::to_string(i), outputs.back());
        }
        // A demanded range straddles concat pieces, including a shifted piece.
        auto shifted = operation(g, OperationKind::kShl, 128, {padded(g, slice(g, x, 0, 32), 128, 0), amount});
        auto joined = operation(g, OperationKind::kConcat, 144, {shifted, constant(g, 16, "16'habcd")});
        auto out = slice(g, joined, 8, 32); g.bindOutputPort("cross", out);
        // Concatenated adjacent scalar payloads are one <=64-bit sparse span.
        auto multi = operation(g, OperationKind::kConcat, 192,
                               {zero(g, 112), slice(g, x, 32, 32), slice(g, x, 0, 32), zero(g, 16)});
        auto sr = operation(g, OperationKind::kLShr, 192, {multi, amount});
        g.bindOutputPort("multi", slice(g, sr, 0, 64));
        auto signedShift = operation(g, OperationKind::kShl, 128, {padded(g, signedData, 128, 0), amount});
        g.bindOutputPort("signed_bits", slice(g, signedShift, 31, 64));
        auto index = input(g,"index",5), hugeIndex = input(g,"huge_index",56), otherIndex = input(g,"other_index",5);
        auto aligned = padded(g,index,33,5);
        auto payload = slice(g,x,0,32);
        auto sparseData = padded(g,payload,256,0);
        auto direct = operation(g,OperationKind::kShl,256,{sparseData,aligned});
        g.bindOutputPort("aligned_left",slice(g,direct,96,16));
        auto direction = directionalShift(g,sparseData,aligned);
        g.bindOutputPort("aligned_direction",slice(g,direction,96,16));
        auto maskDirection = directionalShift(g,padded(g,constant(g,32,"32'hffffffff"),256,0),aligned);
        auto mask16 = slice(g,operation(g,OperationKind::kNot,256,{maskDirection}),96,16);
        auto data16 = slice(g,direction,96,16);
        auto old16 = slice(g,x,32,16);
        auto updated = operation(g,OperationKind::kOr,16,{operation(g,OperationKind::kAnd,16,{old16,mask16}),data16});
        g.bindOutputPort("aligned_update",updated);
        auto rightAligned = operation(g,OperationKind::kLShr,256,{padded(g,payload,256,128),aligned});
        g.bindOutputPort("aligned_right",slice(g,rightAligned,32,16));
        auto huge = operation(g,OperationKind::kShl,256,{sparseData,padded(g,hugeIndex,64,5)});
        g.bindOutputPort("aligned_huge",slice(g,huge,96,16));
        // Cross-field window and a mismatched signed-direction amount must
        // stay on the general scalar-window path, not the aligned shortcut.
        g.bindOutputPort("aligned_cross",slice(g,direct,80,64));
        auto mismatch = directionalShift(g,sparseData,aligned,padded(g,otherIndex,33,5));
        g.bindOutputPort("aligned_mismatch",slice(g,mismatch,96,16));
        auto reg = g.createOperation(OperationKind::kRegister, g.internSymbol("q"));
        g.setAttr(reg, "width", int64_t{16}); g.setAttr(reg, "isSigned", false); g.setAttr(reg, "initValue", std::string("16'd0"));
        auto read = operation(g, OperationKind::kRegisterReadPort, 16, {});
        g.setAttr(g.valueDef(read), "regSymbol", std::string("q")); g.bindOutputPort("q_out", read);
        auto write = g.createOperation(OperationKind::kRegisterWritePort);
        for (auto v : {constant(g, 1, "1'b1"), outputs[0], constant(g, 16, "16'hffff"), clk}) g.addOperand(write, v);
        g.setAttr(write, "regSymbol", std::string("q")); g.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        return d;
    }
    std::string quote(const std::string &s)
    {
        std::string out = "'"; for (char c : s) out += c == '\'' ? "'\\''" : std::string(1,c); return out + "'";
    }
    const char *harness = R"CPP(
#include "grhsim_top.hpp"
#include <array>
#include <cstdint>
#include <iostream>
bool sourceBit(uint64_t x,int payload,int offset,int bit) {
    return bit>=offset && bit<offset+payload && ((x>>(bit-offset))&1);
}
bool shiftedBit(uint64_t x,int payload,int offset,int total,uint64_t shift,int bit,bool right) {
    if(shift>=static_cast<uint64_t>(total))return false;
    const int original=right?bit+static_cast<int>(shift):bit-static_cast<int>(shift);
    return original>=0 && original<total && sourceBit(x,payload,offset,original);
}
uint64_t expected(unsigned i,uint64_t x,uint64_t amount,uint64_t shortAmount,bool select) {
    const int widths[]={1024,1024,192,192,128,128,256,1024};
    const int payloads[]={32,32,64,64,64,32,32,32};
    const int offsets[]={0,0,80,80,0,64,80,80};
    const int lows[]={96,96,64,64,0,0,96,96}, sizes[]={16,16,64,64,64,2,16,64};
    uint64_t value=0; if(i==7)x=0xffffffff;
    for(int j=0;j<sizes[i];++j) {
        const int bit=lows[i]+j;
        bool b=shiftedBit(x,payloads[i],offsets[i],widths[i],i<2?shortAmount:amount,bit,i==1||i==3||i==5||(i==6&&!select));
        if(i==6) {
            const uint64_t saltLo=UINT64_C(0xfedcba9876543210),saltHi=UINT64_C(0x13579bdf2468ace0);
            const bool salt=bit<64?((saltLo>>bit)&1):bit<128?((saltHi>>(bit-64))&1):false;
            b=((!b&&salt)||sourceBit(x,32,80,bit))!=salt;
        }
        if(b)value|=UINT64_C(1)<<j;
    }
    return value;
}
int main() {
    GrhSIM_top m; m.init(); m.clk=0;m.select=0;m.x=0;m.amount=0;m.short_amount=0;m.signed_data=0;m.index=0;m.huge_index=0;m.other_index=0;m.eval();
    uint64_t rng=1;unsigned q=0;
    for(unsigned step=0;step<5000;++step) {
        rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
        m.x=step<64?(UINT64_C(1)<<step):rng;
        m.signed_data=static_cast<uint32_t>(m.x);
        const uint64_t edge[]={0,1,31,32,63,64,65,79,80,95,96,97,127,128,191,192,255,256,1023,1024,UINT64_MAX,UINT64_MAX-63,UINT64_C(1)<<63};
        m.amount=step<1152?step:edge[step%23];
        m.short_amount=step%3==0?((UINT64_C(0)-m.amount)&UINT64_C(0x1ffffffff)):(m.amount&UINT64_C(0x1ffffffff));
        m.index=step%32;m.other_index=(step/32)%32;m.huge_index=step%2?m.index:((rng>>8)&UINT64_C(0xffffffffffffff));
        m.select=step&1;m.clk=0;m.eval();
        auto selected=m.index==3?(m.x&65535):0;
        if(m.aligned_left!=selected || m.aligned_direction!=selected || m.aligned_right!=selected ||
           m.aligned_update!=(m.index==3?(m.x&65535):((m.x>>32)&65535)) ||
           m.aligned_huge!=(m.huge_index==3?(m.x&65535):0))return 6;
        uint64_t alignedCross=0;
        for(int bit=0;bit<64;++bit)if(shiftedBit(m.x,32,0,256,uint64_t(m.index)*32,80+bit,false))alignedCross|=UINT64_C(1)<<bit;
        if(m.aligned_cross!=alignedCross || m.aligned_mismatch!=((m.index!=0&&m.other_index==3)?(m.x&65535):0))return 7;
        const std::array<uint64_t,8> actual{m.c0,m.c1,m.c2,m.c3,m.c4,m.c5,m.c6,m.c7};
        for(unsigned i=0;i<8;++i)if(actual[i]!=expected(i,m.x,m.amount,m.short_amount,m.select)) {
            std::cerr<<"case "<<i<<" step "<<step<<" observed "<<actual[i]<<" expected "<<expected(i,m.x,m.amount,m.short_amount,m.select)<<'\n';return 1;
        }
        for(int bit=0;bit<1024;++bit)
            if(bool((m.wide_tap[bit/64]>>(bit%64))&1)!=shiftedBit(m.x,32,0,1024,m.short_amount,bit,false))return 5;
        uint64_t cross=0,multi=0,signedBits=0;
        for(int bit=0;bit<32;++bit) {
            bool b=bit+8<16?((0xabcd>>(bit+8))&1):shiftedBit(m.x,32,0,128,m.amount,bit-8,false);
            if(b)cross|=UINT64_C(1)<<bit;
        }
        for(int bit=0;bit<64;++bit)if(shiftedBit(m.x,64,16,192,m.amount,bit,true))multi|=UINT64_C(1)<<bit;
        for(int bit=0;bit<64;++bit)if(shiftedBit(static_cast<uint32_t>(m.signed_data),32,0,128,m.amount,bit+31,false))signedBits|=UINT64_C(1)<<bit;
        if(m.cross!=cross || m.multi!=multi || m.signed_bits!=signedBits || m.q_out!=q)return 2;
        m.clk=1;m.eval();q=actual[0];if(m.q_out!=q)return 3;
        m.x^=UINT64_MAX;m.eval();if(m.q_out!=q || m.c0!=expected(0,m.x,m.amount,m.short_amount,m.select))return 4;
    }
}
)CPP";
}

int main()
{
    try
    {
        fallbacks();
        for (bool enabled : {false,true})
        {
            auto d=fixture();require(applyRule(d,enabled)==enabled,"slice-demand trigger mismatch");
            if(enabled)
            {
                unsigned predicates=0;
                for(auto id:d.findGraph("top")->operations())
                    predicates+=d.findGraph("top")->getOperation(id).kind()==OperationKind::kEq;
                // Identical index==3 tests are shared across windows. The
                // ordinary index and the 56-bit index need distinct predicates.
                require(predicates>=2,"aligned field proof did not trigger");
            }
            PassManager cleanup;cleanup.addPass(std::make_unique<DeadCodeElimPass>());
            PassDiagnostics pd;require(cleanup.run(d,pd).success,"DCE failed");
            if(enabled)require(!applyRule(d,true),"slice-demand not idempotent");
            SessionStore session;PassManager pm;pm.options().session=&session;
            ActivityScheduleOptions schedule;schedule.path="top";schedule.eventRegions=true;
            pm.addPass(std::make_unique<ActivitySchedulePass>(schedule));
            require(pm.run(d,pd).success&&!pd.hasError(),"schedule failed");
            const auto dir=std::filesystem::path(WOLF_SLICE_DEMAND_ARTIFACT_DIR)/(enabled?"on":"off");
            std::filesystem::create_directories(dir);
            emit::EmitOptions options;options.outputDir=dir;options.topOverrides={"top"};options.session=&session;
            options.attributes["waveform"]="off";options.attributes["perf"]="off";
            emit::EmitDiagnostics ed;require(emit::EmitGrhSimCpp(&ed).emit(d,options).success&&!ed.hasError(),"emit failed");
            std::ofstream(dir/"harness.cpp")<<harness;
            for(const auto flags:{"-O0","-O3 -fsanitize=address,undefined -fno-omit-frame-pointer"})
            {
                const char *compiler=std::getenv("CXX");
                std::string command=quote(compiler?compiler:WOLF_TEST_CXX_COMPILER)+" -std=c++20 "+flags+" -I"+quote(dir.string());
                for(const auto &p:std::filesystem::directory_iterator(dir))if(p.path().extension()==".cpp")command+=" "+quote(p.path().string());
                command+=" -o "+quote((dir/"check").string());
                require(std::system(command.c_str())==0&&std::system(quote((dir/"check").string()).c_str())==0,"compiled independent oracle failed");
            }
        }
        std::cout<<"slice-demand: sparse shifts, overflow, offset windows, concat/Boolean chains, shared users and clock snapshots passed\n";
    }
    catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
