#include "core/grh.hpp"
#include "core/store.hpp"
#include "emit/grhsim_repcut_package.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

using namespace wolvrix::lib::emit;
using namespace wolvrix::lib::grh;

namespace
{
    int fail(const std::string &message)
    {
        std::cerr << "[emit_grhsim_repcut_package] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool containsFiles(const std::filesystem::path &directory)
    {
        if (!std::filesystem::exists(directory)) return false;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
        {
            if (entry.is_regular_file()) return true;
        }
        return false;
    }

    std::string shellQuote(std::string_view text)
    {
        std::string quoted = "'";
        for (const char ch : text)
        {
            quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
        }
        return quoted + "'";
    }

    std::string diagnosticsSummary(const EmitDiagnostics &diagnostics)
    {
        std::string text;
        for (const auto &message : diagnostics.messages())
        {
            if (!text.empty()) text += " | ";
            text += message.message;
            if (!message.context.empty()) text += " [" + message.context + "]";
        }
        return text;
    }

    ValueId value(Graph &graph, const std::string &name, int32_t width)
    {
        return graph.createValue(graph.internSymbol(name), width, false);
    }

    ValueId input(Graph &graph, const std::string &name, int32_t width)
    {
        const auto id = value(graph, name, width);
        graph.bindInputPort(name, id);
        return id;
    }

    ValueId constant(Graph &graph, const std::string &name, int32_t width, const std::string &literal)
    {
        const auto result = value(graph, name, width);
        const auto op = graph.createOperation(OperationKind::kConstant, graph.internSymbol(name + "_op"));
        graph.setAttr(op, "constValue", literal);
        graph.addResult(op, result);
        return result;
    }

    ValueId binary(Graph &graph, const std::string &name, int32_t width,
                   OperationKind kind, ValueId lhs, ValueId rhs)
    {
        const auto result = value(graph, name, width);
        const auto op = graph.createOperation(kind, graph.internSymbol(name + "_op"));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, result);
        return result;
    }

    ValueId reg(Graph &graph, const std::string &name, int32_t width,
                ValueId clock, ValueId reset, ValueId enable, ValueId data)
    {
        const auto storage = graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
        graph.setAttr(storage, "width", static_cast<int64_t>(width));
        graph.setAttr(storage, "isSigned", false);
        graph.setAttr(storage, "initValue", std::to_string(width) + "'h0");
        const auto result = value(graph, name + "_value", width);
        const auto read = graph.createOperation(OperationKind::kRegisterReadPort,
                                                graph.internSymbol(name + "_read"));
        graph.setAttr(read, "regSymbol", name);
        graph.addResult(read, result);
        const auto writeEnable = binary(graph, name + "_write_enable", 1,
                                         OperationKind::kOr, reset, enable);
        const auto zero = constant(graph, name + "_zero", width, std::to_string(width) + "'h0");
        const auto next = value(graph, name + "_next", width);
        const auto select = graph.createOperation(OperationKind::kMux,
                                                   graph.internSymbol(name + "_reset_mux"));
        graph.addOperand(select, reset);
        graph.addOperand(select, zero);
        graph.addOperand(select, data);
        graph.addResult(select, next);
        const auto mask = constant(graph, name + "_mask", width,
                                   std::to_string(width) + "'b" + std::string(width, '1'));
        const auto write = graph.createOperation(OperationKind::kRegisterWritePort,
                                                 graph.internSymbol(name + "_write"));
        graph.setAttr(write, "regSymbol", name);
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(write, writeEnable);
        graph.addOperand(write, next);
        graph.addOperand(write, mask);
        graph.addOperand(write, clock);
        return result;
    }

    void instance(Graph &top, const std::string &name, const std::string &module,
                  const std::vector<ValueId> &inputs, const std::vector<ValueId> &outputs,
                  std::vector<std::string> inputNames, std::vector<std::string> outputNames)
    {
        const auto op = top.createOperation(OperationKind::kInstance, top.internSymbol(name));
        top.setAttr(op, "instanceName", name);
        top.setAttr(op, "moduleName", module);
        top.setAttr(op, "inputPortName", std::move(inputNames));
        top.setAttr(op, "outputPortName", std::move(outputNames));
        top.setAttr(op, "inoutPortName", std::vector<std::string>{});
        for (const auto id : inputs) top.addOperand(op, id);
        for (const auto id : outputs) top.addResult(op, id);
    }

    Design buildDesign()
    {
        Design design;
        Graph &effect = design.createGraph("EarlyDpi");
        const auto effectClock = input(effect, "clock", 1);
        const auto effectInput = input(effect, "in8", 8);
        const auto effectCondition = constant(effect, "condition", 1, "1'b1");
        const auto effectResult = value(effect, "result", 8);
        const auto dpiImport = effect.createOperation(OperationKind::kDpicImport,
                                                       effect.internSymbol("repcut_test_dpi"));
        effect.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input", "output"});
        effect.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{8, 8});
        effect.setAttr(dpiImport, "argsName", std::vector<std::string>{"in_value", "out_value"});
        effect.setAttr(dpiImport, "argsSigned", std::vector<bool>{false, false});
        effect.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic", "logic"});
        effect.setAttr(dpiImport, "hasReturn", false);
        const auto dpiCall = effect.createOperation(OperationKind::kDpicCall,
                                                     effect.internSymbol("dpi_call"));
        effect.setAttr(dpiCall, "targetImportSymbol", std::string("repcut_test_dpi"));
        effect.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
        effect.setAttr(dpiCall, "inArgName", std::vector<std::string>{"in_value"});
        effect.setAttr(dpiCall, "outArgName", std::vector<std::string>{"out_value"});
        effect.setAttr(dpiCall, "hasReturn", false);
        effect.addOperand(dpiCall, effectCondition);
        effect.addOperand(dpiCall, effectInput);
        effect.addOperand(dpiCall, effectClock);
        effect.addResult(dpiCall, effectResult);
        effect.bindOutputPort("result", effectResult);

        // Standard-stream descriptors are process-wide, unlike fopen handles.
        for (unsigned stream = 0; stream < 3; ++stream)
        {
            const auto suffix = std::to_string(stream);
            const auto text = effect.createValue(effect.internSymbol("stream_text_" + suffix),
                                                  0, false, ValueType::String);
            const auto textOp = effect.createOperation(OperationKind::kConstant,
                                                        effect.internSymbol("stream_text_op_" + suffix));
            effect.setAttr(textOp, "constValue", "\"repcut-standard-stream-" + suffix + "\"");
            effect.addResult(textOp, text);
            const auto print = effect.createOperation(OperationKind::kSystemTask,
                                                       effect.internSymbol("stream_print_" + suffix));
            effect.setAttr(print, "name", std::string("fdisplay"));
            effect.setAttr(print, "procKind", std::string("initial"));
            effect.setAttr(print, "hasTiming", false);
            effect.setAttr(print, "hasSideEffects", true);
            effect.addOperand(print, effectCondition);
            const std::string descriptors[] = {"32'd1", "32'h80000001", "32'h80000002"};
            effect.addOperand(print, constant(effect, "stream_descriptor_" + suffix, 32, descriptors[stream]));
            effect.addOperand(print, text);
        }

        // Two instances of the same graph must own independent model state.
        Graph &bank = design.createGraph("RegisterBank");
        const auto bankClock = input(bank, "clock", 1);
        const auto reset = input(bank, "reset", 1);
        const auto enable = input(bank, "enable", 1);
        const auto data8 = input(bank, "data8", 8);
        const auto data65 = input(bank, "data65", 65);
        const auto data130 = input(bank, "data130", 130);
        const auto data1025 = input(bank, "data1025", 1025);
        const auto mask65 = input(bank, "mask65", 65);
        const auto mask130 = input(bank, "mask130", 130);
        const auto next65 = binary(bank, "next65", 65, OperationKind::kXor, data65, mask65);
        const auto next130 = binary(bank, "next130", 130, OperationKind::kXor, data130, mask130);
        bank.bindOutputPort("q8", reg(bank, "reg8", 8, bankClock, reset, enable, data8));
        bank.bindOutputPort("q65", reg(bank, "reg65", 65, bankClock, reset, enable, next65));
        bank.bindOutputPort("q130", reg(bank, "reg130", 130, bankClock, reset, enable, next130));
        bank.bindOutputPort("q1025", reg(bank, "reg1025", 1025, bankClock, reset, enable, data1025));
        for (const int32_t width : {1, 16, 32, 64})
        {
            const auto suffix = std::to_string(width);
            const auto data = input(bank, "data" + suffix, width);
            bank.bindOutputPort("q" + suffix, reg(bank, "reg" + suffix, width,
                                                 bankClock, reset, enable, data));
        }

        Graph &top = design.createGraph("RepCutTop");
        const auto clock = input(top, "clock", 1);
        const auto topReset = input(top, "reset", 1);
        const auto topEnable = input(top, "enable", 1);
        const auto in8 = input(top, "in8", 8);
        const auto in65 = input(top, "in65", 65);
        const auto in130 = input(top, "in130", 130);
        const auto in1025 = input(top, "in1025", 1025);
        const auto dpiResult = value(top, "dpi_result", 8);
        const auto first8 = value(top, "first8", 8);
        const auto first65 = value(top, "first65", 65);
        const auto first130 = value(top, "first130", 130);
        const auto first1025 = value(top, "first1025", 1025);
        const auto second8 = value(top, "second8", 8);
        const auto second65 = value(top, "second65", 65);
        const auto second130 = value(top, "second130", 130);
        const auto second1025 = value(top, "second1025", 1025);
        const auto mask65A = constant(top, "mask65_a", 65, "65'h10123456789abcdef");
        const auto mask65B = constant(top, "mask65_b", 65, "65'h0fedcba9876543210");
        const auto mask130A = constant(top, "mask130_a", 130,
                                      "130'h2" "0123456789abcdef" "fedcba9876543210");
        const auto mask130B = constant(top, "mask130_b", 130,
                                      "130'h1" "89abcdef01234567" "76543210fedcba98");
        top.bindOutputPort("dpi_result", dpiResult);
        top.bindOutputPort("first8", first8);
        top.bindOutputPort("first65", first65);
        top.bindOutputPort("first130", first130);
        top.bindOutputPort("first1025", first1025);
        top.bindOutputPort("second8", second8);
        top.bindOutputPort("second65", second65);
        top.bindOutputPort("second130", second130);
        top.bindOutputPort("second1025", second1025);
        instance(top, "effect", "EarlyDpi", {clock, in8}, {dpiResult},
                 {"clock", "in8"}, {"result"});
        std::vector<std::string> bankInputs = {
            "clock", "reset", "enable", "data8", "data65", "data130", "mask65", "mask130", "data1025"};
        std::vector<std::string> bankOutputs = {"q8", "q65", "q130", "q1025"};
        std::vector<ValueId> firstInputs = {
            clock, topReset, topEnable, dpiResult, in65, in130, mask65A, mask130A, in1025};
        std::vector<ValueId> secondInputs = {
            clock, topReset, topEnable, first8, first65, first130, mask65B, mask130B, first1025};
        std::vector<ValueId> firstOutputs = {first8, first65, first130, first1025};
        std::vector<ValueId> secondOutputs = {second8, second65, second130, second1025};
        for (const int32_t width : {1, 16, 32, 64})
        {
            const auto suffix = std::to_string(width);
            const auto first = value(top, "first" + suffix, width);
            const auto second = value(top, "second" + suffix, width);
            top.bindOutputPort("first" + suffix, first);
            top.bindOutputPort("second" + suffix, second);
            firstInputs.push_back(input(top, "in" + suffix, width));
            secondInputs.push_back(first);
            firstOutputs.push_back(first);
            secondOutputs.push_back(second);
            bankInputs.push_back("data" + suffix);
            bankOutputs.push_back("q" + suffix);
        }
        instance(top, "part_0", "RegisterBank",
                 firstInputs, firstOutputs, bankInputs, bankOutputs);
        instance(top, "part_1", "RegisterBank",
                 secondInputs, secondOutputs, bankInputs, bankOutputs);
        design.markAsTop(top.symbol());
        return design;
    }

    Design buildNameCollisionDesign(const std::string &topName,
                                    const std::vector<std::string> &instanceNames)
    {
        Design design;
        // This is a real, reachable input graph, not an unused reservation:
        // old private names derived from instance part_0 collide with it.
        Graph &bank = design.createGraph("Gpart_0_model");
        const auto clock = input(bank, "clock", 1);
        const auto reset = input(bank, "reset", 1);
        const auto enable = input(bank, "enable", 1);
        const auto data = input(bank, "data", 8);
        bank.bindOutputPort("q", reg(bank, "state", 8, clock, reset, enable, data));

        Graph &top = design.createGraph(topName);
        const auto topClock = input(top, "clock", 1);
        const auto topReset = input(top, "reset", 1);
        const auto topEnable = input(top, "enable", 1);
        const auto topData = input(top, "data", 8);
        for (std::size_t index = 0; index < instanceNames.size(); ++index)
        {
            const std::string outputName = "out" + std::to_string(index);
            const auto output = value(top, outputName, 8);
            top.bindOutputPort(outputName, output);
            instance(top, instanceNames[index], bank.symbol(),
                     {topClock, topReset, topEnable, topData}, {output},
                     {"clock", "reset", "enable", "data"}, {"q"});
        }
        design.markAsTop(top.symbol());
        return design;
    }

    int runNameCollisionCase(const std::filesystem::path &artifactRoot,
                             const std::string &compiler,
                             const std::string &topName,
                             const std::vector<std::string> &instanceNames)
    {
        const auto caseRoot = artifactRoot / ("names_" + topName);
        const auto packageDir = caseRoot / "package";
        std::filesystem::create_directories(packageDir);
        const Design design = buildNameCollisionDesign(topName, instanceNames);
        wolvrix::lib::store::StoreJson store;
        const auto original = store.storeToString(design);
        EmitOptions options;
        options.outputDir = packageDir.string();
        options.topOverrides = {topName};
        EmitDiagnostics diagnostics;
        EmitGrhSimRepCutPackage emitter(&diagnostics);
        if (!emitter.emit(design, options).success || diagnostics.hasError())
        {
            return fail("name-collision package " + topName + " failed: " + diagnosticsSummary(diagnostics));
        }
        if (!original || store.storeToString(design) != original)
        {
            return fail("name-collision emission changed the original graph " + topName);
        }
        const auto harnessPath = caseRoot / "harness.cpp";
        {
            std::ofstream harness(harnessPath);
            harness << "#include \"grhsim_" << topName << ".hpp\"\n"
                    << "#include <cstdint>\n#include <cstdio>\n\nint main() {\n"
                    << "  GrhSIM_" << topName << " sim;\n"
                    << "  sim.init();\n  sim.clock = false;\n  sim.reset = false;\n"
                    << "  sim.enable = true;\n  sim.data = 0;\n  sim.eval();\n"
                    << "  auto check = [&](std::uint8_t expected) { return ";
            for (std::size_t index = 0; index < instanceNames.size(); ++index)
            {
                if (index != 0) harness << " && ";
                harness << "sim.out" << index << " == expected";
            }
            harness << "; };\n  if (!check(0)) return 1;\n"
                    << "  for (unsigned cycle = 0; cycle < 5; ++cycle) {\n"
                    << "    sim.clock = false;\n    sim.data = 17 + cycle * 31;\n    sim.eval();\n"
                    << "    sim.clock = true;\n    sim.eval();\n"
                    << "    if (!check(sim.data)) return 2;\n  }\n"
                    << "  sim.clock = false;\n  sim.reset = true;\n  sim.enable = false;\n  sim.eval();\n"
                    << "  sim.clock = true;\n  sim.eval();\n  if (!check(0)) return 3;\n"
                    << "  std::puts(\"name-collision-harness-ok\");\n  return 0;\n}\n";
            if (!harness) return fail("cannot write name-collision behavior harness");
        }
        const auto buildLog = caseRoot / "build.log";
        const auto compile =
            "timeout 180s make -C " + shellQuote(packageDir.string()) + " -j2 CXX=" + shellQuote(compiler) +
            " CXXFLAGS='-std=c++20 -O0' > " + shellQuote(buildLog.string()) + " 2>&1";
        if (std::system(compile.c_str()) != 0)
        {
            return fail("name-collision library " + topName + " failed to compile:\n" + readFile(buildLog));
        }
        const auto executable = caseRoot / "harness";
        const auto link = "timeout 60s " + shellQuote(compiler) + " -std=c++20 -O0 -pthread -I" +
            shellQuote(packageDir.string()) + " " + shellQuote(harnessPath.string()) + " " +
            shellQuote((packageDir / ("libgrhsim_" + topName + ".a")).string()) + " -o " +
            shellQuote(executable.string()) + " >> " + shellQuote(buildLog.string()) + " 2>&1";
        if (std::system(link.c_str()) != 0)
        {
            return fail("name-collision harness " + topName + " failed to link:\n" + readFile(buildLog));
        }
        const auto runLog = caseRoot / "run.log";
        const auto run =
            "timeout 30s env -u WOLVI_REPCUT_PIN_THREADS -u WOLVI_REPCUT_HOST_LANE -u WOLVI_REPCUT_HOST_SPIN_US "
            "-u WOLVI_REPCUT_WORKER_SPIN_US -u WOLVI_REPCUT_TIMING_JSONL XS_EMU_THREADS=1 " +
            shellQuote(executable.string()) + " > " + shellQuote(runLog.string()) + " 2>&1";
        if (std::system(run.c_str()) != 0 ||
            readFile(runLog).find("name-collision-harness-ok") == std::string::npos)
        {
            return fail("name-collision harness " + topName + " failed to run:\n" + readFile(runLog));
        }
        return 0;
    }

    constexpr std::string_view kHarness = R"cpp(
#include "grhsim_RepCutTop.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <stdexcept>

using Wide65 = std::array<std::uint64_t, 2>;
using Wide130 = std::array<std::uint64_t, 3>;
// Two full communication chunks plus a partial final chunk.
using Wide1025 = std::array<std::uint64_t, 17>;
static unsigned dpi_calls;
static std::atomic<bool> throw_next_dpi{false};
extern "C" void repcut_test_dpi(std::uint8_t input, std::uint8_t *output) {
    if (throw_next_dpi.exchange(false)) throw std::runtime_error("repcut-test-worker-error");
    ++dpi_calls;
    *output = static_cast<std::uint8_t>(input + 7);
}

struct Inputs {
    bool clock{};
    bool reset{};
    bool enable{};
    std::uint8_t in8{};
    Wide65 in65{};
    Wide130 in130{};
    Wide1025 in1025{};
    bool in1{};
    std::uint16_t in16{};
    std::uint32_t in32{};
    std::uint64_t in64{};
};
struct Outputs {
    std::uint8_t dpi{};
    std::uint8_t first8{};
    Wide65 first65{};
    Wide130 first130{};
    std::uint8_t second8{};
    Wide65 second65{};
    Wide130 second130{};
    Wide1025 first1025{};
    Wide1025 second1025{};
    std::array<std::uint64_t, 4> firstScalars{};
    std::array<std::uint64_t, 4> secondScalars{};
    bool operator==(const Outputs &) const = default;
};
struct Wrapper {
    WolviRepCutGrhSimSim model;
    void init() { model.init(); }
    void set_runtime_profile_enabled(bool enabled) { model.set_runtime_profile_enabled(enabled); }
    void dump_runtime_profile() const { model.dump_runtime_profile(); }
    Outputs eval(const Inputs &in) {
        model.set_clock(in.clock);
        model.set_reset(in.reset);
        model.set_enable(in.enable);
        model.set_in8(in.in8);
        model.set_in65(in.in65);
        model.set_in130(in.in130);
        model.set_in1025(in.in1025);
        model.set_in1(in.in1);
        model.set_in16(in.in16);
        model.set_in32(in.in32);
        model.set_in64(in.in64);
        model.step();
        return {model.get_dpi_result(), model.get_first8(), model.get_first65(),
                model.get_first130(), model.get_second8(), model.get_second65(),
                model.get_second130(), model.get_first1025(), model.get_second1025(),
                {model.get_first1(), model.get_first16(), model.get_first32(), model.get_first64()},
                {model.get_second1(), model.get_second16(), model.get_second32(), model.get_second64()}};
    }
};
struct Facade {
    GrhSIM_RepCutTop model;
    void init() { model.init(); }
    void set_runtime_profile_enabled(bool enabled) { model.set_runtime_profile_enabled(enabled); }
    void dump_runtime_profile() const { model.dump_runtime_profile(); }
    Outputs eval(const Inputs &in) {
        model.clock = in.clock;
        model.reset = in.reset;
        model.enable = in.enable;
        model.in8 = in.in8;
        model.in65 = in.in65;
        model.in130 = in.in130;
        model.in1025 = in.in1025;
        model.in1 = in.in1;
        model.in16 = in.in16;
        model.in32 = in.in32;
        model.in64 = in.in64;
        model.eval();
        return {model.dpi_result, model.first8, model.first65, model.first130,
                model.second8, model.second65, model.second130, model.first1025, model.second1025,
                {model.first1, model.first16, model.first32, model.first64},
                {model.second1, model.second16, model.second32, model.second64}};
    }
};
template <std::size_t N>
std::array<std::uint64_t, N> xored(std::array<std::uint64_t, N> a,
                                  const std::array<std::uint64_t, N> &b) {
    for (std::size_t i = 0; i < N; ++i) a[i] ^= b[i];
    return a;
}
template <class Simulation>
int run() {
    const Wide65 mask65a{0x0123456789abcdefULL, 1};
    const Wide65 mask65b{0xfedcba9876543210ULL, 0};
    const Wide130 mask130a{0xfedcba9876543210ULL, 0x0123456789abcdefULL, 2};
    const Wide130 mask130b{0x76543210fedcba98ULL, 0x89abcdef01234567ULL, 1};
    Simulation sim;
    sim.init();
    sim.set_runtime_profile_enabled(true);
    Inputs in;
    Outputs expected;
    unsigned expectedCalls = 0;
    unsigned sample = 0;
    auto check = [&](const char *phase) {
        const Outputs actual = sim.eval(in);
        if (!(actual == expected) || dpi_calls != expectedCalls) {
            std::fprintf(stderr, "mismatch at %s sample=%u calls=%u/%u dpi=%u/%u "
                                 "first8=%u/%u second8=%u/%u\n", phase, sample,
                         dpi_calls, expectedCalls, actual.dpi, expected.dpi,
                         actual.first8, expected.first8, actual.second8, expected.second8);
            for (std::size_t i = 0; i < actual.first130.size(); ++i) {
                std::fprintf(stderr, " word%zu first130=%llx/%llx second130=%llx/%llx\n", i,
                             (unsigned long long)actual.first130[i],
                             (unsigned long long)expected.first130[i],
                             (unsigned long long)actual.second130[i],
                             (unsigned long long)expected.second130[i]);
            }
            return false;
        }
        std::printf("sample=%u calls=%u dpi=%u first=%u/%llx/%llx/%llx/%llx/%llx "
                    "second=%u/%llx/%llx/%llx/%llx/%llx\n", sample++, dpi_calls,
                    actual.dpi, actual.first8,
                    (unsigned long long)actual.first65[0], (unsigned long long)actual.first65[1],
                    (unsigned long long)actual.first130[0], (unsigned long long)actual.first130[1],
                    (unsigned long long)actual.first130[2], actual.second8,
                    (unsigned long long)actual.second65[0], (unsigned long long)actual.second65[1],
                    (unsigned long long)actual.second130[0], (unsigned long long)actual.second130[1],
                    (unsigned long long)actual.second130[2]);
        return true;
    };
    // A high clock on the first eval establishes the edge baseline; it must
    // not be mistaken for a rising edge before a previous sample exists.
    in.clock = true;
    if (!check("initial-high")) return 1;
    for (unsigned cycle = 0; cycle < 24; ++cycle) {
        in.clock = false;
        in.reset = cycle == 3 || cycle == 17;
        in.enable = cycle % 5 != 3;
        in.in8 = static_cast<std::uint8_t>(cycle * 19 + 5);
        in.in1 = (cycle & 1) != 0;
        in.in16 = static_cast<std::uint16_t>(0xffffu - cycle * 257u);
        in.in32 = 0xffffffffu - cycle * 65537u;
        in.in64 = 0xffffffffffffffffULL - cycle * 0x100000001ULL;
        in.in65 = {0x123456789abcdef0ULL ^ (0x0102040810204081ULL * cycle), cycle & 1u};
        in.in130 = {0xfedcba9876543210ULL ^ (0x0001000100010001ULL * cycle),
                    0x0123456789abcdefULL + cycle, cycle & 3u};
        // Exercise unchanged blocks, each boundary word, and nonzero-to-zero
        // transitions without depending on an implementation-side dirty flag.
        if (cycle == 12) in.in1025 = {};
        else in.in1025[cycle % 17] ^= (0xfedcba9876543210ULL + cycle) | 1;
        in.in1025.back() &= 1;
        if (!check("low")) return 2;
        const Outputs previous = expected;
        in.clock = true;
        expected.dpi = static_cast<std::uint8_t>(in.in8 + 7);
        ++expectedCalls;
        if (in.reset) {
            const auto dpi = expected.dpi;
            expected = {};
            expected.dpi = dpi;
        } else if (in.enable) {
            expected.first8 = expected.dpi;
            expected.first65 = xored(in.in65, mask65a);
            expected.first130 = xored(in.in130, mask130a);
            expected.second8 = previous.first8;
            expected.second65 = xored(previous.first65, mask65b);
            expected.second130 = xored(previous.first130, mask130b);
            expected.first1025 = in.in1025;
            expected.second1025 = previous.first1025;
            expected.firstScalars = {in.in1, in.in16, in.in32, in.in64};
            expected.secondScalars = previous.firstScalars;
        }
        if (!check("rising")) return 3;
        // Neither a repeated eval nor changed data without a new clock edge
        // may call DPI twice or advance either register bank.
        if (!check("held-high")) return 4;
        in.in8 ^= 0xa5;
        in.in1 = !in.in1;
        in.in16 ^= 0xffffu;
        in.in32 ^= 0xffffffffu;
        in.in64 ^= 0xffffffffffffffffULL;
        in.in65[0] ^= ~std::uint64_t{};
        in.in130[0] ^= ~std::uint64_t{};
        in.in1025[8] ^= ~std::uint64_t{};
        // Synchronous reset toggles without a rising edge must not clear state.
        in.reset = !in.reset;
        if (!check("changed-data-high")) return 5;
    }
    sim.init();
    dpi_calls = expectedCalls = 0;
    in = {};
    expected = {};
    if (!check("reinit-low")) return 6;
    // Exercise exception publication and shutdown, then reuse the wrapper.
    // With Host lane 0 this early DPI phase runs on a background worker;
    // the default Host lane exercises the participating-Host exception path.
    throw_next_dpi.store(true);
    in.clock = true;
    try {
        sim.eval(in);
        std::fprintf(stderr, "DPI exception was not propagated\n");
        return 7;
    } catch (const std::runtime_error &error) {
        if (std::string_view(error.what()) != "repcut-test-worker-error") return 8;
    }
    sim.init();
    dpi_calls = expectedCalls = 0;
    in = {};
    expected = {};
    if (!check("reinit-after-exception")) return 9;
    in.clock = true;
    expected.dpi = 7;
    ++expectedCalls;
    if (!check("recovered-rising")) return 10;
    sim.dump_runtime_profile();
    std::puts("grhsim-repcut-harness-ok");
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2) return 10;
    return std::string_view(argv[1]) == "wrapper" ? run<Wrapper>() : run<Facade>();
}
)cpp";

    unsigned availableCpuCount();

    int runInputDirtyTrackingCase(const std::filesystem::path &artifactRoot,
                                  const std::string &compiler)
    {
        struct RestoreEnvironment
        {
            std::vector<std::pair<std::string, std::optional<std::string>>> saved;
            ~RestoreEnvironment()
            {
                for (const auto &[key, old] : saved)
                {
                    if (old) ::setenv(key.c_str(), old->c_str(), 1);
                    else ::unsetenv(key.c_str());
                }
            }
        } restore;
        for (const auto *key : {"WOLVRIX_GRHSIM_INPUT_DIRTY_TRACKING", "WOLVRIX_GRHSIM_INPUT_BIT_ACTIVATION",
                                "WOLVRIX_GRHSIM_INPUT_FULLPASS_SPECIALIZATION"})
        {
            const auto *old = std::getenv(key);
            restore.saved.emplace_back(key, old ? std::optional<std::string>(old) : std::nullopt);
        }
        constexpr int32_t width = 8200; // Includes both eight-word chunks and a partial tail.
        Design design;
        auto assign = [&](Graph &graph, const std::string &name, ValueId source) {
            const auto result = value(graph, name, graph.valueWidth(source));
            const auto op = graph.createOperation(OperationKind::kAssign, graph.internSymbol(name + "_op"));
            graph.addOperand(op, source);
            graph.addResult(op, result);
            return result;
        };
        auto slice = [&](Graph &graph, const std::string &name, ValueId source, int64_t lo, int64_t hi) {
            const auto result = value(graph, name, static_cast<int32_t>(hi - lo + 1));
            const auto op = graph.createOperation(OperationKind::kSliceStatic, graph.internSymbol(name + "_op"));
            graph.setAttr(op, "sliceStart", lo);
            graph.setAttr(op, "sliceEnd", hi);
            graph.addOperand(op, source);
            graph.addResult(op, result);
            return result;
        };
        auto &producer = design.createGraph("DirtyProducer");
        const auto pa = input(producer, "a", width);
        const auto pb = input(producer, "b", width);
        producer.bindOutputPort("a_out", assign(producer, "a_copy", pa));
        producer.bindOutputPort("b_out", assign(producer, "b_copy", pb));
        auto &sink = design.createGraph("DirtySink");
        const auto a = input(sink, "zbus", width);
        const auto b = input(sink, "abus", width);
        const auto direct = input(sink, "direct", width);
        const auto alias = input(sink, "alias_one", width);
        sink.bindInputPort("alias_two", alias);
        sink.bindOutputPort("whole", binary(sink, "whole_xor", width, OperationKind::kXor, a, direct));
        sink.bindOutputPort("low", binary(sink, "low_xor", 64, OperationKind::kXor,
                                           slice(sink, "low0", b, 0, 63),
                                           slice(sink, "low2", b, 128, 191)));
        sink.bindOutputPort("tail", slice(sink, "tail_value", b, 8192, 8199));
        sink.bindOutputPort("aliased", assign(sink, "alias_copy", alias));
        auto &top = design.createGraph("DirtyTop");
        const auto ta = input(top, "a", width);
        const auto tb = input(top, "b", width);
        const auto td = input(top, "direct", width);
        const auto wa = value(top, "wire_a", width);
        const auto wb = value(top, "wire_b", width);
        const auto whole = value(top, "whole", width);
        const auto low = value(top, "low", 64);
        const auto tail = value(top, "tail", 8);
        const auto aliased = value(top, "aliased", width);
        top.bindOutputPort("whole", whole);
        top.bindOutputPort("low", low);
        top.bindOutputPort("tail", tail);
        top.bindOutputPort("aliased", aliased);
        instance(top, "source", "DirtyProducer", {ta, tb}, {wa, wb}, {"a", "b"}, {"a_out", "b_out"});
        instance(top, "sink", "DirtySink", {wa, wb, td, wa, wa}, {whole, low, tail, aliased},
                 {"zbus", "abus", "direct", "alias_one", "alias_two"},
                 {"whole", "low", "tail", "aliased"});
        design.markAsTop(top.symbol());
        const auto caseRoot = artifactRoot / "input_dirty_tracking";
        std::filesystem::create_directories(caseRoot);
        const auto harnessPath = caseRoot / "harness.cpp";
        {
            std::ofstream harness(harnessPath);
            harness << R"cpp(#include "grhsim_DirtyTop.hpp"
#include "Gsink.h"
#include <array>
#include <cstdint>
#include <stdexcept>
int main() {
    GrhSIM_DirtyTop sim;
    std::uint64_t random = 0x9e3779b97f4a7c15ULL;
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        sim.init();
        for (unsigned step = 0; step < 600; ++step) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            const auto word = step % 129;
            if (step % 5 != 0) sim.a[word] ^= random;
            if (step % 3 != 0) sim.b[(word * 17) % 129] ^= random;
            if (step % 7 != 0) sim.direct[(word * 11) % 129] ^= random;
            sim.a.back() &= 255; sim.b.back() &= 255; sim.direct.back() &= 255;
            sim.eval(); sim.eval(); // Cross-partition publication takes one step.
            for (unsigned i = 0; i < 129; ++i)
                if (sim.whole[i] != (sim.a[i] ^ sim.direct[i]) || sim.aliased[i] != sim.a[i]) return 1;
            if (sim.low != (sim.b[0] ^ sim.b[2]) || sim.tail != sim.b.back()) return 2;
            sim.eval(); // Unchanged publication must not leave a stale dirty flag.
            if (sim.low != (sim.b[0] ^ sim.b[2])) return 3;
        }
    }
#ifdef TEST_TRACKED
    Gsink unit;
    unit.init();
    unit.eval_tracked();
    unit.in_0.back() = 3;
    unit.in_1[0] = 2; unit.in_1[2] = 7;
    unit.in_2.back() = 5; // Untracked top input must still be compared.
    unit.input_changes_[0] = 1; unit.input_changes_[64] = 1;
    unit.eval_tracked();
    if (unit.out_0.back() != 6 || unit.out_1 != 5) return 4;
    if (unit.input_changes_[0] || unit.input_changes_[64]) return 5;
    unit.eval_tracked();
    try { unit.eval(); return 6; } catch (const std::logic_error &) {}
    unit.init();
    unit.eval();
    unit.in_0.back() = 9;
    unit.eval();
    if (unit.out_0.back() != 9) return 7;
    try { unit.eval_tracked(); return 8; } catch (const std::logic_error &) {}
    unit.init();
    unit.eval_tracked();
#endif
    return 0;
}
)cpp";
        }
        for (const std::string mode : {"default", "off", "on", "whole", "fullpass"})
        {
            const bool tracked = mode != "off" && mode != "default";
            const auto package = caseRoot / mode;
            EmitOptions options;
            options.outputDir = package.string();
            options.topOverrides = {"DirtyTop"};
            ::setenv("WOLVRIX_GRHSIM_INPUT_BIT_ACTIVATION", mode == "whole" ? "0" : "1", 1);
            ::setenv("WOLVRIX_GRHSIM_INPUT_FULLPASS_SPECIALIZATION", mode == "fullpass" ? "1" : "0", 1);
            ::unsetenv("WOLVRIX_GRHSIM_INPUT_DIRTY_TRACKING");
            if (mode != "default")
            {
                ::setenv("WOLVRIX_GRHSIM_INPUT_DIRTY_TRACKING", "1", 1);
                options.attributes["input_dirty_tracking"] = tracked ? "1" : "0";
            }
            EmitDiagnostics diagnostics;
            EmitGrhSimRepCutPackage emitter(&diagnostics);
            if (!emitter.emit(design, options).success || diagnostics.hasError())
                return fail("dirty-input " + mode + " emit failed: " + diagnosticsSummary(diagnostics));
            const auto sinkHeader = readFile(package / "Gsink.h");
            if (tracked && (sinkHeader.find("std::array<std::int8_t, 128>") == std::string::npos ||
                            readFile(package / "Gsource.h").find("eval_tracked") != std::string::npos))
                return fail("dirty-input selection should track only the two unique internal inputs");
            if (!tracked && sinkHeader.find("eval_tracked") != std::string::npos)
                return fail("disabled dirty-input option changed the adapter API");
            if (mode == "off")
            {
                for (const auto &entry : std::filesystem::directory_iterator(package))
                {
                    const auto extension = entry.path().extension();
                    if (extension != ".cpp" && extension != ".hpp" && extension != ".h") continue;
                    if (readFile(entry.path()) != readFile(caseRoot / "default" / entry.path().filename()))
                        return fail("explicit dirty-input off differs from default: " + entry.path().string());
                }
            }
            const auto log = caseRoot / (mode + ".log");
            const auto executable = caseRoot / (mode + "_harness");
            const auto build = "timeout 180s make -C " + shellQuote(package.string()) + " -j2 CXX=" +
                shellQuote(compiler) + " CXXFLAGS='-std=c++20 -O2' > " + shellQuote(log.string()) + " 2>&1";
            const auto link = "timeout 60s " + shellQuote(compiler) + " -std=c++20 -O2 -pthread " +
                (tracked ? "-DTEST_TRACKED " : "") + "-I" + shellQuote(package.string()) + " " +
                shellQuote(harnessPath.string()) + " " + shellQuote((package / "libgrhsim_DirtyTop.a").string()) +
                " -o " + shellQuote(executable.string()) + " >> " + shellQuote(log.string()) + " 2>&1";
            if (std::system(build.c_str()) != 0 || std::system(link.c_str()) != 0)
                return fail("dirty-input " + mode + " build failed:\n" + readFile(log));
            for (const unsigned threads : {1u, 2u})
            {
                if (threads > availableCpuCount()) continue;
                const auto run = "timeout 30s env -u WOLVI_REPCUT_PIN_THREADS -u WOLVI_REPCUT_TIMING_JSONL -u WOLVI_REPCUT_HOST_LANE "
                    "-u WOLVI_REPCUT_HOST_SPIN_US -u WOLVI_REPCUT_WORKER_SPIN_US " +
                    std::string(threads == 2 ? "WOLVI_REPCUT_HOST_SPIN_US=0 WOLVI_REPCUT_WORKER_SPIN_US=0 " : "") +
                    "XS_EMU_THREADS=" +
                    std::to_string(threads) + " " + shellQuote(executable.string()) + " >> " +
                    shellQuote(log.string()) + " 2>&1";
                if (std::system(run.c_str()) != 0)
                    return fail("dirty-input " + mode + " N=" + std::to_string(threads) +
                                " behavior failed:\n" + readFile(log));
            }
        }
        return 0;
    }

    unsigned availableCpuCount()
    {
#if defined(__linux__)
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0)
        {
            return static_cast<unsigned>(CPU_COUNT(&cpus));
        }
#endif
        return 1;
    }
}

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    if (std::getenv("WOLVRIX_TEST_INPUT_DIRTY_TRACKING"))
    {
#ifdef WOLF_TEST_CXX_COMPILER
        const std::string compiler = WOLF_TEST_CXX_COMPILER;
#else
        const std::string compiler = "c++";
#endif
        return runInputDirtyTrackingCase(std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) /
                                          "grhsim_repcut_package_focused", compiler);
    }
    const Design design = buildDesign();
    wolvrix::lib::store::StoreJson store;
    const auto original = store.storeToString(design);
    if (!original) return fail("cannot serialize fixture before emission");
    const std::filesystem::path artifactRoot =
        std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_repcut_package";
    const auto packageDir = artifactRoot / "package";
    std::error_code ec;
    std::filesystem::remove_all(artifactRoot, ec);
    if (ec) return fail("cannot clear previous test artifacts: " + ec.message());
    std::filesystem::create_directories(packageDir);

    EmitDiagnostics diagnostics;
    EmitGrhSimRepCutPackage emitter(&diagnostics);
    EmitOptions options;
    options.outputDir = packageDir.string();
    options.topOverrides = {"RepCutTop"};
    const auto result = emitter.emit(design, options);
    if (!result.success || diagnostics.hasError())
    {
        return fail("package emission failed: " + diagnosticsSummary(diagnostics));
    }
    if (store.storeToString(design) != original)
    {
        return fail("package emission changed the source design");
    }
    for (const auto *name : {"wolvi_repcut_grhsim_sim.h", "grhsim_RepCutTop.hpp", "Makefile"})
    {
        if (!std::filesystem::is_regular_file(packageDir / name))
        {
            return fail(std::string("missing package artifact: ") + name);
        }
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator(packageDir))
    {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension();
        if (extension != ".cpp" && extension != ".h" && extension != ".hpp") continue;
        const auto text = readFile(entry.path());
        if (text.find("#include <verilated") != std::string::npos ||
            text.find("#include \"verilated") != std::string::npos)
        {
            return fail("GrhSIM package depends on Verilator: " + entry.path().string());
        }
    }
    const auto commonSource = readFile(packageDir / "wolvi_repcut_grhsim_sim_common.cpp");
    if (commonSource.find("run_phase_workers_(load_step_fns_);") == std::string::npos ||
        commonSource.find("run_host_phase_(load_step_fns_);") != std::string::npos)
    {
        return fail("GrhSIM package should dispatch independent input loads through workers");
    }
    if (commonSource.find("wolvi_repcut_enable_code_hugepages") == std::string::npos ||
        commonSource.find("WOLVI_GRHSIM_CODE_HUGEPAGE") == std::string::npos ||
        commonSource.find("mremap(copy") == std::string::npos)
    {
        return fail("GrhSIM package should include the default code-page huge-page fallback");
    }
    // Re-emission must build only this package's current source manifest, not
    // wildcard unrelated or obsolete C++ files left in an existing directory.
    const auto staleSource = packageDir / "obsolete_partition.cpp";
    {
        std::ofstream stale(staleSource);
        stale << "#error stale source must not enter the generated build\n";
        if (!stale) return fail("cannot write stale-source fixture");
    }
    EmitDiagnostics reemitDiagnostics;
    EmitGrhSimRepCutPackage reemitter(&reemitDiagnostics);
    if (!reemitter.emit(design, options).success || reemitDiagnostics.hasError())
    {
        return fail("package re-emission failed: " + diagnosticsSummary(reemitDiagnostics));
    }
    if (readFile(packageDir / "Makefile").find("obsolete_partition.cpp") != std::string::npos ||
        store.storeToString(design) != original)
    {
        return fail("package re-emission included a stale source or changed the source design");
    }

    // Top names are public C++ API. Unsupported names must fail generation
    // explicitly, rather than produce an uncompilable facade or collisions.
    for (const bool collision : {false, true})
    {
        Design invalid = buildDesign();
        auto *top = invalid.findGraph("RepCutTop");
        if (!top) return fail("invalid-name fixture is missing its top graph");
        input(*top, collision ? "name-with-dash" : "class", 1);
        if (collision) input(*top, "name_with_dash", 1);
        const auto invalidOriginal = store.storeToString(invalid);
        EmitOptions invalidOptions = options;
        invalidOptions.outputDir = (artifactRoot / (collision ? "invalid_collision" : "invalid_keyword")).string();
        EmitDiagnostics invalidDiagnostics;
        EmitGrhSimRepCutPackage invalidEmitter(&invalidDiagnostics);
        if (invalidEmitter.emit(invalid, invalidOptions).success || !invalidDiagnostics.hasError())
        {
            return fail(collision ? "colliding public names should be rejected"
                                  : "C++ keyword public name should be rejected");
        }
        if (!invalidOriginal || store.storeToString(invalid) != invalidOriginal)
        {
            return fail("rejected package emission changed the source design");
        }
    }
    // Public ports are emitted into package headers and must not redefine
    // package feature/include guards or shadow the wrapper class. Check these names
    // through the actual emitter's fail-closed path.
    for (const std::string_view reservedPort : {
             std::string_view{"WOLVRIX_GRHSIM_PERF"},
             std::string_view{"WOLVI_REPCUT_GRHSIM_SIM_H"},
             std::string_view{"WolviRepCutGrhSimSim"}})
    {
        Design invalid = buildDesign();
        auto *top = invalid.findGraph("RepCutTop");
        if (!top) return fail("reserved-port fixture is missing its top graph");
        input(*top, std::string(reservedPort), 1);
        const auto invalidOriginal = store.storeToString(invalid);
        EmitOptions invalidOptions = options;
        invalidOptions.outputDir = (artifactRoot / ("invalid_reserved_" + std::string(reservedPort))).string();
        EmitDiagnostics invalidDiagnostics;
        EmitGrhSimRepCutPackage invalidEmitter(&invalidDiagnostics);
        const auto invalidResult = invalidEmitter.emit(invalid, invalidOptions);
        const std::string summary = diagnosticsSummary(invalidDiagnostics);
        if (invalidResult.success || !invalidDiagnostics.hasError() ||
            summary.find(std::string(reservedPort)) == std::string::npos)
        {
            return fail("reserved public port was not rejected: " + std::string(reservedPort) +
                        " diagnostics=" + summary);
        }
        if (!invalidOriginal || store.storeToString(invalid) != invalidOriginal)
        {
            return fail("reserved-port rejection changed the source design: " + std::string(reservedPort));
        }
        if (!invalidResult.artifacts.empty() || containsFiles(*invalidOptions.outputDir))
        {
            return fail("reserved-port rejection left partial package files: " + std::string(reservedPort));
        }
    }

    // Native GrhSIM models currently own separate file-handle maps. Reject
    // these operations before cloning/emitting models, including fflush()
    // without a handle, instead of silently misusing handles across units.
    for (const std::string_view operationName : {"fopen", "ferror", "fdisplay", "fwrite", "fflush", "fclose"})
    {
        Design invalid = buildDesign();
        auto *graph = invalid.findGraph("EarlyDpi");
        if (!graph) return fail("file-handle fixture is missing its reachable unit graph");
        const std::string opName = "file_" + std::string(operationName);
        const bool isFunction = operationName == "fopen" || operationName == "ferror";
        const auto op = graph->createOperation(isFunction ? OperationKind::kSystemFunction
                                                         : OperationKind::kSystemTask,
                                               graph->internSymbol(opName));
        graph->setAttr(op, "name", std::string(operationName));
        graph->setAttr(op, "hasSideEffects", true);
        graph->setAttr(op, "procKind", std::string("initial"));
        graph->setAttr(op, "hasTiming", false);
        auto stringConstant = [&](const std::string &name, const std::string &literal) {
            const auto text = graph->createValue(graph->internSymbol(name), 0, false, ValueType::String);
            const auto textOp = graph->createOperation(OperationKind::kConstant,
                                                      graph->internSymbol(name + "_op"));
            graph->setAttr(textOp, "constValue", literal);
            graph->addResult(textOp, text);
            return text;
        };
        if (operationName == "fopen")
        {
            graph->addOperand(op, stringConstant("filename", "\"never-created.txt\""));
            graph->addOperand(op, stringConstant("filemode", "\"w\""));
        }
        else
        {
            if (!isFunction) graph->addOperand(op, constant(*graph, "file_condition", 1, "1'b1"));
            if (operationName != "fflush")
            {
                graph->addOperand(op, constant(*graph, "file_handle", 32, "32'd3"));
            }
            if (operationName == "fdisplay" || operationName == "fwrite")
            {
                graph->addOperand(op, stringConstant("file_format", "\"test\""));
            }
        }
        if (isFunction) graph->addResult(op, value(*graph, "file_result", 32));

        const auto invalidOriginal = store.storeToString(invalid);
        EmitOptions invalidOptions = options;
        invalidOptions.outputDir = (artifactRoot / ("invalid_file_" + std::string(operationName))).string();
        EmitDiagnostics invalidDiagnostics;
        EmitGrhSimRepCutPackage invalidEmitter(&invalidDiagnostics);
        const auto invalidResult = invalidEmitter.emit(invalid, invalidOptions);
        const std::string summary = diagnosticsSummary(invalidDiagnostics);
        if (invalidResult.success || !invalidDiagnostics.hasError() ||
            summary.find("does not support $" + std::string(operationName)) == std::string::npos ||
            summary.find("file handles are model-local") == std::string::npos ||
            summary.find("no shared package file-handle context") == std::string::npos ||
            summary.find("EarlyDpi." + opName) == std::string::npos)
        {
            return fail("file-handle operation lacks an explicit package diagnostic: " +
                        std::string(operationName) + " diagnostics=" + summary);
        }
        if (!invalidOriginal || store.storeToString(invalid) != invalidOriginal)
        {
            return fail("file-handle rejection changed the source design: " + std::string(operationName));
        }
        if (!invalidResult.artifacts.empty() || containsFiles(*invalidOptions.outputDir))
        {
            return fail("file-handle rejection left partial package files: " + std::string(operationName));
        }
    }
    const auto harnessPath = artifactRoot / "harness.cpp";
    {
        std::ofstream harness(harnessPath);
        harness << kHarness;
        if (!harness) return fail("cannot write behavior harness");
    }
    const auto buildLog = artifactRoot / "build.log";
    const auto executable = artifactRoot / "harness";
#ifdef WOLF_TEST_CXX_COMPILER
    const std::string compiler = WOLF_TEST_CXX_COMPILER;
#else
    const std::string compiler = "c++";
#endif
    const auto compile =
        "timeout 180s make -C " + shellQuote(packageDir.string()) + " -j2 CXX=" + shellQuote(compiler) +
        " CXXFLAGS='-std=c++20 -O0' > " + shellQuote(buildLog.string()) + " 2>&1";
    if (std::system(compile.c_str()) != 0)
    {
        return fail("generated library build failed:\n" + readFile(buildLog));
    }
    const auto link = "timeout 60s " + shellQuote(compiler) + " -std=c++20 -O0 -pthread -I" +
        shellQuote(packageDir.string()) + " " + shellQuote(harnessPath.string()) + " " +
        shellQuote((packageDir / "libgrhsim_RepCutTop.a").string()) + " -o " +
        shellQuote(executable.string()) + " >> " + shellQuote(buildLog.string()) + " 2>&1";
    if (std::system(link.c_str()) != 0)
    {
        return fail("generated harness link failed:\n" + readFile(buildLog));
    }
    std::vector<unsigned> threadCounts{1};
    if (availableCpuCount() >= 2) threadCounts.push_back(2);
    else std::cout << "[emit_grhsim_repcut_package] N=2 run skipped: fewer than 2 allowed CPUs\n";
    if (availableCpuCount() >= 3) threadCounts.push_back(3);
    else std::cout << "[emit_grhsim_repcut_package] N=K run skipped: fewer than 3 allowed CPUs\n";
    std::string reference;
    for (const unsigned threads : threadCounts)
    {
        const std::vector<bool> waitPolicies = threads == 3 ? std::vector<bool>{false, true}
                                                           : std::vector<bool>{false};
        for (const bool useCv : waitPolicies)
        {
            for (const auto *api : {"wrapper", "facade"})
            {
                const std::string name = std::string(api) + "_n" + std::to_string(threads) +
                                         (useCv ? "_cv" : "");
                const auto output = artifactRoot / (name + ".out");
                const auto log = artifactRoot / (name + ".log");
                const auto run =
                    "timeout 30s env -u WOLVI_REPCUT_PIN_THREADS -u WOLVI_REPCUT_HOST_LANE -u WOLVI_REPCUT_HOST_SPIN_US "
                    "-u WOLVI_REPCUT_WORKER_SPIN_US -u WOLVI_REPCUT_TIMING_JSONL "
                    + std::string(useCv ? "WOLVI_REPCUT_HOST_SPIN_US=0 WOLVI_REPCUT_WORKER_SPIN_US=0 " : "")
                    + "XS_EMU_THREADS=" +
                    std::to_string(threads) + " " + shellQuote(executable.string()) + " " + api +
                    " > " + shellQuote(output.string()) + " 2> " + shellQuote(log.string());
                if (std::system(run.c_str()) != 0)
                {
                    return fail(name + " behavior test failed:\n" + readFile(log));
                }
                const auto transcript = readFile(output);
                if (transcript.find("grhsim-repcut-harness-ok") == std::string::npos)
                {
                    return fail(name + " did not complete all behavior checks");
                }
                if (reference.empty()) reference = transcript;
                else if (reference != transcript)
                {
                    return fail(name + " disagrees with the N=1 wrapper transcript");
                }
                const auto runtimeLog = readFile(log);
                if (transcript.find("repcut-standard-stream-0") == std::string::npos ||
                    transcript.find("repcut-standard-stream-1") == std::string::npos ||
                    transcript.find("repcut-standard-stream-2") != std::string::npos ||
                    runtimeLog.find("repcut-standard-stream-2") == std::string::npos)
                {
                    return fail(name + " standard-stream descriptors routed output incorrectly");
                }
                if (runtimeLog.find("requested=" + std::to_string(threads) + " effective=" +
                                    std::to_string(threads)) == std::string::npos)
                {
                    return fail(name + " did not use the requested thread count");
                }
                if (threads >= 2)
                {
                    if (runtimeLog.find("\"thread_pinning\":\"disabled\"") == std::string::npos ||
                        runtimeLog.find("\"worker_cpus\":[]") == std::string::npos)
                    {
                        return fail(name + " enabled thread pinning without the explicit opt-in");
                    }
                    const std::string expectedPolicy = threads == 3 && !useCv ? "all-spin"
                                                                              : "condition-variable";
                    if (runtimeLog.find("\"wait_policy\":\"" + expectedPolicy + "\"") == std::string::npos)
                    {
                        return fail(name + " did not use the expected wait policy");
                    }
                }
                if (threads == 3)
                {
                    const auto alternateOutput = artifactRoot / (name + "_host0.out");
                    const auto alternateLog = artifactRoot / (name + "_host0.log");
                    const auto alternateRun =
                        "timeout 30s env -u WOLVI_REPCUT_PIN_THREADS -u WOLVI_REPCUT_HOST_SPIN_US -u WOLVI_REPCUT_WORKER_SPIN_US "
                        "-u WOLVI_REPCUT_TIMING_JSONL WOLVI_REPCUT_HOST_LANE=0 XS_EMU_THREADS=3 "
                        + std::string(useCv ? "WOLVI_REPCUT_HOST_SPIN_US=0 WOLVI_REPCUT_WORKER_SPIN_US=0 " : "")
                        + shellQuote(executable.string()) + " " + api + " > "
                        + shellQuote(alternateOutput.string()) + " 2> " + shellQuote(alternateLog.string());
                    if (std::system(alternateRun.c_str()) != 0 || readFile(alternateOutput) != reference)
                    {
                        return fail(name + " alternate Host lane / worker exception failed:\n" + readFile(alternateLog));
                    }
                    if (useCv)
                    {
                        const char *mixedEnvironments[] = {
                            "WOLVI_REPCUT_HOST_SPIN_US=infinite WOLVI_REPCUT_WORKER_SPIN_US=0 ",
                            "WOLVI_REPCUT_HOST_SPIN_US=0 WOLVI_REPCUT_WORKER_SPIN_US=infinite ",
                            "WOLVI_REPCUT_HOST_SPIN_US=50 WOLVI_REPCUT_WORKER_SPIN_US=0 ",
                            "WOLVI_REPCUT_HOST_SPIN_US=50 WOLVI_REPCUT_WORKER_SPIN_US=infinite ",
                        };
                        for (unsigned mode = 0; mode < 4; ++mode)
                        {
                            const auto mixedName = name + "_mixed_" + std::to_string(mode);
                            const auto mixedOutput = artifactRoot / (mixedName + ".out");
                            const auto mixedLog = artifactRoot / (mixedName + ".log");
                            const auto mixedRun = "timeout 30s env -u WOLVI_REPCUT_PIN_THREADS -u WOLVI_REPCUT_TIMING_JSONL "
                                "WOLVI_REPCUT_HOST_LANE=0 XS_EMU_THREADS=3 "
                                + std::string(mixedEnvironments[mode]) + shellQuote(executable.string())
                                + " " + api + " > " + shellQuote(mixedOutput.string())
                                + " 2> " + shellQuote(mixedLog.string());
                            if (std::system(mixedRun.c_str()) != 0 || readFile(mixedOutput) != reference)
                            {
                                return fail(mixedName + " mixed wait / exception test failed:\n" + readFile(mixedLog));
                            }
                        }
                    }
                }
            }
        }
    }
    // The first case combines private clone, runtime-header, and model/adapter
    // name collisions. The second isolates a facade/adapter class collision.
    if (runInputDirtyTrackingCase(artifactRoot, compiler) != 0 ||
        runNameCollisionCase(artifactRoot, compiler, "Gpart_0_model_runtime",
                             {"part_0", "foo", "rhSIM_Gfoo_model"}) != 0 ||
        runNameCollisionCase(artifactRoot, compiler, "Top", {"rhSIM_Top"}) != 0)
    {
        return 1;
    }
    std::cout << "[emit_grhsim_repcut_package] package, scalar/wide state, constants, reused models, "
                 "early DPI, reinitialization, and serial/parallel APIs passed\n";
    return 0;
}
