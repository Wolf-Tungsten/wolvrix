#include "grhsim/backend/cpu.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace wolvrix::lib;
    using namespace grhsim;

    void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    using Messages = std::vector<std::pair<std::string, std::string>>;

    Messages runPass(GrhSimModel &model, std::string_view name,
                     std::span<const std::string_view> args = {})
    {
        std::string error;
        auto pass = defaultPassRegistry().create(name, args, error);
        require(bool(pass), error);
        PassManager manager(defaultDialectRegistry());
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics;
        auto result = manager.run(model, diagnostics);
        Messages messages;
        for (const auto &message : diagnostics.messages())
            messages.emplace_back(message.context, message.message);
        for (const auto &[context, message] : messages)
            std::cout << context << ": " << message << '\n';
        require(result.success && result.changed && !model.poisoned(),
                "six-phase store/schedule pass failed: " + std::string(name));
        return messages;
    }

    bool verifies(const GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        return verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics) &&
               !diagnostics.hasError();
    }

    void roundTrip(const GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        std::ostringstream first, second;
        require(writeGrhSimJson(model, first, defaultDialectRegistry(), diagnostics),
                "six-phase store stage store failed");
        std::istringstream input(first.str());
        auto loaded = readGrhSimJson(input, defaultDialectRegistry(), diagnostics);
        if (!loaded)
            for (const auto &message : diagnostics.messages()) std::cerr << message.message << '\n';
        require(bool(loaded), "six-phase store stage load failed");
        require(writeGrhSimJson(*loaded, second, defaultDialectRegistry(), diagnostics) &&
                first.str() == second.str(), "six-phase store stage round trip changed bytes");
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

    ValueId addConstant(GrhSimModel &model, TypeId type, std::string literal)
    {
        const auto value = model.addValue(type);
        const std::array params{Parameter{model.intern("constValue"), std::move(literal)}};
        model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
        return value;
    }

    StateId addState(GrhSimModel &model, const char *name, TypeId type, std::string initLiteral)
    {
        const auto id = model.addState(name, type);
        const std::array initParams{Parameter{model.intern("value"), std::move(initLiteral)}};
        const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(id, steps, initParams);
        return id;
    }

    ValueId addNot(GrhSimModel &model, ValueId operand, std::string_view name)
    {
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto value = model.addValue(bit, name);
        model.addOperation("core.compute.not", std::array{operand}, std::array{value});
        return value;
    }

    ValueId addAnd(GrhSimModel &model, ValueId lhs, ValueId rhs, std::string_view name)
    {
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto value = model.addValue(bit, name);
        model.addOperation("core.compute.and", std::array{lhs, rhs}, std::array{value});
        return value;
    }

    // input.read against an existing port; empty valueName leaves the result
    // unnamed (the layout pass falls back to the readR_<id> shape).
    ValueId addReadOp(GrhSimModel &model, InputId port, std::string_view valueName,
                      SimPhase phase, bool eventOnly)
    {
        const auto type = model.inputs()[port.index - 1].type;
        const auto value = valueName.empty() ? model.addValue(type)
                                             : model.addValue(type, valueName);
        OpId id;
        if (eventOnly)
        {
            const std::array params{Parameter{model.intern("event_only"), true}};
            id = model.addOperation("core.input.read", {}, std::array{value},
                                    std::array{ObjectRef::input(port)}, params);
        }
        else
        {
            id = model.addOperation("core.input.read", {}, std::array{value},
                                    std::array{ObjectRef::input(port)});
        }
        if (phase != SimPhase::None) model.setOperationPhase(id, phase);
        return value;
    }

    OpId producerOf(const GrhSimModel &model, ValueId value)
    {
        for (const auto &op : model.operations())
            for (auto result : model.results(op))
                if (result == value) return op.id;
        throw std::runtime_error("value has no producer");
    }

    OpId addEdgeDet(GrhSimModel &model, ValueId event, int64_t cluster,
                    std::string edge = "posedge")
    {
        const std::array params{Parameter{model.intern("edge"), std::move(edge)},
                                Parameter{model.intern("act"), cluster},
                                Parameter{model.intern("prev"), cluster},
                                Parameter{model.intern("prevInit"), std::string("1'b0")}};
        const auto id = model.addOperation("core.event.edgeDet", std::array{event}, {}, {}, params);
        model.setOperationPhase(id, SimPhase::Event);
        return id;
    }

    OpId addRegWrite(GrhSimModel &model, ValueId cond, ValueId next, ValueId mask,
                     StateId target, std::vector<int64_t> acts)
    {
        const std::array operands{cond, next, mask};
        const std::array refs{ObjectRef::state(target)};
        OpId id;
        if (acts.empty()) id = model.addOperation("core.state.regWrite", operands, {}, refs);
        else
        {
            const std::array params{Parameter{model.intern("event_acts"), std::move(acts)}};
            id = model.addOperation("core.state.regWrite", operands, {}, refs, params);
        }
        model.setOperationPhase(id, SimPhase::General);
        return id;
    }

    ValueId addStateRead(GrhSimModel &model, StateId state, std::string_view name)
    {
        const auto value = model.addValue(model.states()[state.index - 1].type, name);
        model.addOperation("core.state.read", {}, std::array{value},
                           std::array{ObjectRef::state(state)});
        return value;
    }

    const CpuPartition &branch(const CpuBackendMapping &mapping, CpuPhase phase)
    {
        const auto &tree = mapping.partitionTree;
        for (auto id : tree.partitions[tree.root.index - 1].children)
        {
            const auto &partition = tree.partitions[id.index - 1];
            if (partition.attrs.phase == phase) return partition;
        }
        throw std::runtime_error("missing six-phase branch");
    }

    std::vector<OpId> flatOps(const CpuBackendMapping &mapping, PartitionId id)
    {
        const auto &tree = mapping.partitionTree;
        std::vector<OpId> result;
        std::vector<PartitionId> stack{id};
        while (!stack.empty())
        {
            const auto &partition = tree.partitions[stack.back().index - 1];
            stack.pop_back();
            result.insert(result.end(), partition.ops.begin(), partition.ops.end());
            stack.insert(stack.end(), partition.children.rbegin(), partition.children.rend());
        }
        return result;
    }

    std::vector<const CpuPartition *> supernodes(const CpuBackendMapping &mapping)
    {
        std::vector<const CpuPartition *> result;
        for (const auto &partition : mapping.partitionTree.partitions)
            if (partition.attrs.kind == CpuPartitionKind::Supernode) result.push_back(&partition);
        return result;
    }

    const CpuPartition *supernodeOf(const CpuBackendMapping &mapping, OpId op)
    {
        for (const auto *supernode : supernodes(mapping))
            for (auto opId : flatOps(mapping, supernode->id))
                if (opId == op) return supernode;
        return nullptr;
    }

    // The supernode ordinal (M5d-6, resolution 2): the General branch's
    // Supernode children in tree order — fixed at C2; the trailing
    // EmitFunction leaves (C6) are skipped.
    uint32_t supernodeOrdinal(const CpuBackendMapping &mapping, PartitionId target)
    {
        const auto &tree = mapping.partitionTree;
        uint32_t ordinal = 0;
        for (const auto child : branch(mapping, CpuPhase::General).children)
        {
            if (tree.partitions[child.index - 1].attrs.kind != CpuPartitionKind::Supernode) continue;
            if (child == target) return ordinal;
            ++ordinal;
        }
        throw std::runtime_error("supernode not in the general branch order");
    }

    bool bitOf(const CpuEventActivation &entry, uint32_t ordinal)
    {
        return (entry.supernodeWords[ordinal / 64] >> (ordinal % 64)) & uint64_t{1};
    }

    const CpuNamedStore &store(const CpuBackendMapping &mapping, CpuNamedStoreKind kind)
    {
        for (const auto &entry : mapping.dataLayout->namedStores.value())
            if (entry.kind == kind) return entry;
        throw std::runtime_error("named store missing");
    }

    const CpuStoreField *findField(const GrhSimModel &model, const CpuNamedStore &store,
                                   std::string_view name)
    {
        for (const auto &field : store.fields)
            if (model.text(field.name) == name) return &field;
        return nullptr;
    }

    const CpuFanoutEntry<ValueId> *findValueFanout(
        const std::vector<CpuFanoutEntry<ValueId>> &fanout, ValueId source)
    {
        for (const auto &entry : fanout)
            if (entry.source == source) return &entry;
        return nullptr;
    }

    const CpuFanoutEntry<StateId> *findStateFanout(
        const std::vector<CpuFanoutEntry<StateId>> &fanout, StateId source)
    {
        for (const auto &entry : fanout)
            if (entry.source == source) return &entry;
        return nullptr;
    }

    // M5d-6 C segment: classify (all arrays mem — these fixtures pin the P_mem
    // write-plan shape), attribute (B5), then C1 init + C2 merge.
    void runMappingPipeline(GrhSimModel &model)
    {
        runPass(model, "grhsim.select-state-stores",
                std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        runPass(model, "grhsim.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
    }

    void runSchedulePipeline(GrhSimModel &model)
    {
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-activation-map");
        runPass(model, "cpu.st.build-mem-write-plan");
        runPass(model, "cpu.st.pack-general-functions");
        runPass(model, "cpu.st.build-phase-schedule");
    }

    // Shared six-phase design: clk0/clk1 posedge domains (acts 0/1) plus a
    // dual-use negedge reset (act 2) that also feeds data logic, one memory
    // with two writes (static/dynamic address readers) and an event-driven
    // $strobe timeslot task.
    struct MainFixture
    {
        GrhSimModel model;
        ValueId clk0v, clk1v, rstEv, clk1Gv, rstDv, d1v, dshv, mdv;
        ValueId shnv, x1v, q0rv, row3;
        StateId q0, q1, q3, q4, q5, m1;
        OpId w0, w1, w3, w4, w5, mw0, mw1, mr0, mr1, strobe;

        MainFixture() : model("m4_main")
        {
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto bit4 = model.logicType(4, false, LogicDomain::TwoState);
            const auto memType = model.arrayType(bit, 16);
            const auto clk0 = model.addInput("clk0", bit);
            const auto clk1 = model.addInput("clk1", bit);
            const auto rst = model.addInput("rst", bit);
            const auto d1 = model.addInput("d1", bit);
            const auto dsh = model.addInput("dsh", bit);
            const auto md = model.addInput("md", bit);
            // Event-phase reads (the P_event clones) and their detectors.
            clk0v = addReadOp(model, clk0, "clk0", SimPhase::Event, true);
            clk1v = addReadOp(model, clk1, "clk1", SimPhase::Event, true);
            rstEv = addReadOp(model, rst, "rst", SimPhase::Event, true);
            addEdgeDet(model, clk0v, 0);
            addEdgeDet(model, clk1v, 1);
            addEdgeDet(model, rstEv, 2, "negedge");
            // Leftover General-phase event-only clk1 read (no data use): the
            // input fanout must skip it.
            clk1Gv = addReadOp(model, clk1, "clk1_g", SimPhase::General, true);
            // Dual-use reset: a second, unmarked General read feeds data logic.
            rstDv = addReadOp(model, rst, "rst_d", SimPhase::None, false);
            d1v = addReadOp(model, d1, "d1", SimPhase::None, false);
            dshv = addReadOp(model, dsh, "dsh", SimPhase::None, false);
            mdv = addReadOp(model, md, "md", SimPhase::None, false);

            const auto one0 = addConstant(model, bit, "1'b1");
            const auto one1 = addConstant(model, bit, "1'b1");
            const auto one3 = addConstant(model, bit, "1'b1");
            const auto one4 = addConstant(model, bit, "1'b1");
            const auto one5 = addConstant(model, bit, "1'b1");
            const auto mw0e = addConstant(model, bit, "1'b1");
            const auto mw0m = addConstant(model, bit, "1'b1");
            const auto mw1e = addConstant(model, bit, "1'b1");
            const auto mw1d = addConstant(model, bit, "1'b1");
            const auto mw1m = addConstant(model, bit, "1'b1");
            row3 = addConstant(model, bit4, "4'h3");

            q0 = addState(model, "q0", bit, "1'b0");
            q1 = addState(model, "q1", bit, "1'b0");
            q3 = addState(model, "q3", bit, "1'b0");
            q4 = addState(model, "q4", bit, "1'b0");
            q5 = addState(model, "q5", bit, "1'b0");
            m1 = addState(model, "m1", memType, "0");

            // Shared cone feeding domains {0,2} and {1}: it stays a standalone
            // supernode (M3 rule 2) and its result is a boundary value.
            shnv = addNot(model, dshv, "shn");
            const auto c0 = addAnd(model, rstDv, shnv, "c0");
            w0 = addRegWrite(model, c0, shnv, one0, q0, {0, 2});
            x1v = addNot(model, d1v, "x1");
            const auto x1g = addAnd(model, x1v, shnv, "x1g");
            w1 = addRegWrite(model, one1, x1g, one1, q1, {1});
            // Mem readers: one static address (4'h3), one dynamic (x1).
            const auto mrv0 = model.addValue(bit, "mrv0");
            {
                const std::array refs{ObjectRef::state(m1)};
                mr0 = model.addOperation("core.state.memRead", std::array{row3},
                                         std::array{mrv0}, refs);
            }
            w3 = addRegWrite(model, one3, mrv0, one3, q3, {0});
            const auto mrv1 = model.addValue(bit, "mrv1");
            {
                const std::array refs{ObjectRef::state(m1)};
                mr1 = model.addOperation("core.state.memRead", std::array{x1v},
                                         std::array{mrv1}, refs);
            }
            w4 = addRegWrite(model, one4, mrv1, one4, q4, {1});
            // State reader: publish fanout for q0.
            q0rv = addStateRead(model, q0, "q0r");
            const auto y = addNot(model, q0rv, "y");
            w5 = addRegWrite(model, one5, y, one5, q5, {0});
            // Two writes into m1: per-mem static priority follows op-id order.
            {
                const std::array refs{ObjectRef::state(m1)};
                const std::array params{
                    Parameter{model.intern("event_acts"), std::vector<int64_t>{0}}};
                mw0 = model.addOperation("core.state.memWrite",
                                         std::array{mw0e, row3, mdv, mw0m}, {}, refs, params);
                mw1 = model.addOperation("core.state.memWrite",
                                         std::array{mw1e, x1v, mw1d, mw1m}, {}, refs);
            }
            // Timeslot task in P_output (the M2b migrated shape).
            const auto oneO = model.addValue(bit);
            OpId cond;
            {
                const std::array params{Parameter{model.intern("constValue"),
                                                  std::string("1'b1")}};
                cond = model.addOperation("core.compute.constant", {}, std::array{oneO},
                                          {}, params);
                model.setOperationPhase(cond, SimPhase::Output);
            }
            {
                const std::array params{
                    Parameter{model.intern("name"), std::string("strobe")},
                    Parameter{model.intern("proc_kind"), std::string("always")},
                    Parameter{model.intern("has_timing"), false},
                    Parameter{model.intern("event_acts"), std::vector<int64_t>{0, 1}},
                    Parameter{model.intern("timeslotFlag"), int64_t{0}}};
                strobe = model.addOperation("core.system.task", std::array{oneO}, {}, {}, params);
                model.setOperationPhase(strobe, SimPhase::Output);
            }
            (void)cond;
        }
    };
}

namespace
{
    // cpu.st.layout-named-stores: the seven stores in fixed order, mem write
    // operand slot names, prevEvent/eventAct cluster naming with uniquified
    // act bits, and the activeFlags byte arrays sized by the supernode count.
    void namedStoreLayoutTest()
    {
        MainFixture fixture;
        auto &model = fixture.model;
        require(verifies(model), "layout: fixture rejected");
        runMappingPipeline(model);
        const auto messages = runPass(model, "cpu.st.layout-named-stores");
        // One rename per eventAct bit (clk0__posedge/clk1__posedge/rst__negedge
        // were already claimed by the prevEvent slots).
        require(infoValue(messages, "cpu.st.layout-named-stores", "renamed_count=") ==
                std::optional<uint64_t>(3), "layout: renamed count wrong");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::LayoutNamedStores, "layout: stage wrong");
        require(mapping.dataLayout && mapping.dataLayout->namedStores, "layout: stores missing");
        const auto &layout = *mapping.dataLayout;
        // M5d-6: the layout payload is just the type table plus named stores
        // (the legacy arenas are gone).
        const std::array<CpuNamedStoreKind, 7> kinds{
            CpuNamedStoreKind::RegLatch, CpuNamedStoreKind::Mem, CpuNamedStoreKind::Boundary,
            CpuNamedStoreKind::PrevEvent, CpuNamedStoreKind::EventAct,
            CpuNamedStoreKind::TimeslotTrigger, CpuNamedStoreKind::ActiveFlags};
        require(layout.namedStores->size() == kinds.size(), "layout: store count wrong");
        for (std::size_t i = 0; i < kinds.size(); ++i)
            require(layout.namedStores->at(i).kind == kinds[i], "layout: store order wrong");

        // regLatch: one Bool field per non-array state; mem: one array field.
        const auto &regLatch = store(mapping, CpuNamedStoreKind::RegLatch);
        require(regLatch.fields.size() == 5, "layout: reg latch field count wrong");
        for (const char *name : {"q0", "q1", "q3", "q4", "q5"})
        {
            const auto *field = findField(model, regLatch, name);
            require(field && field->state && !field->value, "layout: reg latch field missing");
            require(layout.types[field->type.index - 1].kind == CpuTypeKind::Bool,
                    "layout: reg latch field type wrong");
        }
        const auto &mem = store(mapping, CpuNamedStoreKind::Mem);
        require(mem.fields.size() == 1, "layout: mem field count wrong");
        const auto *m1Field = findField(model, mem, "m1");
        require(m1Field && m1Field->state == fixture.m1, "layout: mem field wrong");
        require(layout.types[m1Field->type.index - 1].kind == CpuTypeKind::Array &&
                layout.types[m1Field->type.index - 1].count == 16, "layout: mem field type wrong");

        // boundary: six input ports (aux = port index) plus the
        // cross-supernode / mem-operand value set with dedicated slot names.
        // V2-M1: sink operands cross supernode boundaries by construction
        // (writes sit in their own sink supernodes), so the write cones'
        // enable/data/mask values hold boundary slots too.
        const auto &boundary = store(mapping, CpuNamedStoreKind::Boundary);
        require(boundary.fields.size() == 25, "layout: boundary field count wrong");
        const std::array<std::string_view, 6> ports{"clk0", "clk1", "rst", "d1", "dsh", "md"};
        for (uint32_t i = 0; i < ports.size(); ++i)
        {
            const auto *field = findField(model, boundary, ports[i]);
            require(field && !field->value && field->aux == i,
                    "layout: boundary port field wrong");
        }
        for (const char *name : {"shn", "m1__w0__enable", "m1__w0__addr", "m1__w0__data",
                                 "m1__w0__mask", "m1__w1__enable", "m1__w1__addr",
                                 "m1__w1__data", "m1__w1__mask"})
            require(findField(model, boundary, name), "layout: boundary value field missing");
        for (const char *name : {"c0", "x1g", "mrv0", "mrv1", "y"})
            require(findField(model, boundary, name), "layout: sink operand boundary field missing");
        require(findField(model, boundary, "m1__w0__data")->value == fixture.mdv,
                "layout: mem data slot value wrong");
        require(findField(model, boundary, "m1__w0__addr")->value == fixture.row3,
                "layout: mem addr slot value wrong");
        require(findField(model, boundary, "shn")->value == fixture.shnv,
                "layout: shared cone slot value wrong");

        // prevEvent: one slot per cluster, named <signal>__<edge>.
        const auto &prevEvent = store(mapping, CpuNamedStoreKind::PrevEvent);
        require(prevEvent.fields.size() == 3, "layout: prev event field count wrong");
        const std::array<std::string_view, 3> prevNames{"clk0__posedge", "clk1__posedge",
                                                        "rst__negedge"};
        const std::array<ValueId, 3> prevValues{fixture.clk0v, fixture.clk1v, fixture.rstEv};
        for (uint32_t i = 0; i < prevNames.size(); ++i)
        {
            const auto *field = findField(model, prevEvent, prevNames[i]);
            require(field && field->aux == i && field->value == prevValues[i] && !field->state,
                    "layout: prev event field wrong");
        }
        // eventAct: one byte-packed Bool bit per cluster, same base name
        // uniquified with the event value id.
        const auto &eventAct = store(mapping, CpuNamedStoreKind::EventAct);
        require(eventAct.fields.size() == 3 && eventAct.sizeBytes == 1,
                "layout: event act store shape wrong");
        std::vector<bool> seenActs(3, false);
        for (const auto &field : eventAct.fields)
        {
            require(field.aux < 3 && !seenActs[field.aux] && !field.state && !field.value,
                    "layout: event act field cluster wrong");
            seenActs[field.aux] = true;
            require(field.offset == field.aux / 8 &&
                    layout.types[field.type.index - 1].kind == CpuTypeKind::Bool,
                    "layout: event act field layout wrong");
            const auto expected = std::string(prevNames[field.aux]) + "_" +
                                  std::to_string(prevValues[field.aux].index);
            require(model.text(field.name) == expected, "layout: event act field name wrong");
        }

        // timeslotTrigger: one Bool byte per event-carrying timeslot task.
        const auto &timeslot = store(mapping, CpuNamedStoreKind::TimeslotTrigger);
        require(timeslot.fields.size() == 1 && timeslot.sizeBytes == 1,
                "layout: timeslot trigger shape wrong");
        require(timeslot.fields.front().aux == 0 && timeslot.fields.front().offset == 0 &&
                model.text(timeslot.fields.front().name) == "strobe",
                "layout: timeslot trigger field wrong");

        // activeFlags: two byte arrays with one byte per General supernode
        // (V2-M2 removed eventActiveFlag).
        const auto supernodeCount = supernodes(mapping).size();
        require(supernodeCount > 0, "layout: no supernodes");
        const auto &active = store(mapping, CpuNamedStoreKind::ActiveFlags);
        const std::array<std::string_view, 2> activeNames{"dataActiveFlag", "dataActiveFlagNext"};
        require(active.fields.size() == activeNames.size(), "layout: active flags count wrong");
        for (std::size_t i = 0; i < activeNames.size(); ++i)
        {
            const auto &field = active.fields[i];
            require(model.text(field.name) == activeNames[i] &&
                    field.aux == supernodeCount && field.offset == i * supernodeCount,
                    "layout: active flags field wrong");
            const auto &type = layout.types[field.type.index - 1];
            require(type.kind == CpuTypeKind::Array && type.count == supernodeCount,
                    "layout: active flags field type wrong");
        }
        require(active.sizeBytes == 2 * supernodeCount, "layout: active flags size wrong");
        roundTrip(model);
    }

    // Naming rules: declaredSymbol sanitizing (hierarchy separators, illegal
    // characters, C++ keywords, leading digits), cross-store uniqueness, and
    // the op-category fallback for unnamed values.
    void namingRulesTest()
    {
        GrhSimModel model("m4_names");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto bit2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto bit8 = model.logicType(8, false, LogicDomain::TwoState);
        const auto clkA = model.addInput("clkA", bit);
        const auto clkB = model.addInput("clkB", bit);
        const auto rin = model.addInput("rin", bit);
        const auto weird = model.addInput("we ird", bit);
        const auto wide = model.addInput("wide", bit8);
        const auto dup = model.addInput("dup", bit);
        const auto clkAv = addReadOp(model, clkA, "clkA", SimPhase::Event, true);
        const auto clkBv = addReadOp(model, clkB, "clkB", SimPhase::Event, true);
        addEdgeDet(model, clkAv, 0);
        addEdgeDet(model, clkBv, 1);
        // Unnamed values whose names fall back to the op-category prefix.
        const auto rv = addReadOp(model, rin, "", SimPhase::None, false);
        const auto nv = model.addValue(bit);
        model.addOperation("core.compute.not", std::array{rv}, std::array{nv});
        const auto wv = addReadOp(model, wide, "", SimPhase::None, false);
        const auto sv = model.addValue(bit2);
        {
            const std::array params{Parameter{model.intern("sliceStart"), int64_t{2}},
                                    Parameter{model.intern("sliceEnd"), int64_t{3}}};
            model.addOperation("core.compute.sliceStatic", std::array{wv}, std::array{sv},
                               {}, params);
        }
        const auto wirv = addReadOp(model, weird, "", SimPhase::None, false);
        const auto one = addConstant(model, bit, "1'b1");
        const auto two = addConstant(model, bit2, "2'b11");
        addState(model, "top.dut.q", bit, "1'b0");
        addState(model, "int", bit, "1'b0");
        addState(model, "3x", bit, "1'b0");
        addState(model, "dup", bit, "1'b0");
        const auto qA = addState(model, "qA", bit, "1'b0");
        const auto qB = addState(model, "qB", bit, "1'b0");
        const auto qC = addState(model, "qC", bit, "1'b0");
        const auto qD = addState(model, "qD", bit, "1'b0");
        const auto qA2 = addState(model, "qA2", bit2, "2'b00");
        const auto qB2 = addState(model, "qB2", bit2, "2'b00");
        addRegWrite(model, one, nv, one, qA, {0});
        addRegWrite(model, one, nv, one, qB, {1});
        addRegWrite(model, one, rv, one, qC, {1});
        addRegWrite(model, one, wirv, one, qD, {0});
        addRegWrite(model, one, sv, two, qA2, {0});
        addRegWrite(model, one, sv, two, qB2, {1});
        require(verifies(model), "naming: fixture rejected");

        runMappingPipeline(model);
        const auto messages = runPass(model, "cpu.st.layout-named-stores");
        // Two eventAct renames plus the dup port (the state claimed "dup").
        require(infoValue(messages, "cpu.st.layout-named-stores", "renamed_count=") ==
                std::optional<uint64_t>(3), "naming: renamed count wrong");
        const auto &mapping = *model.cpuMapping();
        const auto &regLatch = store(mapping, CpuNamedStoreKind::RegLatch);
        for (const char *name : {"top_dut_q", "int_", "_3x", "dup"})
            require(findField(model, regLatch, name), "naming: sanitized state name missing");
        const auto &boundary = store(mapping, CpuNamedStoreKind::Boundary);
        require(findField(model, boundary, "we_ird"), "naming: sanitized port name missing");
        require(findField(model, boundary, "dup_" + std::to_string(dup.index)),
                "naming: cross-store name was not uniquified");
        require(findField(model, boundary, "not_" + std::to_string(nv.index)),
                "naming: unnamed not fallback missing");
        require(findField(model, boundary, "readR_" + std::to_string(rv.index)),
                "naming: unnamed input read fallback missing");
        require(findField(model, boundary, "slice_" + std::to_string(sv.index)),
                "naming: unnamed slice fallback missing");
        roundTrip(model);
    }

    // cpu.st.build-event-activation-map (V2-M2): the map covers only non-sink
    // supernodes holding event-carrying ops. In the MainFixture every event
    // carrier is a sink op (the {0,2}/{1} reg writes anchor SinkEvent
    // clusters; the {0} mem write is a Mem-phase op outside the supernode
    // space), so the map is empty — sink supernodes gate on their own
    // eventActStore signatures and need no P_event activation.
    void eventActivationMapTest()
    {
        MainFixture fixture;
        auto &model = fixture.model;
        require(verifies(model), "activation: fixture rejected");
        runMappingPipeline(model);
        runPass(model, "cpu.st.layout-named-stores");
        const auto messages = runPass(model, "cpu.st.build-event-activation-map");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::EventActivationMap, "activation: stage wrong");
        require(mapping.schedule && mapping.schedule->eventActivation, "activation: payload missing");
        require(!mapping.schedule->memWritePlan, "activation: mem plan filled too early");
        require(mapping.schedule->eventActivation->empty(),
                "activation: sink-only event carriers must produce an empty map");
        require(infoValue(messages, "cpu.st.build-event-activation-map", "event_acts=") ==
                std::optional<uint64_t>(0), "activation: act diagnostic wrong");
        require(infoValue(messages, "cpu.st.build-event-activation-map", "mapped_supernodes=") ==
                std::optional<uint64_t>(0), "activation: mapped diagnostic wrong");
        roundTrip(model);
    }

    // An event-free latch write anchors a SinkEscape supernode: it fires
    // unconditionally every round and appears in no activation entry. The
    // same fixture's {0} reg write is a SinkEvent sink, so the map stays
    // empty even though an edgeDet cluster exists.
    void latchEscapeTest()
    {
        GrhSimModel model("m4_latch");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = model.addInput("clk", bit);
        const auto d0 = model.addInput("d0", bit);
        const auto dl = model.addInput("dl", bit);
        const auto clkv = addReadOp(model, clk, "clk", SimPhase::Event, true);
        addEdgeDet(model, clkv, 0);
        const auto d0v = addReadOp(model, d0, "d0", SimPhase::None, false);
        const auto dlv = addReadOp(model, dl, "dl", SimPhase::None, false);
        const auto one0 = addConstant(model, bit, "1'b1");
        const auto oneL = addConstant(model, bit, "1'b1");
        const auto q0 = addState(model, "q0", bit, "1'b0");
        const auto ql = addState(model, "ql", bit, "1'b0");
        addRegWrite(model, one0, d0v, one0, q0, {0});
        const auto ln = addNot(model, dlv, "ln");
        OpId wl;
        {
            const std::array refs{ObjectRef::state(ql)};
            wl = model.addOperation("core.state.latchWrite", std::array{ln, dlv, oneL},
                                    {}, refs);
        }
        require(verifies(model), "latch: fixture rejected");
        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        const std::array<std::string_view, 2> smallSupernode{"--max-op-in-compute-supernode", "6"};
        runPass(model, "cpu.st.merge-general-supernodes", smallSupernode);
        runPass(model, "cpu.st.layout-named-stores");
        const auto messages = runPass(model, "cpu.st.build-event-activation-map");
        const auto &mapping = *model.cpuMapping();
        const auto *latchSupernode = supernodeOf(mapping, wl);
        require(bool(latchSupernode), "latch: write has no supernode");
        require(latchSupernode->attrs.supernodeCategory == CpuSupernodeCategory::SinkEscape,
                "latch: event-free write is not in the escape class");
        require(mapping.schedule->eventActivation->empty(),
                "latch: escape/sink-only carriers must leave the map empty");
        require(infoValue(messages, "cpu.st.build-event-activation-map", "mapped_supernodes=") ==
                std::optional<uint64_t>(0), "latch: mapped diagnostic wrong");
        roundTrip(model);
    }

    // Activation-map coverage (V2-M2 acceptance): an event-carrying NON-SINK
    // op (a posedge-gated DPI call with a return value) must be covered by
    // its act's entry — a missed mapping would leave the call unexecuted on
    // its edge round. Sink supernodes (the {1} reg writes, the escape latch
    // write) and the Mem-phase {0} mem write must NOT appear: sink clusters
    // self-gate on their signatures and Mem writes run every round.
    void eventActivationCoverageTest()
    {
        GrhSimModel model("m4_event_activation");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(bit, 16);
        const auto clk0 = model.addInput("clk0", bit);
        const auto clk1 = model.addInput("clk1", bit);
        const auto d1 = model.addInput("d1", bit);
        const auto dl = model.addInput("dl", bit);
        const auto md = model.addInput("md", bit);
        const auto clk0v = addReadOp(model, clk0, "clk0", SimPhase::Event, true);
        addEdgeDet(model, clk0v, 0);
        const auto clk1v = addReadOp(model, clk1, "clk1", SimPhase::Event, true);
        addEdgeDet(model, clk1v, 1);
        const auto d1v = addReadOp(model, d1, "d1", SimPhase::None, false);
        const auto dlv = addReadOp(model, dl, "dl", SimPhase::None, false);
        const auto mdv = addReadOp(model, md, "md", SimPhase::None, false);
        const auto one = addConstant(model, bit, "1'b1");
        const auto one2 = addConstant(model, bit, "1'b1");
        const auto oneL = addConstant(model, bit, "1'b1");
        const auto row3 = addConstant(model, model.logicType(4, false, LogicDomain::TwoState),
                                      "4'h3");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", byte, "8'h00");
        const auto ql = addState(model, "ql", bit, "1'b0");
        const auto m1 = addState(model, "m1", memType, "0");
        // SinkEvent {1}: a posedge-clk1 register write.
        const auto w1 = addRegWrite(model, d1v, d1v, d1v, q1, {1});
        // Non-sink carrier of act 1: a gated DPI call with a return value
        // (lowered shape: event-free operands/refs plus event_acts).
        const std::array dblArgs{DpiArgument{model.intern("x"), DpiDirection::Input, byte}};
        const auto dbl = model.addExternFunction("cpu_test_dbl", "core.dpi", "cpu_test_dbl",
                                                 dblArgs, byte);
        const auto z = model.addValue(byte, "z");
        OpId dpi;
        {
            const std::array refs{ObjectRef::function(dbl)};
            const std::array params{
                Parameter{model.intern("event_acts"), std::vector<int64_t>{1}}};
            dpi = model.addOperation("core.dpi.call", std::array{one, mdv}, std::array{z},
                                     refs, params);
        }
        // SinkEvent {1} consumer of the DPI result (keeps z live).
        const auto w2 = addRegWrite(model, one2, z, one2, q2, {1});
        // SinkEscape: an event-free latch write.
        const auto ln = addNot(model, dlv, "ln");
        OpId wl;
        {
            const std::array refs{ObjectRef::state(ql)};
            wl = model.addOperation("core.state.latchWrite", std::array{ln, dlv, oneL},
                                    {}, refs);
        }
        // Mem-phase carrier of act 0: outside the supernode space entirely.
        {
            const std::array refs{ObjectRef::state(m1)};
            const std::array params{
                Parameter{model.intern("event_acts"), std::vector<int64_t>{0}}};
            model.addOperation("core.state.memWrite", std::array{mdv, row3, mdv, mdv},
                               {}, refs, params);
        }
        require(verifies(model), "coverage: fixture rejected");
        runMappingPipeline(model);
        runPass(model, "cpu.st.layout-named-stores");
        const auto messages = runPass(model, "cpu.st.build-event-activation-map");
        const auto &mapping = *model.cpuMapping();
        const auto &entries = *mapping.schedule->eventActivation;
        require(entries.size() == 1 && entries.front().act == 1,
                "coverage: the map must hold exactly the non-sink carrier's act");
        const auto ordinalOf = [&](OpId op) {
            const auto *supernode = supernodeOf(mapping, op);
            require(bool(supernode), "coverage: op has no supernode");
            return supernodeOrdinal(mapping, supernode->id);
        };
        const auto dpiOrdinal = ordinalOf(dpi);
        const auto &words = entries.front().supernodeWords;
        require(bitOf(entries.front(), dpiOrdinal), "coverage: non-sink carrier not mapped");
        uint32_t bits = 0;
        for (const auto word : words) bits += std::popcount(word);
        require(bits == 1, "coverage: the entry must cover only the carrier's supernode");
        require(!bitOf(entries.front(), ordinalOf(w1)) && !bitOf(entries.front(), ordinalOf(w2)) &&
                !bitOf(entries.front(), ordinalOf(wl)),
                "coverage: a sink supernode leaked into the activation map");
        require(infoValue(messages, "cpu.st.build-event-activation-map", "event_acts=") ==
                std::optional<uint64_t>(1), "coverage: act diagnostic wrong");
        require(infoValue(messages, "cpu.st.build-event-activation-map", "mapped_supernodes=") ==
                std::optional<uint64_t>(1), "coverage: mapped diagnostic wrong");
        roundTrip(model);
    }

    // cpu.st.build-mem-write-plan: per-mem op-id priority, exact static-row
    // readers, conservative dynamic readers, and the event-free mark.
    void memWritePlanTest()
    {
        MainFixture fixture;
        auto &model = fixture.model;
        require(verifies(model), "mem plan: fixture rejected");
        runMappingPipeline(model);
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-activation-map");
        runPass(model, "cpu.st.build-mem-write-plan");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::MemWritePlan, "mem plan: stage wrong");
        require(mapping.schedule && mapping.schedule->memWritePlan, "mem plan: payload missing");
        require(!mapping.schedule->timeslotTriggers, "mem plan: triggers filled too early");
        const auto &plan = *mapping.schedule->memWritePlan;
        require(plan.size() == 2, "mem plan: entry count wrong");
        require(plan[0].writeOp == fixture.mw0 && plan[0].priority == 0 && !plan[0].eventFree,
                "mem plan: first entry wrong");
        require(plan[1].writeOp == fixture.mw1 && plan[1].priority == 1 && plan[1].eventFree,
                "mem plan: second entry wrong");
        const auto staticOwner = supernodeOf(mapping, fixture.mr0)->id;
        const auto dynamicOwner = supernodeOf(mapping, fixture.mr1)->id;
        // V2-M1: the event-free mem readers share one non-sink supernode
        // (the merge frame no longer carries an event-domain prohibition);
        // the reader table still distinguishes them by staticRow.
        require(staticOwner == dynamicOwner, "mem plan: event-free readers must share the non-sink frame");
        for (const auto &entry : plan)
        {
            require(entry.readers.size() == 2, "mem plan: reader count wrong");
            bool foundStatic = false, foundDynamic = false;
            for (const auto &reader : entry.readers)
            {
                if (reader.owner == staticOwner && reader.staticRow == std::optional<uint64_t>(3))
                    foundStatic = true;
                if (reader.owner == dynamicOwner && !reader.staticRow) foundDynamic = true;
            }
            require(foundStatic, "mem plan: static-row reader missing");
            require(foundDynamic, "mem plan: dynamic reader missing");
        }
        roundTrip(model);
    }

    // cpu.st.build-phase-schedule: the three fanout tables (event-only inputs
    // skipped, dual-use reset listed, state readers covered), the timeslot
    // trigger map, and the single-core phase task sequence.
    void phaseScheduleTest()
    {
        MainFixture fixture;
        auto &model = fixture.model;
        require(verifies(model), "schedule: fixture rejected");
        runMappingPipeline(model);
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-activation-map");
        runPass(model, "cpu.st.build-mem-write-plan");
        runPass(model, "cpu.st.pack-general-functions");
        runPass(model, "cpu.st.build-phase-schedule");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::PhaseSchedule, "schedule: stage wrong");
        require(bool(mapping.schedule), "schedule: payload missing");
        const auto &schedule = *mapping.schedule;

        // inputFanout: dual-use rst listed, pure-event inputs skipped.
        const auto *rstEntry = findValueFanout(schedule.inputFanout, fixture.rstDv);
        require(rstEntry && rstEntry->targets.activate.size() == 1 &&
                rstEntry->targets.activate.front() ==
                    supernodeOf(mapping, producerOf(model, fixture.rstDv))->id,
                "schedule: dual-use input fanout wrong");
        for (const auto value : {fixture.clk0v, fixture.clk1v, fixture.rstEv, fixture.clk1Gv})
            require(!findValueFanout(schedule.inputFanout, value),
                    "schedule: event-only input fanned out");
        // supernodeFanout: V2-M2 narrowing (boundary 2) — every
        // cross-supernode consumer in this fixture is a sink supernode (the
        // w0/w1 write clusters), so no fanout row survives: sink supernodes
        // gate on their signatures and are never data-activated.
        require(schedule.computeSupernodeFanout.empty(),
                "schedule: sink supernodes must not appear as fanout targets");
        // stateFanout: q0 covers its general reader; mem/latch states have none.
        const auto *q0Entry = findStateFanout(schedule.commitStateFanout, fixture.q0);
        require(q0Entry && q0Entry->targets.activate.size() == 1 &&
                q0Entry->targets.activate.front() ==
                    supernodeOf(mapping, producerOf(model, fixture.q0rv))->id,
                "schedule: state fanout wrong");
        require(!findStateFanout(schedule.commitStateFanout, fixture.m1),
                "schedule: unexpected state fanout entries");
        // timeslotTriggers: timeslotFlag x event_acts Cartesian expansion.
        require(schedule.timeslotTriggers &&
                *schedule.timeslotTriggers == std::vector<CpuTimeslotTrigger>({{0, 0}, {1, 0}}),
                "schedule: timeslot triggers wrong");

        // Task sequence: P_event -> P_general functions -> P_mem -> P_output.
        // The General tasks reference the trailing EmitFunction leaves (the
        // supernodes stay direct branch children in ordinal order, M5d-6).
        const auto &generalBranch = branch(mapping, CpuPhase::General);
        std::vector<PartitionId> generalFunctions;
        for (const auto child : generalBranch.children)
            if (mapping.partitionTree.partitions[child.index - 1].attrs.kind ==
                CpuPartitionKind::EmitFunction)
                generalFunctions.push_back(child);
        require(schedule.numaNodes.size() == 1 && schedule.numaNodes.front().cores.size() == 1,
                "schedule: numa/core shape wrong");
        const auto &tasks = schedule.numaNodes.front().cores.front().tasks;
        require(tasks.size() == generalFunctions.size() + 3, "schedule: task count wrong");
        for (std::size_t i = 0; i < tasks.size(); ++i)
            require(tasks[i].id.index == i + 1 && tasks[i].waitsFor.empty(),
                    "schedule: task ids wrong");
        require(tasks.front().execution == CpuExecution::AlwaysScanCommit &&
                tasks.front().partition == branch(mapping, CpuPhase::Event).children.front(),
                "schedule: event task wrong");
        for (std::size_t i = 0; i < generalFunctions.size(); ++i)
            require(tasks[i + 1].execution == CpuExecution::DataGated &&
                    tasks[i + 1].partition == generalFunctions[i],
                    "schedule: general task wrong");
        require(tasks[generalFunctions.size() + 1].execution == CpuExecution::AlwaysScanCommit &&
                tasks[generalFunctions.size() + 1].partition ==
                    branch(mapping, CpuPhase::Mem).children.front(),
                "schedule: mem task wrong");
        require(tasks[generalFunctions.size() + 2].execution == CpuExecution::EvalEnd &&
                tasks[generalFunctions.size() + 2].partition ==
                    branch(mapping, CpuPhase::Output).children.front(),
                "schedule: output task wrong");
        require(model.mappings().front().complete, "schedule: mapping not complete");
        roundTrip(model);
    }

    // The verifier rejects corrupted M4 payloads; the new passes fail cleanly
    // on missing prerequisites and unexpected arguments.
    void verifierRejectsTest()
    {
        MainFixture fixture;
        auto &model = fixture.model;
        require(verifies(model), "rejects: fixture rejected");
        runMappingPipeline(model);
        const auto preLayoutMapping = *model.cpuMapping();
        runPass(model, "cpu.st.layout-named-stores");
        const auto layoutMapping = *model.cpuMapping();
        runPass(model, "cpu.st.build-event-activation-map");
        runPass(model, "cpu.st.build-mem-write-plan");
        runPass(model, "cpu.st.pack-general-functions");
        runPass(model, "cpu.st.build-phase-schedule");
        const auto &mapping = *model.cpuMapping();
        const auto reject = [&](CpuBackendMapping bad, const char *what) {
            auto broken = model.clone();
            broken.setCpuMapping(std::move(bad));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics) &&
                    diagnostics.hasError(), what);
        };
        // (a) duplicated store field name.
        {
            auto bad = mapping;
            auto &fields = bad.dataLayout->namedStores->at(0).fields;
            fields[1].name = fields[0].name;
            reject(std::move(bad), "rejects: verifier accepted a duplicated store field name");
        }
        // (b) a spurious activation-map entry (the MainFixture's map is
        // legitimately empty — sink-only carriers — so any entry disagrees
        // with the recomputed non-sink coverage).
        {
            auto bad = mapping;
            bad.schedule->eventActivation->push_back(CpuEventActivation{0, {uint64_t{1}}});
            reject(std::move(bad), "rejects: verifier accepted a spurious activation entry");
        }
        // (c) dropped state fanout entry.
        {
            auto bad = mapping;
            bad.schedule->commitStateFanout.erase(bad.schedule->commitStateFanout.begin());
            reject(std::move(bad), "rejects: verifier accepted an incomplete state fanout");
        }
        // (d) tampered static row.
        {
            auto bad = mapping;
            for (auto &reader : bad.schedule->memWritePlan->front().readers)
                if (reader.staticRow)
                {
                    reader.staticRow = *reader.staticRow + 1;
                    break;
                }
            reject(std::move(bad), "rejects: verifier accepted a tampered static row");
        }
        // (e) schedule payload before the layout stage.
        {
            auto bad = preLayoutMapping;
            bad.schedule = CpuSchedulePlan{};
            reject(std::move(bad), "rejects: verifier accepted an early schedule payload");
        }
        // (f) supernodes stripped of their event-act annotations.
        {
            auto bad = layoutMapping;
            for (auto &partition : bad.partitionTree.partitions)
                partition.attrs.eventActs.reset();
            reject(std::move(bad), "rejects: verifier accepted supernodes without event acts");
        }
        // Missing prerequisites fail cleanly without touching the mapping.
        std::string error;
        GrhSimModel fresh("m4_fresh");
        fresh.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        for (auto name : {"cpu.st.layout-named-stores", "cpu.st.build-event-activation-map",
                          "cpu.st.build-mem-write-plan", "cpu.st.build-phase-schedule"})
        {
            auto pass = defaultPassRegistry().create(name, {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(fresh, diagnostics).success && !fresh.cpuMapping(),
                    std::string(name) + " accepted a missing prerequisite");
        }
        // Wrong-stage prerequisite: the activation-map pass requires the layout stage.
        {
            MainFixture wrongStage;
            runMappingPipeline(wrongStage.model);
            auto pass = defaultPassRegistry().create("cpu.st.build-event-activation-map", {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(wrongStage.model, diagnostics).success &&
                    wrongStage.model.cpuMapping()->stage == CpuMappingStage::GeneralSupernodes,
                    "rejects: bitmap pass accepted a wrong-stage prerequisite");
            // C6 packs functions only after the mem write plan (M5d-6 order).
            auto pack = defaultPassRegistry().create("cpu.st.pack-general-functions", {}, error);
            require(bool(pack), error);
            diag::Diagnostics packDiagnostics;
            require(!pack->run(wrongStage.model, packDiagnostics).success &&
                    wrongStage.model.cpuMapping()->stage == CpuMappingStage::GeneralSupernodes,
                    "rejects: function packing accepted a pre-mem-plan mapping");
        }
        // Arguments are rejected at creation.
        const std::array<std::string_view, 1> junk{"--x"};
        for (auto name : {"cpu.st.layout-named-stores", "cpu.st.build-event-activation-map",
                          "cpu.st.build-mem-write-plan", "cpu.st.build-phase-schedule"})
            require(!defaultPassRegistry().create(name, junk, error),
                    std::string(name) + " accepted arguments");
    }

    // An empty model flows through all C-segment passes; the schedule
    // degenerates to the three flat phase tasks and a two-field active-flags
    // store. With no ops and no states there is nothing to attribute or
    // classify, so C1 initializes the mapping directly.
    void emptyModelTest()
    {
        GrhSimModel model("m4_empty");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        runSchedulePipeline(model);
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::PhaseSchedule, "empty: stage wrong");
        const auto &layout = *mapping.dataLayout;
        const auto &active = store(mapping, CpuNamedStoreKind::ActiveFlags);
        require(active.fields.size() == 2 && active.sizeBytes == 2,
                "empty: active flags shape wrong");
        for (const auto &field : active.fields)
        {
            require(field.aux == 0, "empty: active flags aux wrong");
            const auto &type = layout.types[field.type.index - 1];
            require(type.kind == CpuTypeKind::Array && type.count == 1,
                    "empty: active flags type wrong");
        }
        const auto &eventAct = store(mapping, CpuNamedStoreKind::EventAct);
        require(eventAct.fields.empty() && eventAct.sizeBytes == 0, "empty: event act wrong");
        require(store(mapping, CpuNamedStoreKind::TimeslotTrigger).fields.empty(),
                "empty: timeslot triggers wrong");
        require(store(mapping, CpuNamedStoreKind::RegLatch).fields.empty() &&
                store(mapping, CpuNamedStoreKind::Mem).fields.empty() &&
                store(mapping, CpuNamedStoreKind::Boundary).fields.empty() &&
                store(mapping, CpuNamedStoreKind::PrevEvent).fields.empty(),
                "empty: stores not empty");
        const auto &schedule = *mapping.schedule;
        require(schedule.eventActivation->empty() && schedule.memWritePlan->empty() &&
                schedule.timeslotTriggers->empty() && schedule.inputFanout.empty() &&
                schedule.computeSupernodeFanout.empty() && schedule.commitStateFanout.empty(),
                "empty: schedule tables not empty");
        const auto &tasks = schedule.numaNodes.front().cores.front().tasks;
        require(tasks.size() == 3, "empty: task count wrong");
        require(tasks[0].execution == CpuExecution::AlwaysScanCommit &&
                tasks[1].execution == CpuExecution::AlwaysScanCommit &&
                tasks[2].execution == CpuExecution::EvalEnd, "empty: task executions wrong");
        require(model.mappings().front().complete, "empty: mapping not complete");
        roundTrip(model);
    }
}

int main()
{
    try
    {
        namedStoreLayoutTest();
        namingRulesTest();
        eventActivationMapTest();
        latchEscapeTest();
        eventActivationCoverageTest();
        memWritePlanTest();
        phaseScheduleTest();
        verifierRejectsTest();
        emptyModelTest();
        std::cout << "CPU six-phase store/schedule tests passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
