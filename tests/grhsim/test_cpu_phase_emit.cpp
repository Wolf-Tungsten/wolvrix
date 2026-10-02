#include "grhsim/backend/cpu_phase_emit.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/cone_extract.hpp"
#include "grhsim/pass/pass.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    using namespace wolvrix::lib;
    using namespace grhsim;

    void require(bool condition, std::string_view message)
    { if (!condition) throw std::runtime_error(std::string(message)); }

    std::string quote(std::string_view text)
    {
        std::string result = "'";
        for (char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
        return result + "'";
    }

    void command(const std::string &cmd)
    { std::cout << cmd << std::endl; require(std::system(cmd.c_str()) == 0, "generated six-phase CPU compilation or simulation failed"); }

    void runPass(GrhSimModel &model, std::string_view name, std::span<const std::string_view> args = {})
    {
        std::string error;
        auto pass = defaultPassRegistry().create(name, args, error);
        require(bool(pass), error);
        PassManager manager(defaultDialectRegistry());
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics;
        auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages())
            std::cout << message.context << ": " << message.message << '\n';
        require(result.success && !model.poisoned(), "six-phase pipeline pass failed: " + std::string(name));
    }

    // The M5d-6 pipeline: four M2 lowering passes, the A7 store
    // classification and B5 semantic phase attribution, then the C-segment
    // mapping (C1 build-general-nodes initializes the mapping; C2 merge; C3
    // layout; C4 bitmaps; C5 mem write plan; C6 function packing; C7 phase
    // schedule), closed by the M5d-7 C8 TU planning. allMem forces every
    // array into the mem store class for the P_mem-pinned fixtures; by
    // default small arrays classify regLatch and their writes become
    // General-phase NBA ops (M5d-6). tinyTu shrinks the C6/C8 size caps so a
    // small model still exercises chunked functions and multi-TU emit.
    void runSixPhasePipeline(GrhSimModel &model, bool splitSupernodes, bool allMem = false, bool tinyTu = false)
    {
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        if (allMem)
            runPass(model, "grhsim.select-state-stores",
                    std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        else
            runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
        if (splitSupernodes)
        {
            const std::array<std::string_view, 2> nodeCap{"--max-op-in-compute-node", "1"};
            const std::array<std::string_view, 2> supernodeCap{"--max-op-in-compute-supernode", "1"};
            runPass(model, "cpu.st.build-general-nodes", nodeCap);
            runPass(model, "cpu.st.merge-general-supernodes", supernodeCap);
        }
        else
        {
            runPass(model, "cpu.st.build-general-nodes");
            runPass(model, "cpu.st.merge-general-supernodes");
        }
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-bitmaps");
        runPass(model, "cpu.st.build-mem-write-plan");
        if (tinyTu)
            runPass(model, "cpu.st.pack-general-functions",
                    std::array<std::string_view, 2>{"--helper-max-estimated-lines", "32"});
        else
            runPass(model, "cpu.st.pack-general-functions");
        runPass(model, "cpu.st.build-phase-schedule");
        if (tinyTu)
            runPass(model, "cpu.st.plan-translation-units",
                    std::array<std::string_view, 4>{"--chunk-max-estimated-lines", "24",
                                                    "--unit-max-estimated-lines", "64"});
        else
            runPass(model, "cpu.st.plan-translation-units");
        require(model.cpuMapping() && model.cpuMapping()->stage == CpuMappingStage::TranslationUnits,
                "six-phase pipeline did not reach the TranslationUnits stage");
    }

    bool verifies(const GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        return verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics) &&
               !diagnostics.hasError();
    }

    ValueId addInputRead(GrhSimModel &model, const char *name, TypeId type)
    {
        const auto port = model.addInput(name, type);
        const auto value = model.addValue(type, name);
        model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
        return value;
    }

    void addOutputWrite(GrhSimModel &model, const char *name, TypeId type, ValueId value)
    {
        const auto port = model.addOutput(name, type);
        model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
    }

    ValueId addConstant(GrhSimModel &model, TypeId type, std::string literal)
    {
        const auto value = model.addValue(type);
        const std::array params{Parameter{model.intern("constValue"), std::move(literal)}};
        model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
        return value;
    }

    ValueId addCompute(GrhSimModel &model, const char *op, TypeId type, std::string_view resultName,
                       std::initializer_list<ValueId> operands)
    {
        const auto value = model.addValue(type, resultName);
        model.addOperation(op, {operands.begin(), operands.size()}, std::array{value});
        return value;
    }

    StateId addState(GrhSimModel &model, std::string_view name, TypeId type, std::string initLiteral)
    {
        const auto id = model.addState(name, type);
        const std::array initParams{Parameter{model.intern("value"), std::move(initLiteral)}};
        const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(id, steps, initParams);
        return id;
    }

    ValueId addStateRead(GrhSimModel &model, StateId state, std::string_view name)
    {
        const auto value = model.addValue(model.states()[state.index - 1].type, name);
        model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(state)});
        return value;
    }

    // Raw event annotation form (convert output since M5): trailing event
    // operands plus an event_edges parameter; object refs carry no event
    // slots. grhsim.lower-edge-detect rewrites this into edgeDet +
    // event_acts.
    void addEventWrite(GrhSimModel &model, std::string_view opType, std::vector<ValueId> operands,
                       StateId target, std::initializer_list<std::pair<ValueId, const char *>> events)
    {
        std::vector<ObjectRef> refs{ObjectRef::state(target)};
        std::vector<std::string> edges;
        for (const auto &[event, edge] : events)
        {
            operands.push_back(event);
            edges.push_back(edge);
        }
        if (edges.empty())
        {
            model.addOperation(opType, operands, {}, refs);
            return;
        }
        const std::array params{Parameter{model.intern("event_edges"), edges}};
        model.addOperation(opType, operands, {}, refs, params);
    }

    void addEventRegWrite(GrhSimModel &model, ValueId cond, ValueId next, ValueId mask,
                          StateId target,
                          std::initializer_list<std::pair<ValueId, const char *>> events)
    { addEventWrite(model, "core.state.regWrite", {cond, next, mask}, target, events); }

    unsigned bitmapBits(const CpuEventBitmap &bitmap)
    {
        unsigned count = 0;
        for (const auto word : bitmap.supernodeWords) count += static_cast<unsigned>(__builtin_popcountll(word));
        return count;
    }

    struct DriveStep
    {
        std::vector<std::pair<std::string, std::string>> assigns;
        std::vector<std::pair<std::string, std::string>> expects;
    };

    std::string driverSource(std::string_view top, const std::vector<DriveStep> &steps, bool captureStdout,
                             std::string_view prelude)
    {
        std::string src = "#include \"grhsim_" + std::string(top) + ".hpp\"\n"
            "#include <array>\n#include <cstdint>\n#include <cstdio>\n";
        src += prelude;
        src += "int main(){\nGrhSIM_" + std::string(top) + " sim;\n";
        if (captureStdout) src += "std::freopen(\"task_output.txt\",\"w\",stdout);\n";
        src += "sim.init();\nint fails=0;\n";
        std::size_t index = 0;
        for (const auto &step : steps)
        {
            for (const auto &[port, expr] : step.assigns) src += "sim." + port + "=" + expr + ";\n";
            src += "sim.eval();\n";
            for (const auto &[port, expr] : step.expects)
                src += "if(sim." + port + "!=(" + expr + ")){std::fprintf(stderr,\"step " + std::to_string(index) +
                       ": " + port + " mismatch\\n\");++fails;}\n";
            ++index;
        }
        src += "if(std::FILE *f=std::fopen(\"dump_state.txt\",\"w\")){sim.dumpState(f);std::fclose(f);}\n"
               "if(fails)return 1;\n";
        src += captureStdout ? "std::fflush(stdout);\nstd::fprintf(stderr,\"PASS\\n\");\nreturn 0;\n}\n"
                             : "std::printf(\"PASS\\n\");\nreturn 0;\n}\n";
        return src;
    }

    std::filesystem::path artifactRoot()
    {
        if (const auto *testOutput = std::getenv("WOLVRIX_CPU_PHASE_EMIT_TEST_OUTPUT"))
            return testOutput;
        return WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR;
    }

    // End-to-end: emit the mapped model, build it with the emitted Makefile,
    // compile a driver against the static library, run it. Returns the captured
    // task stdout when captureStdout is set (the driver freopens stdout).
    // expectMultiTu (M5d-7) additionally checks the emit produced several
    // translation units and that the generated Makefile lists exactly them.
    std::string compileAndRun(const GrhSimModel &model, const std::filesystem::path &directory,
                              const std::vector<DriveStep> &steps, bool captureStdout = false,
                              std::string_view prelude = {}, bool expectMultiTu = false)
    {
        const auto top = std::string(model.text(model.name()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        diag::Diagnostics diagnostics;
        const auto result = emitSixPhaseCpuCpp(model, directory / "model", diagnostics);
        for (const auto &message : diagnostics.messages())
            std::cout << message.context << ": " << message.message << '\n';
        require(result.success && !result.artifacts.empty(), "six-phase emit failed");
        if (expectMultiTu)
        {
            std::vector<std::string> sources;
            for (const auto &entry : std::filesystem::directory_iterator(directory / "model"))
                if (entry.path().extension() == ".cpp") sources.push_back(entry.path().filename().string());
            require(sources.size() > 1, "multi-TU emit produced a single translation unit");
            std::ifstream makefile(directory / "model" / "Makefile");
            const std::string text{std::istreambuf_iterator<char>(makefile), std::istreambuf_iterator<char>()};
            for (const auto &source : sources)
                require(text.find(source) != std::string::npos,
                        "generated Makefile misses TU source " + source);
        }
        {
            std::ofstream driver(directory / "driver.cpp");
            driver << driverSource(top, steps, captureStdout, prelude);
        }
        command("make --no-print-directory -C " + quote((directory / "model").string()) + " -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all'");
        command(quote(WOLVRIX_TEST_CXX) + " -std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all -I" +
                quote((directory / "model").string()) + " " + quote((directory / "driver.cpp").string()) + " " +
                quote((directory / "model" / ("libgrhsim_" + top + ".a")).string()) + " -o " +
                quote((directory / "driver").string()));
        command("cd " + quote(directory.string()) + " && ./driver");
        const auto dump = directory / "dump_state.txt";
        require(std::filesystem::exists(dump) && !std::filesystem::is_empty(dump), "dumpState produced no output");
        std::ifstream dumpStream(dump);
        const std::string dumpText{std::istreambuf_iterator<char>(dumpStream), std::istreambuf_iterator<char>()};
        require(dumpText.find("boundaryValueStore.") != std::string::npos, "dumpState misses the boundary store section");
        if (!captureStdout) return {};
        std::ifstream captured(directory / "task_output.txt");
        require(bool(captured), "stdout capture file missing");
        return {std::istreambuf_iterator<char>(captured), std::istreambuf_iterator<char>()};
    }

    std::vector<std::string> linesOf(const std::string &text)
    {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        for (std::string line; std::getline(stream, line);) lines.push_back(line);
        return lines;
    }

    // M5d-7: the emit writes one .cpp per translation unit; source-text
    // assertions read them all concatenated.
    std::string concatModelSources(const std::filesystem::path &modelDir)
    {
        std::vector<std::filesystem::path> sources;
        for (const auto &entry : std::filesystem::directory_iterator(modelDir))
            if (entry.path().extension() == ".cpp") sources.push_back(entry.path());
        std::sort(sources.begin(), sources.end());
        std::string text;
        for (const auto &path : sources)
        {
            std::ifstream stream(path);
            text += std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }
        return text;
    }

    // (a) input -> output passthrough: the whole cone extracts into P_output;
    // P_general stays empty and eval still tracks the input across evals.
    void passthroughTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_passthrough");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        addOutputWrite(model, "o", bit, a);
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        const auto &active = (*mapping.dataLayout->namedStores)[6];
        require(active.fields.size() == 3 && active.fields[0].aux == 0, "passthrough should have no supernodes");
        require(mapping.schedule->inputFanout.empty(), "passthrough should have no input fanout rows");
        compileAndRun(model, root / "passthrough", {
            {{{"a", "false"}}, {{"o", "false"}}},
            {{{"a", "true"}}, {{"o", "true"}}},
            {{{"a", "true"}}, {{"o", "true"}}},
            {{{"a", "false"}}, {{"o", "false"}}},
        });
    }

    // (b) 128-bit adder: wide values flow through the pointer-based runtime
    // helpers (P_output clone) with std::array port members.
    void wideAddTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_add128");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto wide = model.logicType(128, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", wide);
        const auto b = addInputRead(model, "b", wide);
        const auto sum = addCompute(model, "core.compute.add", wide, "sum", {a, b});
        addOutputWrite(model, "o", wide, sum);
        runSixPhasePipeline(model, false);
        const auto words = [](unsigned __int128 value) {
            const auto lo = static_cast<std::uint64_t>(value);
            const auto hi = static_cast<std::uint64_t>(value >> 64);
            return "std::array<std::uint64_t,2>{UINT64_C(" + std::to_string(lo) + "),UINT64_C(" + std::to_string(hi) + ")}";
        };
        const auto step = [&](unsigned __int128 av, unsigned __int128 bv) {
            const unsigned __int128 sum = av + bv;
            return DriveStep{{{"a", words(av)}, {"b", words(bv)}}, {{"o", words(sum)}}};
        };
        compileAndRun(model, root / "add128", {
            step(1, 2),
            step(static_cast<unsigned __int128>(~std::uint64_t(0)), 1), // carry across words
            step((static_cast<unsigned __int128>(~std::uint64_t(0)) << 64) | ~std::uint64_t(0), 1), // wraps to 0
            step(0xdeadbeefULL, 0x1234567890ULL),
        });
    }

    void scalarSliceArrayTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_scalar_slice_array");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto word = model.logicType(32, false, LogicDomain::TwoState);
        const auto indexType = model.logicType(4, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto data = addInputRead(model, "data", word);
        const auto index = addInputRead(model, "index", indexType);
        const auto selected = addCompute(model, "core.compute.sliceArray", byte, "selected", {data, index});
        addOutputWrite(model, "selected", byte, selected);
        require(verifies(model), "scalar sliceArray fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "scalar_slice_array", {
            {{{"data", "UINT32_C(0x44332211)"}, {"index", "0"}}, {{"selected", "0x11"}}},
            {{{"index", "1"}}, {{"selected", "0x22"}}},
            {{{"index", "3"}}, {{"selected", "0x44"}}},
            {{{"index", "4"}}, {{"selected", "0"}}},
            {{{"index", "15"}}, {{"selected", "0"}}},
        });
    }

    void randomSystemFunctionTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_random_function");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto word = model.logicType(32, false, LogicDomain::TwoState);
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        addOutputWrite(model, "echo", bit, addInputRead(model, "input", bit));
        const auto result = model.addValue(word, "random_value");
        const std::array params{Parameter{model.intern("name"), std::string("random")},
                                Parameter{model.intern("has_side_effects"), true}};
        model.addOperation("core.system.function", {}, std::array{result}, {}, params);
        addOutputWrite(model, "o", word, result);
        require(verifies(model), "random system function fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        std::vector<int64_t> sampleIds;
        bool sawGeneral = false, sawOutput = false;
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) != "core.system.function") continue;
            for (const auto &param : model.parameters(op))
                if (model.text(param.name) == "sample_id")
                    sampleIds.push_back(std::get<int64_t>(param.value));
            sawGeneral |= op.phase == SimPhase::General;
            sawOutput |= op.phase == SimPhase::Output;
        }
        require(sampleIds.size() == 2 && sampleIds[0] == sampleIds[1] && sawGeneral && sawOutput,
                "output-cone clone must share its random sample with the General producer");

        std::uint64_t rng = UINT64_C(0x6a09e667f3bcc909);
        const auto next = [&]() {
            rng += UINT64_C(0x9e3779b97f4a7c15);
            std::uint64_t value = rng;
            value = (value ^ (value >> 30u)) * UINT64_C(0xbf58476d1ce4e5b9);
            value = (value ^ (value >> 27u)) * UINT64_C(0x94d049bb133111eb);
            return std::to_string(static_cast<std::uint32_t>(value ^ (value >> 31u)));
        };
        compileAndRun(model, root / (tinyTu ? "random_function_tu" : "random_function"), {
            {{}, {{"o", next()}}},
            {{}, {{"o", next()}}},
            {{}, {{"o", next()}}},
        }, false, {}, tinyTu);
    }

    void randomSampleIdAfterCompactTest()
    {
        GrhSimModel model("phase_random_sample_ids");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto word = model.logicType(32, false, LogicDomain::TwoState);
        const std::array constParams{Parameter{model.intern("constValue"), std::string("32'd0")}};
        for (int i = 0; i < 2; ++i)
        {
            const auto value = model.addValue(word);
            model.addOperation("core.compute.constant", {}, std::array{value}, {}, constParams);
        }
        const std::array randomParams{Parameter{model.intern("name"), std::string("random")},
                                      Parameter{model.intern("has_side_effects"), true}};
        const auto first = model.addValue(word);
        model.addOperation("core.system.function", {}, std::array{first}, {}, randomParams, "first");
        const auto second = model.addValue(word);
        model.addOperation("core.system.function", {}, std::array{second}, {}, randomParams, "second");
        const std::array firstSink{first};
        extractCone(model, firstSink, SimPhase::Output);

        std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
        removeOps[1] = 1;
        model.compact(removeOps, std::vector<uint8_t>(model.states().size() + 1, 0));
        const auto secondAfterCompact = model.results(model.operations()[2]).front();
        const std::array secondSink{secondAfterCompact};
        extractCone(model, secondSink, SimPhase::Output);

        std::vector<int64_t> firstIds, secondIds;
        for (const auto &op : model.operations())
        {
            const auto name = model.text(op.name);
            auto &ids = name.starts_with("first") ? firstIds : secondIds;
            if (!name.starts_with("first") && !name.starts_with("second")) continue;
            for (const auto &param : model.parameters(op))
                if (model.text(param.name) == "sample_id") ids.push_back(std::get<int64_t>(param.value));
        }
        require(firstIds.size() == 2 && firstIds[0] == firstIds[1] &&
                secondIds.size() == 2 && secondIds[0] == secondIds[1] &&
                firstIds[0] != secondIds[0],
                "independent random operations share a sample ID after compact");
    }

    // (c) multi-output + one-to-many fanout: x = a^b feeds two output cones
    // (cloned once into P_output) and two event-free register writes (keeping
    // the original x on the General side as a boundary value with a two-target
    // supernode fanout). Node/supernode caps keep every op in its own
    // supernode so the boundary compare-store and the same-round successor
    // activation are exercised (ordinals ascend a->xor->{w1,w2}).
    void fanoutTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_fanout");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        const auto b = addInputRead(model, "b", bit);
        const auto x = addCompute(model, "core.compute.xor", bit, "x", {a, b});
        const auto one = addConstant(model, bit, "1'b1");
        const auto s1 = addState(model, "s1", bit, "1'b0");
        const auto s2 = addState(model, "s2", bit, "1'b0");
        const auto w1 = model.addOperation("core.state.regWrite", std::array{one, x, one}, {},
                                           std::array{ObjectRef::state(s1)});
        // latchWrite shares the event-free emitRegWrite path (latch writes never
        // carry events), so it is covered here rather than by a dedicated model.
        const auto w2 = model.addOperation("core.state.latchWrite", std::array{one, x, one}, {},
                                           std::array{ObjectRef::state(s2)});
        (void)w1; (void)w2;
        addOutputWrite(model, "o1", bit, x);
        addOutputWrite(model, "o2", bit, x);
        addOutputWrite(model, "o3", bit, addStateRead(model, s1, "r1"));
        addOutputWrite(model, "o4", bit, addStateRead(model, s2, "r2"));
        runSixPhasePipeline(model, true);
        const auto &mapping = *model.cpuMapping();
        const auto &stores = *mapping.dataLayout->namedStores;
        const auto &boundary = stores[2];
        const auto hasBoundaryField = [&](std::string_view name) {
            for (const auto &field : boundary.fields)
                if (model.text(field.name) == name) return true;
            return false;
        };
        require(hasBoundaryField("x"), "shared intermediate lost its boundary field");
        require(hasBoundaryField("a") && hasBoundaryField("b"), "input boundary fields missing");
        const auto &active = stores[6];
        require(active.fields.size() == 3 && active.fields[0].aux >= 3, "fanout model should have several supernodes");
        require(!mapping.schedule->computeSupernodeFanout.empty(), "fanout model should have supernode fanout rows");
        require(!mapping.schedule->eventBitmaps || mapping.schedule->eventBitmaps->empty(),
                "pure combinational model should have no event bitmaps");
        const std::vector<DriveStep> steps{
            {{{"a", "true"}, {"b", "false"}}, {{"o1", "true"}, {"o2", "true"}, {"o3", "true"}, {"o4", "true"}}},
            {{{"a", "false"}}, {{"o1", "false"}, {"o2", "false"}, {"o3", "false"}, {"o4", "false"}}},
            {{{"a", "true"}, {"b", "true"}}, {{"o1", "false"}, {"o2", "false"}, {"o3", "false"}, {"o4", "false"}}},
            {{{"b", "false"}}, {{"o1", "true"}, {"o2", "true"}, {"o3", "true"}, {"o4", "true"}}},
            {{}, {{"o1", "true"}, {"o2", "true"}, {"o3", "true"}, {"o4", "true"}}},
        };
        compileAndRun(model, root / "fanout", steps);
        // The emitted header must put every interface port in the first public
        // block's leading run of member declarations (contract §1.2) and the
        // Makefile must carry the LIB line.
        std::ifstream header(root / "fanout" / "model" / "grhsim_phase_fanout.hpp");
        const std::string text{std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>()};
        const auto classAt = text.find("class GrhSIM_phase_fanout");
        const auto publicAt = text.find("public:", classAt);
        require(classAt != std::string::npos && publicAt != std::string::npos, "emitted header misses the class");
        const auto ctorAt = text.find("GrhSIM_phase_fanout()", publicAt);
        require(ctorAt != std::string::npos, "emitted header misses the constructor");
        for (const char *port : {"a", "b", "o1", "o2", "o3", "o4"})
        {
            const auto memberAt = text.find(std::string(" ") + port + "{}", publicAt);
            require(memberAt != std::string::npos && memberAt < ctorAt,
                    std::string("port ") + port + " is not in the leading public member run");
        }
        require(text.find("bool a{};") != std::string::npos, "port a should be a bool member");
        std::ifstream makefile(root / "fanout" / "model" / "Makefile");
        const std::string makefileText{std::istreambuf_iterator<char>(makefile), std::istreambuf_iterator<char>()};
        require(makefileText.find("LIB := libgrhsim_phase_fanout.a") != std::string::npos, "emitted Makefile misses the LIB line");
        require(text.find("regLatchStoreNext") != std::string::npos && text.find("// state=s1") != std::string::npos,
                "regLatch store fields should be named and annotated");
        require(text.find("// value=x") != std::string::npos, "boundary store fields should carry the value comment");
    }

    // The registered pass rejects removed legacy options and models that did
    // not reach the TranslationUnits stage.
    void passRegistrationTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_pass_registration");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        addOutputWrite(model, "o", bit, a);
        std::string error;
        const std::array<std::string_view, 4> unknown{"--output", "x", "--unknown", "1"};
        require(!defaultPassRegistry().create("cpu.st.emit-cpp", unknown, error),
                "emit-phase-cpp accepted an unknown option");
        const auto outDir = root / "pass" / "model";
        std::filesystem::remove_all(outDir.parent_path());
        for (const auto key : {"--commit-compact-walk", "--commit-mem-walk", "--dynamic-stats",
                               "--shape-twin-share", "--branch-shape-share", "--branch-shape-hotness",
                               "--branch-shape-growth-budget", "--falling-edge-elision"})
        {
            const std::array<std::string_view, 4> legacy{"--output", "placeholder", key, "true"};
            require(!defaultPassRegistry().create("cpu.st.emit-cpp", legacy, error),
                    "emit-phase-cpp accepted removed option " + std::string(key));
        }
        {
            // A pre-PhaseSchedule mapping must fail the emit pass.
            GrhSimModel early = model.clone();
            runPass(early, "grhsim.extract-output-cones");
            runPass(early, "grhsim.split-phases");
            runPass(early, "cpu.st.build-general-nodes");
            std::vector<std::string_view> earlyArgs{"--output", "__should_not_exist__"};
            auto pass = defaultPassRegistry().create("cpu.st.emit-cpp", earlyArgs, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(early, diagnostics).success, "emit-phase-cpp accepted a pre-PhaseSchedule mapping");
        }
        runSixPhasePipeline(model, false);
        {
            // With the terminal stage, supported options emit the model and
            // report build-time limitations.
            const std::string out = outDir.string();
            const std::array<std::string_view, 6> args{"--output", out, "--waveform", "declared-symbols",
                                                       "--perf", "eval"};
            auto pass = defaultPassRegistry().create("cpu.st.emit-cpp", args, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            const auto result = pass->run(model, diagnostics);
            require(result.success, "emit-phase-cpp failed on a TranslationUnits mapping");
            require(diagnostics.messages().size() >= 2, "emit-phase-cpp omitted option diagnostics");
            require(std::filesystem::exists(outDir / "grhsim_phase_pass_registration.hpp"),
                    "emit-phase-cpp wrote no header");
        }
    }

    // (1) Counter self-loop: posedge clk drives cnt <= cnt + 1; every eval
    // converges, increments happen exactly on posedges, and a negedge updates
    // prev without firing (so the next posedge is seen). A second register
    // toggles on *both* clk edges (same signal, two clusters), which pins the
    // edge-direction encoding: negedges fire the both-edge detector only.
    void counterTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_counter");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto cnt = addState(model, "cnt", byte, "8'h00");
        const auto one = addConstant(model, bit, "1'b1");
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask = addConstant(model, byte, "8'hff");
        const auto cntRead = addStateRead(model, cnt, "cnt_r");
        const auto sum = addCompute(model, "core.compute.add", byte, "sum", {cntRead, one8});
        addEventRegWrite(model, one, sum, mask, cnt, {{clk, "posedge"}});
        const auto t = addState(model, "t", bit, "1'b0");
        const auto tRead = addStateRead(model, t, "t_r");
        const auto notT = addCompute(model, "core.compute.not", bit, "not_t", {tRead});
        addEventRegWrite(model, one, notT, one, t, {{clk, "both"}});
        addOutputWrite(model, "o", byte, cntRead);
        addOutputWrite(model, "ot", bit, tRead);
        require(verifies(model), "counter fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        const auto &mapping = *model.cpuMapping();
        const auto &stores = *mapping.dataLayout->namedStores;
        require(stores[3].fields.size() == 2, "counter should have two prevEvent slots");
        const auto &bitmaps = *mapping.schedule->eventBitmaps;
        // Each write supernode is private to its clock's bitmap; the shared
        // constant supernode (influence {0,1}) appears in both.
        require(bitmaps.size() == 2 && bitmapBits(bitmaps[0]) == 2 && bitmapBits(bitmaps[1]) == 2 &&
                    bitmaps[0].supernodeWords != bitmaps[1].supernodeWords,
                "counter bitmaps should cover the two write supernodes separately");
        compileAndRun(model, root / (tinyTu ? "counter_tu" : "counter"), {
            {{{"clk", "false"}}, {{"o", "0"}, {"ot", "false"}}},   // no edge at init
            {{{"clk", "true"}}, {{"o", "1"}, {"ot", "true"}}},     // posedge fires both detectors
            {{{"clk", "true"}}, {{"o", "1"}, {"ot", "true"}}},     // no input change, no re-fire
            {{{"clk", "false"}}, {{"o", "1"}, {"ot", "false"}}},   // negedge fires "both" only
            {{{"clk", "true"}}, {{"o", "2"}, {"ot", "true"}}},     // posedge seen again (prev was updated)
            {{{"clk", "false"}}, {{"o", "2"}, {"ot", "false"}}},
            {{{"clk", "true"}}, {{"o", "3"}, {"ot", "true"}}},
            {{{"clk", "false"}}, {{"o", "3"}, {"ot", "false"}}},
            {{{"clk", "true"}}, {{"o", "4"}, {"ot", "true"}}},
            {{{"clk", "false"}}, {{"o", "4"}, {"ot", "false"}}},
            {{{"clk", "true"}}, {{"o", "5"}, {"ot", "true"}}},
        }, false, {}, tinyTu);
    }

    // (2) A -> B NBA chain: at posedge clk, A <= in and B <= A in the same
    // round; B must sample the pre-publish A (the previous edge's value).
    void nbaChainTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_nba_chain");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto in = addInputRead(model, "in", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto a = addState(model, "a", bit, "1'b0");
        const auto b = addState(model, "b", bit, "1'b0");
        const auto aRead = addStateRead(model, a, "a_r");
        addEventRegWrite(model, one, in, one, a, {{clk, "posedge"}});
        addEventRegWrite(model, one, aRead, one, b, {{clk, "posedge"}});
        addOutputWrite(model, "oa", bit, aRead);
        addOutputWrite(model, "ob", bit, addStateRead(model, b, "b_r"));
        require(verifies(model), "nba chain fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventBitmaps->size() == 1, "nba chain should have one cluster");
        compileAndRun(model, root / "nba_chain", {
            {{{"in", "false"}, {"clk", "false"}}, {{"oa", "false"}, {"ob", "false"}}},
            {{{"in", "true"}}, {{"oa", "false"}, {"ob", "false"}}},   // no clock edge
            {{{"clk", "true"}}, {{"oa", "true"}, {"ob", "false"}}},   // B samples old A
            {{{"in", "false"}, {"clk", "false"}}, {{"oa", "true"}, {"ob", "false"}}},
            {{{"clk", "true"}}, {{"oa", "false"}, {"ob", "true"}}},   // B samples old A=1
            {{{"clk", "false"}}, {{"oa", "false"}, {"ob", "true"}}},
            {{{"clk", "true"}}, {{"oa", "false"}, {"ob", "false"}}},
            {{{"in", "true"}, {"clk", "false"}}, {{"oa", "false"}, {"ob", "false"}}},
            {{{"clk", "true"}}, {{"oa", "true"}, {"ob", "false"}}},
            {{{"clk", "false"}}, {{"oa", "true"}, {"ob", "false"}}},
            {{{"clk", "true"}}, {{"oa", "true"}, {"ob", "true"}}},
        });
    }

    // (3) Power-on: prev init == event init, so the first evals (no input
    // toggle) must not report an edge; q <= 1 fires only on a real posedge.
    void powerOnTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_power_on");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        addEventRegWrite(model, one, one, one, q, {{clk, "posedge"}});
        addOutputWrite(model, "o", bit, qRead);
        require(verifies(model), "power-on fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "power_on", {
            {{}, {{"o", "false"}}},   // first eval, no input write: no spurious edge
            {{}, {{"o", "false"}}},
            {{{"clk", "true"}}, {{"o", "true"}}},
            {{}, {{"o", "true"}}},
            {{{"clk", "false"}}, {{"o", "true"}}},
            {{{"clk", "true"}}, {{"o", "true"}}},
        });
    }

    // (4) Repeated asynchronous resets: rst is both an event (negedge) and a
    // data-path user (the reset-value mux select), so P_input keeps flagging
    // it and repeated reset pulses are never swallowed by the && gating.
    void asyncResetTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_async_reset");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto rst = addInputRead(model, "rst", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto zero = addConstant(model, bit, "1'b0");
        const auto one = addConstant(model, bit, "1'b1");
        const auto next = addCompute(model, "core.compute.mux", bit, "next", {rst, d, zero});
        const auto q = addState(model, "q", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        addEventRegWrite(model, one, next, one, q, {{clk, "posedge"}, {rst, "negedge"}});
        addOutputWrite(model, "o", bit, qRead);
        require(verifies(model), "async reset fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventBitmaps->size() == 2, "async reset should have two clusters");
        compileAndRun(model, root / "async_reset", {
            {{{"d", "true"}, {"rst", "true"}, {"clk", "false"}}, {{"o", "false"}}},
            {{{"clk", "true"}}, {{"o", "true"}}},    // posedge clk: q <= d
            {{{"clk", "false"}}, {{"o", "true"}}},   // negedge clk: not in the act set
            {{{"rst", "false"}}, {{"o", "false"}}},  // negedge rst: async reset
            {{{"clk", "true"}}, {{"o", "false"}}},   // posedge clk with rst low: reset dominates
            {{{"rst", "true"}}, {{"o", "false"}}},   // posedge rst: not in the act set
            {{{"clk", "false"}}, {{"o", "false"}}},
            {{{"clk", "true"}}, {{"o", "true"}}},    // posedge clk: q <= d again
            {{{"rst", "false"}}, {{"o", "false"}}},  // second async reset
            {{{"rst", "true"}}, {{"o", "false"}}},
            {{{"clk", "false"}}, {{"o", "false"}}},
            {{{"clk", "true"}}, {{"o", "true"}}},
            {{{"rst", "false"}}, {{"o", "false"}}},  // third async reset
            {{{"d", "false"}, {"rst", "true"}, {"clk", "false"}}, {{"o", "false"}}},
            {{{"clk", "true"}}, {{"o", "false"}}},   // d is 0 now
        });
    }

    // (5) clk0/clk1 dual domains: one cluster per clock, each bitmap covering
    // exactly its own counter's supernode. Toggling one clock must neither
    // fire the other domain (bitmap isolation) nor drop its pending data
    // activation (a spurious fire would clear the sticky flag and lose it).
    void dualClockTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_dual_clock");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto one = addConstant(model, bit, "1'b1");
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask = addConstant(model, byte, "8'hff");
        const auto counter = [&](const char *clkName, const char *cntName) {
            const auto clk = addInputRead(model, clkName, bit);
            const auto cnt = addState(model, cntName, byte, "8'h00");
            const auto cntRead = addStateRead(model, cnt, "cnt_r");
            const auto sum = addCompute(model, "core.compute.add", byte, "sum", {cntRead, one8});
            addEventRegWrite(model, one, sum, mask, cnt, {{clk, "posedge"}});
            return cntRead;
        };
        const auto c0 = counter("clk0", "c0");
        const auto c1 = counter("clk1", "c1");
        addOutputWrite(model, "o0", byte, c0);
        addOutputWrite(model, "o1", byte, c1);
        require(verifies(model), "dual clock fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        const auto &bitmaps = *mapping.schedule->eventBitmaps;
        // The shared constant supernode influences both domains and appears in
        // both bitmaps; each counter supernode is private to its own bitmap, so
        // the two bitmaps must differ. Functional isolation is asserted by the
        // driver (a clk0 edge must never fire the clk1 counter's write).
        require(bitmaps.size() == 2 && bitmapBits(bitmaps[0]) >= 1 && bitmapBits(bitmaps[1]) >= 1,
                "dual clock should have two nonempty bitmaps");
        require(bitmaps[0].supernodeWords != bitmaps[1].supernodeWords,
                "dual clock bitmaps must not be identical");
        compileAndRun(model, root / "dual_clock", {
            {{{"clk0", "false"}, {"clk1", "false"}}, {{"o0", "0"}, {"o1", "0"}}},
            {{{"clk0", "true"}}, {{"o0", "1"}, {"o1", "0"}}},
            {{{"clk1", "true"}}, {{"o0", "1"}, {"o1", "1"}}},
            {{{"clk0", "false"}, {"clk1", "false"}}, {{"o0", "1"}, {"o1", "1"}}},
            {{{"clk0", "true"}}, {{"o0", "2"}, {"o1", "1"}}},
            {{{"clk1", "true"}}, {{"o0", "2"}, {"o1", "2"}}},
            {{{"clk0", "false"}}, {{"o0", "2"}, {"o1", "2"}}},
            {{{"clk1", "false"}}, {{"o0", "2"}, {"o1", "2"}}},
            {{{"clk0", "true"}}, {{"o0", "3"}, {"o1", "2"}}},
            {{{"clk1", "true"}}, {{"o0", "3"}, {"o1", "3"}}},
        });
    }

    // (6) Glitch clock / multiple edges per eval. Plan §72 (literal): "同一
    // eval 内事件信号跨 round 翻转会产生多个边沿（glitch 时钟下同 eval 可能
    // 多次写同一 reg），与 event-driven 仿真行为一致" — P_event re-runs every
    // round, so a state-derived event that flips at publish produces a fresh
    // edge in the next round of the same eval. Model: q toggles at posedge
    // clk, and q itself is the posedge clock of q2 <= d; q2 therefore samples
    // d one round after q rises, inside the same eval as the clk edge. The
    // driver-side half (逐 eval 边沿判定与 prev 更新) toggles clk across evals:
    // negedges never fire the posedge detectors but still refresh prev.
    void glitchClockTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_glitch_clock");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        const auto notQ = addCompute(model, "core.compute.not", bit, "not_q", {qRead});
        addEventRegWrite(model, one, notQ, one, q, {{clk, "posedge"}});
        addEventRegWrite(model, one, d, one, q2, {{qRead, "posedge"}});
        addOutputWrite(model, "oq", bit, qRead);
        addOutputWrite(model, "oq2", bit, addStateRead(model, q2, "q2_r"));
        require(verifies(model), "glitch clock fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventBitmaps->size() == 2, "glitch clock should have two clusters");
        compileAndRun(model, root / (tinyTu ? "glitch_clock_tu" : "glitch_clock"), {
            {{{"d", "true"}, {"clk", "false"}}, {{"oq", "false"}, {"oq2", "false"}}},
            {{{"clk", "true"}}, {{"oq", "true"}, {"oq2", "true"}}},    // q rises; q2 samples d=1 in round 2
            {{{"clk", "false"}}, {{"oq", "true"}, {"oq2", "true"}}},
            {{{"clk", "true"}}, {{"oq", "false"}, {"oq2", "true"}}},   // q falls: no posedge for q2
            {{{"d", "false"}, {"clk", "false"}}, {{"oq", "false"}, {"oq2", "true"}}},
            {{{"clk", "true"}}, {{"oq", "true"}, {"oq2", "false"}}},   // q rises; q2 <= d=0
            {{{"clk", "false"}}, {{"oq", "true"}, {"oq2", "false"}}},
            {{{"clk", "true"}}, {{"oq", "false"}, {"oq2", "false"}}},  // q falls again
            {{{"d", "true"}, {"clk", "false"}}, {{"oq", "false"}, {"oq2", "false"}}},
            {{{"clk", "true"}}, {{"oq", "true"}, {"oq2", "true"}}},    // q2 <= d=1
        }, false, {}, tinyTu);
    }

    StateId addMemState(GrhSimModel &model, std::string_view name, TypeId arrayType, std::string fillLiteral)
    {
        const auto id = model.addState(name, arrayType);
        const std::array params{Parameter{model.intern("value"), std::move(fillLiteral)}};
        const std::array steps{InitStep{model.intern("core.init.fill"), {0, 1}}};
        model.addInit(id, steps, params);
        return id;
    }

    ValueId addMemRead(GrhSimModel &model, StateId mem, TypeId element, ValueId address, std::string_view name)
    {
        const auto value = model.addValue(element, name);
        model.addOperation("core.state.memRead", std::array{address}, std::array{value},
                           std::array{ObjectRef::state(mem)});
        return value;
    }

    // (7) Latch ring: la <= lb | setA, lb <= la & keepB, both level-sensitive
    // latchWrite ops with no events. The ring settles through the round loop
    // (write -> stateFanout -> next round), holds its value when inputs
    // release, and clears within one eval when keepB drops. Every eval must
    // converge (no "did not converge" throw).
    void latchRingTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_latch_ring");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto setA = addInputRead(model, "setA", bit);
        const auto keepB = addInputRead(model, "keepB", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto la = addState(model, "la", bit, "1'b0");
        const auto lb = addState(model, "lb", bit, "1'b0");
        const auto laRead = addStateRead(model, la, "la_r");
        const auto lbRead = addStateRead(model, lb, "lb_r");
        const auto orV = addCompute(model, "core.compute.or", bit, "or_v", {lbRead, setA});
        const auto andV = addCompute(model, "core.compute.and", bit, "and_v", {laRead, keepB});
        model.addOperation("core.state.latchWrite", std::array{one, orV, one}, {}, std::array{ObjectRef::state(la)});
        model.addOperation("core.state.latchWrite", std::array{one, andV, one}, {}, std::array{ObjectRef::state(lb)});
        addOutputWrite(model, "oa", bit, laRead);
        addOutputWrite(model, "ob", bit, lbRead);
        require(verifies(model), "latch ring fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "latch_ring", {
            {{{"setA", "false"}, {"keepB", "true"}}, {{"oa", "false"}, {"ob", "false"}}},
            {{{"setA", "true"}}, {{"oa", "true"}, {"ob", "true"}}},   // ring propagates and settles
            {{{"setA", "false"}}, {{"oa", "true"}, {"ob", "true"}}},  // ring holds (level-sensitive)
            {{{"keepB", "false"}}, {{"oa", "false"}, {"ob", "false"}}}, // clears within one eval
            {{{"keepB", "true"}, {"setA", "true"}}, {{"oa", "true"}, {"ob", "true"}}},
            {{}, {{"oa", "true"}, {"ob", "true"}}},
        });
    }

    // (8) Event-free mem writes converge via cell change detection: memWrite
    // (single cell), memFill (broadcast), memAssign (whole-array copy from an
    // initialized source). Every write re-runs every round while enabled, so
    // any missed change detection would loop to MAX_ROUND and throw.
    void memConvergeTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_converge");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(word, 16);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto fen = addInputRead(model, "fen", bit);
        const auto fdata = addInputRead(model, "fdata", word);
        const auto cen = addInputRead(model, "cen", bit);
        const auto mask = addConstant(model, word, "8'hff");
        const auto memA = addMemState(model, "memA", memType, "8'h00");
        const auto memF = addMemState(model, "memF", memType, "8'ha5");
        const auto memC = addMemState(model, "memC", memType, "8'h00");
        addEventWrite(model, "core.state.memWrite", {wen, waddr, wdata, mask}, memA, {});
        addEventWrite(model, "core.state.memFill", {fen, fdata}, memF, {});
        const auto fRead = model.addValue(memType, "f_read");
        model.addOperation("core.state.read", {}, std::array{fRead}, std::array{ObjectRef::state(memF)});
        addEventWrite(model, "core.state.memAssign", {cen, fRead}, memC, {});
        addOutputWrite(model, "o", word, addMemRead(model, memA, word, raddr, "ra"));
        addOutputWrite(model, "of", word, addMemRead(model, memF, word, raddr, "rf"));
        addOutputWrite(model, "oc", word, addMemRead(model, memC, word, raddr, "rc"));
        require(verifies(model), "mem converge fixture rejected");
        runSixPhasePipeline(model, false, true);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->memWritePlan && mapping.schedule->memWritePlan->size() == 3,
                "mem converge should have three write plan entries");
        for (const auto &entry : *mapping.schedule->memWritePlan)
            require(entry.eventFree, "mem converge writes must be event-free");
        compileAndRun(model, root / "mem_converge", {
            {{{"wen", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"raddr", "0"},
              {"fen", "false"}, {"fdata", "0"}, {"cen", "false"}},
             {{"o", "0"}, {"of", "165"}, {"oc", "0"}}},   // memF init 0xa5; memC still 0 (cen=0)
            {{{"cen", "true"}}, {{"oc", "165"}}},          // one-shot copy of the (never rewritten) memF
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "90"}, {"raddr", "3"}}, {{"o", "90"}}},
            {{}, {{"o", "90"}, {"oc", "165"}}},            // stable, no re-activation loop
            {{{"wdata", "119"}}, {{"o", "119"}}},          // overwrite the same cell
            {{{"wen", "false"}}, {{"o", "119"}}},
            {{{"fen", "true"}, {"fdata", "153"}}, {{"of", "153"}, {"oc", "153"}}},  // fill memF; the copy now tracks (D3)
            {{}, {{"of", "153"}, {"oc", "153"}}},          // fill re-runs every round, converges
            {{{"cen", "false"}}, {{"oc", "153"}}},
        });
    }

    // (9) Static/dynamic reader discrimination: an event-gated memWrite with a
    // dynamic address; one reader reads the constant row 3, one reads a dynamic
    // address. The write's readers table must carry the static row for the
    // former (emit: conditional activation on overlap) and none for the latter
    // (emit: unconditional on any change). Functionally, a write to row 5 must
    // refresh only the dynamic reader's latch; a write to row 3 refreshes both.
    void memReaderGatingTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_gating");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(word, 16);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto one = addConstant(model, bit, "1'b1");
        const auto three = addConstant(model, addrType, "4'h3");
        const auto mask = addConstant(model, word, "8'hff");
        const auto mem = addMemState(model, "mem", memType, "8'h00");
        addEventWrite(model, "core.state.memWrite", {wen, waddr, wdata, mask}, mem, {{clk, "posedge"}});
        const auto rStatic = addMemRead(model, mem, word, three, "r_static");
        const auto rDyn = addMemRead(model, mem, word, raddr, "r_dyn");
        const auto latchS = addState(model, "latch_s", word, "8'h00");
        const auto latchD = addState(model, "latch_d", word, "8'h00");
        model.addOperation("core.state.latchWrite", std::array{one, rStatic, mask}, {}, std::array{ObjectRef::state(latchS)});
        model.addOperation("core.state.latchWrite", std::array{one, rDyn, mask}, {}, std::array{ObjectRef::state(latchD)});
        addOutputWrite(model, "os", word, addStateRead(model, latchS, "ls_r"));
        addOutputWrite(model, "od", word, addStateRead(model, latchD, "ld_r"));
        require(verifies(model), "mem gating fixture rejected");
        // Default merge may coalesce the write's operand producers with the
        // event-free mem-reader/latch chain into one supernode; that supernode
        // holds no event-carrying op, so the emit side gates it on data alone
        // and the P_mem reader re-activation (dataActiveFlagNext) fires it.
        runSixPhasePipeline(model, false, true);
        const auto &mapping = *model.cpuMapping();
        const auto &plan = *mapping.schedule->memWritePlan;
        require(plan.size() == 1 && !plan[0].eventFree, "mem gating write should be event-gated");
        require(plan[0].readers.size() == 2, "mem gating write should have two readers");
        unsigned staticReaders = 0;
        for (const auto &reader : plan[0].readers)
        {
            if (reader.staticRow) { ++staticReaders; require(*reader.staticRow == 3, "static reader row wrong"); }
        }
        require(staticReaders == 1, "mem gating should have exactly one static-row reader");
        compileAndRun(model, root / "mem_gating", {
            {{{"clk", "false"}, {"wen", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"raddr", "0"}},
             {{"os", "0"}, {"od", "0"}}},
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "17"}, {"raddr", "3"}}, {{"os", "0"}, {"od", "0"}}},
            {{{"clk", "true"}}, {{"os", "17"}, {"od", "17"}}},    // write hits row 3: both readers refresh
            {{{"clk", "false"}}, {{"os", "17"}, {"od", "17"}}},
            {{{"waddr", "5"}, {"wdata", "34"}, {"raddr", "5"}}, {{"os", "17"}, {"od", "0"}}},   // raddr 3->5 re-reads mem[5]=0 (no clk edge: no write)
            {{{"clk", "true"}}, {{"os", "17"}, {"od", "34"}}},    // write hits row 5: static reader not re-armed
            {{{"clk", "false"}}, {{"os", "17"}, {"od", "34"}}},
            {{{"waddr", "3"}, {"wdata", "51"}, {"raddr", "3"}}, {{"os", "17"}, {"od", "17"}}},  // raddr 5->3 re-reads mem[3]=17
            {{{"clk", "true"}}, {{"os", "51"}, {"od", "51"}}},    // hit row 3 again
        });
        const std::string text = concatModelSources(root / "mem_gating" / "model");
        // Two reader activations in the P_mem write chunk: one guarded (static
        // row 3), one unconditional (dynamic address).
        const auto pMemAt = text.find("::pMem_c0()");
        require(pMemAt != std::string::npos, "pMem chunk body missing");
        const auto pMemEnd = text.find("::pPublish()", pMemAt);
        const std::string pMem = text.substr(pMemAt, pMemEnd == std::string::npos ? pMemEnd : pMemEnd - pMemAt);
        require(pMem.find("==3)dataActiveFlagNext[") != std::string::npos,
                "static reader should be activated under an address-overlap guard");
        unsigned activations = 0;
        for (std::size_t at = 0; (at = pMem.find("dataActiveFlagNext[", at)) != std::string::npos; ++at) ++activations;
        require(activations == 2, "write body should activate exactly its two readers");
    }

    // (10) memWriteSeq priority: ports apply in operand order, so the LAST
    // triple wins an address collision (core.md: the chain head sits last).
    // Three ports: A writes row 3 a constant 0xaa, B overwrites row 3 with
    // input data, C (highest) writes input address/data under an enable.
    void memPriorityTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_mem_priority");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(word, 16);
        const auto clk = addInputRead(model, "clk", bit);
        const auto enc = addInputRead(model, "enc", bit);
        const auto bdata = addInputRead(model, "bdata", word);
        const auto caddr = addInputRead(model, "caddr", addrType);
        const auto cdata = addInputRead(model, "cdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto one = addConstant(model, bit, "1'b1");
        const auto three = addConstant(model, addrType, "4'h3");
        const auto aa = addConstant(model, word, "8'haa");
        const auto mem = addMemState(model, "mem", memType, "8'h00");
        addEventWrite(model, "core.state.memWriteSeq", {one, three, aa, one, three, bdata, enc, caddr, cdata},
                       mem, {{clk, "posedge"}});
        addOutputWrite(model, "o", word, addMemRead(model, mem, word, raddr, "r"));
        require(verifies(model), "mem priority fixture rejected");
        runSixPhasePipeline(model, false, true, tinyTu);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->memWritePlan->size() == 1, "mem priority should have one write op");
        compileAndRun(model, root / (tinyTu ? "mem_priority_tu" : "mem_priority"), {
            {{{"clk", "false"}, {"enc", "false"}, {"bdata", "0"}, {"caddr", "0"}, {"cdata", "0"}, {"raddr", "3"}},
             {{"o", "0"}}},
            {{{"bdata", "187"}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "187"}}},   // A writes 0xaa, B overwrites 0xbb, C disabled
            {{{"clk", "false"}}, {{"o", "187"}}},
            {{{"bdata", "204"}, {"clk", "true"}}, {{"o", "204"}}},
            {{{"enc", "true"}, {"caddr", "3"}, {"cdata", "221"}, {"clk", "false"}}, {{"o", "204"}}},
            {{{"clk", "true"}}, {{"o", "221"}}},   // C (last port) wins row 3
            {{{"caddr", "7"}, {"cdata", "119"}, {"raddr", "7"}, {"clk", "false"}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "119"}}},   // C writes row 7; A/B keep row 3
            {{{"raddr", "3"}}, {{"o", "204"}}},    // row 3 got B's value again this edge (C hit row 7)
            {{{"enc", "false"}, {"clk", "false"}}, {{"o", "204"}}},
            {{{"clk", "true"}}, {{"o", "204"}}},
        }, false, {}, tinyTu);
    }

    // (11) General-phase $display: an event-free task prints on every firing of
    // its (always-active) supernode; an event-guarded task prints only when its
    // supernode fires — eventActiveFlag && dataActiveFlag (plan §94). Per the
    // documented double-gate semantics (plan §100), an edge with no data change
    // since the last fire does NOT re-fire the task supernode; that is a known
    // divergence from IEEE $display-on-every-edge (see the slice report).
    void generalDisplayTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_display");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto a = addInputRead(model, "a", bit);
        const auto d = addInputRead(model, "d", byte);
        const auto one = addConstant(model, bit, "1'b1");
        const auto stringConst = [&](std::string text) {
            const auto value = model.addValue(model.stringType());
            const std::array params{Parameter{model.intern("constValue"), std::move(text)}};
            model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
            return value;
        };
        const auto taskParams = [&](std::string name) {
            return std::vector{Parameter{model.intern("name"), std::move(name)},
                               Parameter{model.intern("proc_kind"), std::string("always")},
                               Parameter{model.intern("has_timing"), false}};
        };
        model.addOperation("core.system.task", std::array{one, stringConst("comb a=%0d"), a}, {},
                           {}, taskParams("display"));
        {
            auto params = taskParams("display");
            params.push_back(Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}});
            model.addOperation("core.system.task", std::array{one, stringConst("ev d=%0d"), d, clk}, {},
                               {}, params);
        }
        require(verifies(model), "display fixture rejected");
        // Default merge keeps the event-free display out of the event domain
        // (merge prohibition: an op with no event obligation never joins an
        // event-carrying supernode).
        runSixPhasePipeline(model, false);
        const std::vector<DriveStep> steps{
            {{{"a", "false"}, {"d", "0"}, {"clk", "false"}}, {}},   // init fire: comb a=0
            {{{"a", "true"}}, {}},                                   // comb a=1
            {{{"d", "1"}}, {}},                                      // data-only change: no print
            {{{"clk", "true"}}, {}},                                 // edge: ev d=1
            {{{"clk", "false"}}, {}},                                // nothing
            {{{"a", "false"}}, {}},                                  // comb a=0
            {{{"clk", "true"}}, {}},                                 // edge, d unchanged since last fire: no print (documented gap)
            {{{"d", "2"}, {"clk", "false"}}, {}},                    // nothing
            {{{"clk", "true"}}, {}},                                 // ev d=2
        };
        const auto output = compileAndRun(model, root / "display", steps, true);
        const auto lines = linesOf(output);
        const std::vector<std::string> expected{"comb a=0", "comb a=1", "ev d=1", "comb a=0", "ev d=2"};
        require(lines == expected, "display output mismatch: got [" + std::string([&] {
                    std::string joined; for (const auto &line : lines) joined += line + "|"; return joined; }()) + "]");
    }

    // (12) Event-free $monitor: migrate-timeslot-tasks moves it to P_output with
    // __tslot_prev history states and a changed-reduction guard. It reports at
    // most once per eval and only when an operand changed since the last eval
    // (the initial all-zero state matches prev, so eval 1 does not report).
    void monitorFreeTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_monitor_free");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        const auto b = addInputRead(model, "b", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const std::array params{Parameter{model.intern("name"), std::string("monitor")},
                                Parameter{model.intern("proc_kind"), std::string("always")},
                                Parameter{model.intern("has_timing"), false}};
        model.addOperation("core.system.task", std::array{one, a, b}, {}, {}, params);
        require(verifies(model), "monitor fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        const auto &timeslot = (*mapping.dataLayout->namedStores)[5];
        require(timeslot.fields.empty(), "event-free monitor must not take a timeslot flag");
        const std::string text = compileAndRun(model, root / "monitor_free", {
            {{{"a", "false"}, {"b", "false"}}, {}},
            {{{"a", "true"}}, {}},
            {{}, {}},
            {{{"b", "true"}}, {}},
            {{{"a", "false"}}, {}},
            {{}, {}},
        }, true);
        const auto lines = linesOf(text);
        // The callCond slot also gets a __tslot_prev history (const 1 vs prev 0),
        // so eval 1 reports the initial state — matching SV $monitor's time-0
        // report. After that, only evals with a real operand change report.
        const std::vector<std::string> expected{"0 0", "1 0", "1 1", "0 1"};
        require(lines == expected, "monitor output mismatch: got [" + std::string([&] {
                    std::string joined; for (const auto &line : lines) joined += line + "|"; return joined; }()) + "]");
    }

    // (13) Event-driven timeslot task ($strobe on posedge clk): P_event sets
    // the eval-sticky timeslotTriggerFlag on the edge; P_output consumes and
    // clears it. The flag is data-independent, so every posedge reports.
    void monitorEventTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_monitor_event");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto a = addInputRead(model, "a", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const std::array params{Parameter{model.intern("name"), std::string("strobe")},
                                Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}},
                                Parameter{model.intern("proc_kind"), std::string("always")},
                                Parameter{model.intern("has_timing"), false}};
        model.addOperation("core.system.task", std::array{one, a, clk}, {},
                           {}, params);
        require(verifies(model), "strobe fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        const auto &mapping = *model.cpuMapping();
        const auto &timeslot = (*mapping.dataLayout->namedStores)[5];
        require(timeslot.fields.size() == 1, "event strobe should hold one timeslot flag");
        require(mapping.schedule->timeslotTriggers && !mapping.schedule->timeslotTriggers->empty(),
                "event strobe should have a trigger mapping");
        const std::string text = compileAndRun(model, root / (tinyTu ? "monitor_event_tu" : "monitor_event"), {
            {{{"a", "false"}, {"clk", "false"}}, {}},
            {{{"a", "true"}}, {}},
            {{{"clk", "true"}}, {}},    // posedge: print "1"
            {{{"clk", "true"}}, {}},    // no new edge: no print
            {{{"clk", "false"}}, {}},
            {{{"a", "false"}}, {}},
            {{{"clk", "true"}}, {}},    // posedge: print "0"
            {{{"clk", "false"}}, {}},
            {{{"clk", "true"}}, {}},    // posedge: print "0" again (data-independent flag)
        }, true, {}, tinyTu);
        const auto lines = linesOf(text);
        const std::vector<std::string> expected{"1", "0", "0"};
        require(lines == expected, "strobe output mismatch: got [" + std::string([&] {
                    std::string joined; for (const auto &line : lines) joined += line + "|"; return joined; }()) + "]");
    }

    // (14) DPI smoke: an event-free import call (result republished through a
    // boundary field into a latch) and a posedge-gated import call. The driver
    // supplies the extern "C" definitions.
    void dpiSmokeTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_dpi_smoke");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto a = addInputRead(model, "a", byte);
        const auto d = addInputRead(model, "d", byte);
        const auto one = addConstant(model, bit, "1'b1");
        const auto mask = addConstant(model, byte, "8'hff");
        const std::array incArgs{DpiArgument{model.intern("x"), DpiDirection::Input, byte}};
        const auto inc = model.addExternFunction("cpu_test_inc", "core.dpi", "cpu_test_inc", incArgs, byte);
        const auto y = model.addValue(byte, "y");
        model.addOperation("core.dpi.call", std::array{one, a}, std::array{y},
                           std::array{ObjectRef::function(inc)});
        const std::array dblArgs{DpiArgument{model.intern("x"), DpiDirection::Input, byte}};
        const auto dbl = model.addExternFunction("cpu_test_dbl", "core.dpi", "cpu_test_dbl", dblArgs, byte);
        const auto z = model.addValue(byte, "z");
        {
            const std::array params{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            model.addOperation("core.dpi.call", std::array{one, d, clk}, std::array{z},
                               std::array{ObjectRef::function(dbl)}, params);
        }
        const auto q = addState(model, "q", byte, "8'h00");
        const auto q2 = addState(model, "q2", byte, "8'h00");
        model.addOperation("core.state.latchWrite", std::array{one, y, mask}, {}, std::array{ObjectRef::state(q)});
        model.addOperation("core.state.latchWrite", std::array{one, z, mask}, {}, std::array{ObjectRef::state(q2)});
        addOutputWrite(model, "o", byte, addStateRead(model, q, "q_r"));
        addOutputWrite(model, "o2", byte, addStateRead(model, q2, "q2_r"));
        require(verifies(model), "dpi fixture rejected");
        // Default merge keeps the event-free call out of the gated call's
        // supernode (same merge prohibition as the display test).
        runSixPhasePipeline(model, false, false, tinyTu);
        compileAndRun(model, root / (tinyTu ? "dpi_smoke_tu" : "dpi_smoke"), {
            {{{"a", "0"}, {"d", "0"}, {"clk", "false"}}, {{"o", "1"}, {"o2", "0"}}},
            {{{"a", "1"}}, {{"o", "2"}, {"o2", "0"}}},
            {{{"d", "3"}}, {{"o", "2"}, {"o2", "0"}}},    // no edge: gated call skipped
            {{{"clk", "true"}}, {{"o", "2"}, {"o2", "6"}}},
            {{{"clk", "false"}}, {{"o", "2"}, {"o2", "6"}}},
            {{{"d", "4"}}, {{"o", "2"}, {"o2", "6"}}},    // data change without edge: skipped
            {{{"clk", "true"}}, {{"o", "2"}, {"o2", "8"}}},
            {{{"a", "255"}}, {{"o", "0"}, {"o2", "8"}}},  // wraps
        }, false,
        "extern \"C\" std::uint8_t cpu_test_inc(std::uint8_t x){return static_cast<std::uint8_t>(x+1);}\n"
        "extern \"C\" std::uint8_t cpu_test_dbl(std::uint8_t x){return static_cast<std::uint8_t>(x*2);}\n", tinyTu);
    }

    void dpiBitAbiTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_dpi_bit_abi");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto one = addConstant(model, bit, "1'b1");
        const auto input = addInputRead(model, "input", bit);
        const std::array args{
            DpiArgument{model.intern("out"), DpiDirection::Output, bit},
            DpiArgument{model.intern("in"), DpiDirection::Input, bit}};
        const auto function = model.addExternFunction("cpu_test_bit_abi", "core.dpi", "cpu_test_bit_abi", args, bit);
        const auto returned = model.addValue(bit, "returned");
        const auto output = model.addValue(bit, "output");
        model.addOperation("core.dpi.call", std::array{one, input}, std::array{returned, output},
                           std::array{ObjectRef::function(function)});
        addOutputWrite(model, "returned", bit, returned);
        addOutputWrite(model, "output", bit, output);
        require(verifies(model), "1-bit DPI fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "dpi_bit_abi", {
            {{{"input", "false"}}, {{"returned", "false"}, {"output", "false"}}},
            {{{"input", "true"}}, {{"returned", "true"}, {"output", "false"}}},
        }, false, "extern \"C\" std::uint8_t cpu_test_bit_abi(std::uint8_t* out, std::uint8_t in) { *out = 2; return in ? 3 : 0; }\n");
        // M5d-7: DPI import declarations are per-TU (never in the public
        // header); the concatenated sources carry the referenced import.
        const std::string source = concatModelSources(root / "dpi_bit_abi" / "model");
        require(source.find("extern \"C\" std::uint8_t cpu_test_bit_abi(std::uint8_t*,std::uint8_t);") !=
                    std::string::npos, "1-bit DPI declaration does not use the svBit ABI");
    }

    // (15) Perf counters under WOLVRIX_GRHSIM_PERF=1: the 8-field aggregate uses
    // the fixed contract names with new-model semantics (see report table).
    void perfCountersTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_perf");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto s = addState(model, "s", bit, "1'b0");
        const auto sRead = addStateRead(model, s, "s_r");
        model.addOperation("core.state.latchWrite", std::array{one, a, one}, {}, std::array{ObjectRef::state(s)});
        addOutputWrite(model, "o", bit, sRead);
        require(verifies(model), "perf fixture rejected");
        runSixPhasePipeline(model, false);
        const auto top = std::string(model.text(model.name()));
        const auto directory = root / "perf";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        diag::Diagnostics diagnostics;
        require(emitSixPhaseCpuCpp(model, directory / "model", diagnostics).success, "perf emit failed");
        {
            std::ofstream driver(directory / "driver.cpp");
            driver << "#include \"grhsim_" << top << ".hpp\"\n#include <cstdio>\n"
                   << "int main(){\nGrhSIM_" << top << " sim;\nsim.init();\n"
                   << "for(int i=0;i<4;++i){sim.a=(i&1)!=0;sim.eval();}\n"
                   << "const auto c=sim.perf_counters();\n"
                   << "std::printf(\"eval=%llu rounds=%llu r1=%llu r2=%llu compute=%llu commit=%llu touched=%llu memwr=%llu\\n\",\n"
                   << "(unsigned long long)c.evalCount,(unsigned long long)c.totalRoundCount,(unsigned long long)c.round1Count,\n"
                   << "(unsigned long long)c.round2Count,(unsigned long long)c.computeBatchExecCount,(unsigned long long)c.commitBatchExecCount,\n"
                   << "(unsigned long long)c.touchedStateShadowCount,(unsigned long long)c.touchedWriteCount);\n"
                   << "if(c.evalCount!=4||c.totalRoundCount!=4||c.round1Count!=4||c.round2Count!=0||c.commitBatchExecCount!=4||"
                      "c.computeBatchExecCount!=4||c.touchedStateShadowCount!=3||c.touchedWriteCount!=0)return 1;\n"
                   << "std::printf(\"PASS\\n\");\nreturn 0;\n}\n";
        }
        command("make --no-print-directory -C " + quote((directory / "model").string()) + " -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -DWOLVRIX_GRHSIM_PERF=1 -fsanitize=undefined -fno-sanitize-recover=all'");
        command(quote(WOLVRIX_TEST_CXX) + " -std=c++20 -O2 -DWOLVRIX_GRHSIM_PERF=1 -fsanitize=undefined -fno-sanitize-recover=all -I" +
                quote((directory / "model").string()) + " " + quote((directory / "driver.cpp").string()) + " " +
                quote((directory / "model" / ("libgrhsim_" + top + ".a")).string()) + " -o " +
                quote((directory / "driver").string()));
        command("cd " + quote(directory.string()) + " && ./driver");
    }

    // (16) D3 regression: a whole-array state.read of a runtime-written mem is
    // a dynamic reader in the mem write plan — after each write its supernode
    // re-publishes the boundary copy and the event-free memAssign downstream
    // picks up the new contents within the same eval.
    void memAssignReadbackTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_assign_readback");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(word, 16);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto cen = addInputRead(model, "cen", bit);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto mask = addConstant(model, word, "8'hff");
        const auto memF = addMemState(model, "memF", memType, "8'h00");
        const auto memC = addMemState(model, "memC", memType, "8'h00");
        addEventWrite(model, "core.state.memWrite", {wen, waddr, wdata, mask}, memF, {});
        const auto fRead = model.addValue(memType, "f_read");
        model.addOperation("core.state.read", {}, std::array{fRead}, std::array{ObjectRef::state(memF)});
        addEventWrite(model, "core.state.memAssign", {cen, fRead}, memC, {});
        addOutputWrite(model, "oc", word, addMemRead(model, memC, word, raddr, "rc"));
        require(verifies(model), "memAssign readback fixture rejected");
        runSixPhasePipeline(model, false, true);
        const auto &mapping = *model.cpuMapping();
        const auto &plan = *mapping.schedule->memWritePlan;
        require(plan.size() == 2, "memAssign readback should have two write plan entries");
        // The write into memF must list the whole-array read's supernode as a
        // dynamic reader (plan order is op-id order: the memWrite precedes the
        // memAssign, whose target memC has no General readers at all).
        require(plan[0].readers.size() == 1 && !plan[0].readers[0].staticRow,
                "memF write should carry exactly one dynamic whole-array reader");
        require(plan[1].readers.empty(), "memC assign should have no readers");
        compileAndRun(model, root / "mem_assign_readback", {
            {{{"wen", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"cen", "false"}, {"raddr", "0"}}, {{"oc", "0"}}},
            {{{"cen", "true"}}, {{"oc", "0"}}},                       // copy of all-zero memF: no change
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "90"}, {"raddr", "3"}}, {{"oc", "90"}}},
            // ^ write memF[3], reader supernode re-publishes f_read, memAssign
            // copies it — all within this eval's round loop.
            {{{"wen", "false"}}, {{"oc", "90"}}},                     // stable
            {{{"wen", "true"}, {"wdata", "119"}}, {{"oc", "119"}}},   // overwrite propagates again
            {{{"wen", "false"}}, {{"oc", "119"}}},
        });
    }

    // (17) Packed fill: a whole packed-array write (ingest lowers packed
    // aggregate whole-writes to memFill with packed-width data) must slice the
    // data per row — the legacy isPackedFill semantics — on both the scalar
    // (<=64-bit) and wide packed-data paths.
    void packedFillTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_packed_fill");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto one = addConstant(model, bit, "1'b1");
        // Scalar packed: two 2-bit rows, fill data 4'h9 -> row0=1, row1=2.
        const auto pair = model.logicType(2, false, LogicDomain::TwoState);
        const auto packed4 = model.logicType(4, false, LogicDomain::TwoState);
        const auto memS = addMemState(model, "memS", model.arrayType(pair, 2), "2'h0");
        addEventWrite(model, "core.state.memFill", {one, addConstant(model, packed4, "4'h9")},
                      memS, {{clk, "posedge"}});
        // Wide packed: sixteen 8-bit rows, fill data 128'h0f0e...0100 -> row i = i.
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto packed128 = model.logicType(128, false, LogicDomain::TwoState);
        const auto memW = addMemState(model, "memW", model.arrayType(byte, 16), "8'h00");
        addEventWrite(model, "core.state.memFill",
                      {one, addConstant(model, packed128, "128'h0f0e0d0c0b0a09080706050403020100")},
                      memW, {{clk, "posedge"}});
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto row0 = addConstant(model, addrType, "4'h0");
        const auto row1 = addConstant(model, addrType, "4'h1");
        const auto row15 = addConstant(model, addrType, "4'hf");
        addOutputWrite(model, "s0", pair, addMemRead(model, memS, pair, row0, "s0_r"));
        addOutputWrite(model, "s1", pair, addMemRead(model, memS, pair, row1, "s1_r"));
        addOutputWrite(model, "w0", byte, addMemRead(model, memW, byte, row0, "w0_r"));
        addOutputWrite(model, "w15", byte, addMemRead(model, memW, byte, row15, "w15_r"));
        require(verifies(model), "packed fill fixture rejected");
        runSixPhasePipeline(model, false, true);
        compileAndRun(model, root / "packed_fill", {
            {{{"clk", "false"}}, {{"s0", "0"}, {"s1", "0"}, {"w0", "0"}, {"w15", "0"}}},
            {{{"clk", "true"}}, {{"s0", "1"}, {"s1", "2"}, {"w0", "0"}, {"w15", "15"}}},
            {{{"clk", "false"}}, {{"s0", "1"}, {"s1", "2"}, {"w0", "0"}, {"w15", "15"}}},
        });
    }

    // A fused expr-chain op still reaches the placeholder throw (M5a后续切片):
    // the new pipeline never produces core.compute.expr, and emit must fail
    // cleanly before any artifact appears.
    void exprPlaceholderTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_expr_stub");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        const auto x = model.addValue(bit, "x");
        const auto selfId = model.operations().size() + 1; // the id this op will receive
        const std::array params{
            Parameter{model.intern("tree"), std::vector<std::string>{"l0", "n;not;1;0;1;" + std::to_string(selfId)}},
            Parameter{model.intern("rk"), std::string("core.compute.not")}};
        model.addOperation("core.compute.expr", std::array{a}, std::array{x}, {}, params);
        addOutputWrite(model, "o", bit, x);
        require(verifies(model), "expr fixture rejected");
        runSixPhasePipeline(model, false);
        diag::Diagnostics diagnostics;
        require(!emitSixPhaseCpuCpp(model, root / "expr_stub", diagnostics).success,
                "six-phase emit accepted a fused expr model");
        bool sawPlaceholder = false;
        for (const auto &message : diagnostics.messages())
            if (message.message.find("M5a") != std::string::npos) sawPlaceholder = true;
        require(sawPlaceholder, "placeholder throw missing from the diagnostics");
        require(!std::filesystem::exists(root / "expr_stub" / "Makefile"),
                "failed emit must not leave artifacts");
    }

    // (M5d-6) General-phase regLatch-class array writes: with the default
    // classification these small arrays land in the regLatch store and their
    // writes become General-phase NBA ops inside General supernodes (the P_mem
    // plan stays empty). Covers memWriteSeq port priority (last triple wins),
    // masked memWrite (merge into the next buffer), event-free memFill
    // convergence, and the NBA visibility contract at eval granularity (the
    // same trace shape as the mem-class equivalents).
    void regLatchArrayWriteTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_reglatch_write");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(word, 16); // 16 B: regLatch class
        const auto clk = addInputRead(model, "clk", bit);
        const auto enc = addInputRead(model, "enc", bit);
        const auto bdata = addInputRead(model, "bdata", word);
        const auto caddr = addInputRead(model, "caddr", addrType);
        const auto cdata = addInputRead(model, "cdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto fen = addInputRead(model, "fen", bit);
        const auto fdata = addInputRead(model, "fdata", word);
        const auto one = addConstant(model, bit, "1'b1");
        const auto three = addConstant(model, addrType, "4'h3");
        const auto aa = addConstant(model, word, "8'haa");
        const auto nibble = addConstant(model, word, "8'h0f");
        const auto tabS = addMemState(model, "tabS", memType, "8'h00");
        const auto tabM = addMemState(model, "tabM", memType, "8'h00");
        const auto tabF = addMemState(model, "tabF", memType, "8'h00");
        // memWriteSeq priority chain (event-gated), a masked memWrite and an
        // event-free fill — all on regLatch-class arrays.
        addEventWrite(model, "core.state.memWriteSeq", {one, three, aa, one, three, bdata, enc, caddr, cdata},
                      tabS, {{clk, "posedge"}});
        addEventWrite(model, "core.state.memWrite", {wen, waddr, wdata, nibble}, tabM, {});
        addEventWrite(model, "core.state.memFill", {fen, fdata}, tabF, {});
        addOutputWrite(model, "o", word, addMemRead(model, tabS, word, raddr, "rs"));
        addOutputWrite(model, "om", word, addMemRead(model, tabM, word, raddr, "rm"));
        addOutputWrite(model, "of", word, addMemRead(model, tabF, word, raddr, "rf"));
        require(verifies(model), "regLatch write fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu); // default classification: regLatch
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->memWritePlan && mapping.schedule->memWritePlan->empty(),
                "regLatch-class writes must not enter the P_mem write plan");
        compileAndRun(model, root / (tinyTu ? "reglatch_write_tu" : "reglatch_write"), {
            {{{"clk", "false"}, {"enc", "false"}, {"bdata", "0"}, {"caddr", "0"}, {"cdata", "0"},
              {"raddr", "3"}, {"wen", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"fen", "false"},
              {"fdata", "0"}},
             {{"o", "0"}, {"om", "0"}, {"of", "0"}}},
            {{{"bdata", "187"}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "187"}}},   // A writes 0xaa, B overwrites 0xbb, C disabled
            {{{"clk", "false"}}, {{"o", "187"}}},
            {{{"enc", "true"}, {"caddr", "3"}, {"cdata", "221"}, {"clk", "false"}}, {{"o", "187"}}},
            {{{"clk", "true"}}, {{"o", "221"}}},   // C (last port) wins row 3
            // Masked write: low nibble only, merges with the visible row.
            {{{"wen", "true"}, {"waddr", "5"}, {"wdata", "90"}, {"raddr", "5"}}, {{"om", "10"}}},
            {{{"wdata", "48"}}, {{"om", "0"}}},    // RMW on the accumulated row: 0x0a&~0x0f | 0x30&0x0f
            {{{"wen", "false"}}, {{"om", "0"}}},
            // Event-free fill converges; reads see it after the same eval.
            {{{"fen", "true"}, {"fdata", "153"}}, {{"of", "153"}}},
            {{{"fen", "false"}}, {{"of", "153"}}},
        }, false, {}, tinyTu);
    }

    // M5d-7 multi-TU emit: a 128-bit combinational add chain (one big
    // supernode, split into helper chunks by the tiny C6 cap; the chain
    // values cross chunk boundaries through the spill frame), a 12-deep NBA
    // register chain and an array state (init/dump chunks), plus an event
    // cone. Tiny C8 caps force several translation units; the run checks the
    // chunk ABI end to end (frame struct, chunk members, generated Makefile
    // source list) and the simulated values.
    void multiTuTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_multi_tu");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto word = model.logicType(128, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto d = addInputRead(model, "d", byte);
        // 128-bit add chain: x_{i+1} = x_i + i. x23 = d + sum(0..23) = d+276.
        const auto wideD = addCompute(model, "core.compute.concat", word, "wide_d", {
            addConstant(model, model.logicType(120, false, LogicDomain::TwoState), "120'h0"), d});
        ValueId prev = wideD;
        for (uint32_t i = 0; i < 24; ++i)
            prev = addCompute(model, "core.compute.add", word, "x" + std::to_string(i),
                              {prev, addConstant(model, word, std::to_string(i))});
        // NBA register capturing the chain tail at posedge clk.
        const auto q = addState(model, "q", word, "128'h0");
        const auto one = addConstant(model, bit, "1'b1");
        const auto ones128 = addConstant(model, word, "128'hffffffffffffffffffffffffffffffff");
        addEventRegWrite(model, one, prev, ones128, q, {{clk, "posedge"}});
        // 12-deep byte register chain: s_{i+1} <= s_i + 1 at the same edge.
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask8 = addConstant(model, byte, "8'hff");
        std::vector<StateId> chain;
        for (uint32_t i = 0; i < 12; ++i) chain.push_back(addState(model, "s" + std::to_string(i), byte, "8'h00"));
        for (uint32_t i = 0; i + 1 < chain.size(); ++i)
        {
            const auto read = addStateRead(model, chain[i], "sr" + std::to_string(i));
            const auto next = addCompute(model, "core.compute.add", byte, "sn" + std::to_string(i), {read, one8});
            addEventRegWrite(model, one, next, mask8, chain[i + 1], {{clk, "posedge"}});
        }
        // Small array state (const init) to populate the init/dump streams.
        const auto tab = addMemState(model, "tab", model.arrayType(byte, 4), "8'h00");
        (void)tab;
        addOutputWrite(model, "o", byte, [&] {
            // Only the low byte is observable (the driver compares scalars).
            const auto sliced = model.addValue(byte, "o_byte");
            const std::array params{Parameter{model.intern("sliceStart"), int64_t{0}},
                                    Parameter{model.intern("sliceEnd"), int64_t{7}}};
            model.addOperation("core.compute.sliceStatic", std::array{addStateRead(model, q, "q_r")},
                               std::array{sliced}, {}, params);
            return sliced;
        }());
        addOutputWrite(model, "os3", byte, addStateRead(model, chain[3], "s3_r"));
        addOutputWrite(model, "os11", byte, addStateRead(model, chain[11], "s11_r"));
        require(verifies(model), "multi-TU fixture rejected");
        runSixPhasePipeline(model, false, false, true);
        const auto &mapping = *model.cpuMapping();
        require(mapping.translationUnits.has_value(), "TU plan missing after plan-translation-units");
        const auto &plan = *mapping.translationUnits;
        require(plan.units.size() >= 3, "tiny caps should split the emit into several units");
        require(plan.chunkMaxEstimatedLines == 24 && plan.unitMaxEstimatedLines == 64,
                "TU plan should record the caps");
        require(verifies(model), "mapped multi-TU model rejected");
        compileAndRun(model, root / "multi_tu", {
            {{{"clk", "false"}, {"d", "0"}}, {{"o", "0"}, {"os3", "0"}, {"os11", "0"}}},
            // s_i = min(posedge count, i): the chain samples pre-publish values.
            // o is the low byte of q (276 = 0x114, 277 = 0x115).
            {{{"clk", "true"}}, {{"o", "20"}, {"os3", "1"}, {"os11", "1"}}},    // q <= 0+276
            {{{"clk", "false"}}, {{"o", "20"}, {"os3", "1"}, {"os11", "1"}}},
            {{{"d", "1"}}, {{"o", "20"}, {"os3", "1"}, {"os11", "1"}}},         // no edge
            {{{"clk", "true"}}, {{"o", "21"}, {"os3", "2"}, {"os11", "2"}}},    // q <= 1+276
            {{{"clk", "false"}}, {{"o", "21"}, {"os3", "2"}, {"os11", "2"}}},
            {{{"clk", "true"}}, {{"o", "21"}, {"os3", "3"}, {"os11", "3"}}},
            {{{"clk", "false"}}, {{"o", "21"}, {"os3", "3"}, {"os11", "3"}}},
            {{{"clk", "true"}}, {{"o", "21"}, {"os3", "3"}, {"os11", "4"}}},    // s3 saturated
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            {{{"clk", "false"}}, {}}, {{{"clk", "true"}}, {}},
            // Twelve posedges in total: s3 = 3, s11 = 11.
            {{{"clk", "false"}}, {{"o", "21"}, {"os3", "3"}, {"os11", "11"}}},
        }, false, {}, true);
        // The spill frame and the chunk members must appear in the header; the
        // emitted sources are all listed in the generated Makefile (checked by
        // compileAndRun's expectMultiTu).
        std::ifstream header(root / "multi_tu" / "model" / "grhsim_phase_multi_tu.hpp");
        const std::string text{std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>()};
        require(text.find("SnFrame") != std::string::npos, "chunked supernode should emit a spill frame");
        require(text.find("__c0(") != std::string::npos, "chunked supernode should emit chunk members");
        require(text.find("cpu_init_") != std::string::npos, "init chunks missing from the header");
        require(text.find("cpu_dump_") != std::string::npos, "dump chunks missing from the header");
    }
}

int main()
{
    try
    {
        const auto root = artifactRoot() / "cpu_phase_emit";
        std::filesystem::create_directories(root);
        passthroughTest(root);
        wideAddTest(root);
        scalarSliceArrayTest(root);
        randomSystemFunctionTest(root);
        randomSystemFunctionTest(root, true);
        randomSampleIdAfterCompactTest();
        fanoutTest(root);
        passRegistrationTest(root);
        // M5d-7: the deferred M5d-3..M5d-6 runtime diffs (state/NBA, events,
        // DPI, timeslot) also run under tiny C8 caps so they exercise the
        // multi-TU emit (chunk functions + spill frames + parallel build).
        counterTest(root);
        counterTest(root, true);
        nbaChainTest(root);
        powerOnTest(root);
        asyncResetTest(root);
        dualClockTest(root);
        glitchClockTest(root);
        glitchClockTest(root, true);
        latchRingTest(root);
        memConvergeTest(root);
        memReaderGatingTest(root);
        memPriorityTest(root);
        memPriorityTest(root, true);
        generalDisplayTest(root);
        monitorFreeTest(root);
        monitorEventTest(root);
        monitorEventTest(root, true);
        dpiSmokeTest(root);
        dpiSmokeTest(root, true);
        dpiBitAbiTest(root);
        perfCountersTest(root);
        memAssignReadbackTest(root);
        packedFillTest(root);
        regLatchArrayWriteTest(root);
        regLatchArrayWriteTest(root, true);
        multiTuTest(root);
        exprPlaceholderTest(root);
        std::cout << "CPU six-phase emit tests passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
