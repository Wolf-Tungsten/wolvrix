#include "grhsim/backend/cpu.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
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
                "six-phase mapping pass failed: " + std::string(name));
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
                "six-phase stage store failed");
        std::istringstream input(first.str());
        auto loaded = readGrhSimJson(input, defaultDialectRegistry(), diagnostics);
        if (!loaded)
            for (const auto &message : diagnostics.messages()) std::cerr << message.message << '\n';
        require(bool(loaded), "six-phase stage load failed");
        require(writeGrhSimJson(*loaded, second, defaultDialectRegistry(), diagnostics) &&
                first.str() == second.str(), "six-phase stage round trip changed bytes");
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

    ValueId addInputRead(GrhSimModel &model, const char *name, SimPhase phase = SimPhase::None)
    {
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto port = model.addInput(name, bit);
        const auto value = model.addValue(bit, name);
        const auto id = model.addOperation("core.input.read", {}, std::array{value},
                                           std::array{ObjectRef::input(port)});
        if (phase != SimPhase::None) model.setOperationPhase(id, phase);
        return value;
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

    ValueId addOr(GrhSimModel &model, ValueId lhs, ValueId rhs, std::string_view name)
    {
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto value = model.addValue(bit, name);
        model.addOperation("core.compute.or", std::array{lhs, rhs}, std::array{value});
        return value;
    }

    OpId producerOf(const GrhSimModel &model, ValueId value)
    {
        for (const auto &op : model.operations())
            for (auto result : model.results(op))
                if (result == value) return op.id;
        throw std::runtime_error("value has no producer");
    }

    OpId addEdgeDet(GrhSimModel &model, ValueId event, int64_t cluster)
    {
        const std::array params{Parameter{model.intern("edge"), std::string("posedge")},
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

    bool containsOp(const CpuBackendMapping &mapping, const CpuPartition &partition, OpId op)
    {
        const auto ops = flatOps(mapping, partition.id);
        return std::find(ops.begin(), ops.end(), op) != ops.end();
    }

    std::vector<OpId> opsOfType(const GrhSimModel &model, std::string_view type)
    {
        std::vector<OpId> result;
        for (const auto &op : model.operations())
            if (model.text(op.opType) == type) result.push_back(op.id);
        return result;
    }

    std::size_t countKind(const CpuBackendMapping &mapping, CpuPartitionKind kind)
    {
        const auto &partitions = mapping.partitionTree.partitions;
        return std::count_if(partitions.begin(), partitions.end(), [kind](const auto &partition) {
            return partition.attrs.kind == kind;
        });
    }

    void requirePhases(const GrhSimModel &model)
    {
        for (const auto &op : model.operations())
            require(op.phase != SimPhase::None, "split-phases left a phase-less op");
    }

    struct TwoClockFixture
    {
        GrhSimModel model;
        ValueId d, one;
        OpId det0, det1;
        explicit TwoClockFixture(std::string_view name) : model(std::string(name))
        {
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk0 = addInputRead(model, "clk0", SimPhase::Event);
            const auto clk1 = addInputRead(model, "clk1", SimPhase::Event);
            det0 = addEdgeDet(model, clk0, 0);
            det1 = addEdgeDet(model, clk1, 1);
            d = addInputRead(model, "d");
            one = addConstant(model, model.logicType(1, false, LogicDomain::TwoState), "1'b1");
        }
    };

    // Gate case 1: two writes in the same event domain merge with their cones
    // into a single supernode whose eventActs attr is the common set.
    void sameDomainMergeTest()
    {
        GrhSimModel model("f1_same_domain");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", SimPhase::Event);
        addEdgeDet(model, clk, 0);
        const auto d = addInputRead(model, "d");
        const auto x = addNot(model, d, "x");
        const auto one = addConstant(model, bit, "1'b1");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        addRegWrite(model, d, x, one, q1, {0});
        addRegWrite(model, x, d, one, q2, {0});
        require(verifies(model), "f1: fixture rejected");

        const auto split = runPass(model, "cpu.st.split-phases");
        require(infoValue(split, "cpu.st.split-phases", "event_ops=") == std::optional<uint64_t>(2),
                "f1: split event op count wrong");
        require(infoValue(split, "cpu.st.split-phases", "general_ops=") == std::optional<uint64_t>(5),
                "f1: split general op count wrong");
        require(infoValue(split, "cpu.st.split-phases", "attributed_general=") == std::optional<uint64_t>(3),
                "f1: split attribution count wrong");
        requirePhases(model);
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::SplitPhases, "f1: stage after split wrong");
        require(mapping.partitionTree.partitions.size() == 5, "f1: split tree shape wrong");
        require(branch(mapping, CpuPhase::Event).ops.size() == 2, "f1: event branch wrong");
        require(branch(mapping, CpuPhase::General).ops.empty() &&
                branch(mapping, CpuPhase::General).children.empty(), "f1: general shell not empty");
        roundTrip(model);

        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &merged = *model.cpuMapping();
        const auto packs = supernodes(merged);
        require(packs.size() == 1, "f1: same-domain writes did not merge into one supernode");
        require(flatOps(merged, packs.front()->id).size() == 5,
                "f1: supernode does not hold all general ops");
        require(packs.front()->attrs.eventActs &&
                *packs.front()->attrs.eventActs == std::vector<int64_t>{0},
                "f1: supernode eventActs wrong");
        roundTrip(model);

        runPass(model, "cpu.st.pack-general-functions");
        const auto &packed = *model.cpuMapping();
        require(packed.stage == CpuMappingStage::GeneralFunctions, "f1: final stage wrong");
        require(branch(packed, CpuPhase::Event).children.size() == 1 &&
                branch(packed, CpuPhase::Mem).children.size() == 1 &&
                branch(packed, CpuPhase::Output).children.size() == 1, "f1: flat functions missing");
        require(flatOps(packed, branch(packed, CpuPhase::Event).children.front()).size() == 2,
                "f1: event function ops wrong");
        require(supernodes(packed).size() == 1, "f1: packing lost the supernode");
        roundTrip(model);
    }

    // Gate case 2: writes with subset act sets ({0} vs {0,1}) must stay in
    // different supernodes (rule 1 forbids the merge).
    void subsetDomainBlockedTest()
    {
        TwoClockFixture fixture("f2_subset_domain");
        auto &model = fixture.model;
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto x = addNot(model, fixture.d, "x");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto w1 = addRegWrite(model, fixture.one, x, fixture.one, q1, {0});
        const auto w2 = addRegWrite(model, fixture.one, fixture.d, fixture.one, q2, {0, 1});
        require(verifies(model), "f2: fixture rejected");

        runPass(model, "cpu.st.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &mapping = *model.cpuMapping();
        const auto *s1 = supernodeOf(mapping, w1);
        const auto *s2 = supernodeOf(mapping, w2);
        require(s1 && s2 && s1 != s2, "f2: subset-domain writes merged");
        require(s1->attrs.eventActs && *s1->attrs.eventActs == std::vector<int64_t>{0},
                "f2: first supernode eventActs wrong");
        require(s2->attrs.eventActs && *s2->attrs.eventActs == std::vector<int64_t>({0, 1}),
                "f2: second supernode eventActs wrong");
        for (const auto *supernode : supernodes(mapping))
            require(!(containsOp(mapping, *supernode, w1) && containsOp(mapping, *supernode, w2)),
                    "f2: a supernode holds both writes");
        roundTrip(model);
        runPass(model, "cpu.st.pack-general-functions");
        roundTrip(model);
    }

    // Gate case 3: a shared cone feeding two different event domains may not
    // merge with either write, and the two writes may not merge with each
    // other (rule 2 blocks the cone, rule 1 blocks the writes). The
    // event_domain_blocked count (5) is unaffected by the removal of state
    // write->read edges from the influence graph: this fixture contains no
    // state.read op, so the edge sets before and after are identical.
    void crossDomainBlockedTest()
    {
        TwoClockFixture fixture("f3_cross_domain");
        auto &model = fixture.model;
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto x = addNot(model, fixture.d, "x");
        const auto one2 = addConstant(model, bit, "1'b1");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto w1 = addRegWrite(model, fixture.one, x, fixture.one, q1, {0});
        const auto w2 = addRegWrite(model, one2, x, one2, q2, {1});
        require(verifies(model), "f3: fixture rejected");

        runPass(model, "cpu.st.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        const auto merged = runPass(model, "cpu.st.merge-general-supernodes");
        const auto blocked = infoValue(merged, "cpu.st.merge-general-supernodes", "event_domain_blocked=");
        require(blocked && *blocked >= 1, "f3: event-domain prohibition never fired");
        const auto &mapping = *model.cpuMapping();
        const auto cone = opsOfType(model, "core.compute.not");
        require(cone.size() == 1, "f3: cone op missing");
        const auto inputRead = opsOfType(model, "core.input.read");
        require(inputRead.size() == 3, "f3: input reads missing");
        const auto *conePack = supernodeOf(mapping, cone.front());
        const auto *s1 = supernodeOf(mapping, w1);
        const auto *s2 = supernodeOf(mapping, w2);
        require(supernodes(mapping).size() == 3, "f3: expected exactly three supernodes");
        require(conePack && s1 && s2 && conePack != s1 && conePack != s2 && s1 != s2,
                "f3: cross-domain merge happened");
        require(conePack->attrs.eventActs && conePack->attrs.eventActs->empty(),
                "f3: cone supernode should carry an empty eventActs");
        require(containsOp(mapping, *conePack, inputRead.back()),
                "f3: cone supernode lost its input cone");
        require(s1->attrs.eventActs && *s1->attrs.eventActs == std::vector<int64_t>{0},
                "f3: first write supernode eventActs wrong");
        require(s2->attrs.eventActs && *s2->attrs.eventActs == std::vector<int64_t>{1},
                "f3: second write supernode eventActs wrong");
        roundTrip(model);
        runPass(model, "cpu.st.pack-general-functions");
        roundTrip(model);
    }

    // Gate case 4: a pure combinational cluster is exempt from the event
    // domain rules even when its downstream closure spans several domains.
    void combExemptTest()
    {
        TwoClockFixture fixture("f4_comb_exempt");
        auto &model = fixture.model;
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto a = addNot(model, fixture.d, "a");
        const auto b = addNot(model, a, "b");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto w1 = addRegWrite(model, fixture.one, b, fixture.one, q1, {0});
        const auto w2 = addRegWrite(model, fixture.one, b, fixture.one, q2, {1});
        require(verifies(model), "f4: fixture rejected");

        runPass(model, "cpu.st.split-phases");
        const std::array<std::string_view, 2> singleNode{"--max-op-in-compute-node", "1"};
        runPass(model, "cpu.st.build-general-nodes", singleNode);
        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &mapping = *model.cpuMapping();
        const auto nots = opsOfType(model, "core.compute.not");
        require(nots.size() == 2, "f4: cone ops missing");
        const auto inputRead = opsOfType(model, "core.input.read");
        const auto *packA = supernodeOf(mapping, nots[0]);
        const auto *packB = supernodeOf(mapping, nots[1]);
        const auto *s1 = supernodeOf(mapping, w1);
        const auto *s2 = supernodeOf(mapping, w2);
        require(packA && packB && packA == packB, "f4: combinational chain did not merge");
        require(containsOp(mapping, *packA, inputRead.back()),
                "f4: chain supernode lost the input read");
        require(packA->attrs.eventActs && packA->attrs.eventActs->empty(),
                "f4: exempt supernode must carry an empty eventActs");
        require(s1 && s2 && s1 != s2 && packA != s1 && packA != s2,
                "f4: writes merged with each other or with the chain");
        require(s1->attrs.eventActs && *s1->attrs.eventActs == std::vector<int64_t>{0},
                "f4: first write supernode eventActs wrong");
        require(s2->attrs.eventActs && *s2->attrs.eventActs == std::vector<int64_t>{1},
                "f4: second write supernode eventActs wrong");
        roundTrip(model);
    }

    // Gate case 9 (cross-domain state-read exemption): a domain {0} regWrite
    // W1 -> Q whose Q state.read feeds, through a combinational cone, a
    // domain {1} regWrite W2 is a legal two-domain design. State readers
    // consume S directly and are activated by P_publish's stateFanout, so the
    // influence graph carries no state write->read edge: W1 keeps
    // influence == acts == {0} and the pipeline verifies. W1's and W2's
    // supernodes must not merge — they only meet as same-predecessor-set
    // coarsen candidates (mode 2, both preceded by the shared constant node),
    // where rule 1 still blocks them; the shared constant's merges into
    // either write node are blocked by rule 2 (its influence spans both
    // domains), so the prohibition demonstrably stays active.
    void crossDomainStateReadTest()
    {
        TwoClockFixture fixture("f9_state_read_exempt");
        auto &model = fixture.model;
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto w1 = addRegWrite(model, fixture.one, fixture.d, fixture.one, q1, {0});
        const auto qr = addStateRead(model, q1, "qr");
        const auto y = addNot(model, qr, "y");
        const auto w2 = addRegWrite(model, fixture.one, y, fixture.one, q2, {1});
        require(verifies(model), "f9: fixture rejected");

        runPass(model, "cpu.st.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        const auto merged = runPass(model, "cpu.st.merge-general-supernodes");
        const auto blocked = infoValue(merged, "cpu.st.merge-general-supernodes", "event_domain_blocked=");
        require(blocked && *blocked >= 1, "f9: event-domain prohibition never fired");
        const auto &mapping = *model.cpuMapping();
        const auto *s1 = supernodeOf(mapping, w1);
        const auto *s2 = supernodeOf(mapping, w2);
        require(s1 && s2 && s1 != s2, "f9: cross-domain writes merged through the state read");
        require(supernodes(mapping).size() == 3, "f9: expected exactly three supernodes");
        require(s1->attrs.eventActs && *s1->attrs.eventActs == std::vector<int64_t>{0},
                "f9: first write supernode eventActs wrong");
        require(s2->attrs.eventActs && *s2->attrs.eventActs == std::vector<int64_t>{1},
                "f9: second write supernode eventActs wrong");
        // The Q read cone belongs to the reading domain's supernode.
        require(containsOp(mapping, *s2, producerOf(model, qr)) &&
                containsOp(mapping, *s2, producerOf(model, y)),
                "f9: state-read cone did not stay with the reading domain");
        roundTrip(model);
        runPass(model, "cpu.st.pack-general-functions");
        roundTrip(model);
    }

    // The four mem-write kinds split into the flat Mem branch (op-id order)
    // while their operand cones stay on the General side; reg/latch writes
    // merge with their exclusive cones.
    void memWriteSplitTest()
    {
        GrhSimModel model("f5_mem_split");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(bit, 16);
        const auto clk = addInputRead(model, "clk", SimPhase::Event);
        addEdgeDet(model, clk, 0);
        const auto a = addInputRead(model, "a");
        const auto b = addInputRead(model, "b");
        const auto c = addInputRead(model, "c");
        const auto d4 = addInputRead(model, "d4");
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto row = addConstant(model, memType, "16'h0000");
        const auto qr = addState(model, "qr", bit, "1'b0");
        const auto ql = addState(model, "ql", bit, "1'b0");
        const auto mem1 = addState(model, "mem1", memType, "0");
        const auto mem2 = addState(model, "mem2", memType, "0");
        const auto mem3 = addState(model, "mem3", memType, "0");
        const auto mem4 = addState(model, "mem4", memType, "0");
        const auto rc = addAnd(model, a, b, "rc");
        const auto rn = addNot(model, a, "rn");
        const auto wr = addRegWrite(model, rc, rn, one, qr, {0});
        const auto lc = addOr(model, a, b, "lc");
        const auto ln = addNot(model, b, "ln");
        const std::array latchRefs{ObjectRef::state(ql)};
        const auto wl = model.addOperation("core.state.latchWrite", std::array{lc, ln, one}, {}, latchRefs);
        // Mem-write operand cones are exclusive single-use chains.
        const auto mwC = addAnd(model, a, c, "mw_c");
        const auto mwD = addNot(model, a, "mw_d");
        const std::array memWriteRefs{ObjectRef::state(mem1)};
        const auto mw = model.addOperation("core.state.memWrite", std::array{mwC, one, mwD, one}, {},
                                           memWriteRefs);
        const auto mfC = addOr(model, b, c, "mf_c");
        const std::array memFillRefs{ObjectRef::state(mem2)};
        const auto mf = model.addOperation("core.state.memFill", std::array{mfC, zero}, {}, memFillRefs);
        const auto maC = addAnd(model, b, d4, "ma_c");
        const std::array memAssignRefs{ObjectRef::state(mem3)};
        const auto ma = model.addOperation("core.state.memAssign", std::array{maC, row}, {}, memAssignRefs);
        const auto ms1 = addOr(model, a, d4, "ms_1");
        const auto ms2 = addNot(model, b, "ms_2");
        const auto ms3 = addAnd(model, c, d4, "ms_3");
        const auto ms4 = addNot(model, c, "ms_4");
        const std::array memSeqRefs{ObjectRef::state(mem4)};
        const auto mws = model.addOperation("core.state.memWriteSeq",
                                            std::array{ms1, one, ms2, ms3, zero, ms4}, {}, memSeqRefs);
        require(verifies(model), "f5: fixture rejected");

        const auto split = runPass(model, "cpu.st.split-phases");
        require(infoValue(split, "cpu.st.split-phases", "attributed_mem=") == std::optional<uint64_t>(4),
                "f5: mem attribution count wrong");
        const auto &mapping = *model.cpuMapping();
        const std::vector<OpId> memOrder{mw, mf, ma, mws};
        require(branch(mapping, CpuPhase::Mem).ops == memOrder, "f5: mem branch is not flat op-id order");
        for (auto opId : memOrder)
            require(model.operations()[opId.index - 1].phase == SimPhase::Mem,
                    "f5: mem write phase wrong");
        requirePhases(model);
        roundTrip(model);

        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        runPass(model, "cpu.st.pack-general-functions");
        const auto &packed = *model.cpuMapping();
        const auto &memBranch = branch(packed, CpuPhase::Mem);
        require(memBranch.children.size() == 1 && memBranch.ops.empty(), "f5: mem branch shape wrong");
        require(flatOps(packed, memBranch.children.front()) == memOrder,
                "f5: mem function does not hold exactly the four writes");
        const auto *regPack = supernodeOf(packed, wr);
        require(regPack && containsOp(packed, *regPack, producerOf(model, rc)) &&
                containsOp(packed, *regPack, producerOf(model, rn)),
                "f5: regWrite did not merge with its cone");
        require(regPack->attrs.eventActs && *regPack->attrs.eventActs == std::vector<int64_t>{0},
                "f5: regWrite supernode eventActs wrong");
        const auto *latchPack = supernodeOf(packed, wl);
        require(latchPack && containsOp(packed, *latchPack, producerOf(model, lc)) &&
                containsOp(packed, *latchPack, producerOf(model, ln)),
                "f5: latchWrite did not merge with its cone");
        for (auto coneValue : {mwC, mwD, mfC, maC, ms1, ms2, ms3, ms4})
            require(supernodeOf(packed, producerOf(model, coneValue)),
                    "f5: mem operand cone left the general branch");
        roundTrip(model);
    }

    // Flat Event/Output branches keep a deterministic dependency order even
    // when ops were inserted out of order, and the full pipeline produces the
    // expected stage-by-stage partition shapes (no ActiveWord layer).
    void flatBranchesTest()
    {
        GrhSimModel model("f6_flat_branches");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clkPort = model.addInput("clk", bit);
        const auto clk = model.addValue(bit, "clk");
        const auto nclk = model.addValue(bit, "nclk");
        // Deliberately insert the Event ops out of dependency order.
        const auto det = addEdgeDet(model, nclk, 0);
        const auto notOp = model.addOperation("core.compute.not", std::array{clk}, std::array{nclk});
        model.setOperationPhase(notOp, SimPhase::Event);
        const auto readOp = model.addOperation("core.input.read", {}, std::array{clk},
                                               std::array{ObjectRef::input(clkPort)});
        model.setOperationPhase(readOp, SimPhase::Event);
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        addRegWrite(model, one, d, one, q, {0});
        const auto memType = model.arrayType(bit, 16);
        const auto mem = addState(model, "mem", memType, "0");
        const std::array memRefs{ObjectRef::state(mem)};
        const auto mw = model.addOperation("core.state.memWrite", std::array{one, one, d, one}, {}, memRefs);
        // Output cone, also inserted write-before-read.
        const auto outPort = model.addOutput("o", bit);
        const auto os = addState(model, "os", bit, "1'b0");
        const auto ov = model.addValue(bit, "ov");
        const std::array outRefs{ObjectRef::output(outPort)};
        const auto ow = model.addOperation("core.output.write", std::array{ov}, {}, outRefs);
        model.setOperationPhase(ow, SimPhase::Output);
        const std::array osRefs{ObjectRef::state(os)};
        const auto or_ = model.addOperation("core.state.read", {}, std::array{ov}, osRefs);
        model.setOperationPhase(or_, SimPhase::Output);
        require(verifies(model), "f6: fixture rejected");

        runPass(model, "cpu.st.split-phases");
        const auto &mapping = *model.cpuMapping();
        require(!model.mappings().front().complete, "f6: six-phase mapping must stay incomplete");
        require(mapping.partitionTree.partitions.size() == 5, "f6: split tree shape wrong");
        const auto &root = mapping.partitionTree.partitions[mapping.partitionTree.root.index - 1];
        const std::array<CpuPhase, 4> order{CpuPhase::Event, CpuPhase::General,
                                            CpuPhase::Mem, CpuPhase::Output};
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            const auto &child = mapping.partitionTree.partitions[root.children[i].index - 1];
            require(child.attrs.kind == CpuPartitionKind::Phase && child.attrs.phase == order[i],
                    "f6: root branch order wrong");
        }
        const std::vector<OpId> eventOrder{readOp, notOp, det};
        require(branch(mapping, CpuPhase::Event).ops == eventOrder,
                "f6: event branch did not topologically sort the cone");
        const std::vector<OpId> outputOrder{or_, ow};
        require(branch(mapping, CpuPhase::Output).ops == outputOrder,
                "f6: output branch did not topologically sort the cone");
        require(branch(mapping, CpuPhase::Mem).ops == std::vector<OpId>{mw}, "f6: mem branch wrong");
        roundTrip(model);

        runPass(model, "cpu.st.build-general-nodes");
        const auto &noded = *model.cpuMapping();
        require(noded.stage == CpuMappingStage::GeneralNodes, "f6: node stage wrong");
        const auto &generalNodes = branch(noded, CpuPhase::General);
        require(!generalNodes.children.empty() && generalNodes.ops.empty(), "f6: general nodes missing");
        roundTrip(model);

        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &merged = *model.cpuMapping();
        require(merged.stage == CpuMappingStage::GeneralSupernodes, "f6: supernode stage wrong");
        for (const auto *supernode : supernodes(merged))
            require(supernode->attrs.eventActs.has_value(), "f6: supernode lost its eventActs annotation");
        roundTrip(model);

        runPass(model, "cpu.st.pack-general-functions");
        const auto &packed = *model.cpuMapping();
        require(packed.stage == CpuMappingStage::GeneralFunctions, "f6: function stage wrong");
        require(countKind(packed, CpuPartitionKind::ActiveWord) == 0,
                "f6: six-phase tree must not contain active words");
        require(countKind(packed, CpuPartitionKind::EventDomain) == 0,
                "f6: six-phase tree must not contain event domains");
        for (auto phase : {CpuPhase::Event, CpuPhase::Mem, CpuPhase::Output})
        {
            const auto &flat = branch(packed, phase);
            require(flat.children.size() == 1 && flat.ops.empty(), "f6: flat branch shape wrong");
            const auto &function = packed.partitionTree.partitions[flat.children.front().index - 1];
            require(function.attrs.kind == CpuPartitionKind::EmitFunction &&
                    function.children.empty(), "f6: flat function shape wrong");
        }
        require(flatOps(packed, branch(packed, CpuPhase::Event).children.front()) == eventOrder,
                "f6: event function lost the cone order");
        require(flatOps(packed, branch(packed, CpuPhase::Mem).children.front()) == std::vector<OpId>{mw},
                "f6: mem function wrong");
        require(flatOps(packed, branch(packed, CpuPhase::Output).children.front()) == outputOrder,
                "f6: output function wrong");
        roundTrip(model);

        runPass(model, "cpu.st.split-phases");
        require(model.cpuMapping()->stage == CpuMappingStage::SplitPhases &&
                model.cpuMapping()->partitionTree.partitions.size() == 5,
                "f6: split-phases rerun did not reset the tree");
        roundTrip(model);
    }

    // The verifier rejects corrupted six-phase mappings; the new passes fail
    // cleanly on missing prerequisites and invalid options.
    void verifierRejectsTest()
    {
        TwoClockFixture fixture("f7_rejects");
        auto &model = fixture.model;
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto x = addNot(model, fixture.d, "x");
        const auto one2 = addConstant(model, bit, "1'b1");
        const auto q1 = addState(model, "q1", bit, "1'b0");
        const auto q2 = addState(model, "q2", bit, "1'b0");
        const auto w1 = addRegWrite(model, fixture.one, x, fixture.one, q1, {0});
        const auto w2 = addRegWrite(model, one2, x, one2, q2, {1});
        require(verifies(model), "f7: fixture rejected");
        runPass(model, "cpu.st.split-phases");
        const auto splitMapping = *model.cpuMapping();
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        const auto mergedMapping = *model.cpuMapping();

        const auto reject = [&](CpuBackendMapping bad) {
            auto broken = model.clone();
            broken.setCpuMapping(std::move(bad));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics) &&
                    diagnostics.hasError(), "verifier accepted a corrupted six-phase mapping");
        };
        // (a) eventActs attr tampered to a different set.
        auto bad = mergedMapping;
        for (auto &partition : bad.partitionTree.partitions)
            if (partition.attrs.eventActs && *partition.attrs.eventActs == std::vector<int64_t>{0})
            { partition.attrs.eventActs = std::vector<int64_t>{1}; break; }
        reject(bad);
        // (b) rule 2 violation with a consistent attr: the write-feeding cone
        // node moved into a write's supernode; the attr still matches the op
        // scan and uniformity holds, but the downstream closure reaches the
        // other domain. Uses an F4-style model so the donor supernode keeps
        // its remaining nodes.
        {
            TwoClockFixture f4("f7_rule2");
            auto &m4 = f4.model;
            const auto a4 = addNot(m4, f4.d, "a");
            const auto b4 = addNot(m4, a4, "b");
            const auto q41 = addState(m4, "q1", bit, "1'b0");
            const auto q42 = addState(m4, "q2", bit, "1'b0");
            addRegWrite(m4, f4.one, b4, f4.one, q41, {0});
            addRegWrite(m4, f4.one, b4, f4.one, q42, {1});
            runPass(m4, "cpu.st.split-phases");
            const std::array<std::string_view, 2> singleNode{"--max-op-in-compute-node", "1"};
            runPass(m4, "cpu.st.build-general-nodes", singleNode);
            runPass(m4, "cpu.st.merge-general-supernodes");
            auto corrupt = *m4.cpuMapping();
            const auto bOp = producerOf(m4, b4);
            PartitionId bNode, donor;
            for (const auto &partition : corrupt.partitionTree.partitions)
            {
                if (partition.attrs.kind != CpuPartitionKind::Node) continue;
                if (std::find(partition.ops.begin(), partition.ops.end(), bOp) != partition.ops.end())
                { bNode = partition.id; donor = partition.parent; }
            }
            require(bNode && donor, "f7: cone node missing for corruption");
            PartitionId target;
            for (const auto &partition : corrupt.partitionTree.partitions)
                if (partition.attrs.kind == CpuPartitionKind::Supernode && partition.attrs.eventActs &&
                    *partition.attrs.eventActs == std::vector<int64_t>{0})
                    target = partition.id;
            require(bool(target), "f7: write supernode missing for corruption");
            auto &donorChildren = corrupt.partitionTree.partitions[donor.index - 1].children;
            donorChildren.erase(std::find(donorChildren.begin(), donorChildren.end(), bNode));
            require(!donorChildren.empty(), "f7: donor supernode would be empty");
            corrupt.partitionTree.partitions[target.index - 1].children.push_back(bNode);
            corrupt.partitionTree.partitions[bNode.index - 1].parent = target;
            auto broken = m4.clone();
            broken.setCpuMapping(std::move(corrupt));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics) &&
                    diagnostics.hasError(), "f7: verifier accepted a rule-2 event domain violation");
        }
        // (c) SplitPhases coverage hole: a flat-branch op dropped.
        bad = splitMapping;
        for (auto &partition : bad.partitionTree.partitions)
            if (partition.attrs.kind == CpuPartitionKind::Phase &&
                partition.attrs.phase == CpuPhase::Event && !partition.ops.empty())
            { partition.ops.pop_back(); break; }
        reject(bad);
        // (d) a General op listed inside the Event branch.
        bad = splitMapping;
        {
            const auto general = opsOfType(model, "core.compute.not").front();
            for (auto &partition : bad.partitionTree.partitions)
                if (partition.attrs.kind == CpuPartitionKind::Phase &&
                    partition.attrs.phase == CpuPhase::Event)
                { partition.ops.push_back(general); break; }
        }
        reject(bad);
        // (e) a General supernode missing its eventActs annotation.
        bad = mergedMapping;
        for (auto &partition : bad.partitionTree.partitions)
            if (partition.attrs.kind == CpuPartitionKind::Supernode && partition.attrs.eventActs)
            { partition.attrs.eventActs.reset(); break; }
        reject(bad);

        // Missing prerequisites fail cleanly without touching the old mapping.
        std::string error;
        GrhSimModel fresh("f7_fresh");
        fresh.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        for (auto name : {"cpu.st.build-general-nodes", "cpu.st.merge-general-supernodes",
                          "cpu.st.pack-general-functions"})
        {
            auto pass = defaultPassRegistry().create(name, {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(fresh, diagnostics).success && !fresh.cpuMapping(),
                    std::string(name) + " accepted a missing split-phases prerequisite");
        }
        auto splitModel = model.clone();
        {
            CpuBackendMapping mapping = splitMapping;
            splitModel.setCpuMapping(std::move(mapping));
        }
        auto merge = defaultPassRegistry().create("cpu.st.merge-general-supernodes", {}, error);
        require(bool(merge), error);
        diag::Diagnostics mergeDiagnostics;
        require(!merge->run(splitModel, mergeDiagnostics).success &&
                splitModel.cpuMapping()->stage == CpuMappingStage::SplitPhases,
                "merge accepted a wrong-stage prerequisite");
        // The new passes also reject legacy-pipeline mappings.
        GrhSimModel legacy("f7_legacy");
        legacy.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        addInputRead(legacy, "d");
        runPass(legacy, "cpu.st.split-phase");
        auto general = defaultPassRegistry().create("cpu.st.build-general-nodes", {}, error);
        require(bool(general), error);
        diag::Diagnostics legacyDiagnostics;
        require(!general->run(legacy, legacyDiagnostics).success &&
                legacy.cpuMapping()->stage == CpuMappingStage::SplitPhase,
                "build-general-nodes accepted a legacy mapping");
        // Invalid options are rejected at creation.
        const std::array<std::string_view, 2> zeroLimit{"--max-op-in-compute-node", "0"};
        require(!defaultPassRegistry().create("cpu.st.build-general-nodes", zeroLimit, error),
                "accepted a zero node limit");
        const std::array<std::string_view, 2> unknownKey{"--unknown", "1"};
        require(!defaultPassRegistry().create("cpu.st.merge-general-supernodes", unknownKey, error),
                "accepted an unknown option");
        const std::array<std::string_view, 1> oddArgs{"--batch-max-ops"};
        require(!defaultPassRegistry().create("cpu.st.pack-general-functions", oddArgs, error),
                "accepted a value-less option");
        const std::array<std::string_view, 2> zeroTarget{"--target-batch-count", "0"};
        require(bool(defaultPassRegistry().create("cpu.st.pack-general-functions", zeroTarget, error)),
                "rejected a zero target batch count");
        const std::array<std::string_view, 2> splitArgs{"--max-op-in-compute-node", "1"};
        require(!defaultPassRegistry().create("cpu.st.split-phases", splitArgs, error),
                "split-phases accepted arguments");
        (void)w1; (void)w2;
    }

    // An empty model flows through all four passes.
    void emptyModelTest()
    {
        GrhSimModel model("f8_empty");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        runPass(model, "cpu.st.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        runPass(model, "cpu.st.pack-general-functions");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::GeneralFunctions, "f8: final stage wrong");
        require(branch(mapping, CpuPhase::General).children.empty(), "f8: general branch not empty");
        for (auto phase : {CpuPhase::Event, CpuPhase::Mem, CpuPhase::Output})
        {
            const auto &flat = branch(mapping, phase);
            require(flat.children.size() == 1 &&
                    flatOps(mapping, flat.children.front()).empty(), "f8: flat function missing");
        }
        roundTrip(model);
    }
}

int main()
{
    try
    {
        sameDomainMergeTest();
        subsetDomainBlockedTest();
        crossDomainBlockedTest();
        combExemptTest();
        memWriteSplitTest();
        flatBranchesTest();
        verifierRejectsTest();
        emptyModelTest();
        crossDomainStateReadTest();
        std::cout << "CPU six-phase mapping tests passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
