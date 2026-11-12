#include "emit/grhsim/cpu-st-cpp.hpp"
#include "function-writer.hpp"
#include "grhsim_partition_fixture.hpp"
#include "json-writer.hpp"

#include "slang/text/Json.h"

#include <limits>
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

void testStreamingJsonWriter() {
    using wolvrix::lib::emit::grhsim_detail::StreamingJsonWriter;
    std::string text;
    for (unsigned byte = 0; byte < 32; ++byte) text.push_back(static_cast<char>(byte));
    text += "\"/\\ UTF-8: \xc3\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80";
    const auto payload = [&](auto& json) {
        json.startObject();
        json.writeProperty(text); json.writeValue(std::string_view(text));
        json.writeProperty("empty"); json.writeValue(std::string_view{});
        json.writeProperty("nested"); json.startArray();
        json.startObject(); json.endObject();
        json.startArray(); json.endArray();
        json.startObject();
        json.writeProperty("array"); json.startArray(); json.endArray();
        json.writeProperty("object"); json.startObject(); json.endObject();
        json.endObject();
        json.endArray();
        json.writeProperty("numbers"); json.startArray();
        json.writeValue(std::numeric_limits<uint64_t>::max());
        json.writeValue(uint64_t(0));
        json.writeValue(std::numeric_limits<int64_t>::min());
        json.writeValue(std::numeric_limits<int64_t>::max());
        json.writeValue(int64_t(0));
        json.writeValue(true); json.writeValue(false);
        json.endArray();
        json.endObject();
    };
    slang::JsonWriter reference;
    payload(reference);
    std::ostringstream output;
    StreamingJsonWriter streaming(output);
    payload(streaming);
    require(output.str() == reference.view(), "Streaming JSON differs from buffered JSON");

    // Spliced values consume an object property and participate in array
    // comma insertion exactly like values emitted through the regular API.
    std::istringstream objectSpool(output.str()), arraySpool(output.str());
    std::ostringstream rawOutput;
    StreamingJsonWriter raw(rawOutput);
    raw.startObject();
    raw.writeProperty("payload"); raw.writeRawValue(objectSpool);
    raw.writeProperty("array"); raw.startArray();
    raw.writeValue(true); raw.writeRawValue(arraySpool); raw.writeValue(false);
    raw.endArray();
    raw.writeProperty("after"); raw.startObject(); raw.endObject();
    raw.endObject();
    slang::JsonWriter rawReference;
    rawReference.startObject();
    rawReference.writeProperty("payload"); payload(rawReference);
    rawReference.writeProperty("array"); rawReference.startArray();
    rawReference.writeValue(true); payload(rawReference); rawReference.writeValue(false);
    rawReference.endArray();
    rawReference.writeProperty("after"); rawReference.startObject(); rawReference.endObject();
    rawReference.endObject();
    require(rawOutput.str() == rawReference.view(), "Raw JSON spool changed nesting or comma handling");
}

// Resolve names from the emitted map rather than duplicating the naming policy.
std::string mappedName(const std::filesystem::path& directory, std::string_view category, uint32_t index) {
    const auto map = readFile(directory / "code-map.json");
    const auto key = category == "value" ? "value_id" : "operation_id";
    const auto id = std::string(category == "value" ? "v" : "o") + std::to_string(index);
    const std::regex record("\"" + std::string(key) + "\":\"" + id +
        "\",\"cpp_name\":\"([A-Za-z_][A-Za-z0-9_]*)\"");
    std::smatch found;
    require(std::regex_search(map, found, record), "Missing code-map name for " + id);
    return found[1].str();
}

std::string mappedValue(const std::filesystem::path& directory, grh::ValueId value) {
    return mappedName(directory, "value", value.index);
}

std::string mappedOperation(const std::filesystem::path& directory, grh::OperationId op) {
    return mappedName(directory, "operation", op.index);
}

std::string mappedPackedPrefix(const std::filesystem::path& directory, grh::ValueId value) {
    const auto map = readFile(directory / "code-map.json");
    const std::regex record("\"value_id\":\"v" + std::to_string(value.index) +
        "\",\"cpp_name\":\"[A-Za-z_][A-Za-z0-9_]*\",\"packed_prefix\":\"([A-Za-z_][A-Za-z0-9_]*)\"");
    std::smatch found;
    require(std::regex_search(map, found, record), "Missing packed prefix in code-map");
    return found[1].str();
}

std::string generatedCpp(const std::filesystem::path& directory) {
    std::string result;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") result += readFile(entry.path());
    return result;
}

// Only split top-level records; braces and escaped quotes in source metadata
// are data, and must never truncate an operation's nested storage records.
std::vector<std::string> mappedRecords(const std::string& map, std::string_view array) {
    const auto marker = "\"" + std::string(array) + "\":[";
    const auto begin = map.find(marker);
    require(begin != std::string::npos, "Code map lacks array " + std::string(array));
    std::vector<std::string> result;
    bool quoted = false, escaped = false; unsigned depth = 0; std::size_t start = 0;
    for (std::size_t i = begin + marker.size(); i < map.size(); ++i) {
        const auto ch = map[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
            continue;
        }
        if (ch == '"') { quoted = true; continue; }
        if (ch == '{') { if (depth++ == 0) start = i; }
        else if (ch == '}') {
            require(depth != 0, "Unbalanced code-map object");
            if (--depth == 0) result.push_back(map.substr(start, i - start + 1));
        } else if (ch == ']' && depth == 0) return result;
    }
    throw std::runtime_error("Unterminated code-map array");
}

void inspectOperationStorage(const std::filesystem::path& directory, const grh::Graph& graph) {
    const auto map = readFile(directory / "code-map.json");
    const auto header = readFile(directory / "grhsim_top.hpp");
    const auto records = mappedRecords(map, "operations");
    const auto functions = mappedRecords(map, "functions");
    std::map<std::string, std::set<std::string>> phaseOperations;
    const std::regex root(R"REGEX("root_function":"(~?[A-Za-z_][A-Za-z0-9_]*)")REGEX");
    const std::regex operation(R"REGEX("(o[0-9]+)")REGEX");
    for (const auto& function : functions) {
        std::smatch match; require(std::regex_search(function, match, root), "Function map omitted its root");
        const auto start = function.find("\"operations\":[");
        const auto end = start == std::string::npos ? start : function.find(']', start);
        require(start != std::string::npos && end != std::string::npos, "Function map omitted its operation list");
        const auto body = function.substr(start, end - start);
        for (std::sregex_iterator it(body.begin(), body.end(), operation), last; it != last; ++it)
            phaseOperations[match[1].str()].insert((*it)[1].str());
    }
    const std::regex identity(R"REGEX(^[{]"operation_id":"(o[0-9]+)","cpp_name":"([A-Za-z_][A-Za-z0-9_]*)")REGEX");
    std::map<std::string, std::pair<const std::string*, std::string>> byId;
    for (const auto& record : records) {
        std::smatch match; require(std::regex_search(record, match, identity), "Invalid code-map operation identity");
        byId.emplace(match[1].str(), std::pair{&record, match[2].str()});
    }
    std::map<std::string, std::vector<std::string>> actualStorage;
    const auto declaredFields = [](const std::string& body) {
        std::vector<std::string> result;
        const std::regex field(R"REGEX(^[ \t]+.+[ \t]+([A-Za-z_][A-Za-z0-9_]*)(?:[{][^;]*[}])?;[ \t]*$)REGEX");
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            const auto comment = std::min(line.find("/*"), line.find("//"));
            if (comment != std::string::npos) line.resize(comment);
            // Fields use brace initializers. The defaulted constructor has an
            // equals sign and must not become a fictional field named default.
            // Do not reject all parentheses: unsigned _BitInt(N) is a type.
            if (line.find('=') != std::string::npos) continue;
            std::smatch match;
            if (std::regex_match(line, match, field)) result.push_back(match[1].str());
        }
        return result;
    };
    for (const auto& store : mappedRecords(map, "stores")) {
        const std::regex name(R"REGEX(^[{]"name":"([A-Za-z_][A-Za-z0-9_]*)")REGEX");
        std::smatch match; require(std::regex_search(store, match, name), "Invalid exported store name");
        const auto storeName = match[1].str();
        const std::regex type(R"REGEX("type":"([A-Za-z_][A-Za-z0-9_]*)")REGEX");
        require(std::regex_search(store, match, type), "Exported store omitted its declared type");
        const auto begin = header.find("struct " + match[1].str() + " {");
        const auto end = begin == std::string::npos ? begin : header.find("    };", begin);
        require(begin != std::string::npos && end != std::string::npos, "Mapped store has no generated declaration");
        const auto body = header.substr(begin, end - begin);
        const auto fields = mappedRecords(store, "fields");
        for (const auto& member : declaredFields(body)) {
            require(std::any_of(fields.begin(), fields.end(), [&](const auto& record) {
                return record.starts_with("{\"name\":\"" + member + "\",") &&
                    record.find("\"reference\":\"" + storeName + "." + member + "\"") != std::string::npos;
            }), "Exported store omits an actual field: " + storeName + "." + member);
        }
    }
    for (const auto store : {"smallSinkStore", "largeSinkStore", "prevEventStore", "timeslotTriggerStore",
                             "sampledStore", "onceStore", "largeBeforeImageStore", "largeWholeBeforeStore",
                             "largeWholeDirtyStore", "largeSinkEnableBoundaryStore"}) {
        const auto begin = header.find(std::string("struct ") + store + "Type {");
        const auto end = begin == std::string::npos ? begin : header.find("    };", begin);
        if (begin == std::string::npos || end == std::string::npos) continue;
        const auto body = header.substr(begin, end - begin);
        for (const auto& member : declaredFields(body))
            actualStorage[member].push_back(std::string(store) + "." + member);
    }
    for (const auto op : graph.operations()) {
        const auto id = "o" + std::to_string(op.index);
        const auto entry = byId.find(id); require(entry != byId.end(), "Code map omitted an operation");
        const auto& member = entry->second.second;
        const auto storage = mappedRecords(*entry->second.first, "storage");
        auto expected = actualStorage[member];
        const auto capture = actualStorage.find(member + "_captured_enable");
        if (capture != actualStorage.end()) expected.insert(expected.end(), capture->second.begin(), capture->second.end());
        for (const auto& reference : expected) {
            require(std::any_of(storage.begin(), storage.end(), [&](const auto& record) {
                return record.find("\"reference\":\"" + reference + "\"") != std::string::npos;
            }), "Code map omitted actual operation storage: " + reference);
        }
        if (graph.opKind(op) == sim::kHistoryWrite) {
            require(phaseOperations["pOutput"].contains(id),
                    "Output history write is absent from output function provenance: " + id);
        }
        if (graph.opKind(op) == OperationKind::kRegister || graph.opKind(op) == OperationKind::kLatch) {
            require(phaseOperations["init"].contains(id), "State initializer is absent from function provenance: " + id);
            if (sim::attribute<std::string>(graph, op, "grhsim.history").value_or("") != "output-end")
                require(std::any_of(phaseOperations.begin(), phaseOperations.end(), [&](const auto& phase) {
                    return phase.first.starts_with("pPublish") && phase.second.contains(id);
                }), "State publisher is absent from function provenance: " + id);
        }
        if (graph.opKind(op) == sim::kEdgeDetect || entry->second.first->find("\"subscription\":") != std::string::npos)
            require(phaseOperations["init"].contains(id), "Detector/subscription initialization lost provenance: " + id);
    }
}

void inspectFunctions(const std::filesystem::path& directory, std::size_t budget, bool readableDump = true) {
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
    const auto activation = readFile(directory / "activation-stats.json");
    require(manifest.find("\"activation_stats\": \"activation-stats.json\"") != std::string::npos &&
            activation.find("\"schema\": \"grhsim-activation-stats.v1\"") != std::string::npos,
            "Emitter omitted activation-edge statistics");
    const auto header = readFile(directory / "grhsim_top.hpp");
    require(!readFile(directory / "code-map.json").empty(), "Default emitter omitted the code map");
    if (readableDump) require(!readFile(directory / "grhsim.txt").empty(), "Default emitter omitted the readable graph");
    else require(!std::filesystem::exists(directory / "grhsim.txt"), "Readable dump opt-out was ignored");
    require(header.find("chunkFrame_pGeneral") == std::string::npos,
            "General phase driver contains a spill frame");
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
    inspectFunctions(path / "model", budget, !attributes.contains("readable-dump") || attributes.at("readable-dump") != "off");
    inspectOperationStorage(path / "model", fixture.graph);
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
               const std::string& label, std::size_t budget, std::size_t tuBytes,
               const std::map<std::string, std::string>& attributes = {}) {
    const auto path = artifact(label);
    std::filesystem::remove_all(path); std::filesystem::create_directories(path);
    emit::EmitDiagnostics diagnostics;
    emit::GrhSimCpuStCpp emitter(&diagnostics);
    emit::EmitOptions options;
    options.outputDir = (path / "model").string(); options.session = &fixture.session;
    options.attributes = {{"max-function-lines", std::to_string(budget)}, {"max-tu-bytes", std::to_string(tuBytes)}, {"waveform", "declared-symbols"}};
    for (const auto& [key, value] : attributes) options.attributes[key] = value;
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
    inspectFunctions(path / "model", budget, !attributes.contains("readable-dump") || attributes.at("readable-dump") != "off");
    inspectOperationStorage(path / "model", fixture.graph);
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

    Function privateSpill{"private_spill"};
    std::string privateSum = "result = ";
    for (unsigned i = 0; i < 20; ++i) {
        const auto name = "private" + std::to_string(i);
        privateSpill.locals.push_back({"std::uint64_t", name});
        privateSum += (i ? " + @" : "@") + name + "@";
    }
    privateSpill.body = {Code::line(privateSum + ";")};
    const auto legacyPrivate = writeFunction(privateSpill, "ChunkTest", 12);
    require(legacyPrivate.functions.front().localReferences.at("private19") == "chunkFrame.private19",
            "Generic callers localized spills without an address-lifetime contract");
    privateSpill.localizePrivateNumericSpills = true;
    for (const bool zero : {false, true}) {
        privateSpill.zeroInitializeLocals = zero;
        const auto local = writeFunction(privateSpill, "ChunkTest", 12);
        require(local.functions.front().localReferences.at("private19") == "localFrame.private19" &&
                    local.frameDeclaration.find("struct localFrame_private_spill") != std::string::npos &&
                    local.frameDeclaration.find("chunkFrame_private_spill") == std::string::npos,
                "Single-part numeric declaration spills escaped into a shared frame");
        require(local.functions.front().definition.find(std::string("localFrame_private_spill localFrame") +
                    (zero ? "{};" : ";")) != std::string::npos,
                "Part-local aggregate lost the producer's initialization policy");
        require(local.functions.front().lines <= 12, "Part-local aggregate exceeded the line budget");
    }
    privateSpill.locals.push_back({"std::string", "text"});
    privateSpill.locals.push_back({"std::uint64_t", "explicitInit", "{7}"});
    privateSpill.body.push_back(Code::line("result += @text@.size() + @explicitInit@;"));
    const auto lifetimes = writeFunction(privateSpill, "ChunkTest", 12);
    require(lifetimes.functions.front().localReferences.at("text") == "chunkFrame.text" &&
                lifetimes.functions.front().localReferences.at("explicitInit") == "chunkFrame.explicitInit",
            "Nontrivial or explicit initialization moved to a part-local aggregate");
    function.localizePrivateNumericSpills = true;
    const auto repeatedLocals = writeFunction(function, "ChunkTest", 12);
    for (const auto& part : repeatedLocals.functions) {
        const auto values = part.localReferences.find("values");
        if (values != part.localReferences.end())
            require(values->second.starts_with("chunkFrame."), "Repeated loop storage was initialized per helper call");
        require(part.lines <= 12, "Repeated-loop frame accounting exceeded the line budget");
    }
}

void testModels() {
    Fixture fixture;
    const auto clock0 = fixture.input("clock0"), clock1 = fixture.input("clock1");
    const auto enable = fixture.input("enable"), i = fixture.input("i", 2), j = fixture.input("j", 3), data = fixture.input("data", 8);
    const auto q = fixture.state("q", 8), memory = fixture.state("memory", 64, {2, 4});
    auto signedType = Fixture::type(5); signedType.isSigned = true;
    const auto signedData = fixture.graph.createValue(fixture.graph.internSymbol("signed_data"), signedType);
    const auto signedMask = fixture.graph.createValue(fixture.graph.internSymbol("signed_mask"), signedType);
    fixture.graph.bindInputPort("signed_data", signedData); fixture.graph.bindInputPort("signed_mask", signedMask);
    const auto signedQ = fixture.state("signed_q", 5);
    grh::setStateLogicalType(fixture.graph, signedQ, signedType);
    const auto latch = fixture.state("latch", 8); fixture.graph.setOpKind(latch, OperationKind::kLatch);
    const auto mask = fixture.constant(8, "8'h5a");
    const auto qRead = fixture.read(q);
    const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, qRead});
    fixture.write(OperationKind::kRegisterWritePort, q, {enable, data, clock0});
    fixture.write(OperationKind::kRegisterWriteMaskPort, q, {enable, sum, mask, clock1});
    fixture.write(OperationKind::kRegisterWritePort, signedQ, {enable, signedData, clock0});
    fixture.write(OperationKind::kRegisterWriteMaskPort, signedQ, {enable, signedData, signedMask, clock1});
    fixture.write(OperationKind::kRegisterFillPort, memory, {enable, sum, clock0});
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {enable, i, j, qRead, mask, clock1});
    const auto addressed = fixture.read(memory, {i, j});
    fixture.write(OperationKind::kLatchWritePort, latch, {enable, addressed, mask});
    fixture.graph.bindOutputPort("q", qRead);
    fixture.graph.bindOutputPort("memory", fixture.read(memory));
    fixture.graph.bindOutputPort("latch", fixture.read(latch));
    fixture.graph.bindOutputPort("addressed", addressed);
    fixture.graph.bindOutputPort("partial", fixture.read(memory, {i}));
    fixture.graph.bindOutputPort("signed_q", fixture.read(signedQ));
    fixture.graph.bindOutputPort("single_leaf", fixture.compute(OperationKind::kAssign, Fixture::type(5, {1}), {signedData}));
    fixture.convert(); const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=8"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-max-supernode-nodes=4"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (unsigned step = 0; step < 48; ++step) inputs.push_back({{"clock0", step / 2 & 1}, {"clock1", step / 3 & 1},
        {"enable", step % 7 != 0}, {"i", step / 4 & 3}, {"j", step / 5 & 7}, {"data", step * 17 & 255},
        {"signed_data", step * 7 & 31}, {"signed_mask", step * 11 & 31}});
    std::ofstream make(artifact("Makefile")); make << ".PHONY: run\nrun:";
    std::vector<std::string> labels;
    for (const auto budget : {16, 48, 1024}) for (const auto tu : {4096, 1048576}) {
        const auto label = "phases-b" + std::to_string(budget) + "-tu" + std::to_string(tu);
        writeCase(fixture, reference, inputs, label, budget, tu); labels.push_back(label); make << ' ' << label;
        const auto generated = generatedCpp(artifact(label) / "model");
        require(!std::regex_search(generated, std::regex(R"(for \([^;\n]+;[^;\n]+ < 1;)")),
                "Scalar or indexed one-leaf model retained a single-iteration loop");
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
    const auto failureParent = artifact("streaming-failure");
    std::filesystem::remove_all(failureParent);
    std::filesystem::create_directories(failureParent);
    options.outputDir = (failureParent / "model").string();
    options.attributes.clear(); options.maxOutputFileBytes = 1;
    require(!emitter.emit(fixture.design, options).success, "Artifact byte budget was ignored");
    require(std::filesystem::is_empty(failureParent), "Failed streaming emission left output or staging");
    options.maxOutputFileBytes = emit::EmitOptions{}.maxOutputFileBytes;
    require(emitter.emit(fixture.design, options).success, "Emission did not recover after staging cleanup");
    require(std::distance(std::filesystem::directory_iterator(failureParent), std::filesystem::directory_iterator{}) == 1,
            "Successful streaming emission left staging");
    require(!std::filesystem::exists(std::filesystem::path(*options.outputDir) / ".function-bodies"),
            "Private function spool was published");
    options.outputDir = artifact("invalid").string();
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
        {"-max-supernode-nodes=1"});
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
    function.localizePrivateNumericSpills = true;
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
    declarations.localizePrivateNumericSpills = true;
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
    // Independent guarded payloads force private declarations after splitting.
    // Each helper must reinitialize its numeric aggregate on the next run.
    Function localSpills{"run", "unsigned parity"};
    localSpills.parameters = "unsigned parity";
    localSpills.locals.push_back({"unsigned", "parity", "{}", true});
    localSpills.localizePrivateNumericSpills = true;
    for (unsigned group = 0; group < 8; ++group) {
        std::string value;
        for (unsigned i = 0; i < 18; ++i) {
            const auto name = "local" + std::to_string(group) + "_" + std::to_string(i);
            localSpills.locals.push_back({"std::uint64_t", name});
            value += (i ? " + @" : "@") + name + "@";
        }
        const auto array = "array" + std::to_string(group);
        localSpills.locals.push_back({"std::array<std::array<std::uint64_t,2>,2>", array});
        localSpills.body.push_back(Code::block("if (@parity@ == " + std::to_string(group % 2) + ")", {
            Code::line("++@local" + std::to_string(group) + "_0@;"),
            Code::line("++@" + array + "@[1][1];"),
            Code::line("result += " + value + " + @" + array + "@[1][1];")
        }));
    }
    for (const auto budget : {24, 1024}) {
        const auto written = writeFunction(localSpills, "ChunkTest", budget);
        std::string support = "class ChunkTest { public: std::uint64_t result=0;\n" + written.frameDeclaration;
        bool sawLocal = false;
        for (const auto& part : written.functions) {
            require(part.lines <= std::size_t(budget), "Part-local spill execution exceeded the line budget");
            sawLocal |= part.definition.find("localFrame_") != std::string::npos;
            support += part.declaration;
        }
        if (budget == 24) require(sawLocal, "Guarded payload fixture did not exercise private aggregate spills");
        support += "};\n";
        for (const auto& part : written.functions) support += part.definition;
        manualCase(fixture, "chunk-private-numeric-b" + std::to_string(budget), budget, support,
                   "ChunkTest test; for(unsigned epoch=0; epoch<3; ++epoch) { test.run(0); test.run(1); "
                   "assert(test.result==16*(epoch+1)); }");
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

void testReadableFunctionWriter() {
    const std::string unsafe = "original @ghost@ */ INJECTED /* \n\r\t\\";
    Function plain{"readable_chunks"};
    plain.locals = {{"unsigned", "sum"}};
    for (unsigned i = 0; i < 80; ++i)
        plain.body.push_back(Code::line("@sum@ += " + std::to_string(i) + ";"));
    plain.body.push_back(Code::line("result = @sum@;"));
    Function annotated = plain;
    annotated.comment = unsafe; annotated.locals[0].comment = unsafe;
    for (std::size_t i = 0; i < annotated.body.size(); ++i) {
        annotated.body[i] = Code::commented(std::move(annotated.body[i]), unsafe);
        annotated.body[i].sourceOperation = "o" + std::to_string(i + 1);
    }
    for (const auto budget : {16u, 1024u}) {
        const auto before = writeFunction(plain, "ChunkTest", budget);
        const auto after = writeFunction(annotated, "ChunkTest", budget);
        require(before.functions.size() == after.functions.size(), "Comments changed function splitting");
        std::set<std::string> operations;
        for (std::size_t i = 0; i < after.functions.size(); ++i) {
            const auto& part = after.functions[i];
            require(part.name == before.functions[i].name && part.lines == before.functions[i].lines &&
                    part.localReferences == before.functions[i].localReferences,
                    "Comments changed the budget, local liveness or physical references");
            require(part.lines <= budget && part.definition.find("@ghost@") == std::string::npos &&
                    part.definition.find("*/ INJECTED /*") == std::string::npos &&
                    part.definition.find("\\\n") == std::string::npos &&
                    part.definition.find('\r') == std::string::npos,
                    "Source documentation escaped a comment or affected preprocessing");
            operations.insert(part.operations.begin(), part.operations.end());
        }
        for (std::size_t i = 0; i < annotated.body.size(); ++i)
            require(operations.contains("o" + std::to_string(i + 1)), "Chunk mapping lost a source operation");
    }
    Function frame{"readable_frame"};
    std::string expression = "result = ";
    for (unsigned i = 0; i < 1100; ++i) {
        const auto name = "module_payload_" + std::to_string(i);
        frame.locals.push_back({"unsigned", name});
        expression += (i ? " + @" : "@") + name + "@";
    }
    frame.body = {Code::line(expression + ";")};
    for (const auto budget : {16u, 1024u}) {
        const auto compact = writeFunction(frame, "ChunkTest", budget);
        frame.readableFrame = true;
        const auto direct = writeFunction(frame, "ChunkTest", budget);
        frame.readableFrame = false;
        require(compact.frameDeclaration.find("cells_") != std::string::npos &&
                direct.frameDeclaration.find("cells_") == std::string::npos &&
                direct.frameDeclaration.find("unsigned module_payload_1099;") != std::string::npos,
                "Readable frame option did not retain logical field names");
        require(compact.functions.front().localReferences.at("module_payload_1099").find("chunkFrame.cells_") == 0 &&
                direct.functions.front().localReferences.at("module_payload_1099") == "chunkFrame.module_payload_1099",
                "Frame map does not describe the emitted physical reference");
    }
    Function parameter{"shared_parameter"};
    parameter.parameters = "unsigned input";
    parameter.locals = {{"unsigned", "input", "{}", true}};
    std::vector<Code> repeated;
    for (unsigned i = 0; i < 40; ++i) repeated.push_back(Code::line("result += @input@;"));
    parameter.body = {Code::block("if (true)", std::move(repeated))};
    const auto shared = writeFunction(parameter, "ChunkTest", 16);
    require(shared.functions.size() > 1 &&
            shared.functions.front().localReferences.at("input") == "chunkFrame.input",
            "Root parameter-to-frame assignment is absent from the local reference map");
    require(std::any_of(shared.functions.begin() + 1, shared.functions.end(), [&](const auto& part) {
        const auto found = part.localReferences.find("input");
        return found != part.localReferences.end() && found->second == "chunkFrame.input";
    }), "Extracted helper lost its shared parameter reference");
    // Return control is synthesized after the caller's locals are collected.
    // Its map must follow the actual root/helper rendering, including helpers
    // which use only a return check and helpers with no return control at all.
    for (const bool returnsValue : {false, true}) {
        Function control{"mapped_return_control", "bool stop", returnsValue ? "std::uint64_t" : "void"};
        control.locals = {{"bool", "stop", "{}", true}, {"std::uint64_t", "sum", "{5}"}};
        std::vector<Code> early, ordinary;
        for (unsigned i = 0; i < 40; ++i) {
            early.push_back(Code::line("++@sum@;"));
            ordinary.push_back(Code::line("@sum@ += 2;"));
        }
        const auto terminal = returnsValue ? "return @sum@;" : "return;";
        early.push_back(Code::block("if (@sum@ > 10)", {Code::line(terminal, true)}));
        control.body = {Code::block("if (@stop@)", std::move(early)),
                        Code::block("if (true)", std::move(ordinary)), Code::line(terminal, true)};
        const auto written = writeFunction(control, "ChunkTest", 16);
        require(written.functions.size() > 2 &&
                written.functions.front().localReferences.contains("chunk_returned") &&
                written.functions.front().localReferences.contains("chunk_result") == returnsValue,
                "Root return check omitted its physical control references");
        bool terminalHelper = false, ordinaryHelper = false;
        for (std::size_t i = 0; i < written.functions.size(); ++i) {
            const auto& part = written.functions[i];
            require(part.localReferences.size() == part.localTypes.size(),
                    "A mapped writer local omitted its type");
            for (const auto& [name, reference] : part.localReferences) {
                require(part.localTypes.contains(name) && !part.localTypes.at(name).empty(),
                        "A physical local reference omitted its type");
            }
            for (const auto name : {"chunk_returned", "chunk_result"}) {
                const auto reference = std::string("chunkFrame.") + name;
                const bool emitted = part.definition.find(reference) != std::string::npos;
                require(part.localReferences.contains(name) == emitted,
                        "Return control map disagrees with an actual emitted reference");
                if (emitted) {
                    require(part.localReferences.at(name) == reference &&
                            part.localTypes.at(name) == (std::string_view(name) == "chunk_returned" ? "bool" : "std::uint64_t"),
                            "Return control map has an incorrect physical reference or type");
                }
            }
            if (i && part.definition.find("chunkFrame.chunk_returned = true;") != std::string::npos) {
                terminalHelper = true;
                require(part.localReferences.contains("chunk_returned") &&
                        part.localReferences.contains("chunk_result") == returnsValue,
                        "Extracted terminal helper omitted its return control");
            }
            if (i && !part.localReferences.contains("chunk_returned")) ordinaryHelper = true;
            if (!returnsValue) require(!part.localReferences.contains("chunk_result"),
                                       "Void function exported nonexistent result storage");
        }
        require(terminalHelper && ordinaryHelper, "Control mapping fixture did not exercise both helper paths");
    }
}

void testReadableEmission() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto data = fixture.input("data", 8);
    const auto first = fixture.state("tile$pipe-a", 8), second = fixture.state("tile$pipe$a", 8);
    const auto keyword = fixture.state("class", 8);
    const auto unicode = fixture.state("top$\xc3\xa9tage$payload", 8);
    const std::string longPath = "top$" + std::string(300, 'a') + "$decode$" + std::string(180, 'b');
    const auto distant = fixture.state("distant_state", 8);
    graph.setAttr(distant, "originModules", std::vector<std::string>{longPath});
    for (const auto state : {first, second, distant, keyword, unicode})
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, data, clock});
    const auto sum = fixture.compute(OperationKind::kAdd, Fixture::type(8), {fixture.read(first), data});
    const auto sumOp = graph.valueDef(sum);
    graph.setAttr(sumOp, "originModules", std::vector<std::string>{"top$core0$alu", "top$core1$alu"});
    grh::SrcLoc location; location.file = "rtl/original @ghost@ */ INJECTED /* \n\r\t\\";
    location.line = 37; location.column = 9; location.origin = "rtl";
    graph.setOpSrcLoc(sumOp, location);
    graph.setValueSymbol(sum, graph.internSymbol("top$execute$sum"));
    graph.addDeclaredSymbol(graph.valueSymbol(sum));
    const auto missing = fixture.compute(OperationKind::kXor, Fixture::type(8), {fixture.read(second), data});
    const auto missingOp = graph.valueDef(missing);
    graph.bindOutputPort("sum", sum); graph.bindOutputPort("missing", missing);
    graph.bindOutputPort("distant", fixture.read(distant));
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split(); fixture.partition();
    const auto checkpoint = json(fixture.design);
    const auto stamp = tx::DesignStamp::capture(graph);
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 2; ++epoch) {
    model.init(); model.clock = 0; model.enable = 1; model.data = 7; model.eval();
    assert(model.sum == 7 && model.missing == 7 && model.distant == 0);
    model.clock = 1; model.eval();
    assert(model.sum == 14 && model.missing == 0 && model.distant == 7);
    model.clock = 0; model.enable = 0; model.data = 11; model.eval();
    assert(model.sum == 18 && model.missing == 12 && model.distant == 7);
    model.clock = 1; model.eval();
    assert(model.sum == 18 && model.missing == 12 && model.distant == 7);
}
)CPP";
    for (const auto budget : {16u, 1024u}) {
        const auto label = "readable-emission-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "", body, false);
        const auto directory = artifact(label) / "model";
        const auto source = generatedCpp(directory);
        const auto map = readFile(directory / "code-map.json");
        const auto dump = readFile(directory / "grhsim.txt");
        std::set<std::string> allNames;
        for (const auto value : graph.values()) {
            const auto name = mappedValue(directory, value);
            require(name.starts_with("val_") && name.size() <= 180 && allNames.insert(name).second,
                    "Readable value names are invalid, unbounded or ambiguous");
        }
        for (const auto op : graph.operations()) {
            const auto name = mappedOperation(directory, op);
            require(name.starts_with("op_") && name.size() <= 180 && allNames.insert(name).second,
                    "Readable operation names are invalid, unbounded or ambiguous");
        }
        require(mappedOperation(directory, first) != mappedOperation(directory, second),
                "Sanitized hierarchical declaration names collided");
        require(mappedOperation(directory, keyword).find("class") != std::string::npos &&
                mappedOperation(directory, unicode) != mappedOperation(directory, keyword),
                "Keyword or non-ASCII source identifiers lost their unique safe spelling");
        require(mappedOperation(directory, distant).find("distant_state") != std::string::npos &&
                dump.find(longPath) != std::string::npos, "Long source paths were lost from readable exports");
        require(mappedOperation(directory, sumOp).find("shared2") != std::string::npos &&
                dump.find("top$core0$alu") != std::string::npos && dump.find("top$core1$alu") != std::string::npos,
                "Merged logic lost its multiple defining origins");
        require(mappedOperation(directory, missingOp).find("generated") != std::string::npos &&
                source.find("origin unavailable/generated") != std::string::npos,
                "Missing provenance was replaced with an invented RTL origin");
        require(source.find("Phase 1 Input") != std::string::npos &&
                source.find("Phase 6 Output") != std::string::npos &&
                source.find("source=rtl/original") != std::string::npos,
                "Default generated source lacks phase and operation documentation");
        require(source.find("@ghost@") == std::string::npos &&
                source.find("*/ INJECTED /*") == std::string::npos && source.find("\\\n") == std::string::npos &&
                source.find('\r') == std::string::npos, "RTL provenance escaped generated C++ comments");
        require(map.find("\"operations\":[\"o") != std::string::npos &&
                map.find("\"source\":\"grhsim_top_tu") != std::string::npos &&
                map.find("\"reference\":") != std::string::npos,
                "Code map omitted function provenance or emitted references");
        const auto repeat = label + "-explicit";
        manualCase(fixture, repeat, budget, "", body, false,
                   {{"readable-names", "on"}, {"source-comments", "on"}, {"readable-dump", "on"}});
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file()) require(readFile(entry.path()) == readFile(artifact(repeat) / "model" / entry.path().filename()),
                                                "Repeated readable emission was not deterministic");
        const auto off = label + "-off";
        manualCase(fixture, off, budget, "", body, false,
                   {{"readable-names", "off"}, {"source-comments", "off"}, {"readable-dump", "off"}});
        const auto offDirectory = artifact(off) / "model";
        require(mappedValue(offDirectory, data) == "v" + std::to_string(data.index) &&
                mappedOperation(offDirectory, first) == "s" + std::to_string(first.index) &&
                generatedCpp(offDirectory).find("Phase 1 Input") == std::string::npos &&
                generatedCpp(offDirectory).find("origin unavailable/generated") == std::string::npos,
                "Readable naming/comment opt-outs were ignored");
        manualCase(fixture, label + "-frames", budget, "", body, false, {{"readable-frames", "on"}});
        manualCase(fixture, label + "-compact", budget, "", body, false, {{"readable-frames", "off"}});
        require(stamp.matches(graph) && checkpoint == json(fixture.design) &&
                sim::requireSinkSchedule(&fixture.session, graph).complete, "Readable emission modified its graph or schedule");
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
    testReadableFunctionWriter(); testReadableEmission();
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
                             "largeBeforeImageStore", "largeWholeBeforeStore", "largeWholeDirtyStore"}) {
        const auto begin = header.find(std::string("struct ") + store + "Type {");
        if (begin == std::string::npos) continue;
        const auto end = header.find("    };", begin);
        require(end != std::string::npos && header.substr(begin, end - begin).find("cells_") == std::string::npos,
                std::string("Named store was emitted through cells_: ") + store);
    }
    const auto scalarField = mappedOperation(artifact("named-model-stores") / "model",
                                            fixture.graph.findOperation("scalar0"));
    require(header.find("std::uint8_t " + scalarField + ";") != std::string::npos,
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
        {"-max-supernode-nodes=1", "-clone-shared=0"});
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
        "const auto completed=model.perf_counters().totalRoundCount-rounds; assert(completed>=1 && completed<=2);\n"
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

void testFirrtlLiteralFormats() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable"), data = fixture.input("data", 8);
    const auto letter = fixture.constant(8, "8'd90");
    const auto literal = [&](std::string text, const std::vector<grh::ValueId>& payload) {
        const auto format = fixture.compute(OperationKind::kConstant,
            {grh::ValueType::String, 1, false, grh::LogicDomain::TwoState, {}}, {});
        graph.setAttr(graph.valueDef(format), "constValue", std::move(text));
        const auto task = graph.createOperation(OperationKind::kSystemTask);
        for (const auto value : {enable, format}) graph.addOperand(task, value);
        for (const auto value : payload) graph.addOperand(task, value);
        graph.addOperand(task, clock);
        graph.setAttr(task, "name", std::string("write"));
        graph.setAttr(task, "firrtl.effectKind", std::string("printf"));
        graph.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
        return task;
    };
    literal("", {}); literal("", {data});
    literal("%%|quote=\"\\ @\t\r\nUTF-8:\xc3\xa9\001|%0d/%04h/%c\n", {data, data, letter});
    literal(std::string("NUL=%0d\0hidden=%0d", sizeof("NUL=%0d\0hidden=%0d") - 1), {data, data});
    literal("\n", {});
    std::string large(8192, 'L'); large += "|";
    const std::vector<grh::ValueId> arguments(80, data);
    for (std::size_t i = 0; i < arguments.size(); ++i) large += "%0d,";
    large += "\n"; literal(large, arguments);
    fixture.convert(); fixture.split(); fixture.partition();
    for (const auto value : graph.values())
        require(graph.valueType(value) != grh::ValueType::String, "literal printf retained String dataflow");
    const std::string support = R"CPP(
#include <cstdlib>
#include <iostream>
#include <new>
#include <sstream>
static bool forbidLiteralAllocations;
void* operator new(std::size_t bytes) {
    if(forbidLiteralAllocations) std::abort();
    if(void* memory=std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory,std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory,std::size_t) noexcept { std::free(memory); }
)CPP";
    const std::string body = R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); model.clock=false; model.enable=false; model.data=42; model.eval();
    forbidLiteralAllocations=true;
    for(unsigned step=0;step<32;++step) {
        model.clock=step&1; model.data=step; model.eval();
    }
    model.clock=false; model.enable=true; model.eval();
    model.data=42; model.eval(); // No edge, including enabled consumers.
    forbidLiteralAllocations=false;
    std::ostringstream output; auto* previous=std::cout.rdbuf(output.rdbuf());
    model.clock=true; model.eval();
    std::cout.rdbuf(previous);
    std::string expected="%|quote=\"\\ @\t\r\nUTF-8:\xc3\xa9\001|42/002a/Z\nNUL=42\n";
    expected+=std::string(8192,'L')+"|";
    for(unsigned i=0;i<80;++i) expected+="42,";
    expected+='\n'; assert(output.str()==expected);
    output.str(""); previous=std::cout.rdbuf(output.rdbuf());
    model.eval(); std::cout.rdbuf(previous); assert(output.str().empty());
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "firrtl-literal-formats-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false);
        const auto cpp = generatedCpp(artifact(label) / "model");
        require(cpp.find("TaskArg{'f', \"\"") != std::string::npos &&
                cpp.find("NUL=%0d\\000hidden=%0d\", 7, 0, false}") != std::string::npos &&
                cpp.find("static const std::string") == std::string::npos,
                "literal task did not use immutable bytes, empty presence or legacy NUL length");
    }

    Fixture interrupted; auto& interruptedGraph = interrupted.graph;
    const auto interruptedClock = interrupted.input("clock"), interruptedEnable = interrupted.input("enable");
    const auto orderedWrite = [&](const std::string& text) {
        const auto format = interrupted.compute(OperationKind::kConstant,
            {grh::ValueType::String, 1, false, grh::LogicDomain::TwoState, {}}, {});
        interruptedGraph.setAttr(interruptedGraph.valueDef(format), "constValue", text);
        const auto task = interrupted.task("write", {interruptedEnable, format, interruptedClock}, {"posedge"});
        interruptedGraph.setAttr(task, "firrtl.effectKind", std::string("printf"));
    };
    orderedWrite("before\n");
    const auto imported = interruptedGraph.createOperation(OperationKind::kDpicImport,
        interruptedGraph.internSymbol("literal_format_interrupt"));
    interruptedGraph.setAttr(imported, "argsName", std::vector<std::string>{});
    interruptedGraph.setAttr(imported, "argsDirection", std::vector<std::string>{});
    interruptedGraph.setAttr(imported, "argsWidth", std::vector<int64_t>{});
    interruptedGraph.setAttr(imported, "argsSigned", std::vector<bool>{});
    interruptedGraph.setAttr(imported, "hasReturn", false);
    const auto call = interruptedGraph.createOperation(OperationKind::kDpicCall);
    for (const auto value : {interruptedEnable, interruptedClock}) interruptedGraph.addOperand(call, value);
    interruptedGraph.setAttr(call, "targetImportSymbol", std::string("literal_format_interrupt"));
    interruptedGraph.setAttr(call, "eventEdge", std::vector<std::string>{"posedge"});
    orderedWrite("after\n");
    interrupted.convert(); interrupted.split(); interrupted.partition();
    const std::string interruptedSupport = R"CPP(
#include <iostream>
#include <sstream>
static bool interruptLiteralFormat;
extern "C" void literal_format_interrupt() {
    if(interruptLiteralFormat) throw std::runtime_error("expected literal-format interruption");
}
)CPP";
    const std::string interruptedBody = R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); model.enable=true; model.clock=false; model.eval();
    std::ostringstream output; auto* previous=std::cout.rdbuf(output.rdbuf());
    interruptLiteralFormat=true; model.clock=true; bool caught=false;
    try { model.eval(); } catch(const std::runtime_error&) { caught=true; }
    std::cout.rdbuf(previous); assert(caught && output.str()=="before\n");
    model.init(); model.enable=true; model.clock=false; model.eval();
    interruptLiteralFormat=false; output.str(""); previous=std::cout.rdbuf(output.rdbuf());
    model.clock=true; model.eval(); std::cout.rdbuf(previous);
    assert(output.str()=="before\nafter\n");
}
)CPP";
    for (const auto budget : {16, 1024})
        manualCase(interrupted, "firrtl-literal-interrupted-b" + std::to_string(budget), budget,
                   interruptedSupport, interruptedBody, false);
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
    const auto unclockedOwner = owner(graph, graph.valueDef(y)), clockedOwner = owner(graph, graph.valueDef(z));
    require(unclockedOwner.size() == 4 && clockedOwner.size() == 4 && unclockedOwner[2] != clockedOwner[2],
            "Unclocked DPI still shares the clocked DPI execution owner");
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
    // d and clk are not inputs to trigger_inc. A formerly shared owner
    // dragged this unclocked call along with trigger_dbl's data and fire.
    model.d = 3; model.eval(); assert(incCalls == 2 && dblCalls == 0);
    model.clk = true; model.eval(); assert(incCalls == 2 && dblCalls == 1 && model.o2 == 6);
    model.clk = false; model.eval(); assert(incCalls == 2 && dblCalls == 1);
    model.eval(); assert(incCalls == 2 && dblCalls == 1);
    model.d = 4; model.eval(); assert(incCalls == 2 && dblCalls == 1);
    model.clk = true; model.eval(); assert(incCalls == 2 && dblCalls == 2 && model.o2 == 8);
    model.a = 255; model.eval(); assert(incCalls == 3 && dblCalls == 2 && model.o == 0);
    model.eval(); assert(incCalls == 3 && dblCalls == 2);
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

void testSmallSinkBoundaries() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), otherClk = fixture.input("other_clk");
    const auto data = fixture.input("data", 8), raw = fixture.input("raw"), gate = fixture.input("gate");
    const auto shared = fixture.compute(OperationKind::kAnd, Fixture::type(1), {raw, gate});
    const auto one = fixture.constant(1, "1'b1"), zero = fixture.constant(1, "1'b0");
    std::vector<grh::ValueId> enables;
    std::vector<std::string> payloadNames;
    const auto outputs = graph.createOperation(OperationKind::kArrayCreate);
    for (unsigned i = 0; i < 70; ++i) {
        const auto enable = fixture.input("enable" + std::to_string(i)); enables.push_back(enable);
        const auto state = fixture.state("dense" + std::to_string(i), 8);
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, "8'd" + std::to_string(i)}});
        const auto payload = fixture.compute(OperationKind::kXor, Fixture::type(8),
            {data, fixture.constant(8, "8'd" + std::to_string(i))});
        payloadNames.emplace_back(graph.symbolText(graph.operationSymbol(graph.valueDef(payload))));
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, payload, clk});
        graph.addOperand(outputs, fixture.read(state));
    }
    const auto result = graph.createValue(Fixture::type(560, {70}));
    graph.addResult(outputs, result); graph.bindOutputPort("dense", result);
    const auto makeState = [&](const std::string& name, grh::ValueId enable, grh::ValueId payload) {
        const auto state = fixture.state(name, graph.valueWidth(payload));
        fixture.write(OperationKind::kRegisterWritePort, state, {enable, payload, clk});
        graph.bindOutputPort(name, fixture.read(state)); return state;
    };
    makeState("shared_enable", shared, data);
    makeState("shared_payload", one, shared);
    makeState("always_on", one, data);
    makeState("always_off", zero, data);
    const auto localEnable = fixture.compute(OperationKind::kEq, Fixture::type(1),
        {data, fixture.constant(8, "8'd7")});
    makeState("local_enable", localEnable, data);
    const auto priority = makeState("priority", enables[0], data);
    fixture.write(OperationKind::kRegisterWritePort, priority, {enables[1], fixture.constant(8, "8'd91"), otherClk});
    fixture.write(OperationKind::kRegisterWritePort, priority, {shared, fixture.constant(8, "8'd173"), clk});
    // A large consumer keeps shared in nonsink and requires its original
    // boundary view as well as the small enable and payload views.
    const auto large = fixture.state("large", 8192, {1024});
    fixture.write(OperationKind::kRegisterFillPort, large, {shared, data, clk});
    graph.bindOutputPort("large", fixture.read(large, {fixture.constant(10, "0")}));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    require(owner(graph, graph.valueDef(shared))[1] == "nonsink" &&
            owner(graph, graph.valueDef(localEnable))[1] == "small-sink", "Boundary test lost external/local enable cases");
    std::string support = "#include <vector>\nstatic void setEnables(GrhSIM_top& model, unsigned selected) {\n";
    for (unsigned i = 0; i < 70; ++i) support += "model.enable" + std::to_string(i) + " = selected == " + std::to_string(i) + ";\n";
    support += "}\n";
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); std::array<unsigned,70> expected{}, fires{};
    for (unsigned i = 0; i < 70; ++i) expected[i] = i;
    unsigned sharedValue=0, sharedPayload=0, always=0, local=0, priority=0, large=0;
    for (unsigned step = 0; step < 96; ++step) {
        // Keep enables/data unchanged on alternate edges. An unrelated bit
        // in the same word must not make another writer sample its payload.
        const unsigned selected = step % 8 < 4 ? 99 : (step / 8) % 3 == 0 ? 0 : (step / 8) % 3 == 1 ? 63 : 69;
        setEnables(model, selected); model.raw = step % 16 >= 8; model.gate = step % 24 >= 8;
        model.data = step < 16 ? 7 : (step / 4 * 13) & 255;
        model.clk = step & 1; model.other_clk = step & 1; model.eval();
        if (model.clk) {
            if (selected < 70) { expected[selected] = model.data ^ selected; ++fires[selected]; }
            if (model.raw && model.gate) sharedValue = large = model.data;
            sharedPayload = model.raw && model.gate; always = model.data;
            if (model.data == 7) local = model.data;
            if (selected == 0) priority = model.data;
            if (selected == 1) priority = 91;
            if (model.raw && model.gate) priority = 173;
        }
        for (unsigned i = 0; i < 70; ++i) assert(model.dense[i] == expected[i]);
        assert(model.shared_enable == sharedValue && model.shared_payload == sharedPayload);
        assert(model.always_on == always && model.always_off == 0 && model.local_enable == local);
        assert(model.priority == priority && model.large == large);
    }
    auto* stream = std::fopen("dense-counts.tsv", "w+"); assert(stream);
    model.dump_execution_counts(stream); std::rewind(stream); char line[256];
    assert(std::fgets(line,sizeof(line),stream)); assert(std::fgets(line,sizeof(line),stream));
    std::vector<unsigned long long> counts;
    while (std::fgets(line,sizeof(line),stream)) {
        unsigned index; unsigned long long count;
        if (std::sscanf(line,"%u\t%llu",&index,&count)!=2) break;
        assert(index==counts.size()); counts.push_back(count);
    }
    std::fclose(stream);
    for (unsigned i=0;i<70;++i) assert(counts.at(payloadSites[i])==fires[i]);
}
)CPP";
    for (const auto budget : {16, 1024}) for (const auto bitmap : {"on", "off"}) {
        const auto label = "sink-boundaries-" + std::string(bitmap) + "-b" + std::to_string(budget);
        manualCase(fixture,label,budget,support,body,false,{{"execution-counters","on"},{"mem-enable-bitmap",bitmap}});
        const auto modelPath = artifact(label) / "model";
        const auto header = readFile(modelPath / "grhsim_top.hpp");
        const auto member = mappedValue(modelPath, shared);
        std::string source;
        for (const auto& entry : std::filesystem::directory_iterator(modelPath))
            if (entry.path().extension() == ".cpp") source += readFile(entry.path());
        for (const auto store : {"smallSinkEnableBoundaryStore","smallSinkPayloadBoundaryStore","nonSinkBoundaryStore"})
            require(source.find(std::string(store)+"."+member)!=std::string::npos, "Missing shared boundary role: "+std::string(store));
        require((source.find("smallSinkEnableBoundaryStore.words[1]")!=std::string::npos)==(std::string(bitmap)=="on"),
                "Small sink bitmap did not cross its first word or honor off");
        const std::regex probe(R"REGEX(if \(\(\(smallSinkEnableBoundaryStore\.words\[\d+\] & UINT64_C\(\d+\)\))REGEX");
        require(std::regex_search(source,probe)==(std::string(bitmap)=="on"),
                "Small sink boundary bits were published but not used for batch skipping");
        if (std::string(bitmap)=="on") {
            const auto bit=1u; // shared has ordinal zero; enable0 follows it.
            require(source.find("if (((smallSinkEnableBoundaryStore.words[0] & UINT64_C("+
                        std::to_string(1u<<bit)+"))))")!=std::string::npos,
                    "Priority run probe included unrelated bits from the same word");
        }
        require(header.find("chunkFrame_pGeneral")==std::string::npos,"Small sink boundary introduced a General frame");
        const auto manifest = readFile(modelPath / "manifest.json");
        const std::regex sites(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
        std::map<std::string,unsigned> ordinals; unsigned ordinal=0;
        for (std::sregex_iterator it(manifest.begin(),manifest.end(),sites),end;it!=end;++it,++ordinal)
            if ((*it)[2]=="operation") ordinals.emplace((*it)[1],ordinal);
        std::string declaration="static const std::array<unsigned,70> payloadSites{";
        for (const auto& name : payloadNames) declaration+=std::to_string(ordinals.at(name))+",";
        declaration+="};\n";
        auto driver=readFile(artifact(label)/"driver.cpp"); driver.insert(driver.find("int main()"),declaration);
        std::ofstream(artifact(label)/"driver.cpp")<<driver;
    }
}

void testSmallSinkPublishedEnables() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clk = fixture.input("clk"), enable = fixture.input("enable"), data = fixture.input("data",8);
    const auto one = fixture.constant(1,"1'b1");
    const auto imported = graph.createOperation(OperationKind::kDpicImport,graph.internSymbol("sink_gate"));
    graph.setAttr(imported,"argsName",std::vector<std::string>{"enable"});
    graph.setAttr(imported,"argsDirection",std::vector<std::string>{"input"});
    graph.setAttr(imported,"argsWidth",std::vector<int64_t>{1});
    graph.setAttr(imported,"argsSigned",std::vector<bool>{false});
    graph.setAttr(imported,"hasReturn",true); graph.setAttr(imported,"returnWidth",int64_t{1});
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call,"targetImportSymbol",std::string("sink_gate"));
    graph.addOperand(call,one); graph.addOperand(call,enable);
    const auto sampledEnable = graph.createValue(Fixture::type(1)); graph.addResult(call,sampledEnable);
    const auto initial = fixture.state("initial_gate",1);
    grh::setStateInitSpec(graph,initial,{{grh::InitKind::Constant,"1'b1"}});
    const auto initializedEnable = fixture.read(initial);
    const auto negative = fixture.compute(OperationKind::kNot,Fixture::type(1),{enable});
    for (const auto& [name,guard] : std::vector<std::pair<std::string,grh::ValueId>>{
             {"sampled",sampledEnable},{"initialized",initializedEnable},{"inverted",negative}}) {
        for (unsigned i=0;i<2;++i) {
            const auto suffix=name+std::to_string(i);
            const auto state=fixture.state(suffix,8);
            grh::setStateInitSpec(graph,state,{{grh::InitKind::Constant,i?"8'd2":"8'd1"}});
            fixture.write(OperationKind::kRegisterWritePort,state,{guard,data,clk});
            graph.bindOutputPort(suffix,fixture.read(state));
        }
    }
    run(fixture.design,"grh-to-grhsim",fixture.session); fixture.split();
    run(fixture.design,"grhsim-partition-nonsink",fixture.session);
    run(fixture.design,"grhsim-partition-sink",fixture.session,{"-enable-guard-min-size=2"});
    const std::string support=R"CPP(
extern "C" std::uint8_t sink_gate(std::uint8_t enable) { return enable; }
)CPP";
    const std::string body=R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); unsigned sampled[2]={1,2},initialized[2]={1,2},inverted[2]={1,2};
    for(unsigned step=0;step<32;++step) {
        // The first eval after init already has an edge. Its initialized
        // and computed-true enables must be published before the probe.
        model.clk=!(step&1); model.enable=step%8>=4; model.data=step*7; model.eval();
        if(model.clk) for(unsigned i=0;i<2;++i) {
            initialized[i]=model.data;
            if(model.enable) sampled[i]=model.data; else inverted[i]=model.data;
        }
        assert(model.sampled0==sampled[0] && model.sampled1==sampled[1]);
        assert(model.initialized0==initialized[0] && model.initialized1==initialized[1]);
        assert(model.inverted0==inverted[0] && model.inverted1==inverted[1]);
    }
}
)CPP";
    for(const auto budget : {16,1024})
        manualCase(fixture,"sink-published-b"+std::to_string(budget),budget,support,body,false);
}

void testSmallShadowWritePriority() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("ss_clock");
    const auto enableA = fixture.input("ss_enable_a"), enableB = fixture.input("ss_enable_b");
    const auto enableC = fixture.input("ss_enable_c"), address = fixture.input("ss_address", 3);
    const auto dataA = fixture.input("ss_data_a", 8), dataB = fixture.input("ss_data_b", 8);
    const auto dataC = fixture.input("ss_data_c", 8), mask = fixture.input("ss_mask", 8);
    const auto one = fixture.constant(1, "1'b1");
    const auto scalar = fixture.state("small_shadow_scalar", 8);
    const auto whole = fixture.state("small_shadow_whole", 32, {4});
    const auto indexed = fixture.state("small_shadow_indexed", 32, {4});
    const auto gate = fixture.state("small_shadow_gate", 1);
    const auto sampled = fixture.state("small_shadow_sampled", 8), derived = fixture.state("small_shadow_derived", 8);
    const auto lowBit = [&](grh::ValueId value) {
        const auto result = fixture.compute(OperationKind::kSliceStatic, Fixture::type(1), {value});
        graph.setAttr(graph.valueDef(result), "sliceStart", int64_t{0});
        graph.setAttr(graph.valueDef(result), "sliceEnd", int64_t{0});
        return result;
    };
    const std::array<grh::ValueId, 3> enables{enableA, enableB, enableC}, data{dataA, dataB, dataC};
    for (std::size_t i = 0; i < data.size(); ++i) {
        const auto payload = fixture.compute(OperationKind::kArrayCreate, Fixture::type(32, {4}),
            {data[i], data[i], data[i], data[i]});
        fixture.write(OperationKind::kRegisterWritePort, whole, {enables[i], payload, clock});
        if (i != 2) {
            fixture.write(OperationKind::kRegisterWritePort, scalar, {enables[i], data[i], clock});
            fixture.write(OperationKind::kRegisterWriteArrayPort, indexed, {enables[i], address, data[i], clock});
            fixture.write(OperationKind::kRegisterWritePort, gate, {enables[i], lowBit(data[i]), clock});
        } else {
            fixture.write(OperationKind::kRegisterWriteMaskPort, scalar, {enables[i], data[i], mask, clock});
            fixture.write(OperationKind::kRegisterWriteArrayMaskPort, indexed, {enables[i], address, data[i], mask, clock});
            fixture.write(OperationKind::kRegisterWriteMaskPort, gate, {enables[i], lowBit(data[i]), lowBit(mask), clock});
        }
    }
    const auto scalarRead = fixture.read(scalar), gateRead = fixture.read(gate);
    fixture.write(OperationKind::kRegisterWritePort, sampled, {one, scalarRead, clock});
    fixture.write(OperationKind::kRegisterWritePort, derived, {one, scalarRead, gateRead});
    for (const auto& [name, state] : std::vector<std::pair<std::string, grh::OperationId>>{
            {"ss_scalar", scalar}, {"ss_whole", whole}, {"ss_indexed", indexed},
            {"ss_gate", gate}, {"ss_sampled", sampled}, {"ss_derived", derived}})
        graph.bindOutputPort(name, fixture.read(state));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=4096"});
    fixture.partition();
    for (const auto state : {scalar, whole, indexed, gate, sampled, derived})
        require(sim::attribute<std::string>(graph, state, "grhsim.statePolicy")==std::optional<std::string>("small-sink"),
                "Small shadow regression lost its current/next state policy");
    const std::string body = R"CPP(
for (unsigned epoch=0; epoch<3; ++epoch) {
    model.init(); unsigned value=0, whole=0, sampled=0, derived=0;
    std::array<unsigned,4> cells{};
    const auto check = [&] {
        assert(model.ss_scalar==value && model.ss_gate==(value&1));
        assert(model.ss_sampled==sampled && model.ss_derived==derived);
        for (unsigned i=0;i<4;++i) assert(model.ss_whole[i]==whole && model.ss_indexed[i]==cells[i]);
    };
    model.eval(); check();
    const auto pulse = [&](unsigned flags,unsigned a,unsigned b,unsigned c,unsigned bits,unsigned address) {
        model.ss_clock=0; model.ss_enable_a=flags&1; model.ss_enable_b=(flags>>1)&1;
        model.ss_enable_c=(flags>>2)&1; model.ss_data_a=a; model.ss_data_b=b;
        model.ss_data_c=c; model.ss_mask=bits; model.ss_address=address; model.eval(); check();
        const auto old=value; sampled=old;
        if(flags&1) { value=a; whole=a; if(address<4) cells[address]=a; }
        if(flags&2) { value=b; whole=b; if(address<4) cells[address]=b; }
        if(flags&4) {
            value=(value&(~bits&255))|(c&bits); whole=c;
            if(address<4) cells[address]=(cells[address]&(~bits&255))|(c&bits);
        }
        if(!(old&1) && (value&1)) derived=value;
        const auto rounds=model.perf_counters().totalRoundCount;
        model.ss_clock=1; model.eval(); check();
        const auto elapsed=model.perf_counters().totalRoundCount-rounds;
        const auto writes=model.perf_counters().touchedWriteCount;
        model.eval(); check(); assert(model.perf_counters().touchedWriteCount==writes);
        return elapsed;
    };
    assert(pulse(7,0,0,0,0,0)==1); // Same-value writers and zero mask are no-ops.
    assert(pulse(7,0,9,0,255,0)==1); // A/B/A restores current; no derived posedge.
    pulse(7,0,7,7,255,0); // A/B/B keeps the pending value and raises the derived gate.
    pulse(4,0,0,99,0,0); // Mask zero preserves scalar, gate and indexed state.
    pulse(7,7,42,7,255,1); // Restoration to current after a different pending value.
    pulse(7,7,40,40,255,2); // Derived gate falls; same-edge sample still sees old 7.
    pulse(7,40,73,73,255,3); // New derived edge observes the committed value 73.
    pulse(4,0,0,162,15,7); // Partial mask; out-of-range indexed write is ignored.
}
)CPP";
    for (const auto budget : {16, 1024})
        manualCase(fixture, "small-shadow-priority-b"+std::to_string(budget), budget, "", body, false);
}

// Bind execution oracles to real operation sites, independently of bitmap layout.
void bindSinkActivitySites(const std::string& label,
                           const std::vector<std::pair<std::string, std::string>>& requested,
                           std::size_t runs, std::size_t targets, std::size_t maxTargetFanout,
                           bool noPublicationFrontier = false) {
    const auto directory = artifact(label) / "model";
    const auto stats = readFile(directory / "sink-activity-stats.json");
    const auto statistic = [&](std::string_view name) {
        const auto position = stats.find("\"" + std::string(name) + "\"");
        const auto colon = position == std::string::npos ? position : stats.find(':', position);
        require(colon != std::string::npos, "Missing sink activity statistic " + std::string(name));
        return std::stoull(stats.substr(colon + 1));
    };
    require(statistic("eligible_runs") == runs && statistic("eligible_writers") == runs &&
                statistic("rejected_runs") == 0 && statistic("rejected_writers") == 0,
            "Sink activity regression silently fell back to unconditional writers");
    require(statistic("subscribed_targets") == targets && statistic("unique_target_run_pairs") == runs &&
                statistic("max_target_fanout") == maxTargetFanout && statistic("bitmap_words") == 1 &&
                statistic("input_change_sources") >= 3 && statistic("shadow_change_mask_sites") >= runs,
            "Sink activity regression lost its input or competing-target invalidation routes");
    if (noPublicationFrontier)
        require(statistic("publication_change_sources") == 0,
                "Initial sink dirtiness was masked by a nonsink constant publication");
    const auto sites = mappedRecords(readFile(directory / "manifest.json"), "sites");
    std::string declaration = "static const std::array<unsigned," + std::to_string(requested.size()) + "> activitySites{";
    for (const auto& [operation, category] : requested) {
        std::optional<unsigned> ordinal;
        for (unsigned index = 0; index < sites.size(); ++index) {
            const auto& site = sites[index];
            if (site.find("\"operation\":\"" + operation + "\"") == std::string::npos ||
                site.find("\"category\":\"" + category + "\"") == std::string::npos) continue;
            require(!ordinal.has_value(), "Sink activity operation has multiple counter sites");
            ordinal = index;
        }
        require(ordinal.has_value(), "Sink activity operation lacks its actual execution counter: " + operation);
        declaration += std::to_string(*ordinal) + ",";
    }
    declaration += "};";
    const auto path = artifact(label) / "driver.cpp";
    auto driver = readFile(path);
    const auto placeholder = "static const std::array<unsigned," + std::to_string(requested.size()) + "> activitySites{};";
    const auto position = driver.find(placeholder);
    require(position != std::string::npos, "Sink activity driver lost its counter declaration");
    driver.replace(position, placeholder.size(), declaration);
    std::ofstream(path) << driver;
}

std::string sinkActivityCounterSupport() {
    return R"CPP(
#include <vector>
static const std::array<unsigned,6> activitySites{};
static std::array<unsigned long long,6> activityCounts(GrhSIM_top& instance) {
    auto* stream=std::fopen("activity-counts.tsv","w+"); assert(stream);
    instance.dump_execution_counts(stream); std::rewind(stream); char line[256];
    assert(std::fgets(line,sizeof(line),stream)); assert(std::fgets(line,sizeof(line),stream));
    std::vector<unsigned long long> all;
    while(std::fgets(line,sizeof(line),stream)) {
        unsigned index; unsigned long long count;
        if(std::sscanf(line,"%u\t%llu",&index,&count)!=2) break;
        assert(index==all.size()); all.push_back(count);
    }
    std::fclose(stream); std::array<unsigned long long,6> selected{};
    for(unsigned i=0;i<selected.size();++i) selected[i]=all.at(activitySites[i]);
    return selected;
}
static void activityDelta(const std::array<unsigned long long,6>& before,
                          const std::array<unsigned long long,6>& after,
                          const std::array<unsigned,3>& expected) {
    for(unsigned i=0;i<expected.size();++i) {
        assert(after[i*2]==before[i*2]+expected[i]);
        assert(after[i*2+1]==before[i*2+1]+expected[i]);
    }
}
)CPP";
}

void testSmallSinkActivityExactRuns() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("activity_clock_a"), clockB = fixture.input("activity_clock_b");
    std::array<grh::ValueId,3> payloads;
    std::array<grh::OperationId,3> states;
    const std::array<std::string,3> names{"q", "peer", "extra"};
    for (unsigned i = 0; i < names.size(); ++i) {
        const auto data = fixture.input("activity_data_" + names[i], 8);
        payloads[i] = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, data});
        states[i] = fixture.state("activity_" + names[i], 8);
        grh::setStateInitSpec(graph, states[i], {{grh::InitKind::Constant, "8'd" + std::to_string(17 + i * 2)}});
        // Private true constants keep the first zero-data edge independent of
        // any nonsink constant publication that could mask missing init bits.
        const auto one = fixture.constant(1, "1'b1");
        fixture.write(OperationKind::kRegisterWritePort, states[i], {one, payloads[i], i == 1 ? clockB : clockA});
        graph.bindOutputPort("activity_" + names[i], fixture.read(states[i]));
    }
    run(fixture.design,"grh-to-grhsim",fixture.session);
    fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == 3 && schedule.generalRuns[0].partitionKey == schedule.generalRuns[2].partitionKey &&
                schedule.generalRuns[0].partitionKey != schedule.generalRuns[1].partitionKey,
            "Sink activity exact-run regression lost its repeated A/B/A owner");
    std::vector<std::pair<std::string,std::string>> sites;
    for (unsigned i = 0; i < payloads.size(); ++i) {
        const auto& run = schedule.generalRuns[i];
        require(run.orderedWriters.size() == 1 && sim::writerTarget(graph, run.orderedWriters.front()) == states[i],
                "Sink activity exact-run regression changed writer positions");
        const auto enable = graph.valueDef(graph.opOperands(run.orderedWriters.front())[1]);
        require(enable && graph.opKind(enable) == OperationKind::kConstant && owner(graph,enable) == run.partitionKey,
                "Sink activity init regression lost its private true enable");
        const auto payload = graph.valueDef(payloads[i]);
        require(graph.opKind(payload) == OperationKind::kAdd && owner(graph, payload) == run.partitionKey,
                "Sink activity exact-run regression lost its private counted Add");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(run.orderedWriters.front())), "writer-sample");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(payload)), "operation");
    }
    const std::string body = R"CPP(
const auto evaluate = [&](GrhSIM_top& instance,unsigned clocks,std::array<unsigned,3> work,
                          std::array<unsigned,3> values) {
    const auto before=activityCounts(instance);
    instance.activity_clock_a=clocks&1; instance.activity_clock_b=(clocks>>1)&1; instance.eval();
    const auto after=activityCounts(instance); activityDelta(before,after,work);
    assert(instance.activity_q==values[0] && instance.activity_peer==values[1] && instance.activity_extra==values[2]);
    instance.eval(); assert(activityCounts(instance)==after);
    assert(instance.activity_q==values[0] && instance.activity_peer==values[1] && instance.activity_extra==values[2]);
};
model.init();
evaluate(model,0,{0,0,0},{17,19,21});
// Zero data matches its input snapshot. Init must still execute both A runs.
evaluate(model,1,{1,0,1},{0,19,0});
evaluate(model,0,{0,0,0},{0,19,0}); evaluate(model,1,{0,0,0},{0,19,0});
evaluate(model,2,{0,1,0},{0,0,0});
evaluate(model,0,{0,0,0},{0,0,0}); evaluate(model,2,{0,0,0},{0,0,0});
model.activity_data_extra=11;
evaluate(model,0,{0,0,0},{0,0,0}); evaluate(model,1,{0,0,1},{0,0,22});
// Changing q data without a fresh edge must retain its own exact run bit.
model.activity_data_q=5; evaluate(model,1,{0,0,0},{0,0,22});
evaluate(model,0,{0,0,0},{0,0,22}); evaluate(model,1,{1,0,0},{10,0,22});
evaluate(model,0,{0,0,0},{10,0,22}); evaluate(model,3,{0,0,0},{10,0,22});
// A second instance begins dirty while the first remains clean.
GrhSIM_top other; other.init();
evaluate(other,0,{0,0,0},{17,19,21}); evaluate(other,1,{1,0,1},{0,19,0});
evaluate(model,0,{0,0,0},{10,0,22}); evaluate(model,1,{0,0,0},{10,0,22});
evaluate(other,0,{0,0,0},{0,19,0}); evaluate(other,1,{0,0,0},{0,19,0});
// Reinitializing only other must rerun its unchanged zero-data writers.
other.activity_clock_a=0; other.activity_clock_b=0; other.init();
evaluate(other,0,{0,0,0},{17,19,21}); evaluate(other,1,{1,0,1},{0,19,0});
evaluate(model,0,{0,0,0},{10,0,22}); evaluate(model,1,{0,0,0},{10,0,22});
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label = "sink-activity-exact-runs-b" + std::to_string(budget);
        manualCase(fixture,label,budget,sinkActivityCounterSupport(),body,false,{{"execution-counters","on"}});
        bindSinkActivitySites(label,sites,3,3,1,true);
    }
}

void testSmallSinkActivityCompetition() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("activity_clock_a"), clockB = fixture.input("activity_clock_b");
    const auto enableA = fixture.input("activity_enable_a"), enableC = fixture.input("activity_enable_c");
    const auto one = fixture.constant(1, "1'b1");
    const auto state = fixture.state("activity_q", 8);
    grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant,"8'd17"}});
    std::array<grh::ValueId,3> payloads;
    const std::array<std::string,3> names{"a", "b", "c"};
    for (unsigned i = 0; i < names.size(); ++i) {
        const auto data = fixture.input("activity_data_" + names[i], 8);
        payloads[i] = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, data});
        fixture.write(OperationKind::kRegisterWritePort,state,
            {i == 0 ? enableA : i == 1 ? one : enableC,payloads[i],i == 1 ? clockB : clockA});
    }
    graph.bindOutputPort("activity_q",fixture.read(state));
    fixture.convert(); fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size() == 3 && schedule.generalRuns[0].partitionKey != schedule.generalRuns[2].partitionKey,
            "Sink activity competition regression lost its distinct writer priorities");
    std::vector<std::pair<std::string,std::string>> sites;
    for (unsigned i = 0; i < payloads.size(); ++i) {
        const auto& run = schedule.generalRuns[i];
        require(run.orderedWriters.size() == 1 && sim::writerTarget(graph,run.orderedWriters.front()) == state,
                "Sink activity competition regression changed its shared target");
        const auto payload = graph.valueDef(payloads[i]);
        require(graph.opKind(payload) == OperationKind::kAdd && owner(graph,payload) == run.partitionKey,
                "Sink activity competition regression lost its private counted Add");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(run.orderedWriters.front())),"writer-sample");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(payload)),"operation");
    }
    const std::string body = R"CPP(
const auto evaluate = [&](GrhSIM_top& instance,unsigned clocks,std::array<unsigned,3> work,unsigned value) {
    const auto before=activityCounts(instance);
    const auto rounds=instance.perf_counters().totalRoundCount;
    instance.activity_clock_a=clocks&1; instance.activity_clock_b=(clocks>>1)&1; instance.eval();
    const auto elapsed=instance.perf_counters().totalRoundCount-rounds;
    const auto after=activityCounts(instance); activityDelta(before,after,work);
    assert(instance.activity_q==value); instance.eval();
    assert(instance.activity_q==value && activityCounts(instance)==after); return elapsed;
};
model.init(); model.activity_enable_a=1;
evaluate(model,0,{0,0,0},17); evaluate(model,1,{1,0,0},0);
evaluate(model,0,{0,0,0},0); evaluate(model,1,{0,0,0},0);
model.activity_data_a=4; evaluate(model,1,{0,0,0},0);
evaluate(model,0,{0,0,0},0); evaluate(model,1,{1,0,0},8);
model.activity_data_b=7; evaluate(model,0,{0,0,0},8);
evaluate(model,2,{0,1,0},14); evaluate(model,0,{0,0,0},14);
// B changed the shared target. Unchanged A must run at the next A edge.
evaluate(model,1,{1,0,0},8); evaluate(model,0,{0,0,0},8);
evaluate(model,2,{0,1,0},14); evaluate(model,0,{0,0,0},14);
// A/B restores current B: both actual pending-shadow changes invalidate peers.
for(unsigned repeat=0;repeat<3;++repeat) {
    assert(evaluate(model,3,{1,1,0},14)==1);
    evaluate(model,0,{0,0,0},14);
}
// Enable the third writer to exercise A/B/A restoration with counted payloads.
model.activity_enable_c=1; model.activity_data_c=4;
evaluate(model,0,{0,0,0},14); evaluate(model,3,{1,1,1},8);
for(unsigned repeat=0;repeat<3;++repeat) {
    evaluate(model,0,{0,0,0},8);
    assert(evaluate(model,3,{1,1,1},8)==1);
}
// Neither a false enable nor a non-edge eval may discard pending work.
model.activity_enable_a=0; model.activity_enable_c=0;
model.activity_data_a=9; model.activity_data_c=11;
evaluate(model,0,{0,0,0},8); evaluate(model,1,{0,0,0},8);
evaluate(model,0,{0,0,0},8); evaluate(model,1,{0,0,0},8);
model.activity_enable_a=1; evaluate(model,1,{0,0,0},8);
evaluate(model,0,{0,0,0},8); evaluate(model,1,{1,0,0},18);
evaluate(model,0,{0,0,0},18); evaluate(model,2,{0,1,0},14);
evaluate(model,0,{0,0,0},14); evaluate(model,2,{0,0,0},14);
// Separate instances carry separate dirty bits and counters through init.
GrhSIM_top other; other.init(); other.activity_enable_a=1;
evaluate(other,0,{0,0,0},17); evaluate(other,1,{1,0,0},0);
evaluate(other,0,{0,0,0},0); evaluate(other,1,{0,0,0},0);
evaluate(model,0,{0,0,0},14); evaluate(model,1,{1,0,0},18);
evaluate(model,0,{0,0,0},18); evaluate(model,1,{0,0,0},18);
other.activity_clock_a=0; other.activity_clock_b=0; other.init();
other.activity_enable_a=1;
evaluate(other,0,{0,0,0},17); evaluate(other,1,{1,0,0},0);
evaluate(model,0,{0,0,0},18); evaluate(model,1,{0,0,0},18);
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label = "sink-activity-competition-b" + std::to_string(budget);
        manualCase(fixture,label,budget,sinkActivityCounterSupport(),body,false,{{"execution-counters","on"}});
        bindSinkActivitySites(label,sites,3,1,3);
    }
}

void testSmallSinkActivityInterruptedFallback() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("fallback_clock"), fireClock = fixture.input("fallback_fire_clock");
    const auto data = fixture.input("fallback_data",8), dummy = fixture.input("fallback_dummy");
    const auto one = fixture.constant(1,"1'b1"), mask = fixture.constant(8,"8'd90");
    const auto published = fixture.compute(OperationKind::kXor,Fixture::type(8),{data,mask});
    const auto imported = graph.createOperation(OperationKind::kDpicImport,graph.internSymbol("sink_activity_throw_once"));
    graph.setAttr(imported,"argsName",std::vector<std::string>{"published"});
    graph.setAttr(imported,"argsDirection",std::vector<std::string>{"input"});
    graph.setAttr(imported,"argsWidth",std::vector<int64_t>{8});
    graph.setAttr(imported,"argsSigned",std::vector<bool>{false});
    graph.setAttr(imported,"hasReturn",false);
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call,"targetImportSymbol",std::string("sink_activity_throw_once"));
    graph.addOperand(call,one); graph.addOperand(call,published);
    const auto payload = fixture.compute(OperationKind::kAdd,Fixture::type(8),{published,published});
    const auto peer = fixture.state("fallback_peer",8), fireState = fixture.state("fallback_fire",1);
    grh::setStateInitSpec(graph,peer,{{grh::InitKind::Constant,"8'd17"}});
    fixture.write(OperationKind::kRegisterWritePort,peer,{one,payload,clock});
    const auto fireWriter = fixture.write(OperationKind::kRegisterWritePort,fireState,{one,dummy,fireClock});
    graph.bindOutputPort("fallback_peer",fixture.read(peer));
    graph.bindOutputPort("fallback_fire",fixture.read(fireState));
    run(fixture.design,"grh-to-grhsim",fixture.session);
    // Fire used as payload must retain the original per-edge execution path.
    graph.replaceOperand(fireWriter,2,graph.opOperands(fireWriter)[0]);
    fixture.split();
    run(fixture.design,"grhsim-partition-nonsink",fixture.session,{"-clone-shared=0"});
    for (const auto op : {graph.valueDef(one),graph.valueDef(mask),graph.valueDef(published),call}) {
        require(owner(graph,op).size()==4 && owner(graph,op)[1]=="nonsink",
                "Interrupted sink activity fixture lost its publication source");
        graph.setAttr(op,sim::kPartition,Owner{"general","nonsink","sink_activity_throw_source","source"});
    }
    run(fixture.design,"grhsim-partition-sink",fixture.session);
    const auto& schedule = sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==2,"Interrupted sink activity fixture lost its two exact runs");
    grh::OperationId peerWriter;
    for (const auto& run : schedule.generalRuns) for (const auto writer : run.orderedWriters)
        if (sim::writerTarget(graph,writer)==peer) {
            peerWriter=writer;
            require(owner(graph,graph.valueDef(payload))==run.partitionKey,
                    "Interrupted sink activity fixture lost its private counted Add");
        }
    require(bool(peerWriter),"Interrupted sink activity fixture lost its eligible peer writer");
    const std::string support = R"CPP(
static bool sinkActivityThrow;
static unsigned sinkActivityCalls;
extern "C" void sink_activity_throw_once(std::uint8_t published) {
    assert(published==90 || published==93); ++sinkActivityCalls;
    if(sinkActivityThrow) { sinkActivityThrow=false; throw std::runtime_error("expected sink activity interruption"); }
}
static const std::array<unsigned,3> fallbackSites{};
static std::array<unsigned long long,3> fallbackCounts(GrhSIM_top& instance) {
    auto* stream=std::fopen("fallback-counts.tsv","w+"); assert(stream);
    instance.dump_execution_counts(stream); std::rewind(stream); char line[256];
    assert(std::fgets(line,sizeof(line),stream) && std::fgets(line,sizeof(line),stream));
    std::array<unsigned long long,3> counts{}; std::array<bool,3> found{};
    while(std::fgets(line,sizeof(line),stream)) {
        unsigned site; unsigned long long count;
        if(std::sscanf(line,"%u\t%llu",&site,&count)!=2) continue;
        for(unsigned i=0;i<counts.size();++i) if(site==fallbackSites[i]) { counts[i]=count; found[i]=true; }
    }
    std::fclose(stream); assert(found[0] && found[1] && found[2]); return counts;
}
)CPP";
    const std::string body = R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); sinkActivityThrow=false; sinkActivityCalls=0;
    const auto evaluate = [&](unsigned clocks,unsigned peerWork,unsigned fireWork,unsigned value) {
        const auto before=fallbackCounts(model);
        model.fallback_clock=clocks&1; model.fallback_fire_clock=(clocks>>1)&1; model.eval();
        const auto after=fallbackCounts(model);
        assert(after[0]==before[0]+peerWork && after[1]==before[1]+peerWork && after[2]==before[2]+fireWork);
        assert(model.fallback_peer==value && model.fallback_fire==(fireWork || before[2]!=0));
        model.eval(); assert(fallbackCounts(model)==after);
    };
    evaluate(0,0,0,17); assert(sinkActivityCalls==1);
    evaluate(1,1,0,180); evaluate(0,0,0,180); evaluate(1,0,0,180);
    // A rejected fire-as-data writer must execute at every fresh edge even
    // when its stored state and all ordinary data inputs remain unchanged.
    evaluate(0,0,0,180); evaluate(2,0,1,180);
    evaluate(0,0,0,180); evaluate(2,0,1,180); evaluate(0,0,0,180);
    const auto before=fallbackCounts(model); const auto calls=sinkActivityCalls;
    model.fallback_data=7; model.fallback_clock=1; sinkActivityThrow=true; bool caught=false;
    try { model.eval(); } catch(const std::runtime_error& error) {
        caught=std::string(error.what())=="expected sink activity interruption";
    }
    assert(caught && sinkActivityCalls==calls+1 && model.fallback_peer==180);
    assert(fallbackCounts(model)==before);
    // Event consumed this posedge before the source threw. Retrying unchanged
    // inputs must not replay it, but must preserve the newly dirtied peer.
    model.eval(); assert(sinkActivityCalls==calls+1 && model.fallback_peer==180);
    assert(fallbackCounts(model)==before);
    evaluate(0,0,0,180); evaluate(1,1,0,186);
    evaluate(0,0,0,186); evaluate(1,0,0,186);
}
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label = "sink-activity-interrupted-fallback-b"+std::to_string(budget);
        manualCase(fixture,label,budget,support,body,false,{{"execution-counters","on"}});
        const auto directory = artifact(label)/"model";
        const auto stats = readFile(directory/"sink-activity-stats.json");
        require(stats.find("\"eligible_runs\": 1")!=std::string::npos &&
                    stats.find("\"eligible_writers\": 1")!=std::string::npos &&
                    stats.find("\"rejected_runs\": 1")!=std::string::npos &&
                    stats.find("\"fire-as-data\": {\"runs\": 1, \"writers\": 1}")!=std::string::npos &&
                    stats.find("\"publication_change_sources\": 2")!=std::string::npos,
                "Interrupted sink activity regression lost its eligible peer or fire-as-data fallback");
        const auto functions = mappedRecords(readFile(directory/"code-map.json"),"functions");
        const auto root = [&](grh::OperationId op) {
            const auto id="\"o"+std::to_string(op.index)+"\"";
            for (const auto& function : functions) if (function.find(id)!=std::string::npos) {
                std::smatch match;
                require(std::regex_search(function,match,std::regex(R"REGEX("root_function":"([^"]+)")REGEX")),
                        "Interrupted sink activity operation omitted its root");
                return match[1].str();
            }
            throw std::runtime_error("Interrupted sink activity operation lost its generated function");
        };
        require(root(graph.valueDef(published))==root(call) && root(call).starts_with("sn_"),
                "Interrupted sink activity publication no longer precedes the call in one nonsink owner");
        const auto source = generatedCpp(directory);
        const auto guard = source.find("if (smallSinkPayloadBoundaryStore."+mappedValue(directory,published)+" != ");
        require(guard!=std::string::npos && source.substr(guard,source.find('}',guard)-guard).find("sinkDirty[")!=std::string::npos,
                "Interrupted sink activity publication lost its immediate dirty route");
        const std::array<std::pair<grh::OperationId,std::string>,3> requested{{
            {peerWriter,"writer-sample"},{graph.valueDef(payload),"operation"},{fireWriter,"writer-sample"}}};
        const auto sites = mappedRecords(readFile(directory/"manifest.json"),"sites");
        std::string declaration="static const std::array<unsigned,3> fallbackSites{";
        for (const auto& [op,category] : requested) {
            std::optional<unsigned> ordinal;
            const auto operation="\"operation\":\""+std::string(graph.symbolText(graph.operationSymbol(op)))+"\"";
            for (unsigned i=0;i<sites.size();++i)
                if (sites[i].find(operation)!=std::string::npos && sites[i].find("\"category\":\""+category+"\"")!=std::string::npos) {
                    require(!ordinal,"Interrupted sink activity operation has duplicate counter sites"); ordinal=i;
                }
            require(ordinal.has_value(),"Interrupted sink activity operation lacks its actual counter site");
            declaration+=std::to_string(*ordinal)+",";
        }
        declaration+="};";
        const auto path = artifact(label)/"driver.cpp"; auto driver=readFile(path);
        const std::string placeholder="static const std::array<unsigned,3> fallbackSites{};";
        const auto position=driver.find(placeholder); require(position!=std::string::npos,"Missing fallback counter declaration");
        driver.replace(position,placeholder.size(),declaration); std::ofstream(path)<<driver;
    }
}

std::string batchActivityCounterSupport(std::size_t count) {
    return "static const std::array<unsigned,"+std::to_string(count)+"> batchSites{};\n"+R"CPP(
static auto batchCounts(GrhSIM_top& instance) {
    auto* stream=std::fopen("batch-activity-counts.tsv","w+"); assert(stream);
    instance.dump_execution_counts(stream); std::rewind(stream); char line[256];
    assert(std::fgets(line,sizeof(line),stream) && std::fgets(line,sizeof(line),stream));
    std::array<unsigned long long,batchSites.size()> counts{};
    std::array<bool,batchSites.size()> found{};
    while(std::fgets(line,sizeof(line),stream)) {
        unsigned site; unsigned long long count;
        if(std::sscanf(line,"%u\t%llu",&site,&count)!=2) continue;
        for(unsigned i=0;i<counts.size();++i) if(site==batchSites[i]) { counts[i]=count; found[i]=true; }
    }
    std::fclose(stream); for(const auto present : found) assert(present); return counts;
}
)CPP";
}

void bindBatchActivityCounters(const std::string& label, const grh::Graph& graph,
                              const std::vector<std::pair<grh::OperationId,std::string>>& requested) {
    const auto directory=artifact(label)/"model";
    const auto sites=mappedRecords(readFile(directory/"manifest.json"),"sites");
    std::string declaration="static const std::array<unsigned,"+std::to_string(requested.size())+"> batchSites{";
    for(const auto& [op,category] : requested) {
        std::optional<unsigned> ordinal;
        const auto operation="\"operation\":\""+std::string(graph.symbolText(graph.operationSymbol(op)))+"\"";
        for(unsigned i=0;i<sites.size();++i)
            if(sites[i].find(operation)!=std::string::npos && sites[i].find("\"category\":\""+category+"\"")!=std::string::npos) {
                require(!ordinal,"Batch activity operation has duplicate counter sites"); ordinal=i;
            }
        require(ordinal.has_value(),"Batch activity operation lacks its actual counter site");
        declaration+=std::to_string(*ordinal)+",";
    }
    declaration+="};";
    const auto path=artifact(label)/"driver.cpp"; auto driver=readFile(path);
    const auto placeholder="static const std::array<unsigned,"+std::to_string(requested.size())+"> batchSites{};";
    const auto position=driver.find(placeholder); require(position!=std::string::npos,"Missing batch activity counter declaration");
    driver.replace(position,placeholder.size(),declaration); std::ofstream(path)<<driver;
}

std::string batchActivityRoot(const std::filesystem::path& directory, grh::OperationId op) {
    const auto functions=mappedRecords(readFile(directory/"code-map.json"),"functions");
    const auto id="\"o"+std::to_string(op.index)+"\"";
    for(const auto& function : functions) if(function.find(id)!=std::string::npos) {
        std::smatch match;
        require(std::regex_search(function,match,std::regex(R"REGEX("root_function":"([^"]+)")REGEX")),
                "Batch activity operation omitted its root function");
        return match[1].str();
    }
    throw std::runtime_error("Batch activity operation lost its generated function");
}

std::string batchActivityRootCpp(const std::filesystem::path& directory, const std::string& root) {
    const auto functions=mappedRecords(readFile(directory/"code-map.json"),"functions");
    std::string result;
    for(const auto& function : functions) {
        if(function.find("\"root_function\":\""+root+"\"")==std::string::npos) continue;
        std::smatch match;
        require(std::regex_search(function,match,std::regex(R"REGEX("name":"([^"]+)")REGEX")),"Batch helper omitted its name");
        const auto name=match[1].str();
        require(std::regex_search(function,match,std::regex(R"REGEX("source":"([^"]+)")REGEX")),"Batch helper omitted its source");
        const auto cpp=readFile(directory/match[1].str());
        const auto begin=cpp.find("GrhSIM_top::"+name+"(");
        const auto end=begin==std::string::npos ? begin : cpp.find("\n}\n",begin);
        require(begin!=std::string::npos && end!=std::string::npos,"Missing batch helper body");
        result+=cpp.substr(begin,end-begin)+'\n';
    }
    require(!result.empty(),"Missing complete batch root source"); return result;
}

std::uint64_t batchActivityStatistic(const std::filesystem::path& directory,std::string_view name) {
    const auto stats=readFile(directory/"sink-activity-stats.json");
    const auto position=stats.find("\""+std::string(name)+"\"");
    const auto colon=position==std::string::npos ? position : stats.find(':',position);
    require(colon!=std::string::npos,"Missing batch activity statistic");
    return std::stoull(stats.substr(colon+1));
}

struct WriterDispatchCall {
    std::string name;
    std::vector<std::string> guards;
};

// Follow actual split driver calls, retaining enclosing guards and loop order.
std::vector<WriterDispatchCall> writerDispatchCalls(const std::filesystem::path& directory) {
    std::map<std::string, std::string> bodies;
    for (const auto& function : mappedRecords(readFile(directory / "code-map.json"), "functions")) {
        if (function.find("\"root_function\":\"pGeneral\"") == std::string::npos) continue;
        std::smatch match;
        require(std::regex_search(function, match, std::regex(R"REGEX("name":"([^"]+)")REGEX")),
                "General dispatch omitted a function name");
        const auto name = match[1].str();
        require(std::regex_search(function, match, std::regex(R"REGEX("source":"([^"]+)")REGEX")),
                "General dispatch omitted a source file");
        const auto cpp = readFile(directory / match[1].str());
        const auto signature = cpp.find("GrhSIM_top::" + name + "(");
        const auto begin = signature == std::string::npos ? signature : cpp.find("\n{\n", signature);
        const auto end = begin == std::string::npos ? begin : cpp.find("\n}\n", begin + 3);
        require(begin != std::string::npos && end != std::string::npos,
                "General dispatch lacks its complete actual body");
        bodies.emplace(name, cpp.substr(begin + 3, end - begin - 3));
    }
    std::vector<WriterDispatchCall> calls;
    std::set<std::string> active;
    const auto visit = [&](auto&& self, const std::string& name, std::vector<std::string> guards) -> void {
        require(active.insert(name).second && bodies.contains(name), "Invalid recursive General driver helper");
        const auto initialGuards = guards.size();
        std::vector<bool> scopes;
        std::istringstream lines(bodies.at(name));
        std::string line;
        while (std::getline(lines, line)) {
            const auto begin = line.find_first_not_of(" \t");
            if (begin == std::string::npos) continue;
            line.erase(0, begin);
            if (line.starts_with("//") || line.starts_with("/*")) continue;
            if (line == "}") {
                require(!scopes.empty(), "Unbalanced General dispatch scope");
                if (scopes.back()) guards.pop_back();
                scopes.pop_back();
                continue;
            }
            const std::regex phase(R"REGEX(&GrhSIM_top::([A-Za-z_][A-Za-z0-9_]*))REGEX");
            for (std::sregex_iterator it(line.begin(), line.end(), phase), end; it != end; ++it)
                calls.push_back({(*it)[1].str(), guards});
            std::smatch helper;
            if (std::regex_match(line, helper, std::regex(R"REGEX((pGeneral_[A-Za-z0-9_]+)\(\);)REGEX")))
                self(self, helper[1].str(), guards);
            if (line.ends_with("{")) {
                const bool conditional = line.starts_with("if (");
                scopes.push_back(conditional);
                if (conditional) guards.push_back(line);
            }
        }
        require(scopes.empty() && guards.size() == initialGuards, "Unclosed General dispatch scope");
        active.erase(name);
    };
    visit(visit, "pGeneral", {});
    return calls;
}

void testWriterDispatchFreshFireGroups() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clockA = fixture.input("dispatch_clock_a"), clockB = fixture.input("dispatch_clock_b");
    const auto clockC = fixture.input("dispatch_clock_c");
    const auto raw = fixture.input("dispatch_raw"), gate = fixture.input("dispatch_gate");
    const auto enable = fixture.compute(OperationKind::kAnd, Fixture::type(1), {raw, gate});
    const auto data = fixture.input("dispatch_data", 8);
    const auto target = fixture.state("dispatch_q", 8), fallback = fixture.state("dispatch_fallback", 8);
    graph.setOpKind(fallback, OperationKind::kLatch);
    grh::setStateInitSpec(graph, target, {{grh::InitKind::Constant, "8'd17"}});
    std::vector<grh::OperationId> writers;
    for (unsigned i = 0; i < 97; ++i) {
        const auto offset = fixture.constant(8, "8'd" + std::to_string(i));
        const auto payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, offset});
        if (i == 64)
            writers.push_back(fixture.write(OperationKind::kLatchWritePort, fallback,
                {enable, payload, fixture.constant(8, "8'hff")}));
        else
            writers.push_back(fixture.write(OperationKind::kRegisterWritePort, target,
                {enable, payload, clockA, clockB}, {"posedge", "posedge"}));
    }
    graph.bindOutputPort("dispatch_q", fixture.read(target));
    graph.bindOutputPort("dispatch_fallback", fixture.read(fallback));
    const auto imported = graph.createOperation(OperationKind::kDpicImport,
        graph.internSymbol("dispatch_hold_join_false"));
    graph.setAttr(imported, "argsName", std::vector<std::string>{"held"});
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false});
    graph.setAttr(imported, "hasReturn", false);
    const auto one = fixture.constant(1, "1'b1");
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("dispatch_hold_join_false"));
    graph.addOperand(call, one); graph.addOperand(call, one);
    const auto edgeCarrier = fixture.state("dispatch_edge_carrier", 1);
    const auto carrierWriter = fixture.write(OperationKind::kRegisterWritePort, edgeCarrier, {one, one, clockC});
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto joined = graph.opOperands(writers.front())[0];
    const auto edgeArgs = graph.opOperands(graph.valueDef(joined));
    require(graph.opKind(graph.valueDef(joined)) == OperationKind::kOr && edgeArgs.size() == 2,
            "Dispatch fixture lost its two fresh edge leaves");
    const std::array<grh::ValueId, 2> edges{edgeArgs[0], edgeArgs[1]};
    const auto edgeC = graph.opOperands(carrierWriter)[0];
    // The carrier only supplies a genuine EventEdge, and remains the last
    // singleton batch so a nonadjacent caller group cannot consume it.
    const auto held = fixture.compute(OperationKind::kAssign, Fixture::type(1), {joined});
    graph.replaceOperand(call, 2, held);
    const auto zero = fixture.constant(1, "1'b0");
    const auto fire = [&](grh::ValueId base, unsigned variant) {
        auto result = base;
        for (unsigned bit = 0; bit < 7; ++bit)
            result = variant & (1u << bit) ? fixture.compute(OperationKind::kOr, Fixture::type(1), {zero, result}) :
                fixture.compute(OperationKind::kOr, Fixture::type(1), {result, zero});
        return fixture.compute(OperationKind::kAssign, Fixture::type(1), {result});
    };
    for (unsigned i = 0; i < writers.size(); ++i) {
        if (i == 64) continue;
        const auto source = i >= 48 && i < 64 ? edgeC : held;
        const auto variant = i < 64 ? i : i - 65;
        graph.replaceOperand(writers[i], 0, fire(source, variant));
    }
    fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == writers.size() + 1,
            "Dispatch fixture lost its exact ordered writer runs");
    for (unsigned i = 0; i < writers.size(); ++i)
        require(schedule.generalRuns[i].orderedWriters == std::vector<grh::OperationId>{writers[i]},
                "Dispatch fixture changed the A/other/A writer sequence");
    require(owner(graph, graph.valueDef(enable)).size() == 4 &&
                owner(graph, graph.valueDef(held)).size() == 4,
            "Dispatch fixture lost its nonsink enable or held fire boundary");
    std::vector<std::pair<grh::OperationId, std::string>> sites;
    for (unsigned i = 0; i < writers.size(); ++i)
        if (i != 64) sites.emplace_back(writers[i], "writer-sample");
    const std::string support = batchActivityCounterSupport(sites.size()) + R"CPP(
static GrhSIM_top* dispatchInstance;
extern "C" void dispatch_hold_join_false(std::uint8_t) {
    if (dispatchInstance) dispatchInstance->test_hold_dispatch_false();
}
)CPP";
    const std::string body = R"CPP(
model.init(); dispatchInstance=&model;
unsigned previousClocks=0, expected=17, fallback=0;
const auto step = [&](unsigned clocks,unsigned data,bool enabled) {
    const auto before=batchCounts(model);
    const bool freshAB=(clocks & ~previousClocks & 3)!=0;
    const bool freshC=(clocks & ~previousClocks & 4)!=0;
    model.dispatch_clock_a=clocks&1; model.dispatch_clock_b=(clocks>>1)&1;
    model.dispatch_clock_c=(clocks>>2)&1; model.dispatch_data=data;
    model.dispatch_raw=true; model.dispatch_gate=enabled; model.eval();
    const auto after=batchCounts(model);
    unsigned site=0;
    for(unsigned i=0;i<97;++i) if(i!=64) {
        const bool fires=i>=48 && i<64 ? freshC : freshAB;
        assert(after[site]==before[site]+unsigned(enabled && fires)); ++site;
    }
    if(enabled && freshAB) expected=(data+47)&255;
    if(enabled && freshC) expected=(data+63)&255;
    if(enabled) fallback=(data+64)&255;
    if(enabled && freshAB) expected=(data+96)&255;
    assert(model.dispatch_q==expected && model.dispatch_fallback==fallback);
    model.eval(); assert(batchCounts(model)==after && model.dispatch_q==expected && model.dispatch_fallback==fallback);
    previousClocks=clocks;
};
step(0,1,false); step(1,2,true); step(2,3,true); // B rises while the held join stays logically true.
step(2,4,true); step(0,5,true); step(4,6,true); // C alone bypasses both A/B groups.
step(0,7,true); step(3,8,false); step(3,9,true); // Enable publication cannot manufacture an edge.
step(0,10,true); step(7,11,true); // Final A/B group wins over the intervening C writes.
step(0,12,false); step(1,13,true); // Nonsink changes enable before the live writer guards.
dispatchInstance=nullptr;
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "writer-dispatch-fresh-fire-groups-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "writer_batches") == 7 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_groups") == 2 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_batches") == 5 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_max_events") == 2,
                "Dispatch fixture did not retain its 16-fire splits and two separate canonical groups");
        std::vector<std::string> roots;
        for (unsigned i = 0; i < writers.size(); ++i) {
            const auto root = batchActivityRoot(directory, writers[i]);
            if (roots.empty() || roots.back() != root) roots.push_back(root);
            const auto expected = i < 48 ? i / 16 : i <= 64 ? 3 : 4 + (i - 65) / 16;
            require(roots.size() == expected + 1, "Dispatch fixture changed its original batch boundaries");
        }
        roots.push_back(batchActivityRoot(directory, carrierWriter));
        require(roots.size() == 7 && roots[5] != roots[6], "Dispatch carrier lost its singleton fallback");
        const auto calls = writerDispatchCalls(directory);
        std::vector<std::string> actual;
        bool sawWriter = false;
        const auto firstEdge = "eventActStore." + mappedValue(directory, edges[0]);
        const auto secondEdge = "eventActStore." + mappedValue(directory, edges[1]);
        for (const auto& entry : calls) {
            if (entry.name.starts_with("pGeneral_active_batch")) {
                require(!sawWriter && entry.guards.empty(), "Nonsink dispatch moved behind a writer or fire guard");
                continue;
            }
            require(entry.name.starts_with("writer_run_batch"), "Unknown General dispatch phase");
            sawWriter = true; actual.push_back(entry.name);
            const bool grouped = actual.size() <= 3 || actual.size() == 5 || actual.size() == 6;
            if (grouped)
                require(entry.guards.size() == 1 && entry.guards.front().find(firstEdge) != std::string::npos &&
                            entry.guards.front().find(secondEdge) != std::string::npos &&
                            entry.guards.front().find("nonSinkBoundaryStore.") == std::string::npos &&
                            entry.guards.front().find("sinkDirty[") == std::string::npos,
                        "Caller guard used a held fire, enable or stale activity snapshot");
            else require(entry.guards.empty(), "Fallback/singleton batch entered an outer fire group");
        }
        require(actual == roots, "Caller fire groups changed the complete batch order");
        auto header = readFile(directory / "grhsim_top.hpp");
        const auto insertion = header.find("private:\n");
        require(insertion != std::string::npos, "Dispatch fixture lacks a private model boundary");
        header.insert(insertion, "    void test_hold_dispatch_false() { nonSinkBoundaryStore." +
            mappedValue(directory, held) + " = false; }\n");
        std::ofstream(directory / "grhsim_top.hpp") << header;
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testWriterDispatchNontrivialLocalFallback() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clockA = fixture.input("dispatch_string_clock_a"), clockB = fixture.input("dispatch_string_clock_b");
    const auto enable = fixture.input("dispatch_string_enable");
    const auto selector = fixture.input("dispatch_string_select");
    const auto data = fixture.input("dispatch_string_data", 8);
    const auto target = fixture.state("dispatch_string_q", 8);
    grh::setStateInitSpec(graph, target, {{grh::InitKind::Constant, "8'd17"}});
    const grh::LogicalType stringType{grh::ValueType::String, 1, false, grh::LogicDomain::TwoState, {}};
    std::vector<grh::OperationId> writers;
    grh::ValueId stringMux;
    for (unsigned i = 0; i < 80; ++i) {
        grh::ValueId payload;
        if (i == 40) {
            const auto left = fixture.compute(OperationKind::kConstant, stringType, {});
            const auto right = fixture.compute(OperationKind::kConstant, stringType, {});
            graph.setAttr(graph.valueDef(left), "constValue", std::string(64, 'a'));
            graph.setAttr(graph.valueDef(right), "constValue", std::string(64, 'b'));
            stringMux = fixture.compute(OperationKind::kMux, stringType, {selector, left, right});
            const auto equal = fixture.compute(OperationKind::kEq, Fixture::type(1), {stringMux, left});
            payload = fixture.compute(OperationKind::kMux, Fixture::type(8),
                {equal, fixture.constant(8, "8'd1"), fixture.constant(8, "8'd0")});
        } else {
            const auto offset = fixture.constant(8, "8'd" + std::to_string(i));
            payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, offset});
        }
        writers.push_back(fixture.write(OperationKind::kRegisterWritePort, target,
            {enable, payload, clockA, clockB}, {"posedge", "posedge"}));
    }
    graph.bindOutputPort("dispatch_string_q", fixture.read(target));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto joined = graph.opOperands(writers.front())[0];
    const auto zero = fixture.constant(1, "1'b0");
    for (unsigned i = 0; i < writers.size(); ++i) {
        auto fire = joined;
        for (unsigned bit = 0; bit < 7; ++bit)
            fire = i & (1u << bit) ? fixture.compute(OperationKind::kOr, Fixture::type(1), {zero, fire}) :
                fixture.compute(OperationKind::kOr, Fixture::type(1), {fire, zero});
        graph.replaceOperand(writers[i], 0, fixture.compute(OperationKind::kAssign, Fixture::type(1), {fire}));
    }
    fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == writers.size() &&
                owner(graph, graph.valueDef(stringMux)) == schedule.generalRuns[40].partitionKey,
            "Nontrivial dispatch fixture lost its real private String local");
    std::vector<std::pair<grh::OperationId, std::string>> sites;
    for (const auto writer : writers) sites.emplace_back(writer, "writer-sample");
    sites.emplace_back(graph.valueDef(stringMux), "operation");
    const std::string body = R"CPP(
model.init(); unsigned previousClocks=0, expected=17;
const auto step = [&](unsigned clocks,unsigned data,bool select,bool enabled) {
    const auto before=batchCounts(model);
    const bool fires=(clocks & ~previousClocks & 3)!=0;
    model.dispatch_string_clock_a=clocks&1; model.dispatch_string_clock_b=(clocks>>1)&1;
    model.dispatch_string_enable=enabled; model.dispatch_string_select=select;
    model.dispatch_string_data=data; model.eval(); const auto after=batchCounts(model);
    for(unsigned i=0;i<after.size();++i)
        assert(after[i]==before[i]+unsigned(fires && enabled));
    if(fires && enabled) expected=(data+79)&255;
    assert(model.dispatch_string_q==expected);
    model.eval(); assert(batchCounts(model)==after && model.dispatch_string_q==expected);
    previousClocks=clocks;
};
step(0,1,false,false); step(1,2,false,true); step(2,3,true,true);
step(2,4,false,true); step(0,5,true,true); step(3,6,false,false);
step(3,7,true,true); step(0,8,false,true); step(3,9,true,true);
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "writer-dispatch-nontrivial-local-b" + std::to_string(budget);
        manualCase(fixture, label, budget, batchActivityCounterSupport(sites.size()), body, false,
            {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "writer_batches") == 5 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_groups") == 2 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_batches") == 4 &&
                    batchActivityStatistic(directory, "writer_dispatch_fire_max_events") == 2,
                "A nontrivial entry local did not split otherwise identical caller fire groups");
        std::vector<std::string> roots;
        for (unsigned i = 0; i < writers.size(); ++i) {
            const auto root = batchActivityRoot(directory, writers[i]);
            if (roots.empty() || roots.back() != root) roots.push_back(root);
            require(roots.size() == i / 16 + 1, "Nontrivial dispatch changed the original 16-fire batch boundaries");
        }
        bool stringLocal = false;
        for (const auto& function : mappedRecords(readFile(directory / "code-map.json"), "functions"))
            if (function.find("\"root_function\":\"" + roots[2] + "\"") != std::string::npos)
                for (const auto& local : mappedRecords(function, "locals"))
                    stringLocal |= local.find("\"type\":\"std::string\"") != std::string::npos;
        require(stringLocal, "Nontrivial dispatch fallback lacks an actual emitted std::string local");
        std::vector<std::string> actual;
        for (const auto& entry : writerDispatchCalls(directory)) {
            if (entry.name.starts_with("pGeneral_active_batch")) {
                require(actual.empty() && entry.guards.empty(), "Nontrivial fallback moved nonsink dispatch");
                continue;
            }
            actual.push_back(entry.name);
            require(actual.size() == 3 ? entry.guards.empty() : entry.guards.size() == 1,
                    "Caller guard bypassed a nontrivial local or regrouped across its fallback");
        }
        require(actual == roots, "Nontrivial fallback changed original batch order");
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testScalarShadowChangeMasks() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("scm_clock_a"), clockB = fixture.input("scm_clock_b");
    const auto enableA = fixture.input("scm_enable_a"), enableB = fixture.input("scm_enable_b");
    const auto enableC = fixture.input("scm_enable_c"), restore = fixture.input("scm_restore");
    const auto dataA = fixture.input("scm_data_a", 64), dataB = fixture.input("scm_data_b", 64);
    const auto mask = fixture.input("scm_mask", 64);
    const auto signedInput = [&](std::string_view name, int32_t width) {
        auto type = Fixture::type(width);
        type.isSigned = true;
        const auto value = graph.createValue(graph.internSymbol(name), type);
        graph.bindInputPort(name, value);
        return value;
    };
    const auto signedA = signedInput("scm_signed_a", 7), signedB = signedInput("scm_signed_b", 3);
    const auto convertBits = [&](grh::ValueId source, const grh::LogicalType& target) {
        const auto input = graph.valueLogicalType(source);
        if (input.width == target.width) return source;
        const auto slice = [&](int32_t width, int64_t offset, const grh::LogicalType& type) {
            const auto result = fixture.compute(OperationKind::kSliceStatic, type, {source});
            graph.setAttr(graph.valueDef(result), "sliceStart", offset);
            graph.setAttr(graph.valueDef(result), "sliceEnd", offset + width - 1);
            return result;
        };
        if (input.width > target.width) return slice(target.width, 0, target);
        const auto padding = target.width - input.width;
        grh::ValueId high;
        if (input.isSigned) {
            const auto sign = slice(1, input.width - 1, Fixture::type(1));
            high = fixture.compute(OperationKind::kReplicate, Fixture::type(padding), {sign});
            graph.setAttr(graph.valueDef(high), "rep", int64_t{padding});
        } else high = fixture.constant(padding, std::to_string(padding) + "'d0");
        return fixture.compute(OperationKind::kConcat, target, {high, source});
    };
    const std::array<int32_t, 8> widths{1, 3, 7, 8, 17, 32, 63, 64};
    std::vector<std::pair<grh::OperationId, std::string>> sites;
    std::string checks;
    std::size_t index = 0;
    for (const bool isSigned : {false, true}) for (const auto width : widths) {
        const auto suffix = std::string(isSigned ? "s" : "u") + std::to_string(width);
        const auto state = fixture.state("scm_state_" + suffix, width);
        auto type = Fixture::type(width);
        type.isSigned = isSigned;
        grh::setStateLogicalType(graph, state, type);
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, std::to_string(width) + "'d1"}});
        // Slice truncates; sign-bit replication plus concat extends. Assign
        // is an equal-width view and is deliberately not used for conversion.
        const auto payloadA = convertBits(isSigned ? signedA : dataA, type);
        const auto payloadB = convertBits(isSigned ? signedB : dataB, type);
        const auto current = fixture.read(state);
        require(graph.valueLogicalType(current) == type,
                "Scalar shadow change fixture lost the signed current-read type");
        const auto payloadC = fixture.compute(OperationKind::kMux, type, {restore, current, payloadB});
        const auto writeMask = convertBits(mask, Fixture::type(width));
        const auto writerA = fixture.write(OperationKind::kRegisterWritePort, state, {enableA, payloadA, clockA});
        const auto writerB = fixture.write(OperationKind::kRegisterWritePort, state, {enableB, payloadB, clockB});
        const auto writerC = fixture.write(OperationKind::kRegisterWriteMaskPort, state, {enableC, payloadC, writeMask, clockA});
        for (const auto writer : {writerA, writerB, writerC}) sites.emplace_back(writer, "writer-sample");
        graph.bindOutputPort("scm_q_" + suffix, fixture.read(state));
        checks += "assert(scmBits(static_cast<std::uint64_t>(instance.scm_q_" + suffix + ")," +
            std::to_string(width) + ")==expected[" + std::to_string(index++) + "]);\n";
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    fixture.split();
    fixture.partition();
    const std::string support = batchActivityCounterSupport(sites.size()) + R"CPP(
// Expected state comes from logical bits and ordered writes. This oracle
// never constructs a change mask or reads emitter dirty bits/run masks.
static std::uint64_t scmBits(std::uint64_t value,unsigned width) {
    if(width==64) return value;
    return value%(UINT64_C(1)<<width);
}
static std::uint64_t scmExtend(std::uint64_t raw,unsigned sourceWidth,unsigned targetWidth,bool isSigned) {
    const auto source=scmBits(raw,sourceWidth);
    const bool sign=isSigned && ((source>>(sourceWidth-1))%2!=0);
    std::uint64_t result=0;
    for(unsigned bit=0;bit<targetWidth;++bit) {
        const bool value=bit<sourceWidth ? (source>>bit)%2!=0 : sign;
        if(value) result+=UINT64_C(1)<<bit;
    }
    return result;
}
static std::uint64_t scmReplace(std::uint64_t pending,std::uint64_t payload,std::uint64_t mask,unsigned width) {
    std::uint64_t result=0;
    for(unsigned bit=0;bit<width;++bit) {
        const auto selected=(mask>>bit)%2 ? payload : pending;
        if((selected>>bit)%2) result+=UINT64_C(1)<<bit;
    }
    return result;
}
)CPP";
    const std::string body = R"CPP(
const std::array<unsigned,8> widths{1,3,7,8,17,32,63,64};
for(unsigned epoch=0;epoch<2;++epoch) {
    model.scm_clock_a=model.scm_clock_b=0;
    model.init(); std::array<std::uint64_t,16> expected; expected.fill(1);
    unsigned previous=0;
    const auto evaluate=[&](GrhSIM_top& instance,unsigned clocks,bool enableA,bool enableB,bool enableC,
                            bool restore,std::uint64_t dataA,std::uint64_t dataB,
                            std::uint64_t signedA,std::uint64_t signedB,std::uint64_t mask,
                            std::array<int,3> work=std::array<int,3>{-1,-1,-1}) {
        const auto before=batchCounts(instance);
        const bool risingA=(clocks&1) && !(previous&1),risingB=(clocks&2) && !(previous&2);
        const auto current=expected;
        for(unsigned i=0;i<expected.size();++i) {
            const auto width=widths[i%widths.size()]; const bool isSigned=i>=widths.size();
            const auto a=scmExtend(isSigned?signedA:dataA,isSigned?7:64,width,isSigned);
            const auto b=scmExtend(isSigned?signedB:dataB,isSigned?3:64,width,isSigned);
            if(risingA && enableA) expected[i]=a;
            if(risingB && enableB) expected[i]=b;
            if(risingA && enableC) expected[i]=scmReplace(expected[i],restore?current[i]:b,mask,width);
        }
        instance.scm_clock_a=clocks&1; instance.scm_clock_b=(clocks>>1)&1;
        instance.scm_enable_a=enableA; instance.scm_enable_b=enableB; instance.scm_enable_c=enableC;
        instance.scm_restore=restore; instance.scm_data_a=dataA; instance.scm_data_b=dataB;
        instance.scm_signed_a=static_cast<decltype(instance.scm_signed_a)>(signedA);
        instance.scm_signed_b=static_cast<decltype(instance.scm_signed_b)>(signedB);
        instance.scm_mask=mask; instance.eval(); previous=clocks;
        const auto after=batchCounts(instance);
        if(!risingA && !risingB) assert(after==before);
        // These three real manifest sites belong to unsigned8's A/B/A writers.
        // Other widths do not assume an identical pattern of dirty activity.
        for(unsigned writer=0;writer<3;++writer) if(work[writer]>=0)
            assert(after[9+writer]==before[9+writer]+work[writer]);
)CPP" + checks + R"CPP(
        instance.eval(); assert(batchCounts(instance)==after);
)CPP" + checks + R"CPP(
    };
    const auto pulse=[&](bool restore,std::uint64_t a,std::uint64_t b,std::uint64_t sa,std::uint64_t sb,
                         std::uint64_t mask,std::array<int,3> work=std::array<int,3>{-1,-1,-1}) {
        evaluate(model,0,true,true,true,restore,a,b,sa,sb,mask);
        evaluate(model,3,true,true,true,restore,a,b,sa,sb,mask,work);
    };
    // C equals pending B: its zero change must preserve B's publication and
    // the earlier target invalidation, including the next unchanged A edge.
    pulse(false,0,2,0,2,UINT64_MAX,{1,1,1});
    pulse(false,0,2,0,2,UINT64_MAX,{1,1,1});
    pulse(false,0,2,0,2,UINT64_MAX,{1,1,1});
    // A also equals pending/current B, so it must leave clean peers clean.
    pulse(false,2,2,2,2,UINT64_MAX,{1,0,0});
    // B changes next; C restores old current. Hold is an ordered write.
    pulse(true,2,3,2,3,UINT64_MAX,{0,1,1});
    evaluate(model,0,true,true,true,true,2,3,2,3,UINT64_MAX);
    evaluate(model,1,true,true,true,true,2,3,2,3,UINT64_MAX,{1,0,0});
    // A disabled writer and a zero data mask cannot erase B's dirty mark.
    evaluate(model,0,false,true,true,true,2,4,2,4,0);
    evaluate(model,3,false,true,true,true,2,4,2,4,0,{0,1,1});
    const std::array<std::uint64_t,8> patterns{0,1,UINT64_MAX,UINT64_C(0x8000000000000000),
        UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210),
        UINT64_C(0xaaaaaaaaaaaaaaaa),UINT64_C(0x5555555555555555)};
    for(unsigned step=0;step<192;++step) {
        const auto a=patterns[step%patterns.size()],b=patterns[(step/3+3)%patterns.size()];
        const auto mask=patterns[(step/5+1)%patterns.size()];
        // Negative 7/3-bit inputs extend into17/32/63/64; all clock values,
        // disabled writers and mixed/zero/full masks retain ordered semantics.
        evaluate(model,step%4,step%7!=0,step%5!=0,step%3!=0,step%6>=3,
                 a,b,step%128,(step+4)%8,mask);
    }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "scalar-shadow-change-mask-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "scalar_shadow_change_mask_writer_sites") == sites.size() &&
                    batchActivityStatistic(directory, "eligible_writers") == sites.size() &&
                    batchActivityStatistic(directory, "rejected_writers") == 0,
                "Scalar shadow change fixture did not exercise every native scalar mask route");
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testSinkActivityBatchCompetition() {
    Fixture fixture; auto& graph=fixture.graph;
    const auto clock=fixture.input("batch_clock");
    const auto state=fixture.state("batch_q",8);
    grh::setStateInitSpec(graph,state,{{grh::InitKind::Constant,"8'd17"}});
    std::vector<grh::ValueId> payloads;
    for(unsigned i=0;i<130;++i) {
        const auto data=fixture.input("batch_data_"+std::to_string(i),8);
        const auto payload=fixture.compute(OperationKind::kAdd,Fixture::type(8),{data,data});
        payloads.push_back(payload);
        fixture.write(OperationKind::kRegisterWritePort,state,{fixture.constant(1,"1'b1"),payload,clock});
    }
    graph.bindOutputPort("batch_q",fixture.read(state));
    // Keep each true enable private so init is the only initial dirty source.
    run(fixture.design,"grh-to-grhsim",fixture.session); fixture.split(); fixture.partition();
    const auto& schedule=sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==130,"Batch competition lost its exact priority runs");
    std::vector<grh::OperationId> writers;
    std::vector<std::pair<grh::OperationId,std::string>> sites;
    for(unsigned i=0;i<130;++i) {
        const auto& run=schedule.generalRuns[i];
        require(run.orderedWriters.size()==1 && sim::writerTarget(graph,run.orderedWriters.front())==state,
                "Batch competition lost ordered writes to one target");
        const auto writer=run.orderedWriters.front(); writers.push_back(writer);
        const auto payload=graph.valueDef(graph.opOperands(writer)[2]);
        require(graph.opKind(payload)==OperationKind::kAdd && owner(graph,payload)==run.partitionKey,
                "Batch competition lost its private counted payload");
        sites.emplace_back(writer,"writer-sample"); sites.emplace_back(payload,"operation");
    }
    const std::string body=R"CPP(
model.init();
const auto step = [&](bool clock,unsigned work,unsigned value) {
    const auto before=batchCounts(model); const auto rounds=model.perf_counters().totalRoundCount;
    model.batch_clock=clock; model.eval(); const auto after=batchCounts(model);
    for(unsigned i=0;i<130;++i) {
        const unsigned expected=work==2 ? i==0 : work;
        assert(after[2*i]==before[2*i]+expected && after[2*i+1]==before[2*i+1]+expected);
    }
    assert(model.batch_q==value); const auto elapsed=model.perf_counters().totalRoundCount-rounds;
    model.eval(); assert(batchCounts(model)==after); return elapsed;
};
step(false,0,17); step(true,1,0);
step(false,0,0); step(true,0,0);
model.batch_data_0=1; step(false,0,0);
// Only run0 is dirty at General entry. It writes 2; run1 restores 0.
// The live later batch must observe target invalidation and run128/run129.
for(unsigned repeat=0;repeat<3;++repeat) {
    assert(step(true,1,0)==1);
    step(false,0,0);
}
model.batch_data_0=0; step(false,0,0); step(true,2,0);
step(false,0,0); step(true,0,0);
)CPP";
    for(const auto budget : {16,1024}) {
        const auto label="sink-activity-batch-competition-b"+std::to_string(budget);
        manualCase(fixture,label,budget,batchActivityCounterSupport(sites.size()),body,false,{{"execution-counters","on"}});
        const auto directory=artifact(label)/"model";
        require(batchActivityStatistic(directory,"eligible_runs")==130 && batchActivityStatistic(directory,"rejected_runs")==0 &&
                    batchActivityStatistic(directory,"scalar_shadow_change_mask_writer_sites")==130 &&
                    batchActivityStatistic(directory,"bitmap_words")==3 && batchActivityStatistic(directory,"max_target_fanout")==130 &&
                    batchActivityStatistic(directory,"publication_change_sources")==0 &&
                    batchActivityStatistic(directory,"writer_batches")==2 &&
                    batchActivityStatistic(directory,"writer_batches_all_eligible")==2 &&
                    batchActivityStatistic(directory,"writer_batches_with_activity_guard")==2 &&
                    batchActivityStatistic(directory,"writer_batch_activity_mask_words")==3 &&
                    batchActivityStatistic(directory,"writer_batch_activity_max_mask_words")==2,
                "Batch competition silently lost eligibility or its three activity words");
        const auto first=batchActivityRoot(directory,writers[0]),last=batchActivityRoot(directory,writers[128]);
        require(first.starts_with("writer_run_batch") && last.starts_with("writer_run_batch") && first!=last &&
                    batchActivityRoot(directory,writers[127])==first && batchActivityRoot(directory,writers[129])==last,
                "Batch competition lost the original 128-run split");
        const auto a=batchActivityRootCpp(directory,first),b=batchActivityRootCpp(directory,last);
        require(a.find("if ((sinkDirty[0] & UINT64_C(18446744073709551615)) | (sinkDirty[1] & UINT64_C(18446744073709551615)))")!=std::string::npos &&
                    b.find("if ((sinkDirty[2] & UINT64_C(3)))")!=std::string::npos,
                "Batch competition lost its exact cross-word coarse masks");
        bindBatchActivityCounters(label,graph,sites);
    }
}

void testNonsinkClockIsolation() {
    for (const unsigned nodes : {1u, 128u}) for (const bool clone : {false, true}) {
        Fixture fixture; auto& graph = fixture.graph;
        const auto clockA = fixture.input("cold_clock_a"), clockB = fixture.input("cold_clock_b");
        const auto enable = fixture.input("cold_enable"), data = fixture.input("cold_data", 8);
        const auto payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, data});
        const auto inverted = fixture.compute(OperationKind::kLogicNot, Fixture::type(1), {clockA});
        const auto call = [&](const std::string& name, grh::ValueId clock) {
            const auto imported = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
            graph.setAttr(imported, "argsName", std::vector<std::string>{"data", "inverted"});
            graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input", "input"});
            graph.setAttr(imported, "argsWidth", std::vector<int64_t>{8, 1});
            graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false});
            graph.setAttr(imported, "hasReturn", false);
            const auto op = graph.createOperation(OperationKind::kDpicCall);
            graph.setAttr(op, "targetImportSymbol", name);
            graph.setAttr(op, "eventEdge", std::vector<std::string>{"posedge"});
            for (const auto value : {enable, payload, inverted, clock}) graph.addOperand(op, value);
            return op;
        };
        const auto first = call("cold_capture_a", clockA), second = call("cold_capture_b", clockB);
        run(fixture.design, "grh-to-grhsim", fixture.session);
        const auto fireA = graph.opOperands(first)[0], fireB = graph.opOperands(second)[0];
        const auto joined = fixture.compute(OperationKind::kOr, Fixture::type(1), {fireA, fireB});
        const auto alias = fixture.compute(OperationKind::kAssign, Fixture::type(1), {joined});
        graph.replaceOperand(first, 0, alias); graph.replaceOperand(second, 0, alias);
        graph.setAttr(first, "grhsim.effectOrder", int64_t{0});
        graph.setAttr(second, "grhsim.effectOrder", int64_t{1});
        fixture.split();
        run(fixture.design, "grhsim-partition-nonsink", fixture.session,
            {"-max-supernode-nodes=" + std::to_string(nodes), "-clone-shared=" + std::to_string(clone)});
        run(fixture.design, "grhsim-partition-sink", fixture.session);
        const auto dataOwner = owner(graph, graph.valueDef(payload)), firstOwner = owner(graph, first),
            secondOwner = owner(graph, second), invertedOwner = owner(graph, graph.valueDef(inverted));
        require(dataOwner.size() == 4 && firstOwner.size() == 4 && secondOwner.size() == 4 && invertedOwner.size() == 4 &&
                    dataOwner[2] != firstOwner[2] && dataOwner[2] != secondOwner[2] && dataOwner[2] != invertedOwner[2],
                "Clock activity still shares an owner with unrelated data");
        unsigned adds = 0;
        for (const auto op : graph.operations()) adds += graph.opKind(op) == OperationKind::kAdd;
        require(adds == 1, "Cold data was cloned into a clock owner");
        if (nodes == 1 && !clone) require(owner(graph, first)[2] != owner(graph, second)[2] &&
                    owner(graph, first)[2] != owner(graph, graph.valueDef(alias))[2],
                "Pulse regression needs separate join and effect owners");
        const std::string support = batchActivityCounterSupport(1) + R"CPP(
static unsigned coldCallsA, coldCallsB, coldLastData, coldLastInvert;
static bool coldThrow;
static std::vector<unsigned> coldOrder;
extern "C" void cold_capture_a(std::uint8_t data,std::uint8_t inverted) {
    ++coldCallsA; coldLastData=data; coldLastInvert=inverted; coldOrder.push_back(1);
    if (coldThrow) { coldThrow=false; throw std::runtime_error("cold test interruption"); }
}
extern "C" void cold_capture_b(std::uint8_t data,std::uint8_t inverted) {
    ++coldCallsB; coldLastData=data; coldLastInvert=inverted; coldOrder.push_back(2);
}
)CPP";
        const std::string body = R"CPP(
model.init(); model.cold_enable=true; model.cold_data=5; model.eval();
auto count=batchCounts(model)[0]; assert(count==1 && coldCallsA==0 && coldCallsB==0);
const auto evaluate = [&](unsigned clocks,unsigned dataWork,bool fires,unsigned value) {
    const auto before=batchCounts(model)[0]; const auto a=coldCallsA, b=coldCallsB;
    coldOrder.clear(); model.cold_clock_a=clocks&1; model.cold_clock_b=(clocks>>1)&1; model.eval();
    assert(batchCounts(model)[0]==before+dataWork);
    assert(coldCallsA==a+unsigned(fires) && coldCallsB==b+unsigned(fires));
    if (fires) {
        assert((coldOrder==std::vector<unsigned>{1,2}));
        assert(coldLastData==value && coldLastInvert==unsigned(!(clocks&1)));
    } else assert(coldOrder.empty());
    model.eval(); assert(batchCounts(model)[0]==before+dataWork && coldCallsA==a+unsigned(fires) && coldCallsB==b+unsigned(fires));
};
evaluate(1,0,true,10); evaluate(0,0,false,0);
evaluate(2,0,true,10); evaluate(0,0,false,0);
evaluate(3,0,true,10); evaluate(0,0,false,0);
model.cold_data=9; evaluate(0,1,false,0); evaluate(3,0,true,18);
model.cold_enable=false; evaluate(0,0,false,0); evaluate(1,0,false,0);
model.cold_enable=true; evaluate(1,0,false,0); evaluate(0,0,false,0);
const auto a=coldCallsA, b=coldCallsB; const auto stable=batchCounts(model)[0];
coldThrow=true; model.cold_clock_b=true;
bool interrupted=false; try { model.eval(); } catch (const std::runtime_error&) { interrupted=true; }
assert(interrupted && coldCallsA==a+1 && coldCallsB==b && batchCounts(model)[0]==stable);
model.eval(); assert(coldCallsA==a+1 && coldCallsB==b && batchCounts(model)[0]==stable);
evaluate(0,0,false,0); evaluate(3,0,true,18);
model.init(); model.cold_enable=true; model.cold_data=9; model.eval();
assert(batchCounts(model)[0]==1); evaluate(1,0,true,18);
)CPP";
        for (const unsigned budget : {16u, 1024u}) {
            const auto label = "nonsink-clock-isolation-n" + std::to_string(nodes) +
                "-c" + std::to_string(clone) + "-b" + std::to_string(budget);
            manualCase(fixture, label, budget, support, body, false, {{"execution-counters", "on"}});
            bindBatchActivityCounters(label, graph, {{graph.valueDef(payload), "operation"}});
        }
    }
}

void testSinkActivityJoinedFire() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clockA = fixture.input("joined_clock_a"), clockB = fixture.input("joined_clock_b");
    const auto enable = fixture.input("joined_enable"), data = fixture.input("joined_data", 8);
    const auto one = fixture.constant(1, "1'b1");
    const auto payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, data});
    const auto state = fixture.state("joined_q", 8), peer = fixture.state("joined_peer", 8);
    grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, "8'd17"}});
    const auto writer = fixture.write(OperationKind::kRegisterWritePort, state,
        {enable, payload, clockA, clockB}, {"posedge", "posedge"});
    const auto peerWriter = fixture.write(OperationKind::kRegisterWritePort, peer,
        {enable, data, clockA, clockB}, {"posedge", "posedge"});
    const auto imported = graph.createOperation(OperationKind::kDpicImport,
        graph.internSymbol("activity_hold_join_false"));
    graph.setAttr(imported, "argsName", std::vector<std::string>{"fire"});
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{1});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false});
    graph.setAttr(imported, "hasReturn", false);
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("activity_hold_join_false"));
    graph.addOperand(call, one); graph.addOperand(call, enable);
    graph.bindOutputPort("joined_q", fixture.read(state));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto joined = graph.opOperands(writer)[0];
    require(graph.opKind(graph.valueDef(joined)) == OperationKind::kOr,
            "Joined activity fixture lost its two EventEdge leaves");
    const auto alias = fixture.compute(OperationKind::kAssign, Fixture::type(1), {joined});
    graph.replaceOperand(writer, 0, alias); graph.replaceOperand(peerWriter, 0, alias);
    graph.replaceOperand(call, 2, alias);
    fixture.split(); fixture.partition();
    require(owner(graph, graph.valueDef(alias)).size() == 4 && owner(graph, graph.valueDef(alias))[1] == "nonsink",
            "Joined activity fixture lost its held shared Assign snapshot");
    const std::vector<std::pair<grh::OperationId, std::string>> sites{
        {writer, "writer-sample"}, {graph.valueDef(payload), "operation"}};
    const std::string support = batchActivityCounterSupport(sites.size()) + R"CPP(
static GrhSIM_top* joinedInstance;
extern "C" void activity_hold_join_false(std::uint8_t) {
    // Test-only perturbation of the held snapshot, after its computation and
    // before writer application. The current EventEdge leaves remain intact.
    if (joinedInstance) joinedInstance->test_hold_join_false();
}
)CPP";
    const std::string body = R"CPP(
const auto evaluate = [&](GrhSIM_top& instance,unsigned clocks,unsigned work,unsigned value) {
    joinedInstance=&instance; const auto before=batchCounts(instance);
    instance.joined_clock_a=clocks&1; instance.joined_clock_b=(clocks>>1)&1; instance.eval();
    const auto after=batchCounts(instance);
    assert(after[0]==before[0]+work && after[1]==before[1]+work && instance.joined_q==value);
    instance.eval(); assert(batchCounts(instance)==after && instance.joined_q==value);
};
model.init(); model.joined_enable=true; model.joined_data=5;
evaluate(model,0,0,17); evaluate(model,1,1,10);
evaluate(model,0,0,10); evaluate(model,1,0,10);
evaluate(model,2,0,10); // B is a fresh edge while A falls; stable data stays clean.
model.joined_data=7; evaluate(model,2,0,10);
evaluate(model,0,0,10); evaluate(model,2,1,14);
evaluate(model,0,0,14); evaluate(model,3,0,14);
model.joined_data=9; evaluate(model,3,0,14);
evaluate(model,0,0,14); evaluate(model,3,1,18); // Simultaneous edges apply once.
model.joined_enable=false; model.joined_data=11;
evaluate(model,0,0,18); evaluate(model,1,0,18);
model.joined_enable=true; evaluate(model,1,0,18);
evaluate(model,0,0,18); evaluate(model,1,1,22);
GrhSIM_top other; other.init(); other.joined_enable=true;
evaluate(other,0,0,17); evaluate(other,2,1,0);
evaluate(model,0,0,22); evaluate(model,2,0,22);
other.joined_clock_a=other.joined_clock_b=false; other.init(); other.joined_enable=true;
evaluate(other,0,0,17); evaluate(other,3,1,0);
evaluate(model,0,0,22); evaluate(model,3,0,22);
joinedInstance=nullptr;
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "sink-activity-joined-fire-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "eligible_joined_fire_runs") != 0 &&
                    batchActivityStatistic(directory, "eligible_writers") == 2 &&
                    batchActivityStatistic(directory, "rejected_writers") == 0,
                "Joined activity silently retained fallback writers");
        auto header = readFile(directory / "grhsim_top.hpp");
        const auto member = "nonSinkBoundaryStore." + mappedValue(directory, alias);
        require(header.find(mappedValue(directory, alias)) != std::string::npos,
                "Joined activity lacks its real held fire snapshot");
        const auto insertion = header.find("private:\n");
        require(insertion != std::string::npos, "Joined activity lacks its private boundary");
        // The fixture-only method changes access, without changing production
        // emission or replacing any writer/computation/dispatch statement.
        header.insert(insertion, "    void test_hold_join_false() { " + member + " = false; }\n");
        std::ofstream(directory / "grhsim_top.hpp") << header;
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testSinkActivityJoinedCompetition() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clockA = fixture.input("joined_comp_clock_a"), clockB = fixture.input("joined_comp_clock_b");
    const auto target = fixture.state("joined_comp_q", 8);
    grh::setStateInitSpec(graph, target, {{grh::InitKind::Constant, "8'd17"}});
    std::array<grh::ValueId, 3> payloads;
    std::array<grh::OperationId, 3> writers;
    const std::array<std::string, 3> names{"a", "b", "c"};
    for (unsigned i = 0; i < names.size(); ++i) {
        const auto enable = fixture.input("joined_comp_enable_" + names[i]);
        const auto data = fixture.input("joined_comp_data_" + names[i], 8);
        payloads[i] = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, data});
        writers[i] = fixture.write(OperationKind::kRegisterWritePort, target,
            {enable, payloads[i], clockA, clockB}, {"posedge", "posedge"});
    }
    graph.bindOutputPort("joined_comp_q", fixture.read(target));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto joined = graph.opOperands(writers[0])[0];
    const auto alias = fixture.compute(OperationKind::kAssign, Fixture::type(1), {joined});
    for (const auto writer : writers) graph.replaceOperand(writer, 0, alias);
    fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == 3, "Joined competition lost distinct A/B/A priority runs");
    std::vector<std::pair<std::string, std::string>> sites;
    for (unsigned i = 0; i < writers.size(); ++i) {
        require(owner(graph, graph.valueDef(payloads[i])) == schedule.generalRuns[i].partitionKey,
                "Joined competition lost a private payload");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(writers[i])), "writer-sample");
        sites.emplace_back(graph.symbolText(graph.operationSymbol(graph.valueDef(payloads[i]))), "operation");
    }
    const std::string body = R"CPP(
model.init(); model.joined_comp_enable_a=true;
model.joined_comp_data_a=4; model.joined_comp_data_b=7; model.joined_comp_data_c=4;
const auto evaluate = [&](unsigned clocks,std::array<unsigned,3> work,unsigned value) {
    const auto before=activityCounts(model);
    model.joined_comp_clock_a=clocks&1; model.joined_comp_clock_b=(clocks>>1)&1; model.eval();
    const auto after=activityCounts(model); activityDelta(before,after,work);
    assert(model.joined_comp_q==value);
    model.eval(); assert(activityCounts(model)==after && model.joined_comp_q==value);
};
evaluate(0,{0,0,0},17); evaluate(1,{1,0,0},8);
evaluate(0,{0,0,0},8); evaluate(2,{0,0,0},8);
model.joined_comp_enable_b=model.joined_comp_enable_c=true;
evaluate(0,{0,0,0},8); evaluate(3,{0,1,1},8);
for (unsigned edge : {1u,2u,3u}) {
    evaluate(0,{0,0,0},8); evaluate(edge,{1,1,1},8);
}
model.joined_comp_enable_b=model.joined_comp_enable_c=false;
evaluate(0,{0,0,0},8); evaluate(3,{1,0,0},8);
evaluate(0,{0,0,0},8); evaluate(1,{0,0,0},8);
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "sink-activity-joined-competition-b" + std::to_string(budget);
        manualCase(fixture, label, budget, sinkActivityCounterSupport(), body, false, {{"execution-counters", "on"}});
        bindSinkActivitySites(label, sites, 3, 1, 3);
        require(batchActivityStatistic(artifact(label) / "model", "eligible_joined_fire_runs") == 3,
                "Joined competition retained a non-activity fallback");
    }
}

void testSinkActivityCurrentReads() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto readClock = fixture.input("current_read_clock"), writeClock = fixture.input("current_write_clock");
    const auto selfClock = fixture.input("current_self_clock");
    const auto writeEnable = fixture.input("current_write_enable"), readIndex = fixture.input("current_read_index", 8);
    const auto writeIndex = fixture.input("current_write_index", 8), writeData = fixture.input("current_write_data", 8);
    const auto modeEnable = fixture.input("current_mode_enable"), modeData = fixture.input("current_mode_data", 8);
    const auto modeCancel = fixture.input("current_mode_cancel");
    const auto selfEnable = fixture.input("current_self_enable");
    const auto one = fixture.constant(8, "8'd1");
    const auto source = fixture.state("current_source", 1024, {128});
    const auto mode = fixture.state("current_mode", 8), target = fixture.state("current_q", 8);
    const auto feedback = fixture.state("current_feedback", 8);
    grh::setStateInitSpec(graph, source, {{grh::InitKind::Fill, "8'd7"}});
    grh::setStateInitSpec(graph, mode, {{grh::InitKind::Constant, "8'd1"}});
    grh::setStateInitSpec(graph, target, {{grh::InitKind::Constant, "8'd17"}});
    fixture.write(OperationKind::kRegisterWriteArrayPort, source, {writeEnable, writeIndex, writeData, writeClock});
    fixture.write(OperationKind::kRegisterWritePort, mode, {modeEnable, modeData, writeClock});
    fixture.write(OperationKind::kRegisterWritePort, mode, {modeCancel, fixture.constant(8, "8'd0"), writeClock});
    fixture.write(OperationKind::kRegisterWritePort, mode, {modeCancel, modeData, writeClock});
    const auto modeRead = fixture.read(mode);
    const auto localEnable = fixture.compute(OperationKind::kSliceStatic, Fixture::type(1), {modeRead});
    graph.setAttr(graph.valueDef(localEnable), "sliceStart", int64_t{0});
    graph.setAttr(graph.valueDef(localEnable), "sliceEnd", int64_t{0});
    const auto offset = fixture.compute(OperationKind::kLShr, Fixture::type(8), {modeRead, one});
    const auto index = fixture.compute(OperationKind::kAdd, Fixture::type(8), {readIndex, offset});
    const auto read = fixture.read(source, {index});
    const auto payload = fixture.compute(OperationKind::kAdd, Fixture::type(8), {read, fixture.constant(8, "8'd0")});
    const auto writer = fixture.write(OperationKind::kRegisterWritePort, target, {localEnable, payload, readClock});
    const auto increment = fixture.compute(OperationKind::kAdd, Fixture::type(8), {fixture.read(feedback), one});
    fixture.write(OperationKind::kRegisterWritePort, feedback, {selfEnable, increment, selfClock});
    graph.bindOutputPort("current_q", fixture.read(target));
    graph.bindOutputPort("current_feedback", fixture.read(feedback));
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split(); fixture.partition();
    require(owner(graph, graph.valueDef(read)).size() == 3 && owner(graph, graph.valueDef(read))[1] == "small-sink",
            "Current-read activity lost its private large source read");
    const std::vector<std::pair<grh::OperationId, std::string>> sites{
        {writer, "writer-sample"}, {graph.valueDef(payload), "operation"}};
    const std::string body = R"CPP(
const auto evaluate = [&](GrhSIM_top& instance,unsigned clocks,unsigned work,unsigned value) {
    const auto before=batchCounts(instance);
    instance.current_read_clock=clocks&1; instance.current_write_clock=(clocks>>1)&1;
    instance.current_self_clock=(clocks>>2)&1; instance.eval();
    const auto after=batchCounts(instance);
    assert(after[0]==before[0]+work && after[1]==before[1]+work && instance.current_q==value);
    instance.eval(); assert(batchCounts(instance)==after && instance.current_q==value);
};
model.init(); evaluate(model,0,0,17); evaluate(model,1,1,7);
evaluate(model,0,0,7); evaluate(model,1,0,7);
model.current_read_index=2; evaluate(model,1,0,7);
evaluate(model,0,0,7); evaluate(model,1,1,7);
model.current_write_enable=true; model.current_write_index=2; model.current_write_data=9;
evaluate(model,0,0,7); evaluate(model,2,0,7);
model.current_write_enable=false; evaluate(model,0,0,7); evaluate(model,1,1,9);
evaluate(model,0,0,9); evaluate(model,1,0,9);
model.current_mode_enable=true; model.current_mode_data=0;
evaluate(model,0,0,9); evaluate(model,3,0,9);
model.current_mode_enable=false; evaluate(model,0,0,9); evaluate(model,1,0,9);
model.current_mode_enable=true; model.current_mode_data=1;
evaluate(model,0,0,9); evaluate(model,2,0,9);
model.current_mode_enable=false; evaluate(model,0,0,9); evaluate(model,1,1,9);
// Make the reader dirty without changing its final index. Both phases must
// sample old current before the large write invalidates the later edge.
model.current_read_index=3; evaluate(model,0,0,9);
model.current_read_index=2; evaluate(model,0,0,9);
model.current_write_enable=true; model.current_write_data=11;
evaluate(model,3,1,9);
model.current_write_enable=false; evaluate(model,0,0,9); evaluate(model,1,1,11);
// The same current-state read controls enable and a dynamic array index.
model.current_mode_enable=true; model.current_mode_data=3;
evaluate(model,0,0,11); evaluate(model,2,0,11);
model.current_mode_enable=false; evaluate(model,0,0,11); evaluate(model,1,1,7);
model.current_mode_enable=true; model.current_mode_data=1;
evaluate(model,0,0,7); evaluate(model,2,0,7);
model.current_mode_enable=false; evaluate(model,0,0,7); evaluate(model,1,1,11);
// Small next-shadow A/B/A cancellation leaves current and its reader clean.
model.current_mode_cancel=true; evaluate(model,0,0,11); evaluate(model,2,0,11);
model.current_mode_cancel=false; evaluate(model,0,0,11); evaluate(model,1,0,11);
model.current_read_index=255; evaluate(model,1,0,11);
evaluate(model,0,0,11); evaluate(model,1,1,0);
model.current_read_index=2; evaluate(model,1,0,0);
evaluate(model,0,0,0); evaluate(model,1,1,11);
model.current_self_enable=true;
for (unsigned value=1; value<=4; ++value) {
    evaluate(model,0,0,11); evaluate(model,4,0,11); assert(model.current_feedback==value);
}
model.current_self_enable=false; evaluate(model,0,0,11); evaluate(model,4,0,11);
assert(model.current_feedback==4);
GrhSIM_top other; other.init(); evaluate(other,0,0,17); evaluate(other,1,1,7);
evaluate(model,0,0,11); evaluate(model,1,0,11);
other.current_read_clock=other.current_write_clock=false; other.init();
evaluate(other,0,0,17); evaluate(other,1,1,7);
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "sink-activity-current-reads-b" + std::to_string(budget);
        manualCase(fixture, label, budget, batchActivityCounterSupport(sites.size()), body, false,
                   {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "eligible_current_state_runs") >= 2 &&
                    batchActivityStatistic(directory, "subscribed_small_current_states") == 2 &&
                    batchActivityStatistic(directory, "subscribed_large_current_states") == 1 &&
                    batchActivityStatistic(directory, "small_current_publication_mask_sites") >= 2 &&
                    batchActivityStatistic(directory, "large_current_write_mask_sites") != 0,
                "Current-read fixture lost actual source-state invalidation routes");
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testSinkActivityLargeCurrentInterrupted() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto readClock = fixture.input("partial_read_clock"), writeClock = fixture.input("partial_write_clock");
    const auto enable = fixture.input("partial_write_enable"), dataA = fixture.input("partial_data_a", 8);
    const auto dataB = fixture.input("partial_data_b", 8), one = fixture.constant(1, "1'b1");
    const auto zero = fixture.constant(8, "8'd0"), first = fixture.constant(8, "8'd1");
    const auto source = fixture.state("partial_source", 1024, {128}), target = fixture.state("partial_q", 8);
    fixture.write(OperationKind::kRegisterWriteArrayPort, source, {enable, zero, dataA, writeClock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, source, {enable, first, dataB, writeClock});
    const auto payload = fixture.read(source, {zero});
    const auto writer = fixture.write(OperationKind::kRegisterWritePort, target, {one, payload, readClock});
    graph.bindOutputPort("partial_q", fixture.read(target));
    graph.bindOutputPort("partial_tail", fixture.read(source, {first}));
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split(); fixture.partition();
    const std::vector<std::pair<grh::OperationId, std::string>> sites{
        {writer, "writer-sample"}, {graph.valueDef(payload), "operation"}};
    const std::string support = R"CPP(
#include <cstdlib>
#include <new>
static unsigned partialAllocations;
static bool partialAllocationArmed;
void* operator new(std::size_t bytes) {
    if (partialAllocationArmed && ++partialAllocations==2) throw std::bad_alloc();
    if (auto* result=std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
)CPP" + batchActivityCounterSupport(sites.size());
    const std::string body = R"CPP(
for (unsigned epoch=0; epoch<2; ++epoch) {
    model.partial_read_clock=model.partial_write_clock=false; model.partial_write_enable=false;
    model.init(); model.eval();
    model.partial_read_clock=true; model.eval(); assert(model.partial_q==0);
    model.partial_read_clock=false; model.eval();
    const auto clean=batchCounts(model);
    model.partial_read_clock=true; model.eval(); assert(batchCounts(model)==clean);
    model.partial_read_clock=false; model.eval();
    model.partial_write_enable=true; model.partial_data_a=9; model.partial_data_b=17;
    model.partial_write_clock=true; partialAllocations=0; partialAllocationArmed=true;
    bool caught=false; try { model.eval(); } catch (const std::bad_alloc&) { caught=true; }
    partialAllocationArmed=false;
    assert(caught && partialAllocations==2 && model.partial_q==0 && batchCounts(model)==clean);
    // The first current store completed, but Publish never ran. A new reader
    // edge must see it immediately; the consumed write edge must not replay.
    model.partial_read_clock=true; model.eval();
    assert(model.partial_q==9 && model.partial_tail==0);
    const auto after=batchCounts(model);
    assert(after[0]==clean[0]+1 && after[1]==clean[1]+1);
    model.eval(); assert(batchCounts(model)==after && model.partial_q==9 && model.partial_tail==0);
    model.partial_read_clock=false; model.eval(); model.partial_read_clock=true; model.eval();
    assert(batchCounts(model)==after && model.partial_q==9);
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "sink-activity-large-current-interrupted-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false,
                   {{"execution-counters", "on"}, {"mem-write-activate", "on-change"}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "subscribed_large_current_states") == 1 &&
                    batchActivityStatistic(directory, "large_current_write_mask_sites") == 2 &&
                    batchActivityStatistic(directory, "small_current_publication_mask_sites") == 0,
                "Interrupted current reader lacks both immediate store invalidations");
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testSinkActivityWholeCurrentWrites() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto readClock = fixture.input("whole_read_clock"), writeClock = fixture.input("whole_write_clock");
    const auto enable = fixture.input("whole_write_enable"), cancel = fixture.input("whole_cancel_enable");
    const auto data = graph.createValue(graph.internSymbol("whole_write_data"), Fixture::type(1024, {128}));
    graph.bindInputPort("whole_write_data", data);
    const auto one = fixture.constant(1, "1'b1"), zero = fixture.constant(8, "8'd0");
    const auto fill = fixture.constant(1024, "1024'h" + std::string(256, 'd'), {128});
    const auto source = fixture.state("whole_source", 1024, {128}), target = fixture.state("whole_q", 8);
    grh::setStateInitSpec(graph, source, {{grh::InitKind::Fill, "8'd3"}});
    grh::setStateInitSpec(graph, target, {{grh::InitKind::Constant, "8'd17"}});
    fixture.write(OperationKind::kRegisterWritePort, source, {enable, data, writeClock});
    fixture.write(OperationKind::kRegisterWritePort, source, {cancel, fill, writeClock});
    fixture.write(OperationKind::kRegisterWritePort, source, {cancel, data, writeClock});
    const auto payload = fixture.read(source, {zero});
    const auto writer = fixture.write(OperationKind::kRegisterWritePort, target, {one, payload, readClock});
    graph.bindOutputPort("whole_q", fixture.read(target));
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split(); fixture.partition();
    const std::vector<std::pair<grh::OperationId, std::string>> sites{
        {writer, "writer-sample"}, {graph.valueDef(payload), "operation"}};
    const std::string body = R"CPP(
model.init();
const auto evaluate = [&](unsigned clocks,unsigned work,unsigned value) {
    const auto before=batchCounts(model);
    model.whole_read_clock=clocks&1; model.whole_write_clock=(clocks>>1)&1; model.eval();
    const auto after=batchCounts(model);
    assert(after[0]==before[0]+work && after[1]==before[1]+work && model.whole_q==value);
    model.eval(); assert(batchCounts(model)==after && model.whole_q==value);
};
evaluate(0,0,17); evaluate(1,1,3); evaluate(0,0,3); evaluate(1,0,3);
model.whole_write_enable=true; model.whole_write_data.fill(9);
evaluate(0,0,3); evaluate(2,0,3);
model.whole_write_enable=false; evaluate(0,0,3); evaluate(1,1,9);
evaluate(0,0,9); evaluate(1,0,9);
// A same-value whole store follows the original activation policy.
model.whole_write_enable=true; evaluate(0,0,9); evaluate(2,0,9);
model.whole_write_enable=false; evaluate(0,0,9); evaluate(1,WHOLE_ONWRITE,9);
// Large current A/B/A stores conservatively invalidate even with no net change.
model.whole_cancel_enable=true; evaluate(0,0,9); evaluate(2,0,9);
model.whole_cancel_enable=false; evaluate(0,0,9); evaluate(1,1,9);
evaluate(0,0,9); evaluate(1,0,9);
)CPP";
    for (const auto budget : {16, 1024}) for (const std::string policy : {"on-change", "on-write"}) {
        const auto label = "sink-activity-whole-current-b" + std::to_string(budget) + "-" + policy;
        const auto support = "#define WHOLE_ONWRITE " + std::string(policy == "on-write" ? "1\n" : "0\n") +
            batchActivityCounterSupport(sites.size());
        manualCase(fixture, label, budget, support, body, false,
                   {{"execution-counters", "on"}, {"mem-write-activate", policy}});
        const auto directory = artifact(label) / "model";
        require(batchActivityStatistic(directory, "subscribed_large_current_states") == 1 &&
                    batchActivityStatistic(directory, "large_current_write_mask_sites") == 3 &&
                    batchActivityStatistic(directory, "small_current_publication_mask_sites") == 0,
                "Whole-array current reader lacks an immediate store invalidation");
        bindBatchActivityCounters(label, graph, sites);
    }
}

void testSinkActivityBatchMasksAndFallback() {
    Fixture fixture; auto& graph=fixture.graph;
    std::vector<grh::ValueId> clocks;
    std::vector<grh::OperationId> states;
    for(unsigned i=0;i<34;++i) clocks.push_back(fixture.input("batch_clock_"+std::to_string(i)));
    for(unsigned i=0;i<33;++i) {
        const auto data=fixture.input("batch_data_"+std::to_string(i),8);
        const auto payload=fixture.compute(OperationKind::kAdd,Fixture::type(8),{data,data});
        const auto state=fixture.state("batch_q_"+std::to_string(i),8); states.push_back(state);
        grh::setStateInitSpec(graph,state,{{grh::InitKind::Constant,"8'd"+std::to_string(i+17)}});
        fixture.write(OperationKind::kRegisterWritePort,state,{fixture.constant(1,"1'b1"),payload,clocks[i]});
        graph.bindOutputPort("batch_q_"+std::to_string(i),fixture.read(state));
    }
    const auto fallback=fixture.state("batch_fallback",1);
    const auto fallbackWriter=fixture.write(OperationKind::kRegisterWritePort,fallback,
        {fixture.constant(1,"1'b1"),fixture.input("batch_unused"),clocks.back()});
    graph.bindOutputPort("batch_fallback",fixture.read(fallback));
    run(fixture.design,"grh-to-grhsim",fixture.session);
    graph.replaceOperand(fallbackWriter,2,graph.opOperands(fallbackWriter)[0]);
    fixture.split(); fixture.partition();
    const auto& schedule=sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==34,"Batch masks lost their distinct fire runs");
    std::vector<grh::OperationId> writers;
    std::vector<std::pair<grh::OperationId,std::string>> sites;
    for(unsigned i=0;i<33;++i) {
        const auto& run=schedule.generalRuns[i];
        require(run.orderedWriters.size()==1 && sim::writerTarget(graph,run.orderedWriters.front())==states[i],
                "Batch masks changed original run positions");
        const auto writer=run.orderedWriters.front(); writers.push_back(writer);
        const auto payload=graph.valueDef(graph.opOperands(writer)[2]);
        require(graph.opKind(payload)==OperationKind::kAdd && owner(graph,payload)==run.partitionKey,
                "Batch masks lost their private counted payload");
        sites.emplace_back(writer,"writer-sample"); sites.emplace_back(payload,"operation");
    }
    sites.emplace_back(fallbackWriter,"writer-sample");
    std::string support=batchActivityCounterSupport(sites.size());
    support+="static void batchClocks(GrhSIM_top& instance,bool value) {\n";
    std::string checks="static void batchOutputs(GrhSIM_top& instance,unsigned updated,unsigned value,bool initialized,bool fired) {\n";
    for(unsigned i=0;i<34;++i) support+="instance.batch_clock_"+std::to_string(i)+"=value;\n";
    for(unsigned i=0;i<33;++i) checks+="assert(instance.batch_q_"+std::to_string(i)+"==(initialized ? "+
        std::to_string(i+17)+" : updated=="+std::to_string(i)+" ? value : 0));\n";
    support+="}\n"+checks+"assert(instance.batch_fallback==fired);\n}\n";
    const std::string body=R"CPP(
model.init(); bool fired=false;
const auto step = [&](bool clock,unsigned selected,unsigned work,unsigned value,bool initial=false) {
    const auto before=batchCounts(model); batchClocks(model,clock); model.eval(); const auto after=batchCounts(model);
    for(unsigned i=0;i<33;++i) {
        const unsigned expected=clock && (selected==99 || selected==i);
        assert(after[2*i]==before[2*i]+expected && after[2*i+1]==before[2*i+1]+expected);
    }
    assert(after[66]==before[66]+(clock ? 1 : 0)); fired|=clock;
    batchOutputs(model,work,value,initial,fired); model.eval(); assert(batchCounts(model)==after);
};
step(false,98,98,0,true); step(true,99,98,0);
step(false,98,98,0); step(true,98,98,0);
// Only the final eligible bit is dirty. The first two batches share its word,
// and their exact coarse masks must exclude it (checked in generated shape).
model.batch_data_32=7; step(false,98,98,0); step(true,32,32,14);
step(false,98,32,14); step(true,98,32,14);
model.batch_data_32=0; model.batch_data_16=3;
step(false,98,32,14); // Output remains old until the next legitimate edges.
// Two dirty runs reside in different batches but the same activity word.
const auto before=batchCounts(model); batchClocks(model,true); model.eval(); const auto after=batchCounts(model);
for(unsigned i=0;i<33;++i) {
    const unsigned expected=i==16 || i==32;
    assert(after[2*i]==before[2*i]+expected && after[2*i+1]==before[2*i+1]+expected);
}
assert(after[66]==before[66]+1); batchOutputs(model,16,6,false,true);
step(false,98,16,6); step(true,98,16,6);
)CPP";
    for(const auto budget : {16,1024}) {
        const auto label="sink-activity-batch-masks-fallback-b"+std::to_string(budget);
        manualCase(fixture,label,budget,support,body,false,{{"execution-counters","on"}});
        const auto directory=artifact(label)/"model";
        require(batchActivityStatistic(directory,"eligible_runs")==33 && batchActivityStatistic(directory,"rejected_runs")==1 &&
                    batchActivityStatistic(directory,"bitmap_words")==1 && batchActivityStatistic(directory,"publication_change_sources")==0 &&
                    batchActivityStatistic(directory,"writer_batches")==3 &&
                    batchActivityStatistic(directory,"writer_batches_all_eligible")==2 &&
                    batchActivityStatistic(directory,"writer_batches_with_activity_guard")==2 &&
                    batchActivityStatistic(directory,"writer_batch_activity_mask_words")==2 &&
                    batchActivityStatistic(directory,"writer_batch_activity_max_mask_words")==1,
                "Batch masks silently lost eligible or fallback runs");
        require(readFile(directory/"sink-activity-stats.json").find("\"fire-as-data\": {\"runs\": 1, \"writers\": 1}")!=std::string::npos,
                "Mixed batch lost its actual fire-as-data fallback");
        const auto a=batchActivityRoot(directory,writers[0]),b=batchActivityRoot(directory,writers[16]),c=batchActivityRoot(directory,writers[32]);
        require(a!=b && a!=c && b!=c && batchActivityRoot(directory,writers[15])==a &&
                    batchActivityRoot(directory,writers[31])==b && batchActivityRoot(directory,fallbackWriter)==c,
                "Batch masks changed the original 16-fire splits or flushed around eligibility");
        const auto first=batchActivityRootCpp(directory,a),second=batchActivityRootCpp(directory,b),mixed=batchActivityRootCpp(directory,c);
        require(first.find("if ((sinkDirty[0] & UINT64_C(65535)))")!=std::string::npos &&
                    second.find("if ((sinkDirty[0] & UINT64_C(4294901760)))")!=std::string::npos,
                "Shared-word batches probed activity bits outside their exact run masks");
        require(mixed.find("if ((sinkDirty[")==std::string::npos &&
                    mixed.find("if (sinkDirty[0] & UINT64_C(4294967296))")!=std::string::npos,
                "Mixed fallback batch gained a coarse activity gate or lost the eligible inner gate");
        bindBatchActivityCounters(label,graph,sites);
    }
}

void testSparsePhaseDispatch() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("clock_a"), clockB = fixture.input("clock_b");
    const auto bulk = fixture.input("bulk"), tail = fixture.input("tail");
    const auto enableA = fixture.input("enable_a"), enableB = fixture.input("enable_b");
    const auto gateEnable = fixture.input("gate_enable"), gateData = fixture.input("gate_data");
    const auto data = fixture.input("data", 8), address = fixture.input("address", 4);
    const auto one = fixture.constant(1, "1'b1"), zero = fixture.constant(1, "1'b0");
    const auto three = fixture.constant(8, "8'd3"), seven = fixture.constant(8, "8'd7");
    const auto priorityMask = fixture.constant(8, "8'hf0");
    const auto dataB = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, three});
    const auto dataLastA = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, seven});
    const auto packedStates = graph.createOperation(OperationKind::kArrayCreate);
    std::vector<grh::OperationId> states;
    for (unsigned i = 0; i < 145; ++i) {
        const auto state = fixture.state("sparse" + std::to_string(i), 8);
        states.push_back(state);
        // Distinct initial values keep 145 independent current/next fields.
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, "8'd" + std::to_string(i + 1)}});
        fixture.write(OperationKind::kRegisterWritePort, state,
            {i < 128 ? bulk : tail, data, i % 2 ? clockB : clockA});
        graph.addOperand(packedStates, fixture.read(state));
    }
    const auto packed = graph.createValue(Fixture::type(145 * 8, {145}));
    graph.addResult(packedStates, packed); graph.bindOutputPort("states", packed);
    const auto constantOn = fixture.state("constant_on", 8);
    const auto constantOff = fixture.state("constant_off", 8);
    const auto localState = fixture.state("local_enable", 8);
    const auto mixedState = fixture.state("mixed_fire", 8);
    const auto priority = fixture.state("priority", 8);
    const auto localEnable = fixture.compute(OperationKind::kAnd, Fixture::type(1),
        {enableA, fixture.compute(OperationKind::kNot, Fixture::type(1), {bulk})});
    fixture.write(OperationKind::kRegisterWritePort, constantOn, {one, data, clockB});
    fixture.write(OperationKind::kRegisterWritePort, constantOff, {zero, data, clockB});
    fixture.write(OperationKind::kRegisterWritePort, localState, {localEnable, dataB, clockA});
    fixture.write(OperationKind::kRegisterWritePort, mixedState, {enableB, dataLastA, clockA, clockB},
        {"posedge", "negedge"});
    // Preserve A/B/A priority when enabled runs are skipped or grouped.
    fixture.write(OperationKind::kRegisterWritePort, priority, {enableA, data, clockA});
    fixture.write(OperationKind::kRegisterWritePort, priority, {enableB, dataB, clockA});
    fixture.write(OperationKind::kRegisterWriteMaskPort, priority, {enableA, dataLastA, priorityMask, clockA});
    const auto gate = fixture.state("derived_gate", 1);
    const auto derived = fixture.state("derived_sink", 8);
    const auto gateRead = fixture.read(gate);
    fixture.write(OperationKind::kRegisterWritePort, gate, {gateEnable, gateData, clockA});
    const auto derivedData = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, fixture.read(states.front())});
    fixture.write(OperationKind::kRegisterWritePort, derived, {one, derivedData, gateRead});
    for (const auto& [name, state] : std::vector<std::pair<std::string, grh::OperationId>>{
            {"constant_on", constantOn}, {"constant_off", constantOff}, {"local", localState},
            {"mixed", mixedState}, {"priority", priority}, {"gate", gate}, {"derived", derived}})
        graph.bindOutputPort(name, fixture.read(state));
    const auto memoryA = fixture.state("memory_a", 128, {16});
    const auto memoryB = fixture.state("memory_b", 128, {16});
    const auto memoryPriority = fixture.state("memory_priority", 128, {16});
    grh::setStateInitSpec(graph, memoryA, {{grh::InitKind::Fill, "8'd17"}});
    grh::setStateInitSpec(graph, memoryB, {{grh::InitKind::Fill, "8'd33"}});
    // Both payloads must be captured before either large state is committed.
    fixture.write(OperationKind::kRegisterWriteArrayPort, memoryA,
        {enableA, address, fixture.read(memoryB, {address}), clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memoryB,
        {enableB, address, fixture.read(memoryA, {address}), clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memoryPriority, {enableA, address, data, clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memoryPriority, {enableB, address, dataB, clockA});
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memoryPriority,
        {enableA, address, dataLastA, priorityMask, clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memoryPriority, {tail, address, derivedData, gateRead});
    graph.bindOutputPort("memory_a", fixture.read(memoryA));
    graph.bindOutputPort("memory_b", fixture.read(memoryB));
    graph.bindOutputPort("memory_priority", fixture.read(memoryPriority));
    fixture.convert(); const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=8"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=4"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    const auto localOwner = owner(graph, graph.valueDef(localEnable));
    require(localOwner.size() > 1 && localOwner[1] == "small-sink",
        "Sparse dispatch regression lost its sink-local enable cone");
    std::vector<std::map<std::string, uint64_t>> inputs;
    const auto step = [&](unsigned a, unsigned b, unsigned bulkValue, unsigned tailValue,
                          unsigned enableAValue, unsigned enableBValue, unsigned gateEnableValue,
                          unsigned gateDataValue, unsigned dataValue, unsigned addressValue) {
        inputs.push_back({{"clock_a", a}, {"clock_b", b}, {"bulk", bulkValue}, {"tail", tailValue},
            {"enable_a", enableAValue}, {"enable_b", enableBValue}, {"gate_enable", gateEnableValue},
            {"gate_data", gateDataValue}, {"data", dataValue}, {"address", addressValue}});
    };
    step(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    step(1, 0, 0, 1, 0, 0, 0, 0, 201, 0); // Only the final publish batch is dirty.
    inputs.push_back(inputs.back()); // All clocks and inputs clean.
    step(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    step(0, 1, 0, 0, 0, 0, 0, 0, 19, 0); // Constant enable on clock B.
    step(0, 0, 0, 0, 0, 1, 0, 0, 23, 0); // Falling clock B mixed fire.
    step(1, 1, 1, 1, 1, 1, 1, 1, 43, 2); // Both input edges, followed by NBA derived edge.
    inputs.push_back(inputs.back());
    step(0, 0, 0, 0, 0, 0, 0, 1, 53, 2);
    step(1, 0, 0, 0, 0, 1, 1, 0, 61, 2); // B priority alone, gate falls after publish.
    step(0, 0, 0, 0, 1, 0, 0, 0, 67, 3);
    step(1, 0, 0, 1, 1, 0, 1, 1, 71, 3); // A priority and private local enable.
    step(1, 1, 0, 0, 0, 0, 0, 1, 79, 4); // Clock B rises while A stays high.
    step(0, 0, 0, 0, 0, 0, 0, 1, 83, 4);
    step(1, 1, 1, 0, 1, 1, 1, 0, 89, 4);
    step(0, 0, 0, 0, 0, 0, 0, 0, 97, 15);
    step(1, 0, 0, 1, 1, 1, 1, 1, 101, 15);
    inputs.push_back(inputs.back());
    for (const auto budget : {16u, 1024u}) for (const std::string bitmap : {"on", "off"}) {
        const auto label = "sparse-dispatch-b" + std::to_string(budget) + "-bitmap-" + bitmap;
        writeCase(fixture, reference, inputs, label, budget, 4096,
            {{"mem-enable-bitmap", bitmap}, {"mem-write-activate", "on-write"}});
        const auto directory = artifact(label) / "model";
        const auto header = readFile(directory / "grhsim_top.hpp");
        require(header.find("pPublish_dirty_batch1") != std::string::npos,
            "Sparse publish fixture did not cross its 128-state batch boundary");
        std::string sources;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp") sources += readFile(entry.path());
        const std::string clearByte = "dirtyStateFlags[0] = 0;";
        const auto clearPosition = sources.find(clearByte);
        require(sources.find("dirty_flags") != std::string::npos && clearPosition != std::string::npos &&
                    sources.find(clearByte, clearPosition + clearByte.size()) == std::string::npos &&
                    sources.find("dirtyStateFlags[0] &=") == std::string::npos,
                "Sparse publish must snapshot its activity byte and clear it once before dispatch");
        const auto begin = sources.find("void GrhSIM_top::writer_run_batch0()\n{");
        require(begin != std::string::npos, "Sparse fixture has no writer batch entry");
        const auto end = sources.find("\n}\n", begin);
        const auto entry = sources.substr(begin, end - begin);
        const auto guard = entry.find("if (");
        require(guard != std::string::npos && entry.find("eventActStore.", guard) != std::string::npos,
            "Writer batch entry lost its clock fire guard");
        const auto call = entry.find("writer_run_batch0_chunk");
        require(call == std::string::npos || guard < call, "Writer helpers run before their fire guard");
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label
             << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
}

void testSinkRunCommonEnableFactors() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("clock_a"), clockB = fixture.input("clock_b");
    const auto gate = fixture.input("gate"), nextClock = fixture.input("next_clock");
    const auto bits = fixture.input("bits", 32), data = fixture.input("data", 8);
    const auto indexA = fixture.input("index_a", 5), indexB = fixture.input("index_b", 5);
    const auto indexC = fixture.input("index_c", 5), observedIndex = fixture.input("observed_index", 5);
    const auto factorIndexA = fixture.input("factor_index_a", 5);
    const auto factorIndexB = fixture.input("factor_index_b", 5);
    const auto factorIndexC = fixture.input("factor_index_c", 5);
    const auto one = fixture.constant(1, "1'b1"), mask = fixture.constant(8, "8'hf0");
    const auto three = fixture.constant(8, "8'd3"), seven = fixture.constant(8, "8'd7");
    const auto dataB = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, three});
    const auto dataLastA = fixture.compute(OperationKind::kAdd, Fixture::type(8), {data, seven});
    std::vector<grh::ValueId> localEnables;
    const auto enable = [&](grh::ValueId index) {
        const auto selected = fixture.compute(OperationKind::kSliceDynamic, Fixture::type(1), {bits, index});
        const auto result = fixture.compute(OperationKind::kAnd, Fixture::type(1), {gate, selected});
        localEnables.push_back(result);
        return result;
    };
    for (unsigned i = 0; i < 3; ++i) {
        const auto state = fixture.state("factor" + std::to_string(i), 8);
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, "8'd" + std::to_string(i + 1)}});
        fixture.write(OperationKind::kRegisterWritePort, state,
            {enable(i == 0 ? factorIndexA : i == 1 ? factorIndexB : factorIndexC), data, clockA});
        graph.bindOutputPort("factor" + std::to_string(i), fixture.read(state));
    }
    const auto priority = fixture.state("factor_priority", 8);
    // The final masked A must preserve bits written by the intervening B.
    const auto enableA = enable(indexA), enableB = enable(indexB);
    // Shared A belongs in nonsink; only the first three private enables
    // above are used to prove the common-factor extraction structure.
    localEnables.resize(3);
    fixture.write(OperationKind::kRegisterWritePort, priority, {enableA, data, clockB});
    fixture.write(OperationKind::kRegisterWritePort, priority, {enableB, dataB, clockB});
    fixture.write(OperationKind::kRegisterWriteMaskPort, priority, {enableA, dataLastA, mask, clockB});
    graph.bindOutputPort("priority", fixture.read(priority));
    const auto clockState = fixture.state("factor_clock", 1);
    const auto derived = fixture.state("factor_derived", 8);
    fixture.write(OperationKind::kRegisterWritePort, clockState, {one, nextClock, clockA});
    fixture.write(OperationKind::kRegisterWritePort, derived,
        {enable(indexC), dataLastA, fixture.read(clockState)});
    graph.bindOutputPort("derived_clock", fixture.read(clockState));
    graph.bindOutputPort("derived", fixture.read(derived));
    const auto observed = fixture.state("factor_observed", 8);
    const auto observedBit = fixture.compute(OperationKind::kSliceDynamic, Fixture::type(1), {bits, observedIndex});
    const auto observedEnable = fixture.compute(OperationKind::kLogicAnd, Fixture::type(1), {gate, observedBit});
    graph.setValueSymbol(observedBit, graph.internSymbol("factor_observed_bit"));
    graph.addDeclaredSymbol(graph.valueSymbol(observedBit));
    graph.setValueSymbol(observedEnable, graph.internSymbol("factor_observed_enable"));
    graph.addDeclaredSymbol(graph.valueSymbol(observedEnable));
    fixture.write(OperationKind::kRegisterWritePort, observed, {observedEnable, dataB, clockB});
    graph.bindOutputPort("observed", fixture.read(observed));
    fixture.convert(); const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session);
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=4"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    for (const auto value : localEnables) {
        const auto key = owner(graph, graph.valueDef(value));
        require(key.size() == 3 && key[1] == "small-sink",
            "Common-factor fixture lost its sink-local enables");
    }
    require(owner(graph, graph.valueDef(observedEnable)) == owner(graph, graph.valueDef(observedBit)) &&
                owner(graph, graph.valueDef(observedEnable))[1] == "small-sink",
        "Observed common-factor regression did not retain its local enable and dynamic slice");
    std::vector<std::map<std::string, uint64_t>> inputs;
    const auto step = [&](unsigned a, unsigned b, unsigned enabled, unsigned next, uint64_t selected,
                          unsigned value, unsigned ia, unsigned ib, unsigned ic, unsigned io) {
        inputs.push_back({{"clock_a", a}, {"clock_b", b}, {"gate", enabled}, {"next_clock", next},
            {"bits", selected}, {"data", value}, {"index_a", ia}, {"index_b", ib},
            {"index_c", ic}, {"observed_index", io}, {"factor_index_a", ia},
            {"factor_index_b", ib}, {"factor_index_c", ic}});
    };
    step(0, 0, 0, 0, 0, 3, 0, 1, 2, 3);
    step(1, 1, 1, 1, 15, 17, 0, 1, 2, 3); // Both clocks, then an NBA-derived edge.
    inputs.push_back(inputs.back());
    step(0, 0, 0, 1, 0, 29, 1, 2, 3, 4);
    step(1, 1, 0, 0, 0, 43, 1, 2, 3, 4); // Observed enable must clear with gate false.
    step(0, 0, 0, 0, 32, 51, 2, 3, 4, 5);
    step(0, 1, 0, 0, 32, 63, 2, 3, 4, 5); // Observed bit refreshes even when gate remains false.
    step(1, 1, 1, 1, 14, 79, 0, 1, 2, 5); // B enabled, A disabled; derived edge follows A.
    inputs.push_back(inputs.back());
    step(0, 0, 1, 1, 5, 83, 0, 1, 2, 0);
    step(1, 0, 1, 0, 5, 97, 0, 1, 2, 0); // A enabled, B disabled.
    step(0, 1, 1, 0, 0x80000003u, 113, 0, 1, 31, 31);
    step(1, 1, 1, 1, 0x80000003u, 127, 0, 1, 31, 31); // A/B/A, high dynamic bit, derived edge.
    step(0, 0, 0, 1, 0, 137, 31, 30, 29, 31);
    step(1, 1, 0, 0, 0, 149, 31, 30, 29, 31);
    inputs.push_back(inputs.back());
    step(0, 0, 1, 0, 2, 157, 0, 1, 2, 1);
    step(0, 1, 1, 0, 2, 157, 0, 1, 2, 1); // B priority alone.
    step(0, 0, 1, 0, 1, 163, 0, 1, 2, 0);
    step(0, 1, 1, 0, 1, 163, 0, 1, 2, 0); // A priority alone.
    step(0, 0, 1, 0, 3, 173, 0, 1, 2, 1);
    step(0, 1, 1, 0, 3, 173, 0, 1, 2, 1); // Full A/B/A masked priority.
    const std::string support = R"CPP(
#include "fstapi.h"
#include <cassert>
#include <map>
struct FactorWave {
    std::map<fstHandle, std::string> names;
    std::map<std::string, std::map<std::uint64_t, std::string>> values;
    static void change(void *user, std::uint64_t time, fstHandle handle, const unsigned char *bits) {
        auto& wave = *static_cast<FactorWave*>(user);
        wave.values[wave.names.at(handle)][time] = reinterpret_cast<const char*>(bits);
    }
    std::string at(const std::string& name, std::uint64_t time) const {
        const auto& series = values.at(name); const auto found = series.upper_bound(time);
        assert(found != series.begin()); return std::prev(found)->second;
    }
};
static void checkFactorWave(const char *path) {
    auto *reader = fstReaderOpen(path); assert(reader); FactorWave wave;
    while (auto *hier = fstReaderIterateHier(reader)) if (hier->htyp == FST_HT_VAR)
        assert(wave.names.emplace(hier->u.var.handle, hier->u.var.name).second);
    fstReaderSetFacProcessMaskAll(reader);
    assert(fstReaderIterBlocks2(reader, FactorWave::change, nullptr, &wave, nullptr));
)CPP";
    for (const auto budget : {16u, 1024u}) {
        const auto label = "sink-common-factor-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        const auto directory = artifact(label) / "model";
        std::string sources;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp") sources += readFile(entry.path());
        const auto gateField = "inputStore." + mappedValue(directory, gate);
        bool guarded = false;
        std::istringstream source(sources); std::string line;
        while (std::getline(source, line))
            if (line.find("if (") != std::string::npos && line.find("eventActStore.") != std::string::npos &&
                line.find(gateField) != std::string::npos) guarded = true;
        require(guarded, "Common boundary gate was not lifted ahead of local dynamic enables");
        if (budget == 1024) {
            const auto indexField = "smallSinkEnableBoundaryStore." + mappedValue(directory, factorIndexA);
            bool beforeSelect = false; std::vector<bool> guards;
            std::istringstream lines(sources);
            while (std::getline(lines, line)) {
                if (line.find(" >> ") != std::string::npos && line.find(indexField) != std::string::npos)
                    beforeSelect |= std::find(guards.begin(), guards.end(), true) != guards.end();
                for (const auto ch : line) {
                    if (ch == '{') guards.push_back(line.find("if (") != std::string::npos &&
                        line.find("eventActStore.") != std::string::npos && line.find(gateField) != std::string::npos);
                    else if (ch == '}' && !guards.empty()) guards.pop_back();
                }
            }
            require(beforeSelect, "Dynamic bit selection escaped the common boundary gate guard");
        }
        std::string checks = support; bool previousClock = false, selected = false, enabled = false;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& in = inputs[i];
            if (in.at("clock_b") && !previousClock) {
                selected = (in.at("bits") >> in.at("observed_index")) & 1;
                enabled = selected && in.at("gate");
            }
            previousClock = in.at("clock_b");
            checks += "    assert(wave.at(\"factor_observed_bit\", " + std::to_string(i) + ") == \"" +
                (selected ? "1" : "0") + "\");\n";
            checks += "    assert(wave.at(\"factor_observed_enable\", " + std::to_string(i) + ") == \"" +
                (enabled ? "1" : "0") + "\");\n";
        }
        checks += "    fstReaderClose(reader);\n}\n";
        const auto driverPath = artifact(label) / "driver.cpp";
        auto driver = readFile(driverPath); driver.insert(driver.find("int main()"), checks);
        std::size_t offset = 0;
        for (unsigned epoch = 0; epoch < 3; ++epoch) {
            offset = driver.find("model.init();", offset);
            require(offset != std::string::npos, "Common-factor driver lost an initialization epoch");
            const auto configure = "model.configure_waveform(true, \"factor" + std::to_string(epoch) + ".fst\");\n";
            driver.insert(offset, configure); offset += configure.size() + std::string("model.init();").size();
        }
        driver.insert(driver.find("model.dump_runtime_profile();"),
            "model.configure_waveform(false);\ncheckFactorWave(\"factor0.fst\");\n"
            "checkFactorWave(\"factor1.fst\");\ncheckFactorWave(\"factor2.fst\");\n");
        std::ofstream(driverPath) << driver;
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label
             << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
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

void testPackedBoolUnpack() {
    Fixture fixture;
    struct UnpackCase {
        std::string name;
        int32_t sourceWidth, width;
        std::vector<int64_t> shape;
        int64_t offset = 0;
        bool complement = false;
    };
    const std::array<UnpackCase, 7> cases{{
        {"w65", 65, 65, {5, 13}},
        {"w127", 127, 127, {127}},
        {"w128", 128, 128, {2, 8, 8}},
        {"w129", 129, 129, {3, 43}},
        {"w4096", 4096, 4096, {4, 16, 64}},
        {"window", 257, 129, {3, 43}, 63},
        {"inverted", 129, 129, {3, 43}, 0, true}
    }};
    std::string prepare, verify;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index];
        const auto input = fixture.input("packed_" + item.name, item.sourceWidth);
        const auto type = Fixture::type(item.width, item.shape);
        grh::ValueId result;
        if (item.sourceWidth != item.width) {
            // Assign requires equal widths. A wider source is sliced before
            // the expression-result unpack materializes its bool leaves.
            result = fixture.compute(OperationKind::kSliceStatic, type, {input});
            fixture.graph.setAttr(fixture.graph.valueDef(result), "sliceStart", item.offset);
            fixture.graph.setAttr(fixture.graph.valueDef(result), "sliceEnd", item.offset + item.width - 1);
        } else result = fixture.compute(item.complement ? OperationKind::kNot : OperationKind::kAssign, type, {input});
        fixture.graph.bindOutputPort("bits_" + item.name, result);

        const auto words = "words_" + item.name;
        const auto wordCount = (item.sourceWidth + 63) / 64;
        const auto member = "candidate.packed_" + item.name;
        prepare += "std::array<std::uint64_t," + std::to_string(wordCount) + "> " + words + "{};\n"
            "for (unsigned word=0;word<" + std::to_string(wordCount) + ";++word) " + words +
            "[word]=unpackPatternWord(epoch,step,stream+" + std::to_string(index) + ",word);\n";
        prepare += member + "=0;\n";
        // Construct packed inputs from independent words with constant shifts;
        // neither input construction nor the oracle shifts a wide value by i.
        for (int32_t word = 0; word < wordCount; ++word)
            prepare += member + " |= static_cast<decltype(" + member + ")>(" + words + "[" +
                std::to_string(word) + "])" + (word ? " << " + std::to_string(word * 64) : "") + ";\n";
        std::string leaf = "candidate.bits_" + item.name;
        int64_t stride = item.width;
        for (const auto size : item.shape) {
            stride /= size;
            leaf += "[(i" + (stride > 1 ? "/" + std::to_string(stride) : "") + ")%" + std::to_string(size) + "]";
        }
        verify += "for (unsigned i=0;i<" + std::to_string(item.width) + ";++i) { const bool expected=" +
            std::string(item.complement ? "!" : "") + "unpackExpectedBit(" + words + ".data(),i+" +
            std::to_string(item.offset) + "); assert(" + leaf + "==expected); }\n";
    }
    fixture.convert(); fixture.split(); fixture.partition();
    const std::string support = R"CPP(
static std::uint64_t unpackPatternWord(unsigned epoch, unsigned step, unsigned stream, unsigned word) {
    if (step == 0) return 0;
    if (step == 1) return UINT64_MAX;
    if (step == 2) return UINT64_C(0xaaaaaaaaaaaaaaaa);
    if (step == 3) return UINT64_C(0x5555555555555555);
    constexpr unsigned boundaries[]{0,1,7,8,63,64,65,126,127,128,129,191,255,256,4095};
    if (step < 4 + std::size(boundaries)) {
        const auto bit = boundaries[step - 4];
        return word == bit / 64 ? UINT64_C(1) << (bit % 64) : 0;
    }
    std::uint64_t value = UINT64_C(0x9e3779b97f4a7c15) * (1 + word + stream * 67 + epoch * 257 + step * 8191);
    value ^= value << 13; value ^= value >> 7; value ^= value << 17;
    return value;
}
static bool unpackExpectedBit(const std::uint64_t* words, unsigned bit) {
    return ((words[bit / 64] >> (bit % 64)) & UINT64_C(1)) != 0;
}
)CPP";
    const std::string body = "auto exercise=[&](GrhSIM_top& candidate,unsigned epoch,unsigned step,unsigned stream) {\n" +
        prepare + "candidate.eval();\n" + verify + "candidate.eval();\n" + verify + R"CPP(
};
GrhSIM_top other;
for (unsigned epoch=0;epoch<3;++epoch) {
    model.init(); other.init();
    for (unsigned step=0;step<26;++step) {
        exercise(model,epoch,step,0);
        exercise(other,epoch,25-step,23);
        exercise(model,epoch,step,0);
    }
    const auto saved = model.bits_w4096;
    other.init();
    assert(model.bits_w4096 == saved);
    exercise(other,epoch,23,41);
    exercise(model,epoch,25,0);
}
)CPP";
    for (const auto budget : {16, 1024})
        manualCase(fixture, "packed-bool-unpack-b" + std::to_string(budget), budget, "#include <iterator>\n" + support, body, false);
}

void testPackedArrayConstants() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto choose = fixture.input("choose");
    struct ConstantCase { std::string name; grh::LogicalType type; std::string text; };
    auto signed3 = Fixture::type(72, {2, 3, 4}); signed3.isSigned = true;
    auto signed8 = Fixture::type(8, {2, 4}); signed8.isSigned = true;
    auto signed16 = Fixture::type(16, {2, 8}); signed16.isSigned = true;
    const std::array<ConstantCase, 7> cases{{
        {"zero", Fixture::type(1024, {4, 256}), "'0"},
        {"ones", Fixture::type(300, {3, 100}), "'1"},
        {"signed_three", signed3, "72'hd792e4a3b65c1987af"},
        {"signed_ones", signed3, "'1"},
        {"signed_eight", signed8, "'1"},
        {"signed_sixteen", signed16, "'1"},
        {"bytes", Fixture::type(128, {2, 4}), "128'h0123456789abcdefa569c3f00f965ab4"}
    }};
    std::vector<grh::ValueId> constants;
    for (const auto& item : cases) {
        const auto value = fixture.compute(OperationKind::kConstant, item.type, {});
        graph.setAttr(graph.valueDef(value), "constValue", item.text);
        graph.setValueSymbol(value, graph.internSymbol("packed_constant_" + item.name));
        graph.addDeclaredSymbol(graph.valueSymbol(value));
        constants.push_back(value);
        const auto data = fixture.input("data_" + item.name, item.type.width);
        const auto source = fixture.compute(OperationKind::kAssign, item.type, {data});
        graph.bindOutputPort(item.name + "_mux", fixture.compute(OperationKind::kMux, item.type, {choose, source, value}));
        graph.bindOutputPort(item.name + "_whole", value);
        graph.bindOutputPort(item.name + "_and", fixture.compute(OperationKind::kReduceAnd, Fixture::type(1), {value}));
        graph.bindOutputPort(item.name + "_or", fixture.compute(OperationKind::kReduceOr, Fixture::type(1), {value}));
        graph.bindOutputPort(item.name + "_xor", fixture.compute(OperationKind::kReduceXor, Fixture::type(1), {value}));
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto reference = fixture.design.clone();
    fixture.split(); fixture.partition();
    std::vector<std::map<std::string, uint64_t>> inputs;
    for (const auto value : {uint64_t{0}, UINT64_MAX, UINT64_C(0x8ac739e12d654b0f), UINT64_C(0x13579bdf2468ace0)})
        for (const unsigned selected : {0u, 1u, 0u}) {
            std::map<std::string, uint64_t> values{{"choose", selected}};
            for (const auto& item : cases)
                values.emplace("data_" + item.name, item.type.width < 64 ? value & ((uint64_t{1} << item.type.width) - 1) : value);
            inputs.push_back(std::move(values));
        }
    for (const auto budget : {16, 1024}) {
        const auto label = "packed-array-constants-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        std::string generated;
        for (const auto& entry : std::filesystem::directory_iterator(artifact(label) / "model"))
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") generated += readFile(entry.path());
        for (const auto value : constants)
            require(generated.find(mappedPackedPrefix(artifact(label) / "model", value)) == std::string::npos,
                "A shaped constant was repacked from its initialized array");
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label
             << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
}

void testStaticArrayWindows() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto low = fixture.input("low", 64), middle = fixture.input("middle", 64);
    const auto high = fixture.input("high", 64), top = fixture.input("top", 64);
    const auto slice = [&](grh::ValueId source, int64_t first, grh::LogicalType type) {
        const auto value = fixture.compute(OperationKind::kSliceStatic, type, {source});
        graph.setAttr(graph.valueDef(value), "sliceStart", first);
        graph.setAttr(graph.valueDef(value), "sliceEnd", first + type.width - 1);
        return value;
    };
    const auto flat192 = fixture.compute(OperationKind::kConcat, Fixture::type(192), {high, middle, low});
    const auto flat256 = fixture.compute(OperationKind::kConcat, Fixture::type(256), {top, high, middle, low});
    const auto flat144 = slice(flat192, 0, Fixture::type(144));
    const auto flat96 = slice(flat192, 0, Fixture::type(96));
    auto signed3 = Fixture::type(144, {2, 3, 8}); signed3.isSigned = true;
    auto signed6 = Fixture::type(96, {2, 8}); signed6.isSigned = true;
    auto signed64 = Fixture::type(256, {2, 2}); signed64.isSigned = true;
    const auto saved = [&](std::string name, const grh::LogicalType& type, grh::ValueId flat) {
        const auto value = fixture.compute(OperationKind::kAssign, type, {flat});
        graph.setValueSymbol(value, graph.internSymbol(name));
        graph.addDeclaredSymbol(graph.valueSymbol(value));
        return value;
    };
    const auto flags = saved("saved_static_flags", Fixture::type(192, {2, 3, 32}), flat192);
    const auto flagsFallback = saved("saved_static_flags_fallback", Fixture::type(192, {2, 3, 32}), flat192);
    const auto three = saved("saved_static_three", signed3, flat144);
    const auto six = saved("saved_static_six", signed6, flat96);
    const auto words = saved("saved_static_words", signed64, flat256);
    const auto output = [&](std::string name, grh::ValueId source, int64_t first, grh::LogicalType type) {
        graph.bindOutputPort(name, slice(source, first, type));
    };
    output("bool_first", flags, 0, Fixture::type(1));
    output("bool_last", flags, 191, Fixture::type(1));
    output("bool_cross", flags, 17, Fixture::type(13));
    output("bool_max_window", flags, 31, Fixture::type(32));
    output("bool_end", flags, 167, Fixture::type(25));
    output("bool_wide_fallback", flagsFallback, 57, Fixture::type(80));
    output("bool_end_fallback", flagsFallback, 117, Fixture::type(75));
    output("three_cross", three, 2, Fixture::type(8));
    output("three_middle", three, 14, Fixture::type(33));
    output("three_wide", three, 5, Fixture::type(79));
    auto signed9 = Fixture::type(9); signed9.isSigned = true;
    output("three_signed", three, 1, signed9);
    output("three_shaped", three, 126, Fixture::type(18, {2, 3}));
    // The ROB-shaped case reads eight or nine 6-bit leaves, not all sixteen.
    output("six_aligned", six, 0, Fixture::type(48));
    output("six_cross", six, 1, Fixture::type(48));
    output("six_wide", six, 29, Fixture::type(65));
    output("six_end", six, 79, Fixture::type(17));
    output("words_aligned", words, 64, Fixture::type(64));
    output("words_cross", words, 60, Fixture::type(80));
    output("words_middle", words, 127, Fixture::type(65));
    output("words_end", words, 193, Fixture::type(63));
    output("words_wide", words, 63, Fixture::type(193));
    auto signedResult64 = Fixture::type(64); signedResult64.isSigned = true;
    output("words_signed", words, 63, signedResult64);
    const auto localFlags = fixture.compute(OperationKind::kAssign, Fixture::type(192, {2, 3, 32}), {flat192});
    const auto localSix = fixture.compute(OperationKind::kAssign, signed6, {flat96});
    output("local_bool_cross", localFlags, 31, Fixture::type(9));
    output("local_six_cross", localSix, 5, Fixture::type(48));
    const auto arrayState = fixture.state("static_snapshot_array", 96, {2, 8});
    grh::setStateLogicalType(graph, arrayState, signed6);
    const auto capture48 = fixture.state("static_capture48", 48);
    const auto capture65 = fixture.state("static_capture65", 65);
    const auto stateSample = fixture.read(arrayState);
    graph.setValueSymbol(stateSample, graph.internSymbol("saved_static_state"));
    graph.addDeclaredSymbol(graph.valueSymbol(stateSample));
    fixture.write(OperationKind::kRegisterWritePort, arrayState, {enable, six, clock});
    fixture.write(OperationKind::kRegisterWritePort, capture48,
        {enable, slice(stateSample, 1, Fixture::type(48)), clock});
    fixture.write(OperationKind::kRegisterWritePort, capture65,
        {enable, slice(stateSample, 29, Fixture::type(65)), clock});
    graph.bindOutputPort("captured48", fixture.read(capture48));
    graph.bindOutputPort("captured65", fixture.read(capture65));
    output("current48", stateSample, 1, Fixture::type(48));
    output("current65", stateSample, 29, Fixture::type(65));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=4096"});
    fixture.partition();
    std::vector<std::map<std::string, uint64_t>> inputs;
    uint64_t random = UINT64_C(0x967a3c5921e4bf08);
    for (unsigned step = 0; step < 36; ++step) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        const auto first = step < 2 ? uint64_t{0} : step < 4 ? UINT64_MAX :
            step < 12 ? uint64_t{1} << (std::array<unsigned, 8>{0, 1, 2, 5, 31, 32, 62, 63}[step - 4]) : random;
        inputs.push_back({{"clock", step & 1}, {"enable", step % 7 != 0}, {"low", first},
            {"middle", step < 4 ? first : (random << 17) ^ (random >> 11)},
            {"high", step < 4 ? first : ~random}, {"top", step < 4 ? first : random ^ (random >> 23)}});
    }
    for (const auto budget : {16, 1024}) {
        const auto label = "static-array-windows-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        std::string generated;
        for (const auto& entry : std::filesystem::directory_iterator(artifact(label) / "model"))
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") generated += readFile(entry.path());
        for (const auto value : {flags, three, six, words, localFlags, localSix, stateSample})
            require(generated.find(mappedPackedPrefix(artifact(label) / "model", value)) == std::string::npos,
                "Static window extraction packed its entire source array");
        require(generated.find("nonSinkBoundaryStore." + mappedValue(artifact(label) / "model", stateSample)) != std::string::npos,
            "Static window regression did not retain its saved state snapshot");
        require(generated.find(mappedPackedPrefix(artifact(label) / "model", flagsFallback)) != std::string::npos,
            "Large static window lost its compact packing fallback");
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label
             << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
}

void testDynamicArrayBits() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("clock"), enable = fixture.input("enable");
    const auto flags = fixture.input("flags", 64), bits3 = fixture.input("bits3", 36);
    const auto index = fixture.input("index", 10), signedIndex = fixture.input("signed_index", 10);
    auto signedIndexType = Fixture::type(10); signedIndexType.isSigned = true;
    graph.setValueLogicalType(signedIndex, signedIndexType);
    const auto high = fixture.input("index_high", 64), low = fixture.input("index_low", 64);
    const auto wideIndex = fixture.compute(OperationKind::kConcat, Fixture::type(128), {high, low});
    auto signedWideType = Fixture::type(128); signedWideType.isSigned = true;
    const auto signedWideIndex = fixture.compute(OperationKind::kAssign, signedWideType, {wideIndex});
    const auto repeated = fixture.compute(OperationKind::kReplicate, Fixture::type(384), {flags});
    graph.setAttr(graph.valueDef(repeated), "rep", int64_t{6});
    const auto packed = fixture.compute(OperationKind::kSliceStatic, Fixture::type(352), {repeated});
    graph.setAttr(graph.valueDef(packed), "sliceStart", int64_t{0});
    graph.setAttr(graph.valueDef(packed), "sliceEnd", int64_t{351});
    const auto controls = fixture.compute(OperationKind::kAssign, Fixture::type(352, {2, 176}), {packed});
    auto signed3 = Fixture::type(36, {2, 2, 3}); signed3.isSigned = true;
    const auto leaves3 = fixture.compute(OperationKind::kAssign, signed3, {bits3});
    const auto savedControls = fixture.compute(OperationKind::kAssign, Fixture::type(352, {2, 176}), {controls});
    const auto savedLeaves3 = fixture.compute(OperationKind::kAssign, signed3, {leaves3});
    graph.setValueSymbol(savedControls, graph.internSymbol("saved_dynamic_controls"));
    graph.setValueSymbol(savedLeaves3, graph.internSymbol("saved_dynamic_leaves"));
    graph.addDeclaredSymbol(graph.valueSymbol(savedControls));
    graph.addDeclaredSymbol(graph.valueSymbol(savedLeaves3));
    const auto bit = [&](grh::ValueId source, grh::ValueId offset) {
        return fixture.compute(OperationKind::kSliceDynamic, Fixture::type(1), {source, offset});
    };
    const auto localControls = fixture.compute(OperationKind::kAssign, Fixture::type(352, {2, 176}), {packed});
    const auto localLeaves3 = fixture.compute(OperationKind::kAssign, signed3, {bits3});
    graph.bindOutputPort("local_bool", bit(localControls, index));
    graph.bindOutputPort("saved_bool", bit(savedControls, index));
    graph.bindOutputPort("signed_index_bool", bit(controls, signedIndex));
    graph.bindOutputPort("wide_index_bool", bit(controls, wideIndex));
    graph.bindOutputPort("signed_wide_index_bool", bit(savedControls, signedWideIndex));
    graph.bindOutputPort("local_three", bit(localLeaves3, index));
    graph.bindOutputPort("saved_three", bit(savedLeaves3, signedIndex));
    graph.bindOutputPort("wide_index_three", bit(savedLeaves3, wideIndex));
    // More than one result bit retains the existing cross-leaf path.
    const auto fallback = fixture.compute(OperationKind::kAssign, signed3, {bits3});
    graph.bindOutputPort("cross_leaf", fixture.compute(OperationKind::kSliceDynamic, Fixture::type(4), {fallback, index}));
    const auto arrayState = fixture.state("snapshot_array", 352, {2, 176});
    const auto capturedState = fixture.state("snapshot_bit", 1);
    const auto stateSample = fixture.read(arrayState);
    graph.setValueSymbol(stateSample, graph.internSymbol("saved_dynamic_state"));
    graph.addDeclaredSymbol(graph.valueSymbol(stateSample));
    fixture.write(OperationKind::kRegisterWritePort, arrayState, {enable, controls, clock});
    fixture.write(OperationKind::kRegisterWritePort, capturedState, {enable, bit(stateSample, index), clock});
    graph.bindOutputPort("captured", fixture.read(capturedState));
    graph.bindOutputPort("state_bit", bit(stateSample, index));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto reference = fixture.design.clone();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=4096"});
    fixture.partition();
    std::vector<std::map<std::string, uint64_t>> inputs;
    const std::array<unsigned, 23> offsets{0, 1, 2, 3, 31, 32, 34, 35, 36, 63, 64, 127, 175, 176, 255, 350, 351, 352, 353, 383, 384, 511, 1023};
    uint64_t random = UINT64_C(0x98e37bac470156d2);
    for (unsigned step = 0; step < offsets.size() * 2; ++step) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        const auto offset = offsets[step / 2];
        inputs.push_back({{"clock", step & 1}, {"enable", step % 7 != 0},
            {"flags", step < 2 ? UINT64_MAX : random}, {"bits3", random & UINT64_C(0xfffffffff)},
            {"index", offset}, {"signed_index", step % 5 == 0 ? 1023 : offset},
            {"index_high", step % 4 == 0 ? 1 : step % 4 == 1 ? UINT64_C(1) << 63 : 0},
            {"index_low", step % 6 == 0 ? UINT64_MAX : offset}});
    }
    for (const auto budget : {16, 1024}) {
        const auto label = "dynamic-array-bits-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        std::string generated;
        for (const auto& entry : std::filesystem::directory_iterator(artifact(label) / "model"))
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") generated += readFile(entry.path());
        for (const auto value : {controls, savedControls, leaves3, savedLeaves3, stateSample, localControls, localLeaves3})
            require(generated.find(mappedPackedPrefix(artifact(label) / "model", value)) == std::string::npos,
                "Dynamic bit extraction packed its entire source array");
        for (const auto value : {localControls, localLeaves3})
            require(generated.find("Store." + mappedValue(artifact(label) / "model", value)) == std::string::npos,
                "Dynamic bit regression did not retain its private local array");
        require(generated.find("nonSinkBoundaryStore." + mappedValue(artifact(label) / "model", stateSample)) != std::string::npos,
            "Dynamic bit regression did not retain its saved state snapshot");
        require(generated.find(mappedPackedPrefix(artifact(label) / "model", fallback)) != std::string::npos,
            "Multi-bit dynamic extraction lost its cross-leaf fallback");
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label
             << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
}

void testMaskedViewBits() {
    Fixture fixture;
    const auto flags = fixture.input("flags", 64), bits3 = fixture.input("bits3", 36);
    const auto bits5 = fixture.input("bits5", 60), bits56 = fixture.input("bits56", 56);
    const auto slice = [&](grh::ValueId source, int64_t first, const grh::LogicalType& type) {
        const auto result = fixture.compute(OperationKind::kSliceStatic, type, {source});
        fixture.graph.setAttr(fixture.graph.valueDef(result), "sliceStart", first);
        fixture.graph.setAttr(fixture.graph.valueDef(result), "sliceEnd", first + type.width - 1);
        return result;
    };
    const auto repeatedFlags = fixture.compute(OperationKind::kReplicate, Fixture::type(384), {flags});
    fixture.graph.setAttr(fixture.graph.valueDef(repeatedFlags), "rep", int64_t{6});
    const auto scalarControl = slice(repeatedFlags, 0, Fixture::type(352));
    const auto controls = fixture.compute(OperationKind::kAssign, Fixture::type(352, {2, 176}), {scalarControl});
    auto signed3 = Fixture::type(36, {2, 2, 3}); signed3.isSigned = true;
    auto signed56 = Fixture::type(336, {2, 3}); signed56.isSigned = true;
    const auto data3 = fixture.compute(OperationKind::kAssign, signed3, {bits3});
    const auto data5 = fixture.compute(OperationKind::kAssign, Fixture::type(60, {3, 4}), {bits5});
    std::array<grh::ValueId, 6> leaves;
    for (unsigned i = 0; i < leaves.size(); ++i) {
        const auto delta = fixture.constant(56, "56'd" + std::to_string(UINT64_C(0x125a53e7951b) * i));
        leaves[i] = fixture.compute(OperationKind::kXor, Fixture::type(56), {bits56, delta});
    }
    const auto flat56 = fixture.compute(OperationKind::kConcat, Fixture::type(336),
        {leaves[5], leaves[4], leaves[3], leaves[2], leaves[1], leaves[0]});
    const auto data56 = fixture.compute(OperationKind::kAssign, signed56, {flat56});
    // Keep the typed array views intact and append dialect operations only
    // after conversion has checked the base GRH contract.
    run(fixture.design, "grh-to-grhsim", fixture.session);
    const auto masked = [&](std::string name, const grh::LogicalType& type, std::vector<int64_t> views) {
        const auto result = fixture.compute(sim::kMaskedOr, type,
            {controls, scalarControl, data3, data5, data56, bits56});
        fixture.graph.setAttr(fixture.graph.valueDef(result), "views", std::move(views));
        fixture.graph.bindOutputPort(name, result);
    };
    masked("selectedBits", Fixture::type(1), {
        0, 0, 2, 2, 0, 63, 2, 3, 0, 64, 3, 4, 0, 175, 3, 59,
        0, 350, 4, 335, 0, 351, 4, 55, 1, 351, 5, 55});
    masked("selected3", Fixture::type(3), {
        0, 63, 2, 3, 0, 64, 2, 33, 0, 128, 3, 6,
        0, 175, 4, 79, 0, 351, 4, 333, 1, 350, 5, 53});
    // Include both one-leaf and cross-leaf static windows.
    masked("selectedArray", Fixture::type(6, {2, 3}), {
        0, 127, 4, 57, 0, 128, 3, 4, 0, 255, 2, 2, 1, 351, 5, 17});
    masked("selectedWideArray", Fixture::type(48, {2, 3}), {
        0, 1, 4, 8, 0, 65, 4, 64, 0, 129, 4, 119, 0, 351, 4, 40});
    masked("selected56", Fixture::type(56), {
        0, 63, 4, 56, 0, 175, 4, 280, 1, 351, 5, 0});
    fixture.graph.bindOutputPort("slice3", slice(data3, 33, Fixture::type(3)));
    fixture.graph.bindOutputPort("slice5", slice(data5, 55, Fixture::type(5)));
    fixture.graph.bindOutputPort("sliceArray", slice(data56, 58, Fixture::type(6, {2, 3})));
    fixture.graph.bindOutputPort("sliceLast", slice(data56, 331, Fixture::type(5)));
    fixture.graph.bindOutputPort("sliceCross", slice(data3, 2, Fixture::type(4)));
    const auto reference = fixture.design.clone();
    fixture.split(); fixture.partition();
    std::vector<std::map<std::string, uint64_t>> inputs;
    uint64_t random = UINT64_C(0xa45c3e2970b681df);
    for (unsigned step = 0; step < 32; ++step) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        const auto control = step == 0 ? uint64_t{0} : step == 1 ? UINT64_MAX :
            step < 10 ? uint64_t{1} << (std::array<unsigned, 8>{0, 1, 31, 46, 47, 63, 62, 32}[step - 2]) : random;
        inputs.push_back({{"flags", control}, {"bits3", (random ^ (random >> 21)) & UINT64_C(0xfffffffff)},
            {"bits5", (random ^ (random << 11)) & UINT64_C(0xfffffffffffffff)},
            {"bits56", (random ^ (random >> 9)) & UINT64_C(0xffffffffffffff)}});
    }
    for (const auto budget : {16, 1024}) {
        const auto label = "masked-view-bits-b" + std::to_string(budget);
        writeCase(fixture, reference, inputs, label, budget, 4096);
        std::string generated;
        for (const auto& entry : std::filesystem::directory_iterator(artifact(label) / "model"))
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") generated += readFile(entry.path());
        require(generated.find(mappedPackedPrefix(artifact(label) / "model", controls)) == std::string::npos,
                "Static masked views packed the entire 352-bit control array");
        require(generated.find(mappedPackedPrefix(artifact(label) / "model", data3)) == std::string::npos,
                "Cross-leaf static extraction packed its entire source array");
        std::ofstream make(artifact("Makefile"), std::ios::app);
        make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
    }
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
    const auto scalarSource = fixture.input("scalar_source", 48);
    const auto localShape = fixture.compute(OperationKind::kAssign, Fixture::type(48, {2, 4}), {scalarSource});
    const auto localFlat = fixture.compute(OperationKind::kAssign, Fixture::type(48), {localShape});
    const auto localView = fixture.compute(OperationKind::kAssign, Fixture::type(48, {4, 2}), {localFlat});
    fixture.graph.bindOutputPort("local_view", localView);
    prepare += "model.scalar_source=UINT64_C(0x123456789abc)^step;\n";
    verify += "for(unsigned i=0;i<8;++i) assert(model.local_view[i/2][i%2]==((model.scalar_source>>(i*6))&63));\n";
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
        require(manifest.find("\"array_view_elisions\": 18") != std::string::npos,
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

namespace {
void testSharedPayloadEnableSets() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clockA = fixture.input("sets_clock_a"), clockB = fixture.input("sets_clock_b");
    const auto controls = fixture.input("sets_controls", 32), observedData = fixture.input("sets_observed_data", 8);
    const auto next = fixture.input("sets_next", 12), priorityA = fixture.input("sets_priority_a");
    const auto priorityB = fixture.input("sets_priority_b"), priorityData = fixture.input("sets_priority_data", 12);
    const auto data = graph.createValue(graph.internSymbol("sets_data"), Fixture::type(16, {2}));
    graph.bindInputPort("sets_data", data);
    const auto one = fixture.constant(1, "1'b1"), zeroIndex = fixture.constant(3, "3'd0");
    const auto seven = fixture.constant(8, "8'd7"), maskA = fixture.constant(12, "12'h135");
    const auto maskC = fixture.constant(12, "12'h276"), priorityMask = fixture.constant(12, "12'hff0");
    const auto source = fixture.state("sets_source", 12);
    fixture.write(OperationKind::kRegisterWritePort, source, {one, next, clockA});
    graph.bindOutputPort("sets_source_out", fixture.read(source));
    const auto tag = [&](grh::ValueId value, const std::string& role) {
        graph.setAttr(graph.valueDef(value), "test.sharedRole", role);
        return value;
    };
    std::vector<std::pair<grh::OperationId, std::string>> sharedCones;
    for (const bool large : {false, true}) {
        const std::string prefix = large ? "large" : "small";
        // Construct an actually shared DAG. Separate equivalent shaped roots
        // are not merged by common-expr, and cannot use a union-demand oracle.
        const auto root = tag(fixture.compute(OperationKind::kXor, Fixture::type(16, {2}),
                                              {data, fixture.read(source)}), prefix + "/root");
        const auto slice = [&](unsigned offset, const std::string& role) {
            const auto value = tag(fixture.compute(OperationKind::kSliceStatic, Fixture::type(12), {root}), prefix + '/' + role);
            graph.setAttr(graph.valueDef(value), "sliceStart", int64_t{offset});
            graph.setAttr(graph.valueDef(value), "sliceEnd", int64_t{offset + 11});
            return value;
        };
        const auto ab = slice(0, "ab"), bc = slice(4, "bc");
        for (const auto value : {root, ab, bc})
            sharedCones.emplace_back(graph.valueDef(value), large ? "large-sink" : "small-sink");
        std::array<grh::ValueId,3> payloads;
        for (unsigned writer = 0; writer < 4; ++writer) {
            const auto width = writer == 3 ? 8 : 12;
            const auto state = fixture.state("sets_" + prefix + std::to_string(writer), large ? width * 8 : width,
                                             large ? std::vector<int64_t>{8} : std::vector<int64_t>{});
            const auto index = fixture.input("sets_" + prefix + "_index" + std::to_string(writer), 5);
            const auto enable = tag(fixture.compute(OperationKind::kSliceDynamic, Fixture::type(1), {controls, index}),
                                    prefix + "/enable" + std::to_string(writer));
            grh::ValueId payload;
            if (writer < 3) {
                if (writer == 0) payload = tag(fixture.compute(OperationKind::kXor, Fixture::type(12),
                                                               {ab, maskA}), prefix + "/a");
                else if (writer == 1) payload = tag(fixture.compute(OperationKind::kXor, Fixture::type(12),
                                                                    {ab, bc}), prefix + "/b");
                else payload = tag(fixture.compute(OperationKind::kXor, Fixture::type(12),
                                                    {bc, maskC}), prefix + "/c");
                payloads[writer] = payload;
            } else {
                payload = tag(fixture.compute(OperationKind::kAdd, Fixture::type(8), {observedData, seven}), prefix + "/observed");
                graph.setValueSymbol(payload, graph.internSymbol("sets_" + prefix + "_observed"));
                graph.addDeclaredSymbol(graph.valueSymbol(payload));
            }
            fixture.write(large ? OperationKind::kRegisterFillPort : OperationKind::kRegisterWritePort,
                          state, {enable, payload, clockA});
            graph.bindOutputPort("sets_" + prefix + std::to_string(writer), fixture.read(state));
        }
        require(graph.opOperands(graph.valueDef(ab))[0]==root && graph.opOperands(graph.valueDef(bc))[0]==root &&
                    graph.opOperands(graph.valueDef(payloads[0]))[0]==ab &&
                    graph.opOperands(graph.valueDef(payloads[1]))[0]==ab &&
                    graph.opOperands(graph.valueDef(payloads[1]))[1]==bc &&
                    graph.opOperands(graph.valueDef(payloads[2]))[0]==bc,
                "Shared enable-set fixture must share producers across A, B and C");
    }
    const auto priority = fixture.state("sets_priority", 12);
    fixture.write(OperationKind::kRegisterWritePort, priority, {priorityA, priorityData, clockB});
    fixture.write(OperationKind::kRegisterWritePort, priority, {priorityB, next, clockB});
    fixture.write(OperationKind::kRegisterWriteMaskPort, priority, {priorityA, priorityData, priorityMask, clockB});
    graph.bindOutputPort("sets_priority_out", fixture.read(priority));
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=8"});
    // The generic split keeps multi-writer sharing in nonsink. Exercise the
    // emitter's legal sink-local shared DAG explicitly, before scheduling.
    for (const auto& [op, category] : sharedCones)
        graph.setAttr(op, sim::kPartition, Owner{"general", category});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    std::map<std::string, std::string> siteNames;
    std::map<std::string, Owner> owners;
    for (const auto op : graph.operations()) {
        const auto role = sim::attribute<std::string>(graph, op, "test.sharedRole");
        if (!role) continue;
        siteNames[*role] = std::string(graph.symbolText(graph.operationSymbol(op)));
        const auto path = owner(graph, op);
        require(path.size() == 3 && path[0] == "general" && path[1] == (role->starts_with("large") ? "large-sink" : "small-sink"),
                "Shared payload fixture lost a local enable, shared payload or observed result");
        const auto category = role->substr(0, role->find('/'));
        const auto [found, inserted] = owners.emplace(category, path);
        require(inserted || found->second == path, "Shared payload fixture split a subset across writer groups");
    }
    const std::string support = R"CPP(
#include "fstapi.h"
#include <map>
struct SetsWave {
    std::map<fstHandle,std::string> names;
    std::map<std::string,std::map<std::uint64_t,unsigned>> values;
    static void change(void *context, std::uint64_t time, fstHandle handle, const unsigned char *bits) {
        auto& self = *static_cast<SetsWave*>(context);
        const auto& name = self.names.at(handle);
        if (name == "sets_small_observed" || name == "sets_large_observed")
            self.values[name][time] = std::stoul(reinterpret_cast<const char*>(bits), nullptr, 2);
    }
    unsigned at(const std::string& name, std::uint64_t time) const {
        const auto& series = values.at(name); const auto entry = series.upper_bound(time);
        assert(entry != series.begin()); return std::prev(entry)->second;
    }
};
)CPP";
    const std::string body = R"CPP(
const std::array<unsigned,12> masks{0,2,1,4,3,6,5,7,0,8,2,0};
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    const auto path = "sets-" + std::to_string(epoch) + ".fst";
    model.configure_waveform(true, path.c_str()); model.init();
    std::array<unsigned,4> expected{}; unsigned source = 0, priority = 0, observed = 0;
    std::array<unsigned,11> expectedCounts{}; std::array<unsigned,36> observations{};
    for (unsigned step = 0; step < masks.size(); ++step) {
        const auto mask = masks[step]; const unsigned data = (0xabcd + step * 197 + epoch * 313) & 65535;
        model.sets_clock_a = false; model.sets_clock_b = false; model.sets_controls = mask;
        model.sets_data[0] = data & 255; model.sets_data[1] = data >> 8;
        model.sets_observed_data = 17 * step + epoch; model.sets_next = (step * 53 + epoch * 17 + 11) & 4095;
        model.sets_priority_a = step % 3 != 0; model.sets_priority_b = step % 4 != 0;
        model.sets_priority_data = (data >> 3) & 4095;
        model.sets_small_index0 = model.sets_large_index0 = 0;
        model.sets_small_index1 = model.sets_large_index1 = 1;
        model.sets_small_index2 = model.sets_large_index2 = 2;
        model.sets_small_index3 = model.sets_large_index3 = 3;
        model.eval(); observations[step * 3] = observed;
        const bool edge = step % 3 != 2; model.sets_clock_a = edge; model.sets_clock_b = true;
        if (edge) {
            const unsigned root = data ^ source, ab = root & 4095, bc = root >> 4;
            if (mask & 1) expected[0] = ab ^ 0x135;
            if (mask & 2) expected[1] = ab ^ bc;
            if (mask & 4) expected[2] = bc ^ 0x276;
            observed = (model.sets_observed_data + 7) & 255;
            if (mask & 8) expected[3] = observed;
            source = model.sets_next;
            expectedCounts[0] += (mask & 7) != 0; expectedCounts[1] += (mask & 3) != 0;
            expectedCounts[2] += (mask & 6) != 0;
            for (unsigned bit = 0; bit < 3; ++bit) expectedCounts[3 + bit] += (mask >> bit) & 1;
            for (unsigned site = 6; site < 11; ++site) ++expectedCounts[site];
        }
        if (model.sets_priority_a) priority = model.sets_priority_data;
        if (model.sets_priority_b) priority = model.sets_next;
        if (model.sets_priority_a) priority = (priority & 15) | (model.sets_priority_data & 0xff0);
        model.eval(); observations[step * 3 + 1] = observed;
        assert(model.sets_source_out == source && model.sets_priority_out == priority);
        assert(model.sets_small0 == expected[0] && model.sets_small1 == expected[1]);
        assert(model.sets_small2 == expected[2] && model.sets_small3 == expected[3]);
        for (unsigned row = 0; row < 8; ++row) {
            assert(model.sets_large0[row] == expected[0] && model.sets_large1[row] == expected[1]);
            assert(model.sets_large2[row] == expected[2] && model.sets_large3[row] == expected[3]);
        }
        model.eval(); observations[step * 3 + 2] = observed;
    }
    auto *counts = std::fopen("shared-enable-counts.tsv", "w+"); assert(counts);
    model.dump_execution_counts(counts); std::rewind(counts); char line[256];
    assert(std::fgets(line, sizeof(line), counts) && std::fgets(line, sizeof(line), counts));
    std::map<unsigned,unsigned long long> sites;
    while (std::fgets(line, sizeof(line), counts)) {
        unsigned site; unsigned long long count;
        if (std::sscanf(line, "%u\t%llu", &site, &count) == 2) sites[site] = count;
    }
    std::fclose(counts);
    for (unsigned category = 0; category < 2; ++category) for (unsigned site = 0; site < 11; ++site)
        assert(sites.at(sharedEnableSites[category * 11 + site]) == expectedCounts[site]);
    model.configure_waveform(false); auto *reader = fstReaderOpen(path.c_str()); assert(reader); SetsWave wave;
    while (auto *entry = fstReaderIterateHier(reader)) if (entry->htyp == FST_HT_VAR)
        wave.names.emplace(entry->u.var.handle, entry->u.var.name);
    fstReaderSetFacProcessMaskAll(reader); assert(fstReaderIterBlocks2(reader, SetsWave::change, nullptr, &wave, nullptr));
    for (unsigned time = 0; time < observations.size(); ++time) {
        assert(wave.at("sets_small_observed", time) == observations[time]);
        assert(wave.at("sets_large_observed", time) == observations[time]);
    }
    fstReaderClose(reader);
}
)CPP";
    for (const auto budget : {16, 1024}) for (const std::string bitmap : {"on", "off"}) {
        const auto label = "shared-payload-enables-" + bitmap + "-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, true,
                   {{"execution-counters", "on"}, {"mem-enable-bitmap", bitmap}, {"mem-write-activate", "on-change"}});
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        const std::regex allSites(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
        std::map<std::string,unsigned> sites; unsigned index = 0;
        for (std::sregex_iterator it(manifest.begin(), manifest.end(), allSites), end; it != end; ++it, ++index)
            if ((*it)[2] == "operation") sites.emplace((*it)[1], index);
        std::string declaration = "static const std::array<unsigned,22> sharedEnableSites{";
        for (const std::string prefix : {"small", "large"})
            for (const std::string role : {"root", "ab", "bc", "a", "b", "c", "enable0", "enable1", "enable2", "enable3", "observed"}) {
                const auto& name = siteNames.at(prefix + '/' + role);
                require(sites.contains(name), "Shared enable-set operation has no execution counter");
                declaration += std::to_string(sites.at(name)) + ',';
            }
        declaration += "};\n";
        const auto driverPath = artifact(label) / "driver.cpp";
        auto driver = readFile(driverPath); driver.insert(driver.find("int main()"), declaration);
        std::ofstream(driverPath) << driver;
    }
}

void testSnapshotIdentityViews() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("identity_clock"), enable = fixture.input("identity_enable"), select = fixture.input("identity_select");
    const auto next = graph.createValue(graph.internSymbol("identity_next"), Fixture::type(512, {8}));
    graph.bindInputPort("identity_next", next);
    const auto source = fixture.state("identity_source", 512, {8});
    const auto saved = fixture.read(source); graph.bindOutputPort("identity_source_out", saved);
    fixture.write(OperationKind::kRegisterWritePort, source, {enable, next, clock});
    const auto assemble = [&](bool reverse, std::string role) {
        std::vector<grh::ValueId> parts;
        for (int64_t i = 0; i < 8; ++i) {
            const auto part = fixture.compute(OperationKind::kSliceStatic, Fixture::type(64), {saved});
            graph.setAttr(graph.valueDef(part), "sliceStart", i * 64);
            graph.setAttr(graph.valueDef(part), "sliceEnd", i * 64 + 63); parts.push_back(part);
        }
        const auto op = graph.createOperation(OperationKind::kArrayCreate, graph.internSymbol("identity_create_" + role));
        for (unsigned i = 0; i < 8; ++i) graph.addOperand(op, parts[reverse ? 7 - i : i]);
        const auto value = graph.createValue(Fixture::type(512, {8})); graph.addResult(op, value); return value;
    };
    const auto identity = assemble(false, "identity"), reordered = assemble(true, "reordered");
    const auto observed = assemble(false, "observed"); graph.bindOutputPort("identity_observed", observed);
    const auto flat = fixture.compute(OperationKind::kAssign, Fixture::type(512), {identity});
    const auto shaped = fixture.compute(OperationKind::kAssign, Fixture::type(512, {8}), {flat});
    const auto payload = fixture.compute(OperationKind::kMux, Fixture::type(512), {select, shaped, fixture.constant(512, "0")});
    const auto target = fixture.state("identity_target", 512, {8}), reverseTarget = fixture.state("identity_reverse", 512, {8});
    fixture.write(OperationKind::kRegisterWritePort, target, {enable, payload, clock});
    fixture.write(OperationKind::kRegisterWritePort, reverseTarget, {enable, reordered, clock});
    graph.bindOutputPort("identity_target_out", fixture.read(target));
    graph.bindOutputPort("identity_reverse_out", fixture.read(reverseTarget));
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split(); fixture.partition();
    for (const auto budget : {16, 1024}) {
        const auto label = "snapshot-identity-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "", R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); std::array<std::uint64_t,8> saved{}, target{}, reversed{};
    for(unsigned step=0;step<12;++step) {
        model.identity_clock=0; model.identity_enable=step%4!=2; model.identity_select=step%3!=0;
        for(unsigned i=0;i<8;++i) model.identity_next[i]=UINT64_C(0x123456789abcdef0)^(epoch*101+step*37+i*19);
        model.eval(); model.identity_clock=1; model.eval();
        if(model.identity_enable) {
            for(unsigned i=0;i<8;++i) { target[i]=model.identity_select?saved[i]:0; reversed[i]=saved[7-i]; }
            saved=model.identity_next;
        }
        assert(model.identity_source_out==saved && model.identity_observed==saved);
        assert(model.identity_target_out==target && model.identity_reverse_out==reversed);
        model.eval(); assert(model.identity_target_out==target && model.identity_reverse_out==reversed);
    }
}
)CPP", true, {{"execution-counters", "on"}});
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        const auto elided = manifest.substr(manifest.find("\"elided_operations\""));
        bool hit = false, observedSite = false;
        for (const auto op : graph.operations()) {
            const auto name = std::string(graph.getOperation(op).symbolText());
            if (name.find("identity_create_") == std::string::npos) continue;
            const bool removed = elided.find("\"" + name + "\"") != std::string::npos;
            if (name.find("identity_create_identity") != std::string::npos) hit |= removed;
            if (name.find("identity_create_reordered") != std::string::npos) require(!removed, "Reordered ArrayCreate was identity-elided");
            if (name.find("identity_create_observed") != std::string::npos && !removed)
                observedSite |= manifest.find("\"operation\":\"" + name + "\"") != std::string::npos;
        }
        require(hit && observedSite, "Snapshot identity did not elide or observed ArrayCreate lost its counter");
    }
}

void testLeafAssemblyViews() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("leaf_clock"), enableA = fixture.input("leaf_enable_a");
    const auto enableB = fixture.input("leaf_enable_b"), index = fixture.input("leaf_index", 8);
    const auto next = graph.createValue(graph.internSymbol("leaf_next"), Fixture::type(1344, {224, 3}));
    graph.bindInputPort("leaf_next", next);
    const auto source = fixture.state("leaf_source", 1344, {224, 3});
    fixture.write(OperationKind::kRegisterWritePort, source, {enableA, next, clock});
    graph.bindOutputPort("leaf_source_out", fixture.read(source));
    std::map<std::string, std::vector<std::string>> sites;
    const auto tag = [&](grh::ValueId value, const std::string& role) {
        const auto name = "leaf_" + role;
        graph.setOpSymbol(graph.valueDef(value), graph.internSymbol(name)); sites[role].push_back(name); return value;
    };
    // sn4296: 224 rows of three 2-bit leaves, with one selected row
    // repeated 32 times above the complete old-state snapshot.
    const auto assemble = [&](const std::string& role) {
        const auto row = fixture.read(source, {index});
        const auto flat = tag(fixture.compute(OperationKind::kAssign, Fixture::type(6), {row}), role + "_row");
        const auto repeat = tag(fixture.compute(OperationKind::kReplicate, Fixture::type(192), {flat}), role + "_repeat");
        graph.setAttr(graph.valueDef(repeat), "rep", int64_t{32});
        const auto whole = tag(fixture.read(source), role + "_snapshot");
        const auto concat = tag(fixture.compute(OperationKind::kConcat, Fixture::type(1536), {repeat, whole}), role + "_concat");
        return tag(fixture.compute(OperationKind::kAssign, Fixture::type(1536, {256, 3}), {concat}), role + "_result");
    };
    graph.bindOutputPort("leaf_live", assemble("live"));
    std::vector<grh::OperationId> writers;
    for (unsigned i = 0; i < 3; ++i) {
        const auto target = fixture.state("leaf_target" + std::to_string(i), 1536, {256, 3});
        const auto payload = assemble("target" + std::to_string(i));
        writers.push_back(fixture.write(OperationKind::kRegisterWritePort, target, {i == 1 ? enableB : enableA, payload, clock}));
        graph.bindOutputPort("leaf_target" + std::to_string(i), fixture.read(target));
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=4096"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    const auto writerOwner = owner(graph, writers[0]);
    require(owner(graph, writers[1]) == writerOwner && owner(graph, writers[2]) == writerOwner,
            "Leaf assembly A/B/A writers did not share an execution owner");
    for (const auto& [role, names] : sites) for (const auto& name : names)
        require(bool(graph.findOperation(name)), "Leaf assembly fixture lost operation: " + role);
    const std::string body = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init();
    std::array<std::array<std::uint8_t,3>,224> saved{};
    std::array<std::array<std::array<std::uint8_t,3>,256>,3> expected{};
    const auto assemble = [&](const auto& data, unsigned index) {
        std::array<std::array<std::uint8_t,3>,256> result{};
        for (unsigned row = 0; row < 224; ++row) result[row] = data[row];
        if (index < 224) for (unsigned row = 224; row < 256; ++row) result[row] = data[index];
        return result;
    };
    for (unsigned step = 0; step < 16; ++step) {
        model.leaf_clock = 0; model.leaf_enable_a = step % 4 != 1; model.leaf_enable_b = step % 3 == 1;
        model.leaf_index = step % 4 == 3 ? 255 : (step * 37 + epoch * 13) % 224;
        for (unsigned row = 0; row < 224; ++row) for (unsigned col = 0; col < 3; ++col)
            model.leaf_next[row][col] = (row + col * 3 + step * 2 + epoch) & 3;
        model.eval(); const auto old = assemble(saved, model.leaf_index);
        assert(model.leaf_live == old);
        model.leaf_clock = 1; model.eval();
        if (model.leaf_enable_a) { expected[0] = old; expected[2] = old; saved = model.leaf_next; }
        if (model.leaf_enable_b) expected[1] = old;
        assert(model.leaf_source_out == saved && model.leaf_live == assemble(saved, model.leaf_index));
        assert(model.leaf_target0 == expected[0] && model.leaf_target1 == expected[1] && model.leaf_target2 == expected[2]);
        model.eval();
        assert(model.leaf_target0 == expected[0] && model.leaf_target1 == expected[1] && model.leaf_target2 == expected[2]);
    }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "leaf-assembly-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "", body, false, {{"execution-counters", "on"}});
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        const auto elided = manifest.substr(manifest.find("\"elided_operations\""));
        for (const auto& [role, names] : sites) for (const auto& name : names) {
            if (role.ends_with("_repeat") || role.ends_with("_concat"))
                require(elided.find("\"" + name + "\"") != std::string::npos, "Private leaf assembly scalar was materialized: " + role);
            if (role.ends_with("_snapshot"))
                require(elided.find("\"" + name + "\"") == std::string::npos &&
                            manifest.find("\"operation\":\"" + name + "\"") != std::string::npos,
                        "Leaf assembly removed a RegisterRead snapshot");
        }
        std::string generated;
        for (const auto& entry : std::filesystem::directory_iterator(artifact(label) / "model"))
            if (entry.path().extension() == ".cpp" || entry.path().extension() == ".hpp") generated += readFile(entry.path());
        require(generated.find("_BitInt(1344)") == std::string::npos && generated.find("_BitInt(1536)") == std::string::npos,
                "Leaf assembly retained a whole-array wide scalar pack");
    }
}


void testLeafAssemblyEdges() {
    Fixture edges; auto& edgeGraph = edges.graph;
    std::map<std::string, std::vector<std::string>> edgeSites;
    const auto edgeInput = [&](std::string_view name, const grh::LogicalType& type) {
        const auto value = edgeGraph.createValue(edgeGraph.internSymbol(name), type);
        edgeGraph.bindInputPort(name, value); return value;
    };
    const auto edgeTag = [&](grh::ValueId value, const std::string& role) {
        const auto name = "leaf_edge_" + role;
        edgeGraph.setOpSymbol(edgeGraph.valueDef(value), edgeGraph.internSymbol(name)); edgeSites[role].push_back(name); return value;
    };
    const auto edgeClock = edges.input("edge_clock"), edgeEnable = edges.input("edge_enable");
    const auto secondClock = edges.input("edge_second_clock");
    const auto select = edges.input("edge_select"), nibble = edges.input("edge_nibble", 4);
    auto signedLeaves = Fixture::type(64, {2, 8}); signedLeaves.isSigned = true;
    const auto signedData = edgeInput("edge_signed", signedLeaves);
    const auto tiny = edgeInput("edge_tiny", Fixture::type(16, {2, 2}));
    const auto repeated = edgeTag(edges.compute(OperationKind::kReplicate, Fixture::type(16), {nibble}), "signed_repeat");
    edgeGraph.setAttr(edgeGraph.valueDef(repeated), "rep", int64_t{4});
    auto signedNibbleType = Fixture::type(4); signedNibbleType.isSigned = true;
    const auto signedNibble = edgeTag(edges.compute(OperationKind::kSliceStatic, signedNibbleType, {signedData}), "single_signed");
    edgeGraph.setAttr(edgeGraph.valueDef(signedNibble), "sliceStart", int64_t{0});
    edgeGraph.setAttr(edgeGraph.valueDef(signedNibble), "sliceEnd", int64_t{3});
    const auto lowNibbles = edges.compute(OperationKind::kReplicate, Fixture::type(124), {nibble});
    edgeGraph.setAttr(edgeGraph.valueDef(lowNibbles), "rep", int64_t{31});
    const auto singleSignedConcat = edges.compute(OperationKind::kConcat, Fixture::type(128), {signedNibble, lowNibbles});
    edgeGraph.bindOutputPort("edge_single_signed", edges.compute(OperationKind::kAssign, Fixture::type(128, {32}), {singleSignedConcat}));
    const auto forward = edgeTag(edges.compute(OperationKind::kConcat, Fixture::type(96), {tiny, repeated, signedData}), "signed_forward");
    const auto reverse = edgeTag(edges.compute(OperationKind::kConcat, Fixture::type(96), {signedData, repeated, tiny}), "signed_reverse");
    const auto chosen = edgeTag(edges.compute(OperationKind::kMux, Fixture::type(96), {select, forward, reverse}), "signed_mux");
    auto signedResult = Fixture::type(96, {2, 3, 4}); signedResult.isSigned = true;
    edgeGraph.bindOutputPort("edge_signed_result", edges.compute(OperationKind::kAssign, signedResult, {chosen}));
    edgeGraph.bindOutputPort("edge_shaped_mux", edgeTag(edges.compute(OperationKind::kMux, signedResult,
        {select, forward, reverse}), "root_shaped_mux"));
    const auto five = edges.input("edge_five", 5), low70 = edgeInput("edge_low70", Fixture::type(70, {14}));
    const auto bad = edgeTag(edges.compute(OperationKind::kConcat, Fixture::type(75), {five, low70}), "misaligned");
    edgeGraph.bindOutputPort("edge_misaligned", edges.compute(OperationKind::kAssign, Fixture::type(75, {25}), {bad}));
    const auto stopData = edgeInput("edge_stop_data", Fixture::type(64, {8}));
    const auto wideScalar = [&](const std::string& role) {
        return edgeTag(edges.compute(OperationKind::kConcat, Fixture::type(128), {stopData, stopData}), role);
    };
    const auto stored = wideScalar("stored");
    edgeGraph.setValueSymbol(stored, edgeGraph.internSymbol("edge_stored_value"));
    edgeGraph.addDeclaredSymbol(edgeGraph.valueSymbol(stored));
    const auto observed = wideScalar("observed");
    edgeGraph.setValueSymbol(observed, edgeGraph.internSymbol("edge_observed_value"));
    edgeGraph.addDeclaredSymbol(edgeGraph.valueSymbol(observed));
    edgeGraph.bindOutputPort("edge_stored", edges.compute(OperationKind::kAssign, Fixture::type(128, {16}), {stored}));
    edgeGraph.bindOutputPort("edge_observed", edges.compute(OperationKind::kAssign, Fixture::type(128, {16}), {observed}));
    const auto cross = wideScalar("cross_owner");
    edgeGraph.bindOutputPort("edge_cross_live", edges.compute(OperationKind::kAssign, Fixture::type(128, {16}), {cross}));
    const auto crossTarget = edges.state("edge_cross_target", 128, {16});
    const auto crossPayload = edges.compute(OperationKind::kAssign, Fixture::type(128, {16}), {cross});
    edges.write(OperationKind::kRegisterWritePort, crossTarget,
                {edgeEnable, crossPayload, edgeClock});
    edgeGraph.bindOutputPort("edge_cross_saved", edges.read(crossTarget));
    const auto crossSecond = edges.state("edge_cross_second", 128, {16});
    const auto secondPayload = edges.compute(OperationKind::kAssign, Fixture::type(128, {16}), {cross});
    edges.write(OperationKind::kRegisterWritePort, crossSecond,
                {edgeEnable, secondPayload, secondClock});
    edgeGraph.bindOutputPort("edge_cross_second", edges.read(crossSecond));

    // One 64-bit intermediate can be a terminal for 32-bit leaves and an
    // expandable concat for 1-bit leaves. The first plan still needs its value.
    const auto high3 = edges.input("edge_high3", 3), low61 = edges.input("edge_low61", 61);
    const auto mixed = edgeTag(edges.compute(OperationKind::kConcat, Fixture::type(64), {high3, low61}), "mixed_terminal");
    const auto mixedConcat = edges.compute(OperationKind::kConcat, Fixture::type(128), {mixed, mixed});
    const auto mixedWords = edges.compute(OperationKind::kAssign, Fixture::type(128, {4}), {mixedConcat});
    const auto mixedBits = edges.compute(OperationKind::kAssign, Fixture::type(128, {128}), {mixedConcat});
    edgeGraph.bindOutputPort("edge_mixed_words", mixedWords);
    edgeGraph.bindOutputPort("edge_mixed_bits", mixedBits);
    const auto control = edgeTag(edges.compute(OperationKind::kAssign, Fixture::type(1), {select}), "mux_control");
    const auto controlRepeat = edges.compute(OperationKind::kReplicate, Fixture::type(128), {control});
    edgeGraph.setAttr(edgeGraph.valueDef(controlRepeat), "rep", int64_t{128});
    const auto controlMux = edges.compute(OperationKind::kMux, Fixture::type(128), {control, controlRepeat, mixedConcat});
    edgeGraph.bindOutputPort("edge_control_bits", edges.compute(OperationKind::kAssign, Fixture::type(128, {128}), {controlMux}));

    run(edges.design, "grh-to-grhsim", edges.session); edges.split(); edges.partition();
    require(owner(edgeGraph, edgeGraph.valueDef(mixed)) == owner(edgeGraph, edgeGraph.valueDef(mixedWords)) &&
                owner(edgeGraph, edgeGraph.valueDef(mixed)) == owner(edgeGraph, edgeGraph.valueDef(mixedBits)),
            "Mixed-leaf terminal fixture unexpectedly crossed an execution owner");
    const auto crossOwner = owner(edgeGraph, edgeGraph.valueDef(cross));
    require(crossOwner.size() == 4 && crossOwner[0] == "general" && crossOwner[1] == "nonsink",
            "Cross-owner leaf assembly fixture did not retain a nonsink boundary");
    const auto firstSink = owner(edgeGraph, edgeGraph.valueDef(crossPayload));
    const auto secondSink = owner(edgeGraph, edgeGraph.valueDef(secondPayload));
    require(firstSink.size() == 3 && firstSink[1] == "small-sink" &&
                secondSink.size() == 3 && secondSink[1] == "small-sink" && firstSink != secondSink &&
                edgeGraph.opOperands(edgeGraph.valueDef(crossPayload))[0] == cross &&
                edgeGraph.opOperands(edgeGraph.valueDef(secondPayload))[0] == cross,
            "Cross-owner leaf assembly fixture lost its two distinct sink consumers");
    for (const auto& [role, names] : edgeSites) for (const auto& name : names)
        require(bool(edgeGraph.findOperation(name)), "Leaf assembly edge fixture lost operation: " + role);
    const std::string edgeBody = R"CPP(
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); std::array<std::uint8_t,16> saved{};
    for (unsigned step = 0; step < 16; ++step) {
        model.edge_clock = 0; model.edge_second_clock = 0; model.edge_enable = step % 3 != 0; model.edge_select = step % 2;
        model.edge_nibble = (step * 3 + epoch) & 15; model.edge_five = (step * 7 + epoch) & 31;
        model.edge_high3 = (step + epoch) & 7;
        model.edge_low61 = (UINT64_C(0x123456789abcdef) * (step + 1) + epoch) & ((UINT64_C(1) << 61) - 1);
        for (unsigned i = 0; i < 16; ++i) model.edge_signed[i / 8][i % 8] = static_cast<std::int8_t>(((i * 7 + step * 5 + epoch) & 15) - 8);
        for (unsigned i = 0; i < 4; ++i) model.edge_tiny[i / 2][i % 2] = (step * 3 + i * 7 + epoch) & 15;
        for (unsigned i = 0; i < 14; ++i) model.edge_low70[i] = (step * 11 + i * 7 + epoch) & 31;
        for (unsigned i = 0; i < 8; ++i) model.edge_stop_data[i] = (step * 13 + i * 29 + epoch) & 255;
        model.eval();
        const unsigned repeat = model.edge_nibble * 0x1111;
        for (unsigned i = 0; i < 32; ++i)
            assert(model.edge_single_signed[i] == (i == 31 ? (unsigned(model.edge_signed[0][0]) & 15) : model.edge_nibble));
        unsigned __int128 source = 0; unsigned tiny = 0;
        for (unsigned i = 0; i < 16; ++i) source |= static_cast<unsigned __int128>(unsigned(model.edge_signed[i / 8][i % 8]) & 15) << (i * 4);
        for (unsigned i = 0; i < 4; ++i) tiny |= unsigned(model.edge_tiny[i / 2][i % 2]) << (i * 4);
        const unsigned __int128 signedExpected = model.edge_select ? source | (static_cast<unsigned __int128>(repeat) << 64) |
            (static_cast<unsigned __int128>(tiny) << 80) : tiny | (static_cast<unsigned __int128>(repeat) << 16) | (source << 32);
        for (unsigned i = 0; i < 24; ++i)
            assert(unsigned(model.edge_signed_result[i / 12][i / 4 % 3][i % 4]) == ((signedExpected >> (i * 4)) & 15));
        assert(model.edge_shaped_mux == model.edge_signed_result);
        unsigned __int128 misaligned = static_cast<unsigned __int128>(model.edge_five) << 70;
        for (unsigned i = 0; i < 14; ++i) misaligned |= static_cast<unsigned __int128>(model.edge_low70[i]) << (i * 5);
        for (unsigned i = 0; i < 25; ++i) assert(model.edge_misaligned[i] == ((misaligned >> (i * 3)) & 7));
        for (unsigned i = 0; i < 16; ++i) {
            const auto value = model.edge_stop_data[i % 8];
            assert(model.edge_stored[i] == value && model.edge_observed[i] == value && model.edge_cross_live[i] == value);
        }
        const std::uint64_t mixed = (std::uint64_t(model.edge_high3) << 61) | model.edge_low61;
        for (unsigned i = 0; i < 4; ++i) assert(model.edge_mixed_words[i] == std::uint32_t(mixed >> (32 * (i % 2))));
        for (unsigned i = 0; i < 128; ++i) {
            const bool bit = (mixed >> (i % 64)) & 1;
            assert(model.edge_mixed_bits[i] == bit && model.edge_control_bits[i] == (model.edge_select ? true : bit));
        }
        model.edge_clock = 1; model.edge_second_clock = 1; model.eval();
        if (model.edge_enable) for (unsigned i = 0; i < 16; ++i) saved[i] = model.edge_stop_data[i % 8];
        assert(model.edge_cross_saved == saved && model.edge_cross_second == saved);
    }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "leaf-assembly-edges-b" + std::to_string(budget);
        manualCase(edges, label, budget, "", edgeBody, true, {{"execution-counters", "on"}});
        const auto manifest = readFile(artifact(label) / "model/manifest.json");
        const auto elided = manifest.substr(manifest.find("\"elided_operations\""));
        for (const auto& [role, names] : edgeSites) for (const auto& name : names) {
            const bool removed = elided.find("\"" + name + "\"") != std::string::npos;
            if (role.starts_with("signed_")) require(removed, "Signed leaf assembly scalar did not elide: " + role);
            else require(!removed && manifest.find("\"operation\":\"" + name + "\"") != std::string::npos,
                         "Required leaf assembly terminal lost materialization or counters: " + role);
        }
    }
}

void testLeafAssemblyConstants() {
    Fixture fixture; auto& graph = fixture.graph;
    std::vector<std::pair<grh::OperationId, std::string>> placements;
    std::map<std::string, grh::OperationId> sites;
    const auto place = [&](grh::ValueId value, const std::string& role, const std::string& key = "main") {
        const auto op = graph.valueDef(value);
        graph.setOpSymbol(op, graph.internSymbol("leaf_constant_" + role));
        placements.emplace_back(op, key); sites.emplace(role, op); return value;
    };
    const auto constant = [&](const std::string& role, const std::string& text, int width = 80,
                              const std::string& key = "main") {
        auto type = Fixture::type(width); type.isSigned = true;
        const auto value = place(fixture.compute(OperationKind::kConstant, type, {}), role, key);
        graph.setAttr(graph.valueDef(value), "constValue", text); return value;
    };
    const auto select = fixture.input("leaf_constant_select");
    std::vector<grh::ValueId> inputs;
    std::string prepare;
    for (unsigned i = 0; i < 16; ++i) {
        const auto name = "leaf_constant_input" + std::to_string(i);
        inputs.push_back(fixture.input(name));
        prepare += "m." + name + "=((step*13+epoch*7) >> " + std::to_string(i % 5) + ")&1; "
            "low|=unsigned(m." + name + ")<<" + std::to_string(i) + ";\n";
    }
    const auto lowOp = graph.createOperation(OperationKind::kConcat);
    const auto low = graph.createValue(Fixture::type(16));
    for (auto i = inputs.rbegin(); i != inputs.rend(); ++i) graph.addOperand(lowOp, *i);
    graph.addResult(lowOp, low); place(low, "low");
    // The low 17 bits are zero, the next 23 are one, and the high 40 alternate.
    // A signed source also checks that leaf extraction uses packed bits.
    const std::string image = "80'sh" "aaaa" "aaaaaafffffe0000";
    const std::string inverse = "80'sh" "5555" "555555000001ffff";
    const auto a = constant("private_a", image), b = constant("private_b", inverse);
    const auto direct = place(fixture.compute(OperationKind::kConcat, Fixture::type(96), {a, low}), "direct_concat");
    graph.bindOutputPort("leaf_constant_bits", place(fixture.compute(OperationKind::kAssign,
        Fixture::type(96, {96}), {direct}), "direct_result"));
    auto duplicate = place(fixture.compute(OperationKind::kConcat, Fixture::type(160), {a, a}), "private_duplicate_concat");
    for (unsigned i = 0; i < 24; ++i)
        duplicate = place(fixture.compute(OperationKind::kAssign, Fixture::type(160), {duplicate}),
            "private_chain" + std::to_string(i));
    graph.bindOutputPort("leaf_constant_duplicate", place(fixture.compute(OperationKind::kAssign,
        Fixture::type(160, {160}), {duplicate}), "duplicate_result"));
    const auto chosen = place(fixture.compute(OperationKind::kMux, Fixture::type(80), {select, a, b}), "private_mux");
    const auto assembled = place(fixture.compute(OperationKind::kConcat, Fixture::type(96), {chosen, low}), "private_concat");
    std::string verify;
    for (const int width : {1, 2, 4, 8}) {
        auto type = Fixture::type(96, {2, 48 / width}); type.isSigned = true;
        const auto role = "signed" + std::to_string(width);
        graph.bindOutputPort("leaf_constant_" + role,
            place(fixture.compute(OperationKind::kAssign, type, {assembled}), role + "_result"));
        verify += "for(unsigned i=0;i<" + std::to_string(96 / width) + ";++i) assert((unsigned(m.leaf_constant_" + role +
            "[i/" + std::to_string(48 / width) + "][i%" + std::to_string(48 / width) + "])&" +
            std::to_string((1u << width) - 1) + ")==leafConstantExpected(low,i*" + std::to_string(width) + ',' +
            std::to_string(width) + ",!m.leaf_constant_select));\n";
    }
    // Long equal runs must become bounded loops, including explicit zero writes.
    const auto pressure = constant("private_pressure", "4096'h" + std::string(512, 'a') +
        std::string(256, 'f') + std::string(256, '0'), 4096);
    const auto pressureConcat = place(fixture.compute(OperationKind::kConcat, Fixture::type(4112),
        {pressure, low}), "pressure_concat");
    graph.bindOutputPort("leaf_constant_pressure", place(fixture.compute(OperationKind::kAssign,
        Fixture::type(4112, {4112}), {pressureConcat}), "pressure_result"));

    const auto preserved = [&](const std::string& role, const std::string& sourceOwner = "main",
                               const std::string& resultOwner = "main") {
        const auto value = constant(role + "_constant", image, 80, sourceOwner);
        const auto concat = place(fixture.compute(OperationKind::kConcat, Fixture::type(96),
            {value, low}), role + "_concat", sourceOwner);
        graph.bindOutputPort("leaf_constant_" + role, place(fixture.compute(OperationKind::kAssign,
            Fixture::type(96, {96}), {concat}), role + "_result", resultOwner));
        return std::pair{value, concat};
    };
    const auto [live, liveConcat] = preserved("live");
    const auto mask = place(fixture.compute(OperationKind::kReplicate, Fixture::type(96), {select}), "packed_mask");
    graph.setAttr(graph.valueDef(mask), "rep", int64_t{96});
    graph.bindOutputPort("leaf_constant_packed", place(fixture.compute(OperationKind::kXor,
        Fixture::type(96), {liveConcat, mask}), "packed_result"));
    // A live scalar constant consumer separately pins the old literal operation.
    graph.bindOutputPort("leaf_constant_scalar", place(fixture.compute(OperationKind::kXor,
        Fixture::type(80), {live, low}), "scalar_result"));
    const auto [cross, crossConcat] = preserved("cross", "cross_source", "cross_remote");
    graph.bindOutputPort("leaf_constant_cross_local", place(fixture.compute(OperationKind::kAssign,
        Fixture::type(96, {96}), {crossConcat}), "cross_local_result", "cross_source"));
    const auto [observed, observedConcat] = preserved("observed");
    for (const auto value : {observed, observedConcat}) {
        graph.setValueSymbol(value, graph.internSymbol(value == observed ?
            "leaf_constant_observed_value" : "leaf_constant_observed_concat_value"));
        graph.addDeclaredSymbol(graph.valueSymbol(value));
    }
    const auto [stored, storedConcat] = preserved("stored");
    const auto [output, outputConcat] = preserved("output");
    graph.bindOutputPort("leaf_constant_output_scalar", output);
    (void)cross; (void)storedConcat; (void)outputConcat;

    const auto clock = fixture.input("leaf_constant_clock"), enable = fixture.input("leaf_constant_enable");
    const auto saved = fixture.state("leaf_constant_saved", 96, {96});
    const auto sinkTag = [&](grh::ValueId value, const std::string& role) {
        const auto op = graph.valueDef(value);
        graph.setOpSymbol(op, graph.internSymbol("leaf_constant_" + role)); sites.emplace(role, op); return value;
    };
    const auto sinkA = sinkTag(fixture.constant(80, image), "sink_private_a");
    const auto sinkB = sinkTag(fixture.constant(80, inverse), "sink_private_b");
    const auto sinkMux = sinkTag(fixture.compute(OperationKind::kMux, Fixture::type(80),
        {select, sinkA, sinkB}), "sink_private_mux");
    const auto sinkConcat = sinkTag(fixture.compute(OperationKind::kConcat, Fixture::type(96),
        {sinkMux, low}), "sink_private_concat");
    const auto sinkPayload = sinkTag(fixture.compute(OperationKind::kAssign, Fixture::type(96, {96}),
        {sinkConcat}), "sink_payload");
    fixture.write(OperationKind::kRegisterWritePort, saved, {enable, sinkPayload, clock});
    graph.bindOutputPort("leaf_constant_saved", fixture.read(saved));

    // Anchor each original output cone in General with a scalar DPI consumer.
    // Array probes select one leaf without adding a pressure-width packed ABI.
    std::vector<grh::ValueId> arguments;
    std::vector<std::string> argumentNames;
    std::vector<int64_t> argumentWidths;
    std::vector<bool> argumentSigns;
    std::string anchorSupport = "extern \"C\" std::uint8_t leaf_constant_anchor(";
    for (const auto& port : graph.outputPorts()) {
        auto value = port.value;
        if (!graph.valueShape(value).empty()) {
            value = place(fixture.compute(OperationKind::kSliceStatic, Fixture::type(1), {value}),
                "probe_" + port.name, "audit");
            graph.setAttr(graph.valueDef(value), "sliceStart", int64_t{0});
            graph.setAttr(graph.valueDef(value), "sliceEnd", int64_t{0});
        }
        const auto type = graph.valueLogicalType(value);
        if (!arguments.empty()) anchorSupport += ',';
        anchorSupport += type.width > 64 ? "const unsigned _BitInt(128)&" : "std::uint8_t";
        anchorSupport += " p" + std::to_string(arguments.size());
        argumentNames.push_back("p" + std::to_string(arguments.size()));
        argumentWidths.push_back(type.width); argumentSigns.push_back(type.isSigned); arguments.push_back(value);
    }
    // A real crossing retains an unobserved boundary store for the pure constant.
    anchorSupport += ",const unsigned _BitInt(128)& p" + std::to_string(arguments.size());
    argumentNames.push_back("p" + std::to_string(arguments.size()));
    argumentWidths.push_back(80); argumentSigns.push_back(true); arguments.push_back(stored);
    anchorSupport += ") { return 0; }\n";
    const auto imported = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("leaf_constant_anchor"));
    graph.setAttr(imported, "argsName", argumentNames);
    graph.setAttr(imported, "argsDirection", std::vector<std::string>(arguments.size(), "input"));
    graph.setAttr(imported, "argsWidth", argumentWidths); graph.setAttr(imported, "argsSigned", argumentSigns);
    graph.setAttr(imported, "hasReturn", true); graph.setAttr(imported, "returnWidth", int64_t{8});
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("leaf_constant_anchor")); graph.addOperand(call, select);
    for (const auto value : arguments) graph.addOperand(call, value);
    const auto called = graph.createValue(Fixture::type(8)); graph.addResult(call, called); place(called, "anchor_result", "audit");
    graph.bindOutputPort("leaf_constant_anchor_result", called);

    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    for (const auto& [op, key] : placements) {
        const auto path = owner(graph, op);
        require(path.size() == 4 && path[0] == "general" && path[1] == "nonsink",
                "Constant leaf fixture lost an original General nonsink operation");
        graph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "leaf_constants_" + key, "node_" + key});
    }
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    require(owner(graph, graph.valueDef(a)) == owner(graph, graph.valueDef(direct)) &&
                owner(graph, graph.valueDef(crossConcat)) != owner(graph, sites.at("cross_result")),
            "Constant leaf fixture lost its private or crossing consumer");
    const auto sinkOwner = owner(graph, graph.valueDef(sinkPayload));
    require(sinkOwner.size() == 3 && sinkOwner[1] == "small-sink" &&
                owner(graph, graph.valueDef(sinkA)) == sinkOwner && owner(graph, graph.valueDef(sinkB)) == sinkOwner,
            "Constant leaf fixture lost the sink's uninitialized reusable frame");
    const std::string support = anchorSupport + R"CPP(
static unsigned leafConstantExpected(unsigned low,unsigned first,unsigned width,bool inverse=false) {
    constexpr std::array<std::uint64_t,2> words{UINT64_C(0xaaaaaafffffe0000),UINT64_C(0xaaaa)};
    unsigned result=0;
    for(unsigned i=0;i<width;++i) {
        const unsigned bit=first+i;
        const bool value=bit<16 ? (low>>bit)&1 : bool((words[(bit-16)/64]>>((bit-16)%64))&1)^inverse;
        result|=unsigned(value)<<i;
    }
    return result;
}
)CPP";
    const std::string body = R"CPP(
model.configure_waveform(true,"leaf-constants.fst");
GrhSIM_top peer;
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); peer.init();
    std::array<bool,96> saved{},peerSaved{};
    const auto exercise=[&](GrhSIM_top& m,unsigned step,std::array<bool,96>& expectedSaved) {
        unsigned low=0; m.leaf_constant_select=step&1;
        m.leaf_constant_clock=0; m.leaf_constant_enable=step%3!=1;
)CPP" + prepare + R"CPP(
        m.eval();
        for(unsigned i=0;i<96;++i) {
            const bool expected=leafConstantExpected(low,i,1);
            assert(m.leaf_constant_bits[i]==expected && m.leaf_constant_live[i]==expected);
            assert(m.leaf_constant_cross[i]==expected && m.leaf_constant_cross_local[i]==expected);
            assert(m.leaf_constant_observed[i]==expected && m.leaf_constant_stored[i]==expected && m.leaf_constant_output[i]==expected);
            assert(bool((m.leaf_constant_packed>>i)&1)==(expected^bool(m.leaf_constant_select)));
        }
        for(unsigned i=0;i<80;++i) {
            assert(bool((m.leaf_constant_output_scalar>>i)&1)==bool(leafConstantExpected(0,i+16,1)));
            const bool expected=bool(leafConstantExpected(0,i+16,1))^(i<16 && bool((low>>i)&1));
            assert(bool((m.leaf_constant_scalar>>i)&1)==expected);
        }
        for(unsigned i=0;i<160;++i)
            assert(m.leaf_constant_duplicate[i]==bool(leafConstantExpected(0,i%80+16,1)));
        std::array<std::uint64_t,64> pressureWords{};
        for(unsigned i=16;i<64;++i) pressureWords[i]=i<32 ? UINT64_MAX : UINT64_C(0xaaaaaaaaaaaaaaaa);
        for(unsigned i=0;i<4112;++i) {
            const bool expected=i<16 ? (low>>i)&1 : (pressureWords[(i-16)/64]>>((i-16)%64))&1;
            assert(m.leaf_constant_pressure[i]==expected);
        }
)CPP" + verify + R"CPP(
        m.eval();
        for(unsigned i=0;i<96;++i) assert(m.leaf_constant_bits[i]==bool(leafConstantExpected(low,i,1)));
        assert(m.leaf_constant_saved==expectedSaved);
        m.leaf_constant_clock=1; m.eval();
        if(m.leaf_constant_enable) for(unsigned i=0;i<96;++i)
            expectedSaved[i]=leafConstantExpected(low,i,1,!m.leaf_constant_select);
        assert(m.leaf_constant_saved==expectedSaved);
        m.eval(); assert(m.leaf_constant_saved==expectedSaved);
    };
    for(unsigned step=0;step<12;++step) { exercise(model,step,saved); exercise(peer,step+19,peerSaved); }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "leaf-constants-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, true, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        const auto manifest = readFile(directory / "manifest.json");
        const auto elided = manifest.substr(manifest.find("\"elided_operations\""));
        const auto map = readFile(directory / "code-map.json");
        const auto records = mappedRecords(map, "operations");
        for (const auto& [role, op] : sites) {
            const auto name = std::string(graph.getOperation(op).symbolText());
            const bool removed = elided.find("\"" + name + "\"") != std::string::npos;
            const bool expected = role.starts_with("private_") || role.starts_with("sink_private_") ||
                role == "direct_concat" || role == "pressure_concat";
            require(removed == expected, "Constant leaf materialization mismatch: " + role);
            const bool counted = manifest.find("\"operation\":\"" + name + "\"") != std::string::npos;
            require(counted != removed, "Constant leaf changed surviving-operation counter semantics: " + role);
            const auto prefix = "{\"operation_id\":\"o" + std::to_string(op.index) + "\",";
            bool mapped = false;
            for (const auto& record : records) if (record.starts_with(prefix)) {
                mapped = record.find(std::string("\"elided\":") + (removed ? "true" : "false")) != std::string::npos;
                break;
            }
            require(mapped, "Constant leaf lost its operation/source-map record: " + role);
        }
        const auto storedPrefix = "{\"value_id\":\"v" + std::to_string(stored.index) + "\",";
        bool storedBoundary = false;
        for (const auto& record : mappedRecords(map, "values")) if (record.starts_with(storedPrefix))
            storedBoundary = record.find("\"role\":\"primary\"") != std::string::npos;
        require(storedBoundary, "Stored constant fixture lost its ordinary crossing boundary");
        const auto cpp = generatedCpp(directory);
        require(cpp.find("_BitInt(4096)") == std::string::npos && cpp.find("_BitInt(4160)") == std::string::npos,
                "Constant pressure plan retained a runtime wide temporary");
        require(cpp.find("wave_register(\"leaf_constant_observed_value\"") != std::string::npos &&
                    cpp.find("wave_register(\"leaf_constant_observed_concat_value\"") != std::string::npos,
                "Constant leaf plan removed a declared waveform observer");
    }
}

void indexedWakeSites(const std::string& label, const std::vector<std::string>& names) {
    const auto manifest = readFile(artifact(label) / "model/manifest.json");
    const std::regex entries(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
    std::map<std::string, unsigned> sites; unsigned index = 0;
    for (std::sregex_iterator it(manifest.begin(), manifest.end(), entries), end; it != end; ++it, ++index)
        if ((*it)[2] == "operation") sites.emplace((*it)[1], index);
    std::string declaration = "static const std::array<unsigned," + std::to_string(names.size()) + "> indexedWakeSites{";
    for (const auto& name : names) {
        require(sites.contains(name), "Indexed wake read lost its execution site: " + name);
        declaration += std::to_string(sites.at(name)) + ',';
    }
    declaration += "};\n";
    const auto path = artifact(label) / "driver.cpp";
    auto driver = readFile(path); driver.insert(driver.find("static std::vector<unsigned long long> indexedWakeCounts"), declaration);
    std::ofstream(path) << driver;
}

const std::string indexedWakeSupport = R"CPP(
#include <cassert>
#include <vector>
static std::vector<unsigned long long> indexedWakeCounts(GrhSIM_top& model) {
    auto* stream = std::tmpfile(); assert(stream); model.dump_execution_counts(stream); std::rewind(stream);
    char line[256]; assert(std::fgets(line,sizeof(line),stream) && std::fgets(line,sizeof(line),stream));
    std::vector<unsigned long long> counts;
    while (std::fgets(line,sizeof(line),stream)) {
        unsigned site; unsigned long long count;
        if (std::sscanf(line,"%u\t%llu",&site,&count)!=2) break;
        assert(site==counts.size()); counts.push_back(count);
    }
    std::fclose(stream); std::vector<unsigned long long> result;
    for (const auto site : indexedWakeSites) result.push_back(counts.at(site));
    return result;
}
)CPP";

void testIndexedMemoryWake() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("iw_clock"), sampleClock=fixture.input("iw_sample_clock");
    const auto sampleEnable=fixture.input("iw_sample_enable");
    const auto enable = fixture.input("iw_enable"), middle = fixture.input("iw_middle"), last = fixture.input("iw_last");
    const auto wholeEnable = fixture.input("iw_whole"), rowEnable = fixture.input("iw_row_write");
    const auto writeRow = fixture.input("iw_write_row", 4), writeCol = fixture.input("iw_write_col", 4);
    const auto data = fixture.input("iw_data", 8), middleData = fixture.input("iw_middle_data", 8);
    const auto lastData = fixture.input("iw_last_data", 8), mask = fixture.input("iw_mask", 8);
    const auto aRow = fixture.input("iw_a_row", 128), aCol = fixture.input("iw_a_col", 4);
    const auto bRow = fixture.input("iw_b_row", 8), bCol = fixture.input("iw_b_col", 4);
    auto signedIndex = Fixture::type(8); signedIndex.isSigned = true; graph.setValueLogicalType(bRow, signedIndex);
    const auto readRow = fixture.input("iw_read_row", 4), extra = fixture.input("iw_extra", 8);
    const auto shapedInput = [&](const std::string& name, int width, std::vector<int64_t> shape) {
        const auto value = graph.createValue(graph.internSymbol(name), Fixture::type(width, std::move(shape)));
        graph.bindInputPort(name, value); return value;
    };
    const auto wholeData = shapedInput("iw_whole_data", 768, {8,12}), rowData = shapedInput("iw_row_data", 96, {12});
    const auto memory = fixture.state("indexed_memory", 768, {8,12});
    const auto tag = [&](grh::ValueId value, const std::string& name) {
        graph.setOpSymbol(graph.valueDef(value), graph.internSymbol(name)); return value;
    };
    const auto a = tag(fixture.read(memory, {aRow, aCol}), "indexed_wake_read_a");
    const auto b = tag(fixture.read(memory, {bRow, bCol}), "indexed_wake_read_b");
    const auto row = tag(fixture.read(memory, {readRow}), "indexed_wake_read_row");
    const auto aResult=tag(fixture.compute(OperationKind::kXor, Fixture::type(8), {a, extra}), "indexed_wake_extra_fanout");
    graph.bindOutputPort("iw_a", aResult);
    graph.bindOutputPort("iw_b", b); graph.bindOutputPort("iw_row", row);
    for (const auto& [name,value] : std::vector<std::pair<std::string,grh::ValueId>>{{"a",aResult},{"b",b},{"row",row}}) {
        const auto type=graph.valueLogicalType(value);
        const auto first=fixture.state("indexed_capture_"+name+"_a",type.width,type.shape);
        const auto second=fixture.state("indexed_capture_"+name+"_b",type.width,type.shape);
        fixture.write(OperationKind::kRegisterWritePort,first,{sampleEnable,value,clock});
        fixture.write(OperationKind::kRegisterWritePort,second,{sampleEnable,value,sampleClock});
    }
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {enable, writeRow, writeCol, data, mask, clock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {middle, writeRow, writeCol, middleData, clock});
    fixture.write(OperationKind::kRegisterWriteArrayMaskPort, memory, {last, writeRow, writeCol, lastData, mask, clock});
    fixture.write(OperationKind::kRegisterWriteArrayPort, memory, {rowEnable, writeRow, rowData, clock});
    fixture.write(OperationKind::kRegisterWritePort, memory, {wholeEnable, wholeData, clock});
    run(fixture.design, "grh-to-grhsim", fixture.session);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=1"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session,
        {"-max-supernode-nodes=1", "-clone-shared=0"});
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    std::set<Owner> readers;
    for (const auto value : {a,b,row}) {
        const auto path = owner(graph, graph.valueDef(value));
        require(path.size()==4 && path[0]=="general" && path[1]=="nonsink",
                "Indexed wake fixture read is not an executable nonsink");
        readers.insert(path);
    }
    require(readers.size()==3, "Indexed wake fixture merged independent read owners");
    const std::string body = R"CPP(
for (unsigned epoch=0; epoch<3; ++epoch) {
    model.init(); std::array<unsigned,96> expected{};
    model.iw_clock=0; model.iw_sample_clock=0; model.iw_sample_enable=0;
    model.iw_a_row=1; model.iw_a_col=1; model.iw_b_row=2; model.iw_b_col=2;
    model.iw_read_row=5; model.iw_extra=0; model.iw_enable=0; model.iw_middle=0; model.iw_last=0;
    model.iw_whole=0; model.iw_row_write=0;
    for (unsigned i=0;i<96;++i) model.iw_whole_data[i/12][i%12]=i+17;
    for (unsigned i=0;i<12;++i) model.iw_row_data[i]=i+71;
    const auto check = [&] {
        const bool av=model.iw_a_row<8 && model.iw_a_col<12;
        const auto br=static_cast<unsigned>(static_cast<std::uint8_t>(model.iw_b_row));
        assert(model.iw_a==((av?expected[static_cast<unsigned>(model.iw_a_row)*12+model.iw_a_col]:0)^model.iw_extra));
        assert(model.iw_b==(br<8 && model.iw_b_col<12?expected[br*12+model.iw_b_col]:0));
        for(unsigned i=0;i<12;++i) assert(model.iw_row[i]==(model.iw_read_row<8?expected[model.iw_read_row*12+i]:0));
    };
    model.eval(); check();
    const auto pulse = [&](unsigned r,unsigned c,unsigned value,unsigned bits,unsigned flags) {
        model.iw_clock=0; model.iw_write_row=r; model.iw_write_col=c; model.iw_data=value; model.iw_mask=bits;
        model.iw_enable=flags&1; model.iw_middle=(flags>>1)&1; model.iw_last=(flags>>2)&1;
        model.iw_row_write=(flags>>3)&1; model.iw_whole=(flags>>4)&1;
        model.iw_middle_data=value^0xff; model.iw_last_data=value; model.eval(); check();
        const auto before=indexedWakeCounts(model); const auto old=expected;
        const bool wrote=((flags&7) && r<8 && c<12) || ((flags&8) && r<8) || (flags&16);
        if(r<8 && c<12) {
            const unsigned i=r*12+c;
            if(flags&1) expected[i]=(expected[i]&(~bits&255))|(value&bits);
            if(flags&2) expected[i]=value^0xff;
            if(flags&4) expected[i]=(expected[i]&(~bits&255))|(value&bits);
        }
        if((flags&8) && r<8) for(unsigned i=0;i<12;++i) expected[r*12+i]=i+71;
        if(flags&16) for(unsigned i=0;i<96;++i) expected[i]=i+17;
        model.iw_clock=1; model.eval(); check(); const auto after=indexedWakeCounts(model);
        // Every valid on-write commit activates all readers, including a read
        // of an unrelated or currently invalid address, as in gsim.
        for(unsigned i=0;i<3;++i) assert(after[i]-before[i]==(indexedWakeOnWrite?wrote:(old!=expected)));
        const auto settled=after; model.eval(); assert(indexedWakeCounts(model)==settled); check();
    };
    pulse(6,3,19,255,1); // All three readers use unrelated addresses.
    pulse(1,1,0,255,1); pulse(1,1,9,0,1); // Same value and zero mask still touch.
    pulse(1,1,7,255,1); pulse(1,1,7,255,7); // A/B/A cancellation.
    pulse(2,2,9,255,1); pulse(5,2,13,255,1);
    pulse(5,3,21,255,1); pulse(5,4,22,255,1); // Partial row crosses leaf 63/64.
    pulse(15,1,99,255,1); pulse(1,15,99,255,1);
    pulse(5,0,0,255,8); pulse(0,0,0,255,16); // Partial-row and whole writes.
    model.iw_clock=0; model.iw_a_row=(static_cast<unsigned _BitInt(128)>(1)<<80)|1;
    model.iw_b_row=-1; model.iw_read_row=8; model.eval(); check();
    pulse(1,1,201,255,1); // Invalid readers still execute after a valid write.
    model.iw_clock=0; model.iw_a_row=1; model.iw_b_row=2; model.iw_read_row=5; model.eval(); check();
    pulse(1,1,202,255,1); pulse(5,3,91,255,1);
    const auto before=indexedWakeCounts(model); model.iw_extra=33; model.eval(); check();
    const auto after=indexedWakeCounts(model);
    for(unsigned i=0;i<3;++i) assert(after[i]==before[i]);
    assert(after[3]>before[3]); // Independent input fanout must remain active.
}
)CPP";
    for (const auto budget : {16u,1024u}) for (const std::string bitmap : {"on","off"})
        for (const std::string mode : {"on-write","on-change"}) {
            const auto label="indexed-wake-"+mode+"-"+bitmap+"-b"+std::to_string(budget);
            manualCase(fixture,label,budget,"static constexpr bool indexedWakeOnWrite="+std::string(mode=="on-write"?"true;\n":"false;\n")+indexedWakeSupport,
                       body,false,{{"execution-counters","on"},{"mem-enable-bitmap",bitmap},{"mem-write-activate",mode}});
            const auto header=readFile(artifact(label)/"model/grhsim_top.hpp");
            for (const auto removed : {"largeBeforeStore", "memoryReadIndexStore", "memoryTouchedStore", "memory_read_touched", "memory_touch_range"})
                require(header.find(removed)==std::string::npos,"On-write reader activation retained obsolete bookkeeping");
            const auto mapState=mappedOperation(artifact(label)/"model",memory);
            require((header.find("std::map<std::size_t,std::uint8_t> "+mapState+";")!=std::string::npos)==(mode=="on-change"),
                    "Large before-images must be limited to on-change states");
            indexedWakeSites(label,{"indexed_wake_read_a","indexed_wake_read_b","indexed_wake_read_row","indexed_wake_extra_fanout"});
        }
}

void testIndexedMemoryWakeTemporal() {
    Fixture fixture; auto& graph=fixture.graph;
    const auto clockA=fixture.input("it_clock_a"), clockB=fixture.input("it_clock_b");
    const auto enableA=fixture.input("it_enable_a"), enableB=fixture.input("it_enable_b");
    const auto gateEnable=fixture.input("it_gate_enable"), gateData=fixture.input("it_gate_data");
    const auto index=fixture.input("it_index",4), address=fixture.input("it_address",4), data=fixture.input("it_data",8);
    const auto one=fixture.constant(1,"1'b1");
    const auto memory=fixture.state("temporal_memory",64,{8}), peer=fixture.state("temporal_peer",64,{8});
    grh::setStateInitSpec(graph,memory,{{grh::InitKind::Fill,"8'd17"}});
    grh::setStateInitSpec(graph,peer,{{grh::InitKind::Fill,"8'd33"}});
    const auto full=fixture.read(memory), selected=fixture.read(memory,{index}), peerRead=fixture.read(peer,{address});
    graph.setOpSymbol(graph.valueDef(full),graph.internSymbol("indexed_wake_full"));
    graph.setOpSymbol(graph.valueDef(selected),graph.internSymbol("indexed_wake_mixed"));
    const auto parity=fixture.compute(OperationKind::kReduceXor,Fixture::type(1),{full});
    const auto combined=fixture.compute(OperationKind::kAdd,Fixture::type(8),{selected,parity});
    graph.bindOutputPort("it_combined",combined);
    // Keep the indexed and full reads in one execution owner. Its full read must
    // override any narrower subscription inferred from its indexed peer.
    const auto savedA=fixture.state("temporal_saved_a",8), savedB=fixture.state("temporal_saved_b",8);
    fixture.write(OperationKind::kRegisterWritePort,savedA,{one,combined,clockA});
    fixture.write(OperationKind::kRegisterWritePort,savedB,{one,combined,clockB});
    const auto selective=fixture.read(memory,{index});
    graph.setOpSymbol(graph.valueDef(selective),graph.internSymbol("indexed_wake_selective"));
    graph.bindOutputPort("it_selective",selective);
    for(const auto captureClock : {clockA,clockB}) {
        const auto capture=fixture.state(captureClock==clockA?"temporal_selective_a":"temporal_selective_b",8);
        fixture.write(OperationKind::kRegisterWritePort,capture,{one,selective,captureClock});
    }
    fixture.write(OperationKind::kRegisterWriteArrayPort,memory,{enableA,address,data,clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort,peer,{enableA,address,selected,clockA});
    fixture.write(OperationKind::kRegisterWriteArrayPort,memory,{enableB,address,peerRead,clockB});
    const auto gate=fixture.state("temporal_gate",1), derived=fixture.state("temporal_derived",8);
    const auto gateRead=fixture.read(gate);
    fixture.write(OperationKind::kRegisterWritePort,gate,{gateEnable,gateData,clockA});
    fixture.write(OperationKind::kRegisterWritePort,derived,{one,selected,gateRead});
    const auto fullOnly=fixture.read(peer);
    graph.setOpSymbol(graph.valueDef(fullOnly),graph.internSymbol("indexed_wake_full_only"));
    const auto peerParity=fixture.compute(OperationKind::kReduceXor,Fixture::type(1),{fullOnly});
    graph.bindOutputPort("it_peer_parity",peerParity);
    for(const auto captureClock : {clockA,clockB}) {
        const auto capture=fixture.state(captureClock==clockA?"temporal_full_a":"temporal_full_b",1);
        fixture.write(OperationKind::kRegisterWritePort,capture,{one,peerParity,captureClock});
    }
    for(const auto& [name,state] : std::vector<std::pair<std::string,grh::OperationId>>{
            {"it_saved_a",savedA},{"it_saved_b",savedB},{"it_derived",derived},{"it_gate",gate}})
        graph.bindOutputPort(name,fixture.read(state));
    run(fixture.design,"grh-to-grhsim",fixture.session);
    const auto reference=fixture.design.clone();
    run(fixture.design,"grhsim-split-event-and-output",fixture.session);
    run(fixture.design,"grhsim-split-nonsink-and-sink",fixture.session,{"-mem-min-bytes=1"});
    run(fixture.design,"grhsim-partition-nonsink",fixture.session,{"-max-supernode-nodes=4","-clone-shared=0"});
    run(fixture.design,"grhsim-partition-sink",fixture.session);
    auto mixedOwner=owner(graph,graph.valueDef(selected));
    auto fullOwner=owner(graph,graph.valueDef(full));
    require(mixedOwner.size()==4 && mixedOwner[1]=="nonsink" && fullOwner.size()==4 && fullOwner[1]=="nonsink",
            "Indexed wake fallback fixture lost its nonsink readers");
    // Leaf nodes constrain rewriting; their enclosing supernode executes once.
    mixedOwner.resize(3); fullOwner.resize(3);
    require(mixedOwner==fullOwner,
            "Indexed wake fallback fixture did not retain a mixed full/indexed nonsink owner");
    auto selectiveOwner=owner(graph,graph.valueDef(selective));
    require(selectiveOwner.size()==4 && selectiveOwner[1]=="nonsink",
            "Indexed wake fallback fixture lost its selective nonsink reader");
    selectiveOwner.resize(3);
    require(selectiveOwner!=mixedOwner,
            "Indexed wake fallback fixture merged its selective and full readers");
    require(owner(graph,graph.valueDef(fullOnly)).size()==4 && owner(graph,graph.valueDef(fullOnly))[1]=="nonsink",
            "Indexed wake fallback fixture lost its full-only nonsink reader");
    std::vector<std::map<std::string,uint64_t>> inputs;
    const auto step=[&](unsigned a,unsigned b,unsigned ea,unsigned eb,unsigned ge,unsigned gd,unsigned ix,unsigned at,unsigned value) {
        inputs.push_back({{"it_clock_a",a},{"it_clock_b",b},{"it_enable_a",ea},{"it_enable_b",eb},
            {"it_gate_enable",ge},{"it_gate_data",gd},{"it_index",ix},{"it_address",at},{"it_data",value}});
    };
    step(0,0,0,0,0,0,1,6,0);
    step(1,0,1,0,0,0,1,6,79); // Unrelated write still wakes the full/mixed owner.
    inputs.push_back(inputs.back());
    step(0,0,1,0,0,0,1,1,91); step(1,0,1,0,1,1,1,1,91); // NBA samples old data; derived edge sees committed data.
    step(0,0,0,0,1,0,1,1,92); step(1,1,1,1,1,0,1,1,93); // Both clocks: both payloads sampled before large commits.
    step(0,0,0,0,1,0,7,7,0); step(0,1,0,1,0,0,7,7,0);
    step(0,0,0,0,1,0,15,6,0); step(1,0,1,0,1,1,15,6,37); // Invalid indexed read alongside a valid full read.
    step(0,0,0,0,1,0,1,15,0); step(1,1,1,1,1,0,1,15,255); // Invalid writes do not touch state.
    step(0,0,0,0,0,0,1,1,0); step(0,1,0,1,0,0,1,1,0);
    for(const auto budget : {16u,1024u}) for(const std::string bitmap : {"on","off"})
        for(const std::string mode : {"on-write","on-change"}) {
            const auto label="indexed-wake-temporal-"+mode+"-"+bitmap+"-b"+std::to_string(budget);
            writeCase(fixture,reference,inputs,label,budget,4096,
                {{"execution-counters","on"},{"mem-enable-bitmap",bitmap},{"mem-write-activate",mode}});
            require(readFile(artifact(label)/"model/grhsim_top.hpp").find("_last_read_leaf")==std::string::npos,
                    "Indexed temporal fixture retained read subscription storage");
            const auto path=artifact(label)/"driver.cpp";
            auto driver=readFile(path);
            const std::string marker="const auto iwBefore=indexedWakeCounts(model);\n";
            // The first unrelated write changes both full-read observations.
            // The selective memory reader shares its owner with the peer's
            // full reader, so the peer write wakes all four operation sites.
            const auto first=driver.find("model.it_clock_a = 1;");
            require(first!=std::string::npos,"Indexed temporal oracle lost its first edge");
            driver.insert(first,marker);
            const auto eval=driver.find("model.eval();",first+marker.size());
            require(eval!=std::string::npos,"Indexed temporal oracle lost edge evaluation");
            driver.insert(eval+std::string("model.eval();").size(),
                "\n{ const auto counts=indexedWakeCounts(model); for(unsigned i=0;i<4;++i) assert(counts[i]>iwBefore[i]); }\n");
            driver.insert(driver.find("int main()"),indexedWakeSupport);
            std::ofstream(path)<<driver;
            indexedWakeSites(label,{"indexed_wake_full","indexed_wake_mixed","indexed_wake_full_only","indexed_wake_selective"});
            std::ofstream make(artifact("Makefile"),std::ios::app);
            make << "\n.PHONY: " << label << "\nrun: " << label << '\n' << label << ":\n\t$(MAKE) -C " << label << " -f run.mk run\n";
        }
}

void testConditionalPackedScopes() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("clock"), enableA = fixture.input("enable_a"), enableB = fixture.input("enable_b");
    const auto data = graph.createValue(graph.internSymbol("data"), Fixture::type(16, {2}));
    graph.bindInputPort("data", data);
    const auto offsetA = fixture.input("offset_a", 4), offsetB = fixture.input("offset_b", 4), offsetLast = fixture.input("offset_last", 4);
    std::vector<grh::ValueId> payloads;
    for (unsigned i = 0; i < 3; ++i) {
        const auto state = fixture.state("packed_state" + std::to_string(i), 12);
        const auto payload = fixture.compute(OperationKind::kSliceDynamic, Fixture::type(12), {data, i == 1 ? offsetB : i == 2 ? offsetLast : offsetA});
        payloads.push_back(payload);
        fixture.write(OperationKind::kRegisterWritePort, state, {i == 1 ? enableB : enableA, payload, clock});
        graph.bindOutputPort("state" + std::to_string(i), fixture.read(state));
    }
    fixture.convert(); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session);
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(schedule.generalRuns.size() == 1 && schedule.generalRuns.front().orderedWriters.size() == 3,
            "Conditional packing fixture must keep its A/B/A writers in one run");
    require(schedule.sinkGroups.size() == 1 && schedule.sinkGroups.front().guards.empty(),
            "Conditional packing fixture unexpectedly hoisted an enable");
    const auto& runKey = schedule.generalRuns.front().partitionKey;
    for (const auto payload : payloads)
        require(owner(graph, graph.valueDef(payload)) == runKey, "Conditional packed payload is not sink-private");
    for (std::size_t i = 0; i < 3; ++i) {
        const auto writer = schedule.generalRuns.front().orderedWriters[i];
        require(graph.opOperands(writer)[1] == (i == 1 ? enableB : enableA),
                "Conditional packing fixture lost A/B/A enable order");
    }
    const std::string body = R"CPP(
const std::array<std::array<unsigned,3>,12> steps{{
    {0,1,0xabcd}, {1,0,0x1234}, {0,1,0x7654}, {0,0,0xffff},
    {1,1,0xdead}, {0,1,0xbeef}, {1,0,0x1357}, {0,0,0x2468},
    {1,1,0xfedc}, {0,1,0x0123}, {1,0,0x89ab}, {1,1,0x4567}
}};
for (unsigned epoch = 0; epoch < 3; ++epoch) {
    model.init(); std::array<unsigned,3> expected{};
    model.offset_a=0; model.offset_b=4; model.offset_last=2;
    for (const auto& step : steps) {
        model.clock = false; model.enable_a = step[0]; model.enable_b = step[1];
        const unsigned data = step[2] ^ (epoch * 0x1111);
        model.data[0] = data & 255; model.data[1] = data >> 8; model.eval();
        assert(model.state0 == expected[0] && model.state1 == expected[1] && model.state2 == expected[2]);
        model.clock = true; model.eval();
        if (step[0]) { expected[0] = data & 4095; expected[2] = (data >> 2) & 4095; }
        if (step[1]) expected[1] = (data >> 4) & 4095;
        assert(model.state0 == expected[0] && model.state1 == expected[1] && model.state2 == expected[2]);
        model.eval();
        assert(model.state0 == expected[0] && model.state1 == expected[1] && model.state2 == expected[2]);
    }
}
)CPP";
    for (const auto budget : {16, 1024})
        manualCase(fixture, "conditional-packed-scopes-b" + std::to_string(budget), budget, "", body, false);
}

void testGroupedNonsinkPublications() {
    Fixture fixture;
    auto& graph = fixture.graph;
    const auto clock = fixture.input("group_clock");
    const auto head = fixture.input("group_head", 16), middle = fixture.input("group_middle", 16);
    const auto tail = fixture.input("group_tail", 16), other = fixture.input("group_other", 16);
    std::vector<std::pair<grh::OperationId, std::string>> placements;
    const auto place = [&](grh::ValueId value, std::string key) {
        placements.emplace_back(graph.valueDef(value), std::move(key)); return value;
    };
    const auto constant = [&](unsigned width, unsigned value, const std::string& key) {
        return place(fixture.constant(width, std::to_string(width) + "'d" + std::to_string(value)), key);
    };
    const auto binary = [&](OperationKind kind, unsigned width, grh::ValueId a, grh::ValueId b,
                            const std::string& key) {
        return place(fixture.compute(kind, Fixture::type(width), {a, b}), key);
    };
    const auto slice = [&](grh::ValueId value, unsigned width) {
        const auto result = place(fixture.compute(OperationKind::kSliceStatic, Fixture::type(width), {value}), "source");
        graph.setAttr(graph.valueDef(result), "sliceStart", int64_t{0});
        graph.setAttr(graph.valueDef(result), "sliceEnd", int64_t{width - 1}); return result;
    };
    const auto headLow = slice(head, 8), middleLow = slice(middle, 8), tailLow = slice(tail, 8), otherLow = slice(other, 8);
    const auto enabled = slice(other, 1), one = constant(1, 1, "source");
    std::vector<grh::ValueId> firstGroup, secondGroup;
    auto sumA = constant(16, 0, "consumer_a");
    // More than 1024 emitted lines even at the production budget. The first
    // and final snapshots change independently, while the same dirty flag
    // must survive all intervening helpers.
    for (unsigned i = 0; i < 400; ++i) {
        const auto input = i == 0 ? headLow : i == 399 ? tailLow : middleLow;
        const auto value = binary(OperationKind::kXor, 8, input, constant(8, (i * 17 + 3) & 255, "source"), "source");
        firstGroup.push_back(value);
        sumA = binary(OperationKind::kAdd, 16, sumA, value, "consumer_a");
    }
    auto sumB = constant(16, 0, "consumer_b");
    for (unsigned i = 0; i < 4; ++i) {
        const auto value = binary(OperationKind::kAdd, 8, otherLow, constant(8, i * 29 + 7, "source"), "source");
        secondGroup.push_back(value);
        sumB = binary(OperationKind::kAdd, 16, sumB, value, "consumer_b");
    }
    const auto singleton = binary(OperationKind::kXor, 8, headLow, constant(8, 90, "source"), "source");
    const auto payload = binary(OperationKind::kAdd, 8, headLow, constant(8, 7, "source"), "source");
    const auto emptyPayload = binary(OperationKind::kAdd, 8, headLow, constant(8, 19, "source"), "source");

    // These owners each have two matching scalar publications, but one also
    // handles String and the other External. They must retain ordinary
    // publication paths even beside an eligible grouped owner.
    const grh::LogicalType stringType{grh::ValueType::String, 0, false, grh::LogicDomain::TwoState, {}};
    const auto text = place(fixture.compute(OperationKind::kConstant, stringType, {}), "string");
    graph.setAttr(graph.valueDef(text), "constValue", std::string("publication"));
    const auto stringA = binary(OperationKind::kXor, 8, otherLow, constant(8, 51, "string"), "string");
    const auto stringB = binary(OperationKind::kXor, 8, otherLow, constant(8, 119, "string"), "string");
    const auto imported = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("group_publication_effect"));
    graph.setAttr(imported, "argsName", std::vector<std::string>{"input", "text"});
    graph.setAttr(imported, "argsDirection", std::vector<std::string>{"input", "input"});
    graph.setAttr(imported, "argsType", std::vector<std::string>{"logic", "string"});
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>{8, 0});
    graph.setAttr(imported, "argsSigned", std::vector<bool>{false, false});
    graph.setAttr(imported, "hasReturn", true); graph.setAttr(imported, "returnWidth", int64_t{8});
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("group_publication_effect"));
    for (const auto value : {one, headLow, text}) graph.addOperand(call, value);
    const auto sampled = graph.createValue(Fixture::type(8)); graph.addResult(call, sampled); place(sampled, "effect");
    const auto effectA = binary(OperationKind::kXor, 8, sampled, constant(8, 3, "effect"), "effect");
    const auto effectB = binary(OperationKind::kAdd, 8, sampled, constant(8, 5, "effect"), "effect");
    auto sumC = binary(OperationKind::kAdd, 16, singleton, effectA, "consumer_c");
    for (const auto value : {effectB, stringA, stringB})
        sumC = binary(OperationKind::kAdd, 16, sumC, value, "consumer_c");
    graph.setOpSymbol(graph.valueDef(sumA), graph.internSymbol("group_consumer_a"));
    graph.setOpSymbol(graph.valueDef(sumB), graph.internSymbol("group_consumer_b"));
    for (const auto& [name, value] : std::vector<std::pair<std::string, grh::ValueId>>{
            {"group_sum_a", sumA}, {"group_sum_b", sumB}, {"group_sum_c", sumC}}) {
        graph.bindOutputPort(name, value);
        for (unsigned copy = 0; copy < 2; ++copy) {
            const auto state = fixture.state(name + std::to_string(copy), 16);
            fixture.write(OperationKind::kRegisterWritePort, state, {one, value, clock});
            graph.bindOutputPort(name + std::to_string(copy), fixture.read(state));
        }
    }
    for (unsigned copy = 0; copy < 2; ++copy) {
        const auto state = fixture.state("group_small" + std::to_string(copy), 8);
        fixture.write(OperationKind::kRegisterWritePort, state, {enabled, payload, clock});
        graph.bindOutputPort("group_small" + std::to_string(copy), fixture.read(state));
    }
    // Two large consumers keep this value in nonsink, with neither ordinary
    // nonsink fanout nor small-sink activity subscribers.
    for (unsigned copy = 0; copy < 2; ++copy) {
        const auto name = "group_large" + std::to_string(copy);
        const auto state = fixture.state(name,8192,{1024});
        fixture.write(OperationKind::kRegisterFillPort,state,{one,emptyPayload,clock});
        graph.bindOutputPort(name,fixture.read(state,{fixture.constant(10,"0")}));
    }
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    // Fix only the activity partition, after the ordinary extraction has
    // proved these operations belong to nonsink. This makes exact fanout
    // groups independent of the partitioner's cost-model choices.
    for (const auto& [op, key] : placements) {
        const auto path = owner(graph, op);
        require(path.size() == 4 && path[0] == "general" && path[1] == "nonsink",
                "Grouped-publication fixture lost a nonsink operation");
        graph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "group_" + key, "node_" + key});
    }
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=0"});
    const auto fanout = [&](grh::ValueId value) {
        std::set<Owner> result;
        auto source = owner(graph, graph.valueDef(value)); source.resize(3);
        for (const auto& use : graph.valueUsers(value)) {
            auto target = owner(graph, use.operation);
            if (target.size() != 4 || target[1] != "nonsink") continue;
            target.resize(3); if (target != source) result.insert(std::move(target));
        }
        return result;
    };
    const auto fanoutA = fanout(firstGroup.front()), fanoutB = fanout(secondGroup.front());
    require(fanoutA.size() == 1 && fanoutB.size() == 1 && fanoutA != fanoutB,
            "Grouped-publication fixture lost its distinct complete fanouts");
    for (const auto& values : {firstGroup, secondGroup}) for (const auto value : values)
        require(fanout(value) == (values.size() == firstGroup.size() ? fanoutA : fanoutB),
                "Grouped-publication fixture split a complete fanout group");
    require(fanout(payload).empty() && fanout(emptyPayload).empty() && fanout(singleton).size() == 1,
            "Grouped-publication fixture lost empty or singleton fanout");
    require(graph.valueUsers(emptyPayload).size() == 2,
            "Grouped-publication empty payload lost its two large consumers");
    for (const auto& use : graph.valueUsers(emptyPayload))
        require(sim::isWriter(graph.opKind(use.operation)) && owner(graph,use.operation)[1] == "large-sink",
                "Grouped-publication empty payload acquired a sink activity subscriber");
    const std::string support = R"CPP(
extern "C" std::uint8_t group_publication_effect(std::uint8_t input, const char* text) {
    assert(std::string(text) == "publication"); return input + 11;
}
static std::array<unsigned long long,2> groupPublicationCounts(GrhSIM_top& model) {
    auto *stream = std::fopen("group-publication-counts.tsv", "w+"); assert(stream);
    model.dump_execution_counts(stream); std::rewind(stream); char line[256];
    assert(std::fgets(line,sizeof(line),stream) && std::fgets(line,sizeof(line),stream));
    std::array<unsigned long long,2> result{}; std::array<bool,2> found{};
    while (std::fgets(line,sizeof(line),stream)) {
        unsigned site; unsigned long long count;
        if (std::sscanf(line,"%u\t%llu",&site,&count)!=2) continue;
        for (unsigned i=0;i<2;++i) if (site==groupPublicationSites[i]) { result[i]=count; found[i]=true; }
    }
    std::fclose(stream); assert(found[0] && found[1]); return result;
}
)CPP";
    const std::string body = R"CPP(
const std::array<std::array<unsigned,4>,14> steps{{
    {0,0,0,0}, {256,256,256,256}, {257,256,256,256}, {513,512,512,512},
    {513,512,519,512}, {769,768,775,768}, {769,781,775,768}, {1025,1037,1031,1024},
    {1025,1037,1031,1025}, {1281,1293,1287,1281}, {1287,1293,1281,1281},
    {1543,1549,1537,1537}, {1543,1549,1537,1538}, {1799,1805,1793,1794}
}};
for (unsigned epoch=0;epoch<3;++epoch) {
    model.init(); bool initialized=false; std::array<unsigned,4> previous{}; unsigned small=0,large=0;
    for (const auto& step : steps) {
        model.group_clock=false; model.group_head=step[0]; model.group_middle=step[1];
        model.group_tail=step[2]; model.group_other=step[3];
        std::array<unsigned,4> low{}; for (unsigned i=0;i<4;++i) low[i]=step[i]&255;
        unsigned a=0,b=0;
        for (unsigned i=0;i<400;++i) a=(a+((i==0?low[0]:i==399?low[2]:low[1])^((i*17+3)&255)))&65535;
        for (unsigned i=0;i<4;++i) b+=((low[3]+i*29+7)&255);
        const unsigned effect=(low[0]+11)&255;
        const unsigned c=(low[0]^90)+(effect^3)+((effect+5)&255)+(low[3]^51)+(low[3]^119);
        const auto before=groupPublicationCounts(model); model.eval(); const auto after=groupPublicationCounts(model);
        const bool changedA=!initialized || low[0]!=previous[0] || low[1]!=previous[1] || low[2]!=previous[2];
        const bool changedB=!initialized || low[3]!=previous[3];
        assert(after[0]==before[0]+changedA && after[1]==before[1]+changedB);
        assert(model.group_sum_a==a && model.group_sum_b==b && model.group_sum_c==c);
        assert(model.group_small0==small && model.group_small1==small);
        assert(model.group_large0==large && model.group_large1==large);
        // A clean call following a dirty call must not reuse the old flag.
        model.eval(); assert(groupPublicationCounts(model)==after);
        model.group_clock=true; model.eval();
        if (low[3]&1) small=(low[0]+7)&255;
        large=(low[0]+19)&255;
        assert(model.group_small0==small && model.group_small1==small);
        assert(model.group_large0==large && model.group_large1==large);
        assert(model.group_sum_a0==a && model.group_sum_a1==a);
        assert(model.group_sum_b0==b && model.group_sum_b1==b);
        assert(model.group_sum_c0==c && model.group_sum_c1==c);
        model.eval(); assert(groupPublicationCounts(model)==after);
        initialized=true; previous=low;
    }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "grouped-publications-b" + std::to_string(budget);
        // The site declaration precedes the helper that reads it.
        manualCase(fixture, label, budget, "static const std::array<unsigned,2> groupPublicationSites{};\n" + support,
                   body, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        const auto functions = mappedRecords(readFile(directory / "code-map.json"), "functions");
        const auto location = [&](grh::OperationId op) {
            const auto id = "\"o" + std::to_string(op.index) + "\"";
            std::pair<std::string,std::string> result;
            for (const auto& function : functions) if (function.find(id)!=std::string::npos) {
                std::smatch match;
                require(std::regex_search(function,match,std::regex(R"REGEX("name":"([^"]+)")REGEX")), "Function lacks name");
                result.first=match[1];
                require(std::regex_search(function,match,std::regex(R"REGEX("root_function":"([^"]+)")REGEX")), "Function lacks root");
                result.second=match[1]; return result;
            }
            throw std::runtime_error("Grouped-publication operation has no generated function");
        };
        const auto first = location(graph.valueDef(firstGroup.front()));
        const auto last = location(graph.valueDef(firstGroup.back()));
        require(first.second==last.second && first.first!=last.first && first.second.starts_with("sn_"),
                "Grouped publications did not cross helpers at the requested budget");
        const auto consumerA = location(graph.valueDef(sumA)).second, consumerB = location(graph.valueDef(sumB)).second;
        const auto ordinalA = std::stoul(consumerA.substr(3)), ordinalB = std::stoul(consumerB.substr(3));
        require(ordinalA!=ordinalB && ordinalA/8==ordinalB/8, "Grouped fanouts did not occupy distinct masks of one activity byte");
        const auto header = readFile(directory / "grhsim_top.hpp");
        const auto frame = header.find("struct chunkFrame_" + first.second);
        const auto end = frame==std::string::npos ? frame : header.find("    };",frame);
        require(frame!=std::string::npos && end!=std::string::npos &&
                    header.substr(frame,end-frame).find("publication_changed")!=std::string::npos,
                "Grouped-publication fixture did not retain a dirty flag across helper frames");
        const auto source = generatedCpp(directory);
        for (const auto& [value, store] : std::vector<std::pair<grh::ValueId,std::string>>{
                 {enabled,"smallSinkEnableBoundaryStore"}, {singleton,"nonSinkBoundaryStore"}})
            require(source.find("if ("+store+"."+mappedValue(directory,value)+" != ")!=std::string::npos,
                    "Packed enable or singleton publication lost its ordinary change guard");
        const auto payloadGuard = source.find("if (smallSinkPayloadBoundaryStore."+mappedValue(directory,payload)+" != ");
        require(payloadGuard!=std::string::npos &&
                    source.substr(payloadGuard,source.find('}',payloadGuard)-payloadGuard).find("sinkDirty[")!=std::string::npos,
                "Empty ordinary fanout lost the comparison required by its sink subscribers");
        const auto emptyBoundary = "nonSinkBoundaryStore."+mappedValue(directory,emptyPayload);
        require(source.find(emptyBoundary+" = ")!=std::string::npos &&
                    source.find("if ("+emptyBoundary+" != ")==std::string::npos,
                "Publication without ordinary or sink subscribers retained a redundant comparison");
        for (const auto op : {call, graph.valueDef(text)}) {
            const auto root = location(op).second;
            for (const auto& function : functions) {
                if (function.find("\"root_function\":\""+root+"\"")==std::string::npos) continue;
                std::smatch match;
                require(std::regex_search(function,match,std::regex(R"REGEX("name":"([^"]+)")REGEX")), "Fallback function lacks name");
                const auto name=match[1].str();
                require(std::regex_search(function,match,std::regex(R"REGEX("source":"([^"]+)")REGEX")), "Fallback function lacks source");
                const auto cpp=readFile(directory/match[1].str());
                const auto begin=cpp.find("GrhSIM_top::"+name+"(");
                const auto end=begin==std::string::npos ? begin : cpp.find("\n}\n",begin);
                require(begin!=std::string::npos && end!=std::string::npos, "Fallback function body was not found");
                require(cpp.substr(begin,end-begin).find("publication_changed")==std::string::npos,
                        "External/String owner entered grouped publication");
            }
        }
        const auto manifest = readFile(directory / "manifest.json");
        const std::regex sites(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
        std::map<std::string,unsigned> ordinals; unsigned ordinal=0;
        for (std::sregex_iterator it(manifest.begin(),manifest.end(),sites),stop;it!=stop;++it,++ordinal)
            if ((*it)[2]=="operation") ordinals.emplace((*it)[1],ordinal);
        const auto path = artifact(label)/"driver.cpp"; auto driver=readFile(path);
        const std::string placeholder="static const std::array<unsigned,2> groupPublicationSites{};";
        const auto begin=driver.find(placeholder); require(begin!=std::string::npos,"Missing grouped counter site declaration");
        driver.replace(begin,placeholder.size(),"static const std::array<unsigned,2> groupPublicationSites{"+
            std::to_string(ordinals.at("group_consumer_a"))+","+std::to_string(ordinals.at("group_consumer_b"))+"};");
        std::ofstream(path)<<driver;
    }
}

void testCompletePublicationKeys() {
    Fixture fixture;
    auto& graph = fixture.graph;
    std::vector<std::pair<grh::OperationId,std::string>> placements;
    const auto place = [&](grh::ValueId value, const std::string& key) {
        placements.emplace_back(graph.valueDef(value),key); return value;
    };
    const auto constant = [&](unsigned value) {
        return place(fixture.constant(8,"8'd"+std::to_string(value)),"source");
    };
    std::array<std::array<grh::ValueId,2>,3> pairs;
    for (unsigned group=0;group<3;++group) {
        const auto input=fixture.input("complete_data_"+std::to_string(group),8);
        for (unsigned i=0;i<2;++i)
            pairs[group][i]=place(fixture.compute(OperationKind::kAdd,Fixture::type(8),
                {input,constant(4*group+2*i+1)}),"source");
    }
    const auto addConsumer = [&](grh::ValueId a,grh::ValueId b) {
        return place(fixture.compute(OperationKind::kAdd,Fixture::type(16),{a,b}),"consumer");
    };
    const auto consumer=addConsumer(addConsumer(pairs[0][0],pairs[0][1]),
                                    addConsumer(pairs[1][0],pairs[1][1]));
    graph.bindOutputPort("complete_sum",consumer);
    std::vector<grh::OperationId> writers;
    const auto writePair = [&](unsigned group,unsigned clock) {
        const auto data=fixture.compute(OperationKind::kAdd,Fixture::type(8),{pairs[group][0],pairs[group][1]});
        const auto name="complete_q_"+std::to_string(writers.size());
        const auto state=fixture.state(name,8);
        const auto writer=fixture.write(OperationKind::kRegisterWritePort,state,
            {fixture.constant(1,"1'b1"),data,fixture.input("complete_clock_"+std::to_string(clock))});
        graph.bindOutputPort(name,fixture.read(state)); writers.push_back(writer);
    };
    writePair(0,0); writePair(1,1); writePair(2,2); writePair(2,3);
    const auto observedClock=fixture.input("complete_clock_4");
    for (unsigned i=0;i<2;++i) {
        const auto state=fixture.state("complete_observed_"+std::to_string(i),16);
        fixture.write(OperationKind::kRegisterWritePort,state,{fixture.constant(1,"1'b1"),consumer,observedClock});
        graph.bindOutputPort("complete_observed_"+std::to_string(i),fixture.read(state));
    }
    run(fixture.design,"grh-to-grhsim",fixture.session); fixture.split();
    run(fixture.design,"grhsim-partition-nonsink",fixture.session,{"-clone-shared=0"});
    for (const auto& [op,key] : placements) {
        const auto path=owner(graph,op);
        require(path.size()==4 && path[0]=="general" && path[1]=="nonsink",
                "Complete-key fixture lost its original nonsink publication");
        graph.setAttr(op,sim::kPartition,Owner{"general","nonsink","complete_"+key,"node_"+key});
    }
    run(fixture.design,"grhsim-partition-sink",fixture.session,{"-enable-guard-min-size=0"});
    const auto& schedule=sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==5,"Complete-key fixture lost its five exact clock runs");
    const auto nonsinkFanout = [&](grh::ValueId value) {
        std::set<Owner> result;
        auto source=owner(graph,graph.valueDef(value)); source.resize(3);
        for (const auto& use : graph.valueUsers(value)) {
            auto target=owner(graph,use.operation);
            if (target.size()!=4 || target[1]!="nonsink") continue;
            target.resize(3); if (target!=source) result.insert(std::move(target));
        }
        return result;
    };
    const auto ordinary=nonsinkFanout(pairs[0][0]);
    require(ordinary.size()==1 && nonsinkFanout(pairs[0][1])==ordinary &&
                nonsinkFanout(pairs[1][0])==ordinary && nonsinkFanout(pairs[1][1])==ordinary &&
                nonsinkFanout(pairs[2][0]).empty() && nonsinkFanout(pairs[2][1]).empty(),
            "Complete-key fixture lost equal NS fanouts or empty-NS sink publications");
    std::array<std::uint64_t,3> sinkMasks{};
    std::vector<std::pair<grh::OperationId,std::string>> sites;
    for (unsigned i=0;i<writers.size();++i) {
        std::optional<std::size_t> runOrdinal;
        for (std::size_t run=0;run<schedule.generalRuns.size();++run)
            if (std::find(schedule.generalRuns[run].orderedWriters.begin(),schedule.generalRuns[run].orderedWriters.end(),writers[i])!=
                schedule.generalRuns[run].orderedWriters.end()) runOrdinal=run;
        require(runOrdinal.has_value(),"Complete-key writer lost its exact run");
        sinkMasks[i<2 ? i : 2] |= std::uint64_t{1} << *runOrdinal;
        const auto payload=graph.valueDef(graph.opOperands(writers[i])[2]);
        require(graph.opKind(payload)==OperationKind::kAdd && owner(graph,payload)==schedule.generalRuns[*runOrdinal].partitionKey,
                "Complete-key fixture lost its private counted payload");
        sites.emplace_back(writers[i],"writer-sample"); sites.emplace_back(payload,"operation");
    }
    require(sinkMasks[0]!=sinkMasks[1] && (sinkMasks[0]&sinkMasks[1])==0 &&
                (sinkMasks[2]&(sinkMasks[0]|sinkMasks[1]))==0,
            "Equal NS fanouts no longer have distinct sink destinations");
    sites.emplace_back(graph.valueDef(consumer),"operation");
    const std::string body=R"CPP(
for(unsigned epoch=0;epoch<2;++epoch) {
    model.init(); bool initialized=false; std::array<unsigned,3> previous{};
    std::array<unsigned,4> held{};
    for(const auto data : std::array<std::array<unsigned,3>,6>{{{0,0,0},{0,0,0},{1,0,0},{1,2,0},{1,2,3},{4,5,3}}}) {
        model.complete_clock_0=false; model.complete_clock_1=false; model.complete_clock_2=false;
        model.complete_clock_3=false; model.complete_clock_4=false;
        model.complete_data_0=data[0]; model.complete_data_1=data[1]; model.complete_data_2=data[2];
        const auto before=batchCounts(model); model.eval(); const auto low=batchCounts(model);
        const bool changedA=!initialized || data[0]!=previous[0];
        const bool changedB=!initialized || data[1]!=previous[1];
        const bool changedC=!initialized || data[2]!=previous[2];
        for(unsigned i=0;i<8;++i) assert(low[i]==before[i]);
        assert(low[8]==before[8]+(changedA||changedB));
        assert(model.complete_sum==2*data[0]+2*data[1]+16);
        assert(model.complete_q_0==held[0] && model.complete_q_1==held[1]);
        assert(model.complete_q_2==held[2] && model.complete_q_3==held[3]);
        model.eval(); assert(batchCounts(model)==low);
        model.complete_clock_0=true; model.complete_clock_1=true; model.complete_clock_2=true;
        model.complete_clock_3=true; model.complete_clock_4=true; model.eval();
        const auto high=batchCounts(model);
        for(unsigned i=0;i<4;++i) {
            const bool changed=i==0 ? changedA : i==1 ? changedB : changedC;
            assert(high[2*i]==low[2*i]+changed && high[2*i+1]==low[2*i+1]+changed);
        }
        assert(high[8]==low[8]);
        held={2*data[0]+4,2*data[1]+12,2*data[2]+20,2*data[2]+20};
        assert(model.complete_q_0==held[0] && model.complete_q_1==held[1]);
        assert(model.complete_q_2==held[2] && model.complete_q_3==held[3]);
        assert(model.complete_observed_0==model.complete_sum && model.complete_observed_1==model.complete_sum);
        model.eval(); assert(batchCounts(model)==high); initialized=true; previous=data;
    }
}
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label="complete-publication-keys-b"+std::to_string(budget);
        manualCase(fixture,label,budget,batchActivityCounterSupport(sites.size()),body,false,{{"execution-counters","on"}});
        const auto directory=artifact(label)/"model";
        require(batchActivityStatistic(directory,"eligible_runs")==5 && batchActivityStatistic(directory,"rejected_runs")==0 &&
                    batchActivityStatistic(directory,"publication_aggregation_groups")==3 &&
                    batchActivityStatistic(directory,"publication_aggregation_sites")==6 &&
                    batchActivityStatistic(directory,"publication_aggregation_sink_groups")==3 &&
                    batchActivityStatistic(directory,"publication_aggregation_sink_sites")==6 &&
                    batchActivityStatistic(directory,"publication_change_mask_sites_before_grouping")==7 &&
                    batchActivityStatistic(directory,"publication_change_mask_sites")==4,
                "Complete-key aggregation lost exact groups, sites or single-flush mask savings");
        const auto sourceRoot=batchActivityRoot(directory,graph.valueDef(pairs[0][0]));
        const auto cpp=batchActivityRootCpp(directory,sourceRoot);
        const auto map=readFile(directory/"code-map.json");
        const auto primary = [&](grh::ValueId value) {
            const auto prefix="{\"value_id\":\"v"+std::to_string(value.index)+"\",";
            for (const auto& record : mappedRecords(map,"values")) if (record.starts_with(prefix))
                for (const auto& storage : mappedRecords(record,"storage")) {
                    std::smatch match;
                    if (storage.find("\"role\":\"primary\"")!=std::string::npos &&
                        std::regex_search(storage,match,std::regex(R"REGEX("reference":"([^"]+)")REGEX"))) return match[1].str();
                }
            throw std::runtime_error("Complete-key source lost its primary snapshot");
        };
        const auto flagFor = [&](grh::ValueId value) {
            require(batchActivityRoot(directory,graph.valueDef(value))==sourceRoot,
                    "Complete-key source publications no longer share one owner");
            const auto term=" |= "+primary(value)+" != ";
            const auto position=cpp.find(term);
            require(position!=std::string::npos && cpp.find(term,position+term.size())==std::string::npos,
                    "Complete-key publication lacks exactly one original-position comparison");
            auto begin=cpp.rfind('\n',position); begin=begin==std::string::npos ? 0 : begin+1;
            while (begin<position && (cpp[begin]==' ' || cpp[begin]=='\t')) ++begin;
            return cpp.substr(begin,position-begin);
        };
        std::array<std::string,3> flags;
        for (unsigned group=0;group<3;++group) {
            flags[group]=flagFor(pairs[group][0]);
            require(!flags[group].empty() && flags[group]==flagFor(pairs[group][1]),
                    "Equal complete destinations did not share one dirty flag");
        }
        require(flags[0]!=flags[1] && flags[0]!=flags[2] && flags[1]!=flags[2],
                "Different sink destinations were merged by their equal NS fanout");
        const auto consumerRoot=batchActivityRoot(directory,graph.valueDef(consumer));
        require(consumerRoot.starts_with("sn_"),"Complete-key consumer lost its nonsink activity bit");
        const auto ordinal=std::stoul(consumerRoot.substr(3));
        const auto activation="activeCurrent["+std::to_string(ordinal/8)+"] |= UINT8_C("+
            std::to_string(std::uint32_t{1}<<(ordinal%8))+");";
        for (unsigned group=0;group<3;++group) {
            const auto guard="if ("+flags[group]+")";
            const auto begin=cpp.find(guard);
            const auto end=begin==std::string::npos ? begin : cpp.find('}',begin);
            require(begin!=std::string::npos && end!=std::string::npos && cpp.find(guard,begin+guard.size())==std::string::npos,
                    "Complete-key group did not have exactly one activation flush");
            const auto flush=cpp.substr(begin,end-begin);
            require(flush.find("sinkDirty[0] |= UINT64_C("+std::to_string(sinkMasks[group])+");")!=std::string::npos &&
                        (group==2 ? flush.find("activeCurrent[")==std::string::npos : flush.find(activation)!=std::string::npos),
                    "Complete-key flush lost its exact NS/sink destination set");
        }
        bindBatchActivityCounters(label,graph,sites);
    }
}

void testImmutableConstantSources() {
    const auto valueRecord = [](const std::filesystem::path& directory, grh::ValueId value) {
        const auto prefix = "{\"value_id\":\"v" + std::to_string(value.index) + "\",";
        for (const auto& record : mappedRecords(readFile(directory / "code-map.json"), "values"))
            if (record.starts_with(prefix)) return record;
        throw std::runtime_error("Immutable-source value is absent from code map");
    };
    const auto primary = [&](const std::filesystem::path& directory, grh::ValueId value) {
        for (const auto& record : mappedRecords(valueRecord(directory, value), "storage")) {
            std::smatch match;
            if (record.find("\"role\":\"primary\"") != std::string::npos &&
                std::regex_search(record, match, std::regex(R"REGEX("reference":"([^"]+)")REGEX")))
                return match[1].str();
        }
        return std::string{};
    };
    const auto operationBlock = [](const std::string& cpp, grh::OperationId op) {
        const auto marker = "/* op o" + std::to_string(op.index) + ' ';
        std::istringstream input(cpp); std::string line, result; bool collecting = false;
        while (std::getline(input, line)) {
            if (line.find("++opExecCounts_[") != std::string::npos && line.find("/* op o") != std::string::npos) {
                if (collecting) break;
                collecting = line.find(marker) != std::string::npos;
            }
            if (collecting) { result += line + '\n'; if (line == "}") break; }
        }
        require(!result.empty(), "Immutable-source operation lost its counter/provenance site");
        return result;
    };
    const auto rootFunction = [](const std::filesystem::path& directory, grh::OperationId op) {
        const auto id = "\"o" + std::to_string(op.index) + "\"";
        for (const auto& record : mappedRecords(readFile(directory / "code-map.json"), "functions")) {
            if (record.find(id) == std::string::npos) continue;
            std::smatch match;
            require(std::regex_search(record, match, std::regex(R"REGEX("root_function":"([^"]+)")REGEX")),
                    "Immutable-source function omitted its root");
            return match[1].str();
        }
        throw std::runtime_error("Immutable-source operation lost function provenance");
    };

    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("immutable_clock"), enable = fixture.input("immutable_enable");
    const auto marker = fixture.input("immutable_marker", 8);
    std::vector<std::pair<grh::OperationId, std::string>> placements;
    const auto place = [&](grh::ValueId value, const std::string& key) {
        placements.emplace_back(graph.valueDef(value), key); return value;
    };
    const auto one = place(fixture.constant(1, "1'b1"), "root");
    struct NumericCase {
        unsigned width; bool sign; std::string text, bits;
        grh::ValueId constant{}, same{}, cross{};
    };
    std::vector<NumericCase> cases{
        {1, false, "1'b1", "1"},
        {7, true, "7'sh7d", "125"},
        {9, false, "9'h1a5", "421"},
        {31, true, "31'sh7ffffffd", "UINT64_C(0x7ffffffd)"},
        {64, false, "64'h8000000000000005", "UINT64_C(0x8000000000000005)"},
        {64, true, "64'shfffffffffffffffd", "UINT64_C(0xfffffffffffffffd)"},
        {65, false, "65'h10000000000000003", "((ImmutableWide(1)<<64)|3)"},
        {130, true, "130'sh3fffffffffffffffffffffffffffffffd", "((ImmutableWide(1)<<130)-3)"}
    };
    std::string support = "using ImmutableWide = unsigned _BitInt(192);\n";
    std::string prepare, verify;
    for (auto& item : cases) {
        const auto suffix = std::string(item.sign ? "s" : "u") + std::to_string(item.width);
        auto type = Fixture::type(item.width); type.isSigned = item.sign;
        const auto input = graph.createValue(graph.internSymbol("immutable_input_" + suffix), type);
        graph.bindInputPort("immutable_input_" + suffix, input);
        item.constant = place(fixture.compute(OperationKind::kConstant, type, {}), "source_" + suffix);
        graph.setAttr(graph.valueDef(item.constant), "constValue", item.text);
        graph.setOpSymbol(graph.valueDef(item.constant), graph.internSymbol("immutable_constant_" + suffix));
        item.same = place(fixture.compute(OperationKind::kAdd, type, {input, item.constant}), "source_" + suffix);
        item.cross = place(fixture.compute(OperationKind::kXor, type, {input, item.constant}), "consumer_" + suffix);
        const auto target = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("immutable_numeric_" + suffix));
        graph.setAttr(target, "argsName", std::vector<std::string>{"input", "sum", "cross"});
        graph.setAttr(target, "argsDirection", std::vector<std::string>(3, "input"));
        graph.setAttr(target, "argsWidth", std::vector<int64_t>(3, item.width));
        graph.setAttr(target, "argsSigned", std::vector<bool>(3, item.sign));
        graph.setAttr(target, "hasReturn", true); graph.setAttr(target, "returnWidth", int64_t{8});
        const auto call = graph.createOperation(OperationKind::kDpicCall);
        graph.setAttr(call, "targetImportSymbol", "immutable_numeric_" + suffix);
        for (const auto value : {one, input, item.same, item.cross}) graph.addOperand(call, value);
        const auto result = graph.createValue(Fixture::type(8)); graph.addResult(call, result); place(result, "audit_" + suffix);
        graph.bindOutputPort("immutable_result_" + suffix, result);
        const auto scalar = item.width == 1 ? std::string("std::uint8_t") : item.width > 64 ?
            "const unsigned _BitInt(" + std::to_string((item.width + 63) / 64 * 64) + ")&" :
            std::string(item.sign ? "std::int" : "std::uint") +
                std::to_string(item.width <= 8 ? 8 : item.width <= 16 ? 16 : item.width <= 32 ? 32 : 64) + "_t";
        support += "extern \"C\" std::uint8_t immutable_numeric_" + suffix + "(" + scalar + " x," + scalar +
            " sum," + scalar + " cross) { const ImmutableWide mask=(ImmutableWide(1)<<" + std::to_string(item.width) +
            ")-1, c=" + item.bits + "; assert((ImmutableWide(sum)&mask)==((ImmutableWide(x)+c)&mask)); "
            "assert((ImmutableWide(cross)&mask)==((ImmutableWide(x)^c)&mask)); return static_cast<std::uint8_t>(sum); }\n";
        const auto port = "m.immutable_input_" + suffix;
        prepare += port + "=static_cast<decltype(" + port + ")>((ImmutableWide(step*17+3)<<" +
            std::to_string(item.width > 64 ? 60 : 0) + ")|step);\n";
        verify += "assert(m.immutable_result_" + suffix + "==static_cast<std::uint8_t>((ImmutableWide(" + port + ")+" + item.bits + ")&((ImmutableWide(1)<<" + std::to_string(item.width) + ")-1)));\n";
    }
    // Byte-contiguous array assignment copies from the physical scalar by
    // address; its immutable RHS shortcut must not feed memcpy an rvalue.
    const auto copiedArray = place(fixture.compute(OperationKind::kAssign, Fixture::type(64, {8}),
        {cases[4].constant}), "array_copy");
    const auto copiedScalar = place(fixture.compute(OperationKind::kAssign, Fixture::type(64),
        {copiedArray}), "array_copy");
    const grh::LogicalType stringType{grh::ValueType::String, 0, false, grh::LogicDomain::TwoState, {}};
    const auto textConstant = [&](const std::string& text) {
        const auto value = place(fixture.compute(OperationKind::kConstant, stringType, {}), "strings");
        graph.setAttr(graph.valueDef(value), "constValue", text); return value;
    };
    const auto localText = textConstant(std::string(160, 'L') + "/\"\\\n\t@\xc3\xa9");
    const auto storedText = textConstant(std::string(180, 'S') + "/stored");
    const auto nulText = textConstant(std::string("A\0B", 3));
    const auto format = textConstant("immutable %s %s %s %d %h %h packed=%h");
    auto packedType = Fixture::type(72, {2, 3, 4}); packedType.isSigned = true;
    const auto packedConstant = place(fixture.compute(OperationKind::kConstant, packedType, {}), "strings");
    graph.setAttr(graph.valueDef(packedConstant), "constValue", std::string("72'hd792e4a3b65c1987af"));
    graph.setValueSymbol(storedText, graph.internSymbol("immutable_stored_text"));
    graph.addDeclaredSymbol(graph.valueSymbol(storedText));
    const auto imported = [&](const std::string& name, const std::vector<std::string>& kinds,
                              const std::vector<int64_t>& widths) {
        const auto target = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
        std::vector<std::string> names;
        for (std::size_t i = 0; i < widths.size(); ++i) names.push_back("arg" + std::to_string(i));
        graph.setAttr(target, "argsName", names); graph.setAttr(target, "argsType", kinds);
        graph.setAttr(target, "argsDirection", std::vector<std::string>(widths.size(), "input"));
        graph.setAttr(target, "argsWidth", widths); graph.setAttr(target, "argsSigned", std::vector<bool>(widths.size(), false));
        graph.setAttr(target, "hasReturn", true); graph.setAttr(target, "returnWidth", int64_t{8});
        return target;
    };
    const auto stringsTarget = imported("immutable_strings", {"string", "string", "string", "logic"}, {0, 0, 0, 8});
    const auto stringsCall = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(stringsCall, "targetImportSymbol", std::string(graph.getOperation(stringsTarget).symbolText()));
    for (const auto value : {one, localText, storedText, nulText, marker}) graph.addOperand(stringsCall, value);
    const auto stringsResult = graph.createValue(Fixture::type(8)); graph.addResult(stringsCall, stringsResult); place(stringsResult, "strings");
    graph.bindOutputPort("immutable_strings_result", stringsResult);
    // Keep effects assigned to this owner contiguous in effect order. The
    // crossing stored-string call must follow both same-owner consumers.
    const auto task = fixture.task("display", {enable, format, localText, storedText, nulText,
        cases[1].constant, cases[4].constant, cases[7].constant, packedConstant, clock}, {"posedge"});
    placements.emplace_back(task, "strings");
    const auto storedTarget = imported("immutable_stored", {"string"}, {0});
    const auto storedCall = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(storedCall, "targetImportSymbol", std::string(graph.getOperation(storedTarget).symbolText()));
    for (const auto value : {one, storedText}) graph.addOperand(storedCall, value);
    const auto storedResult = graph.createValue(Fixture::type(8)); graph.addResult(storedCall, storedResult); place(storedResult, "strings_consumer");
    graph.bindOutputPort("immutable_stored_result", storedResult);
    const auto copiedTarget = imported("immutable_copied_constant", {"logic"}, {64});
    const auto copiedCall = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(copiedCall, "targetImportSymbol", std::string(graph.getOperation(copiedTarget).symbolText()));
    for (const auto value : {one, copiedScalar}) graph.addOperand(copiedCall, value);
    const auto copiedResult = graph.createValue(Fixture::type(8)); graph.addResult(copiedCall, copiedResult); place(copiedResult, "array_copy_audit");
    graph.bindOutputPort("immutable_copied_result", copiedResult);
    support += R"CPP(
extern "C" std::uint8_t immutable_strings(const char* local, const char* stored, const char* nul, std::uint8_t marker) {
    assert(std::string(local)==std::string(160,'L')+"/\"\\\n\t@\xc3\xa9");
    assert(std::string(stored)==std::string(180,'S')+"/stored");
    assert(std::string(nul)=="A"); return marker+3;
}
extern "C" std::uint8_t immutable_stored(const char* text) {
    assert(std::string(text)==std::string(180,'S')+"/stored"); return std::string(text).size();
}
extern "C" std::uint8_t immutable_copied_constant(std::uint64_t value) {
    assert(value==UINT64_C(0x8000000000000005)); return 167;
}
static std::array<unsigned long long,2> immutableCounts(GrhSIM_top& model) {
    auto* stream=std::fopen("immutable-counts.tsv","w+"); assert(stream);
    model.dump_execution_counts(stream); std::rewind(stream); char line[256];
    std::array<unsigned long long,2> result{}; std::array<bool,2> found{};
    while(std::fgets(line,sizeof(line),stream)) {
        unsigned site; unsigned long long count;
        if(std::sscanf(line,"%u\t%llu",&site,&count)!=2) continue;
        for(unsigned i=0;i<2;++i) if(site==immutableSites[i]) { result[i]=count; found[i]=true; }
    }
    std::fclose(stream); assert(found[0] && found[1]); return result;
}
)CPP";
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    for (const auto& [op, key] : placements) {
        const auto path = owner(graph, op);
        require(path.size() == 4 && path[0] == "general" && path[1] == "nonsink",
                "Immutable-source fixture lost an original General nonsink operation");
        graph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "immutable_" + key, "node_" + key});
    }
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const std::string body = "GrhSIM_top peer;\nauto exercise=[&](GrhSIM_top& m,unsigned step) {\n" + prepare +
        "m.immutable_marker=step*13; m.immutable_enable=true; m.immutable_clock=false; m.eval();\n" + verify +
        "assert(m.immutable_strings_result==static_cast<std::uint8_t>(m.immutable_marker+3)); "
        "assert(m.immutable_stored_result==187 && m.immutable_copied_result==167); "
        "auto counts=immutableCounts(m); assert(counts[0]>0 && counts[0]==counts[1]);\n"
        "std::ostringstream captured; auto* previous=std::cout.rdbuf(captured.rdbuf()); "
        "m.immutable_clock=true; m.eval(); std::cout.rdbuf(previous); "
        "assert(captured.str().ends_with(\" packed=d792e4a3b65c1987af\\n\")); std::cout<<captured.str(); "
        "assert(immutableCounts(m)[0]==immutableCounts(m)[1]);\n};\n"
        "for(unsigned epoch=0;epoch<3;++epoch) { model.init(); peer.init(); "
        "assert((immutableCounts(model)==std::array<unsigned long long,2>{})); "
        "assert((immutableCounts(peer)==std::array<unsigned long long,2>{})); "
        "for(unsigned step=0;step<8;++step) { exercise(model,step+epoch*11); exercise(peer,step+epoch*11+29); } }\n";
    for (const auto budget : {16, 1024}) {
        const auto label = "immutable-sources-b" + std::to_string(budget);
        manualCase(fixture, label, budget, "#include <iostream>\n#include <sstream>\nstatic const std::array<unsigned,2> immutableSites{};\n" + support,
                   body, true, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        const auto cpp = generatedCpp(directory);
        for (const auto& item : cases) {
            const auto original = graph.valueDef(item.constant);
            const auto store = primary(directory, item.constant);
            require(!store.empty(), "Immutable scalar lost its original physical store");
            const auto publication = operationBlock(cpp, original);
            const auto same = operationBlock(cpp, graph.valueDef(item.same));
            const auto cross = operationBlock(cpp, graph.valueDef(item.cross));
            const auto sourceRoot = rootFunction(directory, original);
            require(sourceRoot.starts_with("sn_") && sourceRoot == rootFunction(directory, graph.valueDef(item.same)) &&
                        sourceRoot != rootFunction(directory, graph.valueDef(item.cross)),
                    "Immutable scalar did not cover same-owner and crossing General RHS uses");
            // Small budgets can split one operation after its counter. The
            // runtime cases cover those frames; inspect RHS spelling in the
            // unsplit owner so a neighbouring publication cannot mask it.
            if (budget == 1024) {
                require(publication.find(store) != std::string::npos, "Immutable scalar lost its original publication");
                if (item.width <= 64) require(same.find(store) == std::string::npos && cross.find(store) == std::string::npos,
                        "Immutable scalar RHS still loads its resettable boundary store");
                else require(cross.find(store) != std::string::npos, "Wide scalar unexpectedly used the narrow immutable-source path");
            }
        }
        require(primary(directory, localText).empty() && !primary(directory, storedText).empty(),
                "Immutable string fixture lost its no-store/stored cases");
        for (const auto value : {localText, storedText, nulText, format}) {
            const auto helper = "constant_" + mappedValue(directory, value);
            const auto block = operationBlock(cpp, graph.valueDef(value));
            require(budget != 1024 || block.find(helper + "()") != std::string::npos,
                    "Immutable string helper lost its original operation/counter site");
            require(cpp.find("const std::string& GrhSIM_top::" + helper + "()") != std::string::npos,
                    "Immutable string helper does not return an addressable const reference");
        }
        const auto taskBody = operationBlock(cpp, task);
        require(budget != 1024 || taskBody.find("TaskArg{'s', &constant_" + mappedValue(directory, localText) + "()") != std::string::npos,
                "Task string argument lost its addressable immutable source");
        require(budget != 1024 || taskBody.find(primary(directory, cases[1].constant)) != std::string::npos,
                "Task numeric argument was replaced with an unaddressable literal");
        if (budget == 1024) {
            const auto source = primary(directory, cases[4].constant);
            const auto copyBody = operationBlock(cpp, graph.valueDef(copiedArray));
            require(!source.empty() && copyBody.find("std::memcpy(") != std::string::npos &&
                        copyBody.find(", &(" + source + "), 8);") != std::string::npos,
                    "Scalar-to-array assignment lost the physical memcpy source");
            std::smatch packedArgument;
            require(std::regex_search(taskBody, packedArgument,
                        std::regex(R"REGEX(TaskArg\{'l', &([^,]+), sizeof\(\1\), 72, true\})REGEX")) &&
                        packedArgument[1].str().find(mappedPackedPrefix(directory, packedConstant)) != std::string::npos,
                    "Task shaped constant lost its addressable packed bit image");
        }
        const auto manifest = readFile(directory / "manifest.json");
        const std::regex sites(R"REGEX(\{"operation":"([^"]+)","category":"([^"]+)","function":"[^"]+"\})REGEX");
        std::map<std::string,unsigned> ordinals; unsigned ordinal = 0;
        for (std::sregex_iterator it(manifest.begin(), manifest.end(), sites), last; it != last; ++it, ++ordinal)
            if ((*it)[2] == "operation") ordinals.emplace((*it)[1], ordinal);
        const auto path = artifact(label) / "driver.cpp"; auto driver = readFile(path);
        const std::string placeholder = "static const std::array<unsigned,2> immutableSites{};";
        const auto begin = driver.find(placeholder); require(begin != std::string::npos, "Missing immutable counter declaration");
        driver.replace(begin, placeholder.size(), "static const std::array<unsigned,2> immutableSites{" +
            std::to_string(ordinals.at(std::string(graph.getOperation(graph.valueDef(cases[1].constant)).symbolText()))) + ',' +
            std::to_string(ordinals.at(std::string(graph.getOperation(graph.valueDef(cases[1].same)).symbolText()))) + "};");
        std::ofstream(path) << driver;
    }

    Fixture events; auto& eventGraph = events.graph;
    const auto requested = events.input("immutable_request"), data = events.input("immutable_event_data", 8);
    const auto eventOne = events.constant(1, "1'b1"), numericConstant = events.constant(7, "7'h5d");
    const auto eventText = events.compute(OperationKind::kConstant, stringType, {});
    eventGraph.setAttr(eventGraph.valueDef(eventText), "constValue", std::string("ready"));
    const auto sampledCall = [&](const std::string& name, bool text) {
        const auto target = eventGraph.createOperation(OperationKind::kDpicImport, eventGraph.internSymbol(name));
        eventGraph.setAttr(target, "argsName", std::vector<std::string>{"requested", "constant"});
        eventGraph.setAttr(target, "argsDirection", std::vector<std::string>(2, "input"));
        eventGraph.setAttr(target, "argsType", std::vector<std::string>{"logic", text ? "string" : "logic"});
        eventGraph.setAttr(target, "argsWidth", std::vector<int64_t>{1, text ? 0 : 7});
        eventGraph.setAttr(target, "argsSigned", std::vector<bool>(2, false));
        eventGraph.setAttr(target, "hasReturn", true);
        eventGraph.setAttr(target, "returnType", std::string(text ? "string" : "logic"));
        eventGraph.setAttr(target, "returnWidth", int64_t{text ? 0 : 1});
        const auto call = eventGraph.createOperation(OperationKind::kDpicCall);
        eventGraph.setAttr(call, "targetImportSymbol", name);
        for (const auto value : {eventOne, requested, text ? eventText : numericConstant}) eventGraph.addOperand(call, value);
        const auto result = eventGraph.createValue(text ? stringType : Fixture::type(1));
        eventGraph.addResult(call, result); return result;
    };
    const auto numericSample = sampledCall("immutable_sample_numeric", false);
    const auto textSample = sampledCall("immutable_sample_text", true);
    const auto textClock = events.compute(OperationKind::kEq, Fixture::type(1), {textSample, eventText});
    const auto numericState = events.state("immutable_numeric_seen", 8), textState = events.state("immutable_text_seen", 8);
    events.write(OperationKind::kRegisterWritePort, numericState, {eventOne, data, numericSample});
    events.write(OperationKind::kRegisterWritePort, textState, {eventOne, data, textClock});
    eventGraph.bindOutputPort("immutable_numeric_seen", events.read(numericState));
    eventGraph.bindOutputPort("immutable_text_seen", events.read(textState));
    run(events.design, "grh-to-grhsim", events.session); events.split(); events.partition();
    for (const auto value : {numericSample, textSample}) {
        const auto producer = eventGraph.valueDef(value);
        require(eventGraph.opKind(producer) == sim::kDpiCall &&
                    sim::attribute<bool>(eventGraph, producer, "grhsim.sampleBoundary").value_or(false),
                "Immutable Event fixture lost its legal effectful sample boundary");
        require(std::any_of(eventGraph.valueUsers(value).begin(), eventGraph.valueUsers(value).end(), [&](const auto& use) {
                    return owner(eventGraph, use.operation) == Owner{"event"};
                }), "Immutable Event fixture no longer observes the sampled store from Event");
    }
    const std::string eventSupport = R"CPP(
extern "C" std::uint8_t immutable_sample_numeric(std::uint8_t requested,std::uint8_t constant) {
    assert(constant==93); return requested;
}
extern "C" std::string immutable_sample_text(std::uint8_t requested,const char* constant) {
    assert(std::string(constant)=="ready"); return requested ? constant : "";
}
)CPP";
    const std::string eventBody = R"CPP(
GrhSIM_top peer;
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); peer.init();
    for(auto* instance : {&model,&peer}) {
        auto& m=*instance; m.immutable_request=true; m.immutable_event_data=37+epoch;
        // Event runs before the first General sample: resettable values are
        // still zero/empty even when the effect's argument is immutable.
        m.eval(); assert(m.immutable_numeric_seen==0 && m.immutable_text_seen==0);
        m.eval(); assert(m.immutable_numeric_seen==37+epoch && m.immutable_text_seen==37+epoch);
        m.immutable_request=false; m.eval(); m.eval();
        m.immutable_request=true; m.immutable_event_data=91+epoch;
        m.eval(); assert(m.immutable_numeric_seen==37+epoch && m.immutable_text_seen==37+epoch);
        m.eval(); assert(m.immutable_numeric_seen==91+epoch && m.immutable_text_seen==91+epoch);
    }
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "immutable-event-samples-b" + std::to_string(budget);
        manualCase(events, label, budget, eventSupport, eventBody, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        const auto cpp = generatedCpp(directory);
        for (const auto value : {numericSample, textSample}) {
            const auto store = primary(directory, value);
            require(!store.empty() && (budget != 1024 || operationBlock(cpp, eventGraph.valueDef(value)).find(store) != std::string::npos),
                    "Immutable Event sample lost physical publication or provenance");
            bool eventRead = false;
            for (const auto& use : eventGraph.valueUsers(value)) if (owner(eventGraph, use.operation) == Owner{"event"}) {
                require(rootFunction(directory, use.operation) == "pEvent", "Sample observer left Event");
                eventRead |= budget != 1024 || operationBlock(cpp, use.operation).find(store) != std::string::npos;
            }
            require(eventRead, "Event bypassed the previous resettable DPI sample");
        }
    }

    // An interrupted nonsink has already cleared its activity bit. A later
    // eval must not invent a constant whose original publication was skipped.
    Fixture interrupted; auto& interruptedGraph = interrupted.graph;
    const auto interruptedOne = interrupted.constant(1, "1'b1");
    const auto beforeThrow = interrupted.constant(8, "8'd29");
    const auto throwingTarget = interruptedGraph.createOperation(OperationKind::kDpicImport,
        interruptedGraph.internSymbol("immutable_throw_once"));
    interruptedGraph.setAttr(throwingTarget, "argsName", std::vector<std::string>{"before"});
    interruptedGraph.setAttr(throwingTarget, "argsDirection", std::vector<std::string>{"input"});
    interruptedGraph.setAttr(throwingTarget, "argsWidth", std::vector<int64_t>{8});
    interruptedGraph.setAttr(throwingTarget, "argsSigned", std::vector<bool>{false});
    interruptedGraph.setAttr(throwingTarget, "hasReturn", false);
    const auto throwingCall = interruptedGraph.createOperation(OperationKind::kDpicCall);
    interruptedGraph.setAttr(throwingCall, "targetImportSymbol", std::string("immutable_throw_once"));
    interruptedGraph.addOperand(throwingCall, interruptedOne); interruptedGraph.addOperand(throwingCall, beforeThrow);
    const auto afterThrow = interrupted.constant(8, "8'd101");
    const auto afterTarget = interruptedGraph.createOperation(OperationKind::kDpicImport,
        interruptedGraph.internSymbol("immutable_after_throw"));
    interruptedGraph.setAttr(afterTarget, "argsName", std::vector<std::string>{"before", "after"});
    interruptedGraph.setAttr(afterTarget, "argsDirection", std::vector<std::string>(2, "input"));
    interruptedGraph.setAttr(afterTarget, "argsWidth", std::vector<int64_t>(2, 8));
    interruptedGraph.setAttr(afterTarget, "argsSigned", std::vector<bool>(2, false));
    interruptedGraph.setAttr(afterTarget, "hasReturn", true); interruptedGraph.setAttr(afterTarget, "returnWidth", int64_t{8});
    const auto afterCall = interruptedGraph.createOperation(OperationKind::kDpicCall);
    interruptedGraph.setAttr(afterCall, "targetImportSymbol", std::string("immutable_after_throw"));
    for (const auto value : {interruptedOne, beforeThrow, afterThrow}) interruptedGraph.addOperand(afterCall, value);
    const auto afterResult = interruptedGraph.createValue(Fixture::type(8)); interruptedGraph.addResult(afterCall, afterResult);
    interruptedGraph.bindOutputPort("immutable_after_throw", afterResult);
    run(interrupted.design, "grh-to-grhsim", interrupted.session);
    // Keep the fire constant before the throwing call too. The converter's
    // appended equivalent constant must not accidentally reorder this fixture.
    interruptedGraph.replaceOperand(throwingCall, 0, interruptedOne);
    interruptedGraph.replaceOperand(afterCall, 0, interruptedOne);
    interrupted.split();
    run(interrupted.design, "grhsim-partition-nonsink", interrupted.session, {"-clone-shared=0"});
    for (const auto op : {interruptedGraph.valueDef(interruptedOne), interruptedGraph.valueDef(beforeThrow),
                          throwingCall, interruptedGraph.valueDef(afterThrow)}) {
        require(owner(interruptedGraph, op).size() == 4 && owner(interruptedGraph, op)[1] == "nonsink",
                "Interrupted-source fixture lost a nonsink operation");
        interruptedGraph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "immutable_throw_source", "source"});
    }
    interruptedGraph.setAttr(afterCall, sim::kPartition, Owner{"general", "nonsink", "immutable_throw_consumer", "consumer"});
    run(interrupted.design, "grhsim-partition-sink", interrupted.session);
    const std::string interruptedSupport = R"CPP(
static bool immutableThrowNext;
extern "C" void immutable_throw_once(std::uint8_t before) {
    assert(before==29); if(immutableThrowNext) { immutableThrowNext=false; throw std::runtime_error("expected immutable-source interruption"); }
}
extern "C" std::uint8_t immutable_after_throw(std::uint8_t before,std::uint8_t after) {
    assert(before==29 && after==0); return before+after;
}
)CPP";
    const std::string interruptedBody = R"CPP(
for(unsigned epoch=0;epoch<3;++epoch) {
    model.init(); immutableThrowNext=true; bool caught=false;
    try { model.eval(); } catch(const std::runtime_error&) { caught=true; }
    assert(caught && model.immutable_after_throw==0);
    model.eval(); assert(model.immutable_after_throw==29);
}
)CPP";
    for (const auto budget : {16, 1024}) {
        const auto label = "immutable-interrupted-source-b" + std::to_string(budget);
        manualCase(interrupted, label, budget, interruptedSupport, interruptedBody, false, {{"execution-counters", "on"}});
        const auto directory = artifact(label) / "model";
        const auto cpp = generatedCpp(directory), beforeStore = primary(directory, beforeThrow), afterStore = primary(directory, afterThrow);
        const auto consumer = operationBlock(cpp, afterCall);
        require(!beforeStore.empty() && !afterStore.empty() && (budget != 1024 ||
                    (consumer.find(beforeStore) == std::string::npos && consumer.find(afterStore) != std::string::npos)),
                "Interrupted owner did not distinguish published-prefix and skipped constants");
        require(rootFunction(directory, interruptedGraph.valueDef(beforeThrow)) == rootFunction(directory, throwingCall) &&
                    rootFunction(directory, interruptedGraph.valueDef(afterThrow)) == rootFunction(directory, throwingCall),
                "Interrupted constants no longer bracket a call in the same owner");
    }
}

void testLargeCaptureMetadata() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clockA = fixture.input("capture_clock_a"), clockB = fixture.input("capture_clock_b");
    const auto enableA = fixture.input("capture_a"), enableB = fixture.input("capture_b");
    const auto raw = fixture.input("capture_raw"), gate = fixture.input("capture_gate");
    const auto data = fixture.input("capture_data", 8), index = fixture.input("capture_index", 4);
    auto wideType = Fixture::type(130); wideType.isSigned = true;
    const auto wideData = graph.createValue(graph.internSymbol("capture_wide_data"), wideType);
    graph.bindInputPort("capture_wide_data", wideData);
    const auto zero = fixture.constant(4, "4'd0"), one = fixture.constant(4, "4'd1");
    const auto two = fixture.constant(4, "4'd2"), invalid = fixture.constant(4, "4'd15");
    const auto high = fixture.constant(8, "8'ha6"), last = fixture.constant(8, "8'hd5");
    const auto mask = fixture.constant(8, "8'hf0"), zeroMask = fixture.constant(8, "8'h00");
    const auto memory = [&](const std::string& name, unsigned cells, const std::string& initial) {
        const auto state = fixture.state(name, cells * 8, {cells});
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Fill, initial}});
        graph.bindOutputPort(name, fixture.read(state)); return state;
    };
    const auto priority = memory("capture_priority", 8, "8'h39");
    const auto dynamic = memory("capture_dynamic", 8, "8'h7e");
    const auto mixed = memory("capture_mixed", 2, "8'h18");
    const auto stableMixed = memory("capture_stable_mixed", 2, "8'h27");
    const auto local = memory("capture_local", 2, "8'h4d");
    const auto bulk = memory("capture_bulk", 65, "8'h5a");
    std::vector<grh::OperationId> writers;
    const auto write = [&](OperationKind kind, grh::OperationId state,
                           std::initializer_list<grh::ValueId> args,
                           std::vector<std::string> edges = {"posedge"}) {
        const auto op = fixture.write(kind, state, args, std::move(edges)); writers.push_back(op); return op;
    };
    const auto firstA = write(OperationKind::kRegisterWriteArrayPort, priority, {enableA, zero, high, clockA});
    write(OperationKind::kRegisterWriteArrayPort, priority, {enableB, zero, data, clockA});
    const auto lastA = write(OperationKind::kRegisterWriteArrayMaskPort, priority, {enableA, zero, last, mask, clockA});
    write(OperationKind::kRegisterWriteArrayPort, priority, {enableA, one, high, clockA});
    const auto invalidWrite = write(OperationKind::kRegisterWriteArrayPort, priority, {enableA, invalid, high, clockA});
    write(OperationKind::kRegisterWriteArrayMaskPort, priority, {enableA, two, high, zeroMask, clockA});
    write(OperationKind::kRegisterWriteArrayPort, dynamic, {enableA, index, data, clockA});
    const auto mixedFirst = write(OperationKind::kRegisterWriteArrayPort, mixed,
        {enableA, zero, data, clockA, clockB}, {"posedge", "negedge"});
    const auto mixedLast = write(OperationKind::kRegisterWriteArrayPort, mixed,
        {enableA, one, high, clockA, clockB}, {"posedge", "negedge"});
    const auto stableMixedFirst = write(OperationKind::kRegisterWriteArrayPort, stableMixed,
        {enableA, zero, data, clockA, clockB}, {"posedge", "posedge"});
    const auto stableMixedLast = write(OperationKind::kRegisterWriteArrayPort, stableMixed,
        {enableA, one, high, clockA, clockB}, {"posedge", "posedge"});
    const auto localEnableA = fixture.compute(OperationKind::kAnd, Fixture::type(1), {raw, gate});
    const auto localEnableB = fixture.compute(OperationKind::kAnd, Fixture::type(1), {raw, gate});
    const auto localFirst = write(OperationKind::kRegisterWriteArrayPort, local, {localEnableA, zero, data, clockA});
    const auto localLast = write(OperationKind::kRegisterWriteArrayPort, local, {localEnableB, one, high, clockA});
    std::vector<grh::OperationId> bulkWriters;
    for (unsigned i = 0; i < 65; ++i)
        bulkWriters.push_back(write(OperationKind::kRegisterWriteArrayPort, bulk,
            {enableA, fixture.constant(7, "7'd" + std::to_string(i)),
             fixture.constant(8, "8'd" + std::to_string((i * 3 + 7) & 255)), clockA}));
    const auto wide = fixture.state("capture_wide", 260, {2});
    auto wideStateType = Fixture::type(260, {2}); wideStateType.isSigned = true;
    grh::setStateLogicalType(graph, wide, wideStateType);
    grh::setStateInitSpec(graph, wide, {{grh::InitKind::Fill, "130'h20000000000000000000000000000000b"}});
    graph.bindOutputPort("capture_wide", fixture.read(wide));
    const auto negative = fixture.compute(OperationKind::kConstant, wideType, {});
    graph.setAttr(graph.valueDef(negative), "constValue", std::string("130'sh3fffffffffffffffffffffffffffffffd"));
    const auto wideMask = fixture.constant(130, "130'h1000000000000000100000000000000ff");
    write(OperationKind::kRegisterWriteArrayPort, wide, {enableB, zero, wideData, clockA});
    const auto wideWrite = write(OperationKind::kRegisterWriteArrayMaskPort, wide,
        {enableA, zero, negative, wideMask, clockA});
    run(fixture.design, "grh-to-grhsim", fixture.session);
    // Positive alias cases use the same stable Event boundary identity.
    const auto sharedFire = graph.opOperands(firstA)[0];
    require(graph.opKind(graph.valueDef(sharedFire)) == sim::kEdgeDetect,
            "Capture metadata fixture lost its stable Event fire");
    for (const auto op : writers) if (op != mixedFirst && op != mixedLast &&
            op != stableMixedFirst && op != stableMixedLast)
        graph.replaceOperand(op, 0, sharedFire);
    // Keep explicit writer-local ORs as negative cases even when conversion
    // shares equivalent multi-clock event helpers.
    for (const auto op : {mixedFirst, mixedLast}) {
        const auto fire = graph.opOperands(op)[0];
        const auto join = graph.valueDef(fire);
        require(graph.opKind(join) == OperationKind::kOr && graph.opOperands(join).size() == 2,
                "Capture metadata fixture lost its multi-clock join");
        const auto first = graph.opOperands(join)[0];
        const auto second = graph.opOperands(join)[1];
        const auto localFire = fixture.compute(OperationKind::kOr, Fixture::type(1), {first, second});
        graph.replaceOperand(op, 0, localFire);
    }
    // Two users keep this OR in a nonsink owner. The writer-local ORs above
    // remain separate negative cases. Use a distinct second edge so their
    // sink groups cannot pick a local OR as this shared run's fresh fire.
    const auto stableMixedFire = graph.opOperands(stableMixedFirst)[0];
    graph.replaceOperand(stableMixedLast, 0, stableMixedFire);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=1"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    const auto& captureSchedule = sim::requireSinkSchedule(&fixture.session, graph);
    require(std::any_of(captureSchedule.sinkGroups.begin(), captureSchedule.sinkGroups.end(),
                [](const auto& group) { return !group.guards.empty(); }),
            "Capture metadata fixture did not exercise hoisted boundary guards");
    require(owner(graph, graph.valueDef(stableMixedFire)).size() == 4 &&
                owner(graph, graph.valueDef(stableMixedFire))[1] == "nonsink" &&
                graph.opOperands(stableMixedLast)[0] == stableMixedFire,
            "Capture metadata fixture lost its shared nonsink multi-clock fire");
    for (const auto value : {localEnableA, localEnableB})
        require(owner(graph, graph.valueDef(value)).size() == 3 &&
                    owner(graph, graph.valueDef(value))[1] == "large-sink",
                "Capture metadata fixture lost its unstable writer-local enable");
    for (const auto op : {mixedFirst, mixedLast}) {
        const auto fire = graph.opOperands(op)[0];
        require(graph.opKind(graph.valueDef(fire)) == OperationKind::kOr &&
                    owner(graph, graph.valueDef(fire)) == owner(graph, op),
                "Capture metadata fixture lost its writer-local multi-clock fire");
    }
    const std::string support = "using CaptureWide = unsigned _BitInt(130);\n";
    const std::string body = R"CPP(
for (unsigned epoch=0; epoch<3; ++epoch) {
    model.capture_clock_a=false; model.capture_clock_b=false; model.init();
    std::array<unsigned,8> priority{}, dynamic{}; priority.fill(0x39); dynamic.fill(0x7e);
    std::array<unsigned,2> mixed{0x18,0x18}, stableMixed{0x27,0x27}, local{0x4d,0x4d};
    std::array<unsigned,65> bulk{}; bulk.fill(0x5a);
    const CaptureWide initial=(CaptureWide(1)<<129)|11;
    const CaptureWide partial=(CaptureWide(1)<<128)|(CaptureWide(1)<<64)|255;
    std::array<CaptureWide,2> wide{initial,initial}; bool previousA=false, previousB=false;
    for (unsigned step=0; step<48; ++step) {
        model.capture_clock_a=step%2; model.capture_clock_b=(step/3)%2;
        model.capture_a=step%5!=0; model.capture_b=step%3!=0;
        model.capture_raw=step%4!=0; model.capture_gate=step%7!=0;
        model.capture_data=step*17+epoch*23+9; model.capture_index=(step/2)%12;
        model.capture_wide_data=(CaptureWide(step+3)<<120)|(CaptureWide(epoch+step+7)<<64)|(step*31+19);
        const bool fireA=model.capture_clock_a&&!previousA;
        const bool fireMixed=fireA||(!model.capture_clock_b&&previousB);
        const bool fireStableMixed=fireA||(model.capture_clock_b&&!previousB);
        const bool a=fireA&&model.capture_a, b=fireA&&model.capture_b;
        const bool l=fireA&&model.capture_raw&&model.capture_gate, m=fireMixed&&model.capture_a;
        unsigned writes=0, states=0;
        if(a) { priority[0]=0xa6; ++writes; }
        if(b) { priority[0]=model.capture_data; ++writes; }
        if(a) { priority[0]=(priority[0]&15)|0xd0; priority[1]=0xa6; writes+=3; }
        if(a||b) ++states; // The valid zero-mask write to cell 2 still touches.
        if(a&&model.capture_index<8) { dynamic[model.capture_index]=model.capture_data; ++writes; ++states; }
        if(m) { mixed[0]=model.capture_data; mixed[1]=0xa6; writes+=2; ++states; }
        if(fireStableMixed&&model.capture_a) { stableMixed[0]=model.capture_data; stableMixed[1]=0xa6; writes+=2; ++states; }
        if(l) { local[0]=model.capture_data; local[1]=0xa6; writes+=2; ++states; }
        if(a) { for(unsigned i=0;i<65;++i) bulk[i]=(i*3+7)&255; writes+=65; ++states; }
        if(b) { wide[0]=model.capture_wide_data; ++writes; }
        if(a) { wide[0]=(wide[0]&~partial)|((~CaptureWide(0)-2)&partial); ++writes; }
        if(a||b) ++states;
        const auto before=model.perf_counters(); model.eval(); const auto after=model.perf_counters();
        for(unsigned i=0;i<8;++i) assert(model.capture_priority[i]==priority[i]&&model.capture_dynamic[i]==dynamic[i]);
        for(unsigned i=0;i<2;++i) {
            assert(model.capture_mixed[i]==mixed[i]&&model.capture_local[i]==local[i]);
            assert(model.capture_stable_mixed[i]==stableMixed[i]);
            assert(CaptureWide(model.capture_wide[i])==wide[i]);
        }
        for(unsigned i=0;i<65;++i) assert(model.capture_bulk[i]==bulk[i]);
        assert(after.commitBatchExecCount-before.commitBatchExecCount==writes);
        assert(after.touchedWriteCount-before.touchedWriteCount==writes);
        assert(after.touchedStateShadowCount-before.touchedStateShadowCount==states);
        assert(after.totalRoundCount-before.totalRoundCount==(writes?2:1));
        model.eval(); const auto settled=model.perf_counters();
        assert(settled.touchedWriteCount==after.touchedWriteCount&&settled.commitBatchExecCount==after.commitBatchExecCount);
        previousA=model.capture_clock_a; previousB=model.capture_clock_b;
    }
}
)CPP";
    for (const auto budget : {16, 1024}) for (const std::string bitmap : {"on", "off"})
        for (const std::string specialization : {"on", "off"}) {
            const auto label = "large-capture-metadata-b" + std::to_string(budget) + "-bitmap-" + bitmap + "-specialize-" + specialization;
            manualCase(fixture, label, budget, support, body, false,
                {{"mem-enable-bitmap", bitmap}, {"mem-write-activate", "on-write"}, {"large-capture-specialization", specialization}});
            const auto directory = artifact(label) / "model";
            const auto operations = mappedRecords(readFile(directory / "code-map.json"), "operations");
            const auto record = [&](grh::OperationId op) -> const std::string& {
                const auto prefix = "{\"operation_id\":\"o" + std::to_string(op.index) + "\",";
                for (const auto& item : operations) if (item.starts_with(prefix)) return item;
                throw std::runtime_error("Large capture writer missing from code map");
            };
            const auto bit = [&](grh::OperationId op, const std::string& name) {
                std::smatch found;
                require(std::regex_search(record(op), found, std::regex("\"" + name + "\":([0-9]+)")), "Missing capture bit metadata");
                return std::stoul(found[1]);
            };
            const auto captured = [&](grh::OperationId op, unsigned arg) {
                for (const auto& item : mappedRecords(record(op), "captures")) {
                    if (item.find("\"role\":\"argument-" + std::to_string(arg) + "\"") == std::string::npos) continue;
                    std::smatch found; require(std::regex_search(item, found, std::regex(R"REGEX("reference":"([^"]+)")REGEX")), "Capture omitted actual source");
                    return found[1].str();
                }
                throw std::runtime_error("Capture argument missing from code map");
            };
            std::set<unsigned long> ownBits;
            for (const auto op : writers) {
                const auto own = bit(op, "enable_bit"), commit = bit(op, "commit_enable_bit");
                require(ownBits.insert(own).second, "General capture bitmap lost a writer's own bit");
                if (specialization == "off" || bitmap == "off") require(own == commit, "Disabled specialization changed commit enable identity");
                if (bitmap == "on") require(record(op).find("\"role\":\"captured-commit-enable\"") != std::string::npos,
                    "Canonical commit enable missing from operation storage map");
            }
            require(*ownBits.rbegin() >= 64, "Capture metadata fixture did not cross bitmap words");
            if (specialization == "on" && bitmap == "on") {
                require(bit(firstA, "commit_enable_bit") == bit(lastA, "commit_enable_bit"), "A/B/A stable guards did not share commit enable");
                require(bit(bulkWriters.front(), "commit_enable_bit") == bit(bulkWriters.back(), "commit_enable_bit"), "Cross-word stable writers did not share commit enable");
                require(bit(stableMixedFirst, "commit_enable_bit") == bit(stableMixedLast, "commit_enable_bit") &&
                            bit(stableMixedFirst, "commit_enable_bit") != bit(firstA, "commit_enable_bit"),
                        "Stable nonsink OR fire lost its complete shared capture identity");
                for (const auto op : {mixedFirst, mixedLast})
                    require(bit(op, "commit_enable_bit") == bit(op, "enable_bit") &&
                                bit(op, "commit_enable_bit") != bit(firstA, "commit_enable_bit"),
                            "Writer-local multi-clock OR fire was aliased without a stable boundary");
                for (const auto op : {localFirst, localLast}) require(bit(op, "commit_enable_bit") == bit(op, "enable_bit"),
                    "Writer-local enable was aliased without a stable boundary");
            }
            for (const auto& [op, arg] : std::vector<std::pair<grh::OperationId, unsigned>>{{firstA, 2}, {firstA, 3}, {invalidWrite, 2}, {wideWrite, 3}, {wideWrite, 4}}) {
                const auto source = captured(op, arg);
                require((source.find("largeSinkPayloadBoundaryStore.") == std::string::npos) == (specialization == "on"),
                    "True scalar/wide captured constant did not expose its actual specialized source");
                require(record(op).find("\"role\":\"captured-argument-" + std::to_string(arg) + "\",\"reference\":\"" + source + "\"") != std::string::npos,
                    "Capture and storage records disagree on actual argument source");
            }
        }
}

void testLargeLatchCaptures() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto enable = fixture.input("capture_latch_enable"), data = fixture.input("capture_latch_data", 8);
    const auto dynamicMask = fixture.input("capture_latch_mask", 8);
    const auto partial = fixture.constant(8, "8'hf0"), zero = fixture.constant(8, "8'h00");
    const auto constantData = fixture.constant(8, "8'ha5");
    const auto latch = [&](const std::string& name, const std::string& initial) {
        const auto state = fixture.state(name, 8); graph.setOpKind(state, OperationKind::kLatch);
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Constant, initial}});
        graph.bindOutputPort(name, fixture.read(state)); return state;
    };
    const auto firstState = latch("capture_latch_first", "8'h39");
    const auto lastState = latch("capture_latch_last", "8'h7e");
    // The zero-mask writer observes both states without changing either.
    // This keeps real General feedback, so state changes require a settling
    // round and an unchanged or disabled latch converges immediately.
    const auto feedback = fixture.compute(OperationKind::kOr, Fixture::type(8),
        {fixture.read(firstState), fixture.read(lastState)});
    const auto first = fixture.write(OperationKind::kLatchWritePort, firstState, {enable, data, partial});
    const auto last = fixture.write(OperationKind::kLatchWritePort, lastState, {enable, constantData, dynamicMask});
    const auto zeroWrite = fixture.write(OperationKind::kLatchWritePort, lastState, {enable, feedback, zero});
    fixture.convert();
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=1"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    run(fixture.design, "grhsim-partition-sink", fixture.session, {"-enable-guard-min-size=2"});
    for (const auto op : {first, last, zeroWrite})
        require(graph.opKind(op) == sim::kLatchWrite && graph.opOperands(op).size() == 3 &&
                    owner(graph, op).size() == 3 && owner(graph, op)[1] == "large-sink",
                "Latch capture fixture did not exercise the large controls=1 path");
    for (const auto value : graph.opOperands(graph.valueDef(feedback)))
        require(owner(graph, graph.valueDef(value)).size() == 3 &&
                    owner(graph, graph.valueDef(value))[0] == "general",
                "Latch capture fixture lost its General state observations");
    const auto& schedule = sim::requireSinkSchedule(&fixture.session, graph);
    for (const auto& group : schedule.sinkGroups)
        require(!group.fire && group.guards == std::vector<grh::ValueId>{enable},
                "Latch capture fixture lost its null fire or hoisted enable");
    const std::string body = R"CPP(
for(unsigned epoch=0; epoch<3; ++epoch) {
    model.capture_latch_enable=false; model.init(); unsigned first=0x39, last=0x7e;
    model.eval(); assert(model.capture_latch_first==first && model.capture_latch_last==last);
    for(unsigned step=0; step<32; ++step) {
        model.capture_latch_enable=step%5!=0;
        // Repeat payloads on alternate evals, with zero, partial and full masks.
        model.capture_latch_data=(step/2*29+epoch*17)&255;
        model.capture_latch_mask=step%4==0 ? 0 : step%4==1 ? 15 : step%4==2 ? 240 : 255;
        const auto previousFirst=first, previousLast=last;
        if(model.capture_latch_enable) {
            first=(first&15)|(model.capture_latch_data&240);
            last=(last&~unsigned(model.capture_latch_mask))|(0xa5&model.capture_latch_mask);
        }
        const auto before=model.perf_counters(); model.eval(); const auto after=model.perf_counters();
        assert(model.capture_latch_first==first && model.capture_latch_last==last);
        assert(after.totalRoundCount-before.totalRoundCount==
            (first!=previousFirst || last!=previousLast ? 2 : 1));
        model.eval(); const auto settled=model.perf_counters();
        assert(model.capture_latch_first==first && model.capture_latch_last==last);
        assert(settled.touchedStateShadowCount==after.touchedStateShadowCount);
        assert(settled.totalRoundCount-after.totalRoundCount==1);
    }
}
)CPP";
    for (const auto budget : {16, 1024}) for (const std::string bitmap : {"on", "off"})
        for (const std::string specialization : {"on", "off"}) for (const std::string mode : {"on-write", "on-change"}) {
            const auto label = "large-latch-capture-b" + std::to_string(budget) + "-bitmap-" + bitmap +
                "-specialize-" + specialization + "-" + mode;
            manualCase(fixture, label, budget, {}, body, false,
                {{"mem-enable-bitmap", bitmap}, {"large-capture-specialization", specialization}, {"mem-write-activate", mode}});
            const auto operations = mappedRecords(readFile(artifact(label) / "model/code-map.json"), "operations");
            const auto record = [&](grh::OperationId op) -> const std::string& {
                const auto prefix = "{\"operation_id\":\"o" + std::to_string(op.index) + "\",";
                for (const auto& item : operations) if (item.starts_with(prefix)) return item;
                throw std::runtime_error("Large latch writer missing from code map");
            };
            const auto bit = [&](grh::OperationId op, const std::string& name) {
                std::smatch found;
                require(std::regex_search(record(op), found, std::regex("\"" + name + "\":([0-9]+)")),
                        "Missing latch capture bit metadata");
                return std::stoul(found[1]);
            };
            std::set<unsigned long> own;
            for (const auto op : {first, last, zeroWrite}) {
                require(own.insert(bit(op, "enable_bit")).second, "Latch writers lost separate capture bits");
                require(bit(op, "commit_enable_bit") == bit(
                            specialization == "on" && bitmap == "on" ? first : op, "enable_bit"),
                        "Latch capture canonical enable ignored specialization or null fire");
                const auto captures = mappedRecords(record(op), "captures");
                require(captures.size() == 2, "Latch controls=1 capture offset omitted data or mask");
                for (unsigned argument=1; argument<=2; ++argument) {
                    const auto& capture = captures[argument-1];
                    require(capture.find("\"role\":\"argument-" + std::to_string(argument) + "\"") != std::string::npos,
                            "Latch capture metadata used register argument offsets");
                    const bool constant = op == last ? argument == 1 : argument == 2;
                    const bool physical = capture.find("largeSinkPayloadBoundaryStore.") != std::string::npos;
                    require(physical == (!constant || specialization == "off"),
                            "Latch data/mask capture did not honor actual specialized source");
                }
            }
        }
}

void testLargeCaptureInterruptedConstants() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("capture_throw_clock"), enable = fixture.input("capture_throw_enable");
    const auto one = fixture.constant(1, "1'b1"), index = fixture.constant(1, "1'b0");
    auto type = Fixture::type(130); type.isSigned = true;
    const auto before = fixture.compute(OperationKind::kConstant, type, {});
    graph.setAttr(graph.valueDef(before), "constValue", std::string("130'sh3fffffffffffffffffffffffffffffffd"));
    const auto imported = [&](const std::string& name, unsigned arguments, bool result) {
        const auto target = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol(name));
        std::vector<std::string> names; for (unsigned i=0; i<arguments; ++i) names.push_back("arg"+std::to_string(i));
        graph.setAttr(target, "argsName", names); graph.setAttr(target, "argsDirection", std::vector<std::string>(arguments, "input"));
        graph.setAttr(target, "argsWidth", std::vector<int64_t>(arguments, 130)); graph.setAttr(target, "argsSigned", std::vector<bool>(arguments, true));
        graph.setAttr(target, "hasReturn", result); if (result) graph.setAttr(target, "returnWidth", int64_t{8});
    };
    imported("capture_throw_once", 1, false);
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("capture_throw_once"));
    graph.addOperand(call, one); graph.addOperand(call, before);
    const auto after = fixture.compute(OperationKind::kConstant, type, {});
    graph.setAttr(graph.valueDef(after), "constValue", std::string("130'sh3fffffffffffffffffffffffffffffff9"));
    imported("capture_after_throw", 2, true);
    const auto audit = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(audit, "targetImportSymbol", std::string("capture_after_throw"));
    for (const auto value : {one, before, after}) graph.addOperand(audit, value);
    const auto result = graph.createValue(Fixture::type(8)); graph.addResult(audit, result); graph.bindOutputPort("capture_throw_audit", result);
    std::vector<grh::OperationId> writes;
    for (const auto& [name, value] : std::vector<std::pair<std::string, grh::ValueId>>{{"capture_prefix", before}, {"capture_suffix", after}}) {
        const auto state = fixture.state(name, 260, {2}); auto stateType = Fixture::type(260, {2}); stateType.isSigned = true;
        grh::setStateLogicalType(graph, state, stateType);
        grh::setStateInitSpec(graph, state, {{grh::InitKind::Fill, "130'h20000000000000000000000000000000b"}});
        writes.push_back(fixture.write(OperationKind::kRegisterWriteArrayPort, state, {enable, index, value, clock}));
        graph.bindOutputPort(name, fixture.read(state));
    }
    run(fixture.design, "grh-to-grhsim", fixture.session);
    graph.replaceOperand(call, 0, one); graph.replaceOperand(audit, 0, one);
    run(fixture.design, "grhsim-split-event-and-output", fixture.session);
    run(fixture.design, "grhsim-split-nonsink-and-sink", fixture.session, {"-mem-min-bytes=1"});
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    for (const auto op : {graph.valueDef(one), graph.valueDef(before), call, graph.valueDef(after)}) {
        require(owner(graph, op).size() == 4 && owner(graph, op)[1] == "nonsink", "Interrupted capture fixture lost a nonsink constant source");
        graph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "capture_throw_source", "source"});
    }
    graph.setAttr(audit, sim::kPartition, Owner{"general", "nonsink", "capture_throw_audit", "audit"});
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const std::string support = R"CPP(
using CaptureWide = unsigned _BitInt(130);
static bool captureThrowNext;
extern "C" void capture_throw_once(const unsigned _BitInt(192)& before) {
    assert(CaptureWide(before)==~CaptureWide(0)-2);
    if(captureThrowNext) { captureThrowNext=false; throw std::runtime_error("expected capture interruption"); }
}
extern "C" std::uint8_t capture_after_throw(const unsigned _BitInt(192)& before,const unsigned _BitInt(192)& after) {
    assert(CaptureWide(before)==~CaptureWide(0)-2 && after==0); return 29;
}
)CPP";
    const std::string body = R"CPP(
for(unsigned epoch=0; epoch<3; ++epoch) {
    model.init(); model.capture_throw_clock=false; model.capture_throw_enable=true;
    captureThrowNext=true; bool caught=false;
    try { model.eval(); } catch(const std::runtime_error&) { caught=true; }
    assert(caught);
    model.capture_throw_clock=true; const auto before=model.perf_counters(); model.eval(); const auto after=model.perf_counters();
    assert(model.capture_throw_audit==29);
    if(CaptureWide(model.capture_prefix[0])!=~CaptureWide(0)-2 || model.capture_suffix[0]!=0) {
        std::fprintf(stderr,"Interrupted capture epoch=%u commits=%llu writes=%llu\n",epoch,
            static_cast<unsigned long long>(after.commitBatchExecCount-before.commitBatchExecCount),
            static_cast<unsigned long long>(after.touchedWriteCount-before.touchedWriteCount));
        model.dumpState(stderr);
    }
    assert(CaptureWide(model.capture_prefix[0])==~CaptureWide(0)-2 && model.capture_suffix[0]==0);
    const CaptureWide untouched=(CaptureWide(1)<<129)|11;
    assert(CaptureWide(model.capture_prefix[1])==untouched && CaptureWide(model.capture_suffix[1])==untouched);
    assert(after.commitBatchExecCount-before.commitBatchExecCount==2);
    assert(after.touchedWriteCount-before.touchedWriteCount==2 && after.touchedStateShadowCount-before.touchedStateShadowCount==2);
    model.eval(); assert(model.perf_counters().touchedWriteCount==after.touchedWriteCount);
}
)CPP";
    for (const auto budget : {16,1024}) for (const std::string specialization : {"on","off"}) {
        const auto label = "large-capture-interrupted-b" + std::to_string(budget) + "-specialize-" + specialization;
        manualCase(fixture, label, budget, support, body, false,
            {{"large-capture-specialization", specialization}, {"mem-write-activate", "on-write"}});
        const auto records = mappedRecords(readFile(artifact(label)/"model/code-map.json"), "operations");
        for (unsigned i=0; i<writes.size(); ++i) {
            const auto prefix = "{\"operation_id\":\"o" + std::to_string(writes[i].index) + "\",";
            bool found = false;
            for (const auto& record : records) if (record.starts_with(prefix))
                for (const auto& capture : mappedRecords(record, "captures"))
                    if (capture.find("\"role\":\"argument-3\"") != std::string::npos) {
                        const bool physical = capture.find("largeSinkPayloadBoundaryStore.") != std::string::npos;
                        require(physical == (i == 1 || specialization == "off"),
                            "Interrupted prefix/suffix constant capture ignored its original publication point");
                        found = true;
                    }
            require(found, "Interrupted capture lost its data argument metadata");
        }
    }
}

void testPrivateScalarMuxFusion() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock = fixture.input("fuse_clock"), enable = fixture.input("fuse_enable");
    const auto c0 = fixture.input("fuse_c0"), c1 = fixture.input("fuse_c1"), c2 = fixture.input("fuse_c2");
    const auto a = fixture.input("fuse_a",8), b = fixture.input("fuse_b",8);
    const auto wideA = fixture.input("fuse_wide_a",16), wideB = fixture.input("fuse_wide_b",16);
    auto signed3 = Fixture::type(3); signed3.isSigned = true;
    auto signed7 = Fixture::type(7); signed7.isSigned = true;
    const auto narrow = graph.createValue(graph.internSymbol("fuse_signed3"),signed3);
    graph.bindInputPort("fuse_signed3",narrow);
    std::array<grh::ValueId,4> chain;
    chain[0] = fixture.compute(OperationKind::kMux,signed3,{c0,narrow,a});
    chain[1] = fixture.compute(OperationKind::kMux,signed7,{c1,chain[0],b});
    chain[2] = fixture.compute(OperationKind::kMux,Fixture::type(16),{c2,chain[1],wideA});
    chain[3] = fixture.compute(OperationKind::kMux,Fixture::type(16),{c0,chain[2],wideB});
    const auto saved = fixture.state("fuse_saved",16);
    const auto writer = fixture.write(OperationKind::kRegisterWritePort,saved,{enable,chain[3],clock});
    graph.bindOutputPort("fuse_saved",fixture.read(saved));
    // A real multi-use DAG stays materialized even when both consumers are
    // private muxes in this same demand region.
    const auto shared = fixture.compute(OperationKind::kMux,Fixture::type(8),{c0,a,b});
    const auto left = fixture.compute(OperationKind::kMux,Fixture::type(8),{c1,shared,b});
    const auto right = fixture.compute(OperationKind::kMux,Fixture::type(8),{c2,shared,a});
    const auto sum = fixture.compute(OperationKind::kAdd,Fixture::type(8),{left,right});
    const auto other = fixture.state("fuse_multi",8);
    const auto otherWriter = fixture.write(OperationKind::kRegisterWritePort,other,{enable,sum,clock});
    graph.bindOutputPort("fuse_multi",fixture.read(other));
    run(fixture.design,"grh-to-grhsim",fixture.session); fixture.split(); fixture.partition();
    const auto& schedule = sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==1 && schedule.generalRuns[0].orderedWriters.size()==2,
            "Scalar mux fixture lost its exact shared-enable writer run");
    std::vector<std::pair<grh::OperationId,std::string>> sites;
    for (const auto value : chain) {
        require(graph.opKind(graph.valueDef(value))==OperationKind::kMux &&
                    owner(graph,graph.valueDef(value))==schedule.generalRuns[0].partitionKey,
                "Scalar mux fixture lost its private mux chain");
        sites.emplace_back(graph.valueDef(value),"operation");
    }
    for (const auto value : {shared,left,right,sum}) sites.emplace_back(graph.valueDef(value),"operation");
    sites.emplace_back(writer,"writer-sample"); sites.emplace_back(otherWriter,"writer-sample");
    require(graph.valueUsers(shared).size()==2,"Scalar mux multi-use negative lost an operand occurrence");
    const std::string body = R"CPP(
for (unsigned epoch=0;epoch<2;++epoch) {
    model.init(); unsigned held=0, heldMulti=0;
#if FUSION_COUNTERS
    std::array<unsigned long long,10> expected{};
#endif
    for (unsigned selectors=0;selectors<8;++selectors) for (unsigned pattern=0;pattern<8;++pattern) {
        model.fuse_clock=false; model.fuse_enable=(selectors+pattern)%3!=0;
        model.fuse_c0=selectors&1; model.fuse_c1=selectors&2; model.fuse_c2=selectors&4;
        model.fuse_a=(pattern*37+selectors)&255; model.fuse_b=(pattern*53+91)&255;
        model.fuse_wide_a=(pattern*11113+selectors*313)&65535;
        model.fuse_wide_b=(pattern*7531+0xa55a)&65535;
        model.fuse_signed3=static_cast<std::int8_t>(pattern);
#if FUSION_COUNTERS
        const auto before=batchCounts(model); assert(before==expected);
#endif
        model.eval(); assert(model.fuse_saved==held && model.fuse_multi==heldMulti);
#if FUSION_COUNTERS
        assert(batchCounts(model)==before);
#endif
        if (model.fuse_enable) {
            const auto first=model.fuse_c0 ? fusionBits(unsigned(model.fuse_signed3),3,false,3)
                                           : fusionBits(model.fuse_a,8,false,3);
            const auto second=model.fuse_c1 ? fusionBits(first,3,true,7) : fusionBits(model.fuse_b,8,false,7);
            const auto third=model.fuse_c2 ? fusionBits(second,7,true,16) : model.fuse_wide_a;
            held=model.fuse_c0 ? third : model.fuse_wide_b;
            const auto shared=model.fuse_c0 ? model.fuse_a : model.fuse_b;
            heldMulti=((model.fuse_c1 ? shared : model.fuse_b)+(model.fuse_c2 ? shared : model.fuse_a))&255;
#if FUSION_COUNTERS
            for (auto& count : expected) ++count;
#endif
        }
        model.fuse_clock=true; model.eval();
        assert(model.fuse_saved==held && model.fuse_multi==heldMulti);
#if FUSION_COUNTERS
        assert(batchCounts(model)==expected);
#endif
        model.eval(); assert(model.fuse_saved==held && model.fuse_multi==heldMulti);
#if FUSION_COUNTERS
        assert(batchCounts(model)==expected);
#endif
    }
}
)CPP";
    const std::string commonSupport = R"CPP(
static unsigned fusionBits(unsigned value,unsigned sourceWidth,bool sign,unsigned resultWidth) {
    const auto mask=(1u<<sourceWidth)-1; value&=mask;
    if (sign && sourceWidth<resultWidth && ((value>>(sourceWidth-1))&1)) value|=~mask;
    return value&((1u<<resultWidth)-1);
}
)CPP";
    for (const auto budget : {16,1024}) for (const bool counters : {false,true}) {
        const auto label="private-scalar-mux-fusion-"+std::string(counters ? "counters" : "plain")+"-b"+std::to_string(budget);
        const auto support=std::string("#define FUSION_COUNTERS ")+(counters ? "1\n" : "0\n")+commonSupport+
            (counters ? batchActivityCounterSupport(sites.size()) : std::string{});
        manualCase(fixture,label,budget,support,body,false,{{"execution-counters",counters ? "on" : "off"}});
        const auto directory=artifact(label)/"model";
        require(batchActivityStatistic(directory,"eligible_runs")==1 && batchActivityStatistic(directory,"rejected_runs")==0,
                "Scalar mux fixture no longer enters the optimization's eligible scope");
        const auto functions=mappedRecords(readFile(directory/"code-map.json"),"functions");
        const auto hasLocal = [&](grh::ValueId value) {
            const auto marker="\"name\":\""+mappedValue(directory,value)+"\"";
            for (const auto& function : functions)
                for (const auto& local : mappedRecords(function,"locals")) if (local.find(marker)!=std::string::npos) return true;
            return false;
        };
        for (unsigned i=0;i<3;++i)
            require(!hasLocal(chain[i]),"Fused intermediate still has a local or frame field");
        require(hasLocal(shared),"Multi-use mux was duplicated instead of materialized");
        const auto finalId="\"o"+std::to_string(graph.valueDef(chain.back()).index)+"\"";
        bool provenance=false;
        for (const auto& function : functions) {
            const auto start=function.find("\"operations\":["), end=function.find(']',start);
            require(start!=std::string::npos && end!=std::string::npos,"Mux function omitted its operation provenance");
            const auto operations=function.substr(start,end-start);
            if (operations.find(finalId)==std::string::npos) continue;
            bool complete=true;
            for (const auto value : chain)
                complete &= operations.find("\"o"+std::to_string(graph.valueDef(value).index)+"\"")!=std::string::npos;
            provenance |= complete;
        }
        require(provenance,"Final mux expression lost a folded producer's operation provenance");
        const auto cpp=generatedCpp(directory), name=mappedValue(directory,chain.back());
        const auto start=cpp.find(name+" = ");
        require(start!=std::string::npos,"Final mux assignment disappeared");
        const auto expression=cpp.substr(start,cpp.find(';',start)-start);
        std::size_t selects=0, position=0;
        while ((position=expression.find("numeric_select(",position))!=std::string::npos) { ++selects; position+=15; }
        require(selects==4,"Scalar fusion did not preserve all four eager numeric_select calls");
        if (counters) bindBatchActivityCounters(label,graph,sites);
    }
}

void testPrivateScalarMuxFusionDepthCutoff() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto clock=fixture.input("cutoff_clock"), enable=fixture.input("cutoff_enable");
    const auto a=fixture.input("cutoff_a",16), b=fixture.input("cutoff_b",16);
    std::array<grh::ValueId,20> chain;
    std::string setSelectors;
    for (unsigned i=0;i<chain.size();++i) {
        const auto port="cutoff_c"+std::to_string(i);
        const auto selector=fixture.input(port);
        chain[i]=fixture.compute(OperationKind::kMux,Fixture::type(16),
            {selector,i==0 ? a : chain[i-1],i&1 ? a : b});
        setSelectors+="model."+port+"=(selectors>>"+std::to_string(i)+")&1u;\n";
    }
    const auto state=fixture.state("cutoff_saved",16);
    const auto writer=fixture.write(OperationKind::kRegisterWritePort,state,{enable,chain.back(),clock});
    graph.bindOutputPort("cutoff_saved",fixture.read(state));
    run(fixture.design,"grh-to-grhsim",fixture.session); fixture.split(); fixture.partition();
    const auto& schedule=sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size()==1 && schedule.generalRuns[0].orderedWriters==std::vector{writer},
            "Mux cutoff fixture lost its single exact writer run");
    for (unsigned i=0;i<chain.size();++i) {
        require(graph.opKind(graph.valueDef(chain[i]))==OperationKind::kMux &&
                    owner(graph,graph.valueDef(chain[i]))==schedule.generalRuns[0].partitionKey &&
                    graph.valueUsers(chain[i]).size()==1,
                "Mux cutoff fixture lost a private single-use mux");
        if (i+1<chain.size()) {
            const auto use=graph.valueUsers(chain[i])[0];
            require(use.operation==graph.valueDef(chain[i+1]) && use.operandIndex==1,
                    "Mux cutoff chain no longer links through one data operand");
        }
    }
    const std::string support=R"CPP(
static unsigned cutoffReference(unsigned selectors,unsigned a,unsigned b) {
    // Every false selector overwrites the earlier value. The last overwrite
    // wins; all true selectors retain a. No emitter grouping is modeled here.
    unsigned expected=a;
    for (unsigned stage=0;stage<20;++stage)
        if (((selectors>>stage)&1u)==0) expected=stage&1 ? a : b;
    return expected&65535u;
}
)CPP";
    const std::string body=R"CPP(
std::array<unsigned,44> choices{0xfffffu,0u,0x55555u,0xaaaaau};
for (unsigned bit=0;bit<20;++bit) {
    choices[4+2*bit]=0xfffffu^(1u<<bit); choices[5+2*bit]=1u<<bit;
}
for (unsigned epoch=0;epoch<2;++epoch) {
    model.init(); unsigned held=0;
    for (unsigned index=0;index<choices.size();++index) for (unsigned pattern=0;pattern<8;++pattern) {
        const auto selectors=choices[index];
        const bool enabled=(index+pattern+epoch)%3!=0;
        model.cutoff_clock=false; model.cutoff_enable=enabled;
        model.cutoff_a=(pattern*19073+index*197+epoch*11)&65535u;
        model.cutoff_b=(pattern*7321+index*3907+epoch*97+0x5aa5u)&65535u;
)CPP"+setSelectors+R"CPP(
        model.eval(); model.eval(); assert(model.cutoff_saved==held);
        if (enabled) held=cutoffReference(selectors,model.cutoff_a,model.cutoff_b);
        model.cutoff_clock=true; model.eval(); assert(model.cutoff_saved==held);
        model.eval(); model.eval(); assert(model.cutoff_saved==held);
        // Payload and enable changes without another edge cannot write state.
        model.cutoff_a^=65535u; model.cutoff_b^=0xaaaau; model.cutoff_enable=!enabled;
        model.eval(); assert(model.cutoff_saved==held);
    }
}
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label="private-scalar-mux-cutoff-b"+std::to_string(budget);
        manualCase(fixture,label,budget,support,body,false);
        const auto directory=artifact(label)/"model";
        require(batchActivityStatistic(directory,"eligible_runs")==1 && batchActivityStatistic(directory,"rejected_runs")==0,
                "Mux cutoff fixture no longer enters one eligible run");
        const auto stats=readFile(directory/"private-mux-stats.json");
        const auto statistic = [&](std::string_view name) {
            const auto position=stats.find("\""+std::string(name)+"\"");
            const auto colon=position==std::string::npos ? position : stats.find(':',position);
            require(colon!=std::string::npos,"Missing mux cutoff statistic");
            return std::stoull(stats.substr(colon+1));
        };
        require(statistic("nonempty_regions")==1 && statistic("folded_muxes")==17 &&
                    statistic("local_assignments_elided")==17 && statistic("max_expression_nodes")==8 &&
                    statistic("max_expression_depth")==8,
                "Twenty-mux chain did not hit the exact depth-8 partition");
        const auto functions=mappedRecords(readFile(directory/"code-map.json"),"functions");
        std::array<bool,20> locals{};
        std::array<std::string,20> ids;
        for (unsigned i=0;i<chain.size();++i) {
            const auto marker="\"name\":\""+mappedValue(directory,chain[i])+"\"";
            ids[i]="\"o"+std::to_string(graph.valueDef(chain[i]).index)+"\"";
            for (const auto& function : functions)
                for (const auto& local : mappedRecords(function,"locals"))
                    locals[i]=locals[i] || local.find(marker)!=std::string::npos;
            require(locals[i]==(i==7 || i==15 || i==19),
                    "Mux cutoff retained an unexpected local/frame root or lost a required root");
        }
        for (const auto [begin,end] : std::array<std::pair<unsigned,unsigned>,3>{{{0,7},{8,15},{16,19}}}) {
            bool complete=false;
            for (const auto& function : functions) {
                const auto start=function.find("\"operations\":["), stop=function.find(']',start);
                require(start!=std::string::npos && stop!=std::string::npos,"Mux cutoff function lost its operation list");
                const auto operations=function.substr(start,stop-start);
                if (operations.find(ids[end])==std::string::npos) continue;
                bool group=true;
                for (unsigned i=begin;i<=end;++i) group &= operations.find(ids[i])!=std::string::npos;
                complete |= group;
            }
            require(complete,"Mux cutoff root lost an original operation in its expression segment");
        }
    }
}

void testNumericSelects() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto choose0 = fixture.input("select_c0"), choose1 = fixture.input("select_c1"), choose2 = fixture.input("select_c2");
    const auto input = [&](const std::string& name, const grh::LogicalType& type) {
        const auto value = graph.createValue(graph.internSymbol(name), type);
        graph.bindInputPort(name, value); return value;
    };
    struct SelectCase {
        unsigned width; bool sign; std::string name;
        grh::LogicalType type;
        std::array<grh::ValueId,4> data;
        grh::ValueId mux{}, priority{};
    };
    std::vector<SelectCase> cases;
    std::string prepare, verify;
    for (const unsigned width : {1u,3u,7u,8u,16u,31u,32u,63u,64u,65u,128u}) {
        for (const bool sign : {false,true}) {
            if (width > 64 && sign) continue;
            auto type = Fixture::type(width); type.isSigned = sign;
            SelectCase item{width,sign,(sign ? "s" : "u") + std::to_string(width),type,{}};
            for (unsigned arm=0; arm<4; ++arm) {
                const auto name = "select_" + item.name + "_" + std::to_string(arm);
                item.data[arm] = input(name,type);
                const auto port = "model." + name;
                prepare += port + "=static_cast<decltype(" + port + ")>(patterns[(pattern+" +
                    std::to_string(arm) + ")%patterns.size()]);\n";
            }
            item.mux = fixture.compute(OperationKind::kMux,type,{choose0,item.data[0],item.data[1]});
            graph.bindOutputPort("select_mux_" + item.name,item.mux);
            const auto source = "model.select_" + item.name + "_";
            const auto normalized = [&](unsigned arm) {
                return "selectBits(SelectWide(" + source + std::to_string(arm) + ")," +
                    std::to_string(width) + ",false," + std::to_string(width) + ")";
            };
            const auto expected = "selectExpected_" + item.name;
            verify += "const auto " + expected + "=model.select_c0 ? " + normalized(0) + " : " + normalized(1) + ";\n";
            verify += "assert(selectBits(SelectWide(model.select_mux_" + item.name + ")," + std::to_string(width) +
                ",false," + std::to_string(width) + ")==" + expected + ");\n";
            if (width <= 64)
                verify += "assert(selectBits(SelectWide(model.select_priority_" + item.name + ")," + std::to_string(width) +
                    ",false," + std::to_string(width) + ")== (model.select_c0 ? " + normalized(0) +
                    " : model.select_c1 ? " + normalized(1) + " : model.select_c2 ? " + normalized(2) + " : " + normalized(3) + "));\n";
            cases.push_back(std::move(item));
        }
    }
    // Extend from the semantic sign bit, then truncate to the result width.
    auto signed7 = Fixture::type(7); signed7.isSigned = true;
    const auto narrow = input("select_signed7",signed7), wide = fixture.input("select_unsigned64",64);
    prepare += "model.select_signed7=static_cast<std::int8_t>(patterns[pattern]); model.select_unsigned64=static_cast<std::uint64_t>(patterns[(pattern+1)%patterns.size()]);\n";
    for (const unsigned width : {3u,16u,32u,64u}) {
        auto type = Fixture::type(width); type.isSigned = width != 3;
        const auto result = fixture.compute(OperationKind::kMux,type,{choose0,narrow,wide});
        const auto name = "select_convert_" + std::to_string(width);
        graph.bindOutputPort(name,result);
        verify += "assert(selectBits(SelectWide(model." + name + ")," + std::to_string(width) + ",false," +
            std::to_string(width) + ")== (model.select_c0 ? selectBits(SelectWide(model.select_signed7),7,true," +
            std::to_string(width) + ") : selectBits(SelectWide(model.select_unsigned64),64,false," + std::to_string(width) + ")));\n";
    }
    const grh::LogicalType realType{grh::ValueType::Real,64,false,grh::LogicDomain::TwoState,{}};
    const grh::LogicalType stringType{grh::ValueType::String,0,false,grh::LogicDomain::TwoState,{}};
    const auto realA = input("select_real_a",realType), realB = input("select_real_b",realType);
    const auto textA = input("select_text_a",stringType), textB = input("select_text_b",stringType);
    graph.bindOutputPort("select_real",fixture.compute(OperationKind::kMux,realType,{choose0,realA,realB}));
    graph.bindOutputPort("select_text",fixture.compute(OperationKind::kMux,stringType,{choose0,textA,textB}));
    const auto shapedType = Fixture::type(24,{3});
    const auto shapedA = input("select_shape_a",shapedType), shapedB = input("select_shape_b",shapedType);
    graph.bindOutputPort("select_shape",fixture.compute(OperationKind::kMux,shapedType,{choose0,shapedA,shapedB}));

    const auto clock = fixture.input("select_clock"), enableA = fixture.input("select_enable_a"), enableB = fixture.input("select_enable_b");
    const auto guardA = fixture.input("select_guard_a",16), guardB = fixture.input("select_guard_b",16);
    const auto shared = fixture.compute(OperationKind::kAdd,Fixture::type(16),{guardA,guardB});
    const auto flipped = fixture.compute(OperationKind::kXor,Fixture::type(16),{shared,fixture.constant(16,"16'ha55a")});
    const std::array<grh::ValueId,3> payloads{
        fixture.compute(OperationKind::kMux,Fixture::type(16),{choose0,shared,guardB}),
        fixture.compute(OperationKind::kMux,Fixture::type(16),{choose1,guardA,shared}),
        fixture.compute(OperationKind::kMux,Fixture::type(16),{choose2,flipped,guardA})
    };
    for (unsigned i=0; i<3; ++i) {
        const auto state = fixture.state("select_saved_" + std::to_string(i),16);
        fixture.write(OperationKind::kRegisterWritePort,state,{i == 1 ? enableB : enableA,payloads[i],clock});
        graph.bindOutputPort("select_saved_" + std::to_string(i),fixture.read(state));
    }

    // Keep actual kMux nodes, including bool and shaped results, while testing
    // the normalized priority operation directly with all three conditions.
    run(fixture.design,"grh-to-grhsim",fixture.session);
    for (auto& item : cases) {
        if (item.width > 64) continue;
        item.priority = fixture.compute(sim::kPrioritySelect,item.type,
            {choose0,choose1,choose2,item.data[0],item.data[1],item.data[2],item.data[3]});
        graph.bindOutputPort("select_priority_" + item.name,item.priority);
    }
    fixture.split();
    run(fixture.design,"grhsim-partition-nonsink",fixture.session);
    run(fixture.design,"grhsim-partition-sink",fixture.session,{"-enable-guard-min-size=0"});
    const auto& schedule = sim::requireSinkSchedule(&fixture.session,graph);
    require(schedule.generalRuns.size() == 1 && schedule.generalRuns.front().orderedWriters.size() == 3,
            "Numeric select fixture lost its A/B/A writer run");
    for (const auto payload : payloads)
        require(owner(graph,graph.valueDef(payload)) == schedule.generalRuns.front().partitionKey,
                "Numeric select payload is no longer sink-private");
    for (const auto& item : cases)
        require(graph.opKind(graph.valueDef(item.mux)) == OperationKind::kMux &&
                    (item.width > 64 || graph.opKind(graph.valueDef(item.priority)) == sim::kPrioritySelect),
                "Numeric select fixture lost a requested selection operation");

    const std::string support = R"CPP(
#include <bit>
using SelectWide = unsigned _BitInt(128);
static SelectWide selectBits(SelectWide value,unsigned sourceWidth,bool sign,unsigned resultWidth) {
    const SelectWide sourceMask=sourceWidth == 128 ? ~SelectWide(0) : (SelectWide(1)<<sourceWidth)-1;
    value &= sourceMask;
    if (sign && sourceWidth<resultWidth && ((value>>(sourceWidth-1))&1)) value |= ~sourceMask;
    return resultWidth == 128 ? value : value&((SelectWide(1)<<resultWidth)-1);
}
)CPP";
    const std::string body = R"CPP(
const std::array<SelectWide,8> patterns{
    0,~SelectWide(0),1,SelectWide(1)<<63,SelectWide(1)<<64,SelectWide(1)<<127,
    (SelectWide(UINT64_C(0xaaaaaaaaaaaaaaaa))<<64)|UINT64_C(0x5555555555555555),
    (SelectWide(UINT64_C(0xfedcba9876543210))<<64)|UINT64_C(0x0123456789abcdef)
};
for (unsigned epoch=0; epoch<2; ++epoch) {
    model.init(); std::array<unsigned,3> saved{};
    for (unsigned selectors=0; selectors<8; ++selectors) for (unsigned pattern=0; pattern<patterns.size(); ++pattern) {
        model.select_clock=false;
        model.select_c0=selectors&1; model.select_c1=selectors&2; model.select_c2=selectors&4;
        model.select_enable_a=(selectors+pattern+epoch)&1;
        model.select_enable_b=(selectors+pattern+epoch)&2;
        model.select_guard_a=static_cast<std::uint16_t>(patterns[pattern]);
        model.select_guard_b=static_cast<std::uint16_t>(patterns[(pattern+3)%patterns.size()]);
        model.select_real_a=std::bit_cast<double>(pattern&1 ? UINT64_C(0x8000000000000000) : UINT64_C(0x7ff8000000001234));
        model.select_real_b=std::bit_cast<double>(pattern&1 ? UINT64_C(0x7ff0000000000000) : UINT64_C(0x4004000000000000));
        model.select_text_a=std::string(160,'A')+std::to_string(pattern);
        model.select_text_b=std::string("B\0tail",6)+std::to_string(epoch);
        for(unsigned i=0;i<3;++i) { model.select_shape_a[i]=pattern*17+i; model.select_shape_b[i]=selectors*29+i; }
)CPP" + prepare + R"CPP(
        model.eval();
)CPP" + verify + R"CPP(
        assert(std::bit_cast<std::uint64_t>(model.select_real)==std::bit_cast<std::uint64_t>(model.select_c0 ? model.select_real_a : model.select_real_b));
        assert(model.select_text==(model.select_c0 ? model.select_text_a : model.select_text_b));
        assert(model.select_shape==(model.select_c0 ? model.select_shape_a : model.select_shape_b));
        assert(model.select_saved_0==saved[0] && model.select_saved_1==saved[1] && model.select_saved_2==saved[2]);
        const unsigned sum=(unsigned(model.select_guard_a)+model.select_guard_b)&65535;
        if(model.select_enable_a) { saved[0]=model.select_c0 ? sum : model.select_guard_b; saved[2]=model.select_c2 ? (sum^0xa55a) : model.select_guard_a; }
        if(model.select_enable_b) saved[1]=model.select_c1 ? model.select_guard_a : sum;
        model.select_clock=true; model.eval();
        assert(model.select_saved_0==saved[0] && model.select_saved_1==saved[1] && model.select_saved_2==saved[2]);
        model.eval();
        assert(model.select_saved_0==saved[0] && model.select_saved_1==saved[1] && model.select_saved_2==saved[2]);
    }
}
)CPP";
    for (const auto budget : {16,1024})
        manualCase(fixture,"numeric-selects-b" + std::to_string(budget),budget,support,body,false);
}

void testNarrowConstantLogicalShifts() {
    Fixture fixture; auto& graph = fixture.graph;
    const auto data = fixture.input("shift_data", 128);
    std::vector<grh::OperationId> placements;
    const auto place = [&](grh::ValueId value) {
        placements.push_back(graph.valueDef(value)); return value;
    };
    const auto one = place(fixture.constant(1, "1'b1"));
    struct AmountCase { std::string name; unsigned width; bool sign; grh::ValueId value{}; };
    std::vector<AmountCase> amounts{
        {"u32",32,false}, {"u8",8,false}, {"s8",8,true},
        {"u16",16,false}, {"s16",16,true}, {"u31",31,false}, {"s31",31,true},
        {"s32",32,true}, {"u64",64,false}, {"s64",64,true},
        {"u128",128,false}, {"s128",128,true}
    };
    for (auto& amount : amounts) {
        auto type = Fixture::type(amount.width); type.isSigned = amount.sign;
        amount.value = graph.createValue(graph.internSymbol("shift_amount_" + amount.name), type);
        graph.bindInputPort("shift_amount_" + amount.name, amount.value);
    }
    struct ShiftCase {
        unsigned width; bool sign; uint64_t bits; std::size_t amount;
        grh::ValueId source{}, shifted{}, selected{};
        std::string inputName;
    };
    std::vector<ShiftCase> cases;
    // Keep the original 5/6/7/8 sources in one owner with the shared dynamic
    // amount. Width-two/three literals deliberately exercise normalization.
    for (const auto width : {2u,3u,4u})
        for (const auto bits : {UINT64_C(5),UINT64_C(6),UINT64_C(7),UINT64_C(8)})
            cases.push_back({width,false,bits,0});
    for (const auto width : {2u,3u,4u}) {
        ShiftCase item{width,false,0,0};
        item.inputName = "shift_runtime_u" + std::to_string(width); cases.push_back(std::move(item));
    }
    for (std::size_t amount = 1; amount < amounts.size(); ++amount) {
        for (const auto bits : {UINT64_C(5),UINT64_C(6),UINT64_C(7)})
            cases.push_back({3,false,bits,amount});
        cases.push_back({4,false,8,amount});
    }
    for (const auto width : {8u,16u,31u,32u,64u}) {
        const auto high = uint64_t{1} << (width - 1);
        const auto mask = width == 64 ? UINT64_MAX : (uint64_t{1} << width) - 1;
        for (const auto sign : {false,true})
            for (const auto bits : {high,mask,UINT64_C(127),UINT64_C(128)})
                cases.push_back({width,sign,bits,0});
    }
    std::vector<grh::ValueId> selectedBits;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto& item = cases[index];
        auto sourceType = Fixture::type(item.width); sourceType.isSigned = item.sign;
        if (item.inputName.empty()) {
            item.source = place(fixture.compute(OperationKind::kConstant, sourceType, {}));
            graph.setAttr(graph.valueDef(item.source), "constValue", std::to_string(item.width) +
                (item.sign ? "'sd" : "'d") + std::to_string(item.bits));
            graph.setOpSymbol(graph.valueDef(item.source), graph.internSymbol("narrow_shift_constant_" + std::to_string(index)));
        } else item.source = fixture.input(item.inputName, item.width);
        // Explicit unsigned result types define the logical bit image even
        // for signed source constants; signed-result shift policy is separate.
        item.shifted = place(fixture.compute(OperationKind::kLShr, Fixture::type(item.width),
            {item.source, amounts[item.amount].value}));
        const auto shiftedData = place(fixture.compute(OperationKind::kLShr, Fixture::type(128), {data, item.shifted}));
        item.selected = place(fixture.compute(OperationKind::kSliceStatic, Fixture::type(1), {shiftedData}));
        graph.setAttr(graph.valueDef(item.selected), "sliceStart", int64_t{0});
        graph.setAttr(graph.valueDef(item.selected), "sliceEnd", int64_t{0});
        graph.bindOutputPort("shift_bit_" + std::to_string(index), item.selected);
        selectedBits.push_back(item.selected);
    }
    struct SignedResultCase {
        unsigned width; int64_t value; std::string inputName;
        grh::ValueId source{}, shifted{};
    };
    std::vector<SignedResultCase> signedResults;
    for (const auto width : {8u,16u,32u}) {
        for (const auto value : {int64_t{-1},int64_t{-3},-(int64_t{1} << (width-1))})
            signedResults.push_back({width,value,{}});
        signedResults.push_back({width,0,"shift_signed_runtime_s" + std::to_string(width)});
    }
    for (std::size_t index=0; index<signedResults.size(); ++index) {
        auto& item=signedResults[index];
        auto type=Fixture::type(item.width); type.isSigned=true;
        if (item.inputName.empty()) {
            item.source=place(fixture.compute(OperationKind::kConstant,type,{}));
            const auto bits=static_cast<uint64_t>(item.value)&((uint64_t{1}<<item.width)-1);
            graph.setAttr(graph.valueDef(item.source),"constValue",std::to_string(item.width)+"'sd"+std::to_string(bits));
            graph.setOpSymbol(graph.valueDef(item.source),graph.internSymbol("signed_result_lshr_constant_"+std::to_string(index)));
        } else {
            item.source=graph.createValue(graph.internSymbol(item.inputName),type);
            graph.bindInputPort(item.inputName,item.source);
        }
        item.shifted=place(fixture.compute(OperationKind::kLShr,type,{item.source,amounts[0].value}));
        graph.bindOutputPort("shift_signed_result_"+std::to_string(index),item.shifted);
        const auto low=place(fixture.compute(OperationKind::kSliceStatic,Fixture::type(1),{item.shifted}));
        graph.setAttr(graph.valueDef(low),"sliceStart",int64_t{0});
        graph.setAttr(graph.valueDef(low),"sliceEnd",int64_t{0});
        selectedBits.push_back(low);
    }
    const auto imported = graph.createOperation(OperationKind::kDpicImport, graph.internSymbol("audit_narrow_constant_shifts"));
    std::vector<std::string> argumentNames;
    std::string signature, audit;
    for (std::size_t index = 0; index < selectedBits.size(); ++index) {
        const auto argument = "bit" + std::to_string(index); argumentNames.push_back(argument);
        if (index) signature += ',';
        signature += "std::uint8_t " + argument;
        audit += "checksum=(checksum*33+" + argument + ")&255;\n";
    }
    graph.setAttr(imported, "argsName", argumentNames);
    graph.setAttr(imported, "argsDirection", std::vector<std::string>(selectedBits.size(), "input"));
    graph.setAttr(imported, "argsWidth", std::vector<int64_t>(selectedBits.size(), 1));
    graph.setAttr(imported, "argsSigned", std::vector<bool>(selectedBits.size(), false));
    graph.setAttr(imported, "hasReturn", true); graph.setAttr(imported, "returnWidth", int64_t{8});
    const auto call = graph.createOperation(OperationKind::kDpicCall);
    graph.setAttr(call, "targetImportSymbol", std::string("audit_narrow_constant_shifts"));
    graph.addOperand(call, one);
    for (const auto value : selectedBits) graph.addOperand(call, value);
    const auto checksum = graph.createValue(Fixture::type(8)); graph.addResult(call, checksum); place(checksum);
    graph.bindOutputPort("shift_checksum", checksum);
    run(fixture.design, "grh-to-grhsim", fixture.session); fixture.split();
    run(fixture.design, "grhsim-partition-nonsink", fixture.session, {"-clone-shared=0"});
    for (const auto op : placements) {
        const auto path = owner(graph, op);
        require(path.size() == 4 && path[0] == "general" && path[1] == "nonsink",
                "Narrow-shift fixture lost an original General nonsink operation");
        graph.setAttr(op, sim::kPartition, Owner{"general", "nonsink", "narrow_constant_shifts", "shared"});
    }
    run(fixture.design, "grhsim-partition-sink", fixture.session);
    const std::string support = R"CPP(
using ShiftWide = unsigned _BitInt(128);
// Divide the normalized source pattern, then query the selected source-data
// bit. This oracle never performs the generated dynamic 128-bit right shift.
static bool expectedShiftBit(std::uint64_t low, std::uint64_t high, std::uint64_t constant,
                             unsigned width, ShiftWide amount) {
    if (width < 64) constant %= std::uint64_t{1} << width;
    if (amount >= width) constant = 0;
    else for (unsigned remaining=static_cast<unsigned>(amount); remaining; --remaining) constant /= 2;
    if (constant >= 128) return false;
    auto word = constant < 64 ? low : high;
    for (unsigned remaining=static_cast<unsigned>(constant%64); remaining; --remaining) word /= 2;
    return word%2 != 0;
}
// Preserve the existing signed-result LShr behavior: a negative host scalar
// supplies sign-filled low bits below W, while counts at/above W return zero.
static std::int64_t expectedSignedLshr(std::int64_t value,unsigned width,ShiftWide amount) {
    if (amount>=width) return 0;
    for (unsigned remaining=static_cast<unsigned>(amount); remaining; --remaining)
        value=value/2-(value<0 && value%2!=0);
    return value;
}
)CPP" + std::string("extern \"C\" std::uint8_t audit_narrow_constant_shifts(") + signature +
        ") { unsigned checksum=0;\n" + audit + "return static_cast<std::uint8_t>(checksum); }\n";
    std::string prepare, verify;
    for (const auto& amount : amounts) {
        const auto port = "model.shift_amount_" + amount.name;
        prepare += port + "=static_cast<decltype(" + port + ")>(rawAmount);\n";
    }
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& item = cases[index]; const auto& amount = amounts[item.amount];
        if (!item.inputName.empty()) {
            const auto input = "model." + item.inputName;
            prepare += input + "=static_cast<decltype(" + input + ")>((pattern[0]^pattern[1]^"
                "static_cast<std::uint64_t>(rawAmount))+epoch*5);\n";
        }
        const auto port = "model.shift_amount_" + amount.name;
        const auto rawAmount = amount.width == 128 ? "ShiftWide(" + port + ")" :
            "(ShiftWide(" + port + ")&ShiftWide(UINT64_C(" +
                std::to_string(amount.width == 64 ? UINT64_MAX : (uint64_t{1} << amount.width) - 1) + ")))";
        const auto expected = "expected" + std::to_string(index);
        const auto source = item.inputName.empty() ? "UINT64_C(" + std::to_string(item.bits) + ')' :
            "static_cast<std::uint64_t>(model." + item.inputName + ')';
        verify += "const bool " + expected + "=expectedShiftBit(pattern[0],pattern[1]," +
            source + ',' + std::to_string(item.width) + ',' + rawAmount + ");\n";
        verify += "assert(model.shift_bit_" + std::to_string(index) + "==" + expected + "); "
            "checksum=(checksum*33+" + expected + ")&255;\n";
    }
    for (std::size_t index=0; index<signedResults.size(); ++index) {
        const auto& item=signedResults[index];
        std::string source=std::to_string(item.value);
        if (!item.inputName.empty()) {
            source="model."+item.inputName;
            prepare+=source+"=static_cast<decltype("+source+")>(epoch==0 ? -3 : epoch==1 ? -1 : "+
                std::to_string(-(int64_t{1}<<(item.width-1)))+");\n";
        }
        const auto expected="signedExpected"+std::to_string(index);
        verify+="const auto "+expected+"=expectedSignedLshr("+source+','+std::to_string(item.width)+
            ",ShiftWide(model.shift_amount_u32));\n";
        verify+="assert(static_cast<std::int64_t>(model.shift_signed_result_"+std::to_string(index)+")=="+expected+"); "
            "checksum=(checksum*33+(static_cast<std::uint64_t>("+expected+")&1))&255;\n";
    }
    const std::string body = R"CPP(
const std::array<ShiftWide,37> rawAmounts{
    0,1,2,3,4,5,6,7,8,9,15,16,17,30,31,32,33,63,64,65,127,128,255,256,65535,65536,
    ShiftWide(1)<<30, ShiftWide(1)<<31, (ShiftWide(1)<<31)|1,
    ShiftWide(1)<<32, (ShiftWide(1)<<32)|2, ShiftWide(1)<<63, (ShiftWide(1)<<63)|3,
    ShiftWide(1)<<64, (ShiftWide(1)<<64)|4, ShiftWide(1)<<100, ~ShiftWide(0)
};
const std::array<std::array<std::uint64_t,2>,6> patterns{{
    {0,0}, {UINT64_MAX,UINT64_MAX},
    {UINT64_C(0x0123456789abcdef),UINT64_C(0xfedcba9876543210)},
    {UINT64_C(0xaaaaaaaaaaaaaaaa),UINT64_C(0x5555555555555555)},
    {1,UINT64_C(0x8000000000000000)}, {UINT64_C(0x8000000000000000),1}
}};
for (unsigned epoch=0; epoch<3; ++epoch) {
    model.init();
    for (const auto rawAmount : rawAmounts) for (const auto& pattern : patterns) {
        model.shift_data=(ShiftWide(pattern[1])<<64)|pattern[0];
)CPP" + prepare + "model.eval(); unsigned checksum=0;\n" + verify + R"CPP(
        assert(model.shift_checksum==checksum);
    }
}
)CPP";
    for (const auto budget : {16,1024}) {
        const auto label = "narrow-constant-lshr-b" + std::to_string(budget);
        manualCase(fixture, label, budget, support, body, false);
        const auto directory = artifact(label) / "model";
        const auto functions = mappedRecords(readFile(directory / "code-map.json"), "functions");
        std::string sharedRoot;
        for (const auto op : placements) {
            const auto id = "\"o" + std::to_string(op.index) + "\""; std::string root;
            for (const auto& function : functions) if (function.find(id) != std::string::npos) {
                std::smatch match;
                require(std::regex_search(function, match, std::regex(R"REGEX("root_function":"([^"]+)")REGEX")),
                        "Narrow-shift function omitted its original owner");
                root = match[1].str(); break;
            }
            require(root.starts_with("sn_"), "Narrow-shift operation left its General owner or lost provenance");
            if (sharedRoot.empty()) sharedRoot = root;
            require(root == sharedRoot, "Narrow-shift sources and dependent shifts no longer share one General owner");
        }
        for (const auto& item : cases) if (item.inputName.empty())
            require(graph.opKind(graph.valueDef(item.source)) == OperationKind::kConstant,
                    "Narrow-shift regression no longer uses a true constant producer");
        for (const auto& item : signedResults) {
            require(graph.valueLogicalType(item.shifted).isSigned,
                    "Signed-result LShr regression lost its signed result type");
            if (item.inputName.empty()) require(graph.opKind(graph.valueDef(item.source))==OperationKind::kConstant,
                    "Signed-result LShr regression no longer uses a true constant producer");
        }
    }
}
}

int main() {
    try { testStreamingJsonWriter(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    // Boundary-role and bitmap cases are appended after the common models.
    // Append after testModels initializes the generated execution Makefile.
    // Literal-format cases join the same generated-model execution target.
    try { testChunkWriter(); testModels(); testNamedModelStores(); testActivityBatchOrder(); testMultipleNonsinkActivation(); testLargeStoreConstruction(); testChunkExecution(); testEffectsAndCalls(); testFirrtlLiteralFormats(); testDpiTriggerActivation(); testDpiIntegralAliases(); testDpiMixedTypes(); testArrayViewsAndWide(); testContiguousArrayImages(); testPackedBoolUnpack(); testLargeConcatExpressions(); testArrayViewElision(); testReadmemAndRandomInit(); testNonconvergence(); testMemoryActivationAndCancellation(); testWholeArrayBeforeImage(); testWaveformContents(); testExecutionCounters(); generateObservationModels(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    try { testNonsinkClockIsolation(); testSmallSinkBoundaries(); testSmallSinkPublishedEnables(); testSmallShadowWritePriority(); testSmallSinkActivityExactRuns(); testSmallSinkActivityCompetition(); testSmallSinkActivityInterruptedFallback(); testWriterDispatchFreshFireGroups(); testWriterDispatchNontrivialLocalFallback(); testScalarShadowChangeMasks(); testSinkActivityBatchCompetition(); testSinkActivityBatchMasksAndFallback(); testSinkActivityJoinedFire(); testSinkActivityJoinedCompetition(); testSinkActivityCurrentReads(); testSinkActivityLargeCurrentInterrupted(); testSinkActivityWholeCurrentWrites(); testSparsePhaseDispatch(); testMaskedViewBits(); testStaticArrayWindows(); testDynamicArrayBits(); testPackedArrayConstants(); testSinkRunCommonEnableFactors(); testSnapshotIdentityViews(); testLeafAssemblyViews(); testLeafAssemblyEdges(); testLeafAssemblyConstants(); testConditionalPackedScopes(); testGroupedNonsinkPublications(); testCompletePublicationKeys(); testImmutableConstantSources(); testLargeCaptureMetadata(); testLargeLatchCaptures(); testLargeCaptureInterruptedConstants(); testPrivateScalarMuxFusion(); testPrivateScalarMuxFusionDepthCutoff(); testNumericSelects(); testNarrowConstantLogicalShifts(); testSharedPayloadEnableSets(); testIndexedMemoryWake(); testIndexedMemoryWakeTemporal(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
