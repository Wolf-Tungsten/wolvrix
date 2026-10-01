// Directed tests for the M5d-4 semantic store classification (pipeline stage
// A7): the grhsim.select-state-stores pass, the StateStoreClass annotation
// carried by the model/JSON/verifier, and the incremental classification rule
// for passes that create or rebuild states after classification
// (grhsim.migrate-timeslot-tasks, grhsim.used-bits).

#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace wolvrix::lib;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-select-state-stores] " << message << '\n';
        return 1;
    }

    void check(bool condition, const char *message)
    {
        if (!condition) throw std::runtime_error(message);
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

    bool passCreationFails(std::string_view name, std::span<const std::string_view> args)
    {
        std::string error;
        return !grhsim::defaultPassRegistry().create(name, args, error);
    }

    bool messagesContain(const Messages &messages, std::string_view needle)
    {
        for (const auto &[context, message] : messages)
            if (message.find(needle) != std::string::npos) return true;
        return false;
    }

    std::optional<uint64_t> infoValue(const Messages &messages, std::string_view context,
                                      std::string_view key)
    {
        for (const auto &[messageContext, message] : messages)
        {
            if (messageContext != context) continue;
            const auto at = message.find(key);
            if (at == std::string::npos) continue;
            const auto begin = at + key.size();
            const auto end = message.find(' ', begin);
            return std::stoull(message.substr(begin, end == std::string::npos ? end : end - begin));
        }
        return std::nullopt;
    }

    Messages verifyMessages(grhsim::GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        grhsim::verifyGrhSimModel(model, grhsim::defaultDialectRegistry(), diagnostics);
        Messages messages;
        for (const auto &message : diagnostics.messages())
            messages.emplace_back(message.context, message.message);
        return messages;
    }

    bool verifies(grhsim::GrhSimModel &model)
    {
        const Messages messages = verifyMessages(model);
        const bool ok = messages.empty();
        for (const auto &[context, message] : messages)
            std::cerr << "[verify] " << context << ": " << message << '\n';
        return ok;
    }

    std::string storeJson(const grhsim::GrhSimModel &model)
    {
        std::ostringstream output;
        diag::Diagnostics diagnostics;
        if (!grhsim::writeGrhSimJson(model, output, grhsim::defaultDialectRegistry(), diagnostics))
            throw std::runtime_error("store failed");
        return output.str();
    }

    std::unique_ptr<grhsim::GrhSimModel> loadJson(const std::string &text, Messages *messages = nullptr)
    {
        std::istringstream input(text);
        diag::Diagnostics diagnostics;
        auto model = grhsim::readGrhSimJson(input, grhsim::defaultDialectRegistry(), diagnostics);
        if (messages)
            for (const auto &message : diagnostics.messages())
                messages->emplace_back(message.context, message.message);
        return model;
    }

    // ---- builders ---------------------------------------------------------

    grhsim::ValueId addConstant(grhsim::GrhSimModel &model, grhsim::TypeId type,
                                std::string literal)
    {
        const auto value = model.addValue(type);
        const std::array params{grhsim::Parameter{model.intern("constValue"), std::move(literal)}};
        model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
        return value;
    }

    grhsim::ValueId addInputRead(grhsim::GrhSimModel &model, std::string_view name,
                                 grhsim::TypeId type)
    {
        const auto id = model.addInput(name, type);
        const auto value = model.addValue(type, name);
        model.addOperation("core.input.read", {}, std::array{value},
                           std::array{grhsim::ObjectRef::input(id)}, {}, name);
        return value;
    }

    void addOutputWrite(grhsim::GrhSimModel &model, std::string_view name, grhsim::ValueId value)
    {
        const auto type = model.values()[value.index - 1].type;
        const auto id = model.addOutput(name, type);
        model.addOperation("core.output.write", std::array{value}, {},
                           std::array{grhsim::ObjectRef::output(id)}, {}, name);
    }

    grhsim::StateId addStateInit(grhsim::GrhSimModel &model, std::string_view name,
                                 grhsim::TypeId type, std::string literal)
    {
        const auto id = model.addState(name, type);
        const std::array params{grhsim::Parameter{model.intern("value"), std::move(literal)}};
        const std::array steps{grhsim::InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(id, steps, params);
        return id;
    }

    // Array states initialize through one full-coverage fill step.
    grhsim::StateId addArrayState(grhsim::GrhSimModel &model, std::string_view name,
                                  grhsim::TypeId element, uint64_t count, std::string fillLiteral)
    {
        const auto id = model.addState(name, model.arrayType(element, count));
        const std::array params{
            grhsim::Parameter{model.intern("value"), std::move(fillLiteral)},
            grhsim::Parameter{model.intern("start"), int64_t{0}},
            grhsim::Parameter{model.intern("count"), static_cast<int64_t>(count)}};
        const std::array steps{grhsim::InitStep{model.intern("core.init.fill"), {0, 3}}};
        model.addInit(id, steps, params);
        return id;
    }

    grhsim::StateStoreClass classOf(const grhsim::GrhSimModel &model, std::string_view name)
    {
        for (const auto &state : model.states())
            if (model.text(state.name) == name) return state.storeClass;
        throw std::runtime_error("state not found: " + std::string(name));
    }

    // Minimal two-state interpreter (same semantics as the whole-opt suite):
    // reads see the pre-step state, writes publish together after evaluation,
    // event edges compare against the previous step's event value
    // (zero-initialized prev slots).
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
            else if (kind == "core.compute.add") result = a(0) + a(1);
            else if (kind == "core.compute.not") result = ~a(0);
            else if (kind == "core.compute.and") result = a(0) & a(1);
            else if (kind == "core.compute.or") result = a(0) | a(1);
            else if (kind == "core.compute.xor") result = a(0) ^ a(1);
            else if (kind == "core.compute.mux") result = a(0) ? a(1) : a(2);
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

    // Contract fixture covering the four NBA contract aspects named by the
    // M5d-4 checkpoint: old-value reads (memRead sees the pre-write cell),
    // partial writes (masked memWrite), multi-write priority (memWriteSeq
    // operand order) and multi-round updates (the self-feedback counter).
    // Store classes: smallMem (4 bytes) -> regLatch; bigMem/seqMem (512
    // bytes) and fillMem (128 bytes) -> mem; the scalars -> regLatch.
    struct ContractFixture
    {
        grhsim::GrhSimModel model{"select-state-stores-contract"};
        grhsim::TypeId bit, byte, word, wide;

        ContractFixture()
        {
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            word = model.logicType(32, false, grhsim::LogicDomain::TwoState);
            wide = model.logicType(64, false, grhsim::LogicDomain::TwoState);
            const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
            const auto addr5 = model.logicType(5, false, grhsim::LogicDomain::TwoState);
            const auto addr2 = model.logicType(2, false, grhsim::LogicDomain::TwoState);

            const auto clk = addInputRead(model, "clk", bit);
            const auto d = addInputRead(model, "d", byte);
            const auto wdata = addInputRead(model, "wdata", wide);
            const auto wmask = addInputRead(model, "wmask", wide);
            const auto addrRaw = addInputRead(model, "addr", byte);
            const auto one = addConstant(model, bit, "1'h1");
            const auto mask8 = addConstant(model, byte, "8'hff");
            const auto slice = [&](grhsim::ValueId source, grhsim::TypeId type) {
                const auto value = model.addValue(type);
                const auto width = model.types()[type.index - 1].width;
                const std::array params{
                    grhsim::Parameter{model.intern("sliceStart"), int64_t{0}},
                    grhsim::Parameter{model.intern("sliceEnd"), static_cast<int64_t>(width - 1)}};
                model.addOperation("core.compute.sliceStatic", std::array{source},
                                   std::array{value}, {}, params);
                return value;
            };
            const auto a6 = slice(addrRaw, addr6);
            const auto a5 = slice(addrRaw, addr5);
            const auto a2 = slice(addrRaw, addr2);
            const std::array posedge{grhsim::Parameter{
                model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            const std::array negedge{grhsim::Parameter{
                model.intern("event_edges"), std::vector<std::string>{"negedge"}}};

            // Self-feedback counter (multi-round update).
            const auto cnt = addStateInit(model, "cnt", byte, "8'h0");
            const auto cntRead = model.addValue(byte);
            model.addOperation("core.state.read", {}, std::array{cntRead},
                               std::array{grhsim::ObjectRef::state(cnt)});
            const auto cntNext = model.addValue(byte);
            model.addOperation("core.compute.add", std::array{cntRead, d}, std::array{cntNext});
            model.addOperation("core.state.regWrite", std::array{one, cntNext, mask8, clk}, {},
                               std::array{grhsim::ObjectRef::state(cnt)}, posedge);
            addOutputWrite(model, "oCnt", cntRead);

            // Large array with a masked (partial) write and a same-round
            // old-value read.
            const auto bigMem = addArrayState(model, "bigMem", wide, 64, "64'h0");
            model.addOperation("core.state.memWrite", std::array{one, a6, wdata, wmask, clk}, {},
                               std::array{grhsim::ObjectRef::state(bigMem)}, posedge);
            const auto bigRead = model.addValue(wide);
            model.addOperation("core.state.memRead", std::array{a6}, std::array{bigRead},
                               std::array{grhsim::ObjectRef::state(bigMem)});
            addOutputWrite(model, "oBig", bigRead);

            // Large array with an ordered two-port sequence write: the later
            // triple overrides the earlier one on the same address.
            const auto seqMem = addArrayState(model, "seqMem", wide, 64, "64'h0");
            const auto notData = model.addValue(wide);
            model.addOperation("core.compute.not", std::array{wdata}, std::array{notData});
            model.addOperation("core.state.memWriteSeq",
                               std::array{one, a6, wdata, one, a6, notData, clk}, {},
                               std::array{grhsim::ObjectRef::state(seqMem)}, posedge);
            const auto seqRead = model.addValue(wide);
            model.addOperation("core.state.memRead", std::array{a6}, std::array{seqRead},
                               std::array{grhsim::ObjectRef::state(seqMem)});
            addOutputWrite(model, "oSeq", seqRead);

            // Large array filled on the falling edge.
            const auto fillMem = addArrayState(model, "fillMem", word, 32, "32'h0");
            const auto w32 = slice(wdata, word);
            model.addOperation("core.state.memFill", std::array{one, w32, clk}, {},
                               std::array{grhsim::ObjectRef::state(fillMem)}, negedge);
            const auto fillRead = model.addValue(word);
            model.addOperation("core.state.memRead", std::array{a5}, std::array{fillRead},
                               std::array{grhsim::ObjectRef::state(fillMem)});
            addOutputWrite(model, "oFill", fillRead);

            // Small array (4 bytes): addressed write and read, regLatch class.
            const auto smallMem = addArrayState(model, "smallMem", byte, 4, "8'h0");
            model.addOperation("core.state.memWrite", std::array{one, a2, d, mask8, clk}, {},
                               std::array{grhsim::ObjectRef::state(smallMem)}, posedge);
            const auto smallRead = model.addValue(byte);
            model.addOperation("core.state.memRead", std::array{a2}, std::array{smallRead},
                               std::array{grhsim::ObjectRef::state(smallMem)});
            addOutputWrite(model, "oSmall", smallRead);
        }
    };

    std::vector<std::vector<uint64_t>> trace(const grhsim::GrhSimModel &model, int steps)
    {
        Simulation sim(model);
        std::vector<std::vector<uint64_t>> out;
        for (int i = 0; i < steps; ++i)
        {
            const uint64_t clk = static_cast<uint64_t>(i & 1);
            sim.step({clk, static_cast<uint64_t>(i * 3 + 1),
                      static_cast<uint64_t>(i) * 0x100000001ULL + 7,
                      (i % 3 == 0) ? ~uint64_t{0} : uint64_t{0x0f0f0f0f0f0f0f0fULL},
                      static_cast<uint64_t>(i * 5 + 2)});
            out.push_back(sim.outputs());
        }
        return out;
    }

    // ---- tests ------------------------------------------------------------

    int testContractClassification()
    {
        ContractFixture fixture;
        auto &model = fixture.model;
        if (!verifies(model)) return fail("contract fixture rejected");
        const auto ops = model.operations().size();
        const auto values = model.values().size();
        const auto states = model.states().size();
        const auto traceBefore = trace(model, 64);

        const Messages messages = runPass(model, "grhsim.select-state-stores");
        using grhsim::StateStoreClass;
        if (classOf(model, "cnt") != StateStoreClass::RegLatch ||
            classOf(model, "smallMem") != StateStoreClass::RegLatch)
            return fail("small states were not classified regLatch");
        if (classOf(model, "bigMem") != StateStoreClass::Mem ||
            classOf(model, "seqMem") != StateStoreClass::Mem ||
            classOf(model, "fillMem") != StateStoreClass::Mem)
            return fail("large arrays were not classified mem");
        if (const auto n = infoValue(messages, "grhsim.select-state-stores", "state_stores_reg_latch="))
        {
            if (*n != 2) return fail("unexpected regLatch count");
        }
        else
            return fail("regLatch count missing from the info diagnostic");
        if (const auto n = infoValue(messages, "grhsim.select-state-stores", "state_stores_mem="))
        {
            if (*n != 3) return fail("unexpected mem count");
        }
        else
            return fail("mem count missing from the info diagnostic");

        // The pass is a pure annotation: the graph and the execution trace
        // are unchanged.
        if (model.operations().size() != ops || model.values().size() != values ||
            model.states().size() != states)
            return fail("classification changed the graph shape");
        if (trace(model, 64) != traceBefore)
            return fail("classification changed the interpreter trace");
        if (!verifies(model)) return fail("classified contract model rejected");
        return 0;
    }

    int testThresholdAndArgs()
    {
        {
            ContractFixture fixture;
            const Messages messages = runPass(fixture.model, "grhsim.select-state-stores",
                                              std::array<std::string_view, 2>{"--mem-min-bytes", "4"});
            using grhsim::StateStoreClass;
            if (classOf(fixture.model, "smallMem") != StateStoreClass::Mem)
                return fail("threshold 4 did not move the small array to mem");
            if (!messagesContain(messages, "state_stores_mem=4"))
                return fail("threshold run did not classify all four arrays as mem");
        }
        {
            ContractFixture fixture;
            runPass(fixture.model, "grhsim.select-state-stores",
                    std::array<std::string_view, 2>{"--mem-min-bytes", "4096"});
            if (classOf(fixture.model, "bigMem") != grhsim::StateStoreClass::RegLatch)
                return fail("large threshold did not keep the big array in regLatch");
        }
        if (!passCreationFails("grhsim.select-state-stores",
                               std::array<std::string_view, 2>{"--mem-min-bytes", "x"}))
            return fail("non-numeric mem-min-bytes accepted");
        if (!passCreationFails("grhsim.select-state-stores",
                               std::array<std::string_view, 2>{"--reclassify", "maybe"}))
            return fail("non-boolean reclassify accepted");
        if (!passCreationFails("grhsim.select-state-stores",
                               std::array<std::string_view, 2>{"--bogus", "1"}))
            return fail("unknown option accepted");
        if (!passCreationFails("grhsim.select-state-stores",
                               std::array<std::string_view, 1>{"--report"}))
            return fail("dangling option accepted");
        return 0;
    }

    int testIdempotentAndReclassify()
    {
        ContractFixture fixture;
        auto &model = fixture.model;
        runPass(model, "grhsim.select-state-stores");
        // Second run keeps every state and reports no mutation.
        {
            std::string error;
            auto pass = grhsim::defaultPassRegistry().create("grhsim.select-state-stores", {}, error);
            grhsim::PassManager manager(grhsim::defaultDialectRegistry());
            manager.addPass(std::move(pass));
            diag::Diagnostics diagnostics;
            const auto result = manager.run(model, diagnostics);
            if (!result.success || result.changed)
                return fail("second classification run was not a no-op");
        }
        // A hand-corrupted class is recomputed under --reclassify.
        for (const auto &state : model.states())
            if (model.text(state.name) == "bigMem")
            {
                model.setStateStoreClass(state.id, grhsim::StateStoreClass::RegLatch);
                break;
            }
        runPass(model, "grhsim.select-state-stores",
                std::array<std::string_view, 2>{"--reclassify", "true"});
        if (classOf(model, "bigMem") != grhsim::StateStoreClass::Mem)
            return fail("reclassify did not restore the mem class");
        if (!verifies(model)) return fail("reclassified model rejected");
        return 0;
    }

    int testJsonCarriers()
    {
        // Unclassified checkpoints keep the four-element state rows.
        ContractFixture unclassified;
        const std::string plain = storeJson(unclassified.model);
        if (plain.find(",\"regLatch\"]") != std::string::npos ||
            plain.find(",\"mem\"]") != std::string::npos)
            return fail("unclassified checkpoint carries store class elements");
        {
            Messages messages;
            auto loaded = loadJson(plain, &messages);
            if (!loaded) return fail("unclassified checkpoint does not load");
            if (storeJson(*loaded) != plain)
                return fail("unclassified checkpoint is not byte stable");
        }
        // Classified checkpoints carry the class as the fifth state-row
        // element and round-trip byte-stably.
        ContractFixture fixture;
        runPass(fixture.model, "grhsim.select-state-stores");
        const std::string classified = storeJson(fixture.model);
        Messages loadMessages;
        auto loaded = loadJson(classified, &loadMessages);
        if (!loaded) return fail("classified checkpoint does not load");
        if (classOf(*loaded, "bigMem") != grhsim::StateStoreClass::Mem ||
            classOf(*loaded, "smallMem") != grhsim::StateStoreClass::RegLatch)
            return fail("classified checkpoint lost the store classes");
        if (storeJson(*loaded) != classified)
            return fail("classified checkpoint is not byte stable");
        // Unknown class strings are rejected at load time. (The fixture has
        // no Mem-phase ops, so `,"mem"]` only terminates mem state rows.)
        std::string corrupted = classified;
        for (std::string::size_type at = 0;
             (at = corrupted.find(",\"mem\"]", at)) != std::string::npos;)
            corrupted.replace(at, 7, ",\"bogus\"]");
        Messages corruptMessages;
        if (loadJson(corrupted, &corruptMessages))
            return fail("checkpoint with an unknown store class loaded");
        if (!messagesContain(corruptMessages, "unknown state store class"))
            return fail("unknown store class was not diagnosed");
        return 0;
    }

    int testVerifierRules()
    {
        using grhsim::StateStoreClass;
        // Totality: one classified state among unclassified ones.
        {
            ContractFixture fixture;
            auto &model = fixture.model;
            for (const auto &state : model.states())
                if (model.text(state.name) == "cnt")
                {
                    model.setStateStoreClass(state.id, StateStoreClass::RegLatch);
                    break;
                }
            if (!messagesContain(verifyMessages(model), "store classification is not total"))
                return fail("partial classification was not rejected");
        }
        // The mem class requires an array state.
        {
            ContractFixture fixture;
            auto &model = fixture.model;
            runPass(model, "grhsim.select-state-stores");
            for (const auto &state : model.states())
                if (model.text(state.name) == "cnt")
                {
                    model.setStateStoreClass(state.id, StateStoreClass::Mem);
                    break;
                }
            if (!messagesContain(verifyMessages(model), "mem store class requires a core.array"))
                return fail("mem class on a scalar state was not rejected");
        }
        // A mem-class state may not be written by regWrite/latchWrite.
        {
            grhsim::GrhSimModel model("select-state-stores-badwrite");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto wide = model.logicType(64, false, grhsim::LogicDomain::TwoState);
            const auto arrayType = model.arrayType(wide, 64);
            const auto mem = addArrayState(model, "mem", wide, 64, "64'h0");
            const auto one = addConstant(model, bit, "1'h1");
            const auto data = model.addValue(arrayType);
            model.addOperation("core.compute.constant", {}, std::array{data}, {},
                               std::array{grhsim::Parameter{model.intern("constValue"),
                                                            std::string("0")}});
            model.addOperation("core.state.regWrite", std::array{one, data, data}, {},
                               std::array{grhsim::ObjectRef::state(mem)});
            model.setStateStoreClass(mem, StateStoreClass::Mem);
            const Messages messages = verifyMessages(model);
            if (!messagesContain(messages, "targets a mem-class state"))
                return fail("regWrite on a mem-class state was not rejected");
        }
        return 0;
    }

    int testCloneAndCompact()
    {
        grhsim::GrhSimModel model("select-state-stores-compact");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto wide = model.logicType(64, false, grhsim::LogicDomain::TwoState);
        addStateInit(model, "q", bit, "1'h0");
        addArrayState(model, "bigMem", wide, 64, "64'h0");
        // Unreferenced small array: compact removes it; the survivors must
        // keep their store classes.
        addArrayState(model, "orphan", wide, 4, "64'h0");
        runPass(model, "grhsim.select-state-stores");
        using grhsim::StateStoreClass;
        auto clone = model.clone();
        if (classOf(clone, "bigMem") != StateStoreClass::Mem ||
            classOf(clone, "orphan") != StateStoreClass::RegLatch)
            return fail("clone lost the store classes");
        std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
        std::vector<uint8_t> removeStates(model.states().size() + 1, 0);
        for (const auto &state : model.states())
            if (model.text(state.name) == "orphan") removeStates[state.id.index] = 1;
        model.compact(removeOps, removeStates);
        if (model.states().size() != 2) return fail("compact did not remove the orphan state");
        if (classOf(model, "bigMem") != StateStoreClass::Mem ||
            classOf(model, "q") != StateStoreClass::RegLatch)
            return fail("compact lost the survivors' store classes");
        if (!verifies(model)) return fail("compacted classified model rejected");
        return 0;
    }

    int testIncrementalMigrateTimeslot()
    {
        using grhsim::StateStoreClass;
        const auto buildModel = [] {
            grhsim::GrhSimModel model("select-state-stores-tslot");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto cond = addConstant(model, bit, "1'h1");
            const auto data = addInputRead(model, "din", byte);
            // A pre-existing state so classification is non-vacuous when
            // the timeslot migration adds its prev states.
            const auto mask8 = addConstant(model, byte, "8'hff");
            const auto q = addStateInit(model, "q", byte, "8'h0");
            model.addOperation("core.state.latchWrite", std::array{cond, data, mask8}, {},
                               std::array{grhsim::ObjectRef::state(q)});
            const std::array params{
                grhsim::Parameter{model.intern("name"), std::string("strobe")},
                grhsim::Parameter{model.intern("proc_kind"), std::string("always")},
                grhsim::Parameter{model.intern("has_timing"), false}};
            model.addOperation("core.system.task", std::array{cond, data}, {}, {}, params);
            return model;
        };
        // Classified model: the new __tslot_prev_* states are classified
        // regLatch incrementally.
        {
            auto model = buildModel();
            runPass(model, "grhsim.select-state-stores");
            runPass(model, "grhsim.migrate-timeslot-tasks");
            std::size_t prevStates = 0;
            for (const auto &state : model.states())
            {
                if (state.storeClass != StateStoreClass::RegLatch)
                    return fail("a state was not classified regLatch");
                if (model.text(state.name).starts_with("__tslot_prev_")) ++prevStates;
            }
            if (prevStates == 0) return fail("timeslot migration created no prev states");
            if (!verifies(model)) return fail("classified timeslot model rejected");
        }
        // Unclassified model: the new states stay unclassified.
        {
            auto model = buildModel();
            runPass(model, "grhsim.migrate-timeslot-tasks");
            if (model.states().empty()) return fail("timeslot migration created no prev states");
            if (model.hasStateStoreClassification())
                return fail("timeslot prev states were classified without a classified model");
            if (!verifies(model)) return fail("unclassified timeslot model rejected");
        }
        return 0;
    }

    int testIncrementalUsedBits()
    {
        grhsim::GrhSimModel model("select-state-stores-used-bits");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto word = model.logicType(32, false, grhsim::LogicDomain::TwoState);
        const auto nibble = model.logicType(4, false, grhsim::LogicDomain::TwoState);
        const auto one = addConstant(model, bit, "1'h1");
        const auto mask32 = addConstant(model, word, "32'hffffffff");
        const auto inD = addInputRead(model, "inD", word);
        // Only the low 4 bits of q are observable, so used-bits rebuilds the
        // state narrowed; the rebuild must inherit the regLatch class.
        const auto q = addStateInit(model, "q", word, "32'h0");
        model.addOperation("core.state.latchWrite", std::array{one, inD, mask32}, {},
                           std::array{grhsim::ObjectRef::state(q)});
        const auto read = model.addValue(word);
        model.addOperation("core.state.read", {}, std::array{read},
                           std::array{grhsim::ObjectRef::state(q)});
        const auto slice = model.addValue(nibble);
        const std::array sliceParams{grhsim::Parameter{model.intern("sliceStart"), int64_t{0}},
                                     grhsim::Parameter{model.intern("sliceEnd"), int64_t{3}}};
        model.addOperation("core.compute.sliceStatic", std::array{read}, std::array{slice}, {},
                           sliceParams);
        addOutputWrite(model, "oQ", slice);

        runPass(model, "grhsim.select-state-stores");
        if (classOf(model, "q") != grhsim::StateStoreClass::RegLatch)
            return fail("fixture state was not classified regLatch");
        runPass(model, "grhsim.used-bits");
        if (model.states().size() != 1) return fail("used-bits did not keep exactly one state");
        const auto &state = model.states().front();
        if (state.storeClass != grhsim::StateStoreClass::RegLatch)
            return fail("narrowed state lost the regLatch class");
        if (model.types()[state.type.index - 1].width != 4)
            return fail("fixture state was not narrowed");
        if (!verifies(model)) return fail("narrowed classified model rejected");
        return 0;
    }
} // namespace

int main()
{
    try
    {
        if (const int status = testContractClassification()) return status;
        if (const int status = testThresholdAndArgs()) return status;
        if (const int status = testIdempotentAndReclassify()) return status;
        if (const int status = testJsonCarriers()) return status;
        if (const int status = testVerifierRules()) return status;
        if (const int status = testCloneAndCompact()) return status;
        if (const int status = testIncrementalMigrateTimeslot()) return status;
        if (const int status = testIncrementalUsedBits()) return status;
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    std::cout << "grhsim-select-state-stores tests passed\n";
    return 0;
}
