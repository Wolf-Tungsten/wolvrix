#include "grhsim/backend/cpu_phase_emit.hpp"
#include "grhsim/backend/cpu_phase_common.hpp"
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
#include <map>
#include <set>
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

    const CpuStoreField *findStoreField(const GrhSimModel &model, CpuNamedStoreKind kind,
                                       std::string_view name);

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
    // state into the large-sink class; by default small states use the
    // General-phase NBA path. memMinBytes overrides the classification
    // threshold directly (e.g. to keep a >512-byte array in the regLatch
    // store). tinyTu shrinks the C6/C8 size caps so a small model still
    // exercises chunked functions and multi-TU emit. sinkGuardMin pins C2's
    // A1 --sink-enable-guard-min-size (the pass default is 8, so existing
    // callers see no behavior change).
    void runSixPhasePipeline(GrhSimModel &model, bool splitSupernodes, bool allMem = false, bool tinyTu = false,
                             uint32_t sinkGuardMin = 8, uint64_t memMinBytes = 64)
    {
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        const auto memMinText = std::to_string(memMinBytes);
        if (allMem)
            runPass(model, "grhsim.select-state-stores",
                    std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        else if (memMinBytes != 64)
            runPass(model, "grhsim.select-state-stores",
                    std::array<std::string_view, 2>{"--mem-min-bytes", memMinText});
        else
            runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
        const auto guardMinText = std::to_string(sinkGuardMin);
        if (splitSupernodes)
        {
            const std::array<std::string_view, 2> nodeCap{"--max-op-in-compute-node", "1"};
            const std::array<std::string_view, 4> supernodeCap{"--max-op-in-compute-supernode", "1",
                                                               "--sink-enable-guard-min-size", guardMinText};
            runPass(model, "cpu.st.build-general-nodes", nodeCap);
            runPass(model, "cpu.st.merge-general-supernodes", supernodeCap);
        }
        else
        {
            const std::array<std::string_view, 2> guardMin{"--sink-enable-guard-min-size", guardMinText};
            runPass(model, "cpu.st.build-general-nodes");
            runPass(model, "cpu.st.merge-general-supernodes", guardMin);
        }
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-activation-map");
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
    OpId addEventWrite(GrhSimModel &model, std::string_view opType, std::vector<ValueId> operands,
                       StateId target, std::initializer_list<std::pair<ValueId, const char *>> events,
                       std::string_view opName = {})
    {
        std::vector<ObjectRef> refs{ObjectRef::state(target)};
        std::vector<std::string> edges;
        for (const auto &[event, edge] : events)
        {
            operands.push_back(event);
            edges.push_back(edge);
        }
        if (edges.empty()) return model.addOperation(opType, operands, {}, refs, {}, opName);
        const std::array params{Parameter{model.intern("event_edges"), edges}};
        return model.addOperation(opType, operands, {}, refs, params, opName);
    }

    OpId addEventStateWrite(GrhSimModel &model, ValueId cond, ValueId next, ValueId mask,
                            StateId target,
                            std::initializer_list<std::pair<ValueId, const char *>> events)
    { return addEventWrite(model, "core.state.write", {cond, next, mask}, target, events); }

    unsigned activationBits(const CpuEventActivation &entry)
    {
        unsigned count = 0;
        for (const auto word : entry.supernodeWords) count += static_cast<unsigned>(__builtin_popcountll(word));
        return count;
    }

    struct DriveStep
    {
        std::vector<std::pair<std::string, std::string>> assigns;
        std::vector<std::pair<std::string, std::string>> expects;
    };

    std::string driverSource(const GrhSimModel &model, std::string_view top,
                             const std::vector<DriveStep> &steps, bool captureStdout,
                             std::string_view prelude)
    {
        std::string src = "#include \"grhsim_" + std::string(top) + ".hpp\"\n"
            "#include <array>\n#include <cstdint>\n#include <cstdio>\n#include <type_traits>\n"
            "template<class T> struct CpuTestArray : std::false_type {};\n"
            "template<class T,std::size_t N> struct CpuTestArray<std::array<T,N>> : std::true_type {};\n"
            "template<class Packed,class T> void cpu_test_unpack(T &dst,Packed bits,unsigned elementBits,unsigned totalBits,std::size_t &row){\n"
            "if constexpr(CpuTestArray<T>::value){for(auto &child:dst)cpu_test_unpack(child,bits,elementBits,totalBits,row);}\n"
            "else{const Packed mask=elementBits==totalBits?~Packed(0):(Packed(1)<<elementBits)-1;\n"
            "dst=static_cast<T>((bits>>(row++*elementBits))&mask);}}\n"
            "template<class Packed,class T> void cpu_test_pack(Packed &bits,const T &src,unsigned elementBits,unsigned totalBits,std::size_t &row){\n"
            "if constexpr(CpuTestArray<T>::value){for(const auto &child:src)cpu_test_pack(bits,child,elementBits,totalBits,row);}\n"
            "else{const Packed mask=elementBits==totalBits?~Packed(0):(Packed(1)<<elementBits)-1;\n"
            "bits|=(static_cast<Packed>(src)&mask)<<(row++*elementBits);}}\n";
        src += prelude;
        src += "int main(){\nGrhSIM_" + std::string(top) + " sim;\n";
        if (captureStdout) src += "std::freopen(\"task_output.txt\",\"w\",stdout);\n";
        src += "sim.init();\nint fails=0;\n";
        const auto portType = [&](std::string_view name, bool input) -> const Type * {
            if (input)
            {
                for (const auto &port : model.inputs())
                    if (model.text(port.name) == name)
                        return &model.types()[port.type.index - 1];
            }
            else
            {
                for (const auto &port : model.outputs())
                    if (model.text(port.name) == name)
                        return &model.types()[port.type.index - 1];
            }
            return nullptr;
        };
        const auto packedType = [](const Type &type) {
            return type.width <= 64 ? std::string("std::uint64_t")
                                    : "unsigned _BitInt(" + std::to_string(type.width) + ")";
        };
        std::size_t index = 0;
        for (const auto &step : steps)
        {
            for (const auto &[port, expr] : step.assigns)
            {
                const Type *type = portType(port, true);
                if (!type || type->shape.empty())
                {
                    src += "sim." + port + "=" + expr + ";\n";
                    continue;
                }
                const auto elementWidth = typeElementWidth(*type);
                require(bool(elementWidth), "shaped input has invalid element width");
                src += "{using CpuPacked=" + packedType(*type) + ";const CpuPacked cpu_bits=static_cast<CpuPacked>(" +
                       expr + ");std::size_t cpu_row=0;cpu_test_unpack(sim." + port + ",cpu_bits," +
                       std::to_string(*elementWidth) + "u," + std::to_string(type->width) + "u,cpu_row);}\n";
            }
            src += "sim.eval();\n";
            for (const auto &[port, expr] : step.expects)
            {
                const Type *type = portType(port, false);
                if (type && !type->shape.empty())
                {
                    const auto elementWidth = typeElementWidth(*type);
                    require(bool(elementWidth), "shaped output has invalid element width");
                    src += "{using CpuPacked=" + packedType(*type) + ";CpuPacked cpu_bits=0;std::size_t cpu_row=0;"
                           "cpu_test_pack(cpu_bits,sim." + port + "," + std::to_string(*elementWidth) + "u," +
                           std::to_string(type->width) + "u,cpu_row);";
                }
                src += "if(" + std::string(type && !type->shape.empty() ? "cpu_bits" : "sim." + port) +
                       "!=(" + expr + ")){std::fprintf(stderr,\"step " + std::to_string(index) +
                       ": " + port + " mismatch\\n\");++fails;}\n";
                if (type && !type->shape.empty()) src += "}\n";
            }
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
                              std::string_view prelude = {}, bool expectMultiTu = false, bool memWriteOnWrite = false)
    {
        const auto top = std::string(model.text(model.name()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        diag::Diagnostics diagnostics;
        const auto result = emitSixPhaseCpuCpp(model, directory / "model", diagnostics, false, true, memWriteOnWrite);
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
            driver << driverSource(model, top, steps, captureStdout, prelude);
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
        const auto &stores = *model.cpuMapping()->dataLayout->namedStores;
        const bool hasBoundaryFields = std::any_of(stores.begin(), stores.end(), [](const CpuNamedStore &store) {
            return (store.kind == CpuNamedStoreKind::Input ||
                    (store.kind >= CpuNamedStoreKind::NonSinkBoundary &&
                     store.kind <= CpuNamedStoreKind::LargeSinkPayloadBoundary)) && !store.fields.empty();
        });
        if (hasBoundaryFields)
            require(dumpText.find("inputStore.") != std::string::npos ||
                    dumpText.find("nonSinkBoundaryStore.") != std::string::npos,
                    "dumpState misses the boundary store section");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        addOutputWrite(model, "o", bit, a);
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        const auto &active = mapping.dataLayout->namedStores->back();
        require(active.fields.size() == 2 && active.fields[0].aux == 0, "passthrough should have no supernodes");
        require(mapping.schedule->inputFanout.empty(), "passthrough should have no input fanout rows");
        compileAndRun(model, root / "passthrough", {
            {{{"a", "false"}}, {{"o", "false"}}},
            {{{"a", "true"}}, {{"o", "true"}}},
            {{{"a", "true"}}, {{"o", "true"}}},
            {{{"a", "false"}}, {{"o", "false"}}},
        });
    }

    // (b) 128-bit adder: wide values are _BitInt objects end to end (native
    // add in the P_output clone), ports drive/compare as _BitInt literals.
    void wideAddTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_add128");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto wide = model.logicType(128, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", wide);
        const auto b = addInputRead(model, "b", wide);
        const auto sum = addCompute(model, "core.compute.add", wide, "sum", {a, b});
        addOutputWrite(model, "o", wide, sum);
        runSixPhasePipeline(model, false);
        const auto words = [](unsigned __int128 value) {
            const auto lo = static_cast<std::uint64_t>(value);
            const auto hi = static_cast<std::uint64_t>(value >> 64);
            return "(((unsigned _BitInt(128))UINT64_C(" + std::to_string(hi) + ")<<64)|UINT64_C(" +
                   std::to_string(lo) + "))";
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

    void largeScalarStoreTest(const std::filesystem::path &root)
    {
        const std::string high = "((unsigned _BitInt(512))1<<400)";
        const std::string allMask = "~((unsigned _BitInt(512))0)";
        {
            GrhSimModel model("phase_large_scalar_write");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto wide = model.logicType(512, false, LogicDomain::TwoState);
            const auto clk = addInputRead(model, "clk", bit);
            const auto en = addInputRead(model, "en", bit);
            const auto data = addInputRead(model, "data", wide);
            const auto mask = addInputRead(model, "mask", wide);
            const auto state = addState(model, "wide", wide, "512'h0");
            addEventWrite(model, "core.state.write", {en, data, mask}, state,
                          {{clk, "posedge"}});
            addOutputWrite(model, "o", wide, addStateRead(model, state, "wide_read"));
            require(verifies(model), "large scalar write fixture rejected");
            runSixPhasePipeline(model, false);
            require(bool(findStoreField(model, CpuNamedStoreKind::LargeSink, "wide")),
                    "512-bit scalar did not enter largeSinkStore");
            const auto &plan = *model.cpuMapping()->schedule->memWritePlan;
            require(plan.size() == 1 && model.text(model.operations()[plan[0].writeOp.index - 1].opType) ==
                        "core.state.write", "large scalar write missing from P_large_sink plan");
            compileAndRun(model, root / "large_scalar_write", {
                {{{"clk", "false"}, {"en", "false"}, {"data", "0"}, {"mask", allMask}}, {{"o", "0"}}},
                {{{"en", "true"}, {"data", high + "|0x5a"}, {"clk", "true"}},
                 {{"o", high + "|0x5a"}}},
                {{{"clk", "false"}, {"data", "0xa5"}, {"mask", "0xff"}},
                 {{"o", high + "|0x5a"}}},
                {{{"clk", "true"}}, {{"o", high + "|0xa5"}}},
            });
        }
        {
            GrhSimModel model("phase_large_scalar_latch");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto wide = model.logicType(512, false, LogicDomain::TwoState);
            const auto en = addInputRead(model, "en", bit);
            const auto data = addInputRead(model, "data", wide);
            const auto mask = addInputRead(model, "mask", wide);
            const auto state = addState(model, "wide", wide, "512'h0");
            model.addOperation("core.state.latchWrite", std::array{en, data, mask}, {},
                               std::array{ObjectRef::state(state)});
            addOutputWrite(model, "o", wide, addStateRead(model, state, "wide_read"));
            require(verifies(model), "large scalar latch fixture rejected");
            runSixPhasePipeline(model, false);
            require(bool(findStoreField(model, CpuNamedStoreKind::LargeSink, "wide")),
                    "512-bit latch did not enter largeSinkStore");
            const auto &plan = *model.cpuMapping()->schedule->memWritePlan;
            require(plan.size() == 1 && model.text(model.operations()[plan[0].writeOp.index - 1].opType) ==
                        "core.state.latchWrite", "large scalar latch missing from P_large_sink plan");
            compileAndRun(model, root / "large_scalar_latch", {
                {{{"en", "false"}, {"data", "0"}, {"mask", allMask}}, {{"o", "0"}}},
                {{{"data", high + "|0x12"}, {"en", "true"}}, {{"o", high + "|0x12"}}},
                {{{"en", "false"}, {"data", "0x34"}}, {{"o", high + "|0x12"}}},
            });
        }
    }

    void scalarSliceArrayTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_scalar_slice_array");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        // Stage-06: sliceArray requires a shaped base — 4 rows x 8 bits packed
        // into the flat 32-bit port vector; the result type carries the width.
        const std::array<int64_t, 1> wordShape{4};
        const auto word = model.logicType(32, false, LogicDomain::TwoState, wordShape);
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", bit);
        const auto b = addInputRead(model, "b", bit);
        const auto x = addCompute(model, "core.compute.xor", bit, "x", {a, b});
        const auto one = addConstant(model, bit, "1'b1");
        const auto s1 = addState(model, "s1", bit, "1'b0");
        const auto s2 = addState(model, "s2", bit, "1'b0");
        const auto w1 = model.addOperation("core.state.write", std::array{one, x, one}, {},
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
        const auto hasValue = [&](CpuNamedStoreKind kind, ValueId value) {
            for (const auto &store : stores)
                if (store.kind == kind)
                    for (const auto &field : store.fields)
                        if (field.value == value) return true;
            return false;
        };
        require(hasValue(CpuNamedStoreKind::SmallSinkPayloadBoundary, x),
                "shared intermediate lost its sink payload field");
        require(stores[2].fields.size() == 2, "input store fields missing");
        const auto &active = stores.back();
        require(active.fields.size() == 2 && active.fields[0].aux >= 3, "fanout model should have several supernodes");
        require(!mapping.schedule->computeSupernodeFanout.empty(), "fanout model should have supernode fanout rows");
        require(!mapping.schedule->eventActivation || mapping.schedule->eventActivation->empty(),
                "pure combinational model should have no event activation entries");
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
        require(text.find("smallSinkStoreNext") != std::string::npos && text.find("// state=s1") != std::string::npos,
                "regLatch store fields should be named and annotated");
        require(text.find("// value=x") != std::string::npos, "boundary store fields should carry the value comment");
        // Emitted shape: the non-sink scan wraps per-byte skip blocks with
        // per-bit live-byte checks and clear-on-fire; fanout activation is
        // batched per-byte mask ORs; publish merges byte-wise; init fills then
        // masks exactly the non-sink bit prefix. The compileAndRun steps
        // above are round-structure sensitive (o3/o4 settle in the same eval)
        // and catch any lost same-word same-round activation.
        const std::string cpp = concatModelSources(root / "fanout" / "model");
        require(cpp.find("nonSinkActiveFlag[0]&std::uint8_t(0x") != std::string::npos,
                "scan should test byte 0 bits with constant masks");
        require(cpp.find("]&=~std::uint8_t(0x") != std::string::npos,
                "scan should clear the fired bit before the supernode call");
        require(cpp.find("]|=std::uint8_t(0x") != std::string::npos,
                "fanout activation should be batched per-byte mask ORs");
        require(cpp.find("nonSinkActiveFlag[cpu_i]|=nonSinkActiveFlagNext[cpu_i]") != std::string::npos,
                "publish should merge Next byte-wise");
        require(cpp.find("nonSinkActiveFlag.fill(std::uint8_t(0xff));") != std::string::npos &&
                cpp.find("nonSinkActiveFlag[0]=std::uint8_t(0x") != std::string::npos,
                "init should fill bytes then mask the non-sink bit prefix");
    }

    // The registered pass rejects removed legacy options and models that did
    // not reach the TranslationUnits stage.
    void passRegistrationTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_pass_registration");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
            const std::array<std::string_view, 4> badWave{"--output", "x", "--waveform", "bogus"};
            require(!defaultPassRegistry().create("cpu.st.emit-cpp", badWave, error),
                    "emit-phase-cpp accepted an unknown waveform mode");
        }
        {
            // With the terminal stage, supported options emit the model
            // (declared-symbols waveform is a real emit mode since M5d-8).
            const std::string out = outDir.string();
            const std::array<std::string_view, 6> args{"--output", out, "--waveform", "declared-symbols",
                                                       "--perf", "eval"};
            auto pass = defaultPassRegistry().create("cpu.st.emit-cpp", args, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            const auto result = pass->run(model, diagnostics);
            require(result.success, "emit-phase-cpp failed on a TranslationUnits mapping");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto cnt = addState(model, "cnt", byte, "8'h00");
        const auto one = addConstant(model, bit, "1'b1");
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask = addConstant(model, byte, "8'hff");
        const auto cntRead = addStateRead(model, cnt, "cnt_r");
        const auto sum = addCompute(model, "core.compute.add", byte, "sum", {cntRead, one8});
        addEventStateWrite(model, one, sum, mask, cnt, {{clk, "posedge"}});
        const auto t = addState(model, "t", bit, "1'b0");
        const auto tRead = addStateRead(model, t, "t_r");
        const auto notT = addCompute(model, "core.compute.not", bit, "not_t", {tRead});
        addEventStateWrite(model, one, notT, one, t, {{clk, "both"}});
        addOutputWrite(model, "o", byte, cntRead);
        addOutputWrite(model, "ot", bit, tRead);
        require(verifies(model), "counter fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        const auto &mapping = *model.cpuMapping();
        const auto &stores = *mapping.dataLayout->namedStores;
        require(stores[8].fields.size() == 2, "counter should have two prevEvent slots");
        // V2 (M2): both writes are SinkEvent sinks (self-gated on their
        // signatures), so no non-sink carrier exists and the P_event
        // activation map is empty.
        require(mapping.schedule->eventActivation->empty(),
                "counter: sink-only event carriers must produce an empty activation map");
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

    // FST waveform (declared-symbols mode): emit with waveform on, build the
    // model (the generated Makefile compiles the vendored libfst C sources),
    // run a driver that captures several evals, then check the capture file.
    void waveformTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_wave");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto wide = model.logicType(128, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        addInputRead(model, "a", wide);
        const auto cnt = addState(model, "cnt", byte, "8'h00");
        const auto one = addConstant(model, bit, "1'b1");
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto cntRead = addStateRead(model, cnt, "cnt_r");
        const auto sum = addCompute(model, "core.compute.add", byte, "sum", {cntRead, one8});
        addEventStateWrite(model, one, sum, mask8, cnt, {{clk, "posedge"}});
        addOutputWrite(model, "o", byte, cntRead);
        for (const char *symbol : {"clk", "a", "cnt", "sum", "o", "ghost"})
            model.addDeclaredSymbol(model.intern(symbol));
        require(verifies(model), "waveform fixture rejected");
        // 1-op supernodes so every crossing value materializes a boundary field.
        runSixPhasePipeline(model, true);
        const auto directory = root / "waveform";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        {
            diag::Diagnostics diagnostics;
            const auto result = emitSixPhaseCpuCpp(model, directory / "model", diagnostics, true);
            for (const auto &message : diagnostics.messages())
                std::cout << message.context << ": " << message.message << '\n';
            require(result.success && !result.artifacts.empty(), "waveform emit failed");
        }
        const auto waveSource = concatModelSources(directory / "model");
        require(waveSource.find("wave_setup_0") != std::string::npos, "waveform emit produced no setup chunk");
        require(waveSource.find("\"cnt\"") != std::string::npos, "waveform setup misses the cnt register");
        require(waveSource.find("\"sum\"") != std::string::npos, "waveform setup misses the sum boundary value");
        require(waveSource.find("\"a\"") != std::string::npos, "waveform setup misses the wide input port");
        require(waveSource.find("ghost") == std::string::npos, "waveform setup dumps a symbol with no live object");
        {
            std::ofstream makefile(directory / "model" / "Makefile_check");
            std::ifstream generated(directory / "model" / "Makefile");
            const std::string text{std::istreambuf_iterator<char>(generated), std::istreambuf_iterator<char>()};
            require(text.find("fstapi.o") != std::string::npos && text.find("LIBFST_SRC_DIR") != std::string::npos,
                    "waveform Makefile misses the libfst rules");
        }
        {
            std::ofstream driver(directory / "driver.cpp");
            driver << "#include \"grhsim_phase_wave.hpp\"\n#include <cstdio>\n"
                      "int main(){\nGrhSIM_phase_wave sim;\nsim.init();\n"
                      "sim.configure_waveform(true,\"wave.fst\");\n"
                      "if(!sim.waveform_enabled())return 2;\n"
                      "for(int i=0;i<8;++i){sim.clk=(i&1)!=0;sim.eval();}\n"
                      "sim.configure_waveform(false);\n"
                      "if(sim.o!=4){std::fprintf(stderr,\"cnt mismatch\\n\");return 1;}\n"
                      "std::printf(\"PASS\\n\");\nreturn 0;\n}\n";
        }
        command("make --no-print-directory -C " + quote((directory / "model").string()) + " -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all'");
        command(quote(WOLVRIX_TEST_CXX) + " -std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all -I" +
                quote((directory / "model").string()) + " -I" + quote(WOLVRIX_TEST_LIBFST_INCLUDE_DIR) + " " +
                quote((directory / "driver.cpp").string()) + " " +
                quote((directory / "model" / "libgrhsim_phase_wave.a").string()) + " -lz -o " +
                quote((directory / "driver").string()));
        command("cd " + quote(directory.string()) + " && ./driver");
        const auto fst = directory / "wave.fst";
        require(std::filesystem::exists(fst) && std::filesystem::file_size(fst) > 512,
                "waveform capture produced no FST output");
    }

    // (2) A -> B NBA chain: at posedge clk, A <= in and B <= A in the same
    // round; B must sample the pre-publish A (the previous edge's value).
    void nbaChainTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_nba_chain");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto in = addInputRead(model, "in", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto a = addState(model, "a", bit, "1'b0");
        const auto b = addState(model, "b", bit, "1'b0");
        const auto aRead = addStateRead(model, a, "a_r");
        addEventStateWrite(model, one, in, one, a, {{clk, "posedge"}});
        addEventStateWrite(model, one, aRead, one, b, {{clk, "posedge"}});
        addOutputWrite(model, "oa", bit, aRead);
        addOutputWrite(model, "ob", bit, addStateRead(model, b, "b_r"));
        require(verifies(model), "nba chain fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventActivation->empty(),
                "nba chain: sink-only event carriers must produce an empty activation map");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        addEventStateWrite(model, one, one, one, q, {{clk, "posedge"}});
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto rst = addInputRead(model, "rst", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto zero = addConstant(model, bit, "1'b0");
        const auto one = addConstant(model, bit, "1'b1");
        const auto next = addCompute(model, "core.compute.mux", bit, "next", {rst, d, zero});
        const auto q = addState(model, "q", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        addEventStateWrite(model, one, next, one, q, {{clk, "posedge"}, {rst, "negedge"}});
        addOutputWrite(model, "o", bit, qRead);
        require(verifies(model), "async reset fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventActivation->empty(),
                "async reset: sink-only event carriers must produce an empty activation map");
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

    // (5) clk0/clk1 dual domains: one cluster per clock, each counter write
    // anchored in its own SinkEvent supernode. Toggling one clock must neither
    // fire the other domain (signature isolation) nor drop its pending data
    // activation (a spurious fire would clear the sticky flag and lose it).
    void dualClockTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_dual_clock");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
            addEventStateWrite(model, one, sum, mask, cnt, {{clk, "posedge"}});
            return cntRead;
        };
        const auto c0 = counter("clk0", "c0");
        const auto c1 = counter("clk1", "c1");
        addOutputWrite(model, "o0", byte, c0);
        addOutputWrite(model, "o1", byte, c1);
        require(verifies(model), "dual clock fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        // V2 (M2): both counter writes are SinkEvent sinks, so the P_event
        // activation map is empty. Functional isolation is asserted by the
        // driver (a clk0 edge must never fire the clk1 counter's write).
        require(mapping.schedule->eventActivation->empty(),
                "dual clock: sink-only event carriers must produce an empty activation map");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto qRead = addStateRead(model, q, "q_r");
        const auto notQ = addCompute(model, "core.compute.not", bit, "not_q", {qRead});
        addEventStateWrite(model, one, notQ, one, q, {{clk, "posedge"}});
        addEventStateWrite(model, one, d, one, q2, {{qRead, "posedge"}});
        addOutputWrite(model, "oq", bit, qRead);
        addOutputWrite(model, "oq2", bit, addStateRead(model, q2, "q2_r"));
        require(verifies(model), "glitch clock fixture rejected");
        runSixPhasePipeline(model, false, false, tinyTu);
        const auto &mapping = *model.cpuMapping();
        require(mapping.schedule->eventActivation->empty(),
                "glitch clock: sink-only event carriers must produce an empty activation map");
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

    StateId addMemState(GrhSimModel &model, std::string_view name, TypeId type, std::string fillLiteral)
    {
        const auto id = model.addState(name, type);
        const std::array params{Parameter{model.intern("value"), std::move(fillLiteral)}};
        const std::array steps{InitStep{model.intern("core.init.fill"), {0, 1}}};
        model.addInit(id, steps, params);
        return id;
    }

    // Indexed state read (stage-06): result is the addressed element (k ==
    // rank) or remaining-dims sub-array type; indices are the row-major
    // address operands i0..ik-1.
    ValueId addArrayRead(GrhSimModel &model, StateId state, TypeId result,
                         std::initializer_list<ValueId> indices, std::string_view name)
    {
        const auto value = model.addValue(result, name);
        model.addOperation("core.state.readArray", {indices.begin(), indices.size()},
                           std::array{value}, std::array{ObjectRef::state(state)});
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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

    // (7b) V2-M3 ICG shape (isomorphic to CASE_025 / XS SRAMTemplate): a
    // transparent-low latch captures the request enable while clk is low,
    // gclk = clk & EN, and the macro registers plus the Memory write are
    // clocked by posedge gclk. The latch write is the event-free sink
    // (SinkEscape, fires every round); the three posedge-gclk writes cluster
    // into one SinkEvent supernode whose scan call site gates on the
    // signature act and whose body carries NO per-op eventActStore guard
    // (V2-M3 guard drop; the writeArray's reqWrite data condition survives).
    // Functional core: the enable must publish while clk is low so gclk can
    // rise — the pre-v2 activation deadlock held every macro register at its
    // init value forever.
    void icgGatedSramTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_icg_sram");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape); // 16 B: regLatch class
        const auto clk = addInputRead(model, "clk", bit);
        const auto reqRead = addInputRead(model, "reqRead", bit);
        const auto reqWrite = addInputRead(model, "reqWrite", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto one = addConstant(model, bit, "1'b1");
        const auto mask4 = addConstant(model, addrType, "4'hf");
        const auto mask8 = addConstant(model, word, "8'hff");
        const auto en = addCompute(model, "core.compute.or", bit, "en", {reqRead, reqWrite});
        const auto notClk = addCompute(model, "core.compute.not", bit, "not_clk", {clk});
        const auto EN = addState(model, "EN", bit, "1'b0");
        const auto raddrD = addState(model, "raddr_d", addrType, "4'h0");
        const auto renD = addState(model, "ren_d", bit, "1'b0");
        const auto mem = addMemState(model, "Memory", memType, "8'h00");
        const auto gclk = addCompute(model, "core.compute.and", bit, "gclk",
                                     {clk, addStateRead(model, EN, "EN_r")});
        model.addOperation("core.state.latchWrite", std::array{notClk, en, one}, {},
                           std::array{ObjectRef::state(EN)});
        addEventStateWrite(model, one, waddr, mask4, raddrD, {{gclk, "posedge"}});
        addEventStateWrite(model, one, en, one, renD, {{gclk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {reqWrite, waddr, wdata, mask8}, mem,
                      {{gclk, "posedge"}});
        addOutputWrite(model, "o", word,
                       addArrayRead(model, mem, word, {addStateRead(model, raddrD, "raddr_r")}, "mread"));
        addOutputWrite(model, "oren", bit, addStateRead(model, renD, "ren_r"));
        require(verifies(model), "ICG fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        // regLatch-class write: the mem write lives in the sink supernode
        // body (the P_mem plan stays empty), where the guard drop applies.
        require(mapping.schedule->memWritePlan && mapping.schedule->memWritePlan->empty(),
                "ICG fixture writes must stay out of the P_mem write plan");
        // No non-sink op carries an event here: the activation map is empty
        // and the edge reaches the sink cluster purely through its gate.
        require(mapping.schedule->eventActivation && mapping.schedule->eventActivation->empty(),
                "ICG fixture should have no non-sink event carriers");
        compileAndRun(model, root / "icg_sram", {
            // Write 0x5a -> Memory[3] (one clk low/high pair per request).
            {{{"clk", "false"}, {"reqRead", "false"}, {"reqWrite", "false"}, {"waddr", "0"},
              {"wdata", "0"}}, {{"o", "0"}, {"oren", "false"}}},
            {{{"reqWrite", "true"}, {"waddr", "3"}, {"wdata", "90"}}, {{"o", "0"}, {"oren", "false"}}},
            {{{"clk", "true"}}, {{"o", "90"}, {"oren", "true"}}},  // posedge gclk: the deadlock repro
            {{{"clk", "false"}}, {{"o", "90"}, {"oren", "true"}}},
            {{{"reqWrite", "false"}}, {{"o", "90"}, {"oren", "true"}}},
            {{{"clk", "true"}}, {{"o", "90"}, {"oren", "true"}}},  // gated: EN=0, no posedge
            {{{"clk", "false"}, {"reqRead", "true"}, {"waddr", "5"}}, {{"o", "90"}, {"oren", "true"}}},
            {{{"clk", "true"}}, {{"o", "0"}, {"oren", "true"}}},   // read cycle: raddr_d<=5, no write
            {{{"clk", "false"}, {"reqRead", "false"}}, {{"o", "0"}, {"oren", "true"}}},
            {{{"clk", "true"}}, {{"o", "0"}, {"oren", "true"}}},   // EN=0: no posedge
            {{{"reqRead", "true"}, {"waddr", "3"}}, {{"o", "0"}, {"oren", "true"}}}, // en high at clk high: latch closed
            {{{"clk", "false"}}, {{"o", "0"}, {"oren", "true"}}},  // latch opens: EN=1
            {{{"clk", "true"}}, {{"o", "90"}, {"oren", "true"}}},  // posedge: raddr_d<=3, mem[3] kept
        });
        // V2-M3 source pin: exactly one guard-shaped actBitsGuard text
        // remains — the SinkEvent scan gate. Pre-M3 the three in-body op
        // guards added three more; the dumpState print shares the bit-read
        // text and is excluded by its static_cast prefix.
        const auto source = concatModelSources(root / "icg_sram" / "model");
        const auto countOf = [&source](const std::string &needle) {
            std::size_t count = 0;
            for (std::size_t at = 0; (at = source.find(needle, at)) != std::string::npos; at += needle.size())
                ++count;
            return count;
        };
        const auto guards = countOf("((eventActStore[") - countOf("static_cast<unsigned>((eventActStore[");
        require(guards == 1,
                "ICG fixture: expected exactly the sink-cluster call-site gate, got " +
                std::to_string(guards) + " eventActStore guard texts");
    }

    // (7c) A1 sink enable-guard subdivision: eight writes sharing one
    // computed enable and one event signature carve out a guard-gated
    // SinkEvent supernode — the scan call site ANDs one boundary read of the
    // enable into the actBitsGuard gate, while the constant-enabled writes
    // keep the plain signature gate. The member write ops are untouched
    // (the V2-M3 in-body guard drop applies to every sink body), so the only
    // new text is the call-site conjunction. Functionally the guard
    // suppresses exactly the shared-enable writes while en is low.
    void sinkEnableGuardTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_sink_guard");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto enA = addInputRead(model, "enA", bit);
        const auto enB = addInputRead(model, "enB", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto one = addConstant(model, bit, "1'b1");
        // A computed enable crosses the supernode boundary as a named
        // boundary field — the typical XS shape (en decoded by comb logic).
        const auto en = addCompute(model, "core.compute.or", bit, "en", {enA, enB});
        std::vector<ValueId> guardedReads;
        std::vector<StateId> guardedStates;
        for (uint32_t i = 0; i < 8; ++i)
        {
            const auto q = addState(model, "q" + std::to_string(i), bit, "1'b0");
            addEventStateWrite(model, en, d, one, q, {{clk, "posedge"}});
            guardedStates.push_back(q);
            guardedReads.push_back(addStateRead(model, q, "q" + std::to_string(i) + "_r"));
        }
        const auto r0 = addState(model, "r0", bit, "1'b0");
        const auto r1 = addState(model, "r1", bit, "1'b0");
        addEventStateWrite(model, one, d, one, r0, {{clk, "posedge"}});
        addEventStateWrite(model, one, d, one, r1, {{clk, "posedge"}});
        auto any = guardedReads.front(), all = guardedReads.front();
        for (uint32_t i = 1; i < guardedReads.size(); ++i)
        {
            any = addCompute(model, "core.compute.or", bit, "qany" + std::to_string(i), {any, guardedReads[i]});
            all = addCompute(model, "core.compute.and", bit, "qall" + std::to_string(i), {all, guardedReads[i]});
        }
        addOutputWrite(model, "qAny", bit, any);
        addOutputWrite(model, "qAll", bit, all);
        addOutputWrite(model, "r0o", bit, addStateRead(model, r0, "r0_r"));
        addOutputWrite(model, "r1o", bit, addStateRead(model, r1, "r1_r"));
        require(verifies(model), "sink-guard fixture rejected");
        runSixPhasePipeline(model, false);
        const auto &mapping = *model.cpuMapping();
        const auto &tree = mapping.partitionTree;
        const auto opCount = [&](const CpuPartition &supernode) {
            uint64_t count = 0;
            for (auto child : supernode.children) count += tree.partitions[child.index - 1].ops.size();
            return count;
        };
        // Exactly one guard supernode: SinkEvent, enableGuard naming the
        // shared enable value, holding exactly the eight shared-en writes.
        const CpuPartition *guardSupernode = nullptr;
        uint32_t guardCount = 0;
        for (const auto &partition : tree.partitions)
        {
            if (!partition.attrs.enableGuard) continue;
            ++guardCount;
            guardSupernode = &partition;
        }
        require(guardCount == 1 && guardSupernode, "sink-guard: expected exactly one guard supernode");
        // The guard names exactly the value the member writes see as their
        // en operand — the semantic passes may have rewired the fixture-time
        // value, so compare against the live operand.
        uint32_t sharedEnIndex = 0;
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) != "core.state.write") continue;
            const auto refs = model.objectRefs(op);
            if (refs.size() != 1) continue;
            bool guardedWrite = false;
            for (auto state : guardedStates) guardedWrite = guardedWrite || refs.front().index == state.index;
            if (!guardedWrite) continue;
            const auto enOperand = model.operands(op).front().index;
            if (!sharedEnIndex) sharedEnIndex = enOperand;
            require(enOperand == sharedEnIndex, "sink-guard: guarded writes disagree on the enable value");
        }
        require(sharedEnIndex != 0, "sink-guard: guarded writes not found");
        const auto &stores = *mapping.dataLayout->namedStores;
        const auto smallEnableStore = std::find_if(stores.begin(), stores.end(), [](const CpuNamedStore &store) {
            return store.kind == CpuNamedStoreKind::SmallSinkEnableBoundary;
        });
        require(smallEnableStore != stores.end() && smallEnableStore->enableBitValues.size() == 1 &&
                smallEnableStore->enableBitValues.front().index == sharedEnIndex,
                "sink-guard: small-sink enable bitmap layout lost the shared enable");
        require(*guardSupernode->attrs.enableGuard == int64_t(sharedEnIndex),
                "sink-guard: enableGuard does not name the shared enable");
        require(guardSupernode->attrs.supernodeCategory &&
                *guardSupernode->attrs.supernodeCategory == CpuSupernodeCategory::SmallSinkEvent,
                "sink-guard: guard supernode is not a SinkEvent");
        require(guardSupernode->attrs.eventActs && guardSupernode->attrs.eventActs->size() == 1,
                "sink-guard: guard supernode signature wrong");
        require(opCount(*guardSupernode) == 8,
                "sink-guard: guard supernode does not hold the eight shared-enable writes");
        // The ordinary signature cluster keeps the two constant-enable
        // writes and carries no guard.
        bool sawOrdinary = false;
        for (const auto &partition : tree.partitions)
        {
            if (partition.attrs.kind != CpuPartitionKind::Supernode || partition.attrs.enableGuard ||
                !partition.attrs.supernodeCategory ||
                *partition.attrs.supernodeCategory != CpuSupernodeCategory::SmallSinkEvent)
                continue;
            require(!sawOrdinary, "sink-guard: more than one ordinary event cluster");
            sawOrdinary = true;
            require(opCount(partition) == 2,
                    "sink-guard: ordinary cluster does not hold exactly the constant-enable writes");
        }
        require(sawOrdinary, "sink-guard: ordinary event cluster missing");
        compileAndRun(model, root / "sink_guard", {
            {{{"clk", "false"}, {"enA", "false"}, {"enB", "false"}, {"d", "true"}},
             {{"qAny", "false"}, {"qAll", "false"}, {"r0o", "false"}, {"r1o", "false"}}},
            // en low at the posedge: only the constant-enabled pair writes.
            {{{"clk", "true"}}, {{"qAny", "false"}, {"qAll", "false"}, {"r0o", "true"}, {"r1o", "true"}}},
            // Raising en without an edge changes nothing.
            {{{"clk", "false"}, {"enA", "true"}}, {{"qAny", "false"}, {"qAll", "false"}}},
            // en high at the posedge: every guarded write lands.
            {{{"clk", "true"}}, {{"qAny", "true"}, {"qAll", "true"}}},
            // d low with en via the other leg: writes of 0 land everywhere.
            {{{"clk", "false"}, {"enA", "false"}, {"enB", "true"}, {"d", "false"}},
             {{"qAll", "true"}, {"r0o", "true"}}},
            {{{"clk", "true"}}, {{"qAny", "false"}, {"qAll", "false"}, {"r0o", "false"}, {"r1o", "false"}}},
        });
        // The shared-enable cluster tests its dense enable bit before
        // invoking the sink body. The two event signatures still gate at the
        // call sites rather than inside each write body.
        const auto source = concatModelSources(root / "sink_guard" / "model");
        const auto countOf = [&source](const std::string &needle) {
            std::size_t count = 0;
            for (std::size_t at = 0; (at = source.find(needle, at)) != std::string::npos; at += needle.size())
                ++count;
            return count;
        };
        require(countOf("smallSinkEnableBoundaryStore.enabledWords[0]&UINT64_C(0x1)") == 1,
                "sink-guard: expected one bitmap-gated sink call site");
        const auto guards = countOf("((eventActStore[") - countOf("static_cast<unsigned>((eventActStore[");
        require(guards == 2,
                "sink-guard: per-op event guards leaked back into the sink bodies, got " +
                std::to_string(guards) + " eventActStore guard texts");
    }

    void smallSinkEnableBatchTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_small_sink_batch");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto en0 = addInputRead(model, "en0", bit);
        const auto en1 = addInputRead(model, "en1", bit);
        const auto d = addInputRead(model, "d", bit);
        const auto one = addConstant(model, bit, "1'b1");
        const auto q0 = addState(model, "q0", bit, "1'b0");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        addEventStateWrite(model, en0, d, one, q0, {{clk, "posedge"}});
        addEventStateWrite(model, en1, d, one, q1, {{clk, "posedge"}});
        addOutputWrite(model, "o0", bit, addStateRead(model, q0, "q0_r"));
        addOutputWrite(model, "o1", bit, addStateRead(model, q1, "q1_r"));
        require(verifies(model), "small-sink batch fixture rejected");
        runSixPhasePipeline(model, false, false, false, 1);
        const auto &stores = *model.cpuMapping()->dataLayout->namedStores;
        const auto enableStore = std::find_if(stores.begin(), stores.end(), [](const CpuNamedStore &store) {
            return store.kind == CpuNamedStoreKind::SmallSinkEnableBoundary;
        });
        require(enableStore != stores.end() && enableStore->enableBitValues.size() == 2,
                "small-sink batch layout must assign two enable bits");
        compileAndRun(model, root / "small_sink_batch", {
            {{{"clk", "false"}, {"en0", "false"}, {"en1", "false"}, {"d", "true"}},
             {{"o0", "false"}, {"o1", "false"}}},
            {{{"clk", "true"}}, {{"o0", "false"}, {"o1", "false"}}},
            {{{"clk", "false"}, {"en0", "true"}}, {}},
            {{{"clk", "true"}}, {{"o0", "true"}, {"o1", "false"}}},
            {{{"clk", "false"}, {"en0", "false"}, {"en1", "true"}}, {}},
            {{{"clk", "true"}}, {{"o0", "true"}, {"o1", "true"}}},
            {{{"clk", "false"}, {"en0", "true"}, {"en1", "false"}, {"d", "false"}}, {}},
            {{{"clk", "true"}}, {{"o0", "false"}, {"o1", "true"}}},
        });
        const auto source = concatModelSources(root / "small_sink_batch" / "model");
        require(source.find("smallSinkEnableBoundaryStore.enabledWords[0]&UINT64_C(0x3)") != std::string::npos,
                "small-sink batch scan did not combine the adjacent enable bits");
    }

    // (8) Event-free array writes converge via cell change detection:
    // writeArray (single row), fill (broadcast), whole-array write (copy from
    // an initialized source). Every write re-runs every round while enabled,
    // so any missed change detection would loop to MAX_ROUND and throw.
    void memConvergeTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_converge");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto flatMask = model.logicType(128, false, LogicDomain::TwoState);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto fen = addInputRead(model, "fen", bit);
        const auto fdata = addInputRead(model, "fdata", word);
        const auto cen = addInputRead(model, "cen", bit);
        const auto mask = addConstant(model, word, "8'hff");
        const auto fullMask = addConstant(model, flatMask, "128'hffffffffffffffffffffffffffffffff");
        const auto memA = addMemState(model, "memA", memType, "8'h00");
        const auto memF = addMemState(model, "memF", memType, "8'ha5");
        const auto memC = addMemState(model, "memC", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, mask}, memA, {});
        addEventWrite(model, "core.state.fill", {fen, fdata}, memF, {});
        const auto fRead = model.addValue(memType, "f_read");
        model.addOperation("core.state.read", {}, std::array{fRead}, std::array{ObjectRef::state(memF)});
        // Stage-06: the retired whole-array assign op is core.state.write; the
        // all-ones mask constant folds away in the emission.
        addEventWrite(model, "core.state.write", {cen, fRead, fullMask}, memC, {});
        addOutputWrite(model, "o", word, addArrayRead(model, memA, word, {raddr}, "ra"));
        addOutputWrite(model, "of", word, addArrayRead(model, memF, word, {raddr}, "rf"));
        addOutputWrite(model, "oc", word, addArrayRead(model, memC, word, {raddr}, "rc"));
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

    // (9) Static/dynamic reader discrimination: an event-gated writeArray with
    // a dynamic address; one reader reads the constant row 3, one reads a
    // dynamic address. The write's readers table must carry the static row for
    // the former (emit: conditional activation on overlap) and none for the
    // latter (emit: unconditional on any change). Functionally, a write to
    // row 5 must refresh only the dynamic reader's latch; a write to row 3
    // refreshes both.
    void arrayReaderGatingTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_gating");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto one = addConstant(model, bit, "1'b1");
        const auto three = addConstant(model, addrType, "4'h3");
        const auto mask = addConstant(model, word, "8'hff");
        const auto mem = addMemState(model, "mem", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, mask}, mem, {{clk, "posedge"}});
        const auto rStatic = addArrayRead(model, mem, word, {three}, "r_static");
        const auto rDyn = addArrayRead(model, mem, word, {raddr}, "r_dyn");
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
        // and the P_mem reader re-activation (nonSinkActiveFlagNext) fires it.
        runSixPhasePipeline(model, false, false, false, 8, 8);
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
        const auto pLargeSinkAt = text.find("::pLargeSink_c0()");
        require(pLargeSinkAt != std::string::npos, "pLargeSink chunk body missing");
        const auto pLargeSinkEnd = text.find("::pPublish()", pLargeSinkAt);
        const std::string pLargeSink = text.substr(pLargeSinkAt, pLargeSinkEnd == std::string::npos ? pLargeSinkEnd : pLargeSinkEnd - pLargeSinkAt);
        require(pLargeSink.find("==3)nonSinkActiveFlagNext[") != std::string::npos,
                "static reader should be activated under an address-overlap guard");
        unsigned activations = 0;
        for (std::size_t at = 0; (at = pLargeSink.find("nonSinkActiveFlagNext[", at)) != std::string::npos; ++at) ++activations;
        require(activations == 2, "write body should activate exactly its two readers");
    }

    void multiDimReaderGatingTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_md_reader_gating");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> shape{2, 3};
        const auto array = model.logicType(48, false, LogicDomain::TwoState, shape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto wi = addInputRead(model, "wi", bit);
        const auto wj = addInputRead(model, "wj", idx2);
        const auto wdata = addInputRead(model, "wdata", byte);
        const auto ri = addInputRead(model, "ri", bit);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto jzero = addConstant(model, idx2, "2'h0");
        const auto jtwo = addConstant(model, idx2, "2'h2");
        const auto mask = addConstant(model, byte, "8'hff");
        const auto mem = addMemState(model, "arr", array, "8'h00");
        addEventWrite(model, "core.state.writeArray", {wen, wi, wj, wdata, mask},
                      mem, {{clk, "posedge"}});
        const auto r02 = addArrayRead(model, mem, byte, {zero, jtwo}, "r02");
        const auto r10 = addArrayRead(model, mem, byte, {one, jzero}, "r10");
        const auto rdyn = addArrayRead(model, mem, byte, {ri, rj}, "rdyn");
        const auto s02 = addState(model, "s02", byte, "8'h00");
        const auto s10 = addState(model, "s10", byte, "8'h00");
        const auto sdyn = addState(model, "sdyn", byte, "8'h00");
        model.addOperation("core.state.latchWrite", std::array{one, r02, mask}, {},
                           std::array{ObjectRef::state(s02)});
        model.addOperation("core.state.latchWrite", std::array{one, r10, mask}, {},
                           std::array{ObjectRef::state(s10)});
        model.addOperation("core.state.latchWrite", std::array{one, rdyn, mask}, {},
                           std::array{ObjectRef::state(sdyn)});
        addOutputWrite(model, "o02", byte, addStateRead(model, s02, "s02_r"));
        addOutputWrite(model, "o10", byte, addStateRead(model, s10, "s10_r"));
        addOutputWrite(model, "odyn", byte, addStateRead(model, sdyn, "sdyn_r"));
        require(verifies(model), "multi-dimensional reader gating fixture rejected");
        runSixPhasePipeline(model, false, false, false, 8, 2);
        const auto &plan = *model.cpuMapping()->schedule->memWritePlan;
        require(plan.size() == 1, "multi-dimensional reader gating write plan size wrong");
        bool row2 = false, row3 = false, dynamic = false;
        for (const auto &reader : plan[0].readers)
        {
            row2 |= reader.staticRow == std::optional<uint64_t>(2);
            row3 |= reader.staticRow == std::optional<uint64_t>(3);
            dynamic |= !reader.staticRow;
        }
        require(row2 && row3 && dynamic, "multi-dimensional reader row mapping is wrong");
        compileAndRun(model, root / "md_reader_gating", {
            {{{"clk", "false"}, {"wen", "false"}, {"wi", "false"}, {"wj", "2"},
              {"wdata", "0"}, {"ri", "false"}, {"rj", "2"}},
             {{"o02", "0"}, {"o10", "0"}, {"odyn", "0"}}},
            {{{"wen", "true"}, {"wdata", "0x12"}, {"clk", "true"}},
             {{"o02", "0x12"}, {"o10", "0"}, {"odyn", "0x12"}}},
            {{{"clk", "false"}, {"wi", "true"}, {"wj", "0"}, {"wdata", "0x34"},
              {"ri", "true"}, {"rj", "0"}},
             {{"o02", "0x12"}, {"o10", "0"}, {"odyn", "0"}}},
            {{{"clk", "true"}}, {{"o02", "0x12"}, {"o10", "0x34"}, {"odyn", "0x34"}}},
            {{{"clk", "false"}, {"wi", "false"}, {"wdata", "0x56"},
              {"ri", "false"}},
             {{"o02", "0x12"}, {"o10", "0x34"}, {"odyn", "0"}}},
            {{{"clk", "true"}}, {{"o02", "0x12"}, {"o10", "0x34"}, {"odyn", "0x56"}}},
        });
    }

    // (10) Declaration-order write priority (stage-06: the retired write
    // sequence op desugars to one writeArray per port; same-state writes apply
    // in declaration order, so the LAST declared write wins an address
    // collision). Three ports: A writes row 3 a constant 0xaa, B overwrites
    // row 3 with input data, C (declared last) writes input address/data under
    // an enable.
    void memPriorityTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_mem_priority");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto enc = addInputRead(model, "enc", bit);
        const auto bdata = addInputRead(model, "bdata", word);
        const auto caddr = addInputRead(model, "caddr", addrType);
        const auto cdata = addInputRead(model, "cdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto one = addConstant(model, bit, "1'b1");
        const auto three = addConstant(model, addrType, "4'h3");
        const auto aa = addConstant(model, word, "8'haa");
        const auto mask8 = addConstant(model, word, "8'hff");
        const auto mem = addMemState(model, "mem", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {one, three, aa, mask8},
                      mem, {{clk, "posedge"}}, "wa");
        addEventWrite(model, "core.state.writeArray", {one, three, bdata, mask8},
                      mem, {{clk, "posedge"}}, "wb");
        addEventWrite(model, "core.state.writeArray", {enc, caddr, cdata, mask8},
                      mem, {{clk, "posedge"}}, "wc");
        addOutputWrite(model, "o", word, addArrayRead(model, mem, word, {raddr}, "r"));
        require(verifies(model), "mem priority fixture rejected");
        runSixPhasePipeline(model, false, true, tinyTu);
        const auto &mapping = *model.cpuMapping();
        // One plan entry per writeArray, declaration rank in execution order.
        // Op ids renumber across the compacting pipeline, so order is checked
        // through the op names (which survive).
        const auto &plan = *mapping.schedule->memWritePlan;
        const auto planNameOf = [&](const CpuMemWritePlanEntry &entry) {
            return std::string(model.text(model.operations()[entry.writeOp.index - 1].name));
        };
        require(plan.size() == 3 && planNameOf(plan[0]) == "wa" && planNameOf(plan[1]) == "wb" &&
                planNameOf(plan[2]) == "wc", "mem priority should schedule lowest rank first");
        require(plan[0].priority == 0 && plan[1].priority == 1 && plan[2].priority == 2,
                "mem priority ranks should ascend by declaration order");
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

    // (10b) P_large_sink enable bitmap: boundary-backed 1-bit write enables
    // become dense enabledWords reads (an aggregate word-OR wraps each run of
    // fully shadowed ports), kept in sync by the boundary compare-store /
    // pInput hooks and built once in init(). --mem-enable-bitmap off restores
    // the plain scattered boundary reads.
    void memEnableBitmapTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_bitmap");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto bypass = addInputRead(model, "bypass", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto mask = addConstant(model, word, "8'hff");
        // Computed enable: boundary-published by its supernode, mirrored
        // through the publishBoundary hook. wen drives the second port
        // directly (input-backed enable, mirrored through the pInput hook).
        const auto nb = addCompute(model, "core.compute.not", bit, "nb", {bypass});
        const auto en = addCompute(model, "core.compute.and", bit, "en", {wen, nb});
        const auto memA = addMemState(model, "memA", memType, "8'h00");
        const auto memB = addMemState(model, "memB", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {en, waddr, wdata, mask}, memA, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, mask}, memB, {{clk, "posedge"}});
        addOutputWrite(model, "oa", word, addArrayRead(model, memA, word, {raddr}, "ra"));
        addOutputWrite(model, "ob", word, addArrayRead(model, memB, word, {raddr}, "rb"));
        require(verifies(model), "mem enable bitmap fixture rejected");
        runSixPhasePipeline(model, false, true);
        compileAndRun(model, root / "mem_enable_bitmap", {
            {{{"clk", "false"}, {"wen", "false"}, {"bypass", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"raddr", "0"}},
             {{"oa", "0"}, {"ob", "0"}}},
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "29"}, {"raddr", "3"}}, {{"oa", "0"}, {"ob", "0"}}},
            {{{"clk", "true"}}, {{"oa", "29"}, {"ob", "29"}}},   // en=wen&!bypass=1: both arrays written
            {{{"clk", "false"}}, {{"oa", "29"}, {"ob", "29"}}},
            {{{"bypass", "true"}, {"wdata", "47"}}, {{"oa", "29"}, {"ob", "29"}}},
            {{{"clk", "true"}}, {{"oa", "29"}, {"ob", "47"}}},   // en=0: memA holds; memB (wen) takes 47
            {{{"bypass", "false"}, {"clk", "false"}}, {{"oa", "29"}, {"ob", "47"}}},
            {{{"clk", "true"}}, {{"oa", "47"}, {"ob", "47"}}},   // en=1 again: memA catches up
        });
        const std::string text = concatModelSources(root / "mem_enable_bitmap" / "model");
        require(text.find("largeSinkEnableBoundaryStore.enabledWords[") != std::string::npos,
                "large-sink enable bitmap missing from emitted model");
        {
            std::ifstream headerStream(root / "mem_enable_bitmap" / "model" / "grhsim_phase_mem_bitmap.hpp");
            const std::string header{std::istreambuf_iterator<char>(headerStream), std::istreambuf_iterator<char>()};
            require(header.find("std::array<std::uint64_t,1> enabledWords;") != std::string::npos,
                    "large-sink enable bitmap member declaration missing");
        }
        const auto countOccurrences = [](const std::string &haystack, const std::string &needle) {
            unsigned count = 0;
            for (std::size_t at = 0; (at = haystack.find(needle, at)) != std::string::npos; at += needle.size())
                ++count;
            return count;
        };
        const auto pLargeSinkAt = text.find("::pLargeSink_c0()");
        require(pLargeSinkAt != std::string::npos, "pLargeSink chunk body missing");
        const auto pLargeSinkEnd = text.find("\nvoid ", pLargeSinkAt + 1);
        const auto pLargeSink = text.substr(pLargeSinkAt, pLargeSinkEnd == std::string::npos ? pLargeSinkEnd : pLargeSinkEnd - pLargeSinkAt);
        require(pLargeSink.find("largeSinkEnableBoundaryStore.enabledWords[0]") != std::string::npos,
                "pLargeSink chunk does not read the enable bitmap");
        require(pLargeSink.find(")!=UINT64_C(0)){") != std::string::npos,
                "fully shadowed port run misses the aggregate word skip");
        // The compare-store hook mirrors the computed enable into its bit, and
        // init() builds every bit from the boundary fields.
        require(text.find("?UINT64_C(0x") != std::string::npos &&
                text.find("largeSinkEnableBoundaryStore.enabledWords[0]=(largeSinkEnableBoundaryStore.") != std::string::npos,
                "init-time bitmap build missing");
        require(text.find("=(cpu_value)?(largeSinkEnableBoundaryStore.enabledWords[") != std::string::npos,
                "publishBoundary enable sync hook missing");
        // Off mode: no bitmap anywhere, and the pLargeSink chunk keeps the two
        // scattered boundary enable reads the bitmap replaced.
        const auto offDir = root / "mem_enable_bitmap_off";
        std::filesystem::remove_all(offDir);
        std::filesystem::create_directories(offDir);
        {
            diag::Diagnostics offDiagnostics;
            const auto offResult = emitSixPhaseCpuCpp(model, offDir / "model", offDiagnostics, false, false);
            require(offResult.success, "mem-enable-bitmap off emit failed");
        }
        const std::string offText = concatModelSources(offDir / "model");
        require(offText.find("enabledWords") == std::string::npos,
                "mem-enable-bitmap off still emits the shadow bitmap");
        const auto offMemAt = offText.find("::pLargeSink_c0()");
        require(offMemAt != std::string::npos, "off-mode pLargeSink chunk body missing");
        const auto offMemEnd = offText.find("\nvoid ", offMemAt + 1);
        const auto offMem = offText.substr(offMemAt, offMemEnd == std::string::npos ? offMemEnd : offMemEnd - offMemAt);
        require(countOccurrences(offMem, "largeSinkEnableBoundaryStore.mem") == 2 &&
                countOccurrences(pLargeSink, "largeSinkEnableBoundaryStore.mem") == 0,
                "bitmap should remove exactly the two enable reads from the pLargeSink chunk");
    }

    // (10d) --mem-write-activate on-write: event-gated P_mem writes drop the
    // per-cell change compare and activate readers in one burst per
    // enable-word run; observable behavior is unchanged (same stimulus
    // vectors as the bitmap test must produce identical outputs).
    void writeOnWriteTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_onwrite");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto bypass = addInputRead(model, "bypass", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto mask = addConstant(model, word, "8'hff");
        const auto nb = addCompute(model, "core.compute.not", bit, "nb", {bypass});
        const auto en = addCompute(model, "core.compute.and", bit, "en", {wen, nb});
        const auto memA = addMemState(model, "memA", memType, "8'h00");
        const auto memB = addMemState(model, "memB", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {en, waddr, wdata, mask}, memA, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, mask}, memB, {{clk, "posedge"}});
        addOutputWrite(model, "oa", word, addArrayRead(model, memA, word, {raddr}, "ra"));
        addOutputWrite(model, "ob", word, addArrayRead(model, memB, word, {raddr}, "rb"));
        require(verifies(model), "mem write on-write fixture rejected");
        runSixPhasePipeline(model, false, true);
        compileAndRun(model, root / "mem_onwrite", {
            {{{"clk", "false"}, {"wen", "false"}, {"bypass", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"raddr", "0"}},
             {{"oa", "0"}, {"ob", "0"}}},
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "29"}, {"raddr", "3"}}, {{"oa", "0"}, {"ob", "0"}}},
            {{{"clk", "true"}}, {{"oa", "29"}, {"ob", "29"}}},
            {{{"clk", "false"}}, {{"oa", "29"}, {"ob", "29"}}},
            {{{"bypass", "true"}, {"wdata", "47"}}, {{"oa", "29"}, {"ob", "29"}}},
            {{{"clk", "true"}}, {{"oa", "29"}, {"ob", "47"}}},
            {{{"bypass", "false"}, {"clk", "false"}}, {{"oa", "29"}, {"ob", "47"}}},
            {{{"clk", "true"}}, {{"oa", "47"}, {"ob", "47"}}},
        }, false, {}, false, /*memWriteOnWrite=*/true);
        const std::string text = concatModelSources(root / "mem_onwrite" / "model");
        require(text.find("cpu_cell!=cpu_next") == std::string::npos,
                "on-write mode still emits the per-cell change compare");
        {
            std::ifstream headerStream(root / "mem_onwrite" / "model" / "grhsim_phase_mem_onwrite.hpp");
            const std::string header{std::istreambuf_iterator<char>(headerStream), std::istreambuf_iterator<char>()};
            require(header.find("--mem-write-activate on-write") != std::string::npos,
                    "on-write marker missing from the generated header");
        }
        require(text.find("mem-write-activate fallback") == std::string::npos,
                "event-gated fixture should have no on-change fallback");
        require(text.find("nonSinkActiveFlagNext[") != std::string::npos,
                "on-write mode dropped reader activation entirely");
    }

    // (10c) P_mem act clustering + dead compare-store elimination: ports
    // sharing one eventActStore guard get a single hoisted test per run; a
    // boundary value consumed only by P_mem (no supernode fanout, no bitmap
    // hook) publishes with a plain store instead of a compare-store.
    void memActClusterDeadCompareTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_cluster");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto e1 = addInputRead(model, "e1", bit);
        const auto e2 = addInputRead(model, "e2", bit);
        const auto a = addInputRead(model, "a", word);
        const auto b = addInputRead(model, "b", word);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto mask = addConstant(model, word, "8'hff");
        // Computed enable (keeps its compare-store: the enable bitmap hook is
        // the change consumer); computed data values feed only P_mem ports, so
        // their publishes must degenerate to plain stores.
        const auto en = addCompute(model, "core.compute.and", bit, "en", {e1, e2});
        const auto sum = addCompute(model, "core.compute.add", word, "sum", {a, b});
        const auto diff = addCompute(model, "core.compute.sub", word, "diff", {a, b});
        const auto memA = addMemState(model, "memA", memType, "8'h00");
        const auto memB = addMemState(model, "memB", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {en, waddr, sum, mask}, memA, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {en, waddr, diff, mask}, memB, {{clk, "posedge"}});
        addOutputWrite(model, "oa", word, addArrayRead(model, memA, word, {raddr}, "ra"));
        addOutputWrite(model, "ob", word, addArrayRead(model, memB, word, {raddr}, "rb"));
        require(verifies(model), "mem act cluster fixture rejected");
        runSixPhasePipeline(model, false, true);
        compileAndRun(model, root / "mem_act_cluster", {
            {{{"clk", "false"}, {"e1", "false"}, {"e2", "false"}, {"a", "0"}, {"b", "0"}, {"waddr", "0"}, {"raddr", "0"}},
             {{"oa", "0"}, {"ob", "0"}}},
            {{{"e1", "true"}, {"e2", "true"}, {"a", "30"}, {"b", "12"}, {"waddr", "5"}, {"raddr", "5"}},
             {{"oa", "0"}, {"ob", "0"}}},
            {{{"clk", "true"}}, {{"oa", "42"}, {"ob", "18"}}},    // both ports fire on the same edge
            {{{"clk", "false"}}, {{"oa", "42"}, {"ob", "18"}}},
            {{{"e2", "false"}, {"a", "7"}, {"b", "1"}}, {{"oa", "42"}, {"ob", "18"}}},
            {{{"clk", "true"}}, {{"oa", "42"}, {"ob", "18"}}},    // en=0: neither writes
            {{{"e2", "true"}, {"clk", "false"}}, {{"oa", "42"}, {"ob", "18"}}},
            {{{"clk", "true"}}, {{"oa", "8"}, {"ob", "6"}}},      // en=1 again: both write
        });
        const std::string text = concatModelSources(root / "mem_act_cluster" / "model");
        const auto countOccurrences = [](const std::string &haystack, const std::string &needle) {
            unsigned count = 0;
            for (std::size_t at = 0; (at = haystack.find(needle, at)) != std::string::npos; at += needle.size())
                ++count;
            return count;
        };
        const auto pLargeSinkAt = text.find("::pLargeSink_c0()");
        require(pLargeSinkAt != std::string::npos, "pLargeSink chunk body missing");
        const auto pLargeSinkEnd = text.find("\nvoid ", pLargeSinkAt + 1);
        const auto pLargeSink = text.substr(pLargeSinkAt, pLargeSinkEnd == std::string::npos ? pLargeSinkEnd : pLargeSinkEnd - pLargeSinkAt);
        // Act clustering: the two same-act ports share one hoisted guard.
        require(countOccurrences(pLargeSink, "((eventActStore") == 1,
                "same-act ports were not folded into one hoisted guard");
        // Dead compare elimination: the computed data values (sum/diff) are
        // P_mem-only, so their publishes are unconditional stores; the hooked
        // enable keeps its compare-store (exactly one left in the model).
        unsigned plainStores = 0;
        for (const auto &line : linesOf(text))
        {
            const auto first = line.find_first_not_of(' ');
            if (first == std::string::npos || line.find("if(") != std::string::npos) continue;
            if (line.compare(first, 30, "largeSinkPayloadBoundaryStore.") == 0 &&
                line.find('=') != std::string::npos)
                ++plainStores;
        }
        require(plainStores >= 2, "P_mem-only boundary values still carry compare-stores");
        require(countOccurrences(text, "!=cpu_value") == 1,
                "only the hooked enable publish should keep its compare-store");
    }

    // (11) General-phase $display under the V2 (M2) firing rules: the
    // event-free task anchors a SinkEscape supernode that fires EVERY round,
    // so it reprints on every eval (including evals whose only change is an
    // unrelated clock edge or the OTHER task's data operand — en-gating
    // alone decides, there is no operand-change prefilter). The event-guarded
    // task anchors a SinkEvent supernode gated on its eventActStore signature,
    // so it prints on every posedge — the pre-v2 "edge with no data change
    // does not re-fire" divergence (old plan §100) is gone.
    void generalDisplayTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_display");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        runSixPhasePipeline(model, false);
        const std::vector<DriveStep> steps{
            {{{"a", "false"}, {"d", "0"}, {"clk", "false"}}, {}},   // init fire: comb a=0
            {{{"a", "true"}}, {}},                                   // comb a=1
            {{{"d", "1"}}, {}},                                      // escape reprints: comb a=1
            {{{"clk", "true"}}, {}},                                 // comb a=1, then the edge: ev d=1
            {{{"clk", "false"}}, {}},                                // escape reprints: comb a=1
            {{{"a", "false"}}, {}},                                  // comb a=0
            {{{"clk", "true"}}, {}},                                 // comb a=0, then ev d=1 (every edge fires)
            {{{"d", "2"}, {"clk", "false"}}, {}},                    // escape reprints: comb a=0
            {{{"clk", "true"}}, {}},                                 // comb a=0, then ev d=2
        };
        const auto output = compileAndRun(model, root / "display", steps, true);
        const auto lines = linesOf(output);
        const std::vector<std::string> expected{"comb a=0", "comb a=1", "comb a=1", "comb a=1",
                                                "ev d=1", "comb a=1", "comb a=0", "comb a=0",
                                                "ev d=1", "comb a=0", "comb a=0", "ev d=2"};
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        const auto &timeslot = (*mapping.dataLayout->namedStores)[10];
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        const auto &timeslot = (*mapping.dataLayout->namedStores)[10];
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
    // supplies the extern "C" definitions. V2 (M2): both calls share one
    // non-sink supernode (the V2-M1 merge has no event prohibition); the
    // P_event activation map raises its nonSinkActiveFlag on the gated call's
    // edge, and the per-op guard skips the gated call on data-only rounds.
    void dpiSmokeTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_dpi_smoke");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        runSixPhasePipeline(model, false, false, tinyTu);
        // V2 (M2): the shared non-sink supernode carries the gated call's act,
        // so the activation map holds exactly that one entry.
        const auto &activation = *model.cpuMapping()->schedule->eventActivation;
        require(activation.size() == 1 && activation.front().act == 0 &&
                activationBits(activation.front()) == 1,
                "dpi smoke: activation map should cover exactly the shared call supernode");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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

    void unsupportedAggregateEmitTest(const std::filesystem::path &root)
    {
        const auto expectRejected = [&](GrhSimModel &model, std::string_view expected) {
            require(verifies(model), "unsupported aggregate fixture rejected by IR verifier");
            runSixPhasePipeline(model, false);
            diag::Diagnostics diagnostics;
            const auto result = emitSixPhaseCpuCpp(model, root / std::string(model.text(model.name())), diagnostics);
            require(!result.success && diagnostics.hasError(), "unsupported aggregate CPU emit succeeded");
            const bool found = std::any_of(diagnostics.messages().begin(), diagnostics.messages().end(),
                [&](const auto &message) { return message.message.find(expected) != std::string::npos; });
            require(found, "unsupported aggregate CPU emit gave the wrong diagnostic");
        };
        {
            GrhSimModel model("phase_four_state_aggregate_reject");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const std::array<int64_t, 1> shape{2};
            const auto array = model.logicType(8, false, LogicDomain::FourState, shape);
            addOutputWrite(model, "o", array, addInputRead(model, "a", array));
            expectRejected(model, "four-state aggregate");
        }
        for (const auto direction : {DpiDirection::Input, DpiDirection::Output, DpiDirection::Inout})
        {
            const std::string name = "phase_dpi_aggregate_arg_" + std::to_string(static_cast<int>(direction));
            GrhSimModel model(name);
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const std::array<int64_t, 1> shape{2};
            const auto array = model.logicType(8, false, LogicDomain::TwoState, shape);
            const std::array args{DpiArgument{model.intern("array"), direction, array}};
            model.addExternFunction("aggregate_arg", "core.dpi", "aggregate_arg", args, {});
            expectRejected(model, "aggregate parameter/return ABI is not implemented (P1)");
        }
        {
            GrhSimModel model("phase_dpi_aggregate_return");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const std::array<int64_t, 1> shape{2};
            const auto array = model.logicType(8, false, LogicDomain::TwoState, shape);
            const std::array<DpiArgument, 0> args{};
            model.addExternFunction("aggregate_return", "core.dpi", "aggregate_return", args, array);
            expectRejected(model, "aggregate parameter/return ABI is not implemented (P1)");
        }
    }

    // (15) Perf counters under WOLVRIX_GRHSIM_PERF=1: the 8-field aggregate uses
    // the fixed contract names with new-model semantics (see report table).
    void perfCountersTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_perf");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
                      // V2-M1: the latch write anchors its own escape sink
                      // supernode, so each eval fires two batches (the
                      // non-sink input supernode plus the sink supernode).
                      "c.computeBatchExecCount!=8||c.touchedStateShadowCount!=3||c.touchedWriteCount!=0)return 1;\n"
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
    // re-publishes the boundary copy and the event-free whole-array write
    // downstream picks up the new contents within the same eval.
    void wholeWriteReadbackTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_mem_assign_readback");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape);
        const auto flatMask = model.logicType(128, false, LogicDomain::TwoState);
        const auto wen = addInputRead(model, "wen", bit);
        const auto waddr = addInputRead(model, "waddr", addrType);
        const auto wdata = addInputRead(model, "wdata", word);
        const auto cen = addInputRead(model, "cen", bit);
        const auto raddr = addInputRead(model, "raddr", addrType);
        const auto mask = addConstant(model, word, "8'hff");
        const auto fullMask = addConstant(model, flatMask, "128'hffffffffffffffffffffffffffffffff");
        const auto memF = addMemState(model, "memF", memType, "8'h00");
        const auto memC = addMemState(model, "memC", memType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, mask}, memF, {});
        const auto fRead = model.addValue(memType, "f_read");
        model.addOperation("core.state.read", {}, std::array{fRead}, std::array{ObjectRef::state(memF)});
        // Stage-06: the retired whole-array assign op is core.state.write.
        addEventWrite(model, "core.state.write", {cen, fRead, fullMask}, memC, {});
        addOutputWrite(model, "oc", word, addArrayRead(model, memC, word, {raddr}, "rc"));
        require(verifies(model), "whole-write readback fixture rejected");
        runSixPhasePipeline(model, false, true);
        const auto &mapping = *model.cpuMapping();
        const auto &plan = *mapping.schedule->memWritePlan;
        require(plan.size() == 2, "whole-write readback should have two write plan entries");
        // The write into memF must list the whole-array read's supernode as a
        // dynamic reader (plan order is op-id order: the writeArray precedes
        // the whole-array write, whose target memC has no General readers).
        require(plan[0].readers.size() == 1 && !plan[0].readers[0].staticRow,
                "memF write should carry exactly one dynamic whole-array reader");
        require(plan[1].readers.empty(), "memC whole write should have no readers");
        compileAndRun(model, root / "mem_assign_readback", {
            {{{"wen", "false"}, {"waddr", "0"}, {"wdata", "0"}, {"cen", "false"}, {"raddr", "0"}}, {{"oc", "0"}}},
            {{{"cen", "true"}}, {{"oc", "0"}}},                       // copy of all-zero memF: no change
            {{{"wen", "true"}, {"waddr", "3"}, {"wdata", "90"}, {"raddr", "3"}}, {{"oc", "90"}}},
            // ^ write memF[3], reader supernode re-publishes f_read, the whole
            // write copies it — all within this eval's round loop.
            {{{"wen", "false"}}, {{"oc", "90"}}},                     // stable
            {{{"wen", "true"}, {"wdata", "119"}}, {{"oc", "119"}}},   // overwrite propagates again
            {{{"wen", "false"}}, {{"oc", "119"}}},
        });
    }

    void crossEventPriorityTest(const std::filesystem::path &root, bool largeSink)
    {
        GrhSimModel model(largeSink ? "phase_large_cross_event_priority"
                                    : "phase_small_cross_event_priority");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto clkLo = addInputRead(model, "clkLo", bit);
        const auto clkHi = addInputRead(model, "clkHi", bit);
        const auto enLo = addInputRead(model, "enLo", bit);
        const auto enHi = addInputRead(model, "enHi", bit);
        const auto loData = addInputRead(model, "loData", byte);
        const auto hiData = addInputRead(model, "hiData", byte);
        const auto loMask = addConstant(model, byte, "8'h0f");
        const auto hiMask = addConstant(model, byte, "8'h03");
        ValueId output;
        if (largeSink)
        {
            const std::array<int64_t, 1> shape{2};
            const auto array = model.logicType(16, false, LogicDomain::TwoState, shape);
            const auto zero = addConstant(model, bit, "1'b0");
            const auto state = addMemState(model, "q", array, "8'ha0");
            addEventWrite(model, "core.state.writeArray", {enLo, zero, loData, loMask},
                          state, {{clkLo, "posedge"}}, "low");
            addEventWrite(model, "core.state.writeArray", {enHi, zero, hiData, hiMask},
                          state, {{clkHi, "posedge"}}, "high");
            output = addArrayRead(model, state, byte, {zero}, "q_r");
        }
        else
        {
            const auto state = addState(model, "q", byte, "8'ha0");
            addEventWrite(model, "core.state.write", {enLo, loData, loMask},
                          state, {{clkLo, "posedge"}}, "low");
            addEventWrite(model, "core.state.write", {enHi, hiData, hiMask},
                          state, {{clkHi, "posedge"}}, "high");
            output = addStateRead(model, state, "q_r");
        }
        addOutputWrite(model, "o", byte, output);
        require(verifies(model), "cross-event priority fixture rejected");
        runSixPhasePipeline(model, false, largeSink);
        std::vector<std::string> orderedWrites;
        if (largeSink)
        {
            for (const auto &entry : *model.cpuMapping()->schedule->memWritePlan)
            {
                require(entry.priority == (orderedWrites.empty() ? 0u : 1u),
                        "large-sink cross-event priority rank is not ascending");
                orderedWrites.emplace_back(model.text(model.operations()[entry.writeOp.index - 1].name));
            }
        }
        else
        {
            const auto &tree = model.cpuMapping()->partitionTree;
            for (const auto supernode : generalSupernodeOrder(tree))
                for (const auto node : tree.partitions[supernode.index - 1].children)
                    for (const auto opId : tree.partitions[node.index - 1].ops)
                    {
                        const auto name = model.text(model.operations()[opId.index - 1].name);
                        if (name == "high" || name == "low") orderedWrites.emplace_back(name);
                    }
        }
        require(orderedWrites == std::vector<std::string>{"low", "high"},
                "cross-event writes were not scheduled by ascending priority");
        const std::vector<DriveStep> steps{
            {{{"clkLo", "false"}, {"clkHi", "false"}, {"enLo", "false"}, {"enHi", "false"},
              {"loData", "0x0a"}, {"hiData", "0x01"}}, {{"o", "0xa0"}}},
            {{{"enLo", "true"}, {"clkLo", "true"}}, {{"o", "0xaa"}}},
            {{{"clkLo", "false"}}, {}},
            {{{"enLo", "false"}, {"enHi", "true"}, {"clkHi", "true"}}, {{"o", "0xa9"}}},
            {{{"clkHi", "false"}}, {}},
            {{{"enLo", "true"}, {"loData", "0x05"}, {"hiData", "0x02"},
              {"clkLo", "true"}, {"clkHi", "true"}}, {{"o", "0xa6"}}},
            {{{"clkLo", "false"}, {"clkHi", "false"}, {"enHi", "false"}}, {}},
            {{{"loData", "0x0f"}, {"clkLo", "true"}}, {{"o", "0xaf"}}},
        };
        compileAndRun(model, root / (largeSink ? "large_cross_event_priority"
                                               : "small_cross_event_priority"), steps);
        if (largeSink)
            compileAndRun(model, root / "large_cross_event_priority_onwrite", steps,
                          false, {}, false, true);
    }

    // Overlapping event-free writes can change a cell twice while leaving its
    // final value unchanged. General readers must settle on that final value
    // instead of keeping eval alive because of the intermediate writes.
    void eventFreePriorityConvergeTest(const std::filesystem::path &root, bool largeSink, bool wide = false)
    {
        const std::string suffix = wide ? "_wide" : "";
        const std::string caseName = std::string(largeSink ? "large_free_priority" : "small_free_priority") + suffix;
        GrhSimModel model("phase_" + caseName);
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const uint32_t width = wide ? 80 : 8;
        const auto word = model.logicType(width, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> shape{2};
        const auto array = model.logicType(width * 2, false, LogicDomain::TwoState, shape);
        const auto enLo = addInputRead(model, "enLo", bit);
        const auto enHi = addInputRead(model, "enHi", bit);
        const auto loData = addInputRead(model, "loData", word);
        const auto hiData = addInputRead(model, "hiData", word);
        const auto hiMask = addInputRead(model, "hiMask", word);
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto loMask = addConstant(model, word, std::to_string(width) + "'h0f");
        const auto fullMask = addConstant(model, word, wide ? "80'hffffffffffffffffffff" : "8'hff");
        const std::string initial = wide ? "80'h123400000000000000a0" : "8'ha0";
        const auto scalar = addState(model, "q", word, initial);
        const auto mem = addMemState(model, "mem", array, initial);
        addEventWrite(model, "core.state.write", {enLo, loData, loMask}, scalar, {}, "scalar_low");
        addEventWrite(model, "core.state.write", {enHi, hiData, hiMask}, scalar, {}, "scalar_high");
        addEventWrite(model, "core.state.writeArray", {enLo, zero, loData, loMask}, mem, {}, "array_low");
        addEventWrite(model, "core.state.writeArray", {enHi, zero, hiData, hiMask}, mem, {}, "array_high");
        const auto scalarRead = addStateRead(model, scalar, "q_r");
        const auto arrayRead = addArrayRead(model, mem, word, {zero}, "mem_r");
        const auto scalarSeen = addState(model, "q_seen", word, "0");
        const auto arraySeen = addState(model, "mem_seen", word, "0");
        model.addOperation("core.state.latchWrite", std::array{one, scalarRead, fullMask}, {},
                           std::array{ObjectRef::state(scalarSeen)});
        model.addOperation("core.state.latchWrite", std::array{one, arrayRead, fullMask}, {},
                           std::array{ObjectRef::state(arraySeen)});
        addOutputWrite(model, "os", word, addStateRead(model, scalarSeen, "q_seen_r"));
        addOutputWrite(model, "oa", word, addStateRead(model, arraySeen, "mem_seen_r"));
        require(verifies(model), "event-free priority fixture rejected");
        const bool tinyTu = largeSink && wide;
        runSixPhasePipeline(model, false, largeSink, tinyTu);
        std::set<std::string> generalReads;
        for (const auto &op : model.operations())
        {
            if (op.phase != SimPhase::General ||
                (model.text(op.opType) != "core.state.read" &&
                 model.text(op.opType) != "core.state.readArray")) continue;
            const auto refs = model.objectRefs(op);
            if (refs.size() == 1 && refs[0].kind == ObjectKind::State)
                generalReads.emplace(model.text(model.states()[refs[0].index - 1].name));
        }
        require(generalReads.contains("q") && generalReads.contains("mem"),
                "event-free priority must keep General readers for both conflicting states");
        if (tinyTu)
        {
            std::map<std::string, uint32_t> chunkByWrite;
            const auto &mapping = *model.cpuMapping();
            const auto &plan = *mapping.schedule->memWritePlan;
            for (const auto &unit : mapping.translationUnits->units)
                for (const auto &chunk : unit.chunks)
                {
                    if (chunk.kind != CpuEmitChunkKind::LargeSink) continue;
                    for (uint32_t i = chunk.offset; i < chunk.offset + chunk.count; ++i)
                        chunkByWrite.emplace(model.text(model.operations()[plan[i].writeOp.index - 1].name),
                                             chunk.offset);
                }
            require(chunkByWrite.at("scalar_low") != chunkByWrite.at("scalar_high") &&
                    chunkByWrite.at("array_low") != chunkByWrite.at("array_high"),
                    "event-free priority must split conflicting writes across LargeSink chunks");
        }
        std::vector<DriveStep> steps{
            {{{"enLo", "false"}, {"enHi", "false"}, {"loData", "0x05"},
              {"hiData", "0x02"}, {"hiMask", "0x03"}}, {{"os", "0xa0"}, {"oa", "0xa0"}}},
            {{{"enLo", "true"}, {"enHi", "true"}}, {{"os", "0xa6"}, {"oa", "0xa6"}}},
            {{}, {{"os", "0xa6"}, {"oa", "0xa6"}}},
            {{{"loData", "0x09"}}, {{"os", "0xaa"}, {"oa", "0xaa"}}},
            {{}, {{"os", "0xaa"}, {"oa", "0xaa"}}},
            {{{"hiMask", "0"}, {"loData", "0x05"}}, {{"os", "0xa5"}, {"oa", "0xa5"}}},
            {{}, {{"os", "0xa5"}, {"oa", "0xa5"}}},
            {{{"hiMask", "0x03"}, {"enHi", "false"}, {"loData", "0x09"}},
             {{"os", "0xa9"}, {"oa", "0xa9"}}},
            {{}, {{"os", "0xa9"}, {"oa", "0xa9"}}},
            {{{"enHi", "true"}}, {{"os", "0xaa"}, {"oa", "0xaa"}}},
            {{{"enLo", "false"}, {"enHi", "false"}}, {{"os", "0xaa"}, {"oa", "0xaa"}}},
        };
        // The wide variant must retain bits 79:64 through all masked writes
        // and through the General reader's full-width latch copy.
        if (wide)
            for (auto &step : steps)
                for (auto &[port, expected] : step.expects)
                    expected = "(((unsigned _BitInt(80))UINT64_C(0x1234)<<64)|UINT64_C(" + expected + "))";
        compileAndRun(model, root / caseName, steps, false, {}, tinyTu);
        if (largeSink)
            compileAndRun(model, root / (caseName + "_onwrite"), steps, false, {}, tinyTu, true);
    }

    // (17) Packed fill: a whole packed-array write (ingest lowers packed
    // aggregate whole-writes to core.state.fill with packed-width data) must
    // slice the data per row — the legacy isPackedFill semantics — on both the
    // scalar (<=64-bit) and wide packed-data paths.
    void packedFillTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_packed_fill");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", bit);
        const auto one = addConstant(model, bit, "1'b1");
        // Scalar packed: two 2-bit rows, fill data 4'h9 -> row0=1, row1=2.
        const auto pair = model.logicType(2, false, LogicDomain::TwoState);
        const auto packed4 = model.logicType(4, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> shapeS{2};
        const auto memS = addMemState(model, "memS", model.logicType(4, false, LogicDomain::TwoState, shapeS),
                                      "2'h0");
        addEventWrite(model, "core.state.fill", {one, addConstant(model, packed4, "4'h9")},
                      memS, {{clk, "posedge"}});
        // Wide packed: sixteen 8-bit rows, fill data 128'h0f0e...0100 -> row i = i.
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto packed128 = model.logicType(128, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> shapeW{16};
        const auto memW = addMemState(model, "memW", model.logicType(128, false, LogicDomain::TwoState, shapeW),
                                      "8'h00");
        addEventWrite(model, "core.state.fill",
                      {one, addConstant(model, packed128, "128'h0f0e0d0c0b0a09080706050403020100")},
                      memW, {{clk, "posedge"}});
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto row0 = addConstant(model, addrType, "4'h0");
        const auto row1 = addConstant(model, addrType, "4'h1");
        const auto row15 = addConstant(model, addrType, "4'hf");
        addOutputWrite(model, "s0", pair, addArrayRead(model, memS, pair, {row0}, "s0_r"));
        addOutputWrite(model, "s1", pair, addArrayRead(model, memS, pair, {row1}, "s1_r"));
        addOutputWrite(model, "w0", byte, addArrayRead(model, memW, byte, {row0}, "w0_r"));
        addOutputWrite(model, "w15", byte, addArrayRead(model, memW, byte, {row15}, "w15_r"));
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
    // plan stays empty). Covers the declaration-order priority chain (the
    // retired write-sequence op's shape: the last write wins a row collision),
    // a masked writeArray (merge into the next buffer), event-free fill
    // convergence, and the NBA visibility contract at eval granularity (the
    // same trace shape as the mem-class equivalents).
    void regLatchArrayWriteTest(const std::filesystem::path &root, bool tinyTu = false)
    {
        GrhSimModel model("phase_reglatch_write");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto addrType = model.logicType(4, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 1> memShape{16};
        const auto memType = model.logicType(128, false, LogicDomain::TwoState, memShape); // 16 B: regLatch class
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
        const auto mask8 = addConstant(model, word, "8'hff");
        const auto nibble = addConstant(model, word, "8'h0f");
        const auto tabS = addMemState(model, "tabS", memType, "8'h00");
        const auto tabM = addMemState(model, "tabM", memType, "8'h00");
        const auto tabF = addMemState(model, "tabF", memType, "8'h00");
        // Declaration-order priority chain (event-gated), a masked writeArray
        // and an event-free fill — all on regLatch-class arrays.
        addEventWrite(model, "core.state.writeArray", {one, three, aa, mask8},
                      tabS, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {one, three, bdata, mask8},
                      tabS, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {enc, caddr, cdata, mask8},
                      tabS, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {wen, waddr, wdata, nibble}, tabM, {});
        addEventWrite(model, "core.state.fill", {fen, fdata}, tabF, {});
        addOutputWrite(model, "o", word, addArrayRead(model, tabS, word, {raddr}, "rs"));
        addOutputWrite(model, "om", word, addArrayRead(model, tabM, word, {raddr}, "rm"));
        addOutputWrite(model, "of", word, addArrayRead(model, tabF, word, {raddr}, "rf"));
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

    // ----- Stage-06 multi-dimensional array coverage -----

    const CpuStoreField *findStoreField(const GrhSimModel &model, CpuNamedStoreKind kind,
                                        std::string_view name)
    {
        for (const auto &store : *model.cpuMapping()->dataLayout->namedStores)
        {
            if (store.kind != kind) continue;
            for (const auto &field : store.fields)
                if (model.text(field.name) == name) return &field;
        }
        return nullptr;
    }

    // Each dimension remains an Array layer in the physical type.
    void requireMultiDimArrayPhysical(const GrhSimModel &model, const CpuStoreField &field,
                                      std::span<const int64_t> shape, uint32_t elementWidth,
                                      std::string_view tag)
    {
        const auto &types = model.cpuMapping()->dataLayout->types;
        CpuTypeId type = field.type;
        for (const int64_t extent : shape)
        {
            const auto &physical = types[type.index - 1];
            require(physical.kind == CpuTypeKind::Array && physical.count == uint64_t(extent),
                    std::string(tag) + ": physical type lost a dimension");
            type = physical.elementType;
        }
        const auto &element = types[type.index - 1];
        require(element.kind == CpuTypeKind::UInt && element.width == elementWidth,
                std::string(tag) + ": physical element type wrong");
    }

    // (S1) shape [3,4] (12 rows x 8 bits) end to end: a two-index writeArray
    // write port and a two-index readArray read port, including a masked
    // write. The row-major linearization (row = i*4 + j) is checked on both
    // store classes (mem via --mem-min-bytes 0, regLatch via the default
    // threshold).
    void multiDimArrayTest(const std::filesystem::path &root, bool allMem)
    {
        GrhSimModel model(allMem ? "phase_md_array_mem" : "phase_md_array_reg");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{3, 4};
        const auto arrType = model.logicType(96, false, LogicDomain::TwoState, arrShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto wi = addInputRead(model, "wi", idx2);
        const auto wj = addInputRead(model, "wj", idx2);
        const auto wdata = addInputRead(model, "wdata", byte);
        const auto wmask = addInputRead(model, "wmask", byte);
        const auto ri = addInputRead(model, "ri", idx2);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto arr = addMemState(model, "arr", arrType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {wen, wi, wj, wdata, wmask},
                      arr, {{clk, "posedge"}}, "w");
        addOutputWrite(model, "o", byte, addArrayRead(model, arr, byte, {ri, rj}, "r"));
        require(verifies(model), "multi-dim array fixture rejected");
        runSixPhasePipeline(model, false, allMem);
        const auto &mapping = *model.cpuMapping();
        const auto *arrField = findStoreField(model, allMem ? CpuNamedStoreKind::LargeSink
                                                            : CpuNamedStoreKind::SmallSink, "arr");
        require(bool(arrField) && arrField->state == arr, "multi-dim array: store field missing");
        requireMultiDimArrayPhysical(model, *arrField, arrShape, 8, "multi-dim array");
        if (allMem)
        {
            const auto &plan = *mapping.schedule->memWritePlan;
            // Op ids renumber across the compacting pipeline; track the write
            // through its name instead.
            require(plan.size() == 1 &&
                        model.text(model.operations()[plan[0].writeOp.index - 1].name) == "w",
                    "multi-dim array: mem-class write missing from the P_mem plan");
        }
        else
        {
            require(mapping.schedule->memWritePlan && mapping.schedule->memWritePlan->empty(),
                    "multi-dim array: regLatch-class write must stay out of the P_mem plan");
        }
        compileAndRun(model, root / (allMem ? "md_array_mem" : "md_array_reg"), {
            {{{"clk", "false"}, {"wen", "false"}, {"wi", "0"}, {"wj", "0"}, {"wdata", "0"},
              {"wmask", "0xff"}, {"ri", "0"}, {"rj", "0"}}, {{"o", "0"}}},
            // arr[1][2] (row 6) <= 0x5a on the posedge.
            {{{"wen", "true"}, {"wi", "1"}, {"wj", "2"}, {"wdata", "0x5a"}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "0"}}},
            {{{"clk", "false"}, {"ri", "1"}, {"rj", "2"}}, {{"o", "0x5a"}}},
            // arr[0][3] (row 3) <= 0xa5.
            {{{"wi", "0"}, {"wj", "3"}, {"wdata", "0xa5"}, {"clk", "true"}}, {{"o", "0x5a"}}},
            {{{"clk", "false"}, {"ri", "0"}, {"rj", "3"}}, {{"o", "0xa5"}}},
            // Masked RMW on arr[1][2]: (0x5a & 0xf0) | (0x3c & 0x0f) = 0x5c.
            {{{"wi", "1"}, {"wj", "2"}, {"wdata", "0x3c"}, {"wmask", "0x0f"}, {"clk", "true"}}, {{"o", "0xa5"}}},
            {{{"clk", "false"}, {"ri", "1"}, {"rj", "2"}}, {{"o", "0x5c"}}},
            // An edge with wen low changes nothing.
            {{{"wen", "false"}, {"clk", "true"}}, {{"o", "0x5c"}}},
        });
    }

    // (S2) partial indexing (k < rank): on a shape [2,3] array a single index
    // addresses the 3-row sub-array (data/mask pack 3 x 8 bits); cross-checked
    // against full two-index accesses — sub-array block [i] holds rows
    // 3i..3i+2 with row 3i+j = [i][j] (row-major). Runs on both store classes.
    void partialIndexArrayTest(const std::filesystem::path &root, bool allMem)
    {
        GrhSimModel model(allMem ? "phase_md_partial_mem" : "phase_md_partial_reg");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto flat24 = model.logicType(24, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{2, 3};
        const auto arrType = model.logicType(48, false, LogicDomain::TwoState, arrShape);
        const std::array<int64_t, 1> rowShape{3};
        const auto rowType = model.logicType(24, false, LogicDomain::TwoState, rowShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto wdata = addInputRead(model, "wdata", rowType);  // packed 3 rows
        const auto wmask = addInputRead(model, "wmask", flat24);
        const auto wen2 = addInputRead(model, "wen2", bit);
        const auto pi = addInputRead(model, "pi", bit);
        const auto fi = addInputRead(model, "fi", bit);
        const auto fj = addInputRead(model, "fj", idx2);
        const auto fdata = addInputRead(model, "fdata", byte);
        const auto ri = addInputRead(model, "ri", bit);
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto arr = addMemState(model, "arr", arrType, "8'h00");
        // Constant full-index reads pin the row-major layout.
        const auto c1 = addConstant(model, bit, "1'b1");
        const auto c0 = addConstant(model, idx2, "2'h0");
        const auto c2 = addConstant(model, idx2, "2'h2");
        // The single-index write selects a three-row block dynamically.
        addEventWrite(model, "core.state.writeArray", {wen, pi, wdata, wmask}, arr, {{clk, "posedge"}});
        addEventWrite(model, "core.state.writeArray", {wen2, fi, fj, fdata, mask8}, arr, {{clk, "posedge"}});
        addOutputWrite(model, "orow", rowType, addArrayRead(model, arr, rowType, {ri}, "row_r"));
        addOutputWrite(model, "oc0", byte, addArrayRead(model, arr, byte, {c1, c0}, "c10_r"));
        addOutputWrite(model, "oc2", byte, addArrayRead(model, arr, byte, {c1, c2}, "c12_r"));
        require(verifies(model), "partial-index fixture rejected");
        runSixPhasePipeline(model, false, allMem);
        compileAndRun(model, root / (allMem ? "md_partial_mem" : "md_partial_reg"), {
            {{{"clk", "false"}, {"wen", "false"}, {"wdata", "0"},
              {"wmask", "0xffffff"}, {"wen2", "false"}, {"fi", "false"}, {"fj", "0"},
              {"fdata", "0"}, {"pi", "true"}, {"ri", "false"}}, {{"orow", "0"}, {"oc0", "0"}, {"oc2", "0"}}},
            // Sub-array write: arr[1][0..2] <= {0x11, 0x22, 0x33}.
            {{{"wen", "true"}, {"wdata", "0x332211"}}, {{"orow", "0"}}},
            {{{"clk", "true"}}, {{"orow", "0"}, {"oc0", "0x11"}, {"oc2", "0x33"}}},
            {{{"clk", "false"}, {"ri", "true"}}, {{"orow", "0x332211"}}},
            // Masked sub-array write: only the middle row's slice is enabled.
            {{{"wdata", "0xaa00"}, {"wmask", "0x00ff00"}, {"clk", "true"}}, {{"orow", "0x33aa11"}}},
            {{{"clk", "false"}}, {{"orow", "0x33aa11"}, {"oc0", "0x11"}, {"oc2", "0x33"}}},
            // Full-index write into block 0.
            {{{"wen", "false"}, {"wen2", "true"}, {"fi", "false"}, {"fj", "1"}, {"fdata", "0x5a"},
              {"clk", "true"}}, {{"orow", "0x33aa11"}}},
            {{{"clk", "false"}, {"ri", "false"}}, {{"orow", "0x005a00"}}},
            // Full-index write into block 1 shows up in the sub-array read.
            {{{"fi", "true"}, {"fdata", "0x77"}, {"clk", "true"}}, {{"orow", "0x005a00"}}},
            {{{"clk", "false"}, {"ri", "true"}}, {{"orow", "0x337711"}, {"oc0", "0x11"}, {"oc2", "0x33"}}},
            // A dynamic partial index must scale by the inner dimension.
            {{{"pi", "false"}, {"wen", "true"}, {"wen2", "false"},
              {"wdata", "0x030201"}, {"wmask", "0xffffff"}, {"clk", "true"}},
             {{"orow", "0x337711"}, {"oc0", "0x11"}, {"oc2", "0x33"}}},
            {{{"clk", "false"}, {"ri", "false"}}, {{"orow", "0x030201"}}},
            {{{"pi", "true"}, {"wdata", "0x060504"}, {"clk", "true"}},
             {{"orow", "0x030201"}, {"oc0", "0x04"}, {"oc2", "0x06"}}},
        });
    }

    // (S3) declaration-order priority: two writeArray ports on the same state
    // write the same row in the same round; the LATER declared write wins
    // (stage-03 semantics). Runs on both store classes; the mem-class run also
    // pins the P_large_sink plan order/priority ranks.
    void multiDimPriorityTest(const std::filesystem::path &root, bool allMem)
    {
        GrhSimModel model(allMem ? "phase_md_prio_mem" : "phase_md_prio_reg");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{4, 4};
        const auto arrType = model.logicType(128, false, LogicDomain::TwoState, arrShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto enb = addInputRead(model, "enb", bit);
        const auto bdata = addInputRead(model, "bdata", byte);
        const auto ri = addInputRead(model, "ri", idx2);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto one = addConstant(model, bit, "1'b1");
        const auto ci = addConstant(model, idx2, "2'h1");
        const auto cj = addConstant(model, idx2, "2'h2");
        const auto aa = addConstant(model, byte, "8'haa");
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto arr = addMemState(model, "arr", arrType, "8'h00");
        addEventWrite(model, "core.state.writeArray", {one, ci, cj, aa, mask8},
                      arr, {{clk, "posedge"}}, "wa");
        addEventWrite(model, "core.state.writeArray", {enb, ci, cj, bdata, mask8},
                      arr, {{clk, "posedge"}}, "wb");
        addOutputWrite(model, "o", byte, addArrayRead(model, arr, byte, {ri, rj}, "r"));
        require(verifies(model), "multi-dim priority fixture rejected");
        runSixPhasePipeline(model, false, allMem);
        const auto &mapping = *model.cpuMapping();
        if (allMem)
        {
            // P_large_sink schedules writeA (priority 0) before writeB
            // (priority 1), so the later declaration overwrites it. Op ids
            // renumber across the compacting pipeline; track by name.
            const auto &plan = *mapping.schedule->memWritePlan;
            const auto planNameOf = [&](const CpuMemWritePlanEntry &entry) {
                return std::string(model.text(model.operations()[entry.writeOp.index - 1].name));
            };
            require(plan.size() == 2 && planNameOf(plan[0]) == "wa" && plan[0].priority == 0 &&
                    planNameOf(plan[1]) == "wb" && plan[1].priority == 1,
                    "multi-dim priority: write plan lost ascending priority");
        }
        else
        {
            require(mapping.schedule->memWritePlan && mapping.schedule->memWritePlan->empty(),
                    "multi-dim priority: regLatch-class writes must stay out of the P_mem plan");
        }
        compileAndRun(model, root / (allMem ? "md_prio_mem" : "md_prio_reg"), {
            {{{"clk", "false"}, {"enb", "false"}, {"bdata", "0"}, {"ri", "1"}, {"rj", "2"}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "0xaa"}}},   // only A fires: arr[1][2] <= 0xaa
            {{{"clk", "false"}}, {{"o", "0xaa"}}},
            // B (declared later) wins the row-6 collision.
            {{{"enb", "true"}, {"bdata", "0x5c"}, {"clk", "true"}}, {{"o", "0x5c"}}},
            {{{"bdata", "0x77"}, {"clk", "false"}}, {{"o", "0x5c"}}},
            {{{"clk", "true"}}, {{"o", "0x77"}}},   // A writes 0xaa, B overwrites 0x77
            {{{"enb", "false"}, {"clk", "false"}}, {{"o", "0x77"}}},
            {{{"clk", "true"}}, {{"o", "0xaa"}}},   // B disabled: A applies alone
        });
    }

    // (S4) wide elements x multi-dim: shape [2,3] with 96-bit rows (576 bits
    // total, 72 bytes). Full two-index writeArray/readArray round trip; runs
    // mem-class (72 B >= the default 64 B threshold) and regLatch-class
    // (--mem-min-bytes raised above the state size).
    void wideElementArrayTest(const std::filesystem::path &root, bool regLatch)
    {
        GrhSimModel model(regLatch ? "phase_md_wide_reg" : "phase_md_wide_mem");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto wide = model.logicType(96, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{2, 3};
        const auto arrType = model.logicType(576, false, LogicDomain::TwoState, arrShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto wi = addInputRead(model, "wi", bit);
        const auto wj = addInputRead(model, "wj", idx2);
        const auto wdata = addInputRead(model, "wdata", wide);
        const auto ri = addInputRead(model, "ri", bit);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto mask96 = addConstant(model, wide, "96'hffffffffffffffffffffffff");
        const auto arr = addMemState(model, "arr", arrType, "96'h0");
        addEventWrite(model, "core.state.writeArray", {wen, wi, wj, wdata, mask96}, arr,
                      {{clk, "posedge"}});
        addOutputWrite(model, "o", wide, addArrayRead(model, arr, wide, {ri, rj}, "r"));
        require(verifies(model), "wide-element fixture rejected");
        runSixPhasePipeline(model, false, false, false, 8, regLatch ? 1024 : 64);
        const auto &mapping = *model.cpuMapping();
        const auto *arrField = findStoreField(model, regLatch ? CpuNamedStoreKind::SmallSink
                                                              : CpuNamedStoreKind::LargeSink, "arr");
        require(bool(arrField) && arrField->state == arr, "wide-element: store field missing");
        // 96-bit rows round up to 16-byte _BitInt cells in a [2][3] array.
        requireMultiDimArrayPhysical(model, *arrField, arrShape, 96, "wide-element");
        const auto bits = [](const char *hi, const char *lo) {
            return "(((unsigned _BitInt(96))UINT64_C(" + std::string(hi) + ")<<64)|UINT64_C(" + lo + "))";
        };
        const std::string v1 = bits("0xdead", "0x123456789abcdef0");
        const std::string v2 = bits("0xbeef", "0x0f1e2d3c4b5a6978");
        compileAndRun(model, root / (regLatch ? "md_wide_reg" : "md_wide_mem"), {
            {{{"clk", "false"}, {"wen", "false"}, {"wi", "false"}, {"wj", "0"}, {"wdata", "0"},
              {"ri", "false"}, {"rj", "0"}}, {{"o", "0"}}},
            // arr[1][2] (row 5) <= v1.
            {{{"wen", "true"}, {"wi", "true"}, {"wj", "2"}, {"wdata", v1}}, {{"o", "0"}}},
            {{{"clk", "true"}}, {{"o", "0"}}},
            {{{"clk", "false"}, {"ri", "true"}, {"rj", "2"}}, {{"o", v1}}},
            // arr[0][1] (row 1) <= v2.
            {{{"wi", "false"}, {"wj", "1"}, {"wdata", v2}, {"clk", "true"}}, {{"o", v1}}},
            {{{"clk", "false"}, {"ri", "false"}, {"rj", "1"}}, {{"o", v2}}},
            // An edge with wen low changes nothing.
            {{{"wen", "false"}, {"clk", "true"}}, {{"o", v2}}},
        });
    }

    // (S5) FST per-element registration (stage-06): a shaped regLatch state
    // registers one var per element (arr[i][j], row-major, referencing the
    // flat smallSinkStore row), while an array whose physical byte volume
    // exceeds 512 is skipped whole. Follows waveformTest's emit/build/run
    // shape (the generated Makefile compiles the vendored libfst sources).
    void waveArrayTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_wave_array");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto word64 = model.logicType(64, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{2, 3};
        const auto arrType = model.logicType(48, false, LogicDomain::TwoState, arrShape);
        const std::array<int64_t, 2> bigShape{6, 11};
        const auto bigType = model.logicType(4224, false, LogicDomain::TwoState, bigShape); // 528 B > 512
        model.addInput("multi_input", arrType);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto wi = addInputRead(model, "wi", bit);
        const auto wj = addInputRead(model, "wj", idx2);
        const auto wdata = addInputRead(model, "wdata", byte);
        const auto ri = addInputRead(model, "ri", bit);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto zero = addConstant(model, bit, "1'b0");
        const auto zero64 = addConstant(model, word64, "64'h0");
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto arr = addMemState(model, "arr", arrType, "8'h00");
        const auto big = addMemState(model, "big", bigType, "64'h0");
        addEventWrite(model, "core.state.writeArray", {wen, wi, wj, wdata, mask8}, arr,
                      {{clk, "posedge"}});
        // A never-enabled fill keeps the big array a live regLatch state.
        addEventWrite(model, "core.state.fill", {zero, zero64}, big, {});
        addOutputWrite(model, "o", byte, addArrayRead(model, arr, byte, {ri, rj}, "r"));
        for (const char *symbol : {"clk", "arr", "big", "o", "multi_input"})
            model.addDeclaredSymbol(model.intern(symbol));
        require(verifies(model), "wave array fixture rejected");
        // --mem-min-bytes 1024 keeps the 528-byte big array in the regLatch
        // store, so its FST exclusion comes from the 512-byte wave cap and
        // not from the mem-class rule.
        runSixPhasePipeline(model, false, false, false, 8, 1024);
        const auto *arrField = findStoreField(model, CpuNamedStoreKind::SmallSink, "arr");
        const auto *bigField = findStoreField(model, CpuNamedStoreKind::SmallSink, "big");
        require(bool(arrField) && bool(bigField), "wave array: both arrays must stay regLatch-class");
        requireMultiDimArrayPhysical(model, *arrField, arrShape, 8, "wave array");
        const auto directory = root / "wave_array";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        {
            diag::Diagnostics diagnostics;
            const auto result = emitSixPhaseCpuCpp(model, directory / "model", diagnostics, true);
            for (const auto &message : diagnostics.messages())
                std::cout << message.context << ": " << message.message << '\n';
            require(result.success && !result.artifacts.empty(), "wave array emit failed");
        }
        const auto waveSource = concatModelSources(directory / "model");
        require(waveSource.find("wave_setup_0") != std::string::npos,
                "wave array emit produced no setup chunk");
        // One var per element, row-major: arr[0][0]..arr[0][2], arr[1][0]..
        // arr[1][2] referencing the matching multidimensional store cells.
        for (const char *name : {"\"arr[0][0]\"", "\"arr[0][1]\"", "\"arr[0][2]\"",
                                 "\"arr[1][0]\"", "\"arr[1][1]\"", "\"arr[1][2]\""})
            require(waveSource.find(name) != std::string::npos,
                    "wave array setup misses the per-element var " + std::string(name));
        require(waveSource.find("&smallSinkStore.arr[0][0]") != std::string::npos &&
                waveSource.find("&smallSinkStore.arr[1][2]") != std::string::npos,
                "wave array setup should reference the multidimensional store cells");
        require(waveSource.find("\"multi_input[0][2]\"") != std::string::npos &&
                waveSource.find("&this->multi_input[0][2]") != std::string::npos,
                "wave array setup should register multidimensional port cells");
        require(waveSource.find("wave_add(cpu_w,\"big") == std::string::npos,
                "the >512-byte array must be skipped whole");
        {
            std::ifstream generated(directory / "model" / "Makefile");
            const std::string text{std::istreambuf_iterator<char>(generated), std::istreambuf_iterator<char>()};
            require(text.find("fstapi.o") != std::string::npos && text.find("LIBFST_SRC_DIR") != std::string::npos,
                    "wave array Makefile misses the libfst rules");
        }
        {
            std::ofstream driver(directory / "driver.cpp");
            driver << "#include \"grhsim_phase_wave_array.hpp\"\n#include <cstdio>\n"
                      "int main(){\nGrhSIM_phase_wave_array sim;\nsim.init();\n"
                      "sim.configure_waveform(true,\"wave.fst\");\n"
                      "if(!sim.waveform_enabled())return 2;\n"
                      "sim.multi_input[0][2]=0xa5;\n"
                      "sim.clk=false;sim.wen=false;sim.wi=false;sim.wj=0;sim.wdata=0;sim.ri=true;sim.rj=2;sim.eval();\n"
                      "sim.wen=true;sim.wi=true;sim.wj=2;sim.wdata=0x5a;sim.eval();\n"
                      "sim.clk=true;sim.eval();\n"
                      "sim.clk=false;sim.eval();\n"
                      "sim.configure_waveform(false);\n"
                      "if(sim.o!=0x5a){std::fprintf(stderr,\"arr readback mismatch\\n\");return 1;}\n"
                      "std::printf(\"PASS\\n\");\nreturn 0;\n}\n";
        }
        command("make --no-print-directory -C " + quote((directory / "model").string()) + " -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
                " CXXFLAGS='-std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all'");
        command(quote(WOLVRIX_TEST_CXX) + " -std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all -I" +
                quote((directory / "model").string()) + " -I" + quote(WOLVRIX_TEST_LIBFST_INCLUDE_DIR) + " " +
                quote((directory / "driver.cpp").string()) + " " +
                quote((directory / "model" / "libgrhsim_phase_wave_array.a").string()) + " -lz -o " +
                quote((directory / "driver").string()));
        command("cd " + quote(directory.string()) + " && ./driver");
        const auto fst = directory / "wave.fst";
        require(std::filesystem::exists(fst) && !std::filesystem::is_empty(fst),
                "wave array capture produced no FST output");
    }

    // (S6) layout contract: a shape [2,3] state maps to the flat row-major
    // physical array Array(UInt8, 6) in whichever store its class selects, and
    // a two-index Mem-phase writeArray names its boundary slots
    // enable/idx0/idx1/data/mask (single-index writes keep the addr name).
    void multiDimLayoutTest()
    {
        const auto build = [](const char *name) {
            GrhSimModel model(name);
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const std::array<int64_t, 2> arrShape{2, 3};
            const auto arrType = model.logicType(48, false, LogicDomain::TwoState, arrShape);
            const auto clk = addInputRead(model, "clk", bit);
            const auto wen = addInputRead(model, "wen", bit);
            const auto wi = addInputRead(model, "wi", bit);
            const auto wj = addInputRead(model, "wj", idx2);
            const auto wdata = addInputRead(model, "wdata", byte);
            const auto ri = addInputRead(model, "ri", bit);
            const auto rj = addInputRead(model, "rj", idx2);
            const auto mask8 = addConstant(model, byte, "8'hff");
            const auto arr = addMemState(model, "arr", arrType, "8'h00");
            addEventWrite(model, "core.state.writeArray", {wen, wi, wj, wdata, mask8}, arr,
                          {{clk, "posedge"}});
            addOutputWrite(model, "o", byte, addArrayRead(model, arr, byte, {ri, rj}, "r"));
            require(verifies(model), "multi-dim layout fixture rejected");
            return model;
        };
        {
            auto model = build("phase_md_layout_mem");
            runSixPhasePipeline(model, false, true);
            const auto *arrField = findStoreField(model, CpuNamedStoreKind::LargeSink, "arr");
            require(bool(arrField), "multi-dim layout: mem field missing");
            const std::array<int64_t, 2> shape{2, 3};
            requireMultiDimArrayPhysical(model, *arrField, shape, 8, "multi-dim layout");
            require(findStoreField(model, CpuNamedStoreKind::LargeSinkEnableBoundary,
                                   "arr__w0__enable"),
                    "multi-dim layout: writeArray enable boundary slot missing");
            for (const char *slot : {"arr__w0__idx0", "arr__w0__idx1", "arr__w0__data",
                                     "arr__w0__mask"})
                require(findStoreField(model, CpuNamedStoreKind::LargeSinkPayloadBoundary, slot),
                        "multi-dim layout: writeArray payload boundary slot missing");
        }
        {
            auto model = build("phase_md_layout_reg");
            runSixPhasePipeline(model, false);
            const auto *arrField = findStoreField(model, CpuNamedStoreKind::SmallSink, "arr");
            require(bool(arrField), "multi-dim layout: reg latch field missing");
            const std::array<int64_t, 2> shape{2, 3};
            requireMultiDimArrayPhysical(model, *arrField, shape, 8, "multi-dim layout");
        }
    }

    void multiDimValueFlowTest(const std::filesystem::path &root, bool largeSink)
    {
        GrhSimModel model(largeSink ? "phase_md_value_flow_large" : "phase_md_value_flow_small");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto maskType = model.logicType(48, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> shape{2, 3};
        const auto array = model.logicType(48, false, LogicDomain::TwoState, shape);
        const auto enable = addInputRead(model, "enable", bit);
        const auto input = addInputRead(model, "input", array);
        const auto once = addCompute(model, "core.compute.not", array, "once", {input});
        const auto twice = addCompute(model, "core.compute.not", array, "twice", {once});
        const auto mask = addConstant(model, maskType, "48'hffffffffffff");
        const auto state = addState(model, "state", array, "128'h0");
        addEventWrite(model, "core.state.write", {enable, twice, mask}, state, {});
        addOutputWrite(model, "output", array, addStateRead(model, state, "readback"));
        require(verifies(model), "multi-dimensional value-flow fixture rejected");
        runSixPhasePipeline(model, false, largeSink);

        const auto payloadValue = std::find_if(model.values().begin(), model.values().end(),
                                               [&](const SimValue &value) {
            return value.name && model.text(value.name) == "twice";
        });
        require(payloadValue != model.values().end(), "multi-dimensional payload value was lost");
        const auto *inputField = findStoreField(model, CpuNamedStoreKind::Input, "input");
        const auto *stateField = findStoreField(model, largeSink ? CpuNamedStoreKind::LargeSink
                                                               : CpuNamedStoreKind::SmallSink, "state");
        const auto *payloadField = [&]() -> const CpuStoreField * {
            const auto kind = largeSink ? CpuNamedStoreKind::LargeSinkPayloadBoundary
                                        : CpuNamedStoreKind::SmallSinkPayloadBoundary;
            for (const auto &store : *model.cpuMapping()->dataLayout->namedStores)
                if (store.kind == kind)
                    for (const auto &field : store.fields)
                        if (field.value == payloadValue->id) return &field;
            return nullptr;
        }();
        require(inputField && stateField && payloadField,
                "multi-dimensional value-flow store fields are missing");
        requireMultiDimArrayPhysical(model, *inputField, shape, 8, "multi-dimensional input");
        requireMultiDimArrayPhysical(model, *stateField, shape, 8, "multi-dimensional state");
        requireMultiDimArrayPhysical(model, *payloadField, shape, 8, "multi-dimensional payload boundary");

        const auto directory = root / (largeSink ? "md_value_flow_large" : "md_value_flow_small");
        compileAndRun(model, directory, {
            {{{"enable", "false"}, {"input", "0x060504030201ull"}}, {{"output", "0"}}},
            {{{"enable", "true"}}, {{"output", "0x060504030201ull"}}},
            {{{"input", "0xa6a5a4a3a2a1ull"}}, {{"output", "0xa6a5a4a3a2a1ull"}}},
            {{{"enable", "false"}, {"input", "0x161514131211ull"}},
             {{"output", "0xa6a5a4a3a2a1ull"}}},
            {{{"enable", "true"}}, {{"output", "0x161514131211ull"}}},
        });
        const auto modelDirectory = directory / "model";
        const auto source = concatModelSources(modelDirectory);
        const auto declaration = std::string("std::array<std::array<std::uint8_t,3>,2>");
        std::ifstream header(modelDirectory / ("grhsim_" + std::string(model.text(model.name())) + ".hpp"));
        const std::string headerText{std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>()};
        for (const std::string &field :
             std::array<std::string, 3>{"input", "output", std::string(model.text(payloadField->name))})
            require(headerText.find(declaration + " " + field) != std::string::npos,
                    "multi-dimensional value-flow C++ declaration lost its shape: " + field);
        const auto local = std::find_if(model.values().begin(), model.values().end(), [&](const SimValue &value) {
            return value.name && model.text(value.name) == "once";
        });
        require(local != model.values().end(), "multi-dimensional local value was lost");
        require(source.find(declaration + " cpu_v" + std::to_string(local->id.index)) != std::string::npos,
                "multi-dimensional local C++ declaration lost its shape");
    }

    void multiDimConstInitTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_md_const_init");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const std::array<int64_t, 2> shape{2, 3};
        const auto array = model.logicType(48, false, LogicDomain::TwoState, shape);
        const auto state = addState(model, "arr", array, "48'h060504030201");
        addOutputWrite(model, "o", array, addStateRead(model, state, "contents"));
        require(verifies(model), "multi-dimensional const initializer rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "md_const_init", {
            {{}, {{"o", "0x060504030201ull"}}},
        });
    }

    // A flat state may be viewed as a preserve-aggregate value by the
    // lowering.  Its bits already have the row-major image of the aggregate;
    // the emitter should bridge that view with one byte copy rather than
    // unpacking every row through a wide temporary.
    void flatStateAggregateViewTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_flat_state_aggregate_view");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const std::array<int64_t, 2> shape{2, 3};
        const auto flat = model.logicType(48, false, LogicDomain::TwoState);
        const auto aggregate = model.logicType(48, false, LogicDomain::TwoState, shape);
        const auto state = addState(model, "state", flat, "48'h060504030201");
        const auto view = model.addValue(aggregate, "view");
        model.addOperation("core.state.read", {}, std::array{view}, std::array{ObjectRef::state(state)});
        addOutputWrite(model, "o", aggregate, view);
        require(verifies(model), "flat-state aggregate-view fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "flat_state_aggregate_view", {
            {{}, {{"o", "UINT64_C(0x060504030201)"}}},
        });
        const auto source = concatModelSources(root / "flat_state_aggregate_view" / "model");
        require(source.find("std::memcpy(&cpu_array,&smallSinkStore.state,sizeof(cpu_array));") != std::string::npos,
                "flat-state aggregate view did not use the direct row-image copy");
    }

    // A shaped compute constant is a local value when it feeds another
    // operation. It must be emitted as a static aggregate: rebuilding the
    // packed literal and unpacking every row on every supernode call is a
    // large avoidable cost for preserve-aggregate models.
    void shapedConstantLocalTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_md_const_local");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> shape{4, 4};
        const auto array = model.logicType(64, false, LogicDomain::TwoState, shape);
        const auto enable = addInputRead(model, "enable", bit);
        const auto constant = addConstant(model, array, "64'h0");
        const auto inverted = addCompute(model, "core.compute.not", array, "inverted", {constant});
        const auto mask = addConstant(model, model.logicType(64, false, LogicDomain::TwoState),
                                      "64'hffffffffffffffff");
        const auto state = addState(model, "state", array, "64'h0");
        addEventWrite(model, "core.state.write", {enable, inverted, mask}, state, {});
        addOutputWrite(model, "o", array, addStateRead(model, state, "state_r"));
        require(verifies(model), "shaped local constant fixture rejected");
        runSixPhasePipeline(model, false);
        compileAndRun(model, root / "md_const_local", {
            {{{"enable", "false"}}, {{"o", "0"}}},
            {{{"enable", "true"}}, {{"o", "UINT64_C(0xffffffffffffffff)"}}},
        });
        const auto source = concatModelSources(root / "md_const_local" / "model");
        const auto declaration = "static constexpr std::array<std::array<std::uint8_t,4>,4> cpu_const_array_" +
                                 std::to_string(inverted.index);
        require(source.find(declaration) != std::string::npos,
                "shaped compute constant was not emitted as a constexpr aggregate");
    }

    // (S7) whole-array read/write on a shaped state: core.state.read packs the
    // flat rows into the total-width value; core.state.write with an all-ones
    // mask constant writes the packed vector back row-wise (the mask folds
    // away). Write-then-read equivalence plus per-row cross-checks on both
    // store classes.
    void wholeArrayWriteReadTest(const std::filesystem::path &root, bool allMem)
    {
        GrhSimModel model(allMem ? "phase_md_whole_mem" : "phase_md_whole_reg");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto idx2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto flat48 = model.logicType(48, false, LogicDomain::TwoState);
        const std::array<int64_t, 2> arrShape{2, 3};
        const auto arrType = model.logicType(48, false, LogicDomain::TwoState, arrShape);
        const auto clk = addInputRead(model, "clk", bit);
        const auto wen = addInputRead(model, "wen", bit);
        const auto d48 = addInputRead(model, "d48", arrType);
        const auto ri = addInputRead(model, "ri", bit);
        const auto rj = addInputRead(model, "rj", idx2);
        const auto mask48 = addConstant(model, flat48, "48'hffffffffffff");
        const auto arr = addMemState(model, "arr", arrType, "8'h00");
        addEventWrite(model, "core.state.write", {wen, d48, mask48}, arr, {{clk, "posedge"}});
        addOutputWrite(model, "o48", arrType, addStateRead(model, arr, "arr_r"));
        addOutputWrite(model, "oc", byte, addArrayRead(model, arr, byte, {ri, rj}, "cell_r"));
        require(verifies(model), "whole-array write/read fixture rejected");
        runSixPhasePipeline(model, false, allMem);
        const auto directory = root / (allMem ? "md_whole_mem" : "md_whole_reg");
        compileAndRun(model, directory, {
            {{{"clk", "false"}, {"wen", "false"}, {"d48", "0"}, {"ri", "false"}, {"rj", "0"}},
             {{"o48", "0"}, {"oc", "0"}}},
            // Whole write: arr <= 0x112233445566 (row-major bytes 66 55 44 | 33 22 11).
            {{{"wen", "true"}, {"d48", "UINT64_C(0x112233445566)"}}, {{"o48", "0"}}},
            {{{"clk", "true"}}, {{"o48", "UINT64_C(0x112233445566)"}, {"oc", "0x66"}}},
            {{{"clk", "false"}, {"ri", "true"}, {"rj", "2"}},
             {{"o48", "UINT64_C(0x112233445566)"}, {"oc", "0x11"}}},
            // An edge with wen low changes nothing.
            {{{"wen", "false"}, {"d48", "0"}, {"clk", "true"}}, {{"oc", "0x11"}}},
            {{{"clk", "false"}, {"ri", "false"}, {"rj", "1"}},
             {{"o48", "UINT64_C(0x112233445566)"}, {"oc", "0x55"}}},
        });
        std::ifstream header(directory / "model" /
                             ("grhsim_" + std::string(model.text(model.name())) + ".hpp"));
        const std::string headerText{std::istreambuf_iterator<char>(header),
                                     std::istreambuf_iterator<char>()};
        require(headerText.find("std::array<std::array<std::uint8_t,3>,2> d48{};") != std::string::npos &&
                headerText.find("std::array<std::array<std::uint8_t,3>,2> o48{};") != std::string::npos,
                "multi-dimensional ports were flattened in the emitted ABI");
        // The all-ones mask folded: the whole write is one flat row loop of
        // per-row stores over the 6 rows.
        const auto source = concatModelSources(directory / "model");
        require(source.find("for(std::size_t cpu_row=0;cpu_row<6;++cpu_row)") != std::string::npos,
                "whole-array write should emit the flat row loop");
    }

    void largeConcatTest(const std::filesystem::path &root)
    {
        GrhSimModel model("phase_large_concat");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto six = model.logicType(6, false, LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", model.logicType(65, false, LogicDomain::TwoState));
        const auto b = addInputRead(model, "b", model.logicType(64, true, LogicDomain::TwoState));
        const auto c = addInputRead(model, "c", six);
        const auto hi = addInputRead(model, "hi", bit);
        const auto lo = addInputRead(model, "lo", bit);
        const auto mixedType = model.logicType(166, false, LogicDomain::TwoState);
        addOutputWrite(model, "mixed", mixedType, addCompute(model, "core.compute.concat", mixedType,
            "mixed_concat", {a, b, c, c, c, c, c, c, lo}));
        const std::array<int64_t, 1> mixedArrayShape{8};
        const std::array<int64_t, 1> mixedOperandShape{4};
        const auto mixedArrayOperand = model.logicType(256, false, LogicDomain::TwoState, mixedOperandShape);
        const auto mixedArrayType = model.logicType(512, false, LogicDomain::TwoState, mixedArrayShape);
        const auto arrayInput = addInputRead(model, "array_input", mixedArrayOperand);
        const auto packedInput = addInputRead(model, "packed_input",
                                              model.logicType(256, false, LogicDomain::TwoState));
        const auto mixedArrayValue = addCompute(model, "core.compute.concat", mixedArrayType,
                                                 "mixed_array_concat", {arrayInput, packedInput});
        addOutputWrite(model, "mixed_array", mixedArrayType, mixedArrayValue);
        // Sub-byte mixed concat: the 6-bit scalar contributes the low three
        // 2-bit rows and the four-row aggregate contributes the high rows.
        // This is the shape that used to build a wide temporary only to slice
        // it straight back into the output array.
        const std::array<int64_t, 1> narrowArrayShape{4};
        const auto narrowArrayOperand = model.logicType(8, false, LogicDomain::TwoState, narrowArrayShape);
        const auto narrowArrayType = model.logicType(14, false, LogicDomain::TwoState,
                                                       std::array<int64_t, 1>{7});
        const auto narrowArrayInput = addInputRead(model, "narrow_array_input", narrowArrayOperand);
        const auto narrowPackedInput = addInputRead(model, "narrow_packed_input",
                                                     model.logicType(6, false, LogicDomain::TwoState));
        const auto narrowArrayValue = addCompute(model, "core.compute.concat", narrowArrayType,
                                                  "narrow_array_concat", {narrowArrayInput, narrowPackedInput});
        addOutputWrite(model, "narrow_array", narrowArrayType, narrowArrayValue);
        // The XS failure shape: thousands of narrow pieces form a 16k-bit
        // array. Check every output bit, including updates of both flag inputs.
        const std::array<int64_t, 1> shape{256};
        const auto array = model.logicType(16384, false, LogicDomain::TwoState, shape);
        std::vector<ValueId> pieces;
        for (unsigned i = 0; i < 2048; ++i)
            pieces.insert(pieces.end(), {c, hi, lo});
        const auto joined = model.addValue(array, "joined");
        model.addOperation("core.compute.concat", pieces, std::array{joined});
        addOutputWrite(model, "large", array, joined);
        require(verifies(model), "large concat fixture rejected");
        runSixPhasePipeline(model, false);
        const std::string arrayBits =
            "(((unsigned _BitInt(256))UINT64_C(0x4444444444444444)<<192)|"
            "((unsigned _BitInt(256))UINT64_C(0x3333333333333333)<<128)|"
            "((unsigned _BitInt(256))UINT64_C(0x2222222222222222)<<64)|"
            "UINT64_C(0x1111111111111111))";
        const std::string packedBits =
            "(((unsigned _BitInt(256))UINT64_C(0x8888888888888888)<<192)|"
            "((unsigned _BitInt(256))UINT64_C(0x7777777777777777)<<128)|"
            "((unsigned _BitInt(256))UINT64_C(0x6666666666666666)<<64)|"
            "UINT64_C(0x5555555555555555))";
        const std::string mixedArrayExpected =
            "(((unsigned _BitInt(512))(" + arrayBits + ")<<256)|((unsigned _BitInt(512))(" + packedBits + ")))";
        compileAndRun(model, root / "large_concat", {
            {{{"a", "0"}, {"b", "0"}, {"c", "0"}, {"hi", "false"}, {"lo", "false"},
              {"array_input", "0"}, {"packed_input", "0"}, {"narrow_array_input", "0"},
              {"narrow_packed_input", "0"}},
             {{"mixed", "0"}, {"large", "repeat_byte(0)"}, {"mixed_array", "0"}, {"narrow_array", "0"}}},
            {{{"a", "((unsigned _BitInt(128))1<<64)|UINT64_C(0x123456789abcdef0)"},
              {"b", "UINT64_C(0xfedcba9876543210)"}, {"c", "21"}, {"hi", "true"}, {"lo", "false"}},
             {{"mixed", "((unsigned _BitInt(192))sim.a<<101)|((unsigned _BitInt(192))(std::uint64_t)sim.b<<37)|UINT64_C(0xaaaaaaaaa)"},
              {"large", "repeat_byte(0x56)"}}},
            {{{"lo", "true"}, {"hi", "false"}}, {{"large", "repeat_byte(0x55)"}}},
            {{{"a", "((unsigned _BitInt(128))1<<65)-1"}, {"b", "~UINT64_C(0)"},
              {"c", "63"}, {"hi", "true"}},
             {{"mixed", "((unsigned _BitInt(192))1<<166)-1"}, {"large", "repeat_byte(0xff)"}}},
            {{{"array_input", arrayBits}, {"packed_input", packedBits}}, {{"mixed_array", mixedArrayExpected}}},
            {{{"narrow_array_input", "UINT64_C(0x1d)"}, {"narrow_packed_input", "UINT64_C(0x36)"}},
             {{"narrow_array", "UINT64_C(0x776)"}}},
        }, false,
        "#include <cstring>\n"
        "unsigned _BitInt(16384) repeat_byte(std::uint8_t byte){\n"
        "std::array<std::uint64_t,256> words;words.fill(UINT64_C(0x0101010101010101)*byte);\n"
        "unsigned _BitInt(16384) bits;std::memcpy(&bits,words.data(),sizeof(bits));return bits;}\n");
        const auto source = concatModelSources(root / "large_concat" / "model");
        require(source.find("std::memcpy(cpu_array.data()+0,&cpu_packed_0,32);") != std::string::npos,
                "mixed aggregate/scalar concat did not use the direct row copy");
        require(source.find("const auto cpu_packed_0=") != std::string::npos &&
                source.find("cpu_array[(0)]=static_cast<std::uint8_t>") != std::string::npos,
                "sub-byte mixed concat did not use direct row assignments");
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
        model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
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
        addEventStateWrite(model, one, prev, ones128, q, {{clk, "posedge"}});
        // 12-deep byte register chain: s_{i+1} <= s_i + 1 at the same edge.
        const auto one8 = addConstant(model, byte, "8'h01");
        const auto mask8 = addConstant(model, byte, "8'hff");
        std::vector<StateId> chain;
        for (uint32_t i = 0; i < 12; ++i) chain.push_back(addState(model, "s" + std::to_string(i), byte, "8'h00"));
        for (uint32_t i = 0; i + 1 < chain.size(); ++i)
        {
            const auto read = addStateRead(model, chain[i], "sr" + std::to_string(i));
            const auto next = addCompute(model, "core.compute.add", byte, "sn" + std::to_string(i), {read, one8});
            addEventStateWrite(model, one, next, mask8, chain[i + 1], {{clk, "posedge"}});
        }
        // Small array state (const init) to populate the init/dump streams.
        const std::array<int64_t, 1> tabShape{4};
        const auto tab = addMemState(model, "tab", model.logicType(32, false, LogicDomain::TwoState, tabShape),
                                     "8'h00");
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
        // V3-M2: the oversized add-chain supernode (est > unit cap 64) splits
        // into a wrapper-only chunk plus SupernodePart chunks that tile the
        // part index space across at least two units.
        {
            std::map<uint32_t, std::vector<uint32_t>> partIndices;
            std::map<uint32_t, std::set<std::string>> partUnits;
            std::set<uint32_t> wrappers;
            for (const auto &unit : plan.units)
                for (const auto &chunk : unit.chunks)
                {
                    if (chunk.kind == CpuEmitChunkKind::Supernode) wrappers.insert(chunk.offset);
                    if (chunk.kind == CpuEmitChunkKind::SupernodePart)
                    {
                        partIndices[chunk.offset].push_back(chunk.count);
                        partUnits[chunk.offset].insert(unit.name);
                    }
                }
            require(!partIndices.empty(), "multi-TU: the oversized supernode should split");
            for (const auto &[ordinal, indices] : partIndices)
            {
                require(wrappers.contains(ordinal), "multi-TU: split supernode lost its wrapper chunk");
                require(indices.size() >= 2, "multi-TU: split supernode has too few parts");
                std::vector<uint32_t> sorted = indices;
                std::sort(sorted.begin(), sorted.end());
                for (uint32_t i = 0; i < sorted.size(); ++i)
                    require(sorted[i] == i, "multi-TU: supernode parts do not tile");
                require(partUnits[ordinal].size() >= 2, "multi-TU: supernode parts stayed in one unit");
            }
        }
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
        largeScalarStoreTest(root);
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
        waveformTest(root);
        nbaChainTest(root);
        powerOnTest(root);
        asyncResetTest(root);
        dualClockTest(root);
        glitchClockTest(root);
        glitchClockTest(root, true);
        latchRingTest(root);
        icgGatedSramTest(root);
        sinkEnableGuardTest(root);
        smallSinkEnableBatchTest(root);
        memConvergeTest(root);
        arrayReaderGatingTest(root);
        multiDimReaderGatingTest(root);
        memPriorityTest(root);
        memPriorityTest(root, true);
        crossEventPriorityTest(root, false);
        crossEventPriorityTest(root, true);
        eventFreePriorityConvergeTest(root, false);
        eventFreePriorityConvergeTest(root, true);
        eventFreePriorityConvergeTest(root, false, true);
        eventFreePriorityConvergeTest(root, true, true);
        memEnableBitmapTest(root);
        writeOnWriteTest(root);
        memActClusterDeadCompareTest(root);
        generalDisplayTest(root);
        monitorFreeTest(root);
        monitorEventTest(root);
        monitorEventTest(root, true);
        // dpiSmokeTest (both TU variants): re-enabled in V2-M2 — the new
        // firing rules (non-sink supernodes fire on nonSinkActiveFlag alone, the
        // P_event activation map raises it on the gated call's edge) execute
        // both the event-free and the posedge-gated import correctly.
        dpiSmokeTest(root);
        dpiSmokeTest(root, true);
        dpiBitAbiTest(root);
        unsupportedAggregateEmitTest(root);
        perfCountersTest(root);
        wholeWriteReadbackTest(root);
        packedFillTest(root);
        regLatchArrayWriteTest(root);
        regLatchArrayWriteTest(root, true);
        // Stage-06 multi-dimensional arrays: both store classes for the
        // indexed/partial/priority/whole access patterns, wide elements, the
        // per-element FST registration and the flat physical layout contract.
        multiDimArrayTest(root, false);
        multiDimArrayTest(root, true);
        partialIndexArrayTest(root, false);
        partialIndexArrayTest(root, true);
        multiDimPriorityTest(root, false);
        multiDimPriorityTest(root, true);
        wideElementArrayTest(root, false);
        wideElementArrayTest(root, true);
        waveArrayTest(root);
        multiDimLayoutTest();
        multiDimValueFlowTest(root, false);
        multiDimValueFlowTest(root, true);
        multiDimConstInitTest(root);
        flatStateAggregateViewTest(root);
        shapedConstantLocalTest(root);
        wholeArrayWriteReadTest(root, false);
        wholeArrayWriteReadTest(root, true);
        largeConcatTest(root);
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
