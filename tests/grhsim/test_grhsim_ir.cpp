#include "core/grh.hpp"
#include "grhsim/convert/grh_to_grhsim.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace wolvrix::lib;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-ir] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool writeFile(const std::filesystem::path &path, const std::string &contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        return static_cast<bool>(output);
    }

    grh::Design makeFlatDesign()
    {
        grh::Design design;
        auto &graph = design.createGraph("top");

        const auto enable = graph.createValue(graph.internSymbol("enable"), 1, false);
        const auto data = graph.createValue(graph.internSymbol("data"), 8, false);
        const auto clock = graph.createValue(graph.internSymbol("clock"), 1, false);
        graph.bindInputPort("enable", enable);
        graph.bindInputPort("data", data);
        graph.bindInputPort("clock", clock);

        const auto mask = graph.createValue(graph.internSymbol("mask"), 8, false);
        const auto maskOp = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("mask_const"));
        graph.setAttr(maskOp, "constValue", std::string("8'hff"));
        graph.addResult(maskOp, mask);

        const auto reg = graph.createOperation(grh::OperationKind::kRegister,
                                               graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{8});
        graph.setAttr(reg, "isSigned", false);
        graph.setAttr(reg, "initValue", std::string("8'h01"));

        const auto q = graph.createValue(graph.internSymbol("q_value"), 8, false);
        const auto read = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                                graph.internSymbol("q_read"));
        graph.setAttr(read, "regSymbol", std::string("q"));
        graph.addResult(read, q);

        const auto write = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                 graph.internSymbol("q_write"));
        graph.setAttr(write, "regSymbol", std::string("q"));
        graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        graph.addOperand(write, enable);
        graph.addOperand(write, data);
        graph.addOperand(write, mask);
        graph.addOperand(write, clock);

        const auto sum = graph.createValue(graph.internSymbol("sum"), 8, false);
        const auto add = graph.createOperation(grh::OperationKind::kAdd,
                                               graph.internSymbol("sum_add"));
        graph.addOperand(add, q);
        graph.addOperand(add, data);
        graph.addResult(add, sum);
        graph.bindOutputPort("sum", sum);
        design.markAsTop("top");
        return design;
    }

    int runRoundTripTest(const std::filesystem::path &artifactDir)
    {
        auto design = makeFlatDesign();
        diag::Diagnostics diagnostics;
        grhsim::GrhToGrhSimOptions options;
        options.top = "top";
        options.logicDomain = grhsim::LogicDomain::TwoState;
        options.keepOrigins = true;
        auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
        if (!model || diagnostics.hasError()) return fail("flat GRH lowering failed");
        if (model->inputs().size() != 3 || model->outputs().size() != 1)
            return fail("lowered interface object counts are wrong");
        if (model->states().size() != 2 || model->initRecords().size() != 2)
            return fail("register or event-history state was not materialized");
        if (model->operations().size() != 8)
            return fail("unexpected lowered operation count");

        bool foundRegWrite = false;
        for (const auto &op : model->operations())
        {
            if (model->text(op.opType) != "core.state.regWrite") continue;
            foundRegWrite = true;
            const auto refs = model->objectRefs(op);
            if (refs.size() != 2 || refs[0].kind != grhsim::ObjectKind::State ||
                refs[1].kind != grhsim::ObjectKind::State)
                return fail("register write target/event-history refs are wrong");
            bool foundEdges = false;
            for (const auto &parameter : model->parameters(op))
            {
                if (model->text(parameter.name) == "event_edges") foundEdges = true;
            }
            if (!foundEdges) return fail("eventEdge was not normalized to event_edges");
        }
        if (!foundRegWrite) return fail("core.state.regWrite is missing");

        diag::Diagnostics passDiagnostics;
        std::string passError;
        auto pass = grhsim::defaultPassRegistry().create("grhsim.verify", {}, passError);
        if (!pass) return fail("grhsim.verify registry lookup failed: " + passError);
        grhsim::PassManager manager(grhsim::defaultDialectRegistry());
        manager.addPass(std::move(pass));
        const auto passResult = manager.run(*model, passDiagnostics);
        if (!passResult.success || passResult.changed || passDiagnostics.hasError())
            return fail("grhsim.verify pass failed or reported mutation");

        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / "grhsim_v1.json";
        const auto secondPath = artifactDir / "grhsim_v1_roundtrip.json";
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(*model, firstPath, grhsim::defaultDialectRegistry(),
                                      storeDiagnostics))
            return fail("GrhSIM JSON store failed");
        const auto originalIdentity = model->identity();
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(),
                                              loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("GrhSIM JSON load failed");
        if (loaded->identity() == originalIdentity)
            return fail("load reused serialized/runtime model identity");
        if (loaded->semanticRevision() != 1 || loaded->metadataRevision() != 1)
            return fail("loaded model revisions must restart at one");
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return fail("round-trip GrhSIM JSON store failed");
        if (readFile(firstPath) != readFile(secondPath))
            return fail("store/load/store did not produce stable bytes");

        std::string invalidFormat = readFile(firstPath);
        const auto formatPos = invalidFormat.find("wolvrix.grhsim.v1");
        if (formatPos == std::string::npos) return fail("stored format marker is missing");
        invalidFormat.replace(formatPos, std::string("wolvrix.grhsim.v1").size(), "wolvrix.grhsim.v0");
        const auto invalidFormatPath = artifactDir / "grhsim_invalid_format.json";
        if (!writeFile(invalidFormatPath, invalidFormat)) return fail("failed to write bad format fixture");
        diag::Diagnostics invalidFormatDiagnostics;
        if (grhsim::loadGrhSimModel(invalidFormatPath, grhsim::defaultDialectRegistry(),
                                    invalidFormatDiagnostics) || !invalidFormatDiagnostics.hasError())
            return fail("loader accepted an unsupported format");

        std::string invalidCount = readFile(firstPath);
        const std::string operationCount = "\"operations\":8";
        const auto operationCountPos = invalidCount.find(operationCount);
        if (operationCountPos == std::string::npos)
            return fail("could not locate serialized operation count");
        invalidCount.replace(operationCountPos, operationCount.size(), "\"operations\":9");
        const auto invalidCountPath = artifactDir / "grhsim_invalid_count.json";
        if (!writeFile(invalidCountPath, invalidCount))
            return fail("failed to write bad count fixture");
        diag::Diagnostics invalidCountDiagnostics;
        if (grhsim::loadGrhSimModel(invalidCountPath, grhsim::defaultDialectRegistry(),
                                    invalidCountDiagnostics) || !invalidCountDiagnostics.hasError())
            return fail("loader accepted a mismatched table count");

        std::string invalidReference = readFile(firstPath);
        const std::string valuePrefix = "\"values\":[[1,1,";
        const auto valuePos = invalidReference.find(valuePrefix);
        if (valuePos == std::string::npos) return fail("could not locate value reference fixture");
        invalidReference.replace(valuePos, valuePrefix.size(), "\"values\":[[1,999,");
        const auto invalidReferencePath = artifactDir / "grhsim_invalid_reference.json";
        if (!writeFile(invalidReferencePath, invalidReference))
            return fail("failed to write bad reference fixture");
        diag::Diagnostics invalidReferenceDiagnostics;
        if (grhsim::loadGrhSimModel(invalidReferencePath, grhsim::defaultDialectRegistry(),
                                    invalidReferenceDiagnostics) || !invalidReferenceDiagnostics.hasError())
            return fail("loader accepted an invalid TypeId reference");
        return 0;
    }

    int runDetachedValueTest()
    {
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            grh::Design design;
            auto &graph = design.createGraph("top"); design.markAsTop("top");
            const auto input = graph.createValue(graph.internSymbol("unused_input"), 8, false);
            graph.bindInputPort("unused_input", input);
            const auto produced = graph.createValue(graph.internSymbol("produced"), 8, false);
            const auto constant = graph.createOperation(grh::OperationKind::kConstant, graph.internSymbol("constant"));
            graph.setAttr(constant, "constValue", std::string("8'h5a")); graph.addResult(constant, produced);
            const auto detached = graph.createValue(graph.internSymbol("detached"), 8, false);
            if (mode == 1) graph.bindOutputPort("floating", detached);
            if (mode == 2)
            {
                const auto result = graph.createValue(graph.internSymbol("result"), 8, false);
                const auto invert = graph.createOperation(grh::OperationKind::kNot, graph.internSymbol("invert"));
                graph.addOperand(invert, detached); graph.addResult(invert, result);
                graph.bindOutputPort("result", result);
            }
            diag::Diagnostics diagnostics;
            grhsim::GrhToGrhSimOptions options; options.top = "top";
            options.logicDomain = grhsim::LogicDomain::FourState;
            const auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
            if (mode == 0)
            {
                if (!model || diagnostics.hasError() || model->values().size() != 2 || model->operations().size() != 2)
                    return fail("detached value was not skipped or unused input/producer was removed");
            }
            else if (model || !diagnostics.hasError()) return fail("referenced undriven value was silently dropped");
        }
        return 0;
    }

    int runUndrivenTwoStateTest()
    {
        for (const int32_t width : {1, 8, 137})
        for (const bool keepOrigins : {false, true})
        {
            grh::Design design;
            auto &graph = design.createGraph("top");
            design.markAsTop("top");
            const auto input = graph.createValue(graph.internSymbol("input"), width, true);
            graph.bindInputPort("input", input);
            const auto floating = graph.createValue(graph.internSymbol("floating"), width, true);
            graph.bindOutputPort("floating", floating);
            // ROB debug fields remain concat operands when RANDOMIZE_REG_INIT is off.
            const auto joined = graph.createValue(graph.internSymbol("joined"), 2 * width, false);
            const auto concat = graph.createOperation(grh::OperationKind::kConcat);
            graph.addOperand(concat, floating);
            graph.addOperand(concat, input);
            graph.addResult(concat, joined);
            graph.bindOutputPort("joined", joined);
            const auto busInput = graph.createValue(graph.internSymbol("bus_in"), width, true);
            const auto busOutput = graph.createValue(graph.internSymbol("bus_out"), width, true);
            const auto enable = graph.createValue(graph.internSymbol("bus_oe"), 1, false);
            graph.bindInoutPort("bus", busInput, busOutput, enable);
            graph.createValue(graph.internSymbol("detached"), width, false);

            diag::Diagnostics diagnostics;
            grhsim::GrhToGrhSimOptions options;
            options.top = "top";
            options.logicDomain = grhsim::LogicDomain::TwoState;
            options.keepOrigins = keepOrigins;
            const auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
            if (!model || diagnostics.hasError()) return fail("undriven two-state logic did not lower");
            unsigned constants = 0;
            unsigned inputs = 0;
            for (const auto &op : model->operations())
            {
                if (model->text(op.opType) == "core.input.read") ++inputs;
                if (model->text(op.opType) != "core.compute.constant") continue;
                ++constants;
                const auto &value = model->values()[model->results(op).front().index - 1];
                const auto &type = model->types()[value.type.index - 1];
                const auto params = model->parameters(op);
                if (params.size() != 1) return fail("undriven constant has no unique literal");
                const auto *text = std::get_if<std::string>(&params.front().value);
                if (!text) return fail("undriven constant literal is not a string");
                const auto literal = slang::SVInt::fromString(*text);
                if (literal.hasUnknown() || literal.countOnes() != 0 || literal.getBitWidth() != type.width)
                    return fail("undriven constant is not a width-correct two-state zero");
                if (type.domain != grhsim::LogicDomain::TwoState ||
                    (type.isSigned ? type.width != static_cast<uint32_t>(width) : type.width != 1))
                    return fail("undriven lowering changed the value type");
                if (keepOrigins != value.origin.valid() || keepOrigins != op.origin.valid())
                    return fail("undriven producer lost its optional source origin");
            }
            if (constants != 3 || inputs != 2 || model->values().size() != 6)
                return fail("undriven lowering duplicated a producer, zeroed an input, or retained a detached value");
        }
        return 0;
    }

    int runHierarchyRejectionTest()
    {
        grh::Design design;
        auto &graph = design.createGraph("hier");
        graph.createOperation(grh::OperationKind::kInstance, graph.internSymbol("child"));
        design.markAsTop("hier");
        diag::Diagnostics diagnostics;
        grhsim::GrhToGrhSimOptions options;
        options.top = "hier";
        auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
        if (model || !diagnostics.hasError()) return fail("hierarchical GRH was not rejected");
        return 0;
    }

    int runIdentityAssignTest()
    {
        using namespace grhsim;
        GrhSimModel model("identity_assign"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto u5 = model.logicType(5, false, LogicDomain::TwoState);
        const auto s5 = model.logicType(5, true, LogicDomain::TwoState);
        const auto u8 = model.logicType(8, false, LogicDomain::TwoState);
        const auto four = model.logicType(5, false, LogicDomain::FourState);
        const auto wide = model.logicType(129, false, LogicDomain::TwoState);
        const auto input = [&](const char *name, TypeId type) {
            const auto port = model.addInput(name, type); const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto a = input("a", u5), b = input("b", four);
        const auto w = input("w", wide);
        const auto wideAlias = model.addValue(wide);
        model.addOperation("core.compute.assign", std::array{w}, std::array{wideAlias});
        const auto first = model.addValue(u5), second = model.addValue(u5);
        // Deliberately emit the use before its producer, and retain conversions.
        model.addOperation("core.compute.assign", std::array{first}, std::array{second});
        model.addOperation("core.compute.assign", std::array{a}, std::array{first});
        for (auto target : {s5, u8})
        {
            const auto converted = model.addValue(target);
            model.addOperation("core.compute.assign", std::array{second}, std::array{converted});
        }
        const auto fourResult = model.addValue(four);
        model.addOperation("core.compute.assign", std::array{b}, std::array{fourResult});
        const auto cycleA = model.addValue(u5), cycleB = model.addValue(u5);
        model.addOperation("core.compute.assign", std::array{cycleB}, std::array{cycleA});
        model.addOperation("core.compute.assign", std::array{cycleA}, std::array{cycleB});
        const auto expression = [&](std::string_view op, std::initializer_list<ValueId> operands) {
            const auto value = model.addValue(u5);
            model.addOperation(op, {operands.begin(), operands.size()}, std::array{value});
            return value;
        };
        const auto sum0 = expression("core.compute.add", {a, second});
        const auto sum1 = expression("core.compute.add", {first, a});
        expression("core.compute.xor", {sum0, a});
        expression("core.compute.xor", {sum1, second});
        expression("core.compute.sub", {sum0, a});
        expression("core.compute.sub", {a, sum0});
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        for (int64_t start : {0, 1, 0})
        {
            const auto value = model.addValue(bit);
            const std::array params{Parameter{model.intern("sliceStart"), start},
                Parameter{model.intern("sliceEnd"), start}};
            model.addOperation("core.compute.sliceStatic", std::array{a}, std::array{value}, {}, params);
        }
        const auto port = model.addOutput("y", u5);
        model.addOperation("core.output.write", std::array{second}, {}, std::array{ObjectRef::output(port)});
        const auto before = model.operations().size();
        const auto revision = model.semanticRevision();
        PassManager manager(defaultDialectRegistry()); std::string error;
        auto pass = defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error);
        if (!pass) return fail("identity pass factory: " + error);
        manager.addPass(std::move(pass));
        diag::Diagnostics diagnostics; const auto result = manager.run(model, diagnostics);
        if (!result.success || !result.changed || model.operations().size() != before - 6 ||
            model.semanticRevision() != revision + 1)
            return fail("identity pass did not remove exactly the type-preserving chain");
        for (const auto &op : model.operations())
            if (model.text(op.opType) == "core.output.write" && model.operands(op)[0] != a)
                return fail("identity pass did not reconnect output to original producer");
        const auto again = manager.run(model, diagnostics);
        if (!again.success || again.changed) return fail("identity pass is not idempotent on cycles/conversions");
        std::stringstream serialized;
        if (!writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics) ||
            !readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics))
            return fail("identity pass output does not round-trip");
        return 0;
    }

    int runAlgebraicComputeTest()
    {
        using namespace grhsim;
        for (const uint32_t width : {1, 5, 64})
        for (const bool isSigned : {false, true})
        {
            GrhSimModel model("algebra"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto type = model.logicType(width, isSigned, LogicDomain::TwoState);
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto x = model.addValue(type), y = model.addValue(type);
            const auto xPort = model.addInput("x", type), yPort = model.addInput("y", type);
            const auto constant = [&](TypeId t, ParameterValue literal) {
                const auto value = model.addValue(t);
                const std::array parameters{Parameter{model.intern("value"), std::move(literal)}};
                model.addOperation("core.compute.constant", {}, std::array{value}, {}, parameters);
                return value;
            };
            const auto zero = constant(type, int64_t{0}), one = constant(type, true);
            const auto mask = constant(type, std::string("-1"));
            const auto condition = constant(bit, std::string("1'bx"));
            std::vector<std::pair<OutputId, OutputId>> expected;
            const auto check = [&](std::string_view name, std::initializer_list<ValueId> args, ValueId equivalent) {
                const auto result = model.addValue(type);
                model.addOperation(std::string("core.compute.") + std::string(name), {args.begin(), args.size()}, std::array{result});
                const auto output = model.addOutput("o" + std::to_string(expected.size()), type);
                model.addOperation("core.output.write", std::array{result}, {}, std::array{ObjectRef::output(output)});
                const auto reference = model.addOutput("ref" + std::to_string(expected.size()), type);
                model.addOperation("core.output.write", std::array{equivalent ? equivalent : result}, {}, std::array{ObjectRef::output(reference)});
                expected.emplace_back(output, reference);
                return result;
            };
            for (const auto op : {"add", "xor", "or"})
            { check(op, {x, zero}, x); check(op, {zero, x}, x); }
            check("sub", {x, zero}, x);
            check("mul", {x, one}, x); check("mul", {one, x}, x);
            check("mul", {x, zero}, zero); check("mul", {zero, x}, zero);
            check("and", {x, zero}, zero); check("and", {zero, x}, zero);
            check("and", {x, mask}, x); check("and", {mask, x}, x);
            check("or", {x, mask}, mask); check("or", {mask, x}, mask);
            check("and", {x, x}, x); check("or", {x, x}, x);
            check("mux", {condition, x, y}, y); check("mux", {condition, x, x}, x);
            if (!(width == 1 && isSigned)) check("div", {x, one}, x);
            if (width == 1)
            {
                check("logicAnd", {x, zero}, zero); check("logicAnd", {zero, x}, zero);
                check("logicAnd", {x, one}, x); check("logicAnd", {one, x}, x);
                check("logicOr", {x, zero}, x); check("logicOr", {zero, x}, x);
                check("logicOr", {x, one}, one); check("logicOr", {one, x}, one);
            }
            // Consumers and constant aliases deliberately precede their producers.
            const auto alias = model.addValue(type), folded = model.addValue(type);
            check("add", {x, folded}, x);
            model.addOperation("core.compute.mul", std::array{alias, x}, std::array{folded});
            model.addOperation("core.compute.assign", std::array{zero}, std::array{alias});
            model.addOperation("core.input.read", {}, std::array{x}, std::array{ObjectRef::input(xPort)});
            model.addOperation("core.input.read", {}, std::array{y}, std::array{ObjectRef::input(yPort)});
            const auto sum = check("add", {x, y}, {});
            check("add", {y, x}, sum);
            check("sub", {x, y}, {});
            check("sub", {y, x}, {});
            // These are deliberate non-identities and must survive.
            check("mod", {x, one}, {});
            check("div", {x, zero}, {});
            const auto opsBefore = model.operations().size();
            PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
            manager.addPass(defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error));
            const auto result = manager.run(model, diagnostics);
            if (!result.success || !result.changed || model.operations().size() >= opsBefore)
                return fail("algebraic pass failed to simplify");
            std::vector<ValueId> outputs(model.outputs().size() + 1);
            unsigned subs = 0, mods = 0, divs = 0;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) == "core.output.write")
                    outputs[model.objectRefs(op)[0].index] = model.operands(op)[0];
                subs += model.text(op.opType) == "core.compute.sub";
                mods += model.text(op.opType) == "core.compute.mod";
                divs += model.text(op.opType) == "core.compute.div";
            }
            for (const auto &[port, reference] : expected)
                if (outputs[port.index] != outputs[reference.index])
                    return fail("algebraic rewrite chose a wrong producer at width " + std::to_string(width) +
                        " signed=" + std::to_string(isSigned) + " output=" + std::to_string(port.index));
            if (subs != 2 || mods != 1 || divs != 1) return fail("non-identity or noncommutative op was removed");
            const auto again = manager.run(model, diagnostics);
            if (!again.success || again.changed) return fail("algebraic pass is not idempotent");
            std::stringstream serialized;
            if (!writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics) ||
                !readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics))
                return fail("algebraic pass output does not round-trip");
        }
        GrhSimModel guards("algebra_guards"); guards.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto u5 = guards.logicType(5, false, LogicDomain::TwoState);
        const auto s5 = guards.logicType(5, true, LogicDomain::TwoState);
        const auto four = guards.logicType(5, false, LogicDomain::FourState);
        const auto input = [&](TypeId type) {
            const auto port = guards.addInput("i" + std::to_string(guards.inputs().size()), type);
            const auto value = guards.addValue(type);
            guards.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto constant = [&](TypeId type, const char *literal) {
            const auto value = guards.addValue(type);
            const std::array params{Parameter{guards.intern("value"), std::string(literal)}};
            guards.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
            return value;
        };
        const auto x = input(u5), f = input(four);
        const auto zero = constant(u5, "0"), one = constant(u5, "1"), fourZero = constant(four, "0");
        const auto preserve = [&](std::string_view name, TypeId type, std::initializer_list<ValueId> args,
                                  std::span<const Parameter> parameters = {}) {
            const auto result = guards.addValue(type);
            guards.addOperation(name, {args.begin(), args.size()}, std::array{result}, {}, parameters);
        };
        preserve("core.compute.logicAnd", u5, {x, one});
        preserve("core.compute.logicOr", u5, {x, zero});
        preserve("core.compute.add", s5, {x, zero});
        preserve("core.compute.and", four, {f, fourZero});
        preserve("core.compute.mux", u5, {f, x, x});
        const std::array params{Parameter{guards.intern("future_semantics"), true}};
        preserve("core.compute.add", u5, {x, zero}, params);
        // Do not choose between legacy and canonical parameter spellings when
        // both occur on a constant; their order must not affect simplification.
        for (const bool reverse : {false, true})
        {
            const auto ambiguous = guards.addValue(u5);
            std::array constants{Parameter{guards.intern("constValue"), std::string("0")},
                Parameter{guards.intern("value"), std::string("1")}};
            if (reverse) std::swap(constants[0], constants[1]);
            guards.addOperation("core.compute.constant", {}, std::array{ambiguous}, {}, constants);
            preserve("core.compute.add", u5, {x, ambiguous});
        }
        const auto cycleA = guards.addValue(u5), cycleB = guards.addValue(u5);
        guards.addOperation("core.compute.add", std::array{cycleB, zero}, std::array{cycleA});
        guards.addOperation("core.compute.add", std::array{cycleA, zero}, std::array{cycleB});
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error));
        const auto result = manager.run(guards, diagnostics);
        if (!result.success || result.changed) return fail("algebraic pass ignored a type, parameter, or cycle guard");
        return 0;
    }
}

#ifndef WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR
#error "WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR must be defined"
#endif

int main()
{
    try
    {
        if (const int status = runRoundTripTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR); status != 0)
            return status;
        if (const int status = runDetachedValueTest(); status != 0) return status;
        if (const int status = runUndrivenTwoStateTest(); status != 0) return status;
        if (const int status = runIdentityAssignTest(); status != 0) return status;
        if (const int status = runAlgebraicComputeTest(); status != 0) return status;
        return runHierarchyRejectionTest();
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
