#include "emit/grhsim_cpp.hpp"
#include "emit/grhsim_partition_work.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/repcut.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unordered_map>

using namespace wolvrix::lib;
using namespace grh;
using namespace emit;
using namespace transform;

namespace
{
    using K = OperationKind;
    using BodyKind = GrhSimExecutionPlan::CommitWriteBody::Kind;
    void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    template<class T> T attr(const Operation &op, std::string_view name)
    {
        const auto v = op.attr(name);
        if (!v || !std::holds_alternative<T>(*v)) throw std::runtime_error("missing fixture attribute");
        return std::get<T>(*v);
    }
    uint64_t total(const GrhSimWorkReport &report, const std::string &feature)
    {
        uint64_t sum = 0;
        for (const auto &[mask, features] : report.conditionalFeatures)
            if (const auto it = features.find(feature); it != features.end()) sum += it->second;
        return sum;
    }
    Design fixture(bool reads)
    {
        Design design; auto &g = design.createGraph("top"); design.markAsTop("top");
        const auto val = [&](const std::string &name, int width) { return g.createValue(g.internSymbol(name), width, false); };
        const auto input = [&](const std::string &name, int width) {
            auto v = val(name, width); g.bindInputPort(name, v); return v;
        };
        const auto constant = [&](const std::string &name, int width, const std::string &literal) {
            auto op = g.createOperation(K::kConstant, g.internSymbol(name + "_op"));
            auto v = val(name, width); g.addResult(op, v); g.setAttr(op, "constValue", literal); return v;
        };
        const auto clk = input("clock", 1), en = input("enable", 1);
        const auto one = constant("one", 8, "8'h01"), mask = constant("mask", 8, "8'hff");
        const unsigned count = reads ? 3 : 4;
        std::vector<ValueId> q;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto name = "r" + std::to_string(i);
            auto state = g.createOperation(K::kRegister, g.internSymbol(name));
            g.setAttr(state, "width", int64_t{8}); g.setAttr(state, "isSigned", false);
            g.setAttr(state, "initValue", std::string("8'h00"));
            auto read = g.createOperation(K::kRegisterReadPort, g.internSymbol(name + "_read"));
            auto v = val(name + "_q", 8); g.addResult(read, v); g.setAttr(read, "regSymbol", name);
            g.setAttr(read, "test.state_id", static_cast<int64_t>(i)); q.push_back(v);
        }
        for (unsigned i = 0; i < count; ++i)
        {
            const auto name = "r" + std::to_string(i);
            auto next = g.createOperation(K::kAdd, g.internSymbol(name + "_next_op"));
            g.addOperand(next, reads && i == 1 ? q[0] : input("d" + std::to_string(i), 8));
            g.addOperand(next, one); auto v = val(name + "_next", 8); g.addResult(next, v);
            g.setAttr(next, "test.consumer_id", static_cast<int64_t>(i));
            auto wr = g.createOperation(K::kRegisterWritePort, g.internSymbol(name + "_write"));
            for (auto x : {en, v, mask, clk}) g.addOperand(wr, x);
            g.setAttr(wr, "regSymbol", name);
            g.setAttr(wr, "eventEdge", std::vector<std::string>{"posedge"});
            g.setAttr(wr, "test.writer_id", static_cast<int64_t>(i));
        }
        auto pack = g.createOperation(K::kConcat, g.internSymbol("observe_op"));
        for (auto v : q) g.addOperand(pack, v);
        auto out = val("observed", 8 * count); g.addResult(pack, out); g.bindOutputPort("observed", out);
        return design;
    }
    struct Unit
    {
        uint32_t part = 0;
        Graph *graph = nullptr;
        std::unordered_map<ValueId, ValueId, ValueIdHash> parentInputs;
        SessionStore session;
        GrhSimExecutionPlan plan;
        GrhSimActivityReport activity;
        EmitOptions options;
    };
    struct Driver { std::size_t unit; ValueId value; };
    struct Result
    {
        std::vector<GrhSimPartitionWork> work;
        std::map<uint32_t, std::vector<std::size_t>> sameReaderBodies;
        std::map<uint32_t, uint64_t> consumerMasks;
        std::map<uint32_t, uint64_t> consumerExecutionMasks;
        std::map<uint32_t, std::size_t> remoteInputs;
        std::map<std::string, std::string> generated;
    };

    Result rebuild(bool reads, const std::vector<uint32_t> &assignment, const std::filesystem::path &out)
    {
        auto design = fixture(reads);
        SessionStore replay;
        replay.emplace("top.repcut.replay_assignment", std::make_unique<SessionSlotValue<RepcutReplayAssignment>>(
            RepcutReplayAssignment{assignment}));
        PassManager manager; manager.options().session = &replay;
        RepcutOptions partition; partition.path = "top"; partition.partitionCount = 3;
        partition.partitioner = "assignment-replay"; partition.costModel = RepcutCostModel::kGrhsimV1;
        // Replay must also bypass the closure refinement for this mode.
        partition.weightMode = RepcutWeightMode::kClosureAware;
        partition.keepIntermediateFiles = true; partition.workDir = (out / "repcut").string();
        manager.addPass(std::make_unique<RepcutPass>(partition));
        PassDiagnostics diagnostics;
        check(manager.run(design, diagnostics).success, "fixture assignment replay failed");
        auto &top = *design.findGraph(design.topGraphs().at(0));
        const auto clock = top.inputPortValue("clock");
        std::vector<Unit> units;
        std::unordered_map<ValueId, Driver, ValueIdHash> drivers;
        for (auto id : top.operations())
        {
            const auto inst = top.getOperation(id); if (inst.kind() != K::kInstance) continue;
            Unit unit;
            const std::string instance = attr<std::string>(inst, "instanceName");
            check(instance.starts_with("part_"), "unexpected fixture instance identity");
            unit.part = std::stoul(instance.substr(5));
            unit.graph = design.findGraph(attr<std::string>(inst, "moduleName"));
            check(unit.graph != nullptr, "rebuilt child graph missing");
            const auto ins = attr<std::vector<std::string>>(inst, "inputPortName");
            const auto outs = attr<std::vector<std::string>>(inst, "outputPortName");
            for (std::size_t i = 0; i < ins.size(); ++i)
                unit.parentInputs.emplace(unit.graph->inputPortValue(ins[i]), inst.operands()[i]);
            for (std::size_t i = 0; i < outs.size(); ++i)
                drivers.emplace(inst.results()[i], Driver{units.size(), unit.graph->outputPortValue(outs[i])});
            units.push_back(std::move(unit));
        }
        check(units.size() == 3, "fixture lost a partition");
        // Plain metadata lets the diagnostic runner compile each model in a
        // separate TU (their common runtime helpers cannot share one TU).
        const auto outputName = [](const Graph &g, ValueId value) {
            for (const auto &port : g.outputPorts()) if (port.value == value) return port.name;
            throw std::runtime_error("fixture output value is not bound");
        };
        std::ofstream wiring(out / "wiring.tsv");
        wiring << "fixture\t" << reads << '\t' << (reads ? 3 : 4) << '\n';
        for (const auto &u : units)
        {
            wiring << "unit\t" << u.part << '\t' << u.graph->symbol() << '\n';
            for (const auto &port : u.graph->inputPorts())
            {
                const auto parent = u.parentInputs.at(port.value);
                wiring << "input\t" << u.part << '\t' << port.name << '\t' << u.graph->valueWidth(port.value) << '\t';
                if (const auto driver = drivers.find(parent); driver != drivers.end())
                {
                    const auto &source = units[driver->second.unit];
                    wiring << "remote\t" << source.part << '\t' << outputName(*source.graph, driver->second.value) << '\n';
                }
                else
                {
                    std::string primary;
                    for (const auto &p : top.inputPorts()) if (p.value == parent) primary = p.name;
                    check(!primary.empty(), "runtime fixture expects directly bound primary inputs");
                    wiring << "primary\t" << primary << '\n';
                }
            }
            for (const auto &port : u.graph->outputPorts())
                wiring << "output\t" << u.part << '\t' << port.name << '\t' << u.graph->valueWidth(port.value) << '\n';
        }
        const auto observer = drivers.at(top.outputPortValue("observed"));
        wiring << "observe\t" << units[observer.unit].part << '\t'
               << outputName(*units[observer.unit].graph, observer.value) << '\n';
        std::vector<unsigned> writerVisits(reads ? 3 : 4);
        for (auto &u : units)
        {
            for (auto id : u.graph->operations())
                if (u.graph->getOperation(id).kind() == K::kRegisterWritePort)
                {
                    const auto writer = attr<int64_t>(u.graph->getOperation(id), "test.writer_id");
                    check(u.part == assignment.at(writer), "replay or refinement changed writer ownership");
                    ++writerVisits.at(writer);
                }
            PassManager scheduler; scheduler.options().session = &u.session;
            ActivityScheduleOptions options; options.path = u.graph->symbol();
            scheduler.addPass(std::make_unique<ActivitySchedulePass>(options)); PassDiagnostics scheduleDiags;
            check(scheduler.run(design, scheduleDiags).success, "rebuilt partition schedule failed");
            u.options.session = &u.session; u.options.topOverrides = {u.graph->symbol()};
            u.options.attributes = {{"perf", "off"}, {"waveform", "off"},
                {"commit_scalar_same_reader_groups", "1"}, {"input_bit_activation", "1"}};
            EmitDiagnostics emitDiags; EmitGrhSimCpp emitter(&emitDiags);
            check(emitter.analyzeExecutionPlan(design, u.options, u.plan).success, "rebuilt partition plan failed");
        }
        check(std::all_of(writerVisits.begin(), writerVisits.end(), [](auto n) { return n == 1; }),
              "whole ASC replay lost or duplicated a state writer");

        bool converged = false;
        for (unsigned round = 0; round < 32; ++round)
        {
            bool changed = false;
            for (auto &u : units)
            {
                GrhSimActivitySeeds seeds;
                for (const auto &[local, parent] : u.parentInputs)
                {
                    if (parent == clock)
                    {
                        seeds.inputs[local] = {{0, 1, 3}}; seeds.events[local] = {1, 2, 3};
                    }
                    else if (const auto it = drivers.find(parent); it != drivers.end())
                    {
                        const auto &driver = it->second;
                        auto &ranges = seeds.inputs[local];
                        const auto &source = units[driver.unit].activity;
                        if (driver.value.index < source.values.size()) ranges = source.values[driver.value.index];
                        for (auto &range : ranges)
                            range.mask = (range.mask & ~UINT64_C(3)) | ((range.mask & 1) << 1) | ((range.mask & 2) >> 1);
                    }
                    else
                    {
                        const auto def = top.valueDef(parent);
                        check(top.valueIsInput(parent) || (def.valid() && top.getOperation(def).kind() == K::kConstant),
                              "fixture wrapper has an unsupported input producer");
                        seeds.inputs[local] = {}; // Stable primary data/enable levels in this scenario.
                    }
                }
                auto next = analyzeGrhSimActivity(*u.graph, u.plan, seeds, GrhSimActivityOptions{});
                changed |= next.values != u.activity.values;
                u.activity = std::move(next);
            }
            if (!changed) { converged = true; break; }
        }
        check(converged, "partition activity did not converge");
        Result result;
        for (auto &u : units)
        {
            std::vector<GrhSimInputBinding> bindings;
            for (const auto &input : u.plan.inputs)
            {
                const auto parent = u.parentInputs.at(input.value);
                const auto driver = drivers.find(parent);
                bindings.push_back(driver == drivers.end()
                    ? GrhSimInputBinding{input.value, GrhSimInputBinding::Kind::Primary, {}}
                    : GrhSimInputBinding{input.value, GrhSimInputBinding::Kind::DeferredState, units[driver->second.unit].part});
            }
            auto work = summarizeGrhSimPartitionWork(u.part, *u.graph, u.plan, u.activity, bindings);
            check(work.work.unattributedFeatures.empty() && !work.work.summary.unmodeledComputeGroups &&
                  !work.work.summary.unmodeledCommitBodies, "partition work has missing feature coverage");
            check(work.work.summary.conditionalFeatures == summarizeGrhSimWork(u.plan, u.activity).conditionalFeatures,
                  "derived source/body views changed total conditional work");
            for (const auto &input : work.inputs)
                result.remoteInputs[u.part] += input.binding.kind == GrhSimInputBinding::Kind::DeferredState;
            for (const auto &body : work.bodies)
            {
                if (body.kind == BodyKind::SameReader) result.sameReaderBodies[u.part].push_back(body.operations.size());
                if (body.enabledSite) check(work.work.sites.at(*body.enabledSite).operations == body.operations,
                                            "body split into independently charged members");
            }
            if (reads)
                for (const auto &site : work.work.sites)
                    if (site.kind == GrhSimAttributedWorkReport::Site::Kind::Compute)
                        for (auto id : site.operations)
                        {
                            const auto op = u.graph->getOperation(id);
                            const auto tag = op.attr("test.consumer_id");
                            if (tag && std::get<int64_t>(*tag) == 1)
                            {
                                result.consumerMasks[u.part] = u.activity.operationMasks.at(id.index);
                                result.consumerExecutionMasks[u.part] = site.mask;
                                std::cout << "CONTEXT case=" << out.filename().string() << " part=" << u.part
                                          << " group_mask=" << site.mask << " local_state_masks=";
                                for (const auto &group : u.plan.groups)
                                    if (group.supernode == site.supernode)
                                        for (auto member : group.operations)
                                            if (u.graph->getOperation(member).kind() == K::kRegisterReadPort)
                                                std::cout << attr<int64_t>(u.graph->getOperation(member), "test.state_id")
                                                          << ':' << u.activity.operationMasks.at(member.index) << ';';
                                std::cout << '\n';
                            }
                        }
            // Missing roles stay explicit; malformed remote owners fail.
            const auto unknown = summarizeGrhSimPartitionWork(u.part, *u.graph, u.plan, u.activity, {});
            for (const auto &input : unknown.inputs) check(input.binding.kind == GrhSimInputBinding::Kind::Unknown,
                                                          "missing source role was guessed");
            if (!bindings.empty())
            {
                const auto rejects = [&](const auto &bad) {
                    bool rejected = false;
                    try { (void)summarizeGrhSimPartitionWork(u.part, *u.graph, u.plan, u.activity, bad); }
                    catch (const std::invalid_argument &) { rejected = true; }
                    check(rejected, "invalid partition input binding was accepted");
                };
                auto bad = bindings;
                bad.front().kind = GrhSimInputBinding::Kind::DeferredState; bad.front().writerPartition = u.part;
                rejects(bad);
                bad = bindings; bad.push_back(bad.front()); rejects(bad);
                bad = bindings; bad.front().input = {}; rejects(bad);
                bad = bindings; bad.front().input = clock; rejects(bad); // Parent graph input.
            }
            u.options.outputDir = (out / ("part_" + std::to_string(u.part))).string();
            EmitDiagnostics diags; EmitGrhSimCpp emitter(&diags);
            const auto generated = emitter.emit(design, u.options);
            check(generated.success, "rebuilt partition emit failed");
            for (const auto &path : generated.artifacts)
            {
                std::ifstream stream(path);
                result.generated.emplace("part_" + std::to_string(u.part) + "/" + std::filesystem::path(path).filename().string(),
                    std::string(std::istreambuf_iterator<char>(stream), {}));
            }
            std::cout << "DERIVED case=" << out.filename().string() << " part=" << u.part
                      << " remote_inputs=" << result.remoteInputs[u.part]
                      << " sources=" << work.sources.size() << " bodies=" << work.bodies.size()
                      << " enables=" << total(work.work.summary, "enable.dynamic_predicates")
                      << " comparisons=" << total(work.work.summary, "enabled.scalar_comparisons")
                      << " activations=" << total(work.work.summary, "changed.reader_activation_calls_upper") << '\n';
            result.work.push_back(std::move(work));
        }
        return result;
    }

    void rejectInvalidReplay(const std::filesystem::path &out)
    {
        for (unsigned test = 0; test < 4; ++test)
        {
            auto design = fixture(false);
            const auto count = design.findGraph("top")->operations().size();
            SessionStore session;
            if (test == 1)
                session.emplace("top.repcut.replay_assignment", std::make_unique<SessionSlotValue<int>>(0));
            else if (test >= 2)
                session.emplace("top.repcut.replay_assignment", std::make_unique<SessionSlotValue<RepcutReplayAssignment>>(
                    RepcutReplayAssignment{test == 2 ? std::vector<uint32_t>{0} : std::vector<uint32_t>{0, 0, 1, 3, 2}}));
            RepcutOptions options; options.path = "top"; options.partitionCount = 3;
            options.partitioner = "assignment-replay"; options.workDir = (out / std::to_string(test)).string();
            PassManager manager; manager.options().session = &session;
            manager.addPass(std::make_unique<RepcutPass>(options)); PassDiagnostics diagnostics;
            check(!manager.run(design, diagnostics).success, "invalid replay was accepted or fell back to solving");
            check(design.findGraph("top") && design.findGraph("top")->operations().size() == count,
                  "invalid replay changed the source graph");
        }
    }

    void run()
    {
        const auto out = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "grhsim_partition_work";
        rejectInvalidReplay(out / "invalid");
        const auto before = rebuild(false, {0, 0, 1, 1, 2}, out / "shared_before");
        const auto after = rebuild(false, {0, 1, 1, 1, 2}, out / "shared_after");
        const auto repeated = rebuild(false, {0, 0, 1, 1, 2}, out / "shared_repeat");
        check(before.generated == repeated.generated, "replayed generation is not deterministic");
        const auto sizes = [](const auto &result, uint32_t part) {
            const auto it = result.sameReaderBodies.find(part);
            return it == result.sameReaderBodies.end() ? std::vector<std::size_t>{} : it->second;
        };
        check(sizes(before, 0) == std::vector<std::size_t>{2} && sizes(before, 1) == std::vector<std::size_t>{2},
              "shared-before must contain two actual same-reader pairs");
        check(sizes(after, 0).empty() && sizes(after, 1) == std::vector<std::size_t>{3},
              "moving one ASC must dissolve one pair and form a three-writer body");
        bool thirdChanged = false;
        for (const auto &[name, code] : before.generated)
            if (name.starts_with("part_2/")) thirdChanged |= after.generated.at(name) != code;
        check(thirdChanged, "move should also rebuild the third partition's changed input bundles");
        const auto local = rebuild(true, {0, 0, 1, 2}, out / "read_local");
        const auto remote = rebuild(true, {0, 1, 1, 2}, out / "read_remote");
        std::cout << "CONSUMER local_value=" << local.consumerMasks.at(0)
                  << " remote_value=" << remote.consumerMasks.at(1)
                  << " local_group=" << local.consumerExecutionMasks.at(0)
                  << " remote_group=" << remote.consumerExecutionMasks.at(1) << '\n';
        check(local.consumerMasks.at(0) == 1 && remote.consumerMasks.at(1) == 2,
              "local-state to deferred-input move lost the half-step arrival change");
        // The destination group also reads local state on rising. The moved
        // expression arrives on falling but its actual group may run on BOTH.
        check(local.consumerExecutionMasks.at(0) == 1 && remote.consumerExecutionMasks.at(1) == 3,
              "replanned work lost destination coactivation on the other half-step");
        check(local.remoteInputs.at(0) == 0 && remote.remoteInputs.at(1) == 1,
              "local read did not become a remote input binding");
        std::cout << "PARTITION WORK PASS: real ASC rebuild, consumer arrival, shared bodies, deterministic C++\n";
    }
}

int main()
{
    try { run(); return 0; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
