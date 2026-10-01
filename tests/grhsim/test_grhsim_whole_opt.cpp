// Directed tests for the M5d-3 whole-graph optimization stage (A2-A6):
// grhsim.reg-to-mem declaration-provenance maintenance and declared-family
// shape recovery, the new grhsim.comb-pack lane packing, the reworked
// grhsim.pack-bit-registers (raw event_edges + read/write safety analysis, no
// CPU schedule), and the wired whole-graph pipeline ending in
// grhsim.simplify(scope=whole).

#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace wolvrix::lib;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-whole-opt] " << message << '\n';
        return 1;
    }

    void check(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    using Messages = std::vector<std::pair<std::string, std::string>>;

    Messages runPass(grhsim::GrhSimModel &model, std::string_view name,
                     std::span<const std::string_view> args = {})
    {
        std::string error;
        auto pass = grhsim::defaultPassRegistry().create(name, args, error);
        if (!pass) throw std::runtime_error("pass creation failed: " + error);
        grhsim::PassManager manager(grhsim::defaultDialectRegistry());
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics;
        auto result = manager.run(model, diagnostics);
        Messages messages;
        for (const auto &message : diagnostics.messages())
            messages.emplace_back(message.context, message.message);
        if (!result.success || diagnostics.hasError() || model.poisoned())
            throw std::runtime_error("pass failed: " + std::string(name));
        return messages;
    }

    bool messagesContain(const Messages &messages, std::string_view needle)
    {
        for (const auto &[context, message] : messages)
            if (message.find(needle) != std::string::npos) return true;
        return false;
    }

    bool verifies(grhsim::GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        const bool ok = grhsim::verifyGrhSimModel(model, grhsim::defaultDialectRegistry(), diagnostics) &&
                        !diagnostics.hasError();
        if (!ok)
            for (const auto &message : diagnostics.messages())
                std::cerr << "[verify] " << message.context << ": " << message.message << '\n';
        return ok;
    }

    int roundTrip(grhsim::GrhSimModel &model, const std::filesystem::path &artifactDir,
                  const std::string &stem)
    {
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / (stem + ".json");
        const auto secondPath = artifactDir / (stem + "_roundtrip.json");
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(model, firstPath, grhsim::defaultDialectRegistry(), storeDiagnostics))
            return fail("store failed for " + stem);
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(), loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("load failed for " + stem);
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return fail("round-trip store failed for " + stem);
        if (readFile(firstPath) != readFile(secondPath))
            return fail("store/load/store is not byte stable for " + stem);
        return 0;
    }

    std::size_t countOps(const grhsim::GrhSimModel &model, std::string_view opType)
    {
        std::size_t count = 0;
        for (const auto &op : model.operations())
            if (model.text(op.opType) == opType) ++count;
        return count;
    }

    const grhsim::SimOp *producerOf(const grhsim::GrhSimModel &model, grhsim::ValueId value)
    {
        for (const auto &op : model.operations())
            for (const auto result : model.results(op))
                if (result == value) return &op;
        return nullptr;
    }

    const grhsim::StateObject *findState(const grhsim::GrhSimModel &model, std::string_view name)
    {
        for (const auto &state : model.states())
            if (model.text(state.name) == name) return &state;
        return nullptr;
    }

    const grhsim::DeclProvenance *findProvenance(const grhsim::GrhSimModel &model, std::string_view name)
    {
        return model.findDeclProvenance(model.strings().lookup(name));
    }

    uint32_t valueWidth(const grhsim::GrhSimModel &model, grhsim::ValueId value)
    {
        return model.types()[model.values()[value.index - 1].type.index - 1].width;
    }

    // Minimal two-state interpreter (same semantics as the reg-to-mem
    // semantics suite): reads see the pre-step state, writes publish together
    // after evaluation, event edges compare against the previous step's event
    // value (zero-initialized prev slots).
    class Simulation
    {
    public:
        explicit Simulation(const grhsim::GrhSimModel &model)
            : m(model), defs(m.values().size() + 1), states(m.states().size() + 1),
              values(m.values().size() + 1), ready(values.size())
        {
            for (const auto &op : m.operations())
                for (const auto result : m.results(op)) defs[result.index] = op.id;
            for (const auto &state : m.states())
            {
                const auto &type = m.types()[state.type.index - 1];
                states[state.id.index].resize(type.kind == grhsim::TypeKind::Array ? type.count : 1);
            }
            for (const auto &record : m.initRecords())
                for (const auto &step : m.steps(record))
                {
                    uint64_t value = 0, start = 0, count = 1;
                    for (const auto &p : m.parameters(step))
                    {
                        if (m.text(p.name) == "value") value = literal(std::get<std::string>(p.value));
                        if (m.text(p.name) == "start") start = std::get<int64_t>(p.value);
                        if (m.text(p.name) == "count") count = std::get<int64_t>(p.value);
                    }
                    check(m.text(step.kind) == "core.init.const" || m.text(step.kind) == "core.init.fill",
                          "unsupported init");
                    for (uint64_t i = start; i < start + count; ++i)
                        states.at(record.state.index).at(i) = value;
                }
        }

        void step(const std::vector<uint64_t> &inputs)
        {
            inputValues = inputs;
            std::fill(ready.begin(), ready.end(), false);
            auto pending = states;
            std::unordered_map<uint32_t, uint64_t> nextEvents;
            for (const auto &op : m.operations())
            {
                const auto kind = m.text(op.opType);
                if (kind != "core.state.regWrite" && kind != "core.state.memWrite" &&
                    kind != "core.state.memWriteSeq" && kind != "core.state.memFill")
                    continue;
                const auto args = m.operands(op);
                const auto objects = m.objectRefs(op);
                const std::vector<std::string> *edges = nullptr;
                for (const auto &p : m.parameters(op))
                    if (m.text(p.name) == "event_edges") edges = std::get_if<std::vector<std::string>>(&p.value);
                check(edges && !edges->empty(), "test expects an explicit event list");
                const auto eventCount = edges->size();
                check(args.size() >= eventCount, "invalid event list");
                bool edge = false;
                for (std::size_t i = 0; i < eventCount; ++i)
                {
                    const auto eventValue = args[args.size() - eventCount + i];
                    const auto event = eval(eventValue);
                    const auto it = prevEvents.find(eventValue.index);
                    const auto previous = it == prevEvents.end() ? 0 : it->second;
                    check((*edges)[i] == "posedge" || (*edges)[i] == "negedge", "unsupported test edge");
                    edge |= (*edges)[i] == "posedge" ? (!previous && event) : (previous && !event);
                    nextEvents[eventValue.index] = event;
                }
                if (!edge) continue;
                auto &target = pending.at(objects[0].index);
                if (kind == "core.state.regWrite")
                {
                    if (eval(args[0]))
                    {
                        const auto mask = eval(args[2]);
                        target[0] = (target[0] & ~mask) | (eval(args[1]) & mask);
                    }
                }
                else if (kind == "core.state.memWrite")
                {
                    if (eval(args[0]))
                    {
                        auto &cell = target.at(eval(args[1]));
                        const auto mask = eval(args[3]);
                        cell = (cell & ~mask) | (eval(args[2]) & mask);
                    }
                }
                else if (kind == "core.state.memFill")
                {
                    if (eval(args[0])) std::fill(target.begin(), target.end(), eval(args[1]));
                }
                else
                {
                    for (std::size_t i = 0; i + 2 < args.size() - eventCount; i += 3)
                        if (eval(args[i])) target.at(eval(args[i + 1])) = eval(args[i + 2]);
                }
            }
            states.swap(pending);
            for (const auto &[value, event] : nextEvents) prevEvents[value] = event;
            std::fill(ready.begin(), ready.end(), false);
        }

        std::vector<uint64_t> outputs()
        {
            std::vector<uint64_t> result(m.outputs().size());
            for (const auto &op : m.operations())
                if (m.text(op.opType) == "core.output.write")
                    result.at(m.objectRefs(op)[0].index - 1) = eval(m.operands(op)[0]);
            return result;
        }

    private:
        static uint64_t literal(const std::string &text)
        {
            auto value = slang::SVInt::fromString(text);
            check(!value.hasUnknown() && value.getBitWidth() <= 64, "unsupported test literal");
            return value.getRawPtr()[0];
        }
        uint64_t eval(grhsim::ValueId value)
        {
            if (ready.at(value.index)) return values[value.index];
            const auto &op = m.operations().at(defs.at(value.index).index - 1);
            const auto kind = m.text(op.opType);
            const auto args = m.operands(op);
            auto a = [&](std::size_t i) { return eval(args[i]); };
            uint64_t result = 0;
            if (kind == "core.input.read") result = inputValues.at(m.objectRefs(op)[0].index - 1);
            else if (kind == "core.state.read") result = states.at(m.objectRefs(op)[0].index)[0];
            else if (kind == "core.state.memRead") result = states.at(m.objectRefs(op)[0].index).at(a(0));
            else if (kind == "core.compute.constant")
            {
                for (const auto &p : m.parameters(op))
                    if (m.text(p.name) == "value" || m.text(p.name) == "constValue")
                        result = literal(std::get<std::string>(p.value));
            }
            else if (kind == "core.compute.assign") result = a(0);
            else if (kind == "core.compute.eq") result = a(0) == a(1);
            else if (kind == "core.compute.lt") result = a(0) < a(1);
            else if (kind == "core.compute.sub") result = a(0) - a(1);
            else if (kind == "core.compute.add") result = a(0) + a(1);
            else if (kind == "core.compute.xor") result = a(0) ^ a(1);
            else if (kind == "core.compute.xnor") result = ~(a(0) ^ a(1));
            else if (kind == "core.compute.and") result = a(0) & a(1);
            else if (kind == "core.compute.or") result = a(0) | a(1);
            else if (kind == "core.compute.not") result = ~a(0);
            else if (kind == "core.compute.lshr") result = a(1) >= 64 ? 0 : a(0) >> a(1);
            else if (kind == "core.compute.logicAnd") result = a(0) && a(1);
            else if (kind == "core.compute.logicOr") result = a(0) || a(1);
            else if (kind == "core.compute.logicNot") result = !a(0);
            else if (kind == "core.compute.mux") result = a(0) ? a(1) : a(2);
            else if (kind == "core.compute.replicate")
            {
                int64_t rep = 0;
                for (const auto &p : m.parameters(op))
                    if (m.text(p.name) == "rep") rep = std::get<int64_t>(p.value);
                const auto w = m.types()[m.values()[args[0].index - 1].type.index - 1].width;
                const auto v = a(0) & (w >= 64 ? ~uint64_t{0} : ((uint64_t{1} << w) - 1));
                for (int64_t i = 0; i < rep; ++i) result = (result << w) | v;
            }
            else if (kind == "core.compute.concat")
            {
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    const auto width = m.types()[m.values()[args[i].index - 1].type.index - 1].width;
                    result = (result << width) | a(i);
                }
            }
            else if (kind == "core.compute.sliceStatic")
            {
                uint64_t start = 0;
                for (const auto &p : m.parameters(op))
                    if (m.text(p.name) == "sliceStart") start = std::get<int64_t>(p.value);
                result = start >= 64 ? 0 : a(0) >> start;
            }
            else
                throw std::runtime_error("unsupported test op: " + std::string(kind));
            const auto width = m.types()[m.values()[value.index - 1].type.index - 1].width;
            check(width <= 64, "test evaluator only supports narrow values");
            values[value.index] = width == 64 ? result : result & ((uint64_t{1} << width) - 1);
            ready[value.index] = true;
            return values[value.index];
        }
        const grhsim::GrhSimModel &m;
        std::vector<grhsim::OpId> defs;
        std::vector<std::vector<uint64_t>> states;
        std::vector<uint64_t> values, inputValues;
        std::vector<bool> ready;
        std::unordered_map<uint32_t, uint64_t> prevEvents;
    };

    struct ModelBuilder
    {
        grhsim::GrhSimModel model{"whole-opt-test"};
        grhsim::TypeId bit, byte;

        ModelBuilder()
        {
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
        }
        grhsim::ValueId input(std::string_view name, grhsim::TypeId type)
        {
            const auto id = model.addInput(name, type);
            const auto value = model.addValue(type, name);
            model.addOperation("core.input.read", {}, std::array{value},
                               std::array{grhsim::ObjectRef::input(id)}, {}, name);
            return value;
        }
        grhsim::ValueId constant(grhsim::TypeId type, uint64_t n)
        {
            const auto value = model.addValue(type);
            model.addOperation("core.compute.constant", {}, std::array{value}, {},
                               std::array{grhsim::Parameter{model.intern("value"),
                               std::to_string(model.types()[type.index - 1].width) + "'d" + std::to_string(n)}});
            return value;
        }
        grhsim::ValueId compute(std::string_view kind, grhsim::TypeId type, std::vector<grhsim::ValueId> args,
                                std::string_view name = {})
        {
            const auto value = model.addValue(type, name);
            model.addOperation(kind, args, std::array{value}, {}, {}, name);
            return value;
        }
        void output(std::string_view name, grhsim::TypeId type, grhsim::ValueId value)
        {
            const auto id = model.addOutput(name, type);
            model.addOperation("core.output.write", std::array{value}, {},
                               std::array{grhsim::ObjectRef::output(id)}, {}, name);
        }
        grhsim::StateId state(std::string_view name, grhsim::TypeId type, uint64_t init, bool declared = true)
        {
            const auto id = model.addState(name, type);
            const std::array steps{grhsim::InitStep{model.intern("core.init.const"), {0, 1}}};
            const std::array params{grhsim::Parameter{model.intern("value"),
                std::to_string(model.types()[type.index - 1].width) + "'d" + std::to_string(init)}};
            model.addInit(id, steps, params);
            if (declared)
            {
                const auto symbol = model.intern(name);
                model.addDeclaredSymbol(symbol);
                grhsim::DeclProvenance record;
                record.symbol = symbol;
                record.width = model.types()[type.index - 1].width;
                record.slices.push_back({grhsim::DeclProvenanceKind::Direct,
                                         grhsim::DeclProvenanceTarget::State, id.index, 0, 0, record.width});
                model.upsertDeclProvenance(std::move(record));
            }
            return id;
        }
        void declareValue(std::string_view name, grhsim::ValueId value)
        {
            const auto symbol = model.intern(name);
            model.addDeclaredSymbol(symbol);
            grhsim::DeclProvenance record;
            record.symbol = symbol;
            record.width = valueWidth(model, value);
            record.slices.push_back({grhsim::DeclProvenanceKind::Direct,
                                     grhsim::DeclProvenanceTarget::Value, value.index, 0, 0, record.width});
            model.upsertDeclProvenance(std::move(record));
        }
        grhsim::ValueId read(grhsim::StateId state, grhsim::TypeId type)
        {
            const auto value = model.addValue(type);
            model.addOperation("core.state.read", {}, std::array{value},
                               std::array{grhsim::ObjectRef::state(state)});
            return value;
        }
        void regWrite(grhsim::StateId state, grhsim::ValueId enable, grhsim::ValueId data,
                      grhsim::ValueId mask, grhsim::ValueId event)
        {
            const std::vector<std::string> edges{"posedge"};
            model.addOperation("core.state.regWrite", std::array{enable, data, mask, event}, {},
                               std::array{grhsim::ObjectRef::state(state)},
                               std::array{grhsim::Parameter{model.intern("event_edges"), edges}});
        }
    };

    // ---- A5: pack-bit-registers fixture -----------------------------------
    // Four 1-bit flags sharing enable/mask/event (packable), plus one
    // four-state bit, one multi-writer bit, one never-read bit and one
    // singleton (own enable) that must all stay unpacked with explicit
    // rejection counters.
    grhsim::GrhSimModel makePackBitsModel()
    {
        ModelBuilder b;
        const auto clk = b.input("clk", b.bit);
        const auto en = b.input("en", b.bit);
        const auto en2 = b.input("en2", b.bit);
        const auto one = b.constant(b.bit, 1);
        std::vector<grhsim::ValueId> data;
        for (unsigned i = 0; i < 4; ++i) data.push_back(b.input("d" + std::to_string(i), b.bit));
        std::vector<grhsim::ValueId> reads;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto state = b.state("flag_" + std::to_string(i), b.bit, i % 2);
            b.regWrite(state, en, data[i], one, clk);
            reads.push_back(b.read(state, b.bit));
        }
        // four-state candidate: rejected_target_type
        const auto x4 = b.model.addState("x4flag", b.model.logicType(1, false, grhsim::LogicDomain::FourState));
        const std::array initStep{grhsim::InitStep{b.model.intern("core.init.const"), {0, 1}}};
        b.model.addInit(x4, initStep, std::array{grhsim::Parameter{b.model.intern("value"), std::string("1'd0")}});
        b.regWrite(x4, en, data[0], one, clk);
        reads.push_back(b.read(x4, b.model.logicType(1, false, grhsim::LogicDomain::FourState)));
        // multi-writer candidate: rejected_multi_writer
        const auto multi = b.state("multi", b.bit, 0);
        b.regWrite(multi, en, data[1], one, clk);
        b.regWrite(multi, en2, data[2], one, clk);
        reads.push_back(b.read(multi, b.bit));
        // never-read candidate: rejected_no_reads
        b.regWrite(b.state("dead", b.bit, 0), en, data[3], one, clk);
        // singleton (own enable): pack_bits_singleton
        const auto solo = b.state("solo", b.bit, 1);
        b.regWrite(solo, en2, data[0], one, clk);
        reads.push_back(b.read(solo, b.bit));
        auto acc = reads[0];
        for (std::size_t i = 1; i < reads.size(); ++i)
            acc = b.compute("core.compute.xor", b.bit, {acc, reads[i]});
        b.output("o", b.bit, acc);
        return std::move(b.model);
    }

    int runPackBitRegistersTest(const std::filesystem::path &artifactDir)
    {
        auto original = makePackBitsModel();
        Simulation reference(original);
        auto model = makePackBitsModel();
        check(!model.cpuMapping(), "pack fixture should not carry a CPU mapping");
        const Messages messages = runPass(model, "grhsim.pack-bit-registers");
        if (!messagesContain(messages, "packed_register_bits=4") ||
            !messagesContain(messages, "packed_register_words=1"))
            return fail("pack-bit-registers did not pack the four flags");
        // Rejections count write ports: the multi-written state rejects both
        // of its writes, hence multi_writer=2.
        for (const std::string_view needle : {"pack_bits_rejected_target_type=1",
                                              "pack_bits_rejected_multi_writer=2",
                                              "pack_bits_rejected_no_reads=1", "pack_bits_singleton=1"})
            if (!messagesContain(messages, needle))
            {
                for (const auto &[context, message] : messages)
                    std::cerr << "[dbg] " << context << ": " << message << '\n';
                return fail("missing rejection counter: " + std::string(needle));
            }
        if (findState(model, "flag_0") || findState(model, "flag_3"))
            return fail("packed member states survived");
        const grhsim::StateObject *packed = nullptr;
        for (const auto &state : model.states())
            if (model.text(state.name).find("packed_bits_") == 0) packed = &state;
        if (!packed) return fail("packed word state missing");
        if (!findState(model, "solo") || !findState(model, "multi") || !findState(model, "dead") ||
            !findState(model, "x4flag"))
            return fail("rejected states should stay unpacked");
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto *record = findProvenance(model, "flag_" + std::to_string(i));
            if (!record || record->slices.size() != 1)
                return fail("packed member lost its provenance record");
            const auto &slice = record->slices[0];
            if (slice.kind != grhsim::DeclProvenanceKind::Merged ||
                slice.target != grhsim::DeclProvenanceTarget::State ||
                slice.targetIndex != packed->id.index || slice.targetOffset != i || slice.width != 1)
                return fail("packed member provenance slice is wrong");
        }
        // The packed word init assembles lane bits (lane 0 = LSB): 0,1,0,1.
        bool initOk = false;
        for (const auto &record : model.initRecords())
        {
            if (record.state != packed->id) continue;
            const auto steps = model.steps(record);
            if (steps.size() != 1) continue;
            const auto params = model.parameters(steps[0]);
            if (params.size() == 1 && std::get<std::string>(params[0].value) == "4'd10") initOk = true;
        }
        if (!initOk) return fail("packed word init is not 4'b1010");
        // The packed write keeps the shared enable/mask/event and writes the
        // concatenated data through a replicated mask.
        bool writeOk = false;
        for (const auto &op : model.operations())
        {
            const auto refs = model.objectRefs(op);
            if (model.text(op.opType) != "core.state.regWrite" || refs.size() != 1 ||
                refs[0].index != packed->id.index)
                continue;
            const auto operands = model.operands(op);
            const auto params = model.parameters(op);
            if (operands.size() != 4 || params.size() != 1) continue;
            const auto *edges = std::get_if<std::vector<std::string>>(&params[0].value);
            if (!edges || *edges != std::vector<std::string>{"posedge"}) continue;
            const grhsim::SimOp *data = producerOf(model, operands[1]);
            const grhsim::SimOp *mask = producerOf(model, operands[2]);
            if (data && mask && model.text(data->opType) == "core.compute.concat" &&
                model.results(*data).size() == 1 && valueWidth(model, model.results(*data)[0]) == 4 &&
                model.text(mask->opType) == "core.compute.replicate")
                writeOk = true;
        }
        if (!writeOk) return fail("packed regWrite shape is wrong");
        if (!verifies(model)) return fail("packed model rejected by the verifier");
        Simulation rewritten(model);
        std::mt19937_64 rng(20261001);
        for (unsigned step = 0; step < 512; ++step)
        {
            // inputs: clk en en2 d0..d3; hold clk low for the first eval, then toggle.
            std::vector<uint64_t> inputs{uint64_t{step % 2}, rng() & 1, rng() & 1,
                                         rng() & 1, rng() & 1, rng() & 1, rng() & 1};
            reference.step(inputs);
            rewritten.step(inputs);
            if (reference.outputs() != rewritten.outputs())
                return fail("pack-bit-registers differential mismatch");
        }
        return roundTrip(model, artifactDir, "grhsim_pack_bits");
    }

    // ---- A4: comb-pack fixtures -------------------------------------------
    // mode 0: lane_i = and(or(a_i, b_i), c)   (bitwise tree)
    // mode 1: lane_i = mux(sel_i, a_i, b_i)   (masked-select rewrite)
    // mode 2: lane_1 reads lane_0's root through a slice (cross-root reject)
    // mode 3: four-state mux lanes (mux not packable, no candidates)
    grhsim::GrhSimModel makeCombPackModel(unsigned mode)
    {
        ModelBuilder b;
        const auto domain = mode == 3 ? grhsim::LogicDomain::FourState : grhsim::LogicDomain::TwoState;
        const auto laneType = b.model.logicType(8, false, domain);
        const auto selType = b.model.logicType(1, false, domain);
        std::vector<grhsim::ValueId> lanes;
        std::optional<grhsim::ValueId> lane0;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto a = b.input("a" + std::to_string(i), laneType);
            const auto bb = b.input("b" + std::to_string(i), laneType);
            grhsim::ValueId lane;
            if (mode == 1 || mode == 3)
            {
                const auto sel = b.input("s" + std::to_string(i), selType);
                lane = b.compute("core.compute.mux", laneType, {sel, a, bb});
            }
            else
            {
                grhsim::ValueId left = a;
                if (mode == 2 && i == 1 && lane0)
                {
                    lane = *lane0;
                    const std::array params{grhsim::Parameter{b.model.intern("sliceStart"), int64_t{0}},
                                            grhsim::Parameter{b.model.intern("sliceEnd"), int64_t{7}}};
                    left = b.model.addValue(laneType);
                    b.model.addOperation("core.compute.sliceStatic", std::array{lane}, std::array{left}, {},
                                         params);
                }
                const auto c = b.input("c" + std::to_string(i), laneType);
                lane = b.compute("core.compute.and", laneType,
                                 {b.compute("core.compute.or", laneType, {left, bb}), c});
            }
            if (i == 0) lane0 = lane;
            const auto name = "lane_" + std::to_string(i);
            b.declareValue(name, lane);
            lanes.push_back(lane);
            b.output("o" + std::to_string(i), laneType, lane);
        }
        return std::move(b.model);
    }

    int runCombPackBitwiseTest(const std::filesystem::path &artifactDir)
    {
        auto original = makeCombPackModel(0);
        Simulation reference(original);
        auto model = makeCombPackModel(0);
        const auto reportPath = artifactDir / "comb_pack_bitwise.tsv";
        const std::string reportArg = reportPath.string();
        const std::array<std::string_view, 2> args{"--report", reportArg};
        const Messages messages = runPass(model, "grhsim.comb-pack", args);
        if (!messagesContain(messages, "comb_pack_groups=1") ||
            !messagesContain(messages, "comb_pack_lanes=4"))
            return fail("comb-pack did not pack the four bitwise lanes");
        if (countOps(model, "core.compute.or") != 1 || countOps(model, "core.compute.and") != 1)
            return fail("narrow lane ops were not replaced by one wide pair");
        if (countOps(model, "core.compute.sliceStatic") != 4)
            return fail("lane slices missing");
        grhsim::ValueId packed;
        for (const auto &op : model.operations())
            if (model.text(op.opType) == "core.compute.and" &&
                valueWidth(model, model.results(op)[0]) == 32)
                packed = model.results(op)[0];
        if (!packed.valid()) return fail("packed 32-bit and result missing");
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto *record = findProvenance(model, "lane_" + std::to_string(i));
            if (!record || record->slices.size() != 1)
                return fail("packed lane lost its provenance record");
            const auto &slice = record->slices[0];
            if (slice.kind != grhsim::DeclProvenanceKind::Merged ||
                slice.target != grhsim::DeclProvenanceTarget::Value || slice.targetIndex != packed.index ||
                slice.targetOffset != i * 8 || slice.width != 8)
                return fail("packed lane provenance slice is wrong");
        }
        if (readFile(reportPath).find("lane_@") == std::string::npos)
            return fail("comb-pack report does not carry the declared family pattern");
        if (!verifies(model)) return fail("packed comb model rejected by the verifier");
        Simulation rewritten(model);
        std::mt19937_64 rng(417);
        for (unsigned step = 0; step < 256; ++step)
        {
            std::vector<uint64_t> inputs(12);
            for (auto &v : inputs) v = rng();
            reference.step(inputs);
            rewritten.step(inputs);
            if (reference.outputs() != rewritten.outputs())
                return fail("comb-pack bitwise differential mismatch");
        }
        return roundTrip(model, artifactDir, "grhsim_comb_pack_bitwise");
    }

    int runCombPackMuxTest(const std::filesystem::path &artifactDir)
    {
        auto original = makeCombPackModel(1);
        Simulation reference(original);
        auto model = makeCombPackModel(1);
        const Messages messages = runPass(model, "grhsim.comb-pack");
        if (!messagesContain(messages, "comb_pack_groups=1"))
            return fail("comb-pack did not pack the mux lanes");
        if (countOps(model, "core.compute.mux") != 0)
            return fail("mux lanes survived the masked-select rewrite");
        if (countOps(model, "core.compute.replicate") != 4)
            return fail("masked-select masks missing");
        if (!verifies(model)) return fail("mux-packed model rejected by the verifier");
        Simulation rewritten(model);
        std::mt19937_64 rng(918);
        for (unsigned step = 0; step < 256; ++step)
        {
            std::vector<uint64_t> inputs(12);
            for (auto &v : inputs) v = rng();
            reference.step(inputs);
            rewritten.step(inputs);
            if (reference.outputs() != rewritten.outputs())
                return fail("comb-pack mux differential mismatch");
        }
        return roundTrip(model, artifactDir, "grhsim_comb_pack_mux");
    }

    int runCombPackRejectTest()
    {
        // Cross-root dependency: lane_1's tree reads lane_0's root, so the
        // full group is rejected; lanes 1-3 still pack (lane_0 is outside).
        {
            auto original = makeCombPackModel(2);
            Simulation reference(original);
            auto model = makeCombPackModel(2);
            const std::array<std::string_view, 4> args{"--min-group-size", "2", "--min-packed-width", "16"};
            const Messages messages = runPass(model, "grhsim.comb-pack", args);
            if (!messagesContain(messages, "comb_pack_rejected_cross_root="))
                return fail("cross-root rejection counter missing");
            if (messagesContain(messages, "comb_pack_rejected_cross_root=0"))
            {
                for (const auto &[context, message] : messages)
                    std::cerr << "[dbg] " << context << ": " << message << '\n';
                return fail("cross-root dependency was not rejected");
            }
            if (!messagesContain(messages, "comb_pack_groups=1") ||
                !messagesContain(messages, "comb_pack_lanes=3"))
                return fail("lanes 1-3 should still pack without lane_0");
            const auto *record = findProvenance(model, "lane_0");
            if (!record || record->slices.size() != 1 ||
                record->slices[0].kind != grhsim::DeclProvenanceKind::Direct)
                return fail("unpacked lane_0 provenance should stay Direct");
            if (!verifies(model)) return fail("cross-root model rejected by the verifier");
            Simulation rewritten(model);
            std::mt19937_64 rng(31337);
            for (unsigned step = 0; step < 128; ++step)
            {
                std::vector<uint64_t> inputs(12);
                for (auto &v : inputs) v = rng();
                reference.step(inputs);
                rewritten.step(inputs);
                if (reference.outputs() != rewritten.outputs())
                    return fail("cross-root differential mismatch");
            }
        }
        // Four-state mux lanes: mux is not a packable internal op under
        // four-state semantics, so nothing becomes a candidate.
        {
            auto model = makeCombPackModel(3);
            const Messages messages = runPass(model, "grhsim.comb-pack");
            if (!messagesContain(messages, "comb_pack_groups=0"))
                return fail("four-state mux lanes must not pack");
            if (countOps(model, "core.compute.mux") != 4)
                return fail("four-state mux lanes were rewritten");
            if (!verifies(model)) return fail("four-state model rejected by the verifier");
        }
        // Argument validation.
        {
            std::string error;
            if (grhsim::defaultPassRegistry().create("grhsim.comb-pack",
                                                     std::array{std::string_view("--min-group-size")},
                                                     error))
                return fail("comb-pack accepted a dangling option");
            if (grhsim::defaultPassRegistry().create(
                    "grhsim.comb-pack",
                    std::array{std::string_view("--min-group-size"), std::string_view("8"),
                               std::string_view("--max-group-size"), std::string_view("4")},
                    error))
                return fail("comb-pack accepted max < min group size");
            if (grhsim::defaultPassRegistry().create(
                    "grhsim.comb-pack",
                    std::array{std::string_view("--enable-declared-roots"), std::string_view("false"),
                               std::string_view("--enable-output-roots"), std::string_view("false"),
                               std::string_view("--enable-storage-data-roots"), std::string_view("false")},
                    error))
                return fail("comb-pack accepted all root sources disabled");
        }
        return 0;
    }

    // ---- A3: reg-to-mem declared-provenance and shape fixture -------------
    // Two decoded write families with declared rows: tbl_0..3 (1-D) and
    // mat_0_0..mat_1_1 (2-D, row-major decode order).
    grhsim::GrhSimModel makeRegToMemDeclaredModel()
    {
        ModelBuilder b;
        const auto addrType = b.model.logicType(3, false, grhsim::LogicDomain::TwoState);
        const auto clk = b.input("clk", b.bit);
        const auto u = b.input("u", b.bit);
        const auto en = b.input("en", b.bit);
        const auto addr = b.input("addr", addrType);
        const auto d = b.input("d", b.byte);
        const auto addr2 = b.input("addr2", addrType);
        const auto d2 = b.input("d2", b.byte);
        const auto mask = b.constant(b.byte, 0xff);
        const std::vector<std::string> edges{"posedge"};
        auto family = [&](std::string_view prefix, bool twoDim, grhsim::ValueId familyAddr,
                          grhsim::ValueId familyData, std::string_view outputPrefix) {
            for (unsigned row = 0; row < 4; ++row)
            {
                const std::string name = twoDim
                    ? std::string(prefix) + "_" + char('0' + row / 2) + "_" + char('0' + row % 2)
                    : std::string(prefix) + "_" + char('0' + row);
                const auto state = b.state(name, b.byte, row);
                const auto old = b.read(state, b.byte);
                const auto hit = b.compute("core.compute.eq", b.bit,
                                           {familyAddr, b.constant(addrType, row)});
                const auto low = b.compute("core.compute.logicAnd", b.bit, {en, hit});
                const auto next = b.compute("core.compute.mux", b.byte, {low, familyData, old});
                b.model.addOperation("core.state.regWrite", std::array{u, next, mask, clk}, {},
                                     std::array{grhsim::ObjectRef::state(state)},
                                     std::array{grhsim::Parameter{b.model.intern("event_edges"), edges}});
                b.output(std::string(outputPrefix) + "_" + char('0' + row), b.byte, old);
            }
        };
        family("tbl", false, addr, d, "qt");
        family("mat", true, addr2, d2, "qm");
        return std::move(b.model);
    }

    int runRegToMemDeclaredTest(const std::filesystem::path &artifactDir)
    {
        auto original = makeRegToMemDeclaredModel();
        Simulation reference(original);
        auto model = makeRegToMemDeclaredModel();
        const auto reportPath = artifactDir / "reg_to_mem_declared.tsv";
        const std::string reportArg = reportPath.string();
        const std::array<std::string_view, 4> args{"--enable-cost-selection", "false", "--report",
                                                   reportArg};
        const Messages messages = runPass(model, "grhsim.reg-to-mem", args);
        if (!messagesContain(messages, "transformed=2") ||
            !messagesContain(messages, "provenance_records=8") ||
            !messagesContain(messages, "declared_families=2") ||
            !messagesContain(messages, "max_array_dims=2"))
            return fail("reg-to-mem provenance/shape counters wrong");
        const grhsim::StateObject *tbl = nullptr, *mat = nullptr;
        for (const auto &state : model.states())
        {
            const std::string_view name = model.text(state.name);
            if (name.find("__reg_to_mem_tbl_@__") == 0) tbl = &state;
            if (name.find("__reg_to_mem_mat_@_@__") == 0) mat = &state;
        }
        if (!tbl || !mat) return fail("declared-pattern table names missing");
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto *record = findProvenance(model, "tbl_" + std::to_string(i));
            if (!record || record->slices.size() != 1)
                return fail("merged row lost its provenance record");
            const auto &slice = record->slices[0];
            if (slice.kind != grhsim::DeclProvenanceKind::Merged ||
                slice.target != grhsim::DeclProvenanceTarget::State ||
                slice.targetIndex != tbl->id.index || slice.targetOffset != i * 8 || slice.width != 8)
                return fail("tbl row provenance slice is wrong");
        }
        const std::array<std::string, 4> matNames{"mat_0_0", "mat_0_1", "mat_1_0", "mat_1_1"};
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto *record = findProvenance(model, matNames[i]);
            if (!record || record->slices.size() != 1 ||
                record->slices[0].targetIndex != mat->id.index || record->slices[0].targetOffset != i * 8)
                return fail("mat row provenance slice is wrong (row-major order expected)");
        }
        const std::string report = readFile(reportPath);
        if (report.find("\tmerged\ttbl_0\t") == std::string::npos ||
            report.find("\t4\n") == std::string::npos)
            return fail("reg-to-mem report lost the 1-D shape column");
        if (report.find("\tmerged\tmat_0_0\t") == std::string::npos ||
            report.find("\t2x2\n") == std::string::npos)
            return fail("reg-to-mem report lost the 2-D shape column");
        if (!verifies(model)) return fail("merged model rejected by the verifier");
        Simulation rewritten(model);
        std::mt19937_64 rng(5150);
        for (unsigned step = 0; step < 512; ++step)
        {
            std::vector<uint64_t> inputs{uint64_t{step % 2}, rng() & 1, rng() & 1, rng() % 6, rng(),
                                         rng() % 6, rng()};
            reference.step(inputs);
            rewritten.step(inputs);
            if (reference.outputs() != rewritten.outputs())
                return fail("reg-to-mem declared differential mismatch");
        }
        return roundTrip(model, artifactDir, "grhsim_reg_to_mem_declared");
    }

    // ---- A2-A6: wired whole-graph pipeline --------------------------------
    // One model mixing all three shapes: a decoded table, four packable bit
    // flags and four isomorphic comb lanes.
    grhsim::GrhSimModel makePipelineModel()
    {
        ModelBuilder b;
        const auto addrType = b.model.logicType(3, false, grhsim::LogicDomain::TwoState);
        const auto clk = b.input("clk", b.bit);
        const auto u = b.input("u", b.bit);
        const auto en = b.input("en", b.bit);
        const auto addr = b.input("addr", addrType);
        const auto d = b.input("d", b.byte);
        const auto one = b.constant(b.bit, 1);
        const auto mask = b.constant(b.byte, 0xff);
        const std::vector<std::string> edges{"posedge"};
        for (unsigned row = 0; row < 4; ++row)
        {
            const auto state = b.state("tbl_" + std::to_string(row), b.byte, row);
            const auto old = b.read(state, b.byte);
            const auto hit = b.compute("core.compute.eq", b.bit, {addr, b.constant(addrType, row)});
            const auto low = b.compute("core.compute.logicAnd", b.bit, {en, hit});
            const auto next = b.compute("core.compute.mux", b.byte, {low, d, old});
            b.model.addOperation("core.state.regWrite", std::array{u, next, mask, clk}, {},
                                 std::array{grhsim::ObjectRef::state(state)},
                                 std::array{grhsim::Parameter{b.model.intern("event_edges"), edges}});
            b.output("qt_" + std::to_string(row), b.byte, old);
        }
        std::vector<grhsim::ValueId> flagReads;
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto state = b.state("flag_" + std::to_string(i), b.bit, i % 2);
            const auto di = b.input("fd" + std::to_string(i), b.bit);
            b.regWrite(state, en, di, one, clk);
            flagReads.push_back(b.read(state, b.bit));
        }
        auto acc = flagReads[0];
        for (std::size_t i = 1; i < flagReads.size(); ++i)
            acc = b.compute("core.compute.xor", b.bit, {acc, flagReads[i]});
        b.output("fo", b.bit, acc);
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto a = b.input("a" + std::to_string(i), b.byte);
            const auto bb = b.input("b" + std::to_string(i), b.byte);
            const auto c = b.input("c" + std::to_string(i), b.byte);
            const auto lane = b.compute("core.compute.and", b.byte,
                                        {b.compute("core.compute.or", b.byte, {a, bb}), c},
                                        "lane_" + std::to_string(i));
            b.declareValue("lane_" + std::to_string(i), lane);
            b.output("lo_" + std::to_string(i), b.byte, lane);
        }
        return std::move(b.model);
    }

    int runWholePipelineTest(const std::filesystem::path &artifactDir)
    {
        auto original = makePipelineModel();
        Simulation reference(original);
        auto model = makePipelineModel();
        runPass(model, "grhsim.canonicalize-compute");
        const std::array<std::string_view, 2> costOff{"--enable-cost-selection", "false"};
        const Messages regToMem = runPass(model, "grhsim.reg-to-mem", costOff);
        if (!messagesContain(regToMem, "transformed=1"))
            return fail("pipeline reg-to-mem did not merge the table");
        const Messages combPack = runPass(model, "grhsim.comb-pack");
        if (!messagesContain(combPack, "comb_pack_groups=1") ||
            !messagesContain(combPack, "comb_pack_lanes=4"))
            return fail("pipeline comb-pack did not pack the lanes");
        const Messages packBits = runPass(model, "grhsim.pack-bit-registers");
        if (!messagesContain(packBits, "packed_register_words=1"))
            return fail("pipeline pack-bit-registers did not pack the flags");
        const Messages simplify = runPass(model, "grhsim.simplify");
        if (!messagesContain(simplify, "simplify_converged=1"))
            return fail("pipeline simplify did not converge");
        if (!verifies(model)) return fail("pipeline result rejected by the verifier");
        for (const std::string_view prefix : {"tbl_0", "flag_0", "lane_0"})
        {
            const auto *record = findProvenance(model, prefix);
            if (!record || record->slices.empty())
                return fail("pipeline lost the provenance of " + std::string(prefix));
        }
        bool haveTable = false, havePacked = false;
        for (const auto &state : model.states())
        {
            const std::string_view name = model.text(state.name);
            if (name.find("__reg_to_mem_tbl_@__") == 0) haveTable = true;
            if (name.find("packed_bits_") == 0) havePacked = true;
        }
        if (!haveTable || !havePacked) return fail("pipeline result misses the table or packed word");
        Simulation rewritten(model);
        std::mt19937_64 rng(777);
        for (unsigned step = 0; step < 512; ++step)
        {
            std::vector<uint64_t> inputs(21);
            inputs[0] = step % 2; // clk toggles every eval
            for (std::size_t i = 1; i < inputs.size(); ++i) inputs[i] = rng();
            reference.step(inputs);
            rewritten.step(inputs);
            if (reference.outputs() != rewritten.outputs())
                return fail("whole-graph pipeline differential mismatch");
        }
        return roundTrip(model, artifactDir, "grhsim_whole_pipeline");
    }
} // namespace

int main()
{
    try
    {
        const std::filesystem::path artifactDir =
            std::filesystem::path(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR) / "whole_opt";
        if (const int status = runPackBitRegistersTest(artifactDir)) return status;
        if (const int status = runCombPackBitwiseTest(artifactDir)) return status;
        if (const int status = runCombPackMuxTest(artifactDir)) return status;
        if (const int status = runCombPackRejectTest()) return status;
        if (const int status = runRegToMemDeclaredTest(artifactDir)) return status;
        return runWholePipelineTest(artifactDir);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
