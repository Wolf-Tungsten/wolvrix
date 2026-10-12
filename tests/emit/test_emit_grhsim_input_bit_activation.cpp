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
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::transform;

namespace {
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
template<class T> void slot(SessionStore &session, const std::string &name, T value) {
    session.insert_or_assign("top.activity_schedule." + name,
        std::make_unique<SessionSlotValue<T>>(std::move(value), "input-bit-activation-test"));
}
int fail(const std::string &text) {
    std::cerr << "[input-bit-activation] " << text << '\n';
    return 1;
}

int run() {
    constexpr const char *key = "WOLVRIX_GRHSIM_INPUT_BIT_ACTIVATION";
    const char *raw = std::getenv(key);
    const std::optional<std::string> saved = raw ? std::optional<std::string>(raw) : std::nullopt;
    struct Restore {
        const char *key; const std::optional<std::string> &value;
        ~Restore() { if (value) ::setenv(key, value->c_str(), 1); else ::unsetenv(key); }
    } restore{key, saved};
    ::unsetenv(key);
    const auto root = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_input_bit_activation";
    std::map<std::string, std::string> defaultFiles;
    std::string expectedTrace;
    for (const std::string mode : {"default", "off", "on", "environment", "precedence",
                                   "bitmap", "split", "guard", "perf", "fullpass", "posedge"}) {
        Design design;
        Graph &graph = design.createGraph("top"); design.markAsTop(graph.symbol());
        ActivityScheduleSupernodeToOps nodes(24);
        auto value = [&](const std::string &name, int32_t width) {
            return graph.createValue(graph.internSymbol(name), width, false, ValueType::Logic);
        };
        auto input = [&](const std::string &name, int32_t width) {
            auto v = value(name, width); graph.bindInputPort(name, v); return v;
        };
        auto op = [&](uint32_t node, OperationKind kind, const std::string &name,
                      std::vector<ValueId> args, int32_t width) {
            const auto id = graph.createOperation(kind, graph.internSymbol(name + "_op"));
            for (auto arg : args) graph.addOperand(id, arg);
            const auto v = value(name, width); graph.addResult(id, v); nodes[node].push_back(id);
            return v;
        };
        auto constant = [&](const std::string &name, int32_t width, const std::string &literal) {
            auto v = op(0, OperationKind::kConstant, name, {}, width);
            graph.setAttr(graph.valueDef(v), "constValue", literal); return v;
        };
        auto slice = [&](uint32_t node, const std::string &name, ValueId source, int64_t bit) {
            auto v = op(node, OperationKind::kSliceStatic, name, {source}, 1);
            graph.setAttr(graph.valueDef(v), "sliceStart", bit);
            graph.setAttr(graph.valueDef(v), "sliceEnd", bit); return v;
        };
        auto range = [&](uint32_t node, const std::string &name, ValueId source,
                         int64_t start, int32_t width) {
            auto v = op(node, OperationKind::kSliceStatic, name, {source}, width);
            graph.setAttr(graph.valueDef(v), "sliceStart", start);
            graph.setAttr(graph.valueDef(v), "sliceEnd", start + width - 1);
            graph.bindOutputPort(name, v); return v;
        };
        const auto one = constant("one", 1, "1'b1");
        const auto zero130 = constant("zero130", 130, "130'h0");
        const auto c65 = constant("c65", 8, "8'd65");
        const auto c16 = constant("c16", 8, "8'd16");
        const auto a = input("a", 193), b = input("b", 129);
        const auto control = input("control", 1), clock = input("clock", 1), data = input("data", 1);
        const auto index = input("index", 8), whole = input("whole", 128);
        const auto dyn = input("dynamic_bus", 193), fixed = input("fixed_bus", 128);
        const auto intent = input("intent_bus", 128), effect = input("effect_bus", 128);
        const auto tail = input("tail", 65), narrow = input("narrow", 64);
        const auto flagged = input("flagged_bus", 128);
        const auto pad = value("pad_input", 130);
        graph.bindInoutPort("pad", pad, zero130, one);

        const auto a3 = slice(1, "a3", a, 3), a140 = slice(1, "a140", a, 140);
        const auto b65 = slice(1, "b65", b, 65);
        auto mix = op(1, OperationKind::kXor, "mix0", {a3, a140}, 1);
        mix = op(1, OperationKind::kXor, "mix1", {mix, b65}, 1);
        mix = op(1, OperationKind::kXor, "mixed", {mix, control}, 1);
        graph.bindOutputPort("mixed", mix);
        auto wholeOut = op(2, OperationKind::kAssign, "whole_out", {whole}, 128);
        graph.bindOutputPort("whole_out", wholeOut);
        graph.bindOutputPort("whole_bit", slice(2, "whole_bit", whole, 5));
        graph.bindOutputPort("dynamic_out", op(3, OperationKind::kSliceDynamic, "dynamic_out", {dyn, index}, 1));

        const auto reg = graph.createOperation(OperationKind::kRegister, graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{1}); graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("1'b0"));
        const auto q = op(4, OperationKind::kRegisterReadPort, "q_value", {}, 1);
        graph.setAttr(graph.valueDef(q), "regSymbol", std::string("q"));
        graph.bindOutputPort("state_out", op(4, OperationKind::kXor, "state_out", {q, slice(4, "a64", a, 64)}, 1));
        const auto write = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol("write_q"));
        for (auto v : {one, data, one, clock}) graph.addOperand(write, v);
        graph.setAttr(write, "regSymbol", std::string("q"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"}); nodes[11].push_back(write);
        graph.bindOutputPort("pad_seen", slice(5, "pad_seen", pad, 129));
        graph.bindOutputPort("tail_out", op(6, OperationKind::kXor, "tail_out",
            {slice(6, "tail63", tail, 63), slice(6, "tail64", tail, 64)}, 1));
        graph.bindOutputPort("fixed_out", op(7, OperationKind::kSliceDynamic, "fixed_out", {fixed, c65}, 1));
        const auto intentValue = op(8, OperationKind::kSliceDynamic, "intent_out", {intent, c16}, 1);
        const auto intentOp = graph.valueDef(intentValue);
        graph.setAttr(intentOp, "regToMem.intent.version", int64_t{1});
        graph.setAttr(intentOp, "regToMem.intent.group", std::string("generic_test_group"));
        graph.setAttr(intentOp, "regToMem.intent.role", std::string("slice"));
        graph.setAttr(intentOp, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(intentOp, "regToMem.intent.elementWidth", int64_t{1});
        graph.setAttr(intentOp, "regToMem.intent.elementCount", int64_t{128});
        graph.bindOutputPort("intent_out", intentValue);
        const auto effectBit = slice(9, "effect_bit", effect, 7);
        graph.bindOutputPort("effect_out", effectBit);
        const auto import = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("input_bit_effect"));
        graph.setAttr(import, "argsDirection", std::vector<std::string>{});
        graph.setAttr(import, "argsWidth", std::vector<int64_t>{});
        graph.setAttr(import, "argsName", std::vector<std::string>{});
        graph.setAttr(import, "argsSigned", std::vector<bool>{});
        graph.setAttr(import, "argsType", std::vector<std::string>{});
        graph.setAttr(import, "hasReturn", false);
        const auto call = graph.createOperation(OperationKind::kDpicCall, graph.internSymbol("effect_call"));
        graph.addOperand(call, one); graph.addOperand(call, clock);
        graph.setAttr(call, "targetImportSymbol", std::string("input_bit_effect"));
        graph.setAttr(call, "inArgName", std::vector<std::string>{});
        graph.setAttr(call, "outArgName", std::vector<std::string>{});
        graph.setAttr(call, "hasReturn", false);
        graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"}); nodes[9].push_back(call);
        graph.bindOutputPort("narrow_out", slice(10, "narrow_out", narrow, 63));
        const auto flaggedBit = slice(12, "flagged_out", flagged, 31);
        graph.setAttr(graph.valueDef(flaggedBit), "hasSideEffects", true);
        graph.bindOutputPort("flagged_out", flaggedBit);

        // A range can cross words, end on a word boundary, or include complete
        // words. Two overlapping reads in one node must union their demands.
        const auto ranges = input("ranges", 769);
        range(13, "range2", ranges, 63, 2);
        range(14, "range64", ranges, 64, 64);
        range(15, "range65", ranges, 61, 65);
        range(16, "range67", ranges, 63, 67);
        range(17, "range96", ranges, 97, 96);
        range(18, "range512", ranges, 193, 512);
        range(19, "range_overlap", ranges, 100, 67);
        graph.bindOutputPort("range_bit", slice(19, "range_bit", ranges, 768));
        range(19, "range_overlap2", ranges, 130, 64);
        const auto constantRange = input("constant_range", 193);
        const auto constantRangeOut = op(20, OperationKind::kSliceDynamic,
            "constant_range_out", {constantRange, c65}, 65);
        graph.setAttr(graph.valueDef(constantRangeOut), "sliceWidth", int64_t{65});
        graph.bindOutputPort("constant_range_out", constantRangeOut);
        // Reading the entire logical width is not a narrowing opportunity.
        // An unproven/out-of-bounds range keeps the whole-input predicate.
        const auto fullRange = input("full_range", 130);
        range(21, "full_range_out", fullRange, 0, 130);
        const auto outOfBounds = input("out_of_bounds", 193);
        range(22, "out_of_bounds_out", outOfBounds, 180, 64);
        range(23, "pad_range", pad, 61, 65);

        ActivityScheduleValueFanout fanout(graph.values().size());
        std::map<uint32_t, uint32_t> nodeByOp;
        for (uint32_t n = 0; n < nodes.size(); ++n) for (auto id : nodes[n]) nodeByOp[id.index] = n;
        for (uint32_t n = 0; n < nodes.size(); ++n) for (auto id : nodes[n]) {
            const Operation consumer = graph.getOperation(id);
            for (auto v : consumer.operands()) {
                auto def = graph.valueDef(v);
                if (!def.valid() || !nodeByOp.contains(def.index) || nodeByOp.at(def.index) != n) {
                    auto &dst = fanout[v.index - 1u];
                    if (std::find(dst.begin(), dst.end(), n) == dst.end()) dst.push_back(n);
                }
            }
        }
        // A conservative frontier entry without a direct input read must remain.
        fanout[whole.index - 1u].push_back(10u);
        ActivityScheduleTopoOrder order(nodes.size()); std::iota(order.begin(), order.end(), 0u);
        SessionStore session;
        slot(session, "supernode_to_ops", nodes); slot(session, "value_fanout", fanout);
        slot(session, "topo_order", order);
        slot(session, "state_read_supernodes", ActivityScheduleStateReadSupernodes{{"q", {4u}}});
        const auto dir = root / mode; std::filesystem::create_directories(dir);
        EmitOptions options; options.outputDir = dir.string(); options.session = &session; options.sessionPathPrefix = "top";
        const bool enabled = mode != "default" && mode != "off" && mode != "precedence";
        if (mode != "default" && mode != "environment") options.attributes["input_bit_activation"] = enabled ? "1" : "0";
        if (mode == "environment" || mode == "precedence") ::setenv(key, "1", 1);
        if (mode == "bitmap") options.attributes["batch_trigger_bitmap_validate"] = "1";
        if (mode == "split") options.attributes["sched_word_helper_max_estimated_lines"] = "1";
        if (mode == "guard") options.attributes["compute_batch_dispatch_guard"] = "1";
        if (mode == "perf") options.attributes["perf"] = "eval";
        if (mode == "fullpass") options.attributes["input_fullpass_specialization"] = "1";
        if (mode == "posedge") options.attributes["posedge_fullpass_specialization"] = "1";
        EmitDiagnostics diagnostics; EmitGrhSimCpp emitter(&diagnostics);
        const auto emitted = emitter.emit(design, options); ::unsetenv(key);
        if (!emitted.success || diagnostics.hasError()) return fail("emission failed: " + mode);
        std::map<std::string, std::string> files;
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("grhsim_top") && (name.ends_with(".cpp") || name.ends_with(".hpp"))) files[name] = read(entry.path());
        }
        if (mode == "default") defaultFiles = files;
        if ((mode == "off" || mode == "precedence") && files != defaultFiles) return fail("disabled output drift: " + mode);
        const auto &eval = files.at("grhsim_top_eval.cpp");
        const bool narrowed = enabled && mode != "fullpass" && mode != "posedge";
        for (const auto &field : {"a", "b", "pad.in", "tail", "fixed_bus", "ranges", "constant_range"}) {
            const bool found = eval.find(" = " + std::string(field) + "[") != std::string::npos;
            if (found != narrowed) return fail("missing/extra mask for " + mode + "/" + field);
        }
        for (const auto &field : {"whole", "dynamic_bus", "intent_bus", "effect_bus", "narrow", "flagged_bus",
                                 "full_range", "out_of_bounds"})
            if (eval.find(" = " + std::string(field) + "[") != std::string::npos) return fail("unsafe mask: " + mode + "/" + field);
        if (mode == "off" || mode == "environment" || mode == "precedence") continue;
        const auto harness = dir / "harness.cpp";
        std::ofstream out(harness);
        out << R"cpp(#include "grhsim_top.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>
static std::uint64_t effects=0;
extern "C" void input_bit_effect(){++effects;}
int main(){GrhSIM_top sim;std::uint64_t rng=17,checksum=0;
auto random=[&](){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
auto bit=[](const auto& a,unsigned i){return bool((a[i/64]>>(i%64))&1);};
auto check=[](const auto& source,const auto& result,unsigned start,unsigned width){
for(unsigned i=0;i<width;++i){bool actual;
if constexpr(std::is_integral_v<std::remove_cvref_t<decltype(result)>>)actual=(result>>i)&1;
else actual=(result[i/64]>>(i%64))&1;
unsigned k=start+i;bool expected=k<source.size()*64&&((source[k/64]>>(k%64))&1);
if(actual!=expected)return false;}return true;};
for(unsigned init=0;init<2;++init){sim.init();sim.clock=false;sim.data=false;sim.eval();bool q=false;
for(unsigned step=0;step<4096;++step){
unsigned changed=step%769;sim.ranges[changed/64]^=UINT64_C(1)<<(changed%64);
changed=random()%193;sim.constant_range[changed/64]^=UINT64_C(1)<<(changed%64);
if(step%8!=7){unsigned n=random()%193;sim.a[n/64]^=UINT64_C(1)<<(n%64);
n=random()%129;sim.b[n/64]^=UINT64_C(1)<<(n%64);}
if(step%4==0){sim.control=!sim.control;sim.data=!sim.data;}
if(step%5==0){for(auto& v:sim.whole)v=random();for(auto& v:sim.dynamic_bus)v=random();sim.dynamic_bus[3]&=1;
for(auto& v:sim.fixed_bus)v=random();for(auto& v:sim.intent_bus)v=random();for(auto& v:sim.effect_bus)v=random();
for(auto& v:sim.flagged_bus)v=random();
for(auto& v:sim.full_range)v=random();sim.full_range[2]&=3;
for(auto& v:sim.out_of_bounds)v=random();sim.out_of_bounds[3]&=1;
for(auto& v:sim.pad.in)v=random();sim.pad.in[2]&=3;sim.tail[0]=random();sim.tail[1]=random();sim.narrow=random();}
sim.index=random()%193;
for(unsigned phase=0;phase<2;++phase){sim.clock=phase;if(phase)q=sim.data;
for(unsigned repeat=0;repeat<2;++repeat){sim.eval();
if(sim.mixed!=(bit(sim.a,3)^bit(sim.a,140)^bit(sim.b,65)^sim.control))return 1;
if(sim.state_out!=(q^bit(sim.a,64)))return 2;
if(sim.whole_out!=sim.whole||sim.whole_bit!=bit(sim.whole,5))return 3;
if(sim.dynamic_out!=bit(sim.dynamic_bus,sim.index)||sim.fixed_out!=bit(sim.fixed_bus,65))return 4;
if(sim.pad_seen!=bit(sim.pad.in,129)||sim.tail_out!=(bit(sim.tail,63)^bit(sim.tail,64)))return 5;
if(sim.intent_out!=bit(sim.intent_bus,16)||sim.effect_out!=bit(sim.effect_bus,7))return 6;
if(sim.narrow_out!=bool(sim.narrow>>63))return 7;
if(sim.flagged_out!=bit(sim.flagged_bus,31))return 8;
if(!check(sim.ranges,sim.range2,63,2)||!check(sim.ranges,sim.range64,64,64))return 9;
if(!check(sim.ranges,sim.range65,61,65)||!check(sim.ranges,sim.range67,63,67))return 10;
if(!check(sim.ranges,sim.range96,97,96)||!check(sim.ranges,sim.range512,193,512))return 11;
if(!check(sim.ranges,sim.range_overlap,100,67)||!check(sim.ranges,sim.range_overlap2,130,64))return 12;
if(sim.range_bit!=bit(sim.ranges,768)||!check(sim.constant_range,sim.constant_range_out,65,65))return 13;
if(sim.full_range_out!=sim.full_range||!check(sim.out_of_bounds,sim.out_of_bounds_out,180,64))return 14;
if(!check(sim.pad.in,sim.pad_range,61,65))return 15;
checksum=checksum*33+sim.mixed+2*sim.state_out+4*sim.tail_out+effects;
}}}}
std::printf("CHECKSUM %llu %llu\n",(unsigned long long)checksum,(unsigned long long)effects);}
)cpp";
        out.close();
        for (const std::string optimization : {"-O0", "-O3"}) {
            const auto binary = dir / ("harness" + optimization.substr(1));
            const auto log = binary.string() + ".log";
            std::string command = "clang++ -std=c++20 " + optimization;
            if (mode == "on" && optimization == "-O3") command += " -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer";
            command += " -I" + dir.string();
            for (const auto &[name, content] : files) if (name.ends_with(".cpp")) command += " " + (dir / name).string();
            command += " " + harness.string() + " -o " + binary.string() + " >" + log + " 2>&1";
            if (std::system(command.c_str())) return fail("compile failed: " + log);
            if (std::system((binary.string() + " >" + log + " 2>&1").c_str())) return fail("runtime failed: " + log);
            const auto output = read(log); const auto start = output.find("CHECKSUM ");
            if (start == std::string::npos) return fail("missing checksum: " + log);
            const auto trace = output.substr(start, output.find('\n', start) - start);
            if (expectedTrace.empty()) expectedTrace = trace;
            if (trace != expectedTrace) return fail("observable trace mismatch: " + mode + "/" + optimization);
        }
    }
    std::cout << "PASS input-bit activation: 11 modes, 16 runtime builds, 32768 evals/build; " << expectedTrace << '\n';
    return 0;
}
}

int main() { return run(); }
