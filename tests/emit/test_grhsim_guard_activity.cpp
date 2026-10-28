#include "emit/grhsim_activity_model.hpp"
#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <iostream>
#include <stdexcept>

using namespace wolvrix::lib;
using namespace grh;
using namespace emit;
using namespace transform;

namespace
{
    void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    uint64_t feature(const std::map<std::string, uint64_t> &features, const std::string &name)
    {
        const auto it = features.find(name); return it == features.end() ? 0 : it->second;
    }

    void checkComputeCoverage(const GrhSimExecutionPlan &plan, const Graph &g)
    {
        std::unordered_map<OperationId, unsigned, OperationIdHash> visits;
        for (const auto &entry : plan.computeLowering)
        {
            check(entry.unmodeledWork.empty(), "unexpected incomplete compute feature group");
            for (auto op : entry.loweredOperations) ++visits[op];
            for (auto op : entry.genericOperations) ++visits[op];
            std::map<std::string, uint64_t> totals;
            std::unordered_map<OperationId, unsigned, OperationIdHash> siteVisits;
            for (const auto &site : entry.sites)
            {
                check(!site.operations.empty(), "compute site has no originating operation");
                for (auto op : site.operations) ++siteVisits[op];
                for (const auto &[name, count] : site.features) totals[name] += count;
            }
            for (auto op : entry.loweredOperations)
                check(siteVisits[op] == 1, "lowered operation lost site provenance");
            for (auto op : entry.genericOperations)
                check(siteVisits[op] == 1, "generic operation lost site provenance");
            check(totals == entry.features, "compute site feature totals differ from group");
        }
        for (const auto &group : plan.groups)
            if (group.phase == GrhSimExecutionPlan::Phase::Compute)
                for (auto op : group.operations)
                    check(visits[op] == 1, "compute operation missing or duplicated in lowering features");
        (void)g;
    }

    GrhSimAttributedWorkReport checkAttribution(const GrhSimExecutionPlan &plan,
                                               const GrhSimActivityReport &activity)
    {
        const auto summary = summarizeGrhSimWork(plan, activity);
        auto attributed = attributeGrhSimWork(plan, activity);
        check(attributed.summary.conditionalFeatures == summary.conditionalFeatures &&
              attributed.summary.staticFeatures == summary.staticFeatures,
              "attribution changed the existing summary");
        auto totals = attributed.unattributedFeatures;
        for (const auto &site : attributed.sites)
        {
            check(!site.operations.empty(), "conditional site has no operation provenance");
            for (const auto &[name, count] : site.features) totals[site.mask][name] += count;
        }
        check(totals == summary.conditionalFeatures, "conditional site masks/features do not conserve summary");
        return attributed;
    }

    void runAttributionBoundaryFixture()
    {
        Design design; auto &g = design.createGraph("attribution_boundary");
        const auto a = g.createOperation(OperationKind::kAssign, g.internSymbol("a"));
        const auto b = g.createOperation(OperationKind::kAssign, g.internSymbol("b"));
        GrhSimExecutionPlan plan;
        plan.groups.push_back({0, 0, GrhSimExecutionPlan::Phase::Compute, 0, {a, b}});
        GrhSimExecutionPlan::ComputeLowering entry;
        entry.supernode = 0; entry.loweredOperations = {a, b};
        entry.features = {{"bool_reduce_iterations", 2}};
        entry.sites.push_back({{a, b}, entry.features});
        plan.computeLowering.push_back(entry);
        GrhSimActivityReport activity;
        activity.computeGroupMasks = {1 | kGrhSimUnknownActivity};
        const auto attributed = checkAttribution(plan, activity);
        check(attributed.sites.size() == 1 && attributed.sites.front().operations.size() == 2 &&
              attributed.sites.front().mask == (1 | kGrhSimUnknownActivity),
              "shared site was split or lost uncertainty");
        activity.computeGroupMasks[0] = 0;
        check(checkAttribution(plan, activity).sites.front().mask == 0, "zero-mask provenance was dropped");
        plan.computeLowering.front().sites.clear();
        const auto missing = checkAttribution(plan, activity);
        check(missing.sites.empty() && missing.unattributedFeatures.at(0).at("compute.bool_reduce_iterations") == 2,
              "missing provenance was converted into guessed per-op costs");
        plan.computeLowering.front() = entry;
        plan.computeLowering.front().sites.front().operations.push_back(a);
        bool rejected = false;
        try { (void)attributeGrhSimWork(plan, activity); }
        catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "duplicate compute site membership was accepted");
        plan.computeLowering.front() = entry;
        plan.computeLowering.front().sites.front().features["bool_reduce_iterations"] = 3;
        rejected = false;
        try { (void)attributeGrhSimWork(plan, activity); }
        catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "nonconserving compute site features were accepted");
    }

    void runBackendCostBucketFixture()
    {
        GrhSimWorkReport work;
        work.staticFeatures["dispatch.compute_batches"] = 3;
        work.conditionalFeatures[1]["compute.scalar.operand_bits"] = 11;
        work.conditionalFeatures[1]["compute.wide.helper.kSliceDynamic"] = 7;
        work.conditionalFeatures[1]["compute.wide.helper_word_iterations"] = 19;
        work.conditionalFeatures[2]["compute.wide.helper.kReduceOr"] = 5;
        work.conditionalFeatures[2]["compute.side_effect.dpic_call"] = 1;
        work.conditionalFeatures[2]["enabled.scalar_comparisons"] = 4;
        work.conditionalFeatures[1]["enabled.packed_fill_slice_words"] = 12;
        const auto buckets = bucketizeGrhSimWork(work);
        check(buckets.staticFeatures.at("dispatch.compute_batches") == 3,
              "backend bucket report lost static features");
        check(buckets.unbucketedFeatures.empty(), "known backend feature was not bucketed");
        check(buckets.conditionalFeatures.at(1).at("compute.scalar").at("compute.scalar.operand_bits") == 11,
              "scalar feature crossed scenario bucket");
        check(buckets.conditionalFeatures.at(1).at("compute.wide.slice").at(
                  "compute.wide.helper.kSliceDynamic") == 7,
              "slice helper was put in the wrong family");
        check(buckets.conditionalFeatures.at(1).at("compute.wide.shape").at(
                  "compute.wide.helper_word_iterations") == 19,
              "wide shape mechanism unit was not preserved");
        check(buckets.conditionalFeatures.at(2).at("compute.wide.reduce").at(
                  "compute.wide.helper.kReduceOr") == 5 &&
              buckets.conditionalFeatures.at(2).at("compute.side_effect").at(
                  "compute.side_effect.dpic_call") == 1 &&
              buckets.conditionalFeatures.at(2).at("commit.enabled").at(
                  "enabled.scalar_comparisons") == 4,
              "backend family partition lost a feature");

        const auto mechanisms = projectGrhSimMechanismUnits(buckets);
        check(mechanisms.unmappedFeatures.empty(),
              "known backend feature was not projected to a mechanism unit");
        check(mechanisms.staticUnits.at("dispatch.compute_batch_sites") == 3,
              "mechanism projection lost static dispatch units");
        check(mechanisms.conditionalUnits.at(1).at("compute.scalar.operand_bits") == 11 &&
              mechanisms.conditionalUnits.at(1).at("compute.wide.slice.helper_sites.Dynamic") == 7 &&
              mechanisms.conditionalUnits.at(1).at("compute.wide.helper_word_iterations") == 19,
              "mechanism projection changed the scenario or helper units");
        check(mechanisms.conditionalAttribution.at(1).at(
                  "compute.wide.slice.helper_sites.Dynamic").at(
                  "compute.wide.helper.kSliceDynamic") == 7,
              "mechanism attribution lost the raw feature");
        check(mechanisms.conditionalUnits.at(1).at("commit.enable_packed_fill_slice_words") == 12 &&
              mechanisms.conditionalAttribution.at(1).at("commit.enable_packed_fill_slice_words")
                  .at("enabled.packed_fill_slice_words") == 12,
              "packed memory fill slices were not projected to a commit mechanism");

        work.conditionalFeatures[1]["compute.future_shape"] = 1;
        const auto incomplete = bucketizeGrhSimWork(work);
        check(incomplete.unbucketedFeatures.at(1).at("compute.future_shape") == 1,
              "unknown backend feature lost its scenario mask or was treated as zero");
        const auto incompleteMechanisms = projectGrhSimMechanismUnits(incomplete);
        check(incompleteMechanisms.unmappedFeatures.at(1).at("compute.future_shape") == 1,
              "unknown mechanism feature lost its scenario mask or was treated as zero");
    }

    void runComputeShapeFixture()
    {
        // Neither a source identifier nor an unused portion of a wide input
        // changes the work performed by these expressions.
        const auto analyze = [&](const std::string &prefix) {
            Design design; auto &g = design.createGraph("shape_fixture"); design.markAsTop(g.symbol());
            auto value = [&](const std::string &name, int width) {
                return g.createValue(g.internSymbol(name), width, false);
            };
            auto input = [&](const std::string &name, int width) {
                auto v = value(prefix + name, width); g.bindInputPort(prefix + name, v); return v;
            };
            auto operation = [&](OperationKind kind, const std::string &name,
                                 std::vector<ValueId> operands, int width) {
                const auto id = g.createOperation(kind, g.internSymbol(name));
                for (auto v : operands) g.addOperand(id, v);
                const auto out = value(name + "_result", width); g.addResult(id, out);
                g.bindOutputPort(name, out); return out;
            };
            const auto a = input("a", 64), b = input("b", 64), index = input("index", 16);
            const auto small = input("small", 128), large = input("large", 16384);
            const auto sum = operation(OperationKind::kAdd, "sum", {a, b}, 64);
            operation(OperationKind::kXor, "use_sum", {sum, b}, 64);
            operation(OperationKind::kDiv, "real_helper", {a, b}, 64);
            operation(OperationKind::kReduceOr, "wide_reduce", {large}, 1);
            const auto slice = [&](const std::string &name, ValueId source, int64_t start, int width) {
                const auto out = operation(OperationKind::kSliceStatic, name, {source}, width);
                g.setAttr(g.valueDef(out), "sliceStart", start);
                g.setAttr(g.valueDef(out), "sliceEnd", start + width - 1);
            };
            slice("small_slice", small, 64, 36); slice("large_slice", large, 64, 36);
            slice("crossing_slice", large, 63, 36); slice("wide_slice", large, 64, 128);
            operation(OperationKind::kSliceDynamic, "dynamic_slice", {large, index}, 36);
            const auto array = operation(OperationKind::kSliceArray, "array_slice", {large, index}, 36);
            g.setAttr(g.valueDef(array), "sliceWidth", int64_t{36});
            SessionStore session; PassManager manager; manager.options().session = &session;
            ActivityScheduleOptions schedule; schedule.path = g.symbol(); schedule.enableCoarsen = false;
            manager.addPass(std::make_unique<ActivitySchedulePass>(schedule)); PassDiagnostics diagnostics;
            check(manager.run(design, diagnostics).success, "shape fixture schedule failed");
            EmitOptions options; options.session = &session; options.topOverrides = {g.symbol()};
            options.attributes = {{"perf", "off"}, {"waveform", "off"}};
            EmitDiagnostics emitDiagnostics; EmitGrhSimCpp emitter(&emitDiagnostics); GrhSimExecutionPlan plan;
            check(emitter.analyzeExecutionPlan(design, options, plan).success, "shape fixture planner failed");
            checkComputeCoverage(plan, g);
            std::map<std::string, std::map<std::string, uint64_t>> byOperation;
            for (const auto &entry : plan.computeLowering) for (const auto &site : entry.sites)
            {
                check(site.operations.size() == 1, "shape fixture unexpectedly fused");
                byOperation.emplace(g.getOperation(site.operations.front()).symbolText(), site.features);
            }
            return byOperation;
        };
        const auto plain = analyze("plain_"), helperNamed = analyze("grhsim_");
        check(plain == helperNamed, "expression cost depends on identifier spelling");
        for (const auto *name : {"sum", "use_sum"})
            check(feature(plain.at(name), "scalar.native_expr") == 1 &&
                  feature(plain.at(name), "scalar.helper_expr") == 0 &&
                  feature(plain.at(name), "scalar.helper_call_sites") == 0,
                  "ordinary scalar operator was classified as a helper reference");
        check(feature(plain.at("real_helper"), "scalar.helper_expr") == 1 &&
              feature(plain.at("real_helper"), "scalar.helper_call_sites") == 1,
              "actual scalar helper call lost its classification");
        for (const auto *name : {"small_slice", "large_slice", "crossing_slice", "dynamic_slice", "array_slice"})
            check(feature(plain.at(name), "wide.helper_word_iterations") == 1,
                  "narrow slice charged unused input words");
        check(feature(plain.at("small_slice"), "wide.helper_operand_words") == 2 &&
              feature(plain.at("large_slice"), "wide.helper_operand_words") == 256,
              "input inventory should retain the full source shape");
        check(feature(plain.at("crossing_slice"), "wide.helper_cross_word_merges") == 1 &&
              feature(plain.at("large_slice"), "wide.helper_cross_word_merges") == 0,
              "aligned and cross-word slices lost distinct work");
        for (const auto *name : {"dynamic_slice", "array_slice"})
            check(feature(plain.at(name), "wide.helper.index_normalizations") == 1 &&
                  feature(plain.at(name), "wide.helper.cross_word_merges") == 1,
                  "dynamic slice lost index or cross-word work");
        check(feature(plain.at("wide_slice"), "wide.helper_word_iterations") == 2 &&
              feature(plain.at("wide_reduce"), "wide.helper_word_iterations") == 256,
              "wide output or whole-input reduction was incorrectly discounted");
    }

    void runOrderedMemoryPlan()
    {
        Design design; auto &g = design.createGraph("ordered_fixture"); design.markAsTop(g.symbol());
        auto value = [&](const std::string &name, int width) { return g.createValue(g.internSymbol(name), width, false); };
        auto input = [&](const std::string &name, int width) { auto v = value(name, width); g.bindInputPort(name, v); return v; };
        auto storedInput = [&](const std::string &name, int width) {
            auto in = input(name, width); auto out = value(name + "_stored", width);
            auto op = g.createOperation(OperationKind::kAssign, g.internSymbol(name + "_assign"));
            g.addOperand(op, in); g.addResult(op, out); return out;
        };
        const auto clk = input("clk", 1), addr = input("address", 2);
        auto mem = g.createOperation(OperationKind::kMemory, g.internSymbol("memory"));
        g.setAttr(mem, "width", int64_t{8}); g.setAttr(mem, "row", int64_t{4}); g.setAttr(mem, "isSigned", false);
        auto rd = g.createOperation(OperationKind::kMemoryReadPort, g.internSymbol("read"));
        auto result = value("read_result", 8); g.addOperand(rd, addr); g.addResult(rd, result);
        g.setAttr(rd, "memSymbol", std::string("memory")); g.bindOutputPort("result", result);
        auto constant = g.createOperation(OperationKind::kConstant, g.internSymbol("mask_op"));
        auto mask = value("mask", 8); g.addResult(constant, mask); g.setAttr(constant, "constValue", std::string("8'hff"));
        for (int i = 0; i < 16; ++i)
        {
            auto prefix = "w" + std::to_string(i);
            auto en = storedInput(prefix + "_en", 1), wa = storedInput(prefix + "_addr", 2), data = storedInput(prefix + "_data", 8);
            auto wr = g.createOperation(OperationKind::kMemoryWritePort, g.internSymbol(prefix));
            for (auto v : {en, wa, data, mask, clk}) g.addOperand(wr, v);
            g.setAttr(wr, "memSymbol", std::string("memory")); g.setAttr(wr, "eventEdge", std::vector<std::string>{"posedge"});
            g.setAttr(wr, kMemoryWritePriorityGroupAttr, std::string("ordered_writers"));
            g.setAttr(wr, kMemoryWritePriorityAttr, int64_t{i});
        }
        SessionStore session; PassManager manager; manager.options().session = &session;
        ActivityScheduleOptions schedule; schedule.path = g.symbol(); schedule.maxOpInCommitSupernode = 1;
        schedule.enableCoarsen = false; schedule.commitGuardEventBuckets = false;
        manager.addPass(std::make_unique<ActivitySchedulePass>(schedule)); PassDiagnostics diagnostics;
        check(manager.run(design, diagnostics).success, "ordered-memory schedule failed");
        EmitOptions options; options.session = &session; options.topOverrides = {g.symbol()};
        options.attributes = {{"perf", "off"}, {"waveform", "off"}};
        EmitDiagnostics emitDiagnostics; EmitGrhSimCpp emitter(&emitDiagnostics); GrhSimExecutionPlan plan;
        check(emitter.analyzeExecutionPlan(design, options, plan).success, "ordered-memory planner failed");
        check(plan.unmodeledCommitWrites.empty(), "ordered-memory plan unexpectedly partial");
        std::size_t affineGroups = 0, affineWrites = 0, enableGroups = 0, otherWrites = 0;
        for (const auto &run : plan.commitWriteRuns)
        {
            enableGroups += run.enables.size();
            for (const auto &body : run.bodies)
                if (body.kind == GrhSimExecutionPlan::CommitWriteBody::Kind::OrderedMemoryAffine)
                {
                    ++affineGroups; affineWrites += body.operations.size();
                    check(body.unmodeledWork.empty() && feature(body.enabledWork, "scalar_comparisons") == 16 &&
                          feature(body.changedWork, "scalar_store_slots_upper") == 16 &&
                          feature(body.changedWork, "reader_activation_calls_upper") == 4,
                          "ordered memory features lost writer or unique-row bounds");
                }
                else otherWrites += body.operations.size();
        }
        check(affineGroups == 1 && affineWrites == 16 && enableGroups == 16 && otherWrites == 0,
              "ordered loop was confused with sixteen independent emitted bodies");
        checkComputeCoverage(plan, g);
        GrhSimActivitySeeds seeds;
        seeds.events[clk] = {1, 2, 3}; seeds.inputs[clk] = {{0, 1, 3}};
        const auto work = checkAttribution(plan, analyzeGrhSimActivity(g, plan, seeds, GrhSimActivityOptions{}));
        std::size_t sharedBodies = 0;
        for (const auto &site : work.sites)
            if (site.kind == GrhSimAttributedWorkReport::Site::Kind::EnabledBody && site.operations.size() == 16)
            {
                ++sharedBodies;
                check(feature(site.features, "enabled.scalar_comparisons") == 16,
                      "ordered shared body lost its source feature count");
            }
        check(sharedBodies == 1 && work.unattributedFeatures.empty(), "ordered body attribution was divided among writers");
    }

    void run()
    {
        Design design; auto &g = design.createGraph("guard_fixture"); design.markAsTop(g.symbol());
        auto value = [&](const std::string &name, int width) { return g.createValue(g.internSymbol(name), width, false); };
        auto input = [&](const std::string &name, int width = 1) { auto v = value(name, width); g.bindInputPort(name, v); return v; };
        auto op = [&](OperationKind kind, const std::string &name, std::vector<ValueId> args, int width = 1) {
            auto id = g.createOperation(kind, g.internSymbol(name + "_op"));
            for (auto arg : args) g.addOperand(id, arg);
            auto v = value(name, width); g.addResult(id, v); return v;
        };
        auto constant = [&](const std::string &name, const std::string &literal, int width = 1) {
            auto v = op(OperationKind::kConstant, name, {}, width);
            g.setAttr(g.valueDef(v), "constValue", literal); return v;
        };
        auto reg = [&](const std::string &name) {
            auto id = g.createOperation(OperationKind::kRegister, g.internSymbol(name));
            g.setAttr(id, "width", int64_t{1}); g.setAttr(id, "isSigned", false);
            g.setAttr(id, "initValue", std::string("1'b0"));
            auto v = op(OperationKind::kRegisterReadPort, name + "_value", {});
            g.setAttr(g.valueDef(v), "regSymbol", name); g.bindOutputPort(name, v); return v;
        };
        const auto a = input("a"), b = input("b"), held = input("held"), uncertain = input("uncertain");
        const auto one = constant("one", "1'b1"), zero = constant("zero", "1'b0");
        auto write = [&](const std::string &name, std::vector<ValueId> events, std::vector<std::string> edges,
                         ValueId enable, ValueId mask) {
            auto id = g.createOperation(OperationKind::kRegisterWritePort,
                                        g.internSymbol(name + "_write_" + std::to_string(g.operations().size())));
            for (auto v : {enable, held, mask}) g.addOperand(id, v);
            for (auto v : events) g.addOperand(id, v);
            g.setAttr(id, "regSymbol", name); g.setAttr(id, "eventEdge", edges); return id;
        };
        const auto qa = reg("source_a"), qb = reg("source_b");
        write("source_a", {a}, {"posedge"}, one, one);
        write("source_b", {b}, {"negedge"}, one, one);
        const auto compound = op(OperationKind::kOr, "compound", {qa, qb});
        const auto qd = reg("derived");
        const auto wd = write("derived", {compound}, {"posedge"}, one, one);
        const auto qn = reg("derived_negative");
        const auto wn = write("derived_negative", {compound}, {"negedge"}, one, one);
        const auto qf = reg("disabled");
        const auto wf = write("disabled", {a}, {"posedge"}, zero, one);
        const auto qm = reg("masked");
        const auto wm = write("masked", {a}, {"posedge"}, one, zero);
        const auto qs = reg("stable_enable");
        const auto ws = write("stable_enable", {a}, {"posedge"}, held, one);
        const auto qc = reg("constant_event");
        const auto wc = write("constant_event", {one}, {"posedge"}, one, one);
        const auto qh = reg("held_event");
        const auto stableExpression = op(OperationKind::kOr, "held_expression", {held, zero});
        write("held_event", {stableExpression}, {"posedge"}, one, one);
        const auto qu = reg("unknown_event");
        const auto unknownExpression = op(OperationKind::kOr, "unknown_expression", {uncertain, zero});
        write("unknown_event", {a, unknownExpression}, {"posedge", "posedge"}, one, one);
        const auto qloop = reg("feedback");
        const auto loop = op(OperationKind::kOr, "loop_expression", {qloop, qa});
        write("feedback", {loop}, {"posedge"}, one, one);
        const auto qclosed = reg("closed_feedback");
        const auto closed = op(OperationKind::kOr, "closed_expression", {qclosed, zero});
        write("closed_feedback", {closed}, {"posedge"}, one, one);
        const auto qmulti = reg("multi_writer");
        write("multi_writer", {a}, {"posedge"}, zero, one);
        write("multi_writer", {b}, {"negedge"}, one, one);
        const auto qx = reg("unknown_literal_enable");
        const auto x = constant("literal_x", "1'bx");
        const auto wx = write("unknown_literal_enable", {a}, {"posedge"}, x, one);

        const auto addr = constant("addr", "2'b0", 2), wideZero = constant("wide_zero", "130'h0", 130);
        const auto highBit = constant("high_bit", "130'h10000000000000000000000000", 130);
        const auto packedZero = constant("packed_zero", "520'h0", 520);
        auto memory = [&](const std::string &name, ValueId mask, bool fill = false, bool packed = false) {
            auto mem = g.createOperation(OperationKind::kMemory, g.internSymbol(name));
            g.setAttr(mem, "width", int64_t{130}); g.setAttr(mem, "row", int64_t{4}); g.setAttr(mem, "isSigned", false);
            auto rd = op(OperationKind::kMemoryReadPort, name + "_value", {addr}, 130);
            g.setAttr(g.valueDef(rd), "memSymbol", name); g.bindOutputPort(name, rd);
            auto wr = g.createOperation(fill ? OperationKind::kMemoryFillPort : OperationKind::kMemoryWritePort,
                                        g.internSymbol(name + "_write"));
            const auto args = fill ? std::vector<ValueId>{one, packed ? packedZero : wideZero, a}
                                   : std::vector<ValueId>{one, addr, wideZero, mask, a};
            for (auto v : args) g.addOperand(wr, v);
            g.setAttr(wr, "memSymbol", name); g.setAttr(wr, "eventEdge", std::vector<std::string>{"posedge"});
            return std::make_pair(rd, wr);
        };
        const auto [memZero, mwZero] = memory("zero_memory", wideZero);
        const auto [memHigh, mwHigh] = memory("high_memory", highBit);
        const auto [memFill, mwFill] = memory("fill_memory", wideZero, true);
        const auto [memPacked, mwPacked] = memory("packed_fill_memory", wideZero, true, true);

        // Cost provenance must survive the scheduler's source-read clones.
        // Test-only tags exercise exact ancestry, never symbol/shape matching.
        std::unordered_map<int64_t, OperationKind> originalKinds;
        for (auto id : g.operations())
        {
            originalKinds.emplace(id.index, g.getOperation(id).kind());
            g.setAttr(id, "test_cost_origin", static_cast<int64_t>(id.index));
        }
        SessionStore session; PassManager manager; manager.options().session = &session;
        ActivityScheduleOptions schedule; schedule.path = g.symbol();
        manager.addPass(std::make_unique<ActivitySchedulePass>(schedule)); PassDiagnostics diagnostics;
        check(manager.run(design, diagnostics).success, "schedule failed");
        std::size_t sourceClones = 0;
        for (auto id : g.operations())
        {
            const auto origin = g.getOperation(id).attr("test_cost_origin");
            check(origin && std::holds_alternative<int64_t>(*origin), "scheduled operation lost raw ancestry");
            const auto raw = std::get<int64_t>(*origin);
            check(originalKinds.contains(raw) && originalKinds.at(raw) == g.getOperation(id).kind(),
                  "scheduled clone has the wrong raw ancestor");
            sourceClones += raw != id.index;
        }
        check(sourceClones != 0, "source-cloning attribution boundary was not exercised");
        EmitOptions options; options.session = &session; options.topOverrides = {g.symbol()};
        options.attributes = {{"perf", "off"}, {"waveform", "off"}};
        EmitDiagnostics emitDiagnostics; EmitGrhSimCpp emitter(&emitDiagnostics); GrhSimExecutionPlan plan;
        check(emitter.analyzeExecutionPlan(design, options, plan).success, "planner failed");
        GrhSimActivitySeeds seeds;
        seeds.events[a] = {1, 2, 3}; seeds.events[b] = {4, 8, 12};
        seeds.inputs[a] = {{0, 1, 3}}; seeds.inputs[b] = {{0, 1, 12}}; seeds.inputs[held] = {};
        const auto old = analyzeGrhSimActivity(g, plan, seeds, true);
        const auto next = analyzeGrhSimActivity(g, plan, seeds, GrhSimActivityOptions{});
        auto mask = [&](ValueId v) { return grhsimActivityInRange(next.values[v.index], 0, g.valueWidth(v)); };
        check(mask(qa) == 1 && mask(qb) == 8, "source domains changed");
        check(mask(qd) == 9 && mask(qn) == 9 && next.eventMasks[wd.index] == 9 && next.eventMasks[wn.index] == 9,
              "compound event must inherit source change scenarios, with conservative polarity");
        check((old.operationMasks[wd.index] & kGrhSimUnknownActivity) && !(mask(qd) & kGrhSimUnknownActivity),
              "opaque control and derived-event analysis did not differ");
        check(mask(qf) == 0 && mask(qm) == 0 && next.eventMasks[wf.index] == 1 && next.eventMasks[wm.index] == 1 &&
              next.writeMasks[wf.index] == 0 && next.writeMasks[wm.index] == 0, "event and possible write were conflated");
        check(mask(qs) == 1 && next.writeMasks[ws.index] == 1, "stable unknown enable level must not disable a write");
        check(mask(qc) == 0 && next.eventMasks[wc.index] == 0 && mask(qh) == 0, "nonchanging values cannot produce recurring edges");
        check(mask(qu) == (1 | kGrhSimUnknownActivity), "genuine unknown input activity was hidden");
        check(mask(qloop) == 1 && mask(qclosed) == 0, "event/state feedback failed to reach its least fixed point");
        check(mask(qmulti) == 8, "disabled writer polluted another writer's domain");
        check(mask(qx) == 1 && next.writeMasks[wx.index] == 1, "X literal was incorrectly proved false");
        check(mask(memZero) == 0 && mask(memHigh) == 1 && next.writeMasks[mwZero.index] == 0 &&
              next.writeMasks[mwHigh.index] == 1, "wide mask proof inspected only the first word");
        check(mask(memFill) == 1 && next.writeMasks[mwFill.index] == 1, "memory fill has an implicit full mask");
        check(mask(memPacked) == 1 && next.writeMasks[mwPacked.index] == 1, "packed memory fill lost its event");
        check(next.provedDisabledWrites == 4 && next.derivedEventReferencesWithUnknown == 1,
              "guard classification statistics incorrect");
        check(next.worklistVisits < g.operations().size() * 10, "feedback propagation did not settle");
        auto noProofs = GrhSimActivityOptions{}; noProofs.honorProvedWriteGuards = false;
        const auto control = analyzeGrhSimActivity(g, plan, seeds, noProofs);
        check(control.writeMasks[wf.index] == 1 && control.writeMasks[wm.index] == 1,
              "write-guard ablation did not isolate literal suppression");
        auto seeded = GrhSimActivityOptions{};
        seeded.unseededEvents = GrhSimUnseededEvents::SeededRootsOnly;
        const auto conditional = analyzeGrhSimActivity(g, plan, seeds, seeded);
        check(conditional.operationMasks[wd.index] == 0 && conditional.operationMasks[wn.index] == 0 &&
              conditional.stateMasks.at("source_a") == 1 && conditional.stateMasks.at("source_b") == 8,
              "derived-event body leaked into the seeded-root slice");
        check(conditional.stateMasks.at("unknown_event") == 1 && mask(qu) == (1 | kGrhSimUnknownActivity),
              "separate slice lost a direct event alternative or hid the full residual");
        check(conditional.residualEventReferences == next.derivedEventReferences,
              "unseeded event conditions disappeared from the residual inventory");
        check(plan.unmodeledCommitWrites.empty(), "guard snapshot unexpectedly partial on fixture");
        std::unordered_map<OperationId, unsigned, OperationIdHash> enabled, bodies;
        bool shared = false;
        for (const auto &run : plan.commitWriteRuns)
        {
            check(run.begin < run.end, "empty write run");
            for (const auto &guard : run.enables)
            {
                check(!guard.operations.empty() && guard.representativeEnable.valid(), "missing enable identity");
                shared |= guard.operations.size() > 1;
                for (auto id : guard.operations) ++enabled[id];
            }
            for (const auto &body : run.bodies)
                for (auto id : body.operations) ++bodies[id];
        }
        for (const auto &write : plan.writes)
            check(enabled[write.operation] == 1 && bodies[write.operation] == 1,
                  "write is missing or duplicated in guard/body plan");
        check(shared && !plan.commitEventChecks.empty(), "shared guard fixture did not exercise sharing");
        checkComputeCoverage(plan, g);
        bool checkedZero = false, checkedWide = false, checkedFill = false, checkedMask = false, checkedPacked = false;
        for (const auto &run : plan.commitWriteRuns) for (const auto &body : run.bodies)
        {
            check(body.unmodeledWork.empty(), "body work unexpectedly incomplete");
            if (body.operations == std::vector<OperationId>{mwZero})
            {
                checkedZero = true;
                check(feature(body.enabledWork, "elided_zero_mask_memory_bodies") == 1 && body.changedWork.empty(),
                      "zero memory mask charged update work");
            }
            if (body.operations == std::vector<OperationId>{mwHigh})
            {
                checkedWide = true;
                check(feature(body.enabledWork, "unconditional_word_stores") == 3 &&
                      feature(body.enabledWork, "mask_merge_words") == 3 &&
                      feature(body.changedWork, "scalar_store_slots_upper") == 0,
                      "wide masked helper stores incorrectly conditioned on change");
            }
            if (body.operations == std::vector<OperationId>{mwFill})
            {
                checkedFill = true;
                check(feature(body.enabledWork, "fill_row_iterations") == 4 &&
                      feature(body.enabledWork, "unconditional_word_stores") == 12 &&
                      feature(body.enabledWork, "packed_fill_slice_words") == 0,
                      "memory fill row multiplicity incorrect");
            }
            if (body.operations == std::vector<OperationId>{wm})
            {
                checkedMask = true;
                check(feature(body.enabledWork, "mask_merge_words") == 1 &&
                      feature(body.enabledWork, "scalar_comparisons") == 1,
                      "zero register mask confused with elided memory-mask path");
            }
            if (body.operations == std::vector<OperationId>{mwPacked})
            {
                checkedPacked = true;
                check(feature(body.enabledWork, "fill_row_iterations") == 4 &&
                      feature(body.enabledWork, "unconditional_word_stores") == 12 &&
                      feature(body.enabledWork, "packed_fill_slice_words") == 12,
                      "packed memory fill extraction not expanded over rows");
            }
        }
        check(checkedZero && checkedWide && checkedFill && checkedMask && checkedPacked, "body feature fixtures not exercised");
        const auto work = summarizeGrhSimWork(plan, next);
        check(checkAttribution(plan, next).unattributedFeatures.empty(), "real lowering lacks operation provenance");
        check(work.unmodeledCommitBodies == 0 && work.unmodeledComputeGroups == 0 &&
              work.genericComputeOperations > 0, "feature coverage was hidden");
        // Isolate the zero-mask register body: it is still compared, but cannot
        // change. Do not gate all enabled work by writeMasks.
        auto isolated = plan; isolated.commitWriteRuns.clear(); isolated.computeLowering.clear();
        for (const auto &run : plan.commitWriteRuns) for (const auto &body : run.bodies)
            if (body.operations == std::vector<OperationId>{wm})
            {
                auto copy = run; copy.bodies = {body}; copy.enables.clear(); isolated.commitWriteRuns.push_back(copy);
            }
        auto isolatedWork = summarizeGrhSimWork(isolated, next);
        check(feature(isolatedWork.conditionalFeatures[1], "enabled.scalar_comparisons") == 1 &&
              feature(isolatedWork.conditionalFeatures[0], "changed.scalar_store_slots_upper") == 1,
              "enable/body/change feature conditions were conflated");
    }
}

int main()
{
    try { runAttributionBoundaryFixture(); runBackendCostBucketFixture(); run(); runOrderedMemoryPlan();
          runComputeShapeFixture();
          std::cout << "GRHSIM FACTORED GUARD PASS\n"; return 0; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
