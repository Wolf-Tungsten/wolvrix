#include "core/grh.hpp"
#include "core/transform.hpp"
#include "emit/system_verilog.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using slang::SVInt;

namespace
{
    template <typename T>
    T attr(const Operation &op, std::string_view name)
    {
        return std::get<T>(*op.attr(name));
    }

    ValueId valueOp(Graph &g, OperationKind kind, int width, std::vector<ValueId> inputs)
    {
        auto v = g.createValue(width, false);
        auto op = g.createOperation(kind);
        for (auto input : inputs) g.addOperand(op, input);
        g.addResult(op, v);
        return v;
    }

    ValueId constant(Graph &g, int width, const std::string &literal)
    {
        auto v = valueOp(g, OperationKind::kConstant, width, {});
        g.setAttr(g.valueDef(v), "constValue", literal);
        return v;
    }

    ValueId slice(Graph &g, ValueId source, int low, int high)
    {
        auto v = valueOp(g, OperationKind::kSliceStatic, high-low+1, {source});
        g.setAttr(g.valueDef(v), "sliceStart", int64_t{low});
        g.setAttr(g.valueDef(v), "sliceEnd", int64_t{high});
        return v;
    }

    ValueId input(Graph &g, const std::string &name, int width)
    {
        auto v = g.createValue(g.internSymbol(name), width, false);
        g.bindInputPort(name, v);
        return v;
    }

    struct Fixture
    {
        Design design;
        Graph &g;
        int width;
        ValueId enable, select, address, data, clock;
        std::vector<OperationId> writes;

        Fixture(int bits, const std::string &name) : g(design.createGraph(name)), width(bits)
        {
            design.markAsTop(name);
            enable = input(g, "enable", 1);
            select = input(g, "select", 1);
            address = input(g, "address", 2);
            data = input(g, "data", bits);
            clock = input(g, "clock", 1);
            auto mem = g.createOperation(OperationKind::kMemory, g.internSymbol("arbitrary_storage"));
            g.setAttr(mem, "width", int64_t{bits});
            g.setAttr(mem, "row", int64_t{4});
            g.setAttr(mem, "isSigned", false);
            auto read = valueOp(g, OperationKind::kMemoryReadPort, bits, {address});
            g.setAttr(g.valueDef(read), "memSymbol", std::string("arbitrary_storage"));
            g.bindOutputPort("read_data", read);
            auto previous=g.createOperation(OperationKind::kRegister,g.internSymbol("previous_row"));
            g.setAttr(previous,"width",int64_t{bits});
            g.setAttr(previous,"isSigned",false);
            auto previousRead=valueOp(g,OperationKind::kRegisterReadPort,bits,{});
            g.setAttr(g.valueDef(previousRead),"regSymbol",std::string("previous_row"));
            g.bindOutputPort("previous_data",previousRead);
            auto sample=g.createOperation(OperationKind::kRegisterWritePort);
            for(auto v:{constant(g,1,"1'b1"),read,
                        constant(g,bits,std::to_string(bits)+"'b"+std::string(bits,'1')),clock})
                g.addOperand(sample,v);
            g.setAttr(sample,"regSymbol",std::string("previous_row"));
            g.setAttr(sample,"eventEdge",std::vector<std::string>{"posedge"});
            auto repeat = valueOp(g, OperationKind::kReplicate, bits, {select});
            g.setAttr(g.valueDef(repeat), "rep", int64_t{bits});
            for (int bit = 0; bit < bits; ++bit)
            {
                auto guard = valueOp(g, OperationKind::kAnd, 1, {enable, slice(g, repeat, bit, bit)});
                auto payload = valueOp(g, OperationKind::kShl, bits,
                                       {slice(g, data, bit, bit), constant(g, 32, "32'd"+std::to_string(bit))});
                std::string mask(bits, '0');
                mask[bits-bit-1] = '1';
                auto op = g.createOperation(OperationKind::kMemoryWritePort);
                for (auto v : {guard, address, payload, constant(g, bits, std::to_string(bits)+"'b"+mask), clock})
                    g.addOperand(op, v);
                g.setAttr(op, "memSymbol", std::string("arbitrary_storage"));
                g.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
                writes.push_back(op);
            }
        }
    };

    using Values = std::unordered_map<ValueId, SVInt, ValueIdHash>;

    SVInt eval(const Graph &g, ValueId v, Values &values)
    {
        if (auto it=values.find(v); it!=values.end()) return it->second;
        auto op=g.getOperation(g.valueDef(v));
        auto ins=op.operands();
        auto operand=[&](size_t n) { return eval(g,ins[n],values); };
        SVInt result;
        switch (op.kind())
        {
        case OperationKind::kConstant: result=SVInt::fromString(attr<std::string>(op,"constValue")); break;
        case OperationKind::kAssign: result=operand(0); break;
        case OperationKind::kSliceStatic:
            result=operand(0).slice(attr<int64_t>(op,"sliceEnd"),attr<int64_t>(op,"sliceStart")); break;
        case OperationKind::kConcat:
        {
            std::vector<SVInt> parts;
            for (size_t i=0;i<ins.size();++i) parts.push_back(operand(i));
            result=SVInt::concat(parts); break;
        }
        case OperationKind::kReplicate:
        {
            std::vector<SVInt> parts(attr<int64_t>(op,"rep"),operand(0));
            result=SVInt::concat(parts); break;
        }
        case OperationKind::kAnd: result=operand(0)&operand(1); break;
        case OperationKind::kOr: result=operand(0)|operand(1); break;
        case OperationKind::kXor: result=operand(0)^operand(1); break;
        case OperationKind::kNot: result=~operand(0); break;
        case OperationKind::kShl: result=operand(0).resize(g.valueWidth(v)).shl(operand(1)); break;
        case OperationKind::kLShr: result=operand(0).resize(g.valueWidth(v)).lshr(operand(1)); break;
        default: throw std::runtime_error("unsupported test evaluator operation "+std::string(toString(op.kind())));
        }
        result=result.resize(g.valueWidth(v));
        values.emplace(v,result);
        return result;
    }

    SVInt execute(const Fixture &f, char en, char sel, const std::string &bits, const SVInt &initial)
    {
        auto logic=[](char ch) {return SVInt::fromString(std::string("1'b")+ch);};
        Values values{{f.enable,logic(en)},{f.select,logic(sel)},
                      {f.address,SVInt(2,2,false)},{f.data,SVInt::fromString(std::to_string(f.width)+"'b"+bits)}};
        SVInt result=initial;
        for(auto id:f.g.operations())
        {
            auto op=f.g.getOperation(id);
            if(op.kind()!=OperationKind::kMemoryWritePort) continue;
            auto inputs=op.operands();
            if(!exactlyEqual(eval(f.g,inputs[0],values),SVInt(1,1,false))) continue;
            auto data=eval(f.g,inputs[2],values),mask=eval(f.g,inputs[3],values);
            for(int bit=0;bit<f.width;++bit)
                if(exactlyEqual(SVInt(mask[bit]),SVInt(1,1,false))) result.set(bit,bit,SVInt(data[bit]));
        }
        return result;
    }

    size_t writeCount(const Fixture &f)
    {
        return std::count_if(f.g.operations().begin(),f.g.operations().end(),[&](auto op)
                            {return f.g.opKind(op)==OperationKind::kMemoryWritePort;});
    }

    void run(Fixture &f)
    {
        std::string error;
        auto pass=makePass("memory-write-coalesce",{},error);
        if(!pass) throw std::runtime_error(error);
        PassManager manager;
        manager.addPass(std::move(pass));
        PassDiagnostics diags;
        auto result=manager.run(f.design,diags);
        if(!result.success || diags.hasError()) throw std::runtime_error("pass failed");
    }

    void check(bool condition,const std::string &message)
    {
        if(!condition) throw std::runtime_error(message);
    }

    void emitFixture(const Fixture &f, const std::string &filename)
    {
        wolvrix::lib::emit::EmitOptions options;
        options.outputDir=WOLF_MEMORY_COALESCE_ARTIFACT_DIR;
        options.outputFilename=filename;
        wolvrix::lib::emit::EmitDiagnostics diags;
        wolvrix::lib::emit::EmitSystemVerilog emitter(&diags);
        check(emitter.emit(f.design,options).success&&!diags.hasError(),"emission failed");
    }
}

int main()
{
    try
    {
        for(int width:{8,33,70,257})
        {
            Fixture f(width,"generic_case_"+std::to_string(width));
            std::vector<SVInt> expected;
            std::string bits;
            for(int bit=0;bit<width;++bit) bits+="10xz"[bit%4];
            auto initial=SVInt::fromString(std::to_string(width)+"'b"+std::string(width,'z'));
            if(width==70) emitFixture(f,"original.sv");
            for(char en:std::string("01xz"))
                for(char sel:std::string("01xz")) expected.push_back(execute(f,en,sel,bits,initial));
            run(f);
            check(writeCount(f)==1,"equivalent replicated-bit guards should merge at width "+std::to_string(width));
            size_t i=0;
            for(char en:std::string("01xz"))
                for(char sel:std::string("01xz"))
                    check(exactlyEqual(expected[i++],execute(f,en,sel,bits,initial)),"four-state memory mismatch");
            run(f);
            check(writeCount(f)==1,"pass must be idempotent");
            if(width==70)
            {
                emitFixture(f,"coalesced.sv");
            }
        }
        for(const std::string mode:{"overlap","priority","dynamic_mask","unknown_mask","event","address","declared","fill","dpi","different_edge"})
        {
            Fixture f(8,"negative_"+mode);
            if(mode=="overlap") f.g.replaceOperand(f.writes[1],3,f.g.opOperands(f.writes[0])[3]);
            if(mode=="priority")
                for(size_t i=0;i<f.writes.size();++i)
                {
                    f.g.setAttr(f.writes[i],kMemoryWritePriorityGroupAttr,std::string("ordered"));
                    f.g.setAttr(f.writes[i],kMemoryWritePriorityAttr,int64_t(i));
                }
            if(mode=="dynamic_mask") f.g.replaceOperand(f.writes[0],3,input(f.g,"unknown_mask",8));
            if(mode=="unknown_mask") f.g.replaceOperand(f.writes[0],3,constant(f.g,8,"8'b0000000x"));
            if(mode=="declared")
                for(auto op:f.writes) f.g.addDeclaredSymbol(f.g.operationSymbol(op));
            if(mode=="fill")
            {
                auto fill=f.g.createOperation(OperationKind::kMemoryFillPort);
                f.g.setAttr(fill,"memSymbol",std::string("arbitrary_storage"));
            }
            if(mode=="dpi")
            {
                auto external=valueOp(f.g,OperationKind::kDpicCall,1,{});
                f.g.replaceOperand(f.writes[0],0,external);
            }
            if(mode=="different_edge")
                for(size_t i=0;i<f.writes.size();++i)
                    f.g.setAttr(f.writes[i],"eventEdge",std::vector<std::string>{i%2 ? "negedge" : "posedge"});
            if(mode=="event")
                for(size_t i=0;i<f.writes.size();++i) f.g.replaceOperand(f.writes[i],4,input(f.g,"clk"+std::to_string(i),1));
            if(mode=="address")
                for(size_t i=0;i<f.writes.size();++i) f.g.replaceOperand(f.writes[i],1,input(f.g,"addr"+std::to_string(i),2));
            run(f);
            check(writeCount(f)==(mode=="different_edge" ? 2 : 8),"unsafe case modified: "+mode);
        }
        // Disjoint masks with holes must preserve unselected memory bits.
        {
            Fixture f(33,"sparse_lanes");
            for(size_t i=1;i<f.writes.size();i+=2) f.g.eraseOp(f.writes[i]);
            std::string bits;
            for(int i=0;i<f.width;++i) bits+="zx10"[i%4];
            auto initial=SVInt::fromString("33'b"+std::string(33,'1'));
            auto expected=execute(f,'1','1',bits,initial);
            run(f);
            check(writeCount(f)==1,"sparse group not coalesced");
            check(exactlyEqual(expected,execute(f,'1','1',bits,initial)),"sparse mask lost data");
        }
        // Packed-array metadata is advisory. A concat on the enable cone must
        // remain pure even when its annotations no longer describe its shape.
        for (bool unknownAttribute : {false, true})
        {
            Fixture f(8,"annotated_concat");
            auto concat=valueOp(f.g,OperationKind::kConcat,2,{f.enable,f.enable});
            auto op=f.g.valueDef(concat);
            f.g.setAttr(op,"svPackedArray.version",int64_t{1});
            f.g.setAttr(op,"svPackedArray.elementWidth",int64_t{1});
            f.g.setAttr(op,"svPackedArray.elementCount",int64_t{9});
            if (unknownAttribute) f.g.setAttr(op,"unrecognized.effect",true);
            auto enable=slice(f.g,concat,0,0);
            for(auto write:f.writes)
            {
                auto guard=f.g.valueDef(f.g.opOperands(write)[0]);
                f.g.replaceOperand(guard,0,enable);
            }
            run(f);
            check(writeCount(f)==(unknownAttribute?8:1),"concat metadata purity mismatch");
        }
        // Right shifts, oversized shifts and large shift-amount constants must
        // preserve four-state data projection, including zero-filled lanes.
        for(const std::string mode:{"right","oversized","large_amount"})
        {
            Fixture f(70,"shift_"+mode);
            for(int bit=0;bit<f.width;++bit)
            {
                ValueId payload;
                if(mode=="right")
                {
                    auto padded=valueOp(f.g,OperationKind::kConcat,140,
                        {f.data,constant(f.g,70,"70'b0")});
                    payload=valueOp(f.g,OperationKind::kLShr,140,
                        {padded,constant(f.g,32,"32'd70")});
                    payload=slice(f.g,payload,0,69);
                }
                else
                {
                    auto amount=mode=="large_amount" ? constant(f.g,80,"80'h10000000000000000") :
                        constant(f.g,32,"32'd70");
                    payload=valueOp(f.g,OperationKind::kShl,70,{f.data,amount});
                }
                f.g.replaceOperand(f.writes[bit],2,payload);
            }
            std::string bits;
            for(int bit=0;bit<f.width;++bit) bits+="xz01"[bit%4];
            auto initial=SVInt::fromString("70'b0");
            auto expected=execute(f,'1','1',bits,initial);
            run(f);
            check(writeCount(f)==1,"shift routing group not coalesced");
            check(exactlyEqual(expected,execute(f,'1','1',bits,initial)),"shift routing changed data");
        }
        std::cout<<"memory-write-coalesce: structural and four-state checks passed\n";
    }
    catch(const std::exception &e)
    {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
