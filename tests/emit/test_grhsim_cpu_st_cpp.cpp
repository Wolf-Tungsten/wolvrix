#include "emit/grhsim/cpu-st-cpp.hpp"
#include "function-writer.hpp"
#include "grhsim_partition_fixture.hpp"

#include <sstream>
#include <regex>

namespace {
using namespace partition_test;
namespace emit = wolvrix::lib::emit;
using wolvrix::lib::emit::grhsim_detail::Code;
using wolvrix::lib::emit::grhsim_detail::Function;
using wolvrix::lib::emit::grhsim_detail::Local;
using wolvrix::lib::emit::grhsim_detail::writeFunction;
using wolvrix::lib::emit::grhsim_detail::writeValueStruct;
using wolvrix::lib::emit::grhsim_detail::writeValueLayout;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path); require(bool(stream), "Missing artifact: " + path.string());
    return {std::istreambuf_iterator<char>(stream), {}};
}

void inspectFunctions(const std::filesystem::path& directory, std::size_t budget) {
    std::size_t functions = 0;
    std::ofstream report(directory.parent_path() / "function-lines.tsv");
    report << "source\tfunction\tactual_lines\n";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".cpp") continue;
        std::istringstream stream(readFile(entry.path()));
        std::string line, signature; std::size_t count = 0;
        while (std::getline(stream, line)) {
            if (!count && line.find("GrhSIM_top::") != std::string::npos && line.find('(') != std::string::npos) {
                count = 1; signature = line; continue;
            }
            if (!count) continue;
            ++count;
            if (line == "}") {
                require(count <= budget, "Function actually exceeds budget: " + signature);
                report << entry.path().filename().string() << '\t' << signature << '\t' << count << '\n';
                ++functions; count = 0;
            }
        }
        require(count == 0, "Generated function did not end at its own scope");
    }
    require(functions > 10, "Independent line scan found too few generated functions");
    const auto manifest = readFile(directory / "manifest.json");
    const auto header = readFile(directory / "grhsim_top.hpp");
    require(header.find("pGeneral_chunk") == std::string::npos &&
                header.find("chunkFrame_pGeneral") == std::string::npos,
            "General phase driver contains computation chunks or a spill frame");
    std::size_t recorded = 0, offset = 0;
    while ((offset = manifest.find("\"lines\":", offset)) != std::string::npos) { ++recorded; ++offset; }
    require(recorded == functions, "Manifest omitted a generated function");
}

void runMakefile(const std::filesystem::path& path) {
    std::ofstream make(path / "run.mk");
    make << "CXX := clang++\nCXXFLAGS := -std=c++20 -O1 -fno-strict-aliasing -DWOLVRIX_GRHSIM_PERF=1 -DWOLVRIX_GRHSIM_SN_TIMING=1\n"
        << "FST_INCLUDE := " << WOLVRIX_CPU_ST_TEST_FST_DIR << "\n"
        << "model/libgrhsim_top.a: model/Makefile\n\t$(MAKE) -C model CXX='$(CXX)' CXXFLAGS='$(CXXFLAGS)'\n"
        << "driver: driver.cpp model/libgrhsim_top.a\n\t$(CXX) $(CXXFLAGS) -I$(FST_INCLUDE) $< model/libgrhsim_top.a -lz -o $@\n"
        << "run: driver\n\tWOLVRIX_GRHSIM_SN_PROFILE=profile.tsv ./driver > observations.txt\n"
        << ".PHONY: run\n";
}

void manualCase(Fixture&, std::string, std::size_t, const std::string&, const std::string&, bool,
                const std::map<std::string, std::string>&);

void testExecutionCounters() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), data = fixture.input("data", 8);
    const auto scalar = fixture.state("scalar", 8), array = fixture.state("array", 8192, {1024});
    const auto index = fixture.constant(10, "0");
    const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, fixture.read(scalar)});
    fixture.write(OperationKind::kRegisterWritePort, scalar, {enable, sum, clock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, array, {enable, index, sum, clock});
    fixture.graph.bindOutputPort("scalar_out", fixture.read(scalar));
    fixture.graph.bindOutputPort("array_out", fixture.read(array, {index}));
    const auto format = fixture.graph.createOperation(OperationKind::kConstant);
    const auto text = fixture.graph.createValue({grh::ValueType::String, 0, false, grh::LogicDomain::TwoState, {}});
    fixture.graph.addResult(format, text); fixture.graph.setAttr(format, "constValue", std::string("count=%d"));
    fixture.task("display", {enable, text, data, clock}, {"posedge"});
    fixture.task("strobe", {enable, text, data, clock}, {"posedge"});
    fixture.task("monitor", {enable, text, data});
    const auto random = fixture.compute(OperationKind::kSystemFunction, Fixture::type(32), {});
    fixture.graph.setAttr(fixture.graph.valueDef(random), "name", std::string("random"));
    fixture.graph.bindOutputPort("random_out", random);
    const auto imported = fixture.graph.createOperation(OperationKind::kDpicImport, fixture.graph.internSymbol("count_dpi"));
    fixture.graph.setAttr(imported, "argsName", std::vector<std::string>{});
    fixture.graph.setAttr(imported, "argsDirection", std::vector<std::string>{});
    fixture.graph.setAttr(imported, "argsWidth", std::vector<int64_t>{});
    fixture.graph.setAttr(imported, "argsSigned", std::vector<bool>{});
    fixture.graph.setAttr(imported, "hasReturn", false);
    const auto call = fixture.graph.createOperation(OperationKind::kDpicCall);
    fixture.graph.addOperand(call, enable); fixture.graph.addOperand(call, clock);
    fixture.graph.setAttr(call, "targetImportSymbol", std::string("count_dpi"));
    fixture.graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    fixture.convert(); fixture.split(); fixture.partition();
    std::map<std::string, std::string> defaultFiles;
    for (const bool explicitOff : {false, true}) {
        const auto path = artifact(explicitOff ? "execution-counts-off" : "execution-counts-default");
        std::filesystem::remove_all(path);
        emit::EmitDiagnostics diagnostics; emit::GrhSimCpuStCpp emitter(&diagnostics);
        emit::EmitOptions options; options.outputDir = path.string(); options.session = &fixture.session;
        if (explicitOff) options.attributes["execution-counters"] = "off";
        const auto result = emitter.emit(fixture.design, options);
        require(result.success, "Failed to emit disabled counter model");
        for (const auto& file : result.artifacts) {
            const auto name = std::filesystem::path(file).filename().string();
            const auto contents = readFile(file);
            if (explicitOff) require(defaultFiles.at(name) == contents, "Disabled counters changed generated source");
            else defaultFiles.emplace(name, contents);
        }
        require(result.artifacts.size() == defaultFiles.size(), "Disabled counters changed emitted files");
    }
    for (const std::size_t budget : {16u, 1024u}) {
        const auto label = "execution-counts-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "static unsigned calls; extern \"C\" void count_dpi() { ++calls; }", R"(
for (unsigned epoch = 0; epoch < 2; ++epoch) {
    model.init(); calls = 0; std::uint8_t expected = 0;
    for (unsigned step = 0; step < 12; ++step) {
        model.clock = step & 1; model.enable = step % 3 != 0; model.data = step + 1; model.eval();
        if (model.clock && model.enable) expected += model.data;
        assert(model.scalar_out == expected && model.array_out == expected);
    }
    assert(calls == 4);
    const char *path = epoch == 0 ? "execution1.tsv" : "execution2.tsv";
    auto *stream = std::fopen(path, "w"); assert(stream); model.dump_execution_counts(stream); std::fclose(stream);
}
)", false, {{"execution-counters", "on"}});
    }
}

void manualCase(Fixture& fixture, std::string label, std::size_t budget,
                const std::string& support, const std::string& body, bool waveform = true,
                const std::map<std::string, std::string>& attributes = {}) {
    const auto path = artifact(label);
    std::filesystem::remove_all(path); std::filesystem::create_directories(path);
    emit::EmitDiagnostics diagnostics; emit::GrhSimCpuStCpp emitter(&diagnostics);
    emit::EmitOptions options;
    options.outputDir = (path / "model").string(); options.session = &fixture.session;
    options.attributes = {{"max-function-lines", std::to_string(budget)}, {"max-tu-bytes", "4096"}, {"waveform", waveform ? "declared-symbols" : "none"}};
    for (const auto& [key, value] : attributes) options.attributes[key] = value;
    if (!emitter.emit(fixture.design, options).success) {
        std::string message;
        for (const auto& entry : diagnostics.messages()) message += entry.message + '\n';
        throw std::runtime_error(label + ": " + message);
    }
    inspectFunctions(path / "model", budget);
    std::ofstream driver(path / "driver.cpp");
    driver << "#include \"model/grhsim_top.hpp\"\n#include <cassert>\n" << support << "\nint main() {\nGrhSIM_top model;\n" << body << "\nreturn 0;\n}\n";
    runMakefile(path);
    std::ofstream make(artifact("Makefile"), std::ios::app);
    make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
}

// Expected results are calculated on the pre-partition logical graph. The
// generated program is compiled/executed by the outer project Make target.
void writeCase(Fixture& fixture, const grh::Design& reference,
               const std::vector<std::map<std::string, uint64_t>>& inputs,
               const std::string& label, std::size_t budget, std::size_t tuBytes) {
    const auto path = artifact(label);
    std::filesystem::remove_all(path); std::filesystem::create_directories(path);
    emit::EmitDiagnostics diagnostics;
    emit::GrhSimCpuStCpp emitter(&diagnostics);
    emit::EmitOptions options;
    options.outputDir = (path / "model").string(); options.session = &fixture.session;
    options.attributes = {{"max-function-lines", std::to_string(budget)}, {"max-tu-bytes", std::to_string(tuBytes)}, {"waveform", "declared-symbols"}};
    const auto checkpoint = json(fixture.design);
    const auto stamp = tx::DesignStamp::capture(fixture.graph);
    const auto result = emitter.emit(fixture.design, options);
    if (!result.success) {
        std::string message;
        for (const auto& entry : diagnostics.messages()) message += entry.message + "\n";
        throw std::runtime_error(label + ": " + message);
    }
    require(stamp.matches(fixture.graph) && checkpoint == json(fixture.design), "Emit changed graph or partitions");
    require(sim::requireSinkSchedule(&fixture.session, fixture.graph).complete, "Emit invalidated schedule");
    inspectFunctions(path / "model", budget);
    std::ofstream driver(path / "driver.cpp");
    driver << "#include \"model/grhsim_top.hpp\"\n#include <cstdio>\nint main() {\nGrhSIM_top model;\nmodel.configure_waveform(true, \"trace.fst\");\nmodel.set_runtime_profile_enabled(true);\n";
    grhsim_test::Oracle oracle(*reference.findGraph("top"));
    for (unsigned epoch = 0; epoch < 3; ++epoch) {
        oracle.init(); driver << "model.init();\n";
        for (std::size_t step = 0; step < inputs.size(); ++step) {
            for (const auto& [name, value] : inputs[step]) driver << "model." << name << " = " << value << ";\n";
            driver << "model.eval();\n";
            const auto expected = oracle.eval(inputs[step]);
            for (const auto& port : reference.findGraph("top")->outputPorts()) {
                const auto type = reference.findGraph("top")->valueLogicalType(port.value);
                const auto value = slang::SVInt::fromString(expected.outputs.at(port.name));
                const auto width = type.leafWidth(), rows = type.width / width;
                for (int64_t row = 0; row < rows; ++row) {
                    std::string member = "model." + port.name;
                    int64_t stride = rows;
                    for (const auto size : type.shape) { stride /= size; member += '[' + std::to_string(row / stride % size) + ']'; }
                    const auto cell = value.slice((row + 1) * width - 1, row * width);
                    for (int64_t word = 0; word < (width + 63) / 64; ++word) {
                        const auto wordWidth = std::min<int64_t>(64, width - word * 64);
                        const auto mask = wordWidth == 64 ? UINT64_MAX : (uint64_t{1} << wordWidth) - 1;
                        driver << "if ((static_cast<std::uint64_t>(" << member << (word ? " >> " + std::to_string(word * 64) : "") << ") & UINT64_C(" << mask << ")) != UINT64_C(" << cell.getRawPtr()[word] << ")) { std::fprintf(stderr, \"" << label << " epoch=" << epoch << " step=" << step << " port=" << port.name << " row=" << row << " word=" << word << " mismatch\\n\"); return 1; }\n";
                    }
                }
            }
        }
    }
    driver << "model.dump_runtime_profile();\nmodel.dumpState(stdout);\nreturn 0;\n}\n";
    runMakefile(path);
}

void testChunkWriter() {
    Function function{"run"};
    function.locals = {{"std::size_t", "i"}, {"std::uint64_t", "sum"}, {"std::array<std::uint64_t,17>", "values"}};
    std::vector<Code> loop;
    for (unsigned i = 0; i < 40; ++i) loop.push_back(Code::line("@values@[@i@] += " + std::to_string(i) + ";"));
    loop.push_back(Code::line("@sum@ += @values@[@i@];"));
    function.body = {Code::block("for (@i@ = 0; @i@ < @values@.size(); ++@i@)", std::move(loop)), Code::line("result = @sum@;")};
    for (const auto budget : {12, 24, 1024}) {
        const auto result = writeFunction(function, "ChunkTest", budget);
        for (const auto& part : result.functions) require(part.lines <= std::size_t(budget), "Actual function budget exceeded");
        if (budget == 12) {
            require(result.functions.size() > 10 && result.frameDeclaration.find("values") != std::string::npos,
                "Tiny budget failed to split loops/spill cross-chunk array");
            require(result.frameDeclaration.find("i") != std::string::npos, "Loop control did not cross chunks");
        }
    }
    rejects([&] { writeFunction(function, "ChunkTest", 4); }, "Impossible budget accepted");

    Function large{"manyLocals"};
    large.locals.push_back({"std::uint64_t", "sum"});
    for (unsigned i = 0; i < 2048; ++i) {
        const auto name = "value" + std::to_string(i);
        large.locals.push_back({"std::uint64_t", name});
        large.body.push_back(Code::line("@" + name + "@ = " + std::to_string(i) + ";"));
        large.body.push_back(Code::line("@sum@ += @" + name + "@;"));
    }
    large.body.push_back(Code::line("// @unknown@sum@ and @unknown@@sum@"));
    const auto written = writeFunction(large, "ChunkTest", 128);
    require(written.functions.size() > 32, "Large local set did not split");
    std::string definitions;
    for (const auto& part : written.functions) {
        require(part.lines <= 128, "Large local set exceeded the function budget");
        require(part.definition.find("@value") == std::string::npos, "Unresolved local placeholder");
        definitions += part.definition;
    }
    require(definitions.find("// @unknownchunkFrame.sum and @unknown@chunkFrame.sum") != std::string::npos,
            "Overlapping unknown delimiters lost a local placeholder");
    Function assigned = large;
    assigned.name = "assigned"; assigned.zeroInitializeLocals = false;
    assigned.locals[0].initializer = " = 0";
    const auto assignedWritten = writeFunction(assigned, "ChunkTest", 128);
    require(assignedWritten.functions.front().definition.find("chunkFrame_assigned chunkFrame;") != std::string::npos &&
            assignedWritten.frameDeclaration.find("sum = 0;") != std::string::npos,
            "Assigned locals still cleared the frame or lost an explicit initializer");

    Function dispatch{"dispatch"};
    for (unsigned i = 0; i < 2048; ++i) {
        const auto ordinal = std::to_string(i);
        dispatch.body.push_back(Code::block("if (active[" + ordinal + "])", {
            Code::line("active[" + ordinal + "] = false;"),
            Code::line("node_" + ordinal + "();")
        }));
    }
    const auto batched = writeFunction(dispatch, "ChunkTest", 128);
    require(batched.functions.size() < 160, "Small dispatch guards created one helper per branch");
    require(batched.frameDeclaration.empty(), "A dispatcher-only split created an empty chunk frame");
    for (const auto& part : batched.functions)
        require(part.declaration.find("chunkFrame") == std::string::npos &&
                    part.definition.find("chunkFrame") == std::string::npos,
                "A dispatcher-only split still threaded a frame argument");
    for (const auto& part : batched.functions)
        require(part.lines <= 128, "Batched dispatch exceeded the actual function budget");
    dispatch.body.push_back(Code::line("accept(chunkFrame);"));
    const auto externalCall = writeFunction(dispatch, "ChunkTest", 128);
    require(std::any_of(externalCall.functions.begin(), externalCall.functions.end(), [](const auto& part) {
                return part.definition.find("accept(chunkFrame);") != std::string::npos;
            }), "Removing an unused spill argument changed a source call");
}

void testModels() {
    Fixture fixture;
    const auto clock0 = fixture.input("clock0"), clock1 = fixture.input("clock1");
    const auto enable = fixture.input("enable"), i = fixture.input("i", 2), j = fixture.input("j", 3), data = fixture.input("data", 8);
    const auto q = fixture.state("q", 8), memory = fixture.state("memory", 64, {2, 4});
    const auto latch = fixture.state("latch", 8); fixture.graph.setOpKind(latch, OperationKind::kLatch);
    const auto mask = fixture.constant(8, "8'h5a");
    const auto qRead = fixture.read(q);
    const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, qRead});
    fixture.write(OperationKind::kRegisterWritePort, q, {enable, data, clock0});
    fixture.write(OperationKind::kRegisterWriteMaskPort, q, {enable, sum, mask, clock1});
    fixture.write(OperationKind::kRegisterFillPort, memory, {enable, sum, clock0});
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {enable, i, j, qRead, mask, clock1});
    const auto addressed = fixture.read(memory, {i, j});
    fixture.write(OperationKind::kLatchWritePort, latch, {enable, addressed, mask});
    fixture.graph.bindOutputPort("q", qRead);
    fixture.graph.bindOutputPort("memory", fixture.read(memory));
    fixture.graph.bindOutputPort("latch", fixture.read(latch));
    fixture.graph.bindOutputPort("addressed", addressed);
    fixture.graph.bindOutputPort("partial", fixture.read(memory, {i}));
    fixture.convert(); const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=8"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-node-work=2", "-max-supernode-work=4"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned step = 0; step < 48; ++step) inputs.push_back({{"clock0", step / 2 & 1}, {"clock1", step / 3 & 1},
        {"enable", step % 7 != 0}, {"i", step / 4 & 3}, {"j", step / 5 & 7}, {"data", step * 17 & 255}});
    std::ofstream make(artifact("Makefile")); make << ".PHONY: run\nrun:";
    std::vector<std::string> labels;
    for (const auto budget : {16, 48, 1024}) for (const auto tu : {4096, 1048576}) {
        const auto label = "phases-b" + std::to_string(budget) + "-tu" + std::to_string(tu);
        writeCase(fixture, reference, inputs, label, budget, tu); labels.push_back(label); make << ' ' << label;
    }
    make << '\n';
    for (const auto& label : labels) make << ".PHONY: " << label << '\n' << label << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    // Missing/stale schedules and impossible options must leave no output.
    emit::GrhSimCpuStCpp emitter;
    emit::EmitOptions options; options.outputDir = artifact("invalid").string();
    std::filesystem::remove_all(*options.outputDir);
    require(!emitter.emit(fixture.design, options).success && !std::filesystem::exists(*options.outputDir), "Missing schedule wrote files");
    options.session = &fixture.session; options.attributes["max-function-lines"] = "4";
    require(!emitter.emit(fixture.design, options).success && !std::filesystem::exists(*options.outputDir), "Impossible budget wrote files");
    options.attributes.clear(); fixture.graph.setMetadata("test.invalidates", true);
    require(!emitter.emit(fixture.design, options).success && !std::filesystem::exists(*options.outputDir), "Stale schedule wrote files");
}

void testMultipleNonsinkActivation() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock0 = fixture.input("clock0"), clock1 = fixture.input("clock1");
    const auto enable = fixture.input("enable");
    const auto data0 = fixture.input("data0", 8), data1 = fixture.input("data1", 8);
    const auto source0 = fixture.state("source0", 8), source1 = fixture.state("source1", 8);
    const auto sink0a = fixture.state("sink0a", 8), sink0b = fixture.state("sink0b", 8);
    const auto sink1a = fixture.state("sink1a", 8), sink1b = fixture.state("sink1b", 8);
    const auto sum0 = fixture.compute(OperationKind::kAdd, Fixture::type(8), {fixture.read(source0), data0});
    const auto sum1 = fixture.compute(OperationKind::kAdd, Fixture::type(8), {fixture.read(source1), data1});
    fixture.write(OperationKind::kRegisterWritePort, source0, {enable, data0, clock0});
    fixture.write(OperationKind::kRegisterWritePort, source1, {enable, data1, clock1});
    fixture.write(OperationKind::kRegisterWritePort, sink0a, {enable, sum0, clock0});
    fixture.write(OperationKind::kRegisterWritePort, sink0b, {enable, sum0, clock0});
    fixture.write(OperationKind::kRegisterWritePort, sink1a, {enable, sum1, clock1});
    fixture.write(OperationKind::kRegisterWritePort, sink1b, {enable, sum1, clock1});
    graph.bindOutputPort("sum0", sum0); graph.bindOutputPort("sum1", sum1);
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-node-work=1", "-max-supernode-work=1"});
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    std::set<Owner> nonsinks;
    for (const auto op : graph.operations()) {
        const auto path = owner(graph, op);
        if (path.size() >= 4 && path[0] == "general" && path[1] == "nonsink") nonsinks.insert(path);
    }
    if (nonsinks.size() < 2) {
        std::string detail = "Multi-branch activation fixture did not create multiple nonsink partitions (" +
            std::to_string(nonsinks.size()) + "):";
        for (const auto& path : nonsinks) { detail += " ["; for (const auto& item : path) detail += item + "/"; detail += "]"; }
        detail += " all:";
        std::set<Owner> all;
        for (const auto op : graph.operations()) all.insert(owner(graph, op));
        for (const auto& path : all) { detail += " ["; for (const auto& item : path) detail += item + "/"; detail += "]"; }
        throw std::runtime_error(detail);
    }
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); model.enable = true; model.data0 = 3; model.data1 = 5; model.clock0 = false; model.clock1 = false;
    model.eval(); assert(model.sum0 == 3 && model.sum1 == 5);
    model.clock0 = true; model.eval(); assert(model.sum0 == 6 && model.sum1 == 5);
    model.clock0 = false; model.clock1 = true; model.eval(); assert(model.sum0 == 6 && model.sum1 == 10);
    model.clock1 = false; model.data0 = 7; model.data1 = 11; model.eval();
    assert(model.sum0 == 10 && model.sum1 == 16);
    model.clock0 = true; model.eval(); assert(model.sum0 == 14 && model.sum1 == 16);
    model.clock0 = false; model.clock1 = true; model.eval(); assert(model.sum0 == 14 && model.sum1 == 22);
}
)CPP";
    for (const auto budget : {16u, 1024u})
        manualCase(fixture, "multiple-nonsink-b" + std::to_string(budget), budget, "", body, false);
}

void testChunkExecution() {
    Fixture fixture;
    fixture.graph.bindOutputPort("one", fixture.constant(1, "1'b1"));
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    Function function{"run", "unsigned stop", "std::uint64_t"};
    function.locals = {{"unsigned", "stop", "{}", true}, {"std::size_t", "i"}, {"std::uint64_t", "sum"},
                       {"unsigned", "calls"}, {"std::array<std::uint64_t,17>", "values"}};
    std::vector<Code> inner;
    inner.push_back(Code::line("@sum@ += @i@;"));
    inner.push_back(Code::block("if (@i@ == @stop@)", {Code::line("return @sum@;", true)}));
    for (unsigned i = 0; i < 40; ++i) inner.push_back(Code::line("@values@[@i@] += " + std::to_string(i) + ";"));
    inner.push_back(Code::line("++@calls@;"));
    inner.push_back(Code::line("record = @calls@;"));
    inner.push_back(Code::line("last = @values@[@i@];"));
    function.body = {Code::block("for (@i@ = 0; @i@ < @values@.size(); ++@i@)", std::move(inner)),
                     Code::line("return @sum@;", true)};
    for (const auto budget : {16, 1024}) {
        const auto written = writeFunction(function, "ChunkTest", budget);
        std::string support = "class ChunkTest { public: unsigned record=0; std::uint64_t last=0;\n" + written.frameDeclaration;
        for (const auto& part : written.functions) support += part.declaration;
        support += "};\n";
        for (const auto& part : written.functions) support += part.definition;
        manualCase(fixture, "chunk-control-b" + std::to_string(budget), budget, support,
            "for (unsigned stop=0; stop<20; ++stop) { ChunkTest test; const auto result=test.run(stop); "
            "const auto n=std::min(stop,16u); assert(result==n*(n+1)/2); "
            "assert(test.record==std::min(stop,17u)); if(stop) assert(test.last==780); }");
    }
    // A single guarded statement can fit while its private declarations do
    // not. Repeated helper extraction must terminate and preserve initializers.
    Function declarations{"run"};
    std::string expression;
    for (unsigned i = 0; i < 20; ++i) {
        const auto name = "x" + std::to_string(i);
        declarations.locals.push_back({"std::uint64_t", name, "{" + std::to_string(i + 1) + "}"});
        expression += (expression.empty() ? "" : " + ") + std::string("@") + name + "@";
    }
    declarations.body = {Code::block("if (result == 0)", {Code::line("result = " + expression + ";")})};
    for (const auto budget : {12, 1024}) {
        const auto written = writeFunction(declarations, "ChunkTest", budget);
        std::string support = "class ChunkTest { public: std::uint64_t result=0;\n" + written.frameDeclaration;
        for (const auto& part : written.functions) {
            require(part.lines <= std::size_t(budget), "Declaration spill exceeded actual line budget");
            support += part.declaration;
        }
        if (budget == 12) require(written.frameDeclaration.find("x19") != std::string::npos,
                                 "Indivisible statement did not spill private locals");
        support += "};\n";
        for (const auto& part : written.functions) support += part.definition;
        manualCase(fixture, "chunk-declarations-b" + std::to_string(budget), budget, support,
                   "ChunkTest test; test.run(); assert(test.result == 210); test.run(); assert(test.result == 210);");
    }
    // Inline phases contain many independently oversized guarded scopes.
    // Splitting them together must preserve each initializer and early return.
    Function scopes{"run", "unsigned parity, unsigned stop", "std::uint64_t"};
    scopes.locals = {{"unsigned", "parity", "{}", true}, {"unsigned", "stop", "{}", true},
                     {"std::uint64_t", "sum", "{5}"}};
    for (unsigned group = 0; group < 128; ++group) {
        const auto name = "value" + std::to_string(group);
        scopes.locals.push_back({"std::uint64_t", name, "{" + std::to_string(group + 1) + "}"});
        std::vector<Code> body;
        if (group == 64) body.push_back(Code::block("if (@stop@ == 64)",
                                                   {Code::line("return @sum@;", true)}));
        for (unsigned step = 0; step < 96; ++step) {
            body.push_back(Code::line("@sum@ += @" + name + "@;"));
            body.push_back(Code::line("++@" + name + "@;"));
        }
        scopes.body.push_back(Code::block("if (@parity@ == " + std::to_string(group % 2) + ")", std::move(body)));
    }
    scopes.body.push_back(Code::line("return @sum@;", true));
    for (const auto budget : {64, 1024}) {
        const auto written = writeFunction(scopes, "ChunkTest", budget);
        std::string support = "class ChunkTest { public:\n" + written.frameDeclaration;
        for (const auto& part : written.functions) {
            require(part.lines <= std::size_t(budget), "Guarded scopes exceeded actual line budget");
            support += part.declaration;
        }
        support += "};\n";
        for (const auto& part : written.functions) support += part.definition;
        manualCase(fixture, "chunk-many-scopes-b" + std::to_string(budget), budget, support, R"CPP(
ChunkTest test;
for (unsigned parity = 0; parity < 2; ++parity) for (unsigned stop = 0; stop <= 128; ++stop) {
    std::uint64_t expected = 5;
    for (unsigned group = parity; group < 128; group += 2) {
        if (group == 64 && stop == 64) break;
        expected += 96 * (group + 1) + 95 * 96 / 2;
    }
    assert(test.run(parity, stop) == expected);
}
)CPP");
    }
}

void testLargeStoreConstruction() {
    const auto path = artifact("large-store-construction");
    std::filesystem::create_directories(path);
    std::vector<Local> fields{{"std::string", "first"}};
    for (unsigned i = 0; i < 70000; ++i)
        fields.push_back({"std::uint8_t", "v" + std::to_string(i)});
    fields.push_back({"std::string", "last"});
    fields.push_back({"std::array<std::string,2>", "strings"});
    fields.push_back({"unsigned", "nonzero", "{42}"});
    const auto indexed = writeValueLayout("Indexed", fields);
    require(indexed.physicalFields.size() < 10 && indexed.declaration.size() < 1024,
            "Large frame layout retained one C++ declaration per value");
    const auto direct = writeDirectValueLayout("Direct", fields);
    require(direct.physicalFields.size() == fields.size() &&
                direct.references.at("v69999") == "v69999" &&
                direct.declaration.find("std::uint8_t v69999;") != std::string::npos,
            "Direct store layout did not retain named members");
    const auto ref = [&](const std::string& name) { return "store->" + indexed.references.at(name); };
    std::ofstream driver(path / "driver.cpp");
    driver << "#include <array>\n#include <cassert>\n#include <cstdint>\n#include <memory>\n#include <string>\n"
        << writeValueStruct("Store", fields)
        << indexed.declaration
        << "struct Model { Store persistent{}; Store chunkFrame{}; };\n"
        << "int main() { auto model=std::make_unique<Model>();\n"
        << "for (auto store : {&model->persistent, &model->chunkFrame}) {\n"
        << "assert(store->first.data() && store->last.data() && store->strings[1].data());\n"
        << "assert(!store->v0 && !store->v65535 && !store->v69999 && store->nonzero == 42);\n"
        << "store->last = std::string(128, 'x'); store->strings[1] = store->last;\n"
        << "store->last.clear(); *store = {};\n"
        << "assert(store->last.empty() && store->strings[1].empty() && store->nonzero == 42);\n"
        << "}\n" << "auto indexed=std::make_unique<Indexed>(); auto *store=indexed.get();\n"
        << "assert(" << ref("first") << ".data() && " << ref("last") << ".data() && " << ref("strings") << "[1].data());\n"
        << "assert(!" << ref("v0") << " && !" << ref("v65535") << " && !" << ref("v69999") << " && " << ref("nonzero") << " == 42);\n"
        << ref("last") << " = std::string(128, 'x'); " << ref("strings") << "[1] = " << ref("last") << "; *store = {};\n"
        << "assert(" << ref("last") << ".empty() && " << ref("strings") << "[1].empty() && " << ref("nonzero") << " == 42);\n"
        << "}\n";
    std::ofstream run(path / "run.mk");
    run << "CXX := clang++\n.PHONY: run\nrun: driver\n\t./driver\n"
        << "driver: driver.cpp\n\t$(CXX) -std=c++20 -O1 $< -o $@\n";
    std::ofstream make(artifact("Makefile"), std::ios::app);
    make << "\n.PHONY: large-store-construction\nrun: large-store-construction\n"
        << "large-store-construction:\n\t$(MAKE) -C large-store-construction -f run.mk run\n";
}

void testNamedModelStores() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), en = fixture.input("en"), data = fixture.input("data", 8);
    const auto address = fixture.constant(1, "0");
    const auto one = fixture.constant(8, "1");
    auto sum = fixture.constant(32, "0");
    for (unsigned i = 0; i < 1100; ++i) {
        const auto scalar = fixture.state("scalar" + std::to_string(i), 8);
        const auto memory = fixture.state("memory" + std::to_string(i), 1024, {128});
        fixture.write(OperationKind::kRegisterWritePort, scalar, {en, data, clk});
        fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {en, address, data, clk});
        const auto first = fixture.read(scalar), second = fixture.read(memory, {address});
        const auto a = fixture.compute(OperationKind::kAdd, Fixture::type(8), {first, one});
        const auto b = fixture.compute(OperationKind::kAdd, Fixture::type(8), {second, one});
        const auto wide = [&](grh::ValueId value) {
            return fixture.compute(OperationKind::kConcat, Fixture::type(32), {fixture.constant(24, "0"), value});
        };
        sum = fixture.compute(OperationKind::kAdd, Fixture::type(32), {sum, wide(a)});
        sum = fixture.compute(OperationKind::kAdd, Fixture::type(32), {sum, wide(b)});
    }
    fixture.graph.bindOutputPort("sum", sum);
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split(); fixture.partition();
    manualCase(fixture, "named-model-stores", 1024, "", R"(
for (unsigned epoch = 0; epoch < 2; ++epoch) {
    model.init(); model.clk = 0; model.en = 1; model.data = 7; model.eval();
    assert(model.sum == 2200);
    model.clk = 1; model.eval(); assert(model.sum == 17600);
    model.clk = 0; model.en = 0; model.data = 13; model.eval();
    model.clk = 1; model.eval(); assert(model.sum == 17600);
}
)", false);
    const auto header = readFile(artifact("named-model-stores") / "model/grhsim_top.hpp");
    for (const auto store : {"smallSinkStore", "largeSinkStore", "inputStore", "nonSinkBoundaryStore",
                             "smallSinkEnableBoundaryStore", "smallSinkPayloadBoundaryStore",
                             "largeSinkEnableBoundaryStore", "largeSinkPayloadBoundaryStore", "prevEventStore",
                             "eventActStore", "timeslotTriggerStore", "sampledStore", "onceStore",
                             "largeBeforeStore", "largeWholeBeforeStore", "largeWholeDirtyStore"}) {
        const auto begin = header.find(std::string("struct ") + store + "Type {");
        if (begin == std::string::npos) continue;
        const auto end = header.find("    };", begin);
        require(end != std::string::npos && header.substr(begin, end - begin).find("cells_") == std::string::npos,
                std::string("Named store was emitted through cells_: ") + store);
    }
    require(header.find("std::uint8_t s4;") != std::string::npos,
            "Model state store did not retain direct semantic fields");
    require(header.find("pGeneral_chunk") == std::string::npos &&
                header.find("chunkFrame_pGeneral") == std::string::npos,
            "pGeneral was split into emitter chunks instead of ordered helper dispatch");
}

void testActivityBatchOrder() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable");
    const auto input = fixture.input("input", 8), one = fixture.constant(8, "1");
    auto previous = input;
    std::vector<grh::ValueId> values;
    for (unsigned i = 0; i < 400; ++i) {
        const auto value = fixture.compute(OperationKind::kAdd, Fixture::type(8), {previous, one});
        const auto state = fixture.state("state" + std::to_string(i), 8);
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, value, clk});
        fixture.graph.bindOutputPort("state" + std::to_string(i), fixture.read(state));
        if (i) {
            const auto peer = fixture.state("peer" + std::to_string(i), 8);
            fixture.write(OperationKind::kRegisterWritePort, peer, {enable, previous, clk});
            fixture.graph.bindOutputPort("peer" + std::to_string(i), fixture.read(peer));
        }
        values.push_back(value); previous = value;
    }
    fixture.graph.bindOutputPort("result", previous);
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-node-work=1", "-max-supernode-work=1", "-clone-shared=0"});
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    std::set<Owner> owners;
    for (const auto value : values) {
        auto path = owner(fixture.graph, fixture.graph.valueDef(value));
        if (path.size() != 4 || path[0] != "general" || path[1] != "nonsink") continue;
        path.resize(3); owners.insert(std::move(path));
    }
    require(owners.size() > 384, "Activity chain did not cross two dispatch batch boundaries: " +
            std::to_string(owners.size()));
    std::string body = "for (unsigned epoch=0; epoch<2; ++epoch) { model.init();\n"
        "for (unsigned step=0; step<12; ++step) { model.clk=step&1; model.enable=step%3!=0;\n"
        "model.input=step*17; const auto rounds=model.perf_counters().totalRoundCount; model.eval();\n"
        "assert(model.perf_counters().totalRoundCount==rounds+1+(model.clk && model.enable));\n"
        "assert(model.result==static_cast<std::uint8_t>(model.input+400));\n";
    for (unsigned i = 0; i < 400; ++i) {
        const auto member = "model.state" + std::to_string(i);
        body += "if (!step) assert(" + member + "==0); else if (model.clk && model.enable) assert(" +
            member + "==static_cast<std::uint8_t>(model.input+" + std::to_string(i + 1) + "));\n";
    }
    body += "}}\n";
    for (const auto budget : {16u, 1024u}) {
        const auto label = "activity-batch-order-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "", body, false, {{"max-tu-bytes", "1048576"}});
        const auto header = readFile(artifact(label) / "model/grhsim_top.hpp");
        require(header.find("pGeneral_active_batch2(") != std::string::npos,
                "Activity chain did not emit three ordered batches");
        require(header.find("pGeneral_chunk") == std::string::npos &&
                    header.find("chunkFrame_pGeneral") == std::string::npos,
                "Ordered activity dispatch created phase chunks or a phase frame");
    }
}

void testEffectsAndCalls() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), en = fixture.input("en"), input = fixture.input("input", 8);
    const auto one = fixture.constant(1, "1'b1");
    const auto wide = fixture.compute(OperationKind::kReplicate, Fixture::type(128), {input});
    fixture.graph.setAttr(fixture.graph.valueDef(wide), "rep", int64_t{16});
    const auto text = fixture.compute(OperationKind::kConstant, {grh::ValueType::String, 1, false, grh::LogicDomain::TwoState, {}}, {});
    fixture.graph.setAttr(fixture.graph.valueDef(text), "constValue", std::string("wide=%0h bit=%0d @tick"));
    const auto imported = fixture.graph.createOperation(OperationKind::kDpicImport, fixture.graph.internSymbol("test_dpi"));
    fixture.graph.setAttr(imported, "argsName", std::vector<std::string>{"wide", "bit", "out", "io", "text"});
    fixture.graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input", "input", "output", "inout", "input"});
    fixture.graph.setAttr(imported, "argsWidth", std::vector<int64_t>{128, 1, 1, 8, 1});
    fixture.graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false, false, false, false});
    fixture.graph.setAttr(imported, "argsType", std::vector<std::string>{"logic", "bit", "bit", "byte", "string"});
    fixture.graph.setAttr(imported, "hasReturn", true); fixture.graph.setAttr(imported, "returnWidth", int64_t{1});
    fixture.graph.setAttr(imported, "returnSigned", false);
    const auto call = fixture.graph.createOperation(OperationKind::kDpicCall, fixture.graph.internSymbol("call"));
    for (const auto value : {en, wide, one, text, input, clk}) fixture.graph.addOperand(call, value);
    fixture.graph.setAttr(call, "targetImportSymbol", std::string("test_dpi"));
    fixture.graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    const auto returned = fixture.graph.createValue(Fixture::type(1)), out = fixture.graph.createValue(Fixture::type(1)), io = fixture.graph.createValue(Fixture::type(8));
    for (const auto value : {returned, out, io}) fixture.graph.addResult(call, value);
    fixture.graph.bindOutputPort("returned", returned); fixture.graph.bindOutputPort("out", out); fixture.graph.bindOutputPort("io", io);
    const auto random = fixture.compute(OperationKind::kSystemFunction, Fixture::type(32), {});
    fixture.graph.setAttr(fixture.graph.valueDef(random), "name", std::string("random"));
    fixture.graph.bindOutputPort("random", random);
    fixture.task("display", {en, text, wide, returned, clk}, {"posedge"});
    fixture.task("strobe", {en, text, wide, returned, clk}, {"posedge"});
    fixture.task("monitor", {one, input});
    fixture.convert(); fixture.split(); fixture.partition();
    const std::string support = R"CPP(
static unsigned calls = 0;
extern "C" std::uint8_t test_dpi(const unsigned _BitInt(128)& wide, std::uint8_t bit, std::uint8_t *out, std::uint8_t *io, const char *text) {
    ++calls;
    assert(bit == 1 && std::string(text) == "wide=%0h bit=%0d @tick");
    assert(static_cast<std::uint8_t>(wide) == *io);
    *out = 2; *io = static_cast<std::uint8_t>(*io + 1); return 3;
}
static std::uint64_t next(std::uint64_t& state) {
    auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
)CPP";
    const std::string body = R"CPP(
model.configure_waveform(true, "trace.fst");
model.set_runtime_profile_enabled(true);
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); calls = 0;
    std::uint64_t rng = UINT64_C(0x6a09e667f3bcc909);
    unsigned expected = 0;
    for (unsigned step = 0; step < 12; ++step) {
        model.clk = step & 1; model.en = step % 3 != 0; model.input = step * 7;
        model.eval();
        expected += (step & 1) && model.en;
        assert(calls == expected);
        assert(model.random == static_cast<std::uint32_t>(next(rng)));
        if (expected) { assert(model.returned && !model.out); }
    }
}
model.dump_runtime_profile();
model.dumpState(stdout);
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "effects-b" + std::to_string(budget), budget, support, body);
}

void testDpiTriggerActivation() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), a = fixture.input("a", 8), d = fixture.input("d", 8);
    const auto one = fixture.constant(1, "1'b1"), mask = fixture.constant(8, "8'hff");
    const auto call = [&](std::string name, grh::ValueId input, bool clocked) {
        const auto imported = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
        graph.setAttr(imported, "argsName", std::vector<std::string>{"input"});
        graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input"});
        graph.setAttr(imported, "argsWidth", std::vector<int64_t>{8});
        graph.setAttr(imported, "argsSigned", std::vector<bool>{false});
        graph.setAttr(imported, "hasReturn", true); graph.setAttr(imported, "returnWidth", int64_t{8});
        const auto op = graph.createOperation(OperationKind::kDpicCall);
        graph.setAttr(op, "targetImportSymbol", name);
        graph.addOperand(op, one); graph.addOperand(op, input);
        if (clocked) { graph.addOperand(op, clk); graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"}); }
        const auto result = graph.createValue(Fixture::type(8)); graph.addResult(op, result);
        return result;
    };
    const auto y = call("trigger_inc", a, false), z = call("trigger_dbl", d, true);
    const auto q = fixture.state("q", 8), q2 = fixture.state("q2", 8);
    graph.setOpKind(q, OperationKind::kLatch); graph.setOpKind(q2, OperationKind::kLatch);
    fixture.write(OperationKind::kLatchWritePort, q, {one, y, mask});
    fixture.write(OperationKind::kLatchWritePort, q2, {one, z, mask});
    graph.bindOutputPort("o", fixture.read(q)); graph.bindOutputPort("o2", fixture.read(q2));
    fixture.convert(); fixture.split(); fixture.partition();
    const std::string support = R"CPP(
static unsigned incCalls = 0, dblCalls = 0;
extern "C" std::uint8_t trigger_inc(std::uint8_t x) { ++incCalls; return x + 1; }
extern "C" std::uint8_t trigger_dbl(std::uint8_t x) { ++dblCalls; return x * 2; }
)CPP";
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); incCalls = dblCalls = 0;
    model.eval(); assert(incCalls == 1 && dblCalls == 0 && model.o == 1);
    model.a = 1; model.eval(); assert(incCalls == 2 && model.o == 2);
    model.d = 3; model.eval(); assert(incCalls == 3 && dblCalls == 0);
    model.clk = true; model.eval(); assert(incCalls == 4 && dblCalls == 1 && model.o2 == 6);
    model.clk = false; model.eval(); assert(incCalls == 4 && dblCalls == 1);
    model.eval(); assert(incCalls == 4 && dblCalls == 1);
    model.d = 4; model.eval(); assert(incCalls == 5 && dblCalls == 1);
    model.clk = true; model.eval(); assert(incCalls == 6 && dblCalls == 2 && model.o2 == 8);
    model.a = 255; model.eval(); assert(incCalls == 7 && dblCalls == 2 && model.o == 0);
    model.eval(); assert(incCalls == 7 && dblCalls == 2);
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "dpi-trigger-b" + std::to_string(budget), budget, support, body);
}

void testDpiIntegralAliases() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), input = fixture.input("input", 32), io = fixture.input("io", 16);
    const auto signedType = [](int width) { auto type = Fixture::type(width); type.isSigned = true; return type; };
    fixture.graph.setValueLogicalType(io, signedType(16));
    const auto condition = fixture.input("condition", 32);
    const auto imported = fixture.graph.createOperation(OperationKind::kDpicImport, fixture.graph.internSymbol("test_integral_dpi"));
    fixture.graph.setAttr(imported, "argsName", std::vector<std::string>{"a", "io", "b"});
    fixture.graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input", "inout", "output"});
    fixture.graph.setAttr(imported, "argsWidth", std::vector<int64_t>{32, 16, 8});
    fixture.graph.setAttr(imported, "argsSigned", std::vector<bool>{true, true, true});
    fixture.graph.setAttr(imported, "argsType", std::vector<std::string>{"int", "shortint", "byte"});
    fixture.graph.setAttr(imported, "hasReturn", true);
    fixture.graph.setAttr(imported, "returnType", std::string("longint"));
    fixture.graph.setAttr(imported, "returnWidth", int64_t{64});
    fixture.graph.setAttr(imported, "returnSigned", true);
    const auto call = fixture.graph.createOperation(OperationKind::kDpicCall);
    for (const auto value : {condition, input, io, clk}) fixture.graph.addOperand(call, value);
    fixture.graph.setAttr(call, "targetImportSymbol", std::string("test_integral_dpi"));
    fixture.graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    for (const auto& [name, width] : {std::pair{"returned", 64}, {"out", 8}, {"changed", 16}}) {
        const auto value = fixture.graph.createValue(signedType(width));
        fixture.graph.addResult(call, value); fixture.graph.bindOutputPort(name, value);
    }
    fixture.convert(); fixture.split(); fixture.partition();
    const std::string support = R"CPP(
static unsigned calls = 0;
extern "C" std::int64_t test_integral_dpi(std::int32_t a, std::int16_t *io, std::int8_t *b) {
    ++calls; assert(a == -1234567 && *io == -1234);
    *io = -2345; *b = -123; return -INT64_C(12345678901234);
}
)CPP";
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); calls = 0;
    model.input = -1234567; model.io = -1234; model.condition = 2;
    model.clk = 0; model.eval(); assert(calls == 0);
    model.clk = 1; model.eval(); assert(calls == 1);
    assert(model.returned == -INT64_C(12345678901234));
    assert(model.out == -123 && model.changed == -2345);
    model.eval(); assert(calls == 1);
    model.clk = 0; model.eval();
    model.condition = 0; model.clk = 1; model.eval(); assert(calls == 1);
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "dpi-aliases-b" + std::to_string(budget), budget, support, body, false);
}

void testDpiMixedTypes() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable"), wide = fixture.input("wide", 128);
    const grh::LogicalType realType{grh::ValueType::Real, 64, false, grh::LogicDomain::TwoState, {}};
    const grh::LogicalType stringType{grh::ValueType::String, 0, false, grh::LogicDomain::TwoState, {}};
    const auto real = graph.createValue(realType), text = graph.createValue(stringType);
    graph.bindInputPort("real", real); graph.bindInputPort("text", text);
    const auto imported = [&](std::string name, std::vector<std::string> directions,
                              std::vector<std::string> kinds, std::vector<int64_t> widths,
                              std::optional<grh::LogicalType> returned) {
        const auto target = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
        std::vector<std::string> names;
        for (std::size_t i = 0; i < kinds.size(); ++i) names.push_back("arg" + std::to_string(i));
        graph.setAttr(target, "argsName", names); graph.setAttr(target, "argsDirection", directions);
        graph.setAttr(target, "argsType", kinds); graph.setAttr(target, "argsWidth", widths);
        graph.setAttr(target, "argsSigned", std::vector<bool>(kinds.size(), false));
        graph.setAttr(target, "hasReturn", returned.has_value());
        if (returned) {
            graph.setAttr(target, "returnType", std::string(returned->kind == grh::ValueType::Real ? "real" : "string"));
            graph.setAttr(target, "returnWidth", int64_t{returned->width});
        }
        const auto call = graph.createOperation(OperationKind::kDpicCall);
        graph.setAttr(call, "targetImportSymbol", name);
        graph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(call, enable);
        return call;
    };
    const auto call = imported("mixed_void", {"output", "input", "inout", "input", "output", "output", "inout", "inout"},
        {"logic", "real", "logic", "string", "real", "string", "real", "string"}, {128, 64, 128, 0, 64, 0, 64, 0}, {});
    for (const auto input : {real, text, wide, real, text, clk}) graph.addOperand(call, input);
    for (const auto& [name, type] : std::vector<std::pair<std::string, grh::LogicalType>>{
            {"wide_out", Fixture::type(128)}, {"real_out", realType}, {"text_out", stringType},
            {"wide_io", Fixture::type(128)}, {"real_io", realType}, {"text_io", stringType}}) {
        const auto value = graph.createValue(type); graph.addResult(call, value); graph.bindOutputPort(name, value);
    }
    const auto realCall = imported("mixed_real", {"input"}, {"real"}, {64}, realType);
    graph.addOperand(realCall, real); graph.addOperand(realCall, clk);
    const auto realResult = graph.createValue(realType); graph.addResult(realCall, realResult); graph.bindOutputPort("real_return", realResult);
    const auto stringCall = imported("mixed_string", {"input"}, {"string"}, {0}, stringType);
    graph.addOperand(stringCall, text); graph.addOperand(stringCall, clk);
    const auto stringResult = graph.createValue(stringType); graph.addResult(stringCall, stringResult); graph.bindOutputPort("text_return", stringResult);
    fixture.convert(); fixture.split(); fixture.partition();
    // Preserve the legacy C++ ABI for string returns/output parameters. Input
    // strings are const char*, while output/inout strings use std::string*.
    const std::string support = R"CPP(
static std::string callOrder;
extern "C" void mixed_void(unsigned _BitInt(128)* out, double real, unsigned _BitInt(128)* io,
    const char* text, double* realOut, std::string* textOut, double* realIo, std::string* textIo) {
    callOrder += 'v'; assert(*realIo == real && *textIo == text);
    *out = *io ^ (static_cast<unsigned _BitInt(128)>(1) << 100); *io += 7;
    *realOut = real * 2; *realIo += 0.5; *textOut = std::string(text) + "/out"; *textIo += "/io";
}
extern "C" double mixed_real(double real) { callOrder += 'r'; return real - 0.25; }
extern "C" std::string mixed_string(const char* text) { callOrder += 's'; return std::string(text) + "/return"; }
)CPP";
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); callOrder.clear();
    model.wide = (static_cast<unsigned _BitInt(128)>(123) << 80) | 19;
    model.real = -2.5; model.text = "mixed"; model.enable = true;
    model.eval(); assert(callOrder.empty());
    model.clk = true; model.eval(); assert(callOrder == "vrs");
    assert(model.wide_out == (model.wide ^ (static_cast<unsigned _BitInt(128)>(1) << 100)));
    assert(model.wide_io == model.wide + 7 && model.real_out == -5.0 && model.real_io == -2.0);
    assert(model.text_out == "mixed/out" && model.text_io == "mixed/io");
    assert(model.real_return == -2.75 && model.text_return == "mixed/return");
    model.eval(); assert(callOrder == "vrs");
    model.clk = false; model.eval(); model.enable = false; model.clk = true; model.eval();
    assert(callOrder == "vrs" && model.text_return == "mixed/return");
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "dpi-mixed-b" + std::to_string(budget), budget, support, body, false);
}

void testSinkRunSharedCones() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), enableA = fixture.input("enable_a"), enableB = fixture.input("enable_b");
    const auto data = fixture.input("data", 8), three = fixture.constant(8, "8'h3"), mask = fixture.constant(8, "8'h55");
    for (const bool array : {false, true}) {
        const auto output = graph.createOperation(OperationKind::kArrayCreate);
        for (unsigned i = 0; i < 48; ++i) {
            const auto state = fixture.state(std::string(array ? "large" : "small") + std::to_string(i), array ? 32 : 8,
                                             array ? std::vector<int64_t>{4} : std::vector<int64_t>{});
            // Distinct init values prevent legal whole-state merging from
            // collapsing the intended writer batches before cone sharing.
            grh::setStateInitSpec(graph, state, {{array ? grh::InitKind::Fill : grh::InitKind::Constant,
                                               "8'd" + std::to_string(i)}});
            const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, three});
            const auto payload = fixture.compute(OperationKind::kXor, Fixture::type(8), {sum, mask});
            fixture.write(array ? OperationKind::kRegisterFillPort : OperationKind::kRegisterWritePort,
                          state, {i < 24 ? enableA : enableB, payload, clk});
            graph.addOperand(output, fixture.read(state));
        }
        const auto result = graph.createValue(Fixture::type(array ? 1536 : 384,
            array ? std::vector<int64_t>{48, 4} : std::vector<int64_t>{48}));
        graph.addResult(output, result); graph.bindOutputPort(array ? "large" : "small", result);
    }
    fixture.convert();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=2"});
    // Each original payload is private. Coarse-owner CSE makes it shared
    // before final clock/guard groups, as in the production pipeline.
    run(fixture.design, "grhsim-simplify", fixture.session);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == 4 && schedule.largeCommitRuns.size() == 2,
            "Shared-cone test did not form the intended small/large ordered runs");
    for (const auto& run : schedule.generalRuns)
        require(run.orderedWriters.size() == 24, "Shared-cone test lost contiguous writer batches: " +
                std::to_string(run.orderedWriters.size()));
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); std::array<std::uint8_t,48> expected{};
    for (unsigned i = 0; i < expected.size(); ++i) expected[i] = i;
    for (unsigned step = 0; step < 64; ++step) {
        model.clk = step & 1; model.enable_a = step % 3 != 0; model.enable_b = step % 5 != 0; model.data = step * 17;
        model.eval();
        const auto payload = static_cast<std::uint8_t>(static_cast<std::uint8_t>(model.data + 3) ^ 0x55);
        for (unsigned i = 0; i < 48; ++i) {
            if (model.clk && (i < 24 ? model.enable_a : model.enable_b)) expected[i] = payload;
            assert(model.small[i] == expected[i]);
            for (unsigned j = 0; j < 4; ++j) assert(model.large[i][j] == expected[i]);
        }
    }
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "sink-shared-b" + std::to_string(budget), budget, "", body, false);
}

void testSinkPrivatePayloads() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), enableA = fixture.input("enable_a"), enableB = fixture.input("enable_b");
    const auto data = fixture.input("data", 8), one = fixture.constant(8, "8'd1");
    const auto maskA = fixture.constant(8, "8'h35"), maskB = fixture.constant(8, "8'ha7");
    std::vector<std::string> payloadNames;
    for (const bool array : {false, true}) {
        for (const bool second : {false, true}) {
            const std::string label = std::string(array ? "large_" : "small_") + (second ? "b" : "a");
            const auto state = fixture.state(label, array ? 8192 : 8, array ? std::vector<int64_t>{1024} : std::vector<int64_t>{});
            grh::setStateInitSpec(graph, state, {{array ? grh::InitKind::Fill : grh::InitKind::Constant, second ? "8'd9" : "8'd3"}});
            const auto shared = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, one});
            const auto payload = fixture.compute(OperationKind::kXor, Fixture::type(8), {shared, second ? maskB : maskA});
            payloadNames.emplace_back(graph.symbolText(graph.operationSymbol(graph.valueDef(payload))));
            fixture.write(array ? OperationKind::kRegisterFillPort : OperationKind::kRegisterWritePort, state, {second ? enableB : enableA, payload, clk});
            graph.bindOutputPort(label, array ? fixture.read(state, {fixture.constant(10, "0")}) : fixture.read(state));
        }
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session);
    run(fixture.design, "grhsim-simplify", fixture.session);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    for (const auto& name : payloadNames) require(bool(graph.findOperation(name)), "Payload counter test lost its private operation");
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); unsigned a = 3, b = 9;
    for (unsigned step = 0; step < 16; ++step) {
        model.clk = step & 1; model.enable_a = step % 4 == 1; model.enable_b = step % 8 == 7; model.data = step * 11;
        model.eval();
        if (model.clk && model.enable_a) a = static_cast<std::uint8_t>(model.data + 1) ^ 0x35;
        if (model.clk && model.enable_b) b = static_cast<std::uint8_t>(model.data + 1) ^ 0xa7;
        assert(model.small_a == a && model.large_a == a && model.small_b == b && model.large_b == b);
    }
    auto* stream = std::fopen("payload-counts.tsv", "w+"); assert(stream);
    model.dump_execution_counts(stream); std::rewind(stream);
    char line[256]; assert(std::fgets(line, sizeof(line), stream)); assert(std::fgets(line, sizeof(line), stream));
    std::vector<unsigned long long> counts;
    while (std::fgets(line, sizeof(line), stream)) {
        unsigned index; unsigned long long count;
        if (std::sscanf(line, "%u\t%llu", &index, &count) != 2) break;
        assert(index == counts.size()); counts.push_back(count);
    }
    std::fclose(stream);
    for (unsigned i = 0; i < payloadSites.size(); ++i) assert(counts.at(payloadSites[i]) == (i % 2 ? 2 : 4));
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "sink-private-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "#include <vector>", body, false, {{"execution-counters", "on"}});
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        // Count every category when resolving site ordinals.
        const std::regex allSites(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
        std::map<std::string, unsigned> sites; unsigned index = 0;
        for (std::sregex_iterator it(manifest.begin(), manifest.end(), allSites), end; it != end; ++it, ++index)
            if ((*it)[2] == "operation") sites.emplace((*it)[1], index);
        std::string declaration = "static const std::array<unsigned,4> payloadSites{";
        for (const auto& name : payloadNames) {
            require(sites.contains(name), "Private payload has no execution site");
            declaration += std::to_string(sites.at(name)) + ",";
        }
        declaration += "};\n";
        const auto path = artifact(label) / "driver.cpp";
        auto driver = readFile(path);
        driver.insert(driver.find("int main()"), declaration);
        std::ofstream(path) << driver;
    }
}

void testArrayViewsAndWide() {
    Fixture fixture;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable"), x = fixture.input("x", 11);
    const auto i = fixture.input("i", 3), j = fixture.input("j", 3), k = fixture.input("k", 3);
    const auto a = fixture.state("a", 88, {2, 2, 2}), b = fixture.state("b", 88, {8});
    const auto wide = fixture.state("wide", 260, {2});
    grh::setStateInitSpec(fixture.graph, a, {{grh::InitKind::Fill, "11'h4b3"}});
    grh::setStateInitSpec(fixture.graph, b, {{grh::InitKind::Constant, "88'h1234567890abcdefabcdef"}});
    grh::setStateInitSpec(fixture.graph, wide, {{grh::InitKind::Fill, "130'h2abcdef0123456789abcdef00123456789"}});
    const auto whole = fixture.read(a), bRead = fixture.read(b);
    const auto assembled = fixture.compute(OperationKind::kArrayCreate, Fixture::type(176, {2, 2, 2, 2}), {whole, whole});
    const auto flat = fixture.compute(OperationKind::kAssign, Fixture::type(176), {assembled});
    const auto regroup = fixture.compute(OperationKind::kAssign, Fixture::type(176, {4, 4}), {flat});
    const auto one = fixture.compute(OperationKind::kSliceArray, Fixture::type(11), {whole, i, j, k});
    const auto partial = fixture.compute(OperationKind::kSliceArray, Fixture::type(44, {2, 2}), {whole, i});
    fixture.write(OperationKind::kRegisterWriteArrayPort, a, {enable, i, j, k, x, clk});
    // Swap views whose source state will be overwritten by another writer in
    // the same LargeSink round; both must observe General snapshots.
    fixture.write(OperationKind::kRegisterWritePort, b, {enable, whole, clk});
    fixture.write(OperationKind::kRegisterWritePort, a, {enable, bRead, clk});
    auto wideType = Fixture::type(130); wideType.isSigned = true;
    const auto replicated = fixture.compute(OperationKind::kReplicate, Fixture::type(132), {x});
    fixture.graph.setAttr(fixture.graph.valueDef(replicated), "rep", int64_t{12});
    const auto d130 = fixture.compute(OperationKind::kSliceStatic, wideType, {replicated});
    fixture.graph.setAttr(fixture.graph.valueDef(d130), "sliceStart", int64_t{0});
    fixture.graph.setAttr(fixture.graph.valueDef(d130), "sliceEnd", int64_t{129});
    fixture.write(OperationKind::kRegisterWriteArrayPort, wide, {enable, i, d130, clk});
    fixture.graph.bindOutputPort("a", whole); fixture.graph.bindOutputPort("b", bRead);
    fixture.graph.bindOutputPort("flat", flat); fixture.graph.bindOutputPort("regroup", regroup);
    fixture.graph.bindOutputPort("one", one); fixture.graph.bindOutputPort("partial", partial);
    fixture.graph.bindOutputPort("wide", fixture.read(wide));
    fixture.graph.bindOutputPort("wideRead", fixture.read(wide, {i}));
    const auto singleton = fixture.compute(OperationKind::kArrayCreate, Fixture::type(11, {1}), {x});
    fixture.graph.bindOutputPort("singleton", singleton);
    fixture.convert(); const auto reference = fixture.design.clone();
    checkpoint(fixture.design, "arrays-before-split");
    fixture.split(); fixture.partition();
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned step = 0; step < 24; ++step) inputs.push_back({{"clk", step & 1}, {"enable", step % 4 != 0},
        {"i", step / 2 % 5}, {"j", step / 3 % 5}, {"k", step / 5 % 5}, {"x", step * 193 & 2047}});
    for (const auto budget : {16, 1024}) {
        const auto label = "arrays-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
}

void testContiguousArrayImages() {
    Fixture fixture;
    std::string prepare, verify;
    for (const auto width : {8, 16, 32, 64, 128}) {
        const auto suffix = std::to_string(width);
        const auto input = fixture.graph.createValue(fixture.graph.internSymbol("input" + suffix), Fixture::type(width * 6, {2, 3}));
        fixture.graph.bindInputPort("input" + suffix, input);
        const auto flat = fixture.compute(OperationKind::kAssign, Fixture::type(width * 6), {input});
        fixture.graph.bindOutputPort("flat" + suffix, flat);
        fixture.graph.bindOutputPort("shaped" + suffix,
            fixture.compute(OperationKind::kAssign, Fixture::type(width * 6, {3, 2}), {flat}));
        fixture.graph.bindOutputPort("reshaped" + suffix,
            fixture.compute(OperationKind::kAssign, Fixture::type(width * 6, {3, 2}), {input}));
        const auto cell = "model.input" + suffix + "[i/3][i%3]";
        prepare += "for (unsigned i=0;i<6;++i) { const auto word=static_cast<unsigned _BitInt(128)>((epoch+1)*193+step*37+i*51); "
            + cell + "=static_cast<std::remove_reference_t<decltype(" + cell + ")>>((word<<64)|word); }\n";
        verify += "{ decltype(model.flat" + suffix + ") expected=0; for (unsigned i=0;i<6;++i) { "
            "expected |= static_cast<decltype(expected)>(" + cell + ") << (i*" + suffix + "); "
            "assert(model.shaped" + suffix + "[i/2][i%2]==" + cell + "); "
            "assert(model.reshaped" + suffix + "[i/2][i%2]==" + cell + "); } "
            "assert(model.flat" + suffix + "==expected); }\n";
    }
    const auto bytes = fixture.graph.createValue(fixture.graph.internSymbol("bytes"), Fixture::type(8192, {128, 8}));
    fixture.graph.bindInputPort("bytes", bytes);
    const auto flat = fixture.compute(OperationKind::kAssign, Fixture::type(8192), {bytes});
    fixture.graph.bindOutputPort("flatBytes", flat);
    fixture.graph.bindOutputPort("shapedBytes",
        fixture.compute(OperationKind::kAssign, Fixture::type(8192, {32, 32}), {flat}));
    prepare += "for (unsigned i=0;i<1024;++i) model.bytes[i/8][i%8]=static_cast<std::uint8_t>(epoch*197+step*39+i*17);\n";
    verify += "for (unsigned i=0;i<1024;++i) { assert(model.shapedBytes[i/32][i%32]==model.bytes[i/8][i%8]); "
        "assert(static_cast<std::uint8_t>(model.flatBytes>>(i*8))==model.bytes[i/8][i%8]); }\n";
    // Keep the equal-width views in the graph: this is an emitter/layout test.
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split(); fixture.partition();
    for (const auto budget : {16, 1024})
        manualCase(fixture, "contiguous-arrays-b" + std::to_string(budget), budget, "#include <type_traits>",
            "for (unsigned epoch=0;epoch<3;++epoch) { model.init(); for (unsigned step=0;step<8;++step) {\n" +
            prepare + "model.eval();\n" + verify + "} }", false);
}

void testLargeConcatExpressions() {
    Fixture fixture;
    const auto bit = fixture.input("bit");
    const auto byte = fixture.input("byte", 8);
    std::vector<grh::ValueId> operands;
    int32_t width = 0;
    for (unsigned i = 0; i < 1217; ++i) {
        operands.push_back(i % 3 ? bit : byte);
        width += i % 3 ? 1 : 8;
    }
    const auto concat = fixture.graph.createOperation(OperationKind::kConcat);
    for (const auto operand : operands) fixture.graph.addOperand(concat, operand);
    const auto joined = fixture.graph.createValue(Fixture::type(width));
    fixture.graph.addResult(concat, joined);
    fixture.graph.bindOutputPort("joined", joined);
    const auto repeated = fixture.compute(OperationKind::kReplicate, Fixture::type(8 * 1217), {byte});
    fixture.graph.setAttr(fixture.graph.valueDef(repeated), "rep", int64_t(1217));
    fixture.graph.bindOutputPort("repeated", repeated);
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split(); fixture.partition();
    for (const auto budget : {16, 1024})
        manualCase(fixture, "large-concat-b" + std::to_string(budget), budget, "", R"CPP(
for (unsigned epoch=0;epoch<3;++epoch) {
    model.init();
    for (unsigned step=0;step<4;++step) {
        model.bit=step&1; model.byte=0xa7^step; model.eval();
        decltype(model.joined) joined=0;
        decltype(model.repeated) repeated=0;
        for (unsigned i=0;i<1217;++i) {
            if (i%3) joined=(joined<<1)|model.bit;
            else joined=(joined<<8)|model.byte;
            repeated=(repeated<<8)|model.byte;
        }
        assert(model.joined==joined); assert(model.repeated==repeated);
    }
}
)CPP", false);
}

void testArrayViewElision() {
    Fixture fixture;
    std::string prepare, verify;
    // Value::users() belongs to its snapshot. Force heap-backed use storage
    // so iterating a span from a destroyed temporary cannot pass accidentally.
    const auto shared = fixture.graph.createValue(fixture.graph.internSymbol("shared"), Fixture::type(48, {2, 4}));
    fixture.graph.bindInputPort("shared", shared);
    const auto packedShared = fixture.compute(OperationKind::kAssign, Fixture::type(48), {shared});
    for (unsigned index = 0; index < 96; ++index) {
        const auto name = "sharedView" + std::to_string(index);
        const auto reshaped = fixture.compute(OperationKind::kAssign, Fixture::type(48, {4, 2}), {packedShared});
        fixture.graph.bindOutputPort(name, reshaped);
        verify += "for (unsigned i=0;i<8;++i) assert(model." + name + "[i/2][i%2]==model.shared[i/4][i%4]);\n";
    }
    prepare += "for (unsigned i=0;i<8;++i) model.shared[i/4][i%4]=(step*17+i*5)&63;\n";
    for (const auto width : {1, 6, 9, 11, 31, 63, 65, 130}) {
        const auto suffix = std::to_string(width);
        const auto input = fixture.graph.createValue(fixture.graph.internSymbol("input" + suffix), Fixture::type(width * 12, {2, 2, 3}));
        fixture.graph.bindInputPort("input" + suffix, input);
        const auto flattened = fixture.compute(OperationKind::kAssign, Fixture::type(width * 12), {input});
        const auto scalarAlias = fixture.compute(OperationKind::kAssign, Fixture::type(width * 12), {flattened});
        const auto reshaped = fixture.compute(OperationKind::kAssign, Fixture::type(width * 12, {3, 4}), {scalarAlias});
        fixture.graph.bindOutputPort("view" + suffix, reshaped);
        // This sibling must retain a real packed value for a scalar consumer.
        const auto packed = fixture.compute(OperationKind::kAssign, Fixture::type(width * 12), {input});
        fixture.graph.bindOutputPort("packed" + suffix, packed);
        fixture.graph.bindOutputPort("zero" + suffix,
            fixture.compute(OperationKind::kLogicNot, Fixture::type(1), {packed}));
        const auto cell = "model.input" + suffix + "[i/6][(i/3)%2][i%3]";
        prepare += "for (unsigned i=0;i<12;++i) { unsigned _BitInt(" + std::to_string(std::max(width, 64)) + ") word="
            "(epoch*197+step*37+i*17); ";
        if (width > 64) prepare += "word |= word << 64; ";
        if (width > 128) prepare += "word |= word << 128; ";
        prepare += cell + "=static_cast<std::remove_reference_t<decltype(" + cell + ")>>(step==0 ? 0 : word & " +
            (width <= 64 ? "(UINT64_MAX >> " + std::to_string(64 - width) + ")" : "(~decltype(word)(0) >> " + std::to_string(std::max(width, 64) - width) + ")") + "); }\n";
        verify += "{ decltype(model.packed" + suffix + ") expected=0; for (unsigned i=0;i<12;++i) { "
            "expected |= static_cast<decltype(expected)>(" + cell + ") << (i*" + suffix + "); "
            "assert(model.view" + suffix + "[i/4][i%4]==" + cell + "); } "
            "assert(model.packed" + suffix + "==expected); assert(model.zero" + suffix + "==(expected==0)); }\n";
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split(); fixture.partition();
    for (const auto budget : {16, 1024}) {
        const auto label = "array-views-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "#include <type_traits>",
            "for (unsigned epoch=0;epoch<3;++epoch) { model.init(); for (unsigned step=0;step<8;++step) {\n" +
            prepare + "model.eval();\n" + verify + "} }", false);
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        require(manifest.find("\"array_view_elisions\": 17") != std::string::npos,
                "Expected private scalar views, including the shared high-fanout view, to avoid materialization");
    }
}

void testReadmemAndRandomInit() {
    Fixture fixture;
    const auto memory = fixture.state("memory", 44, {2, 2}), other = fixture.state("other", 32, {4});
    const auto random = fixture.state("random", 130);
    const auto file = artifact("init.hex");
    { std::ofstream out(file); out << "// duplicate absolute addresses and partial overlay\n@1 123 456 @1 789\n"; }
    grh::InitStep fill{grh::InitKind::Fill, "11'h55"}, readmem{grh::InitKind::Readmem};
    readmem.file = file.string(); readmem.format = "hex"; readmem.start = 1; readmem.count = 2;
    grh::setStateInitSpec(fixture.graph, memory, {fill, readmem});
    grh::InitStep local{grh::InitKind::Fill}; local.random = true; local.seed = 53;
    grh::setStateInitSpec(fixture.graph, other, {local});
    grh::setStateInitSpec(fixture.graph, random, {{grh::InitKind::Random}});
    fixture.graph.bindOutputPort("memory", fixture.read(memory));
    fixture.graph.bindOutputPort("other", fixture.read(other)); fixture.graph.bindOutputPort("random", fixture.read(random));
    fixture.convert(); fixture.split(); fixture.partition();
    const std::string support = R"CPP(
static std::uint64_t next(std::uint64_t& state) {
    auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
)CPP";
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); model.eval();
    assert(model.memory[0][0] == 0x55 && model.memory[0][1] == 0x789);
    assert(model.memory[1][0] == 0x456 && model.memory[1][1] == 0x55);
    std::uint64_t local = 53, shared = UINT64_C(0x6a09e667f3bcc909);
    for (auto cell : model.other) assert(cell == static_cast<std::uint8_t>(next(local)));
    const auto low = next(shared), mid = next(shared), high = next(shared) & 3;
    assert(static_cast<std::uint64_t>(model.random) == low);
    assert(static_cast<std::uint64_t>(model.random >> 64) == mid);
    assert(static_cast<std::uint64_t>(model.random >> 128) == high);
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "init-b" + std::to_string(budget), budget, support, body, false);
}

void testNonconvergence() {
    Fixture fixture;
    const auto one = fixture.constant(1, "1'b1");
    const auto state = fixture.state("oscillating", 1);
    fixture.graph.setOpKind(state, OperationKind::kLatch);
    const auto value = fixture.read(state);
    const auto inverted = fixture.compute(OperationKind::kNot, Fixture::type(1), {value});
    fixture.write(OperationKind::kLatchWritePort, state, {one, inverted, one});
    fixture.graph.bindOutputPort("value", value);
    fixture.convert(); fixture.split(); fixture.partition();
    for (const auto budget : {16, 1024}) manualCase(fixture, "nonconvergence-b" + std::to_string(budget), budget, "", R"CPP(
for (unsigned epoch=0; epoch<3; ++epoch) {
    model.init(); bool rejected=false;
    try { model.eval(); }
    catch (const std::runtime_error& error) { rejected=std::string(error.what()).find("fixed-point round limit exceeded") != std::string::npos; }
    assert(rejected);
}
)CPP", false, {{"max-round", "4"}});
}

void testMemoryActivationAndCancellation() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), address = fixture.input("address", 5);
    const auto high = fixture.constant(8, "8'hff"), low = fixture.constant(8, "8'h0");
    const auto state = fixture.state("memory", 128, {16});
    fixture.write(OperationKind::kRegisterWriteArrayPort, state, {enable, address, high, clock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, state, {enable, address, low, clock});
    fixture.graph.bindOutputPort("memory", fixture.read(state));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=1"});
    fixture.partition();
    for (const auto budget : {16, 1024}) for (const std::string mode : {"on-change", "on-write"})
        for (const std::string bitmap : {"on", "off"}) {
            const auto label = "memory-" + mode + "-bitmap-" + bitmap + "-b" + std::to_string(budget);
            const std::string body = "const unsigned rounds=" + std::to_string(mode == "on-write" ? 2 : 1) + ";\n" + R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); model.enable=1;
    for(unsigned address : {0,15,31}) {
        model.clock=0; model.address=address; model.eval();
        const auto before=model.perf_counters();
        model.clock=1; model.eval(); const auto after=model.perf_counters();
        for (auto cell : model.memory) assert(cell==0);
        assert(after.totalRoundCount-before.totalRoundCount==(address<16?rounds:1));
        assert(after.touchedWriteCount-before.touchedWriteCount==(address<16?2:0));
        assert(after.touchedStateShadowCount-before.touchedStateShadowCount==(address<16 && rounds==2?1:0));
        const auto settled=model.perf_counters(); model.eval();
        assert(model.perf_counters().touchedWriteCount==settled.touchedWriteCount);
    }
}
)CPP";
            manualCase(fixture, label, budget, "", body, false,
                       {{"mem-write-activate", mode}, {"mem-enable-bitmap", bitmap}});
        }
}

void testWholeArrayBeforeImage() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("whole_clock"), enable = fixture.input("whole_enable");
    const auto zero = fixture.constant(8, "8'h00"), one = fixture.constant(8, "8'h01");
    const auto makeArray = [&](grh::ValueId element) {
        const auto op = graph.createOperation(OperationKind::kArrayCreate);
        for (unsigned i = 0; i < 1024; ++i) graph.addOperand(op, element);
        const auto value = graph.createValue(Fixture::type(8192, {1024}));
        graph.addResult(op, value);
        return value;
    };
    const auto ones = makeArray(one), zeros = makeArray(zero);
    const auto state = fixture.state("whole_memory", 8192, {1024});
    fixture.write(OperationKind::kRegisterWritePort, state, {enable, ones, clock});
    fixture.write(OperationKind::kRegisterWritePort, state, {enable, zeros, clock});
    graph.bindOutputPort("whole", fixture.read(state));
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); model.whole_enable = true; model.whole_clock = false; model.eval();
    const auto before = model.perf_counters();
    model.whole_clock = true; model.eval();
    const auto after = model.perf_counters();
    for (auto cell : model.whole) assert(cell == 0);
    assert(after.totalRoundCount - before.totalRoundCount == 1);
    assert(after.touchedStateShadowCount - before.touchedStateShadowCount == 0);
}
)CPP";
    for (const auto budget : {16u, 1024u})
        manualCase(fixture, "whole-array-before-image-b" + std::to_string(budget), budget, "", body, false,
                   {{"mem-write-activate", "on-change"}, {"mem-enable-bitmap", "on"}});
}

void testWaveformContents() {
    Fixture fixture;
    const auto clock = fixture.input("clock"), data = fixture.input("data", 8), wide = fixture.input("wide", 130);
    const auto enable = fixture.input("enable");
    fixture.graph.addDeclaredSymbol(fixture.graph.valueSymbol(wide));
    const auto one = fixture.constant(1, "1'b1");
    const auto state = fixture.state("memory", 16, {2});
    fixture.write(OperationKind::kRegisterFillPort, state, {one, data, clock});
    const auto inverse = fixture.compute(OperationKind::kNot, Fixture::type(8), {data});
    fixture.graph.setValueSymbol(inverse, fixture.graph.internSymbol("child$inverse"));
    fixture.graph.addDeclaredSymbol(fixture.graph.valueSymbol(inverse));
    fixture.graph.bindOutputPort("inverse", inverse);
    fixture.graph.bindOutputPort("memory", fixture.read(state));
    const auto held = fixture.state("held", 8);
    const auto payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, fixture.constant(8, "8'd7")});
    fixture.graph.setValueSymbol(payload, fixture.graph.internSymbol("sink_payload"));
    fixture.graph.addDeclaredSymbol(fixture.graph.valueSymbol(payload));
    fixture.write(OperationKind::kRegisterWritePort, held, {enable, payload, clock});
    fixture.graph.bindOutputPort("held", fixture.read(held));
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    require(owner(fixture.graph, fixture.graph.valueDef(payload))[1] == "small-sink",
            "Observed payload regression did not retain a sink-local value");
    const std::string support = R"CPP(
#include "fstapi.h"
#include <map>
struct WaveValues {
    std::map<fstHandle,std::string> names;
    std::map<std::string,std::map<std::uint64_t,std::string>> values;
    static void change(void *user, std::uint64_t time, fstHandle handle, const unsigned char *bits) {
        auto& self=*static_cast<WaveValues*>(user);
        self.values[self.names.at(handle)][time]=reinterpret_cast<const char*>(bits);
    }
    std::string at(const std::string& name, std::uint64_t time) const {
        const auto& series=values.at(name); auto it=series.upper_bound(time);
        assert(it != series.begin()); return std::prev(it)->second;
    }
};
static std::string bits8(unsigned value) {
    std::string result(8,'0'); for(unsigned i=0;i<8;++i) result[7-i]='0'+((value>>i)&1); return result;
}
static void checkWave(const char *path) {
    auto *reader=fstReaderOpen(path); assert(reader);
    WaveValues wave;
    while (auto *hier=fstReaderIterateHier(reader)) if(hier->htyp==FST_HT_VAR)
        assert(wave.names.emplace(hier->u.var.handle,hier->u.var.name).second);
    fstReaderSetFacProcessMaskAll(reader);
    assert(fstReaderIterBlocks2(reader,WaveValues::change,nullptr,&wave,nullptr));
    for(unsigned step=0;step<4;++step) {
        const unsigned data=3+step*2, expected=step==0?0:step<3?5:9;
        assert(wave.at("memory[0]",step)==bits8(expected));
        assert(wave.at("memory[1]",step)==bits8(expected));
        assert(wave.at("child$inverse",step)==bits8((~data)&255));
        assert(wave.at("sink_payload",step)==bits8(step==0?0:step<3?12:16));
        assert(wave.at("held",step)==bits8(0));
        const auto wide=wave.at("wide",step); assert(wide.size()==130 && wide.front()=='1');
        assert(wide.substr(122)==bits8(step));
    }
    fstReaderClose(reader);
}
)CPP";
    for (const auto budget : {16, 1024}) manualCase(fixture, "waveform-b" + std::to_string(budget), budget, support, R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    const auto path="epoch"+std::to_string(epoch)+".fst";
    model.configure_waveform(true,path.c_str()); model.init();
    assert(model.waveform_enabled());
    for(unsigned step=0;step<4;++step) {
        model.clock=step&1; model.enable=0; model.data=3+step*2;
        model.wide=(static_cast<unsigned _BitInt(130)>(1)<<129)|step; model.eval();
    }
    model.configure_waveform(false); assert(!model.waveform_enabled()); checkWave(path.c_str());
}
)CPP");
}

// Recreate the frozen observations as ordinary GRH, without reading a legacy
// model. The separate comparison target supplies the frozen stimulus/ABI code.
void generateObservationModels() {
    for (const std::string label : {"dpi_smoke", "dpi_smoke_tu", "dpi_bit_abi", "random_function",
                                    "random_function_tu", "monitor_free", "monitor_event", "monitor_event_tu"}) {
        Fixture fixture;
        auto& graph = fixture.graph;
        const auto one = fixture.constant(1, "1'b1");
        const auto imported = [&](std::string name, const std::vector<std::string>& directions,
                                   const std::vector<int64_t>& widths, int64_t resultWidth) {
            const auto op = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
            graph.setAttr(op, "argsDirection", directions); graph.setAttr(op, "argsWidth", widths);
            graph.setAttr(op, "argsSigned", std::vector<bool>(widths.size(), false));
            std::vector<std::string> names;
            for (std::size_t i = 0; i < widths.size(); ++i) names.push_back("arg" + std::to_string(i));
            graph.setAttr(op, "argsName", names);
            graph.setAttr(op, "hasReturn", true); graph.setAttr(op, "returnWidth", resultWidth);
            return op;
        };
        const auto call = [&](grh::OperationId target, std::initializer_list<grh::ValueId> args,
                               const std::vector<int32_t>& widths, bool event = false) {
            const auto op = graph.createOperation(OperationKind::kDpicCall);
            graph.setAttr(op, "targetImportSymbol", std::string(graph.getOperation(target).symbolText()));
            if (event) graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
            for (const auto value : args) graph.addOperand(op, value);
            std::vector<grh::ValueId> results;
            for (const auto width : widths) {
                results.push_back(graph.createValue(Fixture::type(width))); graph.addResult(op, results.back());
            }
            return results;
        };
        if (label.starts_with("dpi_smoke")) {
            const auto clk = fixture.input("clk"), a = fixture.input("a", 8), d = fixture.input("d", 8);
            const auto inc = imported("cpu_test_inc", {"input"}, {8}, 8);
            const auto dbl = imported("cpu_test_dbl", {"input"}, {8}, 8);
            const auto y = call(inc, {one, a}, {8})[0], z = call(dbl, {one, d, clk}, {8}, true)[0];
            const auto mask = fixture.constant(8, "8'hff");
            const auto q = fixture.state("q", 8), q2 = fixture.state("q2", 8);
            graph.setOpKind(q, OperationKind::kLatch); graph.setOpKind(q2, OperationKind::kLatch);
            fixture.write(OperationKind::kLatchWritePort, q, {one, y, mask});
            fixture.write(OperationKind::kLatchWritePort, q2, {one, z, mask});
            graph.bindOutputPort("o", fixture.read(q)); graph.bindOutputPort("o2", fixture.read(q2));
        } else if (label == "dpi_bit_abi") {
            const auto input = fixture.input("input");
            const auto target = imported("cpu_test_bit_abi", {"output", "input"}, {1, 1}, 1);
            const auto results = call(target, {one, input}, {1, 1});
            graph.bindOutputPort("returned", results[0]); graph.bindOutputPort("output", results[1]);
        } else if (label.starts_with("random_function")) {
            graph.bindOutputPort("echo", fixture.input("input"));
            const auto result = fixture.compute(OperationKind::kSystemFunction, Fixture::type(32), {});
            graph.setAttr(graph.valueDef(result), "name", std::string("random"));
            graph.setAttr(graph.valueDef(result), "hasSideEffects", true);
            graph.bindOutputPort("o", result);
            graph.bindOutputPort("samples", fixture.compute(OperationKind::kArrayCreate,
                Fixture::type(128, {4}), {result, result, result, result}));
        } else if (label == "monitor_free") {
            const auto a = fixture.input("a"), b = fixture.input("b");
            const auto op = fixture.task("monitor", {one, a, b});
            graph.setAttr(op, "procKind", std::string("always")); graph.setAttr(op, "hasTiming", false);
        } else {
            const auto clk = fixture.input("clk"), a = fixture.input("a");
            const auto op = fixture.task("strobe", {one, a, clk}, {"posedge"});
            graph.setAttr(op, "procKind", std::string("always")); graph.setAttr(op, "hasTiming", false);
        }
        fixture.convert(); fixture.split(); fixture.partition();
        const auto path = artifact("observations") / label;
        std::filesystem::remove_all(path); std::filesystem::create_directories(path);
        { std::ofstream out(path / "graph.json"); out << json(fixture.design); }
        emit::EmitDiagnostics diagnostics; emit::GrhSimCpuStCpp emitter(&diagnostics);
        emit::EmitOptions options;
        options.outputDir = (path / "model").string(); options.session = &fixture.session;
        const bool split = label.ends_with("_tu");
        options.attributes = {{"max-function-lines", split ? "16" : "1024"},
                              {"max-tu-bytes", split ? "4096" : "1048576"}};
        if (!emitter.emit(fixture.design, options).success) {
            std::string text;
            for (const auto& entry : diagnostics.messages()) text += entry.message + '\n';
            throw std::runtime_error(label + ": " + text);
        }
        inspectFunctions(path / "model", split ? 16 : 1024);
    }
}
}

int main() {
    // Append after testModels initializes the generated execution Makefile.
    try { testChunkWriter(); testModels(); testNamedModelStores(); testActivityBatchOrder(); testMultipleNonsinkActivation(); testLargeStoreConstruction(); testChunkExecution(); testEffectsAndCalls(); testDpiTriggerActivation(); testDpiIntegralAliases(); testDpiMixedTypes(); testSinkRunSharedCones(); testSinkPrivatePayloads(); testArrayViewsAndWide(); testContiguousArrayImages(); testLargeConcatExpressions(); testArrayViewElision(); testReadmemAndRandomInit(); testNonconvergence(); testMemoryActivationAndCancellation(); testWholeArrayBeforeImage(); testWaveformContents(); testExecutionCounters(); generateObservationModels(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
