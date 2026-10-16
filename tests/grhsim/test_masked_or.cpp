#include "grhsim/backend/cpu_phase_common.hpp"
#include "grhsim/backend/cpu_phase_emit.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace wolvrix::lib;
    using namespace grhsim;

    void require(bool condition, const std::string &message)
    { if (!condition) throw std::runtime_error(message); }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream stream(path);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    bool verifies(const GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        const bool ok = verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics);
        if (!ok)
            for (const auto &message : diagnostics.messages())
                std::cerr << message.context << ": " << message.message << '\n';
        return ok;
    }

    bool run(GrhSimModel &model, std::string_view name, std::span<const std::string_view> args = {})
    {
        std::string error;
        auto pass = defaultPassRegistry().create(name, args, error);
        require(bool(pass), error);
        PassManager manager(defaultDialectRegistry());
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics;
        const auto result = manager.run(model, diagnostics);
        for (const auto &message : diagnostics.messages())
            std::cout << message.context << ": " << message.message << '\n';
        require(result.success && !model.poisoned(), "pass failed: " + std::string(name));
        return result.changed;
    }

    std::size_t count(const GrhSimModel &model, std::string_view name)
    {
        std::size_t n = 0;
        for (const auto &op : model.operations()) n += model.text(op.opType) == name;
        return n;
    }

    struct Fixture
    {
        enum Pattern : uint32_t { Wrapped = 1, Computed = 2, MultipleMasks = 4, SeparateStates = 8,
                                  Partial = 16, Repeated = 32, Nested = 64, Signed = 128,
                                  Extra = 256, SignBroadcast = 512, DuplicateSelectors = 1024,
                                  NestedMask = 2048, TruncatedArms = 4096 };
        GrhSimModel model{"masked_or"};
        TypeId bit, word, maskType, arrayType, indexType;
        ValueId mask, elements, root, observedPrefix;
        StateId rows;

        Fixture(uint32_t n, uint32_t width, bool stateRows, bool mismatch = false,
                bool replicated = false, bool derivedClock = false, bool reshapedRows = false,
                uint32_t pattern = 0)
        {
            model.addDialect("core", "1", "wolvrix.grhsim.core.v2");
            bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto count = n + ((pattern & Partial) ? 3 : 0);
            const auto dataWidth = width + ((pattern & TruncatedArms) ? 20 : 0);
            const auto dataWord = model.logicType(dataWidth, bool(pattern & Signed), LogicDomain::TwoState);
            word = model.logicType(width, bool(pattern & Signed), LogicDomain::TwoState);
            maskType = model.logicType(count, false, LogicDomain::TwoState);
            arrayType = model.logicType(count * dataWidth, bool(pattern & Signed), LogicDomain::TwoState,
                                       std::array<int64_t, 1>{count});
            indexType = model.logicType(32, false, LogicDomain::TwoState);
            mask = input("mask", maskType);
            const auto mask2 = pattern & MultipleMasks ? input("mask2", maskType) : mask;
            const auto singleton = model.logicType(count, false, LogicDomain::TwoState, std::array<int64_t, 1>{1});
            const auto maskView = pattern & Wrapped ? compute("core.compute.assign", singleton, {mask}) : mask;
            const auto storageType = reshapedRows ? model.logicType(n * width, false, LogicDomain::TwoState,
                std::array<int64_t, 1>{n / 2}) : arrayType;
            elements = input("elements", storageType, stateRows ? storageType : arrayType);
            if (stateRows) rows = state("rows", storageType);
            auto dataArray = stateRows && reshapedRows ? readState(rows, arrayType) : elements;
            if (pattern & Nested)
            {
                const auto doubled = model.logicType(count * width * 2, false, LogicDomain::TwoState,
                                                    std::array<int64_t, 2>{2, count});
                const auto inverted = compute("core.compute.not", arrayType, {dataArray});
                const auto joined = compute("core.compute.concat", doubled, {dataArray, inverted});
                dataArray = compute("core.compute.sliceArray", arrayType, {joined, constant(indexType, 1)});
            }
            const auto zero = constant(dataWord, 0);
            std::vector<ValueId> terms;
            std::vector<std::pair<StateId, ValueId>> separate;
            for (uint32_t i = 0; i < n; ++i)
            {
                const auto selector = model.addValue(bit);
                const auto selectMask = pattern & MultipleMasks && i >= n / 2 ? mask2 : maskView;
                const auto selectIndex = pattern & DuplicateSelectors ? i % (n / 2) : i;
                model.addOperation("core.compute.sliceStatic", std::array{selectMask}, std::array{selector}, {},
                    std::array{Parameter{model.intern("sliceStart"), int64_t(selectIndex)},
                               Parameter{model.intern("sliceEnd"), int64_t(selectIndex)}});
                const auto rowIndex = pattern & Repeated ? i % (n / 2) : mismatch ? (i + 1) % n : i;
                const auto index = constant(indexType, rowIndex);
                auto data = model.addValue(dataWord);
                if (stateRows && !reshapedRows && !(pattern & (Nested | SeparateStates)))
                    model.addOperation("core.state.readArray", std::array{index}, std::array{data},
                                       std::array{ObjectRef::state(rows)});
                else
                    model.addOperation("core.compute.sliceArray", std::array{dataArray, index}, std::array{data});
                if (pattern & SeparateStates)
                {
                    const auto name = "row" + std::to_string(i);
                    const auto cell = state(name.c_str(), word);
                    separate.emplace_back(cell, data);
                    data = readState(cell);
                }
                if (pattern & Computed)
                    data = compute("core.compute.xor", word, {data, constant(word, i + 1)});
                if (width == 1) terms.push_back(compute("core.compute.and", bit, {data, selector}));
                else if (replicated || (pattern & (SignBroadcast | NestedMask)))
                {
                    const auto expanded = model.addValue(word);
                    if (pattern & SignBroadcast)
                    {
                        const auto signedBit = model.logicType(1, true, LogicDomain::TwoState);
                        const auto sign = compute("core.compute.assign", signedBit, {selector});
                        model.addOperation("core.compute.assign", std::array{sign}, std::array{expanded});
                    }
                    else model.addOperation("core.compute.replicate", std::array{selector}, std::array{expanded}, {},
                            std::array{Parameter{model.intern("rep"), int64_t(width)}});
                    terms.push_back(compute("core.compute.and", word, {expanded, data}));
                }
                else terms.push_back(compute("core.compute.mux", word, {selector, data, zero}));
                if (pattern & NestedMask)
                    terms.back() = compute("core.compute.and", word, {terms.back(), constant(word, i * 13 + 7)});
                if (pattern & Wrapped)
                {
                    const auto wrappedType = model.logicType(width, false, LogicDomain::TwoState,
                                                            std::array<int64_t, 1>{1});
                    const auto wrapped = model.addValue(wrappedType);
                    model.addOperation("core.compute.replicate", std::array{terms.back()}, std::array{wrapped}, {},
                        std::array{Parameter{model.intern("rep"), int64_t(1)}});
                    terms.back() = wrapped;
                }
            }
            // Reverse index order: the matcher must use indices, not OR order.
            root = terms.back();
            for (uint32_t i = n - 1; i-- > 0;)
            {
                const auto name = i == 0 ? "selected_value" : "";
                const auto resultType = (pattern & Wrapped) && i == 0 ?
                    model.logicType(width, false, LogicDomain::TwoState, std::array<int64_t, 1>{1}) : word;
                root = compute("core.compute.or", resultType, {root, terms[i]}, name);
                if (i == n / 2) observedPrefix = root;
            }
            if (pattern & Extra) root = compute("core.compute.or", word, {root, input("extra", word)});
            output("selected", word, root);
            if (stateRows)
            {
                const auto clk = input("clk", bit);
                const auto one = constant(bit, 1);
                write(rows, one, elements, clk);
                for (const auto &[cell, data] : separate) write(cell, one, data, clk);
                const auto capture = state("capture", word);
                write(capture, one, root, clk);
                output("captured", word, readState(capture));
            }
            if (derivedClock)
            {
                const auto byte = model.logicType(8, false, LogicDomain::TwoState);
                const auto counter = state("counter", byte);
                const auto next = compute("core.compute.add", byte, {readState(counter), constant(byte, 1)});
                write(counter, constant(bit, 1), next, root);
                output("edge_count", byte, readState(counter));
            }
        }

        ValueId input(const char *name, TypeId type, TypeId view = {})
        {
            const auto id = model.addInput(name, type);
            const auto value = model.addValue(view ? view : type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(id)});
            return value;
        }
        ValueId constant(TypeId type, uint64_t bits)
        {
            const auto value = model.addValue(type);
            model.addOperation("core.compute.constant", {}, std::array{value}, {},
                std::array{Parameter{model.intern("constValue"), std::to_string(model.types()[type.index - 1].width) +
                    "'d" + std::to_string(bits)}});
            return value;
        }
        ValueId compute(const char *name, TypeId type, std::initializer_list<ValueId> operands,
                        std::string_view resultName = {})
        {
            const auto value = model.addValue(type, resultName);
            model.addOperation(name, {operands.begin(), operands.size()}, std::array{value});
            return value;
        }
        StateId state(const char *name, TypeId type)
        {
            const auto id = model.addState(name, type);
            model.addInit(id, std::array{InitStep{model.intern("core.init.const"), {0, 1}}},
                std::array{Parameter{model.intern("value"), std::string("'0")}});
            return id;
        }
        ValueId readState(StateId id, TypeId view = {})
        {
            const auto value = model.addValue(view ? view : model.states()[id.index - 1].type);
            model.addOperation("core.state.read", {}, std::array{value}, std::array{ObjectRef::state(id)});
            return value;
        }
        void write(StateId id, ValueId enable, ValueId data, ValueId event)
        {
            model.addOperation("core.state.write", std::array{enable, data, event}, {},
                std::array{ObjectRef::state(id)},
                std::array{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}});
        }
        void output(const char *name, TypeId type, ValueId value)
        {
            const auto id = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(id)});
        }
        void outputPhase()
        {
            for (const auto &op : model.operations()) model.setOperationPhase(op.id, SimPhase::Output);
        }
    };

    void unitTests(const std::filesystem::path &root)
    {
        for (const auto width : {1u, 8u, 64u})
            for (const bool replicate : {false, true})
            {
                Fixture f(4, width, false, false, replicate);
                f.outputPhase();
                require(run(f.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"}),
                        "valid masked OR was not recovered");
                require(count(f.model, "cpu.compute.maskedOr") == 1, "unexpected maskedOr count");
                require(count(f.model, "core.compute.sliceArray") == 0, "private element reads survived");
                const auto revision = f.model.semanticRevision();
                require(!run(f.model, "cpu.st.recover-masked-or"), "pass is not idempotent");
                require(f.model.semanticRevision() == revision, "no-op pass changed semantic revision");
                run(f.model, "grhsim.used-bits");
                require(verifies(f.model), "used-bits damaged the recovered op");
            }
        Fixture reversed(4, 1, false, true);
        reversed.outputPhase();
        require(run(reversed.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"}),
                "rotated mask/array indices were not recovered");
        Fixture observed(4, 1, false);
        observed.outputPhase();
        const auto symbol = observed.model.intern("prefix");
        observed.model.addDeclaredSymbol(symbol);
        DeclProvenance provenance;
        provenance.symbol = symbol;
        provenance.width = 1;
        provenance.slices.push_back({DeclProvenanceKind::Direct, DeclProvenanceTarget::Value,
                                    observed.observedPrefix.index, 0, 0, 1});
        observed.model.upsertDeclProvenance(std::move(provenance));
        require(run(observed.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"}),
                "observed prefix was not recovered independently");
        require(observed.model.declProvenances().size() == 1 && verifies(observed.model),
                "observed internal prefix disappeared");

        Fixture shared(4, 1, false);
        shared.output("prefix_copy", shared.word, shared.observedPrefix);
        shared.outputPhase();
        require(run(shared.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"}) &&
                count(shared.model, "core.output.write") == 2 && verifies(shared.model),
                "shared OR prefix was not preserved");

        for (const auto pattern : {Fixture::Wrapped, Fixture::Computed, Fixture::MultipleMasks, Fixture::SeparateStates,
                                  Fixture::Partial, Fixture::Repeated, Fixture::Nested, Fixture::Signed,
                                  Fixture::Extra, Fixture::SignBroadcast, Fixture::NestedMask, Fixture::TruncatedArms})
        {
            Fixture f(48, 2, pattern == Fixture::SeparateStates, false, false, false, false, pattern);
            if (pattern == Fixture::SeparateStates)
                for (const auto name : {"grhsim.select-state-stores", "grhsim.classify-event-inputs",
                     "grhsim.lower-edge-detect", "grhsim.extract-output-cones", "grhsim.migrate-timeslot-tasks",
                     "grhsim.split-phases"}) run(f.model, name);
            else f.outputPhase();
            require(run(f.model, "cpu.st.recover-masked-or") && count(f.model, "cpu.compute.maskedOr") >= 1,
                    "audited pattern was not recovered: " + std::to_string(pattern));
            run(f.model, "grhsim.used-bits");
            require(verifies(f.model), "view recovery / used-bits corrupted geometry");
        }
        Fixture longWide(352, 129, false);
        longWide.outputPhase();
        require(run(longWide.model, "cpu.st.recover-masked-or",
                    std::array<std::string_view, 2>{"--min-terms", "65"}), "long/wide reduction was not recovered");

        Fixture random(48, 8, false);
        for (const auto &op : random.model.operations())
            if (random.model.text(op.opType) == "core.compute.sliceArray")
            {
                const auto result = random.model.results(op)[0];
                random.model.replaceOperation(op.id, "core.system.function", {}, std::array{result}, {},
                    std::array{Parameter{random.model.intern("name"), std::string("random")},
                               Parameter{random.model.intern("has_side_effects"), true}});
                break;
            }
        random.outputPhase();
        require(run(random.model, "cpu.st.recover-masked-or") &&
                count(random.model, "core.system.function") == 1, "recovery removed a sampled producer");

        for (uint32_t defect = 0; defect < 4; ++defect)
        {
            Fixture invalid(4, 8, false, true);
            invalid.outputPhase();
            run(invalid.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"});
            for (const auto &op : invalid.model.operations())
                if (invalid.model.text(op.opType) == "cpu.compute.maskedOr")
                {
                    auto views = *maskedOrViews(invalid.model, op);
                    if (defect == 0) views[0] = -1;
                    if (defect == 1) views[1] = 4;
                    if (defect == 2) views[3] = 32;
                    if (defect == 3) views.pop_back();
                    const std::vector<ValueId> inputs(invalid.model.operands(op).begin(), invalid.model.operands(op).end());
                    const auto result = invalid.model.results(op)[0];
                    invalid.model.replaceOperation(op.id, "cpu.compute.maskedOr", inputs, std::array{result}, {},
                        std::array{Parameter{invalid.model.intern("views"), std::move(views)}});
                    break;
                }
            require(!verifies(invalid.model), "malformed view layout passed verification");
        }

        Fixture roundtrip(48, 1, true, true);
        run(roundtrip.model, "grhsim.select-state-stores");
        run(roundtrip.model, "grhsim.classify-event-inputs");
        run(roundtrip.model, "grhsim.lower-edge-detect");
        run(roundtrip.model, "grhsim.extract-output-cones");
        run(roundtrip.model, "grhsim.migrate-timeslot-tasks");
        run(roundtrip.model, "grhsim.split-phases");
        run(roundtrip.model, "cpu.st.recover-masked-or");
        require(count(roundtrip.model, "cpu.compute.maskedOr") == 2, "General/Output recovery missing");
        require(count(roundtrip.model, "core.state.readArray") == 0, "state element reads survived");
        const auto first = root / "first.json", second = root / "second.json";
        diag::Diagnostics diagnostics;
        require(storeGrhSimModel(roundtrip.model, first, defaultDialectRegistry(), diagnostics), "store failed");
        auto loaded = loadGrhSimModel(first, defaultDialectRegistry(), diagnostics);
        require(bool(loaded) && storeGrhSimModel(*loaded, second, defaultDialectRegistry(), diagnostics), "load/store failed");
        require(readFile(first) == readFile(second), "JSON round trip is not stable");

        auto &bad = *loaded;
        for (const auto &op : bad.operations())
            if (bad.text(op.opType) == "cpu.compute.maskedOr")
            {
                const auto operands = std::vector<ValueId>(bad.operands(op).begin(), bad.operands(op).end());
                const auto results = std::vector<ValueId>(bad.results(op).begin(), bad.results(op).end());
                bad.replaceOperation(op.id, "cpu.compute.maskedOr", std::array{operands[1], operands[0]}, results);
                break;
            }
        require(!verifies(bad), "invalid maskedOr operand geometry passed verification");
        std::string error;
        require(!defaultPassRegistry().create("cpu.st.recover-masked-or",
            std::array<std::string_view, 2>{"--min-terms", "1"}, error), "invalid option accepted");
    }

    std::string quote(const std::string &text)
    {
        std::string out = "'";
        for (const char c : text) out += c == '\'' ? "'\\''" : std::string(1, c);
        return out + "'";
    }

    void runtimeTest(const std::filesystem::path &root, uint32_t n, uint32_t width, bool stateRows,
                     bool reshapedRows = false, bool rotated = false, uint32_t pattern = 0)
    {
        Fixture f(n, width, stateRows, rotated, false, !stateRows && width == 1, reshapedRows, pattern);
        for (const auto name : {"grhsim.select-state-stores", "grhsim.classify-event-inputs",
             "grhsim.lower-edge-detect", "grhsim.extract-output-cones", "grhsim.migrate-timeslot-tasks", "grhsim.split-phases"})
            run(f.model, name);
        run(f.model, "cpu.st.recover-masked-or", std::array<std::string_view, 2>{"--min-terms", "2"});
        require(count(f.model, "cpu.compute.maskedOr") >= 1, "runtime case did not exercise maskedOr");
        run(f.model, "grhsim.verify", std::array<std::string_view, 2>{"--seal", "semantic"});
        for (const auto name : {"cpu.st.build-general-nodes", "cpu.st.merge-general-supernodes",
             "cpu.st.clone-shared-boundaries", "cpu.st.layout-named-stores", "cpu.st.build-event-activation-map",
             "cpu.st.build-mem-write-plan"}) run(f.model, name);
        // Force read/consumer across helper chunks and TUs.
        run(f.model, "cpu.st.pack-general-functions",
            std::array<std::string_view, 2>{"--helper-max-estimated-lines", "12"});
        run(f.model, "cpu.st.build-phase-schedule");
        run(f.model, "cpu.st.plan-translation-units",
            std::array<std::string_view, 4>{"--chunk-max-estimated-lines", "12", "--unit-max-estimated-lines", "32"});
        const auto directory = root / ("n" + std::to_string(n) + "w" + std::to_string(width) +
            (stateRows ? "-state" : "-input") + (reshapedRows ? "-view" : "") +
            (rotated ? "-rotated" : "") + "-p" + std::to_string(pattern));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        diag::Diagnostics diagnostics;
        const auto emitted = emitSixPhaseCpuCpp(f.model, directory, diagnostics, false, true);
        if (!emitted.success)
            for (const auto &message : diagnostics.messages()) std::cerr << message.message << '\n';
        require(emitted.success, "CPU emission failed");
        std::string source;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".cpp") source += readFile(entry.path());
        require(source.find("std::countr_zero(cpu_ma_mask)") != std::string::npos, "sparse scan not emitted");
        if (!pattern && !rotated)
        {
            if (reshapedRows)
                require(source.find("const auto cpu_ma_data=") != std::string::npos, "reshaped array did not use packed fallback");
            else
                require(source.find("const auto &cpu_ma_data=") != std::string::npos, "array was not indexed directly");
        }
        if (stateRows && !pattern)
            require(source.find("const std::array<") == std::string::npos, "whole state-array read created a copy");

        std::ofstream driver(directory / "driver.cpp");
        const auto count = n + ((pattern & Fixture::Partial) ? 3 : 0);
        const auto wordContainer = width <= 64 ? 64 : ((width + 63) / 64) * 64;
        const auto maskContainer = count <= 64 ? 64 : ((count + 63) / 64) * 64;
        driver << "#include \"grhsim_masked_or.hpp\"\n#include <array>\n#include <cstdint>\n#include <cstdio>\n#include <type_traits>\n"
               << "int main(){GrhSIM_masked_or sim;sim.init();constexpr unsigned N=" << n << ",W=" << width
               << ",C=" << count << ";\nusing Word="
               << (width <= 64 ? "std::uint64_t" : "unsigned _BitInt(" + std::to_string(wordContainer) + ")")
               << ";using Mask=" << (count <= 64 ? "std::uint64_t" : "unsigned _BitInt(" + std::to_string(maskContainer) + ")")
               << ";const Word wm=~Word(0)>>(" << wordContainer << "-W);"
               << "const Mask mm=~Mask(0)>>(" << maskContainer << "-C);\n"
               << "std::array<Word,C> previous{},values{};std::uint64_t rng=123456789;unsigned edges=0;bool prev=false;\n"
               << "auto random=[&](){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};\n"
               << "auto randomWord=[&](){Word r=0;for(unsigned b=0;b<W;b+=64)r|=Word(random())<<b;return r&wm;};\n"
               << "auto randomMask=[&](){Mask r=0;for(unsigned b=0;b<C;b+=64)r|=Mask(random())<<b;return r&mm;};\n"
               << "auto reference=[&](Mask mask,const auto &a){Word r="
               << (pattern & Fixture::Extra ? "Word(sim.extra)" : "0") << ";for(unsigned i=0;i<N;++i){const auto select="
               << (pattern & Fixture::MultipleMasks ? "(i>=N/2?Mask(sim.mask2):mask)" : "mask")
               << ";if((select>>" << (pattern & Fixture::DuplicateSelectors ? "(i%(N/2))" : "i")
               << ")&1){const unsigned row="
               << (pattern & Fixture::Repeated ? "i%(N/2)" : rotated ? "(i+1)%N" : "i") << ";r|=(Word(a[row])"
               << (pattern & Fixture::Computed ? "^Word(i+1)" : "") << ")"
               << (pattern & Fixture::NestedMask ? "&Word(i*13+7)" : "") << ";}}return r&wm;};\n"
               << "for(unsigned step=0;step<1024;++step){\n"
               << "for(unsigned i=0;i<C;++i)values[i]=randomWord();\n"
               << "if(W==1&&N>64&&(step==4||step==5)){values.fill(0);if(step==4)values[N-1]=1;}\n"
               << "if(N==4&&W==1&&step<256){for(unsigned i=0;i<N;++i)values[i]=((step/16)>>i)&1;}\n";
        if (reshapedRows)
            driver << "using Storage=std::remove_reference_t<decltype(sim.elements[0])>;"
                   << "for(unsigned i=0;i<N/2;++i)sim.elements[i]=Storage(values[2*i])|(Storage(values[2*i+1])<<W);\n";
        else
            driver << "for(unsigned i=0;i<C;++i)sim.elements[i]=values[i];\n";
        if (pattern & Fixture::TruncatedArms)
            driver << "using Storage=std::remove_reference_t<decltype(sim.elements[0])>;"
                   << "for(unsigned i=0;i<C;++i)sim.elements[i]|=Storage(random())<<W;\n";
        driver
               << "sim.mask=(step==0?Mask(0):step==1?mm:step==2?(Mask(1)<<(C-1)):step==3?Mask(1):randomMask());\n"
               << "if(N==4&&W==1&&step<256)sim.mask=step%16;\n";
        if (width == 1 && n > 64)
            driver << "if(step==4)sim.mask=Mask(1)<<(N-1);if(step==5)sim.mask=mm;\n";
        if (pattern & Fixture::MultipleMasks) driver << "sim.mask2=step==0?Mask(0):step==1?mm:randomMask();\n";
        if (pattern & Fixture::Extra) driver << "sim.extra=randomWord();\n";
        if (stateRows)
            driver << "sim.clk=0;sim.eval();auto expected=reference(sim.mask,previous);"
                   << "if((Word(sim.selected)&wm)!=expected)return 10;"
                   << "sim.clk=1;sim.eval();if((Word(sim.captured)&wm)!=expected)return 11;"
                   << "expected=reference(sim.mask,values);if((Word(sim.selected)&wm)!=expected)return 12;"
                   << "previous=values;\n";
        else
        {
            driver << "const auto expected=reference(sim.mask,values);sim.eval();"
                   << "if((Word(sim.selected)&wm)!=expected)return 13;\n";
            if (width == 1)
                driver << "if(expected&&!prev)edges=(edges+1)&255;prev=expected;"
                       << "if(unsigned(sim.edge_count)!=edges)return 14;\n";
        }
        driver << "}std::puts(\"maskedOr runtime PASS\");}\n";
        driver.close();
        const auto command = "make --no-print-directory -C " + quote(directory.string()) + " -f " +
            quote(std::string(WOLVRIX_MASKED_OR_FIXTURE)) + " check -j 2 CXX=" + quote(WOLVRIX_TEST_CXX) +
            " CXXFLAGS='-std=c++20 -O2 -fsanitize=undefined -fno-sanitize-recover=all'";
        require(std::system(command.c_str()) == 0, "generated maskedOr runtime failed");
    }
}

int main()
{
    try
    {
        const auto *override = std::getenv("WOLVRIX_MASKED_OR_TEST_OUTPUT");
        const std::filesystem::path root = override ? override : WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR;
        std::filesystem::create_directories(root);
        unitTests(root);
        runtimeTest(root, 4, 1, false);
        runtimeTest(root, 4, 8, false);
        runtimeTest(root, 48, 1, true);
        runtimeTest(root, 64, 1, false);
        runtimeTest(root, 64, 64, true);
        runtimeTest(root, 4, 8, false, true);
        runtimeTest(root, 4, 8, true, true);
        runtimeTest(root, 4, 64, true, true);
        runtimeTest(root, 48, 2, true, false, false, Fixture::Wrapped);
        runtimeTest(root, 44, 4, false, false, true, Fixture::Nested);
        runtimeTest(root, 48, 8, true, false, false, Fixture::SeparateStates | Fixture::Computed | Fixture::MultipleMasks);
        runtimeTest(root, 48, 8, false, false, false, Fixture::Partial | Fixture::Repeated | Fixture::Extra);
        runtimeTest(root, 48, 8, false, false, false, Fixture::Signed | Fixture::SignBroadcast);
        runtimeTest(root, 352, 1, true);
        runtimeTest(root, 65, 129, false);
        runtimeTest(root, 80, 65, false, false, true, Fixture::Computed | Fixture::MultipleMasks);
        runtimeTest(root, 32, 8, false, false, false, Fixture::DuplicateSelectors);
        runtimeTest(root, 32, 8, false, false, false, Fixture::NestedMask);
        runtimeTest(root, 16, 8, false, false, false, Fixture::TruncatedArms);
        std::cout << "maskedOr tests PASS\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "maskedOr: " << error.what() << '\n';
        return 1;
    }
}
