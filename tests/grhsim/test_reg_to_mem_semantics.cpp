#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/pass/reg_to_mem.hpp"
#include "grhsim/backend/cpu_emit.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <functional>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace wolvrix::lib;
using namespace grhsim;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

RegToMemOptions semanticOptions() {
    RegToMemOptions options;
    options.enableCostSelection = false;
    return options;
}

// A deliberately small core interpreter: reads always use the old state,
// while writes and event histories are published together after evaluation.
class Simulation {
public:
    explicit Simulation(const GrhSimModel &model) : m(model), defs(m.values().size() + 1),
        states(m.states().size() + 1), values(m.values().size() + 1), ready(values.size()) {
        for (const auto &op : m.operations())
            for (auto result : m.results(op)) defs[result.index] = op.id;
        for (const auto &state : m.states()) {
            const auto &type = m.types()[state.type.index - 1];
            states[state.id.index].resize(type.kind == TypeKind::Array ? type.count : 1);
        }
        for (const auto &record : m.initRecords()) for (const auto &step : m.steps(record)) {
            uint64_t value = 0, start = 0, count = 1;
            for (const auto &p : m.parameters(step)) {
                if (m.text(p.name) == "value") value = literal(std::get<std::string>(p.value));
                if (m.text(p.name) == "start") start = std::get<int64_t>(p.value);
                if (m.text(p.name) == "count") count = std::get<int64_t>(p.value);
            }
            check(m.text(step.kind) == "core.init.const" || m.text(step.kind) == "core.init.fill", "unsupported init");
            for (uint64_t i = start; i < start + count; ++i) states.at(record.state.index).at(i) = value;
        }
    }

    void step(const std::vector<uint64_t> &inputs) {
        inputValues = inputs;
        calls.clear();
        std::fill(ready.begin(), ready.end(), false);
        auto pending = states;
        for (const auto &op : m.operations()) {
            const auto kind = m.text(op.opType);
            if (kind != "core.state.regWrite" && kind != "core.state.memWrite" &&
                kind != "core.state.memWriteSeq" && kind != "core.state.memFill" &&
                kind != "core.dpi.call") continue;
            auto args = m.operands(op);
            auto objects = m.objectRefs(op);
            const auto eventCount=objects.size()-1;
            check(eventCount>0,"test expects explicit event histories");
            const std::vector<std::string> *edges=nullptr;
            for(const auto &p:m.parameters(op))
                if(m.text(p.name)=="event_edges") edges=std::get_if<std::vector<std::string>>(&p.value);
            check(edges && edges->size()==eventCount,"invalid event list");
            bool edge=false;
            for(std::size_t i=0;i<eventCount;++i) {
                const auto event=eval(args[args.size()-eventCount+i]);
                const auto previous=states.at(objects[1+i].index).at(0);
                check((*edges)[i]=="posedge" || (*edges)[i]=="negedge","unsupported test edge");
                edge |= (*edges)[i]=="posedge"?(!previous && event):(previous && !event);
                pending.at(objects[1+i].index).at(0)=event;
            }
            if (!edge) continue;
            if (kind == "core.dpi.call") {
                if (eval(args[0])) {
                    std::vector<uint64_t> arguments;
                    for (std::size_t i = 1; i < args.size() - eventCount; ++i)
                        arguments.push_back(eval(args[i]));
                    calls.push_back(std::move(arguments));
                }
                continue;
            }
            auto &target = pending.at(objects[0].index);
            if (kind == "core.state.regWrite") {
                if (eval(args[0])) {
                    const auto mask = eval(args[2]);
                    target[0] = (target[0] & ~mask) | (eval(args[1]) & mask);
                }
            } else if (kind == "core.state.memWrite") {
                if (eval(args[0])) {
                    auto &cell = target.at(eval(args[1]));
                    const auto mask = eval(args[3]);
                    cell = (cell & ~mask) | (eval(args[2]) & mask);
                }
            } else if (kind == "core.state.memFill") {
                if (eval(args[0])) std::fill(target.begin(), target.end(), eval(args[1]));
            } else {
                for (std::size_t i = 0; i + 2 < args.size()-eventCount; i += 3)
                    if (eval(args[i])) target.at(eval(args[i + 1])) = eval(args[i + 2]);
            }
        }
        states.swap(pending);
        std::fill(ready.begin(), ready.end(), false);
    }

    std::vector<uint64_t> outputs() {
        std::vector<uint64_t> result(m.outputs().size());
        for (const auto &op : m.operations()) if (m.text(op.opType) == "core.output.write")
            result.at(m.objectRefs(op)[0].index - 1) = eval(m.operands(op)[0]);
        return result;
    }

    std::vector<std::vector<uint64_t>> calls;

private:
    static uint64_t literal(const std::string &text) {
        auto value = slang::SVInt::fromString(text);
        check(!value.hasUnknown() && value.getBitWidth() <= 64, "unsupported test literal");
        return value.getRawPtr()[0];
    }
    uint64_t eval(ValueId value) {
        if (ready.at(value.index)) return values[value.index];
        const auto &op = m.operations().at(defs.at(value.index).index - 1);
        const auto kind = m.text(op.opType);
        auto args = m.operands(op);
        auto a = [&](std::size_t i) { return eval(args[i]); };
        uint64_t result = 0;
        if (kind == "core.input.read") result = inputValues.at(m.objectRefs(op)[0].index - 1);
        else if (kind == "core.state.read") result = states.at(m.objectRefs(op)[0].index)[0];
        else if (kind == "core.state.memRead") result = states.at(m.objectRefs(op)[0].index).at(a(0));
        else if (kind == "core.compute.constant") {
            for (const auto &p : m.parameters(op))
                if (m.text(p.name) == "value" || m.text(p.name) == "constValue") result = literal(std::get<std::string>(p.value));
        } else if (kind == "core.compute.assign") result = a(0);
        else if (kind == "core.compute.eq") result = a(0) == a(1);
        else if (kind == "core.compute.lt") result = a(0) < a(1);
        else if (kind == "core.compute.ge") result = a(0) >= a(1);
        else if (kind == "core.compute.sub") result = a(0) - a(1);
        else if (kind == "core.compute.add") result = a(0) + a(1);
        else if (kind == "core.compute.mod") result = a(1) ? a(0) % a(1) : 0;
        else if (kind == "core.compute.div") result = a(1) ? a(0) / a(1) : 0;
        else if (kind == "core.compute.xor") result = a(0) ^ a(1);
        else if (kind == "core.compute.lshr") result = a(1)>=64?0:a(0)>>a(1);
        else if (kind == "core.compute.and") result = a(0) & a(1);
        else if (kind == "core.compute.logicAnd") result = a(0) && a(1);
        else if (kind == "core.compute.logicOr") result = a(0) || a(1);
        else if (kind == "core.compute.logicNot") result = !a(0);
        else if (kind == "core.compute.reduceAnd") {
            const auto width=m.types()[m.values()[args[0].index-1].type.index-1].width;
            result=a(0)==((uint64_t{1}<<width)-1);
        }
        else if (kind == "core.compute.mux") result = a(0) ? a(1) : a(2);
        else if (kind == "core.compute.concat") {
            for (std::size_t i=0;i<args.size();++i) {
                const auto width=m.types()[m.values()[args[i].index-1].type.index-1].width;
                result=(result<<width)|a(i);
            }
        } else if (kind == "core.compute.sliceArray") {
            const auto width=m.types()[m.values()[value.index-1].type.index-1].width;
            const auto sourceWidth=m.types()[m.values()[args[0].index-1].type.index-1].width;
            result=a(1)>=sourceWidth/width?0:a(0)>>(a(1)*width);
        } else if (kind == "core.compute.sliceDynamic") {
            result=a(1)>=64?0:a(0)>>a(1);
        } else if (kind == "core.compute.sliceStatic") {
            uint64_t start=0;
            for(const auto &p:m.parameters(op)) if(m.text(p.name)=="sliceStart") start=std::get<int64_t>(p.value);
            result=start>=64?0:a(0)>>start;
        }
        else throw std::runtime_error("unsupported test op: " + std::string(kind));
        const auto width = m.types()[m.values()[value.index - 1].type.index - 1].width;
        check(width <= 64, "test evaluator only supports narrow values");
        values[value.index] = width==64?result:result & ((uint64_t{1} << width) - 1);
        ready[value.index] = true;
        return values[value.index];
    }
    const GrhSimModel &m;
    std::vector<OpId> defs;
    std::vector<std::vector<uint64_t>> states;
    std::vector<uint64_t> values, inputValues;
    std::vector<bool> ready;
};

GrhSimModel fixture(unsigned mode,unsigned rowCount=4,bool opaqueNames=false) {
    GrhSimModel m("decoded-updates");
    m.addDialect("core", "1", "wolvrix.grhsim.core.v1");
    const auto domain=mode==14?LogicDomain::FourState:LogicDomain::TwoState;
    const auto bit = m.logicType(1, false, domain);
    const auto word = m.logicType(mode==19?1:2, false, domain);
    const auto addr = m.logicType(mode==13?64:rowCount>4?6:3, false, LogicDomain::TwoState);
    auto compute = [&](std::string_view kind, TypeId type, std::vector<ValueId> args) {
        auto value = m.addValue(type);
        m.addOperation(kind, args, std::array{value});
        return value;
    };
    auto input = [&](std::string_view name, TypeId type) {
        auto id = m.addInput(name, type);
        auto value = m.addValue(type);
        m.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(id)});
        return value;
    };
    auto constant = [&](TypeId type, uint64_t n) {
        auto value = m.addValue(type);
        m.addOperation("core.compute.constant", {}, std::array{value}, {},
            std::array{Parameter{m.intern("value"), std::to_string(m.types()[type.index - 1].width) + "'d" + std::to_string(n)}});
        return value;
    };
    const auto a0 = input("a0", addr), a1 = input("a1", addr);
    const auto d0 = input("d0", word), d1 = input("d1", word);
    const auto e0 = input("e0", bit), e1 = input("e1", bit), u = input("u", bit);
    const auto event = input("clock", bit);
    const auto mask = constant(word, mode==10?1:3), zero = constant(word, 0);
    const auto notHighEnable = compute("core.compute.logicNot", bit, {e1});
    auto reset = mode == 5 ? compute("core.compute.logicOr", bit, {u, e0}) : u;
    if(mode==7) reset=compute("core.compute.logicOr",bit,{u,compute("core.compute.reduceAnd",bit,{d0})});
    const auto notReset = compute("core.compute.logicNot", bit, {reset});
    const auto lastData = mode==20?compute("core.compute.xor",word,{d0,d1}):ValueId{};
    for (unsigned row = 0; row < rowCount; ++row) {
        auto q = m.addState(opaqueNames?"unrelated_"+std::to_string((row*17+3)%37):"q"+std::to_string(row), word);
        auto h = m.addState(opaqueNames?"edge_sample_"+std::to_string(rowCount-row):"h"+std::to_string(row), bit);
        const std::array steps{InitStep{m.intern("core.init.const"), {0, 1}}};
        if (mode == 15) {
            m.addInit(q, std::array{InitStep{m.intern("core.init.random"), {0, 1}}},
                std::array{Parameter{m.intern("seed"), int64_t{row + 1}}});
        } else m.addInit(q, steps, std::array{Parameter{m.intern("value"),
            std::string(mode==19?"1'd":"2'd") + std::to_string(row%(mode==19?2:4))}});
        m.addInit(h, steps, std::array{Parameter{m.intern("value"), std::string(mode==9 && row==3?"1'b0":"1'b1")}});
        if(mode==11 && row==0) {
            const auto historyRead=m.addValue(bit);
            m.addOperation("core.state.read",{},std::array{historyRead},std::array{ObjectRef::state(h)});
            const auto historyOutput=m.addOutput("visible-history",bit);
            m.addOperation("core.output.write",std::array{historyRead},{},std::array{ObjectRef::output(historyOutput)});
        }
        auto old = m.addValue(word);
        m.addOperation("core.state.read", {}, std::array{old}, std::array{ObjectRef::state(q)});
        auto output = m.addOutput("q" + std::to_string(row), word);
        m.addOperation("core.output.write", std::array{old}, {}, std::array{ObjectRef::output(output)});
        const auto rowValue = constant(addr, mode==13?~uint64_t{3}+row:row + (mode==6?4:1));
        auto hit0 = compute("core.compute.eq", bit, {a0, rowValue});
        auto hit1 = compute("core.compute.eq", bit, {mode==8?a0:a1, rowValue});
        if(mode==6 && row==3) hit1=compute("core.compute.reduceAnd",bit,{a1});
        auto low = compute("core.compute.logicAnd", bit, {e0, hit0});
        auto high = compute("core.compute.logicAnd", bit, {e1, hit1});
        if (mode == 2) { // Global else-if must suppress even different addresses.
            low = compute("core.compute.logicAnd", bit, {low, notHighEnable});
        }
        if (mode == 3) { // Row-local blocker may become sequence priority.
            auto notHigh = compute("core.compute.logicNot", bit, {high});
            low = compute("core.compute.logicAnd", bit, {low, notHigh});
        }
        if (mode >= 4 && mode!=20) {
            low = compute("core.compute.logicAnd", bit, {low, notReset});
            high = compute("core.compute.logicAnd", bit, {high, notReset});
        }
        auto update = mode == 0 ? u : compute("core.compute.logicOr", bit, {low, high});
        if (mode >= 4 && mode!=20) update = compute("core.compute.logicOr", bit, {reset, update});
        // Mode 21: the reset fallback writes the row number (identity init), so
        // the fill data is a per-row constant that only the row-constant-fill
        // extension may merge.
        auto next = compute("core.compute.mux", word, {low, d0, mode == 0 ? old :
            (mode == 21 ? constant(word, row) : zero)});
        next = compute("core.compute.mux", word, {high, d1, next});
        if (mode == 22) {
            // Both fills write zero in the first row, but disagree elsewhere.
            // The higher-priority fill must not be reordered before memFill.
            const auto fill = compute("core.compute.logicAnd", bit, {reset, e0});
            next = compute("core.compute.mux", word, {fill, constant(word, row), next});
        }
        if(mode==20) {
            // Same-address writes separated by a possibly aliasing write must
            // preserve the middle writer's priority; adjacent fusion cannot jump it.
            const auto last=compute("core.compute.logicAnd",bit,{u,hit0});
            next=compute("core.compute.mux",word,{last,lastData,next});
            update=compute("core.compute.logicOr",bit,{update,last});
        }
        std::vector<ValueId> operands{update,next,mask,mode==16 && row==3?u:event};
        if(mode==18 || mode==19) {
            // One indexed writer may retain a dynamic element mask. For one-bit
            // elements that mask is also valid as an enable predicate.
            operands[0]=high;
            operands[1]=compute("core.compute.mux",word,{high,d1,old});
            operands[2]=d0;
        }
        std::vector<ObjectRef> objects{ObjectRef::state(q),ObjectRef::state(h)};
        std::vector<std::string> edges{"posedge"};
        if(mode==12) {
            auto resetHistory=m.addState("reset-history"+std::to_string(row),bit);
            m.addInit(resetHistory,steps,std::array{Parameter{m.intern("value"),std::string("1'b0")}});
            objects.push_back(ObjectRef::state(resetHistory));
            operands.push_back(u);
            edges.push_back("posedge");
        }
        if(mode==17) m.addOperation("core.state.latchWrite",std::array{update,next,mask},{},
            std::array{ObjectRef::state(q)});
        else m.addOperation("core.state.regWrite",operands,{},objects,
            std::array{Parameter{m.intern("event_edges"),edges}});
    }
    return m;
}

// Observe scalar reads through an event-sensitive side effect with no results.
// Its history and old-state arguments must survive storage compaction.
void addDpiProbe(GrhSimModel &model) {
    const auto bit = model.logicType(1, false, LogicDomain::TwoState);
    ValueId enable, event;
    std::vector<ValueId> reads;
    std::vector<DpiArgument> arguments;
    for (const auto &op : model.operations()) {
        if (model.text(op.opType) == "core.input.read") {
            const auto name = model.text(model.inputs()[model.objectRefs(op)[0].index - 1].name);
            if (name == "e0") enable = model.results(op)[0];
            if (name == "clock") event = model.results(op)[0];
        }
        if (model.text(op.opType) == "core.state.read") {
            const auto value = model.results(op)[0];
            const auto type = model.values()[value.index - 1].type;
            reads.push_back(value);
            arguments.push_back({model.intern("arg" + std::to_string(reads.size())), DpiDirection::Input, type});
        }
    }
    check(enable && event && reads.size() == 4, "invalid DPI probe fixture");
    const auto function = model.addExternFunction("observe", "core.dpi", "observe", arguments, {});
    const auto history = model.addState("dpi-history", bit);
    model.addInit(history, std::array{InitStep{model.intern("core.init.const"), {0, 1}}},
        std::array{Parameter{model.intern("value"), std::string("1'b0")}});
    std::vector<ValueId> operands{enable};
    operands.insert(operands.end(), reads.begin(), reads.end());
    operands.push_back(event);
    model.addOperation("core.dpi.call", operands, {},
        std::array{ObjectRef::function(function), ObjectRef::state(history)},
        std::array{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}});
}

GrhSimModel readFixture(bool readOnly, bool repeated, unsigned window=0, unsigned viewStyle=0,
                       unsigned elementWidth=0) {
    GrhSimModel m("shared-reads");
    m.addDialect("core", "1", "wolvrix.grhsim.core.v1");
    const auto bit=m.logicType(1,false,LogicDomain::TwoState);
    if(!elementWidth) elementWidth=window?1:2;
    const auto word=m.logicType(elementWidth,false,LogicDomain::TwoState);
    const auto addr=m.logicType(window?64:4,false,LogicDomain::TwoState);
    auto input=[&](std::string_view name,TypeId type) {
        const auto id=m.addInput(name,type);
        const auto value=m.addValue(type);
        m.addOperation("core.input.read",{},std::array{value},std::array{ObjectRef::input(id)});
        return value;
    };
    const auto index0=input("index0",addr), index1=input("index1",addr);
    const auto enable=input("enable",bit), data=input("data",word), event=input("clock",bit);
    const auto mask=m.addValue(word);
    m.addOperation("core.compute.constant",{},std::array{mask},{},
        std::array{Parameter{m.intern("value"),std::to_string(elementWidth)+"'d"+std::to_string((1u<<elementWidth)-1)}});
    std::vector<ValueId> reads;
    for(unsigned row=0;row<4;++row) {
        const auto q=m.addState("q"+std::to_string(row),word);
        const std::array steps{InitStep{m.intern("core.init.const"),{0,1}}};
        m.addInit(q,steps,std::array{Parameter{m.intern("value"),std::to_string(elementWidth)+"'d"+
            std::to_string(row&((1u<<elementWidth)-1))}});
        const auto old=m.addValue(word);
        m.addOperation("core.state.read",{},std::array{old},std::array{ObjectRef::state(q)});
        reads.push_back(old);
        if(row==0) {
            const auto out=m.addOutput("extra-user",word);
            m.addOperation("core.output.write",std::array{old},{},std::array{ObjectRef::output(out)});
        }
        if(readOnly) continue;
        const auto h=m.addState("h"+std::to_string(row),bit);
        m.addInit(h,steps,std::array{Parameter{m.intern("value"),std::string("1'b0")}});
        const auto next=m.addValue(word);
        m.addOperation("core.compute.xor",std::array{old,data},std::array{next});
        m.addOperation("core.state.regWrite",std::array{enable,next,mask,event},{},
            std::array{ObjectRef::state(q),ObjectRef::state(h)},
            std::array{Parameter{m.intern("event_edges"),std::vector<std::string>{"posedge"}}});
    }
    std::vector<ValueId> lanes(reads.rbegin(),reads.rend());
    if(repeated) lanes.insert(lanes.end(),reads.rbegin(),reads.rend());
    if(viewStyle==1) lanes={reads[3],reads[3],reads[2],reads[1],reads[0],reads[0]};
    if(viewStyle==2) lanes={reads[3],reads[2],reads[1],reads[2],reads[1],reads[0]};
    const auto packedType=m.logicType(lanes.size()*elementWidth,false,LogicDomain::TwoState);
    const auto packed=m.addValue(packedType);
    m.addOperation("core.compute.concat",lanes,std::array{packed});
    for(auto index:{index0,index1}) {
        const auto outputType=window?m.logicType(3,false,LogicDomain::TwoState):word;
        const auto selected=m.addValue(outputType);
        if(window==2) {
            const auto shifted=m.addValue(packedType);
            m.addOperation("core.compute.lshr",std::array{packed,index},std::array{shifted});
            m.addOperation("core.compute.sliceStatic",std::array{shifted},std::array{selected},{},
                std::array{Parameter{m.intern("sliceStart"),int64_t{1}},Parameter{m.intern("sliceEnd"),int64_t{3}}});
        } else m.addOperation(window?"core.compute.sliceDynamic":"core.compute.sliceArray",std::array{packed,index},std::array{selected},{},
            std::array{Parameter{m.intern("sliceWidth"),int64_t{window?3:elementWidth}}});
        const auto output=m.addOutput("selected"+std::to_string(index.index),outputType);
        m.addOperation("core.output.write",std::array{selected},{},std::array{ObjectRef::output(output)});
    }
    return m;
}
}

void regToMemSemanticsTests() {
    {
        // Names are diagnostic hints only. Row zero may be a constant output
        // outside the recovered writable range [1, 5).
        auto original=fixture(1),rewritten=fixture(1,4,true);
        const auto addZero=[](GrhSimModel &model) {
            const auto word=model.logicType(2,false,LogicDomain::TwoState);
            const auto zero=model.addValue(word);
            model.addOperation("core.compute.constant",{},std::array{zero},{},
                std::array{Parameter{model.intern("value"),std::string("2'd0")}});
            const auto output=model.addOutput("immutable_row_zero",word);
            model.addOperation("core.output.write",std::array{zero},{},std::array{ObjectRef::output(output)});
        };
        addZero(original);addZero(rewritten);
        RegToMemPass pass(semanticOptions());
        diag::Diagnostics diagnostics;
        check(pass.run(rewritten,diagnostics).changed,"opaque names prevented write discovery");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid constant-row rewrite");
        unsigned arrays=0;
        for(const auto &state:rewritten.states()) {
            const auto &type=rewritten.types()[state.type.index-1];
            if(type.kind==TypeKind::Array) {++arrays;check(type.count==4,"invented a writable row zero");}
        }
        check(arrays==1,"renamed rows did not form one table");
        Simulation reference(original),candidate(rewritten);
        for(uint64_t address=0;address<8;++address) for(uint64_t enables=0;enables<4;++enables)
            for(uint64_t clock:{0u,1u}) {
                const std::vector<uint64_t> inputs{address,0,1,3,enables&1,enables>>1,0,clock};
                reference.step(inputs);candidate.step(inputs);
                check(reference.outputs()==candidate.outputs(),"renaming or omitted zero row changed state/output");
                check(candidate.outputs().back()==0,"constant row zero became writable");
            }
    }
    {
        auto model=fixture(1);
        auto registry=makeDefaultDialectRegistry();
        std::string error;
        check(registry.registerDialect({"observer","1","test.observer.v1"},error) &&
            registry.registerOp("observer","observer.state.inspect",error),"cannot register opaque observer");
        model.addDialect("observer","1","test.observer.v1");
        // The pass cannot assume that an unknown state reference is a value read.
        model.addOperation("observer.state.inspect",{},{},std::array{ObjectRef::state(model.states()[0].id)});
        diag::Diagnostics diagnostics;
        std::ostringstream before,after;
        check(writeGrhSimJson(model,before,registry,diagnostics),"invalid opaque observer fixture");
        PassManager manager(registry);
        manager.addPass(std::make_unique<RegToMemPass>(semanticOptions()));
        check(manager.run(model,diagnostics).success,"opaque observer should conservatively skip table");
        check(writeGrhSimJson(model,after,registry,diagnostics),"opaque observer rewrite invalid");
        check(before.str()==after.str(),"opaque state consumer was changed or duplicated");
    }
    {
        // A report error after mutation must not masquerade as changed=false:
        // the manager has to poison the partially completed semantic operation.
        const auto directory=std::filesystem::path(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR)/"reg_to_mem"/"report_directory";
        std::filesystem::create_directories(directory);
        for(bool analysis:{false,true}) {
            auto model=fixture(1);
            auto options=semanticOptions();options.analysisOnly=analysis;options.report=directory;
            diag::Diagnostics diagnostics;
            PassManager manager(defaultDialectRegistry());
            manager.addPass(std::make_unique<RegToMemPass>(options));
            const auto result=manager.run(model,diagnostics);
            check(!result.success && diagnostics.hasError(),"report failure was silently accepted");
            check(model.poisoned()!=analysis,"report failure broke mutation/poison contract");
        }
    }
    {
        auto small=readFixture(false,false,1,1,3);
        diag::Diagnostics diagnostics;
        std::ostringstream before,after;
        check(writeGrhSimJson(small,before,defaultDialectRegistry(),diagnostics),"cost fixture snapshot failed");
        RegToMemPass pass;
        const auto result=pass.run(small,diagnostics);
        check(result.success && !result.changed,"cost selection expanded an expensive small read window");
        check(writeGrhSimJson(small,after,defaultDialectRegistry(),diagnostics),"cost rejection serialization failed");
        check(before.str()==after.str(),"cost rejection mutated model");
        auto original=fixture(1,32),large=fixture(1,32);
        check(pass.run(large,diagnostics).changed,"cost selection rejected large decoded writes");
        Simulation reference(original),candidate(large);
        for(uint64_t address=0;address<64;++address) for(uint64_t clock:{0u,1u}) {
            const std::vector<uint64_t> inputs{address,64-address,1,3,1,1,0,clock};
            reference.step(inputs);candidate.step(inputs);
            check(reference.outputs()==candidate.outputs(),"cost-selected rewrite changed semantics");
        }
    }
    for (unsigned mode : {0u,1u,2u,3u,4u,5u,6u,7u,8u,12u,13u,18u,19u,20u}) {
        auto original = fixture(mode);
        auto rewritten = fixture(mode);
        diag::Diagnostics diagnostics;
        RegToMemOptions options;
        options.enableCostSelection = false;
        options.enableReadRewrite = false; // Scalar read replacement remains mandatory.
        RegToMemPass pass(options);
        check(verifyGrhSimModel(original, defaultDialectRegistry(), diagnostics), "invalid semantics fixture");
        auto result = pass.run(rewritten, diagnostics);
        if (!result.success || !result.changed)
            throw std::runtime_error("decoded write family was not recovered, mode=" + std::to_string(mode));
        check(verifyGrhSimModel(rewritten, defaultDialectRegistry(), diagnostics), "invalid rewritten semantics fixture");
        if(mode==20) {
            unsigned triples=0;
            for(const auto &op:rewritten.operations()) if(rewritten.text(op.opType)=="core.state.memWriteSeq")
                triples+=(rewritten.operands(op).size()-1)/3;
            check(triples==3,"same-address fusion crossed an intervening aliasing write");
        }
        check(!pass.run(rewritten, diagnostics).changed, "reg-to-mem is not idempotent");
        Simulation reference(original), candidate(rewritten);
        for (uint64_t a0 = 0; a0 < 8; ++a0) for (uint64_t a1 = 0; a1 < 8; ++a1)
            for (uint64_t enables = 0; enables < 8; ++enables)
                for (uint64_t data = 0; data < 16; ++data)
                    for (uint64_t clock : {1u, 0u, 1u, 1u}) {
                        const std::vector<uint64_t> inputs{mode==13?~uint64_t{5}+a0:a0,mode==13?~uint64_t{5}+a1:a1, data & 3, data >> 2,
                            enables & 1, (enables >> 1) & 1, enables >> 2, clock};
                        reference.step(inputs);
                        candidate.step(inputs);
                        check(reference.outputs() == candidate.outputs(), "reg-to-mem changed a state transition");
                    }
    }
    {
        auto model = fixture(22, 4, true);
        auto options = semanticOptions();
        options.enableReadRewrite = false;
        options.enableRowConstantFill = true;
        diag::Diagnostics diagnostics;
        check(verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics),
            "invalid overlapping row-constant-fill fixture");
        std::ostringstream before, after;
        check(writeGrhSimJson(model, before, defaultDialectRegistry(), diagnostics),
            "overlapping fill snapshot failed");
        RegToMemPass pass(options);
        const auto result = pass.run(model, diagnostics);
        check(result.success && !result.changed,
            "overlapping fills were accepted using only the first row's data");
        check(writeGrhSimJson(model, after, defaultDialectRegistry(), diagnostics),
            "overlapping fill resnapshot failed");
        check(before.str() == after.str(), "rejected overlapping fills mutated the model");
    }
    for (bool rowConstantFill : {false, true}) {
        // NO00022: identity-init reset writes the row number, so the fill data
        // is a per-row constant.  The family must stay scalar without the gate
        // and merge into one static-triple sequence with it.
        auto original = fixture(21), rewritten = fixture(21, 4, true);
        diag::Diagnostics diagnostics;
        RegToMemOptions options;
        options.enableCostSelection = false;
        options.enableReadRewrite = false;
        options.enableRowConstantFill = rowConstantFill;
        RegToMemPass pass(options);
        check(verifyGrhSimModel(original, defaultDialectRegistry(), diagnostics),
            "invalid row-constant-fill fixture");
        if (!rowConstantFill) {
            std::ostringstream before, after;
            check(writeGrhSimJson(rewritten, before, defaultDialectRegistry(), diagnostics),
                "row-constant-fill gate-off snapshot failed");
            const auto result = pass.run(rewritten, diagnostics);
            check(result.success && !result.changed, "row-constant fill merged without its gate");
            check(writeGrhSimJson(rewritten, after, defaultDialectRegistry(), diagnostics),
                "row-constant-fill gate-off resnapshot failed");
            check(before.str() == after.str(), "disabled row-constant fill mutated the model");
            continue;
        }
        const auto result = pass.run(rewritten, diagnostics);
        check(result.success && result.changed, "row-constant-fill family was not recovered");
        check(verifyGrhSimModel(rewritten, defaultDialectRegistry(), diagnostics),
            "invalid row-constant-fill rewrite");
        auto constValue = [&](ValueId v) -> std::optional<uint64_t> {
            for (const auto &op : rewritten.operations())
                for (auto result : rewritten.results(op)) {
                    if (result != v || rewritten.text(op.opType) != "core.compute.constant") continue;
                    for (const auto &p : rewritten.parameters(op))
                        if (rewritten.text(p.name) == "value") {
                            const auto text = std::get<std::string>(p.value);
                            return std::stoull(text.substr(text.find('\'') + 2));
                        }
                }
            return std::nullopt;
        };
        unsigned sequences = 0, fills = 0;
        for (const auto &op : rewritten.operations()) {
            const auto kind = rewritten.text(op.opType);
            fills += kind == "core.state.memFill";
            if (kind != "core.state.memWriteSeq") continue;
            ++sequences;
            const auto args = rewritten.operands(op);
            std::size_t events = 0;
            for (const auto &p : rewritten.parameters(op))
                if (rewritten.text(p.name) == "event_edges")
                    events = std::get<std::vector<std::string>>(p.value).size();
            const std::size_t triples = (args.size() - events) / 3;
            check(triples == 6, "row-constant fill did not emit 4 static + 2 port triples");
            const auto sharedEnable = args[0];
            for (std::size_t row = 0; row < 4; ++row) {
                check(args[row * 3] == sharedEnable, "static fill triples lost the shared fill enable");
                check(constValue(args[row * 3 + 1]) == std::optional<uint64_t>(row),
                    "static fill triple address is not the row position");
                check(constValue(args[row * 3 + 2]) == std::optional<uint64_t>(row),
                    "static fill triple data is not the per-row constant");
            }
        }
        check(sequences == 1 && fills == 0, "row-constant fill left a memFill or extra sequence");
        check(!pass.run(rewritten, diagnostics).changed, "row-constant fill is not idempotent");
        Simulation reference(original), candidate(rewritten);
        for (uint64_t a0 = 0; a0 < 8; ++a0) for (uint64_t a1 = 0; a1 < 8; ++a1)
            for (uint64_t enables = 0; enables < 8; ++enables)
                for (uint64_t data = 0; data < 16; ++data)
                    for (uint64_t clock : {1u, 0u, 1u, 1u}) {
                        const std::vector<uint64_t> inputs{a0, a1, data & 3, data >> 2,
                            enables & 1, (enables >> 1) & 1, enables >> 2, clock};
                        reference.step(inputs);
                        candidate.step(inputs);
                        check(reference.outputs() == candidate.outputs(),
                            "row-constant fill changed a state transition");
                    }
    }
    for(bool readOnly:{false,true}) for(bool repeated:{false,true}) {
        auto original=readFixture(readOnly,repeated), rewritten=readFixture(readOnly,repeated);
        diag::Diagnostics diagnostics;
        RegToMemPass pass(semanticOptions());
        check(verifyGrhSimModel(original,defaultDialectRegistry(),diagnostics),"invalid read fixture");
        const auto result=pass.run(rewritten,diagnostics);
        check(result.success && result.changed,"read-side table was not recovered");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid read rewrite");
        for(const auto &op:rewritten.operations())
            check(rewritten.text(op.opType)!="core.compute.concat","packed read remained after indexed recovery");
        Simulation reference(original),candidate(rewritten);
        for(uint64_t i=0;i<16;++i) for(uint64_t j=0;j<16;++j)
            for(uint64_t data=0;data<4;++data) for(uint64_t enable=0;enable<2;++enable)
                for(uint64_t clock:{0u,1u,1u}) {
                    const std::vector<uint64_t> inputs{i,j,enable,data,clock};
                    reference.step(inputs);candidate.step(inputs);
                    check(reference.outputs()==candidate.outputs(),"indexed read changed state/output semantics");
                }
    }
    {
        const auto build=[]() {
            auto model=readFixture(false,true);
            ValueId alternateEvent,packed;
            std::vector<OpId> writes;
            for(const auto &op:model.operations()) {
                if(model.text(op.opType)=="core.input.read" &&
                    model.text(model.inputs()[model.objectRefs(op)[0].index-1].name)=="enable")
                    alternateEvent=model.results(op)[0];
                if(model.text(op.opType)=="core.compute.concat") packed=model.results(op)[0];
                if(model.text(op.opType)=="core.state.regWrite") writes.push_back(op.id);
            }
            for(std::size_t i=1;i<writes.size();i+=2) {
                const auto op=model.operations()[writes[i].index-1];
                std::vector<ValueId> args(model.operands(op).begin(),model.operands(op).end());
                std::vector<ObjectRef> refs(model.objectRefs(op).begin(),model.objectRefs(op).end());
                std::vector<Parameter> params(model.parameters(op).begin(),model.parameters(op).end());
                args.back()=alternateEvent;
                model.replaceOperation(op.id,"core.state.regWrite",args,{},refs,params);
            }
            const auto output=model.addOutput("whole-packed-view",model.values()[packed.index-1].type);
            model.addOperation("core.output.write",std::array{packed},{},std::array{ObjectRef::output(output)});
            return model;
        };
        auto original=build(),rewritten=build();
        auto options=semanticOptions();options.enableWriteMerge=false;
        RegToMemPass pass(options);
        diag::Diagnostics diagnostics;
        check(pass.run(rewritten,diagnostics).changed,"read recovery failed with distinct row events");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid distinct-event table");
        Simulation reference(original),candidate(rewritten);
        std::mt19937 random(260910);
        for(unsigned sample=0;sample<4096;++sample) {
            const std::vector<uint64_t> inputs{random()%16,random()%16,random()%2,random()%4,random()%2};
            reference.step(inputs);candidate.step(inputs);
            check(reference.outputs()==candidate.outputs(),"fixed writes lost per-row event or full packed output");
        }
    }
    for(unsigned mode:{9u,10u,11u,14u,15u,16u,17u}) {
        auto model=fixture(mode);
        diag::Diagnostics diagnostics;
        std::ostringstream before,after;
        check(writeGrhSimJson(model,before,defaultDialectRegistry(),diagnostics),"cannot snapshot rejected fixture");
        RegToMemPass pass(semanticOptions());
        const auto result=pass.run(model,diagnostics);
        check(result.success && !result.changed,"unsafe write family was accepted");
        check(writeGrhSimJson(model,after,defaultDialectRegistry(),diagnostics),"rejected fixture became invalid");
        check(before.str()==after.str(),"rejecting reg-to-mem mutated the model");
    }
    {
        auto original = fixture(1), rewritten = fixture(1);
        addDpiProbe(original);
        addDpiProbe(rewritten);
        diag::Diagnostics diagnostics;
        check(verifyGrhSimModel(original, defaultDialectRegistry(), diagnostics), "invalid DPI fixture");
        RegToMemPass pass(semanticOptions());
        const auto result = pass.run(rewritten, diagnostics);
        check(result.success && result.changed, "DPI user prevented table recovery");
        check(verifyGrhSimModel(rewritten, defaultDialectRegistry(), diagnostics), "invalid DPI rewrite");
        Simulation reference(original), candidate(rewritten);
        std::mt19937 random(260909);
        std::size_t callCount = 0;
        for (unsigned sample = 0; sample < 4096; ++sample) {
            const std::vector<uint64_t> inputs{random()%8, random()%8, random()%4, random()%4,
                random()%2, random()%2, random()%2, random()%2};
            reference.step(inputs);
            candidate.step(inputs);
            check(reference.calls == candidate.calls, "DPI call count or old-state arguments changed");
            check(reference.outputs() == candidate.outputs(), "DPI fixture state changed");
            callCount += candidate.calls.size();
        }
        check(callCount > 100, "DPI trace did not exercise enabled events");
    }
    for(unsigned window:{1u,2u}) for(bool repeated:{false,true}) {
        auto original=readFixture(false,repeated,window),rewritten=readFixture(false,repeated,window);
        diag::Diagnostics diagnostics;
        RegToMemPass pass(semanticOptions());
        const auto result=pass.run(rewritten,diagnostics);
        check(result.success && result.changed,"packed bit window was not recovered");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid bit window rewrite");
        for(const auto &op:rewritten.operations()) {
            check(rewritten.text(op.opType)!="core.compute.lshr","wide packed shift was retained");
            if(rewritten.text(op.opType)=="core.compute.concat")
                check(rewritten.types()[rewritten.values()[rewritten.results(op)[0].index-1].type.index-1].width==3,
                    "full packed view was retained");
        }
        Simulation reference(original),candidate(rewritten);
        const std::vector<uint64_t> indices{0,1,2,3,4,5,6,7,8,9,63,64,~uint64_t{0},~uint64_t{1}};
        for(auto i:indices) for(auto j:indices) for(uint64_t data:{0u,1u}) for(uint64_t enable:{0u,1u})
            for(uint64_t clock:{0u,1u,1u}) {
                const std::vector<uint64_t> inputs{i,j,enable,data,clock};
                reference.step(inputs);candidate.step(inputs);
                check(reference.outputs()==candidate.outputs(),"packed window changed partial/overflow semantics");
            }
    }
    for(unsigned style:{1u,2u}) for(unsigned window:{0u,1u,2u}) {
        auto original=readFixture(false,false,window,style);
        auto rewritten=readFixture(false,false,window,style);
        diag::Diagnostics diagnostics;
        RegToMemPass pass(semanticOptions());
        const auto result=pass.run(rewritten,diagnostics);
        check(result.success && result.changed,"nonperiodic view was not recovered");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid nonperiodic view rewrite");
        for(const auto &op:rewritten.operations()) {
            const auto kind=rewritten.text(op.opType);
            check(kind!="core.compute.sliceArray" && kind!="core.compute.sliceDynamic" &&
                kind!="core.compute.lshr","nonperiodic view retained indexed packed selection");
        }
        Simulation reference(original),candidate(rewritten);
        const std::vector<uint64_t> indices{0,1,2,3,4,5,6,7,8,15,~uint64_t{0}};
        for(auto i:indices) for(auto j:indices) for(uint64_t data=0;data<(window?2u:4u);++data)
            for(uint64_t clock:{0u,1u,1u}) {
                // Non-window fixtures have four-bit index inputs.
                const std::vector<uint64_t> inputs{window?i:i&15,window?j:j&15,1,data,clock};
                reference.step(inputs);candidate.step(inputs);
                check(reference.outputs()==candidate.outputs(),"nonperiodic row mapping changed output/state");
            }
    }
    for(unsigned elementWidth:{2u,3u,5u}) for(unsigned style:{0u,1u,2u}) for(unsigned window:{1u,2u}) {
        auto original=readFixture(false,false,window,style,elementWidth);
        auto rewritten=readFixture(false,false,window,style,elementWidth);
        diag::Diagnostics diagnostics;
        RegToMemPass pass(semanticOptions());
        const auto result=pass.run(rewritten,diagnostics);
        check(result.success && result.changed,"multi-bit element window was not recovered");
        check(verifyGrhSimModel(rewritten,defaultDialectRegistry(),diagnostics),"invalid multi-bit window rewrite");
        Simulation reference(original),candidate(rewritten);
        for(uint64_t start=0;start<6*elementWidth+3;++start) for(uint64_t data=0;data<(1u<<elementWidth);++data)
            for(uint64_t clock:{0u,1u}) {
                const std::vector<uint64_t> inputs{start,~uint64_t{0},1,data,clock};
                reference.step(inputs);candidate.step(inputs);
                check(reference.outputs()==candidate.outputs(),"multi-bit window changed cross-lane/overflow semantics");
            }
    }
    {
        auto model=fixture(1);
        diag::Diagnostics diagnostics;
        std::ostringstream before,after;
        check(writeGrhSimJson(model,before,defaultDialectRegistry(),diagnostics),"analysis snapshot failed");
        auto options=semanticOptions(); options.analysisOnly=true;
        RegToMemPass analysis(options);
        const auto result=analysis.run(model,diagnostics);
        check(result.success && !result.changed,"analysis mutated state");
        check(writeGrhSimJson(model,after,defaultDialectRegistry(),diagnostics),"analysis serialization failed");
        check(before.str()==after.str(),"analysis changed serialized model");
        PassManager mapping(defaultDialectRegistry());
        std::string error;
        mapping.addPass(defaultPassRegistry().create("cpu.st.split-phase",{},error));
        check(error.empty() && mapping.run(model,diagnostics).success && model.cpuMapping(),
            "could not establish mapping for invalidation test");
        PassManager manager(defaultDialectRegistry());
        manager.addPass(std::make_unique<RegToMemPass>(semanticOptions()));
        const auto revision=model.semanticRevision();
        check(manager.run(model,diagnostics).success,"semantic pass manager failed");
        check(model.semanticRevision()==revision+1,"semantic revision was not committed once");
        check(!model.cpuMapping() && model.mappings().empty(),"semantic rewrite retained stale backend mappings");
        check(manager.run(model,diagnostics).success && model.semanticRevision()==revision+1,
            "idempotent run changed semantic revision");
    }
    {
        auto model = fixture(1);
        std::vector<ValueId> reads;
        ValueId address;
        for (const auto &op : model.operations()) {
            if (model.text(op.opType) == "core.state.read") reads.push_back(model.results(op)[0]);
            if (model.text(op.opType) == "core.input.read" &&
                model.text(model.inputs()[model.objectRefs(op)[0].index - 1].name) == "a0")
                address = model.results(op)[0];
        }
        std::reverse(reads.begin(), reads.end());
        const auto packed = model.addValue(model.logicType(8, false, LogicDomain::TwoState));
        model.addOperation("core.compute.concat", reads, std::array{packed});
        const auto element = model.logicType(2, false, LogicDomain::TwoState);
        const auto selected = model.addValue(element);
        model.addOperation("core.compute.sliceArray", std::array{packed, address}, std::array{selected}, {},
            std::array{Parameter{model.intern("sliceWidth"), int64_t{2}}});
        const auto output = model.addOutput("indexed", element);
        model.addOperation("core.output.write", std::array{selected}, {}, std::array{ObjectRef::output(output)});
        diag::Diagnostics diagnostics;
        check(verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics), "invalid overlapping candidates");
        const auto directory = std::filesystem::path(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR) / "reg_to_mem";
        std::filesystem::create_directories(directory);
        auto options=semanticOptions();
        options.analysisOnly = true;
        options.report = directory / "overlap_analysis.tsv";
        RegToMemPass analysis(options);
        check(analysis.run(model, diagnostics).success, "overlap analysis failed");
        const auto statuses = [](const std::filesystem::path &path) {
            std::ifstream stream(path);
            check(bool(stream), "cannot read candidate report");
            std::vector<std::string> result;
            std::string line;
            std::getline(stream, line);
            while (std::getline(stream, line)) {
                if(line.starts_with("excluded-state\t")) continue;
                std::istringstream row(line);
                std::string field;
                for (unsigned column = 0; column < 5; ++column) std::getline(row, field, '\t');
                result.push_back(field);
            }
            return result;
        };
        check(statuses(options.report) == std::vector<std::string>{"eligible", "ownership"},
            "analysis accepted overlapping storage owners");
        options.analysisOnly = false;
        options.report = directory / "overlap_rewrite.tsv";
        RegToMemPass rewrite(options);
        check(rewrite.run(model, diagnostics).success, "overlap rewrite failed");
        check(statuses(options.report) == std::vector<std::string>{"merged", "ownership"},
            "analysis and rewrite chose different storage owners");
        check(verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics), "invalid overlap rewrite");
    }
    for(const auto &arguments:std::vector<std::vector<std::string_view>>{
        {"--min-element-count","0"},{"--min-element-count","1"},{"--min-element-count","-4"},
        {"--min-element-count","4junk"},{"--enable-read-rewrite","maybe"},
        {"--enable-cost-selection","maybe"},{"--unknown","true"},
        {"--report"}}) {
        std::string error;
        check(!defaultPassRegistry().create("grhsim.reg-to-mem",arguments,error) && !error.empty(),
            "invalid factory options accepted");
    }
}

void regToMemEmitChecks(const std::filesystem::path &directory) {
    for(unsigned shape=0;shape<10;++shape) {
        const auto build=[&]() {
            if(shape==8) return fixture(21,4,true);
            if(shape==9) return fixture(22,4,true);
            if(shape==4) return readFixture(false,false,1,1);
            if(shape==5) return readFixture(false,false,0,2);
            if(shape==6) return readFixture(false,false,1,1,3);
            if(shape==7) return readFixture(false,false,2,2,5);
            return shape?readFixture(false,true,shape-1):fixture(12);
        };
        auto original=build(), rewritten=build();
        const auto path=directory/std::array{"writes","reads","windows","shifted_windows","edge_window","overlap",
            "multi_bit_window","multi_bit_shift","row_constant_fill","row_constant_fill_overlap"}[shape];
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        diag::Diagnostics diagnostics;
        PassManager manager(defaultDialectRegistry());
        auto options = semanticOptions();
        options.enableRowConstantFill = shape >= 8;
        manager.addPass(std::make_unique<RegToMemPass>(options));
        for(auto name:{"cpu.st.split-phase","cpu.st.form-event-domains","cpu.st.build-compute-nodes",
            "cpu.st.merge-compute-supernodes","cpu.st.pack-active-words","cpu.st.pack-emit-functions",
            "cpu.st.layout-data","cpu.st.build-schedule"}) {
            std::string error;
            auto pass=defaultPassRegistry().create(name,{},error);
            check(bool(pass),"cannot create CPU mapping pass");
            manager.addPass(std::move(pass));
        }
        check(manager.run(rewritten,diagnostics).success,"reg-to-mem CPU mapping failed");
        if(!emitCpuCpp(rewritten,path,diagnostics).success) {
            std::string message="reg-to-mem CPU emit failed shape="+std::to_string(shape);
            for(const auto &diagnostic:diagnostics.messages()) message+="\n"+diagnostic.message;
            throw std::runtime_error(message);
        }
        auto identifier=[](std::string_view value) {
            std::string result(value);
            for(auto &c:result) if(c=='-') c='_';
            return result;
        };
        const auto modelName=identifier(original.text(original.name()));
        std::ofstream driver(path/"main.cpp");
        driver<<"#include \"grhsim_"<<modelName<<".hpp\"\n#include <fstream>\n#include <iostream>\nint main(){\n"
            <<"GrhSIM_"<<modelName<<" model; model.init(); std::ifstream trace(\"trace.txt\"); uint64_t value; unsigned sample=0;\n"
            <<"while(trace>>value){\n";
        bool first=true;
        for(const auto &input:original.inputs()) {
            if(!first) driver<<"trace>>value;\n";
            first=false;
            driver<<"model."<<identifier(original.text(input.name))<<"=value;\n";
        }
        driver<<"model.eval();\n";
        for(const auto &output:original.outputs())
            driver<<"trace>>value; if(model."<<identifier(original.text(output.name))
                <<"!=value){std::cerr<<\"output mismatch at sample \"<<sample;return 1;}\n";
        driver<<"++sample;} return sample==4096?0:2;}\n";
        std::ofstream trace(path/"trace.txt");
        Simulation reference(original);
        std::mt19937 random(260907);
        for(unsigned i=0;i<4096;++i) {
            std::vector<uint64_t> values;
            for(const auto &input:original.inputs()) {
                const auto width=original.types()[input.type.index-1].width;
                values.push_back(width==64?(i%31==0?~uint64_t{0}:random()%12):random()&((1u<<width)-1));
                trace<<values.back()<<' ';
            }
            reference.step(values);
            for(auto value:reference.outputs()) trace<<value<<' ';
            trace<<'\n';
        }
        check(bool(driver) && bool(trace),"failed to write generated differential test");
    }
}
