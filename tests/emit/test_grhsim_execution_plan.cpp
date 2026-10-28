#include "emit/grhsim_activity_model.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>

using namespace wolvrix::lib;
using namespace grh;
using namespace emit;
using namespace transform;

namespace
{
    void check(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
    template <class F> void rejects(F action)
    {
        try { action(); } catch (const std::invalid_argument &) { return; }
        throw std::runtime_error("invalid activity input accepted");
    }
    template <class T> void put(SessionStore &session, const std::string &key, T data)
    {
        session.emplace("top.activity_schedule." + key,
            std::make_unique<SessionSlotValue<T>>(std::move(data), "execution-plan-test"));
    }
    [[maybe_unused]] std::map<std::string, std::string> files(const EmitResult &result)
    {
        std::map<std::string, std::string> out;
        for (const auto &path : result.artifacts)
        {
            std::ifstream input(path);
            out.emplace(std::filesystem::path(path).filename(),
                        std::string(std::istreambuf_iterator<char>(input), {}));
        }
        return out;
    }

    void run(const std::string &emitOnly = {})
    {
        Design design;
        auto &graph = design.createGraph("top"); design.markAsTop("top");
        ActivityScheduleSupernodeToOps nodes(11);
        auto value = [&](const std::string &name, int32_t width) {
            return graph.createValue(graph.internSymbol(name), width, false, ValueType::Logic);
        };
        auto input = [&](const std::string &name, int32_t width) {
            auto v = value(name, width); graph.bindInputPort(name, v); return v;
        };
        auto op = [&](uint32_t group, OperationKind kind, const std::string &name,
                      std::vector<ValueId> inputs, int32_t width) {
            auto id = graph.createOperation(kind, graph.internSymbol(name + "_op"));
            for (auto v : inputs) graph.addOperand(id, v);
            auto out = value(name, width); graph.addResult(id, out); nodes[group].push_back(id); return out;
        };
        auto slice = [&](uint32_t group, const std::string &name, ValueId src, int64_t start, int32_t width) {
            auto out = op(group, OperationKind::kSliceStatic, name, {src}, width);
            graph.setAttr(graph.valueDef(out), "sliceStart", start);
            graph.setAttr(graph.valueDef(out), "sliceEnd", start + width - 1);
            graph.bindOutputPort(name, out); return out;
        };
        const auto a = input("clock_a", 1), b = input("clock_b", 1), x = input("x", 1);
        const auto bus = input("bus", 130);
        const auto one = op(0, OperationKind::kConstant, "one", {}, 1);
        graph.setAttr(graph.valueDef(one), "constValue", std::string("1'b1"));
        const auto inv = op(1, OperationKind::kNot, "inv", {a}, 1);
        const auto gated = op(1, OperationKind::kAnd, "gated", {a, x}, 1);
        const auto low = slice(2, "low", bus, 0, 1);
        const auto high = slice(3, "high", bus, 128, 1);
        const auto crossing = slice(10, "crossing", bus, 63, 3);
        ActivityScheduleStateReadSupernodes readers;
        const auto reg = [&](const std::string &name, ValueId clock, const std::string &edge,
                             uint32_t readGroup, uint32_t writeGroup) {
            auto id = graph.createOperation(OperationKind::kRegister, graph.internSymbol(name));
            graph.setAttr(id, "width", int64_t{1}); graph.setAttr(id, "isSigned", false);
            graph.setAttr(id, "initValue", std::string("1'b0"));
            const auto read = op(readGroup, OperationKind::kRegisterReadPort, name + "_read", {}, 1);
            graph.setAttr(graph.valueDef(read), "regSymbol", name);
            const auto out = op(readGroup, OperationKind::kNot, name + "_out", {read}, 1);
            graph.bindOutputPort(name + "_out", out);
            const auto write = graph.createOperation(OperationKind::kRegisterWritePort, graph.internSymbol(name + "_write"));
            for (auto v : {one, x, one, clock}) graph.addOperand(write, v);
            graph.setAttr(write, "regSymbol", name);
            graph.setAttr(write, "eventEdge", std::vector<std::string>{edge});
            nodes[writeGroup].push_back(write); readers[name] = {readGroup};
            return out;
        };
        const auto qa = reg("qa", a, "posedge", 4, 7);
        const auto qb = reg("qb", b, "negedge", 4, 7);
        const auto qi = reg("qi", inv, "posedge", 5, 8);
        const auto qu = reg("qu", gated, "posedge", 5, 8);
        const auto packed = op(6, OperationKind::kConcat, "packed", {qa, qb, qi, qu}, 4);
        graph.bindOutputPort("packed", packed);
        const auto unpacked = slice(9, "unpacked", packed, 3, 1);
        ActivityScheduleValueFanout fanout(graph.values().size());
        std::map<uint32_t, uint32_t> opGroups;
        for (uint32_t n = 0; n < nodes.size(); ++n) for (auto id : nodes[n]) opGroups[id.index] = n;
        for (uint32_t n = 0; n < nodes.size(); ++n) for (auto id : nodes[n])
        {
            const auto operation = graph.getOperation(id);
            for (auto v : operation.operands())
            {
                const auto producer = graph.valueDef(v);
                if (!producer.valid() || !opGroups.contains(producer.index) || opGroups.at(producer.index) != n)
                    fanout[v.index - 1].push_back(n);
            }
        }
        for (auto &targets : fanout)
        {
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        }
        ActivityScheduleTopoOrder order(nodes.size()); std::iota(order.begin(), order.end(), 0);
        SessionStore session;
        put(session, "supernode_to_ops", nodes); put(session, "topo_order", order);
        put(session, "value_fanout", fanout); put(session, "state_read_supernodes", readers);
        EmitDiagnostics diagnostics;
        EmitGrhSimCpp emitter(&diagnostics);
        EmitOptions options; options.session = &session;
        options.attributes = {{"input_bit_activation", "true"}, {"perf", "off"}, {"waveform", "off"},
                              {"sched_batch_max_ops", "2"}};
        if (!emitOnly.empty())
        {
            options.outputDir = emitOnly;
            check(emitter.emit(design, options).success, "standalone fixture emit failed");
            return;
        }
#ifndef WOLVRIX_EXECUTION_PLAN_EMIT_ONLY
        GrhSimExecutionPlan plan;
        const auto result = emitter.analyzeExecutionPlan(design, options, plan);
        check(result.success && result.artifacts.empty() && !diagnostics.hasError(), "read-only planner failed");
        check(plan.groups.size() == nodes.size() && plan.activeFlagBits == 8 && plan.writes.size() == 4,
              "planner dropped groups/writes");
        check(plan.events.size() == 4 && plan.batches.size() >= 2, "event/batch plan missing");
        std::vector<unsigned> membership(nodes.size());
        for (const auto &batch : plan.batches) for (const auto &word : batch.words) for (auto group : word.groups)
        {
            ++membership.at(group);
            check(plan.groups[group].activeId / 8 == word.activeWord, "active-word map disagrees");
            check(plan.groups[group].phase == batch.phase, "mixed compute/commit batch");
        }
        check(std::all_of(membership.begin(), membership.end(), [](auto n) { return n == 1; }), "batch membership is not exact");
        const auto busPlan = std::find_if(plan.inputs.begin(), plan.inputs.end(), [&](const auto &p) { return p.value == bus; });
        check(busPlan != plan.inputs.end() && busPlan->bitMasks.size() == 4, "actual bit-activation masks not exported");
        GrhSimActivitySeeds seeds;
        seeds.inputs[a] = {{0, 1, 3}}; seeds.inputs[b] = {{0, 1, 12}};
        seeds.inputs[bus] = {{0, 64, 1}, {64, 128, 8}, {128, 130, kGrhSimUnknownActivity}};
        seeds.events[a] = {1, 2, 3}; seeds.events[b] = {4, 8, 12};
        const auto activity = analyzeGrhSimActivity(graph, plan, seeds);
        const auto mask = [&](ValueId v) { return grhsimActivityInRange(activity.values[v.index], 0, graph.valueWidth(v)); };
        check(mask(low) == 1 && mask(high) == kGrhSimUnknownActivity, "boundary bit provenance collapsed");
        check(activity.values[crossing.index] == GrhSimActivityRanges({{0, 1, 1}, {1, 3, 8}}),
              "cross-word slice lost disjoint ranges");
        check(mask(qa) == 1 && mask(qb) == 8 && mask(qi) == 2 && mask(qu) == kGrhSimUnknownActivity,
              "independent clocks/inverted clock/unresolved gate attribution incorrect");
        check(activity.unresolvedEvents == 1 && mask(unpacked) == 1, "concat/slice or unresolved accounting incorrect");
        check(activity.computeGroupMasks[2] == 1 && activity.computeGroupMasks[3] == kGrhSimUnknownActivity,
              "input bit activation did not narrow group seeds");
        check(activity.computeGroupMasks[4] == 9 && activity.operationMasks[graph.valueDef(qa).index] == 1,
              "group execution must differ from per-value change");
        check(activity.computeGroupMasks[9] == (1 | 8 | 2 | kGrhSimUnknownActivity),
              "internal boundary activation must retain the actual whole-value frontier");
        check(activity.computeGroupMasks[7] == 0, "commit dispatch mistaken for change-driven compute");
        const auto coarse = analyzeGrhSimActivity(graph, plan, seeds, false);
        check(coarse.computeGroupMasks[2] == (1 | 8 | kGrhSimUnknownActivity), "whole-value control did not collapse bundle");
        seeds.inputs[bus][0].mask = 2; // Caller models a remote publication arriving next step.
        check(analyzeGrhSimActivity(graph, plan, seeds).computeGroupMasks[2] == 2, "explicit arrival scenario ignored");
        seeds.inputs[bus].push_back({1, 2, 1});
        rejects([&] { analyzeGrhSimActivity(graph, plan, seeds); });
        seeds.inputs[bus].pop_back();
        auto unsupported = plan; unsupported.inputFullpass = true;
        rejects([&] { analyzeGrhSimActivity(graph, unsupported, seeds); });
        rejects([&] { summarizeGrhSimWork(unsupported, activity); });

        options.outputDir = (std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_execution_plan").string();
        const auto emitted = emitter.emit(design, options);
        check(emitted.success, "normal emit failed");
        const auto before = files(emitted);
        check(emitter.analyzeExecutionPlan(design, options, plan).success && files(emitted) == before,
              "analysis touched emitted artifacts");
        check(files(emitter.emit(design, options)) == before, "analysis changed later generated C++");
        auto bad = options; bad.session = nullptr;
        check(!emitter.analyzeExecutionPlan(design, bad, plan).success && plan.groups.empty(), "failed analysis leaves stale plan");
#endif
    }
}

int main(int argc, char **argv)
{
    try { run(argc == 3 && std::string(argv[1]) == "--emit-only" ? argv[2] : "");
          std::cout << "GRHSIM EXECUTION PLAN PASS\n"; return 0; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
