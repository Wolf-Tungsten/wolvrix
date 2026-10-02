#include "grhsim/backend/cpu.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
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

    // M5d-6 mapping entry: the A7 store classification feeds B5's class-aware
    // attribution and C3's layout; C1 initializes the one final mapping.
    // allMem forces every array into the mem store class (mem-min-bytes=0) so
    // mem-write branch tests keep their P_mem shape.
    void attributeAndInit(GrhSimModel &model, bool allMem = false)
    {
        if (allMem)
            runPass(model, "grhsim.select-state-stores",
                    std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        else
            runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
    }

    // C3 -> C4 -> C5 -> C6: the mapping advances from GeneralSupernodes to
    // GeneralFunctions (function packing moved between the mem write plan and
    // the phase schedule in M5d-6).
    void advanceToFunctions(GrhSimModel &model)
    {
        runPass(model, "cpu.st.layout-named-stores");
        runPass(model, "cpu.st.build-event-bitmaps");
        runPass(model, "cpu.st.build-mem-write-plan");
        runPass(model, "cpu.st.pack-general-functions");
    }

    // General branch shape from C6 on: supernodes in ordinal order, then the
    // EmitFunction leaves whose ranges tile [0, supernodeCount).
    void requirePackedGeneralBranch(const CpuBackendMapping &mapping, const char *tag)
    {
        const auto &general = branch(mapping, CpuPhase::General);
        const auto &tree = mapping.partitionTree;
        uint32_t supernodeCount = 0, rangeEnd = 0;
        bool seenFunction = false;
        for (const auto child : general.children)
        {
            const auto &node = tree.partitions[child.index - 1];
            if (node.attrs.kind == CpuPartitionKind::Supernode)
            {
                require(!seenFunction, std::string(tag) + ": supernode trails an emit function");
                ++supernodeCount;
                continue;
            }
            require(node.attrs.kind == CpuPartitionKind::EmitFunction && node.attrs.supernodeRange,
                    std::string(tag) + ": general branch holds a non-function leaf");
            seenFunction = true;
            require(node.attrs.supernodeRange->offset == rangeEnd && node.attrs.supernodeRange->count != 0,
                    std::string(tag) + ": emit function range is not contiguous");
            rangeEnd += node.attrs.supernodeRange->count;
        }
        require(rangeEnd == supernodeCount, std::string(tag) +": emit function ranges do not tile");
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

        runPass(model, "grhsim.select-state-stores");
        const auto split = runPass(model, "grhsim.split-phases");
        require(infoValue(split, "grhsim.split-phases", "attributed=") == std::optional<uint64_t>(3),
                "f1: semantic split attribution count wrong");
        requirePhases(model);
        const auto built = runPass(model, "cpu.st.build-general-nodes");
        require(infoValue(built, "cpu.st.build-general-nodes", "event_ops=") == std::optional<uint64_t>(2),
                "f1: build event op count wrong");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::GeneralNodes, "f1: stage after node formation wrong");
        require(branch(mapping, CpuPhase::Event).ops.size() == 2, "f1: event branch wrong");
        require(branch(mapping, CpuPhase::General).ops.empty() &&
                !branch(mapping, CpuPhase::General).children.empty(), "f1: general nodes missing");
        roundTrip(model);

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

        advanceToFunctions(model);
        const auto &packed = *model.cpuMapping();
        require(packed.stage == CpuMappingStage::GeneralFunctions, "f1: final stage wrong");
        require(branch(packed, CpuPhase::Event).children.size() == 1 &&
                branch(packed, CpuPhase::Mem).children.size() == 1 &&
                branch(packed, CpuPhase::Output).children.size() == 1, "f1: flat functions missing");
        require(flatOps(packed, branch(packed, CpuPhase::Event).children.front()).size() == 2,
                "f1: event function ops wrong");
        require(supernodes(packed).size() == 1, "f1: packing lost the supernode");
        requirePackedGeneralBranch(packed, "f1");
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

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
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
        advanceToFunctions(model);
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

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
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
        advanceToFunctions(model);
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

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
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

        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
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
        advanceToFunctions(model);
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

        // f5 pins the P_mem shape: B5 runs before any classification, so its
        // unclassified-model fallback attributes every array write to the Mem
        // phase; the classification forced to all-mem below keeps C3
        // consistent with that attribution.
        const auto split = runPass(model, "grhsim.split-phases");
        require(infoValue(split, "grhsim.split-phases", "phase_mem=") == std::optional<uint64_t>(4),
                "f5: mem attribution count wrong");
        requirePhases(model);
        runPass(model, "cpu.st.build-general-nodes");
        const auto &mapping = *model.cpuMapping();
        const std::vector<OpId> memOrder{mw, mf, ma, mws};
        require(branch(mapping, CpuPhase::Mem).ops == memOrder, "f5: mem branch is not flat op-id order");
        for (auto opId : memOrder)
            require(model.operations()[opId.index - 1].phase == SimPhase::Mem,
                    "f5: mem write phase wrong");
        roundTrip(model);

        runPass(model, "grhsim.select-state-stores",
                std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        runPass(model, "cpu.st.merge-general-supernodes");
        advanceToFunctions(model);
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

        runPass(model, "grhsim.select-state-stores",
                std::array<std::string_view, 2>{"--mem-min-bytes", "0"});
        const auto split = runPass(model, "grhsim.split-phases");
        require(infoValue(split, "grhsim.split-phases", "phase_mem=") == std::optional<uint64_t>(1),
                "f6: semantic split mem attribution wrong");
        // C1 initializes the mapping and forms the General nodes in one pass.
        runPass(model, "cpu.st.build-general-nodes");
        const auto &mapping = *model.cpuMapping();
        require(!model.mappings().front().complete, "f6: six-phase mapping must stay incomplete");
        const auto &root = mapping.partitionTree.partitions[mapping.partitionTree.root.index - 1];
        const std::array<CpuPhase, 4> order{CpuPhase::Event, CpuPhase::General,
                                            CpuPhase::Mem, CpuPhase::Output};
        require(root.children.size() == order.size(), "f6: root branch count wrong");
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
        require(mapping.stage == CpuMappingStage::GeneralNodes, "f6: node stage wrong");
        const auto &generalNodes = branch(mapping, CpuPhase::General);
        require(!generalNodes.children.empty() && generalNodes.ops.empty(), "f6: general nodes missing");
        roundTrip(model);

        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &merged = *model.cpuMapping();
        require(merged.stage == CpuMappingStage::GeneralSupernodes, "f6: supernode stage wrong");
        for (const auto *supernode : supernodes(merged))
            require(supernode->attrs.eventActs.has_value(), "f6: supernode lost its eventActs annotation");
        roundTrip(model);

        advanceToFunctions(model);
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
        requirePackedGeneralBranch(packed, "f6");
        roundTrip(model);

        // C1 re-runs discard and reinitialize the mapping (the pipeline's
        // single mapping-init point).
        runPass(model, "cpu.st.build-general-nodes");
        require(model.cpuMapping()->stage == CpuMappingStage::GeneralNodes &&
                branch(*model.cpuMapping(), CpuPhase::General).children.size() ==
                    branch(mapping, CpuPhase::General).children.size(),
                "f6: build-general-nodes rerun did not reset the tree");
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
        runPass(model, "grhsim.select-state-stores");
        runPass(model, "grhsim.split-phases");
        runPass(model, "cpu.st.build-general-nodes");
        const auto splitMapping = *model.cpuMapping();
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
            runPass(m4, "grhsim.select-state-stores");
            runPass(m4, "grhsim.split-phases");
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
        // (c) coverage hole: a flat-branch op dropped from the mapping.
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

        // Missing prerequisites fail cleanly. C1 requires total phase
        // attribution (B5's job); C2/C6 require their input-stage mappings.
        std::string error;
        {
            GrhSimModel unattributed("f7_unattributed");
            unattributed.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            addInputRead(unattributed, "d");
            auto pass = defaultPassRegistry().create("cpu.st.build-general-nodes", {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(unattributed, diagnostics).success && !unattributed.cpuMapping(),
                    "build-general-nodes accepted an unattributed model");
        }
        GrhSimModel fresh("f7_fresh");
        fresh.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        for (auto name : {"cpu.st.merge-general-supernodes", "cpu.st.pack-general-functions"})
        {
            auto pass = defaultPassRegistry().create(name, {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(fresh, diagnostics).success && !fresh.cpuMapping(),
                    std::string(name) + " accepted a missing prerequisite mapping");
        }
        auto mergeModel = model.clone();
        {
            CpuBackendMapping mapping = mergedMapping;
            mergeModel.setCpuMapping(std::move(mapping));
        }
        auto merge = defaultPassRegistry().create("cpu.st.merge-general-supernodes", {}, error);
        require(bool(merge), error);
        diag::Diagnostics mergeDiagnostics;
        require(!merge->run(mergeModel, mergeDiagnostics).success &&
                mergeModel.cpuMapping()->stage == CpuMappingStage::GeneralSupernodes,
                "merge accepted a wrong-stage prerequisite");
        auto pack = defaultPassRegistry().create("cpu.st.pack-general-functions", {}, error);
        require(bool(pack), error);
        diag::Diagnostics packDiagnostics;
        require(!pack->run(mergeModel, packDiagnostics).success &&
                mergeModel.cpuMapping()->stage == CpuMappingStage::GeneralSupernodes,
                "pack-general-functions accepted a pre-mem-plan mapping");
        // The removed mapping initializers and the legacy two-phase line are
        // no longer registered (M5d-6).
        for (auto name : {"cpu.st.split-phases", "cpu.st.split-phase", "cpu.st.form-event-domains",
                          "cpu.st.build-compute-nodes", "cpu.st.merge-compute-supernodes",
                          "cpu.st.pack-active-words", "cpu.st.pack-emit-functions", "cpu.st.layout-data",
                          "cpu.st.build-schedule", "grhsim.demonitor-redundant",
                          "grhsim.demonitor-edge-completion", "grhsim.migrate-boundary-ops",
                          "grhsim.migrate-boundary-ops-ec", "grhsim.fuse-expr-chains",
                          "grhsim.fold-residue"})
        {
            error.clear();
            require(!defaultPassRegistry().create(name, {}, error),
                    std::string(name) + " is still registered");
        }
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
        (void)w1; (void)w2;
    }

    // An empty model flows through the whole C segment (C1 initializes the
    // mapping directly — no attribution is needed when there are no ops).
    void emptyModelTest()
    {
        GrhSimModel model("f8_empty");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        runPass(model, "cpu.st.build-general-nodes");
        runPass(model, "cpu.st.merge-general-supernodes");
        advanceToFunctions(model);
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
    // M5d-6 C1 contract: a regLatch-class array's mem writes are General-phase
    // ops (B5's class-aware attribution), anchor their own nodes inside the
    // General branch, and leave the flat Mem branch empty — the mapping
    // verifier accepts them there (op/branch phase agreement is by SimPhase).
    void regLatchMemWriteTest()
    {
        GrhSimModel model("f10_reglatch_mem_write");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto tabType = model.arrayType(bit, 4); // 4 x 1 bit: regLatch class
        const auto clk = addInputRead(model, "clk", SimPhase::Event);
        addEdgeDet(model, clk, 0);
        const auto en = addInputRead(model, "en");
        const auto addr = addInputRead(model, "addr");
        const auto data = addInputRead(model, "data");
        const auto one = addConstant(model, bit, "1'b1");
        const auto tab = addState(model, "tab", tabType, "0");
        const std::array writeRefs{ObjectRef::state(tab)};
        const std::array writeParams{Parameter{model.intern("event_acts"), std::vector<int64_t>{0}}};
        const auto write = model.addOperation("core.state.memWrite", std::array{en, addr, data, one}, {},
                                              writeRefs, writeParams);
        require(verifies(model), "f10: fixture rejected");

        runPass(model, "grhsim.select-state-stores");
        require(model.states()[tab.index - 1].storeClass == StateStoreClass::RegLatch,
                "f10: small array was not classified regLatch");
        const auto split = runPass(model, "grhsim.split-phases");
        require(infoValue(split, "grhsim.split-phases", "mem_writes_reglatch=") == std::optional<uint64_t>(1),
                "f10: regLatch mem write was not kept on the General path");
        require(infoValue(split, "grhsim.split-phases", "phase_mem=") == std::optional<uint64_t>(0),
                "f10: a mem write leaked into the Mem phase");
        require(model.operations()[write.index - 1].phase == SimPhase::General,
                "f10: regLatch mem write phase wrong");
        runPass(model, "cpu.st.build-general-nodes");
        const auto &mapping = *model.cpuMapping();
        require(branch(mapping, CpuPhase::Mem).ops.empty(), "f10: mem branch is not empty");
        runPass(model, "cpu.st.merge-general-supernodes");
        const auto &merged = *model.cpuMapping();
        const auto *pack = supernodeOf(merged, write);
        require(pack, "f10: regLatch mem write is not inside a general supernode");
        require(pack->attrs.eventActs && *pack->attrs.eventActs == std::vector<int64_t>{0},
                "f10: write supernode eventActs wrong");
        roundTrip(model);
        advanceToFunctions(model);
        const auto &packed = *model.cpuMapping();
        const auto &memBranch = branch(packed, CpuPhase::Mem);
        require(memBranch.children.size() == 1 &&
                flatOps(packed, memBranch.children.front()).empty(), "f10: mem function is not empty");
        requirePackedGeneralBranch(packed, "f10");
        roundTrip(model);
    }

    // M5d-7 (C8): plan-translation-units closes the mapping at the
    // TranslationUnits stage with a verifiable TU plan: one Core chunk, chunk
    // streams tiling their ranges, scan chunks tiling [0, supernodeCount),
    // every supernode assigned exactly once. Tiny caps split more units; the
    // plan survives the JSON round trip; the verifier replans with the
    // recorded caps and catches corruption.
    void translationUnitsTest()
    {
        GrhSimModel model("f11_translation_units");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto clk = addInputRead(model, "clk", SimPhase::Event);
        addEdgeDet(model, clk, 0);
        const auto d = addInputRead(model, "d");
        const auto x = addNot(model, d, "x");
        const auto one = addConstant(model, bit, "1'b1");
        const auto q = addState(model, "q", bit, "1'b0");
        addRegWrite(model, one, x, one, q, {0});
        const auto memType = model.arrayType(bit, 16);
        const auto mem = addState(model, "mem", memType, "0");
        model.addOperation("core.state.memWrite", std::array{one, one, d, one}, {},
                           std::array{ObjectRef::state(mem)});
        const auto outPort = model.addOutput("o", bit);
        const auto os = addState(model, "os", bit, "1'b0");
        const auto ov = model.addValue(bit, "ov");
        const auto readOp = model.addOperation("core.state.read", {}, std::array{ov},
                                               std::array{ObjectRef::state(os)});
        model.setOperationPhase(readOp, SimPhase::Output);
        const auto writeOp = model.addOperation("core.output.write", std::array{ov}, {},
                                                std::array{ObjectRef::output(outPort)});
        model.setOperationPhase(writeOp, SimPhase::Output);
        require(verifies(model), "f11: fixture rejected");

        attributeAndInit(model, true);
        runPass(model, "cpu.st.merge-general-supernodes");
        advanceToFunctions(model);
        // C8 rejects a pre-PhaseSchedule mapping.
        {
            auto early = model.clone();
            std::string error;
            auto pass = defaultPassRegistry().create("cpu.st.plan-translation-units", {}, error);
            require(bool(pass), error);
            diag::Diagnostics diagnostics;
            require(!pass->run(early, diagnostics).success &&
                    early.cpuMapping()->stage == CpuMappingStage::GeneralFunctions,
                    "f11: plan-translation-units accepted a pre-schedule mapping");
        }
        runPass(model, "cpu.st.build-phase-schedule");
        require(model.cpuMapping()->stage == CpuMappingStage::PhaseSchedule, "f11: schedule stage wrong");
        require(model.mappings().front().complete, "f11: the phase-schedule stage completes the mapping");
        auto scheduled = model.clone();

        runPass(model, "cpu.st.plan-translation-units");
        const auto &mapping = *model.cpuMapping();
        require(mapping.stage == CpuMappingStage::TranslationUnits, "f11: TU stage wrong");
        require(model.mappings().front().complete, "f11: the terminal mapping must stay complete");
        require(mapping.translationUnits.has_value(), "f11: TU plan missing");
        const auto &plan = *mapping.translationUnits;
        require(plan.chunkMaxEstimatedLines == 2048 && plan.unitMaxEstimatedLines == 32768,
                "f11: default caps wrong");
        require(!plan.units.empty(), "f11: TU plan holds no units");
        const auto supernodeCount = supernodes(mapping).size();
        uint32_t coreChunks = 0, supernodeChunks = 0, scanEnd = 0;
        std::set<std::string> unitNames;
        std::vector<char> covered(supernodeCount);
        for (const auto &unit : plan.units)
        {
            require(unitNames.insert(unit.name).second, "f11: duplicate TU name");
            uint64_t lines = 0;
            for (const auto &chunk : unit.chunks) lines += chunk.estimatedLines;
            require(lines == unit.estimatedLines, "f11: unit size is not the chunk sum");
            for (const auto &chunk : unit.chunks)
            {
                if (chunk.kind == CpuEmitChunkKind::Core) ++coreChunks;
                if (chunk.kind == CpuEmitChunkKind::GeneralScan)
                {
                    require(chunk.offset == scanEnd, "f11: scan chunks do not tile");
                    scanEnd += chunk.count;
                }
                if (chunk.kind == CpuEmitChunkKind::Supernode)
                {
                    require(chunk.count == 1 && chunk.offset < supernodeCount && !covered[chunk.offset],
                            "f11: supernode chunk mis-covers");
                    covered[chunk.offset] = 1;
                    ++supernodeChunks;
                }
            }
        }
        require(coreChunks == 1, "f11: plan must hold exactly one core chunk");
        require(scanEnd == supernodeCount, "f11: scan chunks do not cover the supernodes");
        require(supernodeChunks == supernodeCount, "f11: supernode chunks do not cover the supernodes");
        roundTrip(model);

        // Tiny caps split the same model into strictly more units.
        const std::array<std::string_view, 4> tiny{"--chunk-max-estimated-lines", "8",
                                                   "--unit-max-estimated-lines", "32"};
        runPass(scheduled, "cpu.st.plan-translation-units", tiny);
        const auto &tinyPlan = *scheduled.cpuMapping()->translationUnits;
        require(tinyPlan.chunkMaxEstimatedLines == 8 && tinyPlan.unitMaxEstimatedLines == 32,
                "f11: tiny caps not recorded");
        require(tinyPlan.units.size() > plan.units.size(), "f11: tiny caps did not split the emit");
        roundTrip(scheduled);
        // C8 replans deterministically on an already-planned mapping (the C1
        // rebuild semantics) — re-running with the tiny caps on the planned
        // model yields exactly the tiny plan.
        runPass(model, "cpu.st.plan-translation-units", tiny);
        require(model.cpuMapping()->stage == CpuMappingStage::TranslationUnits,
                "f11: replan left the terminal stage");
        require(*model.cpuMapping()->translationUnits == tinyPlan, "f11: replan is not deterministic");

        // The verifier replans with the recorded caps and catches corruption.
        {
            auto bad = mapping;
            auto &planRef = bad.translationUnits;
            bool corruptedChunk = false;
            for (auto &unit : planRef->units)
                for (auto &chunk : unit.chunks)
                    if (!corruptedChunk && chunk.count > 1) { ++chunk.count; corruptedChunk = true; }
            if (!corruptedChunk) planRef->units.back().estimatedLines += 1;
            auto broken = model.clone();
            broken.setCpuMapping(std::move(bad));
            diag::Diagnostics diagnostics;
            require(!verifyGrhSimModel(broken, defaultDialectRegistry(), diagnostics) &&
                    diagnostics.hasError(), "f11: verifier accepted a corrupted TU plan");
        }
        // Invalid options are rejected at creation.
        std::string error;
        const std::array<std::string_view, 1> oddArgs{"--chunk-max-estimated-lines"};
        require(!defaultPassRegistry().create("cpu.st.plan-translation-units", oddArgs, error),
                "f11: accepted a value-less option");
        const std::array<std::string_view, 2> zeroCap{"--unit-max-estimated-lines", "0"};
        require(!defaultPassRegistry().create("cpu.st.plan-translation-units", zeroCap, error),
                "f11: accepted a zero unit cap");
        const std::array<std::string_view, 2> unknownKey{"--tu-count", "4"};
        require(!defaultPassRegistry().create("cpu.st.plan-translation-units", unknownKey, error),
                "f11: accepted an unknown option");
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
        regLatchMemWriteTest();
        translationUnitsTest();
        std::cout << "CPU six-phase mapping tests passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
