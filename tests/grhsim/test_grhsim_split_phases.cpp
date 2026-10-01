// Directed tests for M5d-5 (first decomposition + partition optimization):
//  - B5 grhsim.split-phases: class-aware phase attribution (P_mem write duty
//    follows the target state's store class) plus the legacy fallback;
//  - the class-aware verifyPhaseAttribution rules (both directions);
//  - B8 semantic seal (grhsim.verify --seal semantic): total attribution, no
//    surviving event_edges, Mem operands produced in P_general;
//  - B6 grhsim.simplify(scope=phase): cross-phase interface values (a Mem
//    write's General operand) are never removed by an in-scope rewrite;
//  - B7 grhsim.clone-shared-compute: boundary-aware cloning against the
//    predictGeneralBoundaries helper, phase-inheriting clones, idle reasons;
//  - prediction vs cpu.st.build-general-nodes node boundaries (consistency);
//  - partition-stage interpreter equivalence (raw event_edges form vs the
//    sealed lowered form) and the timeslot lifecycle through B1-B8.

#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/general_boundaries.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
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
        std::cerr << "[grhsim-split-phases] " << message << '\n';
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
        {
            for (const auto &[context, message] : messages)
                std::cerr << "[grhsim-split-phases] " << context << ": " << message << '\n';
            throw std::runtime_error("pass failed: " + std::string(name));
        }
        return messages;
    }

    bool passCreationFails(std::string_view name, std::span<const std::string_view> args)
    {
        std::string error;
        return !grhsim::defaultPassRegistry().create(name, args, error);
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

    bool infoHas(const Messages &messages, std::string_view context, std::string_view needle)
    {
        for (const auto &[messageContext, message] : messages)
            if (messageContext == context && message.find(needle) != std::string::npos) return true;
        return false;
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

    bool sealVerifies(grhsim::GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        const bool ok = grhsim::verifyGrhSimSemanticSeal(model, diagnostics);
        for (const auto &message : diagnostics.messages())
            std::cerr << "[seal] " << message.context << ": " << message.message << '\n';
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

    grhsim::ValueId addStateRead(grhsim::GrhSimModel &model, grhsim::StateId state,
                                 grhsim::TypeId type, std::string_view name)
    {
        const auto value = model.addValue(type, name);
        model.addOperation("core.state.read", {}, std::array{value},
                           std::array{grhsim::ObjectRef::state(state)}, {}, name);
        return value;
    }

    grhsim::ValueId addMemRead(grhsim::GrhSimModel &model, grhsim::StateId state,
                               grhsim::ValueId addr, grhsim::TypeId element,
                               std::string_view name)
    {
        const auto value = model.addValue(element, name);
        model.addOperation("core.state.memRead", std::array{addr}, std::array{value},
                           std::array{grhsim::ObjectRef::state(state)}, {}, name);
        return value;
    }

    grhsim::OpId addRegWrite(grhsim::GrhSimModel &model, grhsim::StateId state,
                             grhsim::ValueId enable, grhsim::ValueId data, grhsim::ValueId mask,
                             bool posedgeClk = false, grhsim::ValueId clk = {})
    {
        if (!posedgeClk)
        {
            return model.addOperation("core.state.regWrite", std::array{enable, data, mask}, {},
                                      std::array{grhsim::ObjectRef::state(state)});
        }
        const std::array params{grhsim::Parameter{
            model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        return model.addOperation("core.state.regWrite", std::array{enable, data, mask, clk}, {},
                                  std::array{grhsim::ObjectRef::state(state)}, params);
    }

    grhsim::OpId addMemWrite(grhsim::GrhSimModel &model, grhsim::StateId state,
                             grhsim::ValueId enable, grhsim::ValueId addr, grhsim::ValueId data,
                             grhsim::ValueId mask, bool posedgeClk = false,
                             grhsim::ValueId clk = {})
    {
        if (!posedgeClk)
        {
            return model.addOperation("core.state.memWrite",
                                      std::array{enable, addr, data, mask}, {},
                                      std::array{grhsim::ObjectRef::state(state)});
        }
        const std::array params{grhsim::Parameter{
            model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        return model.addOperation("core.state.memWrite",
                                  std::array{enable, addr, data, mask, clk}, {},
                                  std::array{grhsim::ObjectRef::state(state)}, params);
    }

    grhsim::ValueId addCompute(grhsim::GrhSimModel &model, std::string_view kind,
                               std::vector<grhsim::ValueId> operands, grhsim::TypeId type,
                               std::string_view name = {})
    {
        const auto value = model.addValue(type, name);
        model.addOperation(kind, operands, std::array{value}, {}, {}, name);
        return value;
    }

    grhsim::ValueId addSlice(grhsim::GrhSimModel &model, grhsim::ValueId source, int64_t low,
                             int64_t high, grhsim::TypeId type, std::string_view name = {})
    {
        const auto value = model.addValue(type, name);
        const std::array params{grhsim::Parameter{model.intern("sliceStart"), low},
                                grhsim::Parameter{model.intern("sliceEnd"), high}};
        model.addOperation("core.compute.sliceStatic", std::array{source}, std::array{value}, {},
                           params, name);
        return value;
    }

    const grhsim::SimOp *findOp(const grhsim::GrhSimModel &model, std::string_view name)
    {
        for (const auto &op : model.operations())
            if (model.text(op.name) == name) return &op;
        return nullptr;
    }

    std::vector<const grhsim::SimOp *> opsOfType(const grhsim::GrhSimModel &model,
                                                 std::string_view type)
    {
        std::vector<const grhsim::SimOp *> result;
        for (const auto &op : model.operations())
            if (model.text(op.opType) == type) result.push_back(&op);
        return result;
    }

    // ---- dual-form interpreter --------------------------------------------
    // Two-state interpreter covering both event forms: raw event_edges
    // (pre-B2) and the lowered edgeDet/event_acts form (post-B2). Reads see
    // the pre-step state, all writes publish together after evaluation
    // (single-round NBA approximation), edgeDet prev slots initialize from
    // their prevInit literals and update immediately after comparison.
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
            for (const auto &op : m.operations())
            {
                if (m.text(op.opType) != "core.event.edgeDet") continue;
                std::string edge, init;
                int64_t act = -1, prev = -1;
                for (const auto &p : m.parameters(op))
                {
                    if (m.text(p.name) == "edge") edge = std::get<std::string>(p.value);
                    if (m.text(p.name) == "act") act = std::get<int64_t>(p.value);
                    if (m.text(p.name) == "prev") prev = std::get<int64_t>(p.value);
                    if (m.text(p.name) == "prevInit") init = std::get<std::string>(p.value);
                }
                check(act >= 0 && prev >= 0 && !edge.empty() && !init.empty(), "bad edgeDet");
                if (actBits.size() <= static_cast<std::size_t>(act)) actBits.resize(act + 1);
                if (prevSlots.size() <= static_cast<std::size_t>(prev))
                    prevSlots.resize(prev + 1, 0);
                prevSlots[prev] = literal(init) != 0;
                edgeDets.push_back(op.id);
            }
        }

        void step(const std::vector<uint64_t> &inputs)
        {
            inputValues = inputs;
            std::fill(ready.begin(), ready.end(), false);
            std::fill(actBits.begin(), actBits.end(), 0);
            // P_event: evaluate each detector's cone value, compare against
            // its private prev slot, publish the act bit, update prev.
            for (const auto id : edgeDets)
            {
                const auto &op = m.operations()[id.index - 1];
                const auto eventValue = m.operands(op)[0];
                const bool event = eval(eventValue) != 0;
                std::string edge;
                int64_t act = -1, prev = -1;
                for (const auto &p : m.parameters(op))
                {
                    if (m.text(p.name) == "edge") edge = std::get<std::string>(p.value);
                    if (m.text(p.name) == "act") act = std::get<int64_t>(p.value);
                    if (m.text(p.name) == "prev") prev = std::get<int64_t>(p.value);
                }
                const bool previous = prevSlots[prev] != 0;
                const bool fired = edge == "both" ? (event != previous)
                                   : edge == "posedge" ? (!previous && event)
                                                       : (previous && !event);
                actBits[act] = fired ? 1 : 0;
                prevSlots[prev] = event;
            }
            auto pending = states;
            std::unordered_map<uint32_t, uint64_t> nextEvents;
            for (const auto &op : m.operations())
            {
                const auto kind = m.text(op.opType);
                if (kind != "core.state.regWrite" && kind != "core.state.latchWrite" &&
                    kind != "core.state.memWrite" && kind != "core.state.memWriteSeq" &&
                    kind != "core.state.memFill")
                    continue;
                const auto args = m.operands(op);
                const auto objects = m.objectRefs(op);
                const std::vector<std::string> *edges = nullptr;
                const std::vector<int64_t> *acts = nullptr;
                for (const auto &p : m.parameters(op))
                {
                    if (m.text(p.name) == "event_edges")
                        edges = std::get_if<std::vector<std::string>>(&p.value);
                    if (m.text(p.name) == "event_acts")
                        acts = std::get_if<std::vector<int64_t>>(&p.value);
                }
                bool fire = true;
                if (edges && !edges->empty())
                {
                    const auto eventCount = edges->size();
                    check(args.size() >= eventCount, "invalid event list");
                    fire = false;
                    for (std::size_t i = 0; i < eventCount; ++i)
                    {
                        const auto eventValue = args[args.size() - eventCount + i];
                        const auto event = eval(eventValue);
                        const auto it = prevEvents.find(eventValue.index);
                        const auto previous = it == prevEvents.end() ? 0 : it->second;
                        check((*edges)[i] == "posedge" || (*edges)[i] == "negedge",
                              "unsupported test edge");
                        fire |= (*edges)[i] == "posedge" ? (!previous && event)
                                                         : (previous && !event);
                        nextEvents[eventValue.index] = event;
                    }
                }
                else if (acts)
                {
                    fire = false;
                    for (const auto act : *acts) fire |= actBits.at(act) != 0;
                }
                if (!fire) continue;
                const auto eventCount = edges ? edges->size() : 0;
                auto &target = pending.at(objects[0].index);
                if (kind == "core.state.regWrite" || kind == "core.state.latchWrite")
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
        std::vector<grhsim::OpId> edgeDets;
        std::vector<uint8_t> actBits;
        std::vector<uint8_t> prevSlots;
    };

    // Evented counter + two arrays fixture (raw event_edges form). smallMem
    // (4x8 = 32 bits) classifies regLatch; bigMem (64x8 = 512 bits) classifies
    // mem. nb = not(wen2) is a shared bijection consumed by the smallMem
    // write enable and the counter write enable (B7 clone material).
    struct EventFixture
    {
        static constexpr std::size_t kInputs = 6; // clk, rst, wen, wen2, d, waddr

        static grhsim::GrhSimModel build()
        {
            grhsim::GrhSimModel model("split-phases-event");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto addr2 = model.logicType(2, false, grhsim::LogicDomain::TwoState);
            const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);

            const auto clk = addInputRead(model, "clk", bit);
            const auto rst = addInputRead(model, "rst", bit);
            const auto wen = addInputRead(model, "wen", bit);
            const auto wen2 = addInputRead(model, "wen2", bit);
            const auto d = addInputRead(model, "d", byte);
            const auto waddr = addInputRead(model, "waddr", addr6);
            const auto one = addConstant(model, bit, "1'b1");
            const auto zero8 = addConstant(model, byte, "8'h00");
            const auto one8 = addConstant(model, byte, "8'h01");
            const auto mask8 = addConstant(model, byte, "8'hff");
            const auto zero2 = addConstant(model, addr2, "2'h0");
            const auto one6 = addConstant(model, addr6, "6'h01");

            const auto q = addStateInit(model, "q", byte, "8'h00");
            const auto smallMem = addArrayState(model, "smallMem", byte, 4, "8'h00");
            const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");

            const auto qv = addStateRead(model, q, byte, "qv");
            const auto inc = addCompute(model, "core.compute.add", {qv, one8}, byte, "inc");
            const auto nextq = addCompute(model, "core.compute.mux", {rst, zero8, inc}, byte, "nextq");
            const auto nb = addCompute(model, "core.compute.not", {wen2}, bit, "nb");
            const auto qEnable = addCompute(model, "core.compute.or", {nb, rst}, bit, "qen");
            addRegWrite(model, q, qEnable, nextq, mask8, true, clk);

            const auto saddr = addSlice(model, waddr, 0, 1, addr2, "saddr");
            const auto smEnable = addCompute(model, "core.compute.and", {nb, wen}, bit, "smen");
            addMemWrite(model, smallMem, smEnable, saddr, d, mask8, true, clk);
            addMemWrite(model, bigMem, wen2, waddr, qv, mask8, true, clk);

            const auto smv = addMemRead(model, smallMem, zero2, byte, "smv");
            const auto out1 = addCompute(model, "core.compute.xor", {qv, smv}, byte, "out1v");
            addOutputWrite(model, "out1", out1);
            const auto bmv = addMemRead(model, bigMem, one6, byte, "bmv");
            const auto out2 = addCompute(model, "core.compute.add", {bmv, qv}, byte, "out2v");
            addOutputWrite(model, "out2", out2);
            return model;
        }

        static std::vector<uint64_t> inputs(int step)
        {
            const auto u = static_cast<uint64_t>(step);
            return {u & 1, (step >= 4 && step <= 5) ? 1ULL : 0ULL,
                    (step % 3 != 0) ? 1ULL : 0ULL, (step % 5 == 0) ? 1ULL : 0ULL,
                    u * 3 + 1, (u * 7) & 63};
        }

        static std::vector<std::vector<uint64_t>> trace(const grhsim::GrhSimModel &model, int steps)
        {
            Simulation sim(model);
            std::vector<std::vector<uint64_t>> out;
            for (int i = 0; i < steps; ++i)
            {
                sim.step(inputs(i));
                out.push_back(sim.outputs());
            }
            return out;
        }
    };

    // Runs the whole B segment in production order.
    void runPartitionStage(grhsim::GrhSimModel &model)
    {
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        runPass(model, "grhsim.split-phases");
        runPass(model, "grhsim.simplify", std::array<std::string_view, 2>{"--scope", "phase"});
        runPass(model, "grhsim.clone-shared-compute");
    }

    // ---- tests ------------------------------------------------------------

    int testAttribution()
    {
        grhsim::GrhSimModel model("split-phases-attribution");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
        const auto addr2 = model.logicType(2, false, grhsim::LogicDomain::TwoState);
        const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
        const auto d = addInputRead(model, "d", byte);
        const auto we = addInputRead(model, "we", bit);
        const auto addrS = addInputRead(model, "addrS", addr2);
        const auto addrB = addInputRead(model, "addrB", addr6);
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto q = addStateInit(model, "q", byte, "8'h00");
        const auto smallMem = addArrayState(model, "smallMem", byte, 4, "8'h00");
        const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");
        const auto regOp = addRegWrite(model, q, we, d, mask8);
        const auto smallOp = addMemWrite(model, smallMem, we, addrS, d, mask8);
        const auto bigOp = addMemWrite(model, bigMem, we, addrB, d, mask8);

        runPass(model, "grhsim.select-state-stores");
        const Messages messages = runPass(model, "grhsim.split-phases");
        const auto phaseOf = [&](grhsim::OpId id) {
            return model.operations()[id.index - 1].phase;
        };
        if (phaseOf(regOp) != grhsim::SimPhase::General) return fail("regWrite not General");
        if (phaseOf(smallOp) != grhsim::SimPhase::General)
            return fail("regLatch-class mem write not attributed General");
        if (phaseOf(bigOp) != grhsim::SimPhase::Mem)
            return fail("mem-class mem write not attributed Mem");
        if (infoValue(messages, "grhsim.split-phases", "mem_writes_reglatch=") !=
            std::optional<uint64_t>(1))
            return fail("mem_writes_reglatch counter wrong");
        if (infoValue(messages, "grhsim.split-phases", "phase_mem=") !=
            std::optional<uint64_t>(1))
            return fail("phase_mem counter wrong");
        if (!verifies(model)) return fail("attributed model rejected");
        if (!sealVerifies(model)) return fail("attributed model failed the semantic seal");

        // Idempotency: a second run attributes nothing.
        const Messages again = runPass(model, "grhsim.split-phases");
        if (infoValue(again, "grhsim.split-phases", "attributed=") != std::optional<uint64_t>(0))
            return fail("second split-phases run was not a no-op");

        // JSON round-trip preserves the attribution byte-stably.
        const std::string stored = storeJson(model);
        std::istringstream input(stored);
        diag::Diagnostics diagnostics;
        auto loaded = grhsim::readGrhSimJson(input, grhsim::defaultDialectRegistry(), diagnostics);
        if (!loaded || diagnostics.hasError()) return fail("sealed model does not load");
        if (storeJson(*loaded) != stored) return fail("sealed model round-trip is not byte-stable");

        // Legacy fallback: without a classification, mem writes stay Mem.
        grhsim::GrhSimModel legacy("split-phases-legacy");
        legacy.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto lbit = legacy.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto lbyte = legacy.logicType(8, false, grhsim::LogicDomain::TwoState);
        const auto laddr = legacy.logicType(2, false, grhsim::LogicDomain::TwoState);
        const auto ld = addInputRead(legacy, "d", lbyte);
        const auto lwe = addInputRead(legacy, "we", lbit);
        const auto la = addInputRead(legacy, "a", laddr);
        const auto lmask = addConstant(legacy, lbyte, "8'hff");
        const auto lmem = addArrayState(legacy, "mem", lbyte, 4, "8'h00");
        const auto lop = addMemWrite(legacy, lmem, lwe, la, ld, lmask);
        runPass(legacy, "grhsim.split-phases");
        if (legacy.operations()[lop.index - 1].phase != grhsim::SimPhase::Mem)
            return fail("unclassified model lost the legacy all-Mem attribution");

        if (!passCreationFails("grhsim.split-phases", std::array<std::string_view, 2>{"--x", "1"}))
            return fail("grhsim.split-phases accepted arguments");
        return 0;
    }

    int testClassAwareVerifier()
    {
        const auto build = [] {
            grhsim::GrhSimModel model("split-phases-verifier");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto addr2 = model.logicType(2, false, grhsim::LogicDomain::TwoState);
            const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
            const auto d = addInputRead(model, "d", byte);
            const auto we = addInputRead(model, "we", bit);
            const auto as = addInputRead(model, "as", addr2);
            const auto ab = addInputRead(model, "ab", addr6);
            const auto mask = addConstant(model, byte, "8'hff");
            const auto smallMem = addArrayState(model, "smallMem", byte, 4, "8'h00");
            const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");
            const auto smallOp = addMemWrite(model, smallMem, we, as, d, mask);
            const auto bigOp = addMemWrite(model, bigMem, we, ab, d, mask);
            runPass(model, "grhsim.select-state-stores");
            return std::tuple<grhsim::GrhSimModel, grhsim::OpId, grhsim::OpId>(std::move(model),
                                                                               smallOp, bigOp);
        };
        // regLatch-class write carrying Mem is rejected.
        {
            auto [model, smallOp, bigOp] = build();
            (void)bigOp;
            model.setOperationPhase(smallOp, grhsim::SimPhase::Mem);
            if (verifies(model))
                return fail("Mem-phase mem write on a regLatch-class state was accepted");
        }
        // mem-class write carrying General is rejected.
        {
            auto [model, smallOp, bigOp] = build();
            (void)smallOp;
            model.setOperationPhase(bigOp, grhsim::SimPhase::General);
            if (verifies(model))
                return fail("General-phase mem write on a mem-class state was accepted");
        }
        // The correct assignment verifies.
        {
            auto [model, smallOp, bigOp] = build();
            model.setOperationPhase(smallOp, grhsim::SimPhase::General);
            model.setOperationPhase(bigOp, grhsim::SimPhase::Mem);
            if (!verifies(model)) return fail("class-consistent attribution rejected");
        }
        return 0;
    }

    int testSemanticSeal()
    {
        // Positive: the full partition stage seals the event fixture.
        {
            auto model = EventFixture::build();
            runPass(model, "grhsim.select-state-stores");
            runPartitionStage(model);
            const Messages messages =
                runPass(model, "grhsim.verify", std::array<std::string_view, 2>{"--seal", "semantic"});
            (void)messages;
            if (!sealVerifies(model)) return fail("sealed partition-stage model failed the seal");
        }
        // Negative: an unattributed op fails the seal but not plain verify.
        {
            auto model = EventFixture::build();
            runPass(model, "grhsim.select-state-stores");
            runPartitionStage(model);
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto spareConst = addConstant(model, bit, "1'b0");
            addCompute(model, "core.compute.not", {spareConst}, bit, "spare");
            if (!verifies(model)) return fail("partially attributed model rejected by plain verify");
            if (sealVerifies(model)) return fail("seal accepted an unattributed op");
        }
        // Negative: a surviving event_edges parameter fails the seal.
        {
            grhsim::GrhSimModel model("split-phases-seal-raw");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto clk = addInputRead(model, "clk", bit);
            const auto d = addInputRead(model, "d", byte);
            const auto one = addConstant(model, bit, "1'b1");
            const auto mask = addConstant(model, byte, "8'hff");
            const auto q = addStateInit(model, "q", byte, "8'h00");
            addRegWrite(model, q, one, d, mask, true, clk);
            runPass(model, "grhsim.select-state-stores");
            runPass(model, "grhsim.split-phases");
            if (!verifies(model)) return fail("raw event model rejected by plain verify");
            if (sealVerifies(model)) return fail("seal accepted a surviving event_edges parameter");
        }
        // Negative: a Mem-phase operand produced by an Event-phase op fails.
        {
            auto model = EventFixture::build();
            runPass(model, "grhsim.select-state-stores");
            runPartitionStage(model);
            // Find the bigMem (mem-class) write and flip its data operand's
            // producer to Event — robust to the partition-stage rewrites.
            grhsim::StateId bigMem;
            for (const auto &state : model.states())
                if (model.text(state.name) == "bigMem") bigMem = state.id;
            if (!bigMem.valid()) return fail("bigMem state not found");
            const grhsim::SimOp *write = nullptr;
            for (const auto &op : model.operations())
            {
                if (op.phase != grhsim::SimPhase::Mem) continue;
                const auto refs = model.objectRefs(op);
                if (!refs.empty() && refs[0].index == bigMem.index) write = &op;
            }
            if (!write) return fail("bigMem Mem-phase write not found");
            const auto data = model.operands(*write)[2];
            const grhsim::SimOp *producerOp = nullptr;
            for (const auto &op : model.operations())
                for (const auto result : model.results(op))
                    if (result == data) producerOp = &op;
            if (!producerOp) return fail("mem write data producer not found");
            model.setOperationPhase(producerOp->id, grhsim::SimPhase::Event);
            if (sealVerifies(model))
                return fail("seal accepted a Mem operand produced outside P_general");
        }
        if (!passCreationFails("grhsim.verify", std::array<std::string_view, 2>{"--seal", "bogus"}))
            return fail("grhsim.verify accepted an unknown seal mode");
        if (!passCreationFails("grhsim.verify", std::array<std::string_view, 1>{"--seal"}))
            return fail("grhsim.verify accepted a dangling option");
        return 0;
    }

    int testPhaseScopeInterface()
    {
        grhsim::GrhSimModel model("split-phases-interface");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
        const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
        const auto x = addInputRead(model, "x", byte);
        const auto we = addInputRead(model, "we", bit);
        const auto c05 = addConstant(model, byte, "8'h05");
        const auto cff = addConstant(model, byte, "8'hff");
        const auto c00 = addConstant(model, addr6, "6'h00");
        const auto q = addStateInit(model, "q", byte, "8'h00");
        const auto q2 = addStateInit(model, "q2", byte, "8'h00");
        const auto q3 = addStateInit(model, "q3", byte, "8'h00");
        const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");
        // t aliases x and is consumed by both an in-scope (General) op and the
        // Mem-phase write's address operand: the phase simplify must not
        // remove the alias (the Mem consumer is never rewired).
        addCompute(model, "core.compute.assign", {x}, byte, "t");
        const auto t = model.values().back().id;
        addCompute(model, "core.compute.xor", {t, c05}, byte, "u");
        addRegWrite(model, q, we, model.values().back().id, cff);
        // Identical adds: dupA/dupC feed only General sinks (CSE merges one
        // away), dupB feeds the Mem write's data operand (guarded, survives).
        addCompute(model, "core.compute.add", {x, c05}, byte, "dupA");
        addRegWrite(model, q2, we, model.values().back().id, cff);
        addCompute(model, "core.compute.add", {x, c05}, byte, "dupB");
        const auto dupB = model.values().back().id;
        addCompute(model, "core.compute.add", {x, c05}, byte, "dupC");
        addRegWrite(model, q3, we, model.values().back().id, cff);
        const auto memWrite = model.addOperation(
            "core.state.memWrite", std::array{we, t, dupB, cff}, {},
            std::array{grhsim::ObjectRef::state(bigMem)}, {}, "wbig");
        (void)memWrite;
        // Readers keep every state alive under used-bits.
        addOutputWrite(model, "oQ", addStateRead(model, q, byte, "qv"));
        addOutputWrite(model, "oQ2", addStateRead(model, q2, byte, "q2v"));
        addOutputWrite(model, "oQ3", addStateRead(model, q3, byte, "q3v"));
        addOutputWrite(model, "oM", addMemRead(model, bigMem, c00, byte, "bmv"));

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.split-phases");
        // The phase simplify must succeed: removing an interface value would
        // strand the Mem consumer (compact threw before the M5d-5 guard).
        runPass(model, "grhsim.simplify", std::array<std::string_view, 2>{"--scope", "phase"});

        // Names survive compaction; ids do not. Resolve by name and follow
        // producer chains (used-bits may interpose a width slice).
        const auto valueNamed = [&](std::string_view name) -> grhsim::ValueId {
            for (const auto &value : model.values())
                if (model.text(value.name) == name) return value.id;
            return {};
        };
        const auto producerOf = [&](grhsim::ValueId value) -> const grhsim::SimOp * {
            for (const auto &op : model.operations())
                for (const auto result : model.results(op))
                    if (result == value) return &op;
            return nullptr;
        };
        const auto rootedAt = [&](grhsim::ValueId value, grhsim::ValueId target) {
            for (unsigned hops = 0; hops < 4 && value.valid(); ++hops)
            {
                if (value == target) return true;
                const grhsim::SimOp *producerOp = producerOf(value);
                if (!producerOp) return false;
                const auto type = model.text(producerOp->opType);
                if (type != "core.compute.assign" && type != "core.compute.sliceStatic")
                    return false;
                value = model.operands(*producerOp)[0];
            }
            return false;
        };
        const grhsim::SimOp *write = findOp(model, "wbig");
        if (!write) return fail("mem write vanished");
        const auto writeArgs = model.operands(*write);
        const auto tId = valueNamed("t");
        if (!tId.valid()) return fail("interface alias value vanished");
        if (!rootedAt(writeArgs[1], tId))
            return fail("mem write address lost its interface root");
        const grhsim::SimOp *tProducer = producerOf(tId);
        if (!tProducer || model.text(tProducer->opType) != "core.compute.assign")
            return fail("interface alias was removed by the phase simplify");
        // In-scope CSE merged the two pure-General duplicates; the guarded
        // interface add survived.
        if (opsOfType(model, "core.compute.add").size() != 2)
            return fail("phase-scope CSE removed the wrong adds");
        const auto dupBId = valueNamed("dupB");
        if (!dupBId.valid() || !rootedAt(writeArgs[2], dupBId))
            return fail("mem write data lost its guarded producer");
        if (!verifies(model)) return fail("interface model rejected after phase simplify");
        if (!sealVerifies(model)) return fail("interface model failed the seal");
        return 0;
    }

    int testBoundaryClone()
    {
        // Two-node case: the shared bijection's consumers anchor different
        // predicted nodes, so the clone eliminates the boundary.
        {
            grhsim::GrhSimModel model("split-phases-clone-2node");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto src = addInputRead(model, "src", bit);
            const auto one = addConstant(model, bit, "1'b1");
            const auto q1 = addStateInit(model, "q1", bit, "1'b0");
            const auto q2 = addStateInit(model, "q2", bit, "1'b0");
            const auto shared = addCompute(model, "core.compute.not", {src}, bit, "shared.not");
            const auto d1 = addCompute(model, "core.compute.and", {shared, shared}, bit, "d1");
            const auto d2 = addCompute(model, "core.compute.or", {shared, src}, bit, "d2");
            addRegWrite(model, q1, one, d1, one);
            addRegWrite(model, q2, one, d2, one);
            const auto q1v = addStateRead(model, q1, bit, "q1v");
            addOutputWrite(model, "o1", q1v);
            const auto q2v = addStateRead(model, q2, bit, "q2v");
            addOutputWrite(model, "o2", q2v);

            runPass(model, "grhsim.extract-output-cones");
            runPass(model, "grhsim.split-phases");
            const Messages messages = runPass(model, "grhsim.clone-shared-compute");
            if (infoValue(messages, "grhsim.clone-shared-compute", "cloned=") !=
                std::optional<uint64_t>(2))
                return fail("boundary clone did not clone per consumer");
            if (infoValue(messages, "grhsim.clone-shared-compute", "boundary_values_eliminated=") !=
                std::optional<uint64_t>(1))
                return fail("boundary elimination count wrong");
            if (findOp(model, "shared.not")) return fail("dead shared source survived");
            unsigned localNots = 0;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) != "core.compute.not" ||
                    model.text(op.name).find(".local") == std::string_view::npos)
                    continue;
                ++localNots;
                if (op.phase != grhsim::SimPhase::General)
                    return fail("clone lost the source phase");
            }
            if (localNots != 2) return fail("clone count mismatch");
            if (!verifies(model)) return fail("cloned model rejected");
            if (!sealVerifies(model)) return fail("cloned model failed the seal");
            const Messages again = runPass(model, "grhsim.clone-shared-compute");
            if (!infoHas(again, "grhsim.clone-shared-compute", "cloned=0"))
                return fail("boundary clone is not idempotent");
        }
        // One-node case: both consumers absorb into the same sink node, so
        // the value never crosses a boundary and nothing is cloned.
        {
            grhsim::GrhSimModel model("split-phases-clone-1node");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto x = addInputRead(model, "x", bit);
            const auto one = addConstant(model, bit, "1'b1");
            const auto q = addStateInit(model, "q", bit, "1'b0");
            const auto shared = addCompute(model, "core.compute.not", {x}, bit, "shared.not");
            const auto c1 = addCompute(model, "core.compute.and", {shared, x}, bit, "c1");
            const auto c2 = addCompute(model, "core.compute.or", {shared, x}, bit, "c2");
            const auto data = addCompute(model, "core.compute.xor", {c1, c2}, bit, "data");
            addRegWrite(model, q, one, data, one);
            const auto qv = addStateRead(model, q, bit, "qv");
            addOutputWrite(model, "oQ", qv);

            runPass(model, "grhsim.extract-output-cones");
            runPass(model, "grhsim.split-phases");
            const Messages messages = runPass(model, "grhsim.clone-shared-compute");
            if (!infoHas(messages, "grhsim.clone-shared-compute", "cloned=0"))
                return fail("a node-local shared producer was cloned");
            if (!infoHas(messages, "grhsim.clone-shared-compute", "skipped_local=1"))
                return fail("node-local rejection was not counted");
            if (!infoHas(messages, "grhsim.clone-shared-compute",
                         "idle_reason=no_boundary_candidates"))
                return fail("missing idle reason for a boundary-free model");
            if (!verifies(model)) return fail("node-local model rejected");
        }
        // Mem-consumer case: a Mem-phase write samples the shared value, so
        // cloning would not eliminate the boundary; the pass must skip it.
        {
            grhsim::GrhSimModel model("split-phases-clone-mem");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
            const auto x = addInputRead(model, "x", byte);
            const auto we = addInputRead(model, "we", bit);
            const auto cff = addConstant(model, byte, "8'hff");
            const auto c00 = addConstant(model, addr6, "6'h00");
            const auto q = addStateInit(model, "q", byte, "8'h00");
            const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");
            const auto shared = addCompute(model, "core.compute.not", {x}, byte, "shared.not");
            const auto data = addCompute(model, "core.compute.and", {shared, x}, byte, "data");
            addRegWrite(model, q, we, data, cff);
            addMemWrite(model, bigMem, we, c00, shared, cff);
            const auto qv = addStateRead(model, q, byte, "qv");
            addOutputWrite(model, "oQ", qv);
            const auto bmv = addMemRead(model, bigMem, c00, byte, "bmv");
            addOutputWrite(model, "oM", bmv);

            runPass(model, "grhsim.select-state-stores");
            runPass(model, "grhsim.extract-output-cones");
            runPass(model, "grhsim.split-phases");
            const Messages messages = runPass(model, "grhsim.clone-shared-compute");
            if (!infoHas(messages, "grhsim.clone-shared-compute", "cloned=0"))
                return fail("a value sampled by P_mem was cloned away");
            if (!infoHas(messages, "grhsim.clone-shared-compute",
                         "skipped_non_compute_consumer=1"))
                return fail("mem-consumer rejection was not counted");
            if (!verifies(model)) return fail("mem-consumer model rejected");
            if (!sealVerifies(model)) return fail("mem-consumer model failed the seal");
        }
        // Unattributed model: no General ops, the pass is a documented no-op.
        {
            grhsim::GrhSimModel model("split-phases-clone-idle");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
            const auto x = addInputRead(model, "x", bit);
            const auto shared = addCompute(model, "core.compute.not", {x}, bit, "shared.not");
            addCompute(model, "core.compute.and", {shared, x}, bit, "c1");
            addCompute(model, "core.compute.or", {shared, x}, bit, "c2");
            const Messages messages = runPass(model, "grhsim.clone-shared-compute");
            if (!infoHas(messages, "grhsim.clone-shared-compute", "idle_reason=no_general_ops"))
                return fail("unattributed model did not report the idle reason");
        }
        if (!passCreationFails("grhsim.clone-shared-compute",
                               std::array<std::string_view, 2>{"--max-op-in-compute-node", "0"}))
            return fail("clone-shared-compute accepted a zero node cap");
        return 0;
    }

    int testBoundaryPredictionConsistency()
    {
        // Dedicated fixture without regLatch-class arrays: no General-phase
        // mem writes exist (bigMem is 64 bytes and lands in the mem store
        // class), so the predictor and C1 form nodes over the same General
        // op set.
        grhsim::GrhSimModel model("split-phases-predict");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
        const auto addr6 = model.logicType(6, false, grhsim::LogicDomain::TwoState);
        const auto a = addInputRead(model, "a", byte);
        const auto b = addInputRead(model, "b", byte);
        const auto c = addInputRead(model, "c", byte);
        const auto we = addInputRead(model, "we", bit);
        const auto cff = addConstant(model, byte, "8'hff");
        const auto c00 = addConstant(model, addr6, "6'h00");
        const auto q1 = addStateInit(model, "q1", byte, "8'h00");
        const auto q2 = addStateInit(model, "q2", byte, "8'h00");
        const auto q3 = addStateInit(model, "q3", byte, "8'h00");
        const auto bigMem = addArrayState(model, "bigMem", byte, 64, "8'h00");
        // s1 crosses two write cones and is also sampled by the Mem write.
        const auto s1 = addCompute(model, "core.compute.xor", {a, b}, byte, "s1");
        const auto s2 = addCompute(model, "core.compute.and", {s1, c}, byte, "s2");
        addRegWrite(model, q1, we, s2, cff);
        const auto s3 = addCompute(model, "core.compute.or", {s1, c}, byte, "s3");
        addRegWrite(model, q2, we, s3, cff);
        // Absorbed single-consumer chain: no boundary between t1 and t2.
        const auto t1 = addCompute(model, "core.compute.not", {c}, byte, "t1");
        const auto t2 = addCompute(model, "core.compute.xor", {t1, a}, byte, "t2");
        addRegWrite(model, q3, we, t2, cff);
        const auto q1r = addStateRead(model, q1, byte, "q1r");
        const auto q3r = addStateRead(model, q3, byte, "q3r");
        const auto q2r = addStateRead(model, q2, byte, "q2r");
        addMemWrite(model, bigMem, we, s1, q1r, cff);
        addOutputWrite(model, "o1", addCompute(model, "core.compute.xor", {q1r, q2r}, byte, "o1v"));
        addOutputWrite(model, "o2",
                       addCompute(model, "core.compute.add",
                                  {addMemRead(model, bigMem, c00, byte, "bmv"), q3r}, byte, "o2v"));

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        runPass(model, "grhsim.split-phases");
        runPass(model, "grhsim.simplify", std::array<std::string_view, 2>{"--scope", "phase"});
        // C1 forms nodes over the sealed General partition (M5d-6).
        runPass(model, "cpu.st.build-general-nodes");

        const auto *mapping = model.cpuMapping();
        if (!mapping) return fail("cpu mapping missing");
        const auto &tree = mapping->partitionTree;
        std::vector<uint32_t> opNode(model.operations().size() + 1, 0);
        for (const auto branchId : tree.partitions[tree.root.index - 1].children)
        {
            const auto &branch = tree.partitions[branchId.index - 1];
            if (branch.attrs.phase != grhsim::CpuPhase::General) continue;
            for (const auto nodeId : branch.children)
                for (const auto opId : tree.partitions[nodeId.index - 1].ops)
                    opNode[opId.index] = nodeId.index;
        }
        std::vector<grhsim::OpId> producer(model.values().size() + 1);
        for (const auto &op : model.operations())
            for (const auto value : model.results(op)) producer[value.index] = op.id;
        std::set<uint32_t> actual;
        for (const auto &op : model.operations())
        {
            const bool memConsumer = op.phase == grhsim::SimPhase::Mem;
            for (const auto operand : model.operands(op))
            {
                const auto source = producer[operand.index];
                if (!source || !opNode[source.index]) continue;
                if (memConsumer || (opNode[op.id.index] && opNode[op.id.index] != opNode[source.index]))
                    actual.insert(operand.index);
            }
        }
        const auto prediction = grhsim::predictGeneralBoundaries(model, 128);
        std::set<uint32_t> predicted;
        for (std::size_t i = 1; i < prediction.boundaryValue.size(); ++i)
            if (prediction.boundaryValue[i]) predicted.insert(i);
        if (prediction.nodeCount == 0) return fail("prediction formed no nodes");
        if (predicted != actual)
        {
            std::cerr << "[grhsim-split-phases] predicted-only:";
            for (const auto index : predicted)
                if (!actual.count(index)) std::cerr << ' ' << index;
            std::cerr << " actual-only:";
            for (const auto index : actual)
                if (!predicted.count(index)) std::cerr << ' ' << index;
            std::cerr << '\n';
            return fail("predicted boundaries disagree with build-general-nodes");
        }
        return 0;
    }

    int testPartitionEquivalence()
    {
        auto model = EventFixture::build();
        const auto reference = EventFixture::trace(model, 64);
        runPass(model, "grhsim.select-state-stores");
        // Lowering stage (B1-B4) equivalence: raw event_edges vs event_acts.
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        const auto lowered = EventFixture::trace(model, 64);
        if (lowered != reference) return fail("B1-B4 changed the interpreter trace");
        // Partition stage (B5-B8): attribution, per-partition simplify and
        // boundary-aware cloning preserve the trace.
        const Messages split = runPass(model, "grhsim.split-phases");
        if (infoValue(split, "grhsim.split-phases", "phase_mem=") != std::optional<uint64_t>(1))
            return fail("bigMem write was not attributed to P_mem");
        if (infoValue(split, "grhsim.split-phases", "mem_writes_reglatch=") !=
            std::optional<uint64_t>(1))
            return fail("smallMem write was not kept on the General path");
        runPass(model, "grhsim.simplify", std::array<std::string_view, 2>{"--scope", "phase"});
        runPass(model, "grhsim.clone-shared-compute");
        runPass(model, "grhsim.verify", std::array<std::string_view, 2>{"--seal", "semantic"});
        const auto sealed = EventFixture::trace(model, 64);
        if (sealed != reference) return fail("B5-B8 changed the interpreter trace");
        const std::string stored = storeJson(model);
        std::istringstream input(stored);
        diag::Diagnostics diagnostics;
        auto loaded = grhsim::readGrhSimJson(input, grhsim::defaultDialectRegistry(), diagnostics);
        if (!loaded || diagnostics.hasError()) return fail("sealed event model does not load");
        if (storeJson(*loaded) != stored) return fail("sealed event model is not byte-stable");
        return 0;
    }

    int testTimeslotLifecycle()
    {
        grhsim::GrhSimModel model("split-phases-tslot");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, grhsim::LogicDomain::TwoState);
        const auto cond = addConstant(model, bit, "1'h1");
        const auto data = addInputRead(model, "din", byte);
        addOutputWrite(model, "dout", data);
        // A pre-existing state so classification is non-vacuous when the
        // timeslot migration adds its prev states.
        const auto mask8 = addConstant(model, byte, "8'hff");
        const auto q = addStateInit(model, "q", byte, "8'h0");
        model.addOperation("core.state.latchWrite", std::array{cond, data, mask8}, {},
                           std::array{grhsim::ObjectRef::state(q)});
        addOutputWrite(model, "oq", addStateRead(model, q, byte, "qv"));
        {
            const std::array params{
                grhsim::Parameter{model.intern("name"), std::string("monitor")},
                grhsim::Parameter{model.intern("proc_kind"), std::string("always")},
                grhsim::Parameter{model.intern("has_timing"), false}};
            model.addOperation("core.system.task", std::array{cond, data}, {}, {}, params);
        }

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.classify-event-inputs");
        runPass(model, "grhsim.lower-edge-detect");
        runPass(model, "grhsim.extract-output-cones");
        runPass(model, "grhsim.migrate-timeslot-tasks");
        std::size_t prevStates = 0;
        for (const auto &state : model.states())
            if (model.text(state.name).starts_with("__tslot_prev_")) ++prevStates;
        if (prevStates == 0) return fail("timeslot migration created no prev states");

        runPass(model, "grhsim.split-phases");
        runPass(model, "grhsim.simplify", std::array<std::string_view, 2>{"--scope", "phase"});
        runPass(model, "grhsim.clone-shared-compute");
        runPass(model, "grhsim.verify", std::array<std::string_view, 2>{"--seal", "semantic"});

        // The monitoring states keep their regLatch class and their
        // Output-phase latchWrite commit through the whole partition stage.
        std::size_t kept = 0;
        for (const auto &state : model.states())
        {
            if (!model.text(state.name).starts_with("__tslot_prev_")) continue;
            ++kept;
            if (state.storeClass != grhsim::StateStoreClass::RegLatch)
                return fail("timeslot prev state lost its regLatch class");
            bool outputLatchWrite = false;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) != "core.state.latchWrite") continue;
                const auto refs = model.objectRefs(op);
                if (refs.empty() || refs[0].index != state.id.index) continue;
                outputLatchWrite = outputLatchWrite || op.phase == grhsim::SimPhase::Output;
            }
            if (!outputLatchWrite)
                return fail("timeslot prev state lost its Output-phase write-back");
        }
        if (kept != prevStates) return fail("partition stage dropped a timeslot prev state");
        if (!verifies(model)) return fail("timeslot model rejected after the partition stage");
        const std::string stored = storeJson(model);
        std::istringstream input(stored);
        diag::Diagnostics diagnostics;
        if (!grhsim::readGrhSimJson(input, grhsim::defaultDialectRegistry(), diagnostics) ||
            diagnostics.hasError())
            return fail("timeslot model does not load");
        return 0;
    }
} // namespace

int main()
{
    try
    {
        if (const int status = testAttribution()) return status;
        if (const int status = testClassAwareVerifier()) return status;
        if (const int status = testSemanticSeal()) return status;
        if (const int status = testPhaseScopeInterface()) return status;
        if (const int status = testBoundaryClone()) return status;
        if (const int status = testBoundaryPredictionConsistency()) return status;
        if (const int status = testPartitionEquivalence()) return status;
        if (const int status = testTimeslotLifecycle()) return status;
    }
    catch (const std::exception &ex)
    {
        return fail(ex.what());
    }
    std::cout << "grhsim-split-phases tests passed\n";
    return 0;
}
