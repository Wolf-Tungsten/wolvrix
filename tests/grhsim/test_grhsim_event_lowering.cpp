#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    using namespace wolvrix::lib;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-event-lowering] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    grhsim::ValueId addInputRead(grhsim::GrhSimModel &model, const char *name)
    {
        const auto bit = model.logicType(1, false, grhsim::LogicDomain::TwoState);
        const auto port = model.addInput(name, bit);
        const auto value = model.addValue(bit, name);
        model.addOperation("core.input.read", {}, std::array{value},
                           std::array{grhsim::ObjectRef::input(port)});
        return value;
    }

    grhsim::ValueId addConstant(grhsim::GrhSimModel &model, grhsim::TypeId type,
                                std::string literal)
    {
        const auto value = model.addValue(type);
        const std::array params{grhsim::Parameter{model.intern("constValue"), std::move(literal)}};
        model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
        return value;
    }

    grhsim::StateId addState(grhsim::GrhSimModel &model, const char *name, grhsim::TypeId type,
                             std::string initLiteral)
    {
        const auto id = model.addState(name, type);
        const std::array initParams{grhsim::Parameter{model.intern("value"),
                                                      std::move(initLiteral)}};
        const std::array steps{grhsim::InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(id, steps, initParams);
        return id;
    }

    grhsim::OpId addRegWrite(grhsim::GrhSimModel &model, grhsim::ValueId cond,
                             grhsim::ValueId next, grhsim::ValueId mask, grhsim::StateId target,
                             grhsim::ValueId event, std::string edge)
    {
        const std::array operands{cond, next, mask, event};
        const std::array refs{grhsim::ObjectRef::state(target)};
        const std::array params{grhsim::Parameter{model.intern("event_edges"),
                                                  std::vector<std::string>{std::move(edge)}}};
        return model.addOperation("core.state.regWrite", operands, {}, refs, params);
    }

    const grhsim::Parameter *findParam(const grhsim::GrhSimModel &model,
                                       std::span<const grhsim::Parameter> params,
                                       std::string_view name)
    {
        for (const auto &parameter : params)
            if (model.text(parameter.name) == name) return &parameter;
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

    const grhsim::StateObject *findState(const grhsim::GrhSimModel &model, std::string_view name)
    {
        for (const auto &state : model.states())
            if (model.text(state.name) == name) return &state;
        return nullptr;
    }

    const grhsim::InputObject *findInput(const grhsim::GrhSimModel &model, std::string_view name)
    {
        for (const auto &input : model.inputs())
            if (model.text(input.name) == name) return &input;
        return nullptr;
    }

    const grhsim::SimOp *producerOf(const grhsim::GrhSimModel &model, grhsim::ValueId value)
    {
        for (const auto &op : model.operations())
            for (const auto result : model.results(op))
                if (result == value) return &op;
        return nullptr;
    }

    bool getBoolParam(const grhsim::GrhSimModel &model, const grhsim::SimOp &op,
                      std::string_view name, bool expected)
    {
        const grhsim::Parameter *parameter = findParam(model, model.parameters(op), name);
        const auto *flag = parameter ? std::get_if<bool>(&parameter->value) : nullptr;
        return flag && *flag == expected;
    }

    bool getIntParam(const grhsim::GrhSimModel &model, const grhsim::SimOp &op,
                     std::string_view name, int64_t expected)
    {
        const grhsim::Parameter *parameter = findParam(model, model.parameters(op), name);
        const auto *value = parameter ? std::get_if<int64_t>(&parameter->value) : nullptr;
        return value && *value == expected;
    }

    bool getStringParam(const grhsim::GrhSimModel &model, const grhsim::SimOp &op,
                        std::string_view name, std::string_view expected)
    {
        const grhsim::Parameter *parameter = findParam(model, model.parameters(op), name);
        const auto *value = parameter ? std::get_if<std::string>(&parameter->value) : nullptr;
        return value && *value == expected;
    }

    bool getActsParam(const grhsim::GrhSimModel &model, const grhsim::SimOp &op,
                      std::vector<int64_t> expected)
    {
        const grhsim::Parameter *parameter = findParam(model, model.parameters(op), "event_acts");
        const auto *value =
            parameter ? std::get_if<std::vector<int64_t>>(&parameter->value) : nullptr;
        return value && *value == expected;
    }

    bool verifies(const grhsim::GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        return grhsim::verifyGrhSimModel(model, grhsim::defaultDialectRegistry(), diagnostics) &&
               !diagnostics.hasError();
    }

    struct PassRun
    {
        bool success = false;
        bool changed = false;
    };

    PassRun runPasses(grhsim::GrhSimModel &model, std::initializer_list<const char *> names,
                      diag::Diagnostics &diagnostics)
    {
        grhsim::PassManager manager(grhsim::defaultDialectRegistry());
        for (const char *name : names)
        {
            std::string error;
            auto pass = grhsim::defaultPassRegistry().create(name, {}, error);
            if (!pass)
            {
                diagnostics.error("pass lookup failed: " + std::string(name) + ": " + error,
                                  "test");
                return {};
            }
            manager.addPass(std::move(pass));
        }
        const auto result = manager.run(model, diagnostics);
        return {result.success && !diagnostics.hasError(), result.changed};
    }

    bool hasInfo(const diag::Diagnostics &diagnostics, std::string_view passName,
                 std::string_view needle)
    {
        for (const auto &message : diagnostics.messages())
        {
            if (message.kind == diag::DiagnosticKind::Info && message.context == passName &&
                message.message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    bool jsonRoundTripStable(const grhsim::GrhSimModel &model, const std::filesystem::path &dir,
                             const std::string &baseName)
    {
        std::filesystem::create_directories(dir);
        const auto firstPath = dir / (baseName + ".json");
        const auto secondPath = dir / (baseName + "_roundtrip.json");
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(model, firstPath, grhsim::defaultDialectRegistry(),
                                      storeDiagnostics))
            return false;
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(),
                                              loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return false;
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return false;
        return readFile(firstPath) == readFile(secondPath);
    }

    // The M2 lowering pipeline in registration order (one PassManager run).
    PassRun runLoweringPipeline(grhsim::GrhSimModel &model, diag::Diagnostics &diagnostics)
    {
        return runPasses(model,
                         {"grhsim.classify-event-inputs", "grhsim.lower-edge-detect",
                          "grhsim.extract-output-cones", "grhsim.migrate-timeslot-tasks"},
                         diagnostics);
    }

    const grhsim::SimOp *findTask(const grhsim::GrhSimModel &model, std::string_view taskName)
    {
        for (const auto *op : opsOfType(model, "core.system.task"))
            if (getStringParam(model, *op, "name", taskName)) return op;
        return nullptr;
    }

    std::optional<std::string> initConstLiteral(const grhsim::GrhSimModel &model,
                                                grhsim::StateId state)
    {
        for (const auto &record : model.initRecords())
        {
            if (record.state.index != state.index) continue;
            for (const auto &step : model.steps(record))
            {
                if (model.text(step.kind) != "core.init.const") continue;
                const auto *value = findParam(model, model.parameters(step), "value");
                if (const auto *text = value ? std::get_if<std::string>(&value->value) : nullptr)
                    return *text;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> constValueOf(const grhsim::GrhSimModel &model,
                                            grhsim::ValueId value)
    {
        const auto *producer = producerOf(model, value);
        if (!producer || model.text(producer->opType) != "core.compute.constant")
            return std::nullopt;
        const auto *param = findParam(model, model.parameters(*producer), "constValue");
        if (const auto *text = param ? std::get_if<std::string>(&param->value) : nullptr)
            return *text;
        return std::nullopt;
    }

    // Spec case 1: the dut_081 shape end to end (pass 1 + 2).
    int runDut081ShapeTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("dut_081");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q = addState(model, "q", bit, "1'h0");
        const auto clk = addInputRead(model, "clk");
        const auto d = addInputRead(model, "d");
        const auto qv = model.addValue(bit, "q_value");
        model.addOperation("core.state.read", {}, std::array{qv},
                           std::array{ObjectRef::state(q)});
        const auto one = addConstant(model, bit, "1'b1");
        addRegWrite(model, one, d, one, q, clk, "posedge");
        const auto out = model.addOutput("o", bit);
        model.addOperation("core.output.write", std::array{qv}, {},
                           std::array{ObjectRef::output(out)});
        if (!verifies(model)) return fail("dut_081: pre-pass model rejected");

        diag::Diagnostics pass1Diagnostics;
        const auto pass1 = runPasses(model, {"grhsim.classify-event-inputs"}, pass1Diagnostics);
        if (!pass1.success || !pass1.changed) return fail("dut_081: classify pass failed");
        if (!hasInfo(pass1Diagnostics, "grhsim.classify-event-inputs", "event_only_inputs=1"))
            return fail("dut_081: classify diagnostic count wrong");
        const grhsim::SimOp *clkRead = nullptr;
        for (const auto *op : opsOfType(model, "core.input.read"))
        {
            const auto refs = model.objectRefs(*op);
            const auto *input = findInput(model, "clk");
            if (refs.size() == 1 && input && refs[0].index == input->id.index) clkRead = op;
        }
        if (!clkRead || !getBoolParam(model, *clkRead, "event_only", true))
            return fail("dut_081: clk input.read was not marked event_only");
        for (const auto *op : opsOfType(model, "core.input.read"))
        {
            const auto refs = model.objectRefs(*op);
            const auto *input = findInput(model, "d");
            if (refs.size() == 1 && input && refs[0].index == input->id.index &&
                findParam(model, model.parameters(*op), "event_only"))
                return fail("dut_081: d input.read must stay unmarked");
        }

        diag::Diagnostics pass2Diagnostics;
        const auto pass2 = runPasses(model, {"grhsim.lower-edge-detect"}, pass2Diagnostics);
        if (!pass2.success || !pass2.changed) return fail("dut_081: lower pass failed");
        if (!hasInfo(pass2Diagnostics, "grhsim.lower-edge-detect",
                     "clusters=1 edge_dets=1 rewritten_ops=1 removed_history_states=0 "
                     "removed_cone_ops=1 prev_init_fallbacks=0"))
            return fail("dut_081: lower diagnostic counts wrong");

        if (model.states().size() != 1 || model.initRecords().size() != 1 ||
            !findState(model, "q"))
            return fail("dut_081: unexpected state or InitRecord count after lowering");
        if (model.operations().size() != 7) return fail("dut_081: unexpected op count");

        const auto clkInputs = opsOfType(model, "core.input.read");
        const grhsim::SimOp *clkClone = nullptr;
        const grhsim::SimOp *dRead = nullptr;
        for (const auto *op : clkInputs)
        {
            const auto refs = model.objectRefs(*op);
            if (const auto *input = findInput(model, "clk");
                input && refs.size() == 1 && refs[0].index == input->id.index)
                clkClone = op;
            if (const auto *input = findInput(model, "d");
                input && refs.size() == 1 && refs[0].index == input->id.index)
                dRead = op;
        }
        if (!clkClone || !dRead) return fail("dut_081: input.read ops lost");
        if (clkClone->phase != SimPhase::Event)
            return fail("dut_081: clk clone is not Event phase");
        if (!getBoolParam(model, *clkClone, "event_only", true))
            return fail("dut_081: clone lost the event_only parameter");
        if (dRead->phase != SimPhase::None) return fail("dut_081: d read must stay phase-less");

        const auto dets = opsOfType(model, "core.event.edgeDet");
        if (dets.size() != 1) return fail("dut_081: expected exactly one edgeDet");
        const auto *det = dets.front();
        if (det->phase != SimPhase::Event) return fail("dut_081: edgeDet not in Event phase");
        const auto detOperands = model.operands(*det);
        if (detOperands.size() != 1 || detOperands[0] != model.results(*clkClone)[0])
            return fail("dut_081: edgeDet operand is not the clk clone");
        if (!getStringParam(model, *det, "edge", "posedge") ||
            !getIntParam(model, *det, "act", 0) || !getIntParam(model, *det, "prev", 0) ||
            !getStringParam(model, *det, "prevInit", "1'h0"))
            return fail("dut_081: edgeDet parameters wrong");

        const auto writes = opsOfType(model, "core.state.regWrite");
        if (writes.size() != 1) return fail("dut_081: regWrite lost");
        const auto *write = writes.front();
        const auto writeOperands = model.operands(*write);
        const auto writeRefs = model.objectRefs(*write);
        // compact renumbered every value; re-resolve the live value ids.
        const auto constants = opsOfType(model, "core.compute.constant");
        if (constants.size() != 1 || !dRead) return fail("dut_081: constant cone broken");
        const auto oneValue = model.results(*constants.front())[0];
        const auto dValue = model.results(*dRead)[0];
        if (writeOperands.size() != 3 || writeOperands[0] != oneValue ||
            writeOperands[1] != dValue || writeOperands[2] != oneValue)
            return fail("dut_081: regWrite operands not rewired to the event-free form");
        const auto *liveQ = findState(model, "q");
        if (writeRefs.size() != 1 || !liveQ || writeRefs[0].index != liveQ->id.index)
            return fail("dut_081: regWrite refs wrong");
        if (findParam(model, model.parameters(*write), "event_edges"))
            return fail("dut_081: event_edges survived");
        if (!getActsParam(model, *write, {0})) return fail("dut_081: event_acts wrong");
        if (write->phase != SimPhase::General) return fail("dut_081: regWrite not General");

        const auto outputs = opsOfType(model, "core.output.write");
        if (outputs.size() != 1 || outputs.front()->phase != SimPhase::None)
            return fail("dut_081: pass 2 must not touch output.write");
        if (!verifies(model)) return fail("dut_081: lowered model rejected");
        if (!jsonRoundTripStable(model, artifactDir, "dut_081_lowered"))
            return fail("dut_081: JSON round trip not byte stable");

        // Pass 3 (M2b): the output cone is cloned into P_output and the
        // original state.read, now dead, is swept.
        diag::Diagnostics pass3Diagnostics;
        const auto pass3 = runPasses(model, {"grhsim.extract-output-cones"}, pass3Diagnostics);
        if (!pass3.success || !pass3.changed) return fail("dut_081: extract pass failed");
        if (!hasInfo(pass3Diagnostics, "grhsim.extract-output-cones",
                     "cloned_ops=1 removed_ops=1"))
            return fail("dut_081: extract diagnostic counts wrong");
        if (model.operations().size() != 7)
            return fail("dut_081: unexpected op count after extract");
        const auto extracted = opsOfType(model, "core.output.write");
        if (extracted.size() != 1 || extracted.front()->phase != SimPhase::Output)
            return fail("dut_081: output.write not tagged Output");
        const auto outOperands = model.operands(*extracted.front());
        if (outOperands.size() != 1) return fail("dut_081: output.write operand lost");
        const auto *outProducer = producerOf(model, outOperands[0]);
        if (!outProducer || model.text(outProducer->opType) != "core.state.read" ||
            outProducer->phase != SimPhase::Output)
            return fail("dut_081: output cone clone missing");
        // compact renumbered the model; re-resolve the q state.
        const auto *extractedQ = findState(model, "q");
        const auto outRefs = model.objectRefs(*outProducer);
        if (outRefs.size() != 1 || !extractedQ || outRefs[0].index != extractedQ->id.index)
            return fail("dut_081: output cone clone references the wrong state");
        if (opsOfType(model, "core.state.read").size() != 1)
            return fail("dut_081: original output cone op was not swept");
        if (!verifies(model)) return fail("dut_081: extracted model rejected");
        if (!jsonRoundTripStable(model, artifactDir, "dut_081_extracted"))
            return fail("dut_081: extracted JSON round trip not byte stable");

        // Pass 4 (M2b): dut_081 has no timeslot tasks, so it is a no-op.
        diag::Diagnostics pass4Diagnostics;
        const auto pass4 =
            runPasses(model, {"grhsim.migrate-timeslot-tasks"}, pass4Diagnostics);
        if (!pass4.success || pass4.changed)
            return fail("dut_081: migrate pass should be a no-op");
        if (!hasInfo(pass4Diagnostics, "grhsim.migrate-timeslot-tasks",
                     "migrated_event_tasks=0 migrated_free_tasks=0 timeslot_flags=0"))
            return fail("dut_081: migrate diagnostic counts wrong");
        return 0;
    }

    // Spec case 2 + 4: rst is both an event operand and a data-path mux select
    // (dual use, kept on the general side), and it carries two edge clusters.
    int runDualUseResetTest()
    {
        using namespace grhsim;
        GrhSimModel model("dual_use_rst");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q1 = addState(model, "q1", bit, "1'h0");
        const auto q2 = addState(model, "q2", bit, "1'h0");
        const auto rst = addInputRead(model, "rst");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto muxed = model.addValue(bit, "rst_mux");
        model.addOperation("core.compute.mux", std::array{rst, d, zero}, std::array{muxed});
        addRegWrite(model, one, d, one, q1, rst, "posedge");
        addRegWrite(model, one, muxed, one, q2, rst, "negedge");
        if (!verifies(model)) return fail("dual_use: pre-pass model rejected");

        diag::Diagnostics pass1Diagnostics;
        if (!runPasses(model, {"grhsim.classify-event-inputs"}, pass1Diagnostics).success)
            return fail("dual_use: classify pass failed");
        if (!hasInfo(pass1Diagnostics, "grhsim.classify-event-inputs", "event_only_inputs=0"))
            return fail("dual_use: rst must not be classified event-only");

        diag::Diagnostics pass2Diagnostics;
        if (!runPasses(model, {"grhsim.lower-edge-detect"}, pass2Diagnostics).success)
            return fail("dual_use: lower pass failed");
        if (!hasInfo(pass2Diagnostics, "grhsim.lower-edge-detect",
                     "clusters=2 edge_dets=2 rewritten_ops=2 removed_history_states=0 "
                     "removed_cone_ops=0 prev_init_fallbacks=0"))
            return fail("dual_use: lower diagnostic counts wrong");

        // The original rst input.read stays (mux data use) with no phase; the
        // Event-phase clone feeds both edgeDets.
        const grhsim::SimOp *original = nullptr;
        const grhsim::SimOp *clone = nullptr;
        const auto *rstInput = findInput(model, "rst");
        if (!rstInput) return fail("dual_use: rst input lost");
        for (const auto *op : opsOfType(model, "core.input.read"))
        {
            const auto refs = model.objectRefs(*op);
            if (refs.size() != 1 || refs[0].index != rstInput->id.index) continue;
            if (op->phase == SimPhase::Event) clone = op;
            else if (op->phase == SimPhase::None) original = op;
        }
        if (!original) return fail("dual_use: dual-use rst read was swept");
        if (!clone) return fail("dual_use: rst event clone missing");
        const auto muxes = opsOfType(model, "core.compute.mux");
        if (muxes.size() != 1) return fail("dual_use: mux lost");
        if (model.operands(*muxes.front())[0] != model.results(*original)[0])
            return fail("dual_use: mux select not kept on the original rst read");

        const auto dets = opsOfType(model, "core.event.edgeDet");
        if (dets.size() != 2) return fail("dual_use: expected two edgeDets");
        const auto cloneValue = model.results(*clone)[0];
        bool sawPosedge = false, sawNegedge = false;
        for (const auto *det : dets)
        {
            if (det->phase != SimPhase::Event) return fail("dual_use: edgeDet phase wrong");
            if (model.operands(*det).size() != 1 || model.operands(*det)[0] != cloneValue)
                return fail("dual_use: edgeDets must share the cloned rst value");
            if (getStringParam(model, *det, "edge", "posedge") &&
                getIntParam(model, *det, "act", 0) && getIntParam(model, *det, "prev", 0))
                sawPosedge = true;
            if (getStringParam(model, *det, "edge", "negedge") &&
                getIntParam(model, *det, "act", 1) && getIntParam(model, *det, "prev", 1))
                sawNegedge = true;
        }
        if (!sawPosedge || !sawNegedge) return fail("dual_use: cluster numbering wrong");
        const auto writes = opsOfType(model, "core.state.regWrite");
        if (writes.size() != 2) return fail("dual_use: regWrites lost");
        bool sawActs0 = false, sawActs1 = false;
        for (const auto *write : writes)
        {
            if (write->phase != SimPhase::General) return fail("dual_use: regWrite phase wrong");
            if (getActsParam(model, *write, {0})) sawActs0 = true;
            if (getActsParam(model, *write, {1})) sawActs1 = true;
        }
        if (!sawActs0 || !sawActs1) return fail("dual_use: event_acts wrong");
        if (!verifies(model)) return fail("dual_use: lowered model rejected");
        return 0;
    }

    // Spec case 3: a pure event input is classified, then its original
    // input.read is DCE'd and only the Event clone remains.
    int runPureEventInputTest()
    {
        using namespace grhsim;
        GrhSimModel model("pure_event_clk");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q = addState(model, "q", bit, "1'h0");
        const auto clk = addInputRead(model, "clk");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        addRegWrite(model, one, d, one, q, clk, "posedge");
        if (!verifies(model)) return fail("pure_event: pre-pass model rejected");

        diag::Diagnostics diagnostics;
        if (!runPasses(model, {"grhsim.classify-event-inputs", "grhsim.lower-edge-detect"},
                       diagnostics)
                 .success)
            return fail("pure_event: passes failed");
        const auto *clkInput = findInput(model, "clk");
        if (!clkInput) return fail("pure_event: clk input lost");
        unsigned reads = 0;
        for (const auto *op : opsOfType(model, "core.input.read"))
        {
            const auto refs = model.objectRefs(*op);
            if (refs.size() != 1 || refs[0].index != clkInput->id.index) continue;
            ++reads;
            if (op->phase != SimPhase::Event)
                return fail("pure_event: surviving clk read is not the Event clone");
            if (!getBoolParam(model, *op, "event_only", true))
                return fail("pure_event: clone lost event_only");
        }
        if (reads != 1) return fail("pure_event: original clk read was not DCE'd");
        if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "removed_cone_ops=1"))
            return fail("pure_event: expected exactly one swept cone op");
        if (!verifies(model)) return fail("pure_event: lowered model rejected");
        return 0;
    }

    // Spec case 5: two regWrites share the (clk, posedge) cluster.
    int runSharedClusterTest()
    {
        using namespace grhsim;
        GrhSimModel model("shared_cluster");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q1 = addState(model, "q1", bit, "1'h0");
        const auto q2 = addState(model, "q2", bit, "1'h0");
        const auto clk = addInputRead(model, "clk");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        addRegWrite(model, one, d, one, q1, clk, "posedge");
        addRegWrite(model, one, d, one, q2, clk, "posedge");

        diag::Diagnostics diagnostics;
        if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
            return fail("shared: lower pass failed");
        if (!hasInfo(diagnostics, "grhsim.lower-edge-detect",
                     "clusters=1 edge_dets=1 rewritten_ops=2 removed_history_states=0 "
                     "removed_cone_ops=1 prev_init_fallbacks=0"))
            return fail("shared: lower diagnostic counts wrong");
        if (opsOfType(model, "core.event.edgeDet").size() != 1)
            return fail("shared: expected a single shared edgeDet");
        for (const auto *write : opsOfType(model, "core.state.regWrite"))
            if (!getActsParam(model, *write, {0}))
                return fail("shared: both regWrites must reference cluster 0");
        if (!verifies(model)) return fail("shared: lowered model rejected");
        return 0;
    }

    // Spec case 6: prevInit static evaluation — a register used as a clock
    // inherits its init.const literal; a derived clock evaluates recursively;
    // unevaluable cones and width-mismatched init literals fall back to zero.
    int runPrevInitEvalTest()
    {
        using namespace grhsim;
        {
            GrhSimModel model("prev_init_reg");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto word = model.logicType(8, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clkReg = addState(model, "clk_reg", bit, "1'h1");
            const auto wideReg = addState(model, "wide_reg", word, "8'h01");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const auto clkv = model.addValue(bit, "clk_reg_value");
            model.addOperation("core.state.read", {}, std::array{clkv},
                               std::array{ObjectRef::state(clkReg)});
            const auto widev = model.addValue(word, "wide_reg_value");
            model.addOperation("core.state.read", {}, std::array{widev},
                               std::array{ObjectRef::state(wideReg)});
            addRegWrite(model, one, d, one, q, clkv, "posedge");
            addRegWrite(model, one, d, one, q, widev, "negedge");

            diag::Diagnostics diagnostics;
            if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
                return fail("prev_init: lower pass failed");
            if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "prev_init_fallbacks=0"))
                return fail("prev_init: register inits must evaluate without fallback");
            bool sawOne = false, sawWide = false;
            for (const auto *det : opsOfType(model, "core.event.edgeDet"))
            {
                if (getStringParam(model, *det, "prevInit", "1'h1") &&
                    getStringParam(model, *det, "edge", "posedge"))
                    sawOne = true;
                if (getStringParam(model, *det, "prevInit", "8'h01") &&
                    getStringParam(model, *det, "edge", "negedge"))
                    sawWide = true;
            }
            if (!sawOne || !sawWide)
                return fail("prev_init: register init literals were not propagated");
            if (!verifies(model)) return fail("prev_init: lowered model rejected");
        }
        {
            // Derived clock clk & en with en a zero-init register: the cone
            // evaluates to 1'h0 and every cone op is cloned into Event.
            GrhSimModel model("prev_init_derived");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto en = addState(model, "en", bit, "1'h0");
            const auto clk = addInputRead(model, "clk");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const auto env = model.addValue(bit, "en_value");
            model.addOperation("core.state.read", {}, std::array{env},
                               std::array{ObjectRef::state(en)});
            const auto derived = model.addValue(bit, "clk_derived");
            model.addOperation("core.compute.and", std::array{clk, env}, std::array{derived});
            addRegWrite(model, one, d, one, q, derived, "posedge");

            diag::Diagnostics diagnostics;
            if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
                return fail("prev_init_derived: lower pass failed");
            if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "prev_init_fallbacks=0"))
                return fail("prev_init_derived: boolean cone must evaluate");
            const auto dets = opsOfType(model, "core.event.edgeDet");
            if (dets.size() != 1 ||
                !getStringParam(model, *dets.front(), "prevInit", "1'h0"))
                return fail("prev_init_derived: derived clock prevInit must be 1'h0");
            unsigned eventAnds = 0;
            for (const auto *op : opsOfType(model, "core.compute.and"))
                if (op->phase == SimPhase::Event) ++eventAnds;
            if (eventAnds != 1) return fail("prev_init_derived: and clone not in Event phase");
            // The original cone is fully dead: only the clones remain.
            if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "removed_cone_ops=3"))
                return fail("prev_init_derived: expected the whole cone swept");
            if (!verifies(model)) return fail("prev_init_derived: lowered model rejected");
        }
        {
            // add is not statically evaluable: zero fallback plus diagnostic.
            GrhSimModel model("prev_init_fallback");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto a = addInputRead(model, "a");
            const auto b = addInputRead(model, "b");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const auto sum = model.addValue(bit, "sum");
            model.addOperation("core.compute.add", std::array{a, b}, std::array{sum});
            addRegWrite(model, one, d, one, q, sum, "posedge");

            diag::Diagnostics diagnostics;
            if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
                return fail("prev_init_fallback: lower pass failed");
            if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "prev_init_fallbacks=1"))
                return fail("prev_init_fallback: expected one fallback");
            const auto dets = opsOfType(model, "core.event.edgeDet");
            if (dets.size() != 1 ||
                !getStringParam(model, *dets.front(), "prevInit", "1'h0"))
                return fail("prev_init_fallback: fallback literal must be 1'h0");
            if (!verifies(model)) return fail("prev_init_fallback: lowered model rejected");
        }
        {
            // An explicit-width init literal that does not match the state
            // width falls back to a zero literal of the event width.
            GrhSimModel model("prev_init_width_mismatch");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto word = model.logicType(8, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto odd = addState(model, "odd_reg", word, "1'h1");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const auto oddv = model.addValue(word, "odd_value");
            model.addOperation("core.state.read", {}, std::array{oddv},
                               std::array{ObjectRef::state(odd)});
            addRegWrite(model, one, d, one, q, oddv, "posedge");

            diag::Diagnostics diagnostics;
            if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
                return fail("prev_init_width: lower pass failed");
            if (!hasInfo(diagnostics, "grhsim.lower-edge-detect", "prev_init_fallbacks=1"))
                return fail("prev_init_width: expected one fallback");
            const auto dets = opsOfType(model, "core.event.edgeDet");
            if (dets.size() != 1 ||
                !getStringParam(model, *dets.front(), "prevInit", "8'h00"))
                return fail("prev_init_width: fallback literal must be 8'h00");
            if (!verifies(model)) return fail("prev_init_width: lowered model rejected");
        }
        return 0;
    }

    // Spec case 7: the four mem-write kinds are rewired but keep phase None.
    int runMemWriteEventTest()
    {
        using namespace grhsim;
        GrhSimModel model("mem_write_events");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto memType = model.arrayType(bit, 16);
        const auto mem = addState(model, "mem", memType, "0");
        const auto memFill = addState(model, "mem_fill", memType, "0");
        const auto memAssign = addState(model, "mem_assign", memType, "0");
        const auto memSeq = addState(model, "mem_seq", memType, "0");
        const auto clk = addInputRead(model, "clk");
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto row = addConstant(model, memType, "16'h0000");
        const std::array edges{Parameter{model.intern("event_edges"),
                                         std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.memWrite", std::array{one, zero, one, one, clk}, {},
                           std::array{ObjectRef::state(mem)},
                           edges);
        model.addOperation("core.state.memFill", std::array{one, zero, clk}, {},
                           std::array{ObjectRef::state(memFill)},
                           edges);
        model.addOperation("core.state.memAssign", std::array{one, row, clk}, {},
                           std::array{ObjectRef::state(memAssign)},
                           edges);
        model.addOperation("core.state.memWriteSeq",
                           std::array{one, zero, one, one, zero, zero, clk}, {},
                           std::array{ObjectRef::state(memSeq)},
                           edges);
        if (!verifies(model)) return fail("mem_events: pre-pass model rejected");

        diag::Diagnostics diagnostics;
        if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
            return fail("mem_events: lower pass failed");
        if (!hasInfo(diagnostics, "grhsim.lower-edge-detect",
                     "clusters=1 edge_dets=1 rewritten_ops=4 removed_history_states=0"))
            return fail("mem_events: lower diagnostic counts wrong");
        const auto expect = [&](std::string_view type, std::size_t operands) {
            const auto ops = opsOfType(model, type);
            if (ops.size() != 1) return false;
            const auto *op = ops.front();
            return model.operands(*op).size() == operands &&
                   model.objectRefs(*op).size() == 1 &&
                   model.objectRefs(*op)[0].kind == ObjectKind::State &&
                   !findParam(model, model.parameters(*op), "event_edges") &&
                   getActsParam(model, *op, {0}) && op->phase == SimPhase::None;
        };
        if (!expect("core.state.memWrite", 4)) return fail("mem_events: memWrite shape wrong");
        if (!expect("core.state.memFill", 2)) return fail("mem_events: memFill shape wrong");
        if (!expect("core.state.memAssign", 2)) return fail("mem_events: memAssign shape wrong");
        if (!expect("core.state.memWriteSeq", 6))
            return fail("mem_events: memWriteSeq shape wrong");
        if (model.states().size() != 4) return fail("mem_events: unexpected state count");
        if (!verifies(model)) return fail("mem_events: lowered model rejected");
        return 0;
    }

    // Spec case 8: a DPI call keeps its leading Function ref.
    int runDpiCallEventTest()
    {
        using namespace grhsim;
        GrhSimModel model("dpi_call_event");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const std::array funcArgs{DpiArgument{model.intern("x"), DpiDirection::Input, bit}};
        const auto func = model.addExternFunction("check", "core.dpi", "check", funcArgs, {});
        const auto clk = addInputRead(model, "clk");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        const std::array edges{Parameter{model.intern("event_edges"),
                                         std::vector<std::string>{"posedge"}}};
        model.addOperation("core.dpi.call", std::array{one, d, clk}, {},
                           std::array{ObjectRef::function(func)},
                           edges);
        if (!verifies(model)) return fail("dpi_event: pre-pass model rejected");

        diag::Diagnostics diagnostics;
        if (!runPasses(model, {"grhsim.lower-edge-detect"}, diagnostics).success)
            return fail("dpi_event: lower pass failed");
        const auto calls = opsOfType(model, "core.dpi.call");
        if (calls.size() != 1) return fail("dpi_event: call lost");
        const auto *call = calls.front();
        const auto refs = model.objectRefs(*call);
        if (refs.size() != 1 || refs[0].kind != ObjectKind::Function ||
            refs[0].index != func.index)
            return fail("dpi_event: Function ref was not preserved");
        if (model.operands(*call).size() != 2) return fail("dpi_event: operands wrong");
        if (!getActsParam(model, *call, {0})) return fail("dpi_event: event_acts wrong");
        if (call->phase != SimPhase::General) return fail("dpi_event: call not General");
        if (model.states().size() != 0) return fail("dpi_event: unexpected states");
        if (!verifies(model)) return fail("dpi_event: lowered model rejected");
        return 0;
    }

    // Spec case 9: an event-driven $strobe gains a timeslotFlag, keeps its
    // event_acts and moves to P_output with its operand cone cloned.
    int runEventStrobeTaskTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("event_strobe");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q = addState(model, "q", bit, "1'h0");
        const auto clk = addInputRead(model, "clk");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        addRegWrite(model, one, d, one, q, clk, "posedge");
        const auto qv = model.addValue(bit, "q_value");
        model.addOperation("core.state.read", {}, std::array{qv},
                           std::array{ObjectRef::state(q)});
        // $strobe("q=%b", q) on the same clock: [callCond, arg, event].
        const std::array strobeOperands{one, qv, clk};
        const std::array strobeParams{
            Parameter{model.intern("name"), std::string("strobe")},
            Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}},
            Parameter{model.intern("proc_kind"), std::string("always")},
            Parameter{model.intern("has_timing"), false}};
        model.addOperation("core.system.task", strobeOperands, {}, {}, strobeParams);
        if (!verifies(model)) return fail("event_strobe: pre-pass model rejected");

        diag::Diagnostics diagnostics;
        const auto run = runLoweringPipeline(model, diagnostics);
        if (!run.success || !run.changed) return fail("event_strobe: pipeline failed");
        if (!hasInfo(diagnostics, "grhsim.migrate-timeslot-tasks",
                     "migrated_event_tasks=1 migrated_free_tasks=0 timeslot_flags=1"))
            return fail("event_strobe: migrate diagnostic counts wrong");

        const auto tasks = opsOfType(model, "core.system.task");
        if (tasks.size() != 1) return fail("event_strobe: task lost");
        const auto *task = tasks.front();
        if (task->phase != SimPhase::Output)
            return fail("event_strobe: task is not in Output phase");
        if (!getStringParam(model, *task, "name", "strobe") ||
            !getStringParam(model, *task, "proc_kind", "always"))
            return fail("event_strobe: task parameters lost");
        if (!getIntParam(model, *task, "timeslotFlag", 0))
            return fail("event_strobe: timeslotFlag missing or wrong");
        if (!getActsParam(model, *task, {0}))
            return fail("event_strobe: event_acts not preserved");
        if (findParam(model, model.parameters(*task), "event_edges"))
            return fail("event_strobe: event_edges survived");
        const auto taskOperands = model.operands(*task);
        if (taskOperands.size() != 2 || !model.objectRefs(*task).empty())
            return fail("event_strobe: task shape wrong");
        for (const auto operand : taskOperands)
        {
            const auto *producer = producerOf(model, operand);
            if (!producer || producer->phase != SimPhase::Output)
                return fail("event_strobe: task operand not produced in P_output");
        }
        // The cone cloned the constant and the state.read of q into P_output.
        unsigned outputConstants = 0;
        unsigned outputReads = 0;
        for (const auto *op : opsOfType(model, "core.compute.constant"))
            if (op->phase == SimPhase::Output) ++outputConstants;
        for (const auto *op : opsOfType(model, "core.state.read"))
            if (op->phase == SimPhase::Output) ++outputReads;
        if (outputConstants != 1 || outputReads != 1)
            return fail("event_strobe: Output cone clone shape wrong");
        // The original q state.read had no other user and was swept; the
        // constant stays (dual use by the regWrite).
        if (opsOfType(model, "core.state.read").size() != 1)
            return fail("event_strobe: original state.read was not swept");
        if (!verifies(model)) return fail("event_strobe: migrated model rejected");
        if (!jsonRoundTripStable(model, artifactDir, "event_strobe"))
            return fail("event_strobe: JSON round trip not byte stable");
        return 0;
    }

    // Spec case 10: event-free $monitor/$strobe get __tslot_prev_* states, an
    // ne/or changed reduction feeding a logicAnd guard and Output-phase
    // latchWrite write-backs; other tasks (display) stay untouched.
    int runFreeTimeslotTaskTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("free_timeslot");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto q1 = addState(model, "q1", bit, "1'h0");
        const auto q2 = addState(model, "q2", word, "8'h00");
        const auto en = addInputRead(model, "en");
        const auto q1v = model.addValue(bit, "q1_value");
        model.addOperation("core.state.read", {}, std::array{q1v},
                           std::array{ObjectRef::state(q1)});
        const auto q2v = model.addValue(word, "q2_value");
        model.addOperation("core.state.read", {}, std::array{q2v},
                           std::array{ObjectRef::state(q2)});
        const auto taskParams = [&](std::string taskName) {
            return std::array{
                Parameter{model.intern("name"), std::move(taskName)},
                Parameter{model.intern("proc_kind"), std::string("always")},
                Parameter{model.intern("has_timing"), false}};
        };
        // Op ids at creation: 1=input.read, 2/3=state.read, 4=monitor,
        // 5=strobe, 6=display — the prev states are named after 4 and 5.
        model.addOperation("core.system.task", std::array{en, q1v, q2v}, {}, {},
                           taskParams("monitor"));
        model.addOperation("core.system.task", std::array{en}, {}, {}, taskParams("strobe"));
        model.addOperation("core.system.task", std::array{en, q1v}, {}, {},
                           taskParams("display"));
        if (!verifies(model)) return fail("free_tslot: pre-pass model rejected");

        diag::Diagnostics diagnostics;
        const auto run = runLoweringPipeline(model, diagnostics);
        if (!run.success || !run.changed) return fail("free_tslot: pipeline failed");
        if (!hasInfo(diagnostics, "grhsim.migrate-timeslot-tasks",
                     "migrated_event_tasks=0 migrated_free_tasks=2 timeslot_flags=0"))
            return fail("free_tslot: migrate diagnostic counts wrong");

        // Prev states: one per operand slot of each migrated task, zero-init.
        const auto *prev40 = findState(model, "__tslot_prev_4_0");
        const auto *prev41 = findState(model, "__tslot_prev_4_1");
        const auto *prev42 = findState(model, "__tslot_prev_4_2");
        const auto *prev50 = findState(model, "__tslot_prev_5_0");
        if (!prev40 || !prev41 || !prev42 || !prev50)
            return fail("free_tslot: __tslot_prev_* states missing");
        if (prev40->type != bit || prev41->type != bit || prev42->type != word ||
            prev50->type != bit)
            return fail("free_tslot: prev state types wrong");
        if (initConstLiteral(model, prev40->id) != std::optional<std::string>("1'h0") ||
            initConstLiteral(model, prev41->id) != std::optional<std::string>("1'h0") ||
            initConstLiteral(model, prev42->id) != std::optional<std::string>("8'h00") ||
            initConstLiteral(model, prev50->id) != std::optional<std::string>("1'h0"))
            return fail("free_tslot: prev state init literals wrong");
        if (model.states().size() != 6) return fail("free_tslot: state count wrong");

        // The shared en input.read was cloned once into P_output; the
        // original stays for the untouched display task.
        unsigned outputInputReads = 0;
        for (const auto *op : opsOfType(model, "core.input.read"))
            if (op->phase == SimPhase::Output) ++outputInputReads;
        if (outputInputReads != 1) return fail("free_tslot: en clone missing");

        const auto *monitor = findTask(model, "monitor");
        const auto *strobe = findTask(model, "strobe");
        const auto *display = findTask(model, "display");
        if (!monitor || !strobe || !display) return fail("free_tslot: task lost");
        if (monitor->phase != SimPhase::Output || strobe->phase != SimPhase::Output)
            return fail("free_tslot: migrated task not in Output phase");
        if (display->phase != SimPhase::None)
            return fail("free_tslot: display task must stay phase-less");
        if (findParam(model, model.parameters(*monitor), "timeslotFlag") ||
            findParam(model, model.parameters(*strobe), "timeslotFlag"))
            return fail("free_tslot: free task must not carry timeslotFlag");
        const auto monitorOperands = model.operands(*monitor);
        const auto strobeOperands = model.operands(*strobe);
        const auto displayOperands = model.operands(*display);
        if (monitorOperands.size() != 3 || strobeOperands.size() != 1 ||
            displayOperands.size() != 2)
            return fail("free_tslot: task operand counts wrong");
        // display still reads the original (phase-less) en and q1v values.
        for (const auto operand : displayOperands)
        {
            const auto *producer = producerOf(model, operand);
            if (!producer || producer->phase != SimPhase::None)
                return fail("free_tslot: display operands must stay on the original cone");
        }
        // monitor args are the q1v/q2v clones.
        const auto *arg1 = producerOf(model, monitorOperands[1]);
        const auto *arg2 = producerOf(model, monitorOperands[2]);
        if (!arg1 || !arg2 || arg1->phase != SimPhase::Output ||
            arg2->phase != SimPhase::Output ||
            model.text(arg1->opType) != "core.state.read" ||
            model.text(arg2->opType) != "core.state.read")
            return fail("free_tslot: monitor args not rewired to Output clones");

        // Guard: logicAnd(en clone, changed). monitor's changed is the top of
        // a two-op or chain over three ne; strobe's is a single ne.
        const auto *monitorGuard = producerOf(model, monitorOperands[0]);
        if (!monitorGuard || model.text(monitorGuard->opType) != "core.compute.logicAnd" ||
            monitorGuard->phase != SimPhase::Output)
            return fail("free_tslot: monitor guard is not an Output logicAnd");
        const auto guardInputs = model.operands(*monitorGuard);
        if (guardInputs.size() != 2) return fail("free_tslot: guard operand count wrong");
        const auto *guardCond = producerOf(model, guardInputs[0]);
        if (!guardCond || model.text(guardCond->opType) != "core.input.read" ||
            guardCond->phase != SimPhase::Output)
            return fail("free_tslot: guard does not read the en clone");
        const auto *guardChanged = producerOf(model, guardInputs[1]);
        if (!guardChanged || model.text(guardChanged->opType) != "core.compute.or" ||
            guardChanged->phase != SimPhase::Output)
            return fail("free_tslot: monitor changed must be an or reduction");
        const auto *strobeGuard = producerOf(model, strobeOperands[0]);
        if (!strobeGuard || model.text(strobeGuard->opType) != "core.compute.logicAnd" ||
            strobeGuard->phase != SimPhase::Output)
            return fail("free_tslot: strobe guard is not an Output logicAnd");
        const auto strobeGuardInputs = model.operands(*strobeGuard);
        if (strobeGuardInputs.size() != 2) return fail("free_tslot: strobe guard wrong");
        const auto *strobeChanged = producerOf(model, strobeGuardInputs[1]);
        if (!strobeChanged || model.text(strobeChanged->opType) != "core.compute.ne" ||
            strobeChanged->phase != SimPhase::Output)
            return fail("free_tslot: single-slot changed must be the bare ne");

        // Reduction op census: 4 ne (3 monitor + 1 strobe), 2 or, 2 logicAnd.
        unsigned nes = 0, ors = 0, ands = 0;
        for (const auto *op : opsOfType(model, "core.compute.ne"))
            if (op->phase == SimPhase::Output) ++nes;
        for (const auto *op : opsOfType(model, "core.compute.or"))
            if (op->phase == SimPhase::Output) ++ors;
        for (const auto *op : opsOfType(model, "core.compute.logicAnd"))
            if (op->phase == SimPhase::Output) ++ands;
        if (nes != 4 || ors != 2 || ands != 2)
            return fail("free_tslot: changed-reduction op counts wrong");

        // Every ne compares a current-value clone against its prev read.
        for (const auto *op : opsOfType(model, "core.compute.ne"))
        {
            const auto neOperands = model.operands(*op);
            if (neOperands.size() != 2) return fail("free_tslot: ne shape wrong");
            const auto *prevRead = producerOf(model, neOperands[1]);
            if (!prevRead || model.text(prevRead->opType) != "core.state.read" ||
                prevRead->phase != SimPhase::Output)
                return fail("free_tslot: ne does not read a prev state");
            const auto refs = model.objectRefs(*prevRead);
            if (refs.size() != 1 || refs[0].index == 0 ||
                refs[0].index > model.states().size())
                return fail("free_tslot: prev read ref wrong");
            const auto &state = model.states()[refs[0].index - 1];
            if (!model.text(state.name).starts_with("__tslot_prev_"))
                return fail("free_tslot: ne compared against a non-prev state");
        }

        // latchWrite write-backs: Output phase, enable=1'h1, all-ones mask of
        // the prev width, data = the current-value clone.
        const auto latchWrites = opsOfType(model, "core.state.latchWrite");
        if (latchWrites.size() != 4) return fail("free_tslot: latchWrite count wrong");
        for (const auto *write : latchWrites)
        {
            if (write->phase != SimPhase::Output)
                return fail("free_tslot: latchWrite not in Output phase");
            const auto operands = model.operands(*write);
            const auto refs = model.objectRefs(*write);
            if (operands.size() != 3 || refs.size() != 1)
                return fail("free_tslot: latchWrite shape wrong");
            if (refs[0].index == 0 || refs[0].index > model.states().size())
                return fail("free_tslot: latchWrite ref wrong");
            const auto &state = model.states()[refs[0].index - 1];
            if (!model.text(state.name).starts_with("__tslot_prev_"))
                return fail("free_tslot: latchWrite must target a prev state");
            if (constValueOf(model, operands[0]) != std::optional<std::string>("1'h1"))
                return fail("free_tslot: latchWrite enable must be constant 1");
            const auto *data = producerOf(model, operands[1]);
            if (!data || data->phase != SimPhase::Output)
                return fail("free_tslot: latchWrite data must come from P_output");
            const auto width = model.types()[state.type.index - 1].width;
            const std::string expectedMask = width == 1 ? "1'h1" : "8'hff";
            if (constValueOf(model, operands[2]) != std::optional<std::string>(expectedMask))
                return fail("free_tslot: latchWrite mask literal wrong");
        }
        if (!verifies(model)) return fail("free_tslot: migrated model rejected");
        if (!jsonRoundTripStable(model, artifactDir, "free_timeslot"))
            return fail("free_tslot: JSON round trip not byte stable");
        return 0;
    }


    // Spec case 11: all four passes are idempotent — a second run reports no
    // change, zero diagnostics counts, and a byte-identical model.
    int runIdempotencyTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("idempotent");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto q1 = addState(model, "q1", bit, "1'h0");
        const auto q2 = addState(model, "q2", bit, "1'h0");
        const auto clk = addInputRead(model, "clk");
        const auto rst = addInputRead(model, "rst");
        const auto d = addInputRead(model, "d");
        const auto one = addConstant(model, bit, "1'b1");
        const auto zero = addConstant(model, bit, "1'b0");
        const auto muxed = model.addValue(bit, "rst_mux");
        model.addOperation("core.compute.mux", std::array{rst, d, zero}, std::array{muxed});
        addRegWrite(model, one, d, one, q1, clk, "posedge");
        addRegWrite(model, one, muxed, one, q2, rst, "negedge");
        // Give every M2b pass real work: an output cone (pass 3), an
        // event-driven strobe (pass 4 flags) and a free monitor (pass 4
        // prev-state form).
        const auto q1v = model.addValue(bit, "q1_value");
        model.addOperation("core.state.read", {}, std::array{q1v},
                           std::array{ObjectRef::state(q1)});
        const auto out = model.addOutput("o", bit);
        model.addOperation("core.output.write", std::array{q1v}, {},
                           std::array{ObjectRef::output(out)});
        const std::array strobeParams{
            Parameter{model.intern("name"), std::string("strobe")},
            Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}},
            Parameter{model.intern("proc_kind"), std::string("always")},
            Parameter{model.intern("has_timing"), false}};
        model.addOperation("core.system.task", std::array{one, d, clk}, {},
                           {}, strobeParams);
        const std::array monitorParams{
            Parameter{model.intern("name"), std::string("monitor")},
            Parameter{model.intern("proc_kind"), std::string("always")},
            Parameter{model.intern("has_timing"), false}};
        model.addOperation("core.system.task", std::array{one, q1v}, {}, {}, monitorParams);
        if (!verifies(model)) return fail("idempotent: pre-pass model rejected");

        diag::Diagnostics firstDiagnostics;
        const auto first = runLoweringPipeline(model, firstDiagnostics);
        if (!first.success || !first.changed) return fail("idempotent: first run failed");
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / "idempotent_first.json";
        diag::Diagnostics storeDiagnostics;
        if (!storeGrhSimModel(model, firstPath, defaultDialectRegistry(), storeDiagnostics))
            return fail("idempotent: first store failed");

        diag::Diagnostics secondDiagnostics;
        const auto second = runLoweringPipeline(model, secondDiagnostics);
        if (!second.success) return fail("idempotent: second run failed");
        if (second.changed) return fail("idempotent: second run reported a change");
        if (!hasInfo(secondDiagnostics, "grhsim.classify-event-inputs", "event_only_inputs=0"))
            return fail("idempotent: classify second-run count wrong");
        if (!hasInfo(secondDiagnostics, "grhsim.lower-edge-detect",
                     "clusters=0 edge_dets=0 rewritten_ops=0 removed_history_states=0 "
                     "removed_cone_ops=0 prev_init_fallbacks=0"))
            return fail("idempotent: lower second-run counts wrong");
        if (!hasInfo(secondDiagnostics, "grhsim.extract-output-cones",
                     "cloned_ops=0 removed_ops=0"))
            return fail("idempotent: extract second-run counts wrong");
        if (!hasInfo(secondDiagnostics, "grhsim.migrate-timeslot-tasks",
                     "migrated_event_tasks=0 migrated_free_tasks=0 timeslot_flags=0"))
            return fail("idempotent: migrate second-run counts wrong");
        const auto secondPath = artifactDir / "idempotent_second.json";
        diag::Diagnostics secondStoreDiagnostics;
        if (!storeGrhSimModel(model, secondPath, defaultDialectRegistry(),
                              secondStoreDiagnostics))
            return fail("idempotent: second store failed");
        if (readFile(firstPath) != readFile(secondPath))
            return fail("idempotent: model fingerprint changed across runs");
        if (!verifies(model)) return fail("idempotent: lowered model rejected");
        return 0;
    }

    // Verifier guard rails for the lowered form (M2 checks).
    int runVerifierGuardTest()
    {
        using namespace grhsim;
        const auto base = [](const char *name) {
            GrhSimModel model(name);
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            return model;
        };
        const auto edgeDet = [](GrhSimModel &model, ValueId event, int64_t act,
                                SimPhase phase = SimPhase::Event) {
            const std::array<Parameter, 4> params{
                Parameter{model.intern("edge"), std::string("posedge")},
                Parameter{model.intern("act"), act},
                Parameter{model.intern("prev"), act},
                Parameter{model.intern("prevInit"), std::string("1'h0")}};
            const auto op = model.addOperation("core.event.edgeDet", std::array{event}, {}, {},
                                               params);
            model.setOperationPhase(op, phase);
            return op;
        };
        // event_edges surviving next to event_acts is rejected.
        {
            auto model = base("guard_mixed_edges");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clk = addInputRead(model, "clk");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const std::array acts{Parameter{model.intern("event_acts"),
                                            std::vector<int64_t>{0}}};
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, d, one}, {},
                                                       std::array{ObjectRef::state(q)}, acts),
                                    SimPhase::General);
            addRegWrite(model, one, d, one, q, clk, "posedge");
            edgeDet(model, clk, 0);
            if (verifies(model)) return fail("guard: event_edges/event_acts mix accepted");
        }
        // event_acts must resolve into the edgeDet act set.
        {
            auto model = base("guard_act_range");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clk = addInputRead(model, "clk");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const std::array acts{Parameter{model.intern("event_acts"),
                                            std::vector<int64_t>{1}}};
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, d, one}, {},
                                                       std::array{ObjectRef::state(q)}, acts),
                                    SimPhase::General);
            edgeDet(model, clk, 0);
            if (verifies(model)) return fail("guard: out-of-range event_acts accepted");
        }
        // A regWrite with event_acts must use the event-free 3-operand shape.
        {
            auto model = base("guard_operand_residue");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clk = addInputRead(model, "clk");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const std::array acts{Parameter{model.intern("event_acts"),
                                            std::vector<int64_t>{0}}};
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, d, one, clk}, {},
                                                       std::array{ObjectRef::state(q)}, acts),
                                    SimPhase::General);
            edgeDet(model, clk, 0);
            if (verifies(model)) return fail("guard: event-operand residue accepted");
        }
        // An Event-phase op whose operand is produced outside P_event breaks
        // cone self-containment.
        {
            auto model = base("guard_event_cone");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clk = addInputRead(model, "clk");
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const std::array acts{Parameter{model.intern("event_acts"),
                                            std::vector<int64_t>{0}}};
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, d, one}, {},
                                                       std::array{ObjectRef::state(q)}, acts),
                                    SimPhase::General);
            edgeDet(model, clk, 0); // clk's input.read stayed phase-less
            if (verifies(model)) return fail("guard: non-self-contained Event cone accepted");
        }
        // A General-phase op must not read an Event-phase value.
        {
            auto model = base("guard_general_reads_event");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto clkPort = model.addInput("clk", bit);
            const auto clk = model.addValue(bit, "clk");
            model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{clk},
                                                       std::array{ObjectRef::input(clkPort)}),
                                    SimPhase::Event);
            const auto d = addInputRead(model, "d");
            const auto one = addConstant(model, bit, "1'b1");
            const std::array acts{Parameter{model.intern("event_acts"),
                                            std::vector<int64_t>{0}}};
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, d, clk}, {},
                                                       std::array{ObjectRef::state(q)}, acts),
                                    SimPhase::General);
            edgeDet(model, clk, 0);
            if (verifies(model)) return fail("guard: General reading Event value accepted");
        }
        // An Output-phase op whose operand is produced outside P_output breaks
        // output-cone self-containment.
        {
            auto model = base("guard_output_cone");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto a = addInputRead(model, "a");
            const auto b = addInputRead(model, "b");
            const auto both = model.addValue(bit);
            model.setOperationPhase(model.addOperation("core.compute.and",
                                                       std::array{a, b}, std::array{both}),
                                    SimPhase::Output);
            if (verifies(model)) return fail("guard: non-self-contained Output cone accepted");
        }
        // A General-phase op must not read an Output-phase value.
        {
            auto model = base("guard_general_reads_output");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto q = addState(model, "q", bit, "1'h0");
            const auto one = addConstant(model, bit, "1'b1");
            const auto outNext = model.addValue(bit);
            model.setOperationPhase(model.addOperation("core.compute.constant", {},
                                                       std::array{outNext}, {},
                                                       std::array{Parameter{
                                                           model.intern("constValue"),
                                                           std::string("1'h0")}}),
                                    SimPhase::Output);
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                                                       std::array{one, outNext, one}, {},
                                                       std::array{ObjectRef::state(q)}),
                                    SimPhase::General);
            if (verifies(model)) return fail("guard: General reading Output value accepted");
        }
        // A __tslot_prev_* state written by a non-Output latchWrite is rejected.
        {
            auto model = base("guard_tslot_prev_write");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto prev = addState(model, "__tslot_prev_2_0", bit, "1'h0");
            const auto one = addConstant(model, bit, "1'b1");
            model.setOperationPhase(model.addOperation("core.state.latchWrite",
                                                       std::array{one, one, one}, {},
                                                       std::array{ObjectRef::state(prev)}),
                                    SimPhase::General);
            if (verifies(model)) return fail("guard: non-Output __tslot_prev write accepted");
        }
        // timeslotFlag must be a non-negative int64.
        {
            auto model = base("guard_tslot_flag_negative");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto clkPort = model.addInput("clk", bit);
            const auto clk = model.addValue(bit, "clk");
            model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{clk},
                                                       std::array{ObjectRef::input(clkPort)}),
                                    SimPhase::Event);
            const auto cond = addConstant(model, bit, "1'b1");
            const auto *condProducer = producerOf(model, cond);
            model.setOperationPhase(condProducer->id, SimPhase::Output);
            const std::array params{
                Parameter{model.intern("name"), std::string("strobe")},
                Parameter{model.intern("event_acts"), std::vector<int64_t>{0}},
                Parameter{model.intern("proc_kind"), std::string("always")},
                Parameter{model.intern("has_timing"), false},
                Parameter{model.intern("timeslotFlag"), int64_t{-1}}};
            model.setOperationPhase(model.addOperation("core.system.task", std::array{cond},
                                                       {}, {}, params),
                                    SimPhase::Output);
            edgeDet(model, clk, 0);
            if (verifies(model)) return fail("guard: negative timeslotFlag accepted");
        }
        // timeslotFlag requires an Output-phase core.system.task with event_acts.
        {
            auto model = base("guard_tslot_flag_shape");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto cond = addConstant(model, bit, "1'b1");
            const auto *condProducer = producerOf(model, cond);
            model.setOperationPhase(condProducer->id, SimPhase::Output);
            const std::array params{
                Parameter{model.intern("name"), std::string("strobe")},
                Parameter{model.intern("proc_kind"), std::string("always")},
                Parameter{model.intern("has_timing"), false},
                Parameter{model.intern("timeslotFlag"), int64_t{0}}};
            model.setOperationPhase(model.addOperation("core.system.task", std::array{cond},
                                                       {}, {}, params),
                                    SimPhase::Output);
            if (verifies(model)) return fail("guard: event-free timeslotFlag accepted");
        }
        return 0;
    }
} // namespace

#ifndef WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR
#error "WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR must be defined"
#endif

int main()
{
    try
    {
        const std::filesystem::path artifactDir =
            std::filesystem::path(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR) / "event_lowering";
        if (const int status = runDut081ShapeTest(artifactDir); status != 0) return status;
        if (const int status = runDualUseResetTest(); status != 0) return status;
        if (const int status = runPureEventInputTest(); status != 0) return status;
        if (const int status = runSharedClusterTest(); status != 0) return status;
        if (const int status = runPrevInitEvalTest(); status != 0) return status;
        if (const int status = runMemWriteEventTest(); status != 0) return status;
        if (const int status = runDpiCallEventTest(); status != 0) return status;
        if (const int status = runEventStrobeTaskTest(artifactDir); status != 0) return status;
        if (const int status = runFreeTimeslotTaskTest(artifactDir); status != 0) return status;
        if (const int status = runIdempotencyTest(artifactDir); status != 0) return status;
        if (const int status = runVerifierGuardTest(); status != 0) return status;
        return 0;
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
