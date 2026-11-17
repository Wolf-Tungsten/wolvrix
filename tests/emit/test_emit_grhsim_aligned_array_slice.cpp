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
#include <stdexcept>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;
using namespace wolvrix::lib::emit;

namespace {
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
std::string quote(const std::string &text) {
    std::string result = "'";
    for (char ch : text) result += ch == '\'' ? "'\\''" : std::string(1, ch);
    return result + "'";
}
using Files = std::map<std::string, std::string>;
struct Fixture {
    Design design;
    Graph &graph;
    SessionStore session;
    ActivityScheduleSupernodeToOps nodes{2};
    Fixture() : graph(design.createGraph("top")) { design.markAsTop("top"); }
    ValueId value(const std::string &name, int width, bool sign = false) {
        return graph.createValue(graph.internSymbol(name), width, sign, ValueType::Logic);
    }
    ValueId input(const std::string &name, int width) {
        const auto v = value(name, width); graph.bindInputPort(name, v); return v;
    }
    ValueId op(OperationKind kind, const std::string &name, int width,
               const std::vector<ValueId> &args, unsigned group = 0, bool sign = false) {
        const auto id = graph.createOperation(kind, graph.internSymbol(name + "_op"));
        for (auto arg : args) graph.addOperand(id, arg);
        const auto v = value(name, width, sign); graph.addResult(id, v); nodes[group].push_back(id); return v;
    }
    template<class T> void slot(const std::string &name, T data) {
        session.insert_or_assign("top.activity_schedule." + name,
            std::make_unique<SessionSlotValue<T>>(std::move(data), "aligned-array-slice-test"));
    }
    void build() {
        const auto bits = input("bits", 131), small = input("small", 64);
        const auto idx = input("index", 5), large = input("large_index", 64);
        // Model the mixed 16 x 4 + 64-bit concat left by adjacent-slice merging.
        std::vector<ValueId> pieces;
        for (int high = 127; high >= 64; high -= 4) {
            const auto part = op(OperationKind::kSliceStatic, "part" + std::to_string(high), 4, {bits});
            graph.setAttr(graph.valueDef(part), "sliceStart", int64_t{high - 3});
            graph.setAttr(graph.valueDef(part), "sliceEnd", int64_t{high}); pieces.push_back(part);
        }
        const auto low = op(OperationKind::kSliceStatic, "low", 64, {bits});
        graph.setAttr(graph.valueDef(low), "sliceStart", int64_t{0});
        graph.setAttr(graph.valueDef(low), "sliceEnd", int64_t{63}); pieces.push_back(low);
        const auto packed = op(OperationKind::kConcat, "mixed", 128, pieces);
        const auto concat = graph.valueDef(packed);
        graph.setAttr(concat, "svPackedArray.version", int64_t{1});
        graph.setAttr(concat, "svPackedArray.elementWidth", int64_t{4});
        graph.setAttr(concat, "svPackedArray.elementCount", int64_t{32});
        graph.setAttr(concat, "svPackedArray.indexLow", int64_t{0});
        graph.setAttr(concat, "svPackedArray.indexHigh", int64_t{31});
        graph.setAttr(concat, "svPackedArray.laneOrder", std::string("lsb_index_low"));
        graph.setAttr(concat, "svPackedArray.concat.operand0Index", int64_t{31});
        graph.setAttr(concat, "svPackedArray.concat.operandStride", int64_t{-1});
        for (int width : {1, 2, 4, 8, 16, 32, 64, 3, 17, 33, 128}) {
            for (unsigned kind = 0; kind < 2; ++kind) {
                const auto name = std::string(kind ? "large" : "y") + std::to_string(width);
                const auto v = op(OperationKind::kSliceArray, name, width, {bits, kind ? large : idx}, 1, true);
                graph.setAttr(graph.valueDef(v), "sliceWidth", int64_t{width}); graph.bindOutputPort(name, v);
            }
        }
        for (const auto &[name, source] : std::vector<std::pair<std::string, ValueId>>{{"mixed_y", packed}, {"small_y", small}}) {
            const auto v = op(OperationKind::kSliceArray, name, 4, {source, idx}, 1);
            graph.setAttr(graph.valueDef(v), "sliceWidth", int64_t{4}); graph.bindOutputPort(name, v);
        }
        const auto dynamic = op(OperationKind::kSliceDynamic, "dynamic_y", 4, {bits, idx}, 1);
        graph.setAttr(graph.valueDef(dynamic), "sliceWidth", int64_t{4}); graph.bindOutputPort("dynamic_y", dynamic);
        ActivityScheduleValueFanout fanout(graph.values().size());
        for (unsigned group = 0; group < nodes.size(); ++group) for (auto id : nodes[group]) {
            const auto operation = graph.getOperation(id);
            for (auto arg : operation.operands()) {
                auto &targets = fanout[arg.index - 1];
                const auto def = graph.valueDef(arg);
                if (std::find(nodes[group].begin(), nodes[group].end(), def) == nodes[group].end() &&
                    std::find(targets.begin(), targets.end(), group) == targets.end()) targets.push_back(group);
            }
        }
        slot("supernode_to_ops", nodes); slot("value_fanout", fanout);
        slot("topo_order", ActivityScheduleTopoOrder{0, 1});
        slot("state_read_supernodes", ActivityScheduleStateReadSupernodes{});
    }
};
Files emit(const std::filesystem::path &dir, const std::string &mode) {
    Fixture f; f.build();
    EmitOptions options; options.outputDir = dir; options.session = &f.session;
    options.sessionPathPrefix = "top"; options.topOverrides = {"top"}; options.attributes["waveform"] = "off";
    if (mode != "default" && mode != "env") options.attributes["aligned_array_slice"] = mode == "on" ? "1" : "0";
    EmitDiagnostics diag; EmitGrhSimCpp emitter(&diag);
    const auto result = emitter.emit(f.design, options);
    if (!result.success) for (const auto &message : diag.messages()) std::cerr << message.message << '\n';
    require(result.success, "emission failed");
    Files files;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp")
            files[entry.path().filename().string()] = read(entry.path());
    return files;
}
void run(const std::filesystem::path &dir, const Files &files) {
    std::ofstream out(dir / "oracle.cpp");
    out << R"CPP(#include "grhsim_top.hpp"
#include <array>
#include <cstdint>
#include <limits>
uint64_t extract(const std::array<uint64_t,3> &bits,uint64_t start,unsigned width) {
    uint64_t result=0;
    for(unsigned bit=0;bit<width && bit<64;++bit)
        if(start<=191 && bit<=191-start) result|=((bits[(start+bit)/64]>>((start+bit)%64))&1)<<bit;
    return result;
}
int main(){GrhSIM_top m;uint64_t rng=11;
for(unsigned init=0;init<2;++init){m.init();
for(unsigned iteration=0;iteration<420;++iteration){
    for(auto &word:m.bits){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;word=rng;}
    m.bits[2]&=7;m.small=rng;
    m.index=iteration%256; // Include spare C++ storage bits beyond the 5-bit port.
    const uint64_t indices[]={0,1,2,31,32,63,64,127,128,255,UINT64_MAX,UINT64_C(1)<<63};
    m.large_index=indices[iteration%12];
    for(unsigned repeat=0;repeat<3;++repeat){m.eval();
)CPP";
    for (int width : {1, 2, 4, 8, 16, 32, 64, 3, 17, 33, 128}) {
        for (unsigned kind = 0; kind < 2; ++kind) {
            const auto name = std::string(kind ? "large" : "y") + std::to_string(width);
            out << "{const uint64_t start=uint64_t(m." << (kind ? "large_index" : "index") << ")*" << width << "u;\n";
            if (width <= 64) out << "if(uint64_t(m." << name << ")!=extract(m.bits,start," << width << "))return 1;\n";
            else out << "if(m." << name << "[0]!=extract(m.bits,start,64)||m." << name
                     << "[1]!=extract(m.bits,start>UINT64_MAX-64?UINT64_MAX:start+64,64))return 2;\n";
            out << "}\n";
        }
    }
    out << R"CPP(
    if(m.mixed_y!=(m.index<32?extract(m.bits,uint64_t(m.index)*4,4):0))return 3;
    if(m.small_y!=(m.index<16?((m.small>>(m.index*4))&15):0))return 4;
    if(m.dynamic_y!=extract(m.bits,m.index,4))return 5;
    m.index=(m.index+37)&255;
    }}}
return 0;}
)CPP";
    out.close();
    const char *cxx = std::getenv("CXX");
    for (const char *flags : {"-O0", "-O3 -fsanitize=address,undefined -fno-sanitize-recover=all"}) {
        std::string command = quote(cxx ? cxx : WOLF_TEST_CXX_COMPILER) + " -std=c++20 " + flags + " -I" + quote(dir.string());
        for (const auto &[name, text] : files) if (name.ends_with(".cpp")) command += " " + quote((dir / name).string());
        command += " " + quote((dir / "oracle.cpp").string()) + " -o " + quote((dir / "check").string());
        require(std::system(command.c_str()) == 0, "oracle compilation failed");
        require(std::system(quote((dir / "check").string()).c_str()) == 0, "bit extraction oracle failed");
    }
}
}
int main() {
    try {
        const auto base = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "aligned-array-slice";
        std::filesystem::remove_all(base);
        constexpr const char *env = "WOLVRIX_GRHSIM_ALIGNED_ARRAY_SLICE";
        ::unsetenv(env); const auto defaults = emit(base / "default", "default");
        const auto off = emit(base / "off", "off"); require(off == defaults, "default must remain off");
        const auto on = emit(base / "on", "on");
        ::setenv(env, "1", 1);
        require(emit(base / "env", "env") == on, "environment option mismatch");
        require(emit(base / "precedence", "off") == off, "attribute must override environment");
        ::unsetenv(env);
        std::string code;
        for (const auto &[name, text] : on) if (name.ends_with(".cpp")) code += text;
        require(code.find("grhsim_aligned_array_slice_words<4u>") != std::string::npos, "missing aligned lowering");
        for (int width : {3, 17, 33, 128}) require(code.find("grhsim_aligned_array_slice_words<" + std::to_string(width) + "u>") == std::string::npos, "invalid width admitted");
        for (const auto &[name, text] : off) if (name.ends_with(".hpp") && name.find("runtime") == std::string::npos)
            require(on.at(name) == text, "model storage layout changed");
        run(base / "off", off); run(base / "on", on);
        std::cout << "aligned array read widths, mixed lanes, fallback, index and O0/O3 sanitizer oracles passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
