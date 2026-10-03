#include "core/grh.hpp"
#include "grhsim/convert/grh_to_grhsim.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/ir/verifier.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <variant>
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
        if (model->states().size() != 1 || model->initRecords().size() != 1)
            return fail("register state was not materialized");
        if (model->operations().size() != 8)
            return fail("unexpected lowered operation count");

        bool foundRegWrite = false;
        for (const auto &op : model->operations())
        {
            if (model->text(op.opType) != "core.state.regWrite") continue;
            foundRegWrite = true;
            const auto refs = model->objectRefs(op);
            if (refs.size() != 1 || refs[0].kind != grhsim::ObjectKind::State)
                return fail("register write target ref is wrong");
            bool foundEdges = false;
            for (const auto &parameter : model->parameters(op))
            {
                if (model->text(parameter.name) == "event_edges") foundEdges = true;
            }
            if (!foundEdges) return fail("eventEdge was not normalized to event_edges");
            const auto operands = model->operands(op);
            if (operands.size() != 4)
                return fail("register write lost its trailing event operand");
        }
        if (!foundRegWrite) return fail("core.state.regWrite is missing");
        for (const auto &state : model->states())
            if (model->text(state.name).starts_with("__event_"))
                return fail("convert materialized an event-history state");

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
        const auto firstPath = artifactDir / "grhsim_v2.json";
        const auto secondPath = artifactDir / "grhsim_v2_roundtrip.json";
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
        const auto formatPos = invalidFormat.find("wolvrix.grhsim.v2");
        if (formatPos == std::string::npos) return fail("stored format marker is missing");
        invalidFormat.replace(formatPos, std::string("wolvrix.grhsim.v2").size(), "wolvrix.grhsim.v1");
        const auto invalidFormatPath = artifactDir / "grhsim_invalid_format.json";
        if (!writeFile(invalidFormatPath, invalidFormat)) return fail("failed to write bad format fixture");
        diag::Diagnostics invalidFormatDiagnostics;
        if (grhsim::loadGrhSimModel(invalidFormatPath, grhsim::defaultDialectRegistry(),
                                    invalidFormatDiagnostics) || !invalidFormatDiagnostics.hasError())
            return fail("loader accepted a v1-marked checkpoint");

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

    int runBitwisePredicatesTest()
    {
        using namespace grhsim;
        GrhSimModel model("predicates"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto signedBit = model.logicType(1, true, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto four = model.logicType(1, false, LogicDomain::FourState);
        std::vector<OpId> rewritten, unchanged;
        for (auto type : {bit, signedBit, byte, four})
        {
            const auto input = model.addInput("x" + std::to_string(type.index), type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(input)});
            for (auto name : {"core.compute.logicAnd", "core.compute.logicOr"})
            for (bool tagged : {false, true})
            {
                const auto result = model.addValue(bit);
                std::vector<Parameter> parameters;
                if (tagged) parameters.push_back({model.intern("keep"), true});
                const auto op = model.addOperation(name, std::array{value, value}, std::array{result}, {}, parameters);
                (type == bit && !tagged ? rewritten : unchanged).push_back(op);
            }
        }
        const auto beforeOps = model.operations().size(), beforeValues = model.values().size();
        const auto revision = model.semanticRevision();
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.bitwise-predicates", {}, error));
        const auto result = manager.run(model, diagnostics);
        if (!result.success || !result.changed || model.semanticRevision() != revision + 1 ||
            model.operations().size() != beforeOps || model.values().size() != beforeValues)
            return fail("bitwise predicates changed entity coverage or failed");
        for (auto id : rewritten)
        {
            const auto name = model.text(model.operations()[id.index - 1].opType);
            if (name != "core.compute.and" && name != "core.compute.or") return fail("predicate not normalized");
        }
        for (auto id : unchanged)
            if (!model.text(model.operations()[id.index - 1].opType).starts_with("core.compute.logic"))
                return fail("predicate type/parameter guard ignored");
        const auto again = manager.run(model, diagnostics);
        if (!again.success || again.changed) return fail("predicate normalization is not idempotent");
        std::stringstream serialized;
        if (!writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics) ||
            !readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics)) return fail("predicate roundtrip failed");
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

    int runConcatSliceFoldTest()
    {
        using namespace grhsim;
        GrhSimModel model("concat_slice_fold"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto u2 = model.logicType(2, false, LogicDomain::TwoState);
        const auto u4 = model.logicType(4, false, LogicDomain::TwoState);
        const auto u5 = model.logicType(5, false, LogicDomain::TwoState);
        const auto u8 = model.logicType(8, false, LogicDomain::TwoState);
        const auto s2 = model.logicType(2, true, LogicDomain::TwoState);
        const auto f2 = model.logicType(2, false, LogicDomain::FourState);
        std::vector<InputId> inputPorts;
        const auto input = [&](std::string_view name, TypeId type) {
            const auto port = model.addInput(std::string(name), type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            inputPorts.push_back(port);
            return value;
        };
        const auto x = input("x", u8), y = input("y", u8), f = input("f", f2);
        const auto xPort = inputPorts[0];
        const auto slice = [&](ValueId source, int64_t low, int64_t high) {
            const auto type = model.logicType(static_cast<uint32_t>(high - low + 1), false, LogicDomain::TwoState);
            const auto value = model.addValue(type);
            const std::array params{Parameter{model.intern("sliceStart"), low},
                Parameter{model.intern("sliceEnd"), high}};
            model.addOperation("core.compute.sliceStatic", std::array{source}, std::array{value}, {}, params);
            return value;
        };
        const auto concat = [&](TypeId type, std::span<const ValueId> args) {
            const auto value = model.addValue(type);
            model.addOperation("core.compute.concat", args, std::array{value});
            return value;
        };
        unsigned outputCount = 0;
        const auto output = [&](ValueId value) {
            const auto port = model.addOutput("o" + std::to_string(outputCount++),
                model.values()[value.index - 1].type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
            return port;
        };
        // Identity: a full-width in-order gather of one source folds to the source.
        std::vector<ValueId> bits;
        for (int64_t b = 7; b >= 0; --b) bits.push_back(slice(x, b, b));
        const auto multiUse = bits[4];
        const auto identityPort = output(concat(u8, bits));
        const auto multiUsePort = output(multiUse);
        // Range: a consecutive slice run rewrites the concat in place to one sliceStatic.
        const std::array rangeArgs{slice(x, 4, 5), slice(x, 2, 3)};
        const auto rangePort = output(concat(u4, rangeArgs));
        const auto twinPort = output(slice(x, 2, 5));
        // Guards: a gap, reversed order, two sources, four-state source, and a
        // signed result must all survive untouched.
        const std::array gapArgs{slice(x, 6, 7), slice(x, 0, 2)};
        const auto gapPort = output(concat(u5, gapArgs));
        const std::array reversedArgs{slice(x, 0, 0), slice(x, 1, 1)};
        const auto reversedPort = output(concat(u2, reversedArgs));
        const std::array twoSourceArgs{slice(x, 1, 1), slice(y, 0, 0)};
        const auto twoSourcePort = output(concat(u2, twoSourceArgs));
        const std::array fourArgs{slice(f, 1, 1), slice(f, 0, 0)};
        const auto fourPort = output(concat(u2, fourArgs));
        const std::array signedArgs{slice(x, 1, 1), slice(x, 0, 0)};
        const auto signedPort = output(concat(s2, signedArgs));
        PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
        manager.addPass(defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error));
        const auto result = manager.run(model, diagnostics);
        if (!result.success || !result.changed) return fail("concat-slice fold pass failed or reported no change");
        // Value ids are renumbered by compaction; resolve everything structurally.
        std::vector<ValueId> outputs(model.outputs().size() + 1);
        ValueId xRead{};
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) == "core.output.write")
                outputs[model.objectRefs(op)[0].index] = model.operands(op)[0];
            if (model.text(op.opType) == "core.input.read" && model.objectRefs(op)[0].index == xPort.index)
                xRead = model.results(op)[0];
        }
        if (!xRead) return fail("input read of x was not preserved");
        const auto producerOf = [&](ValueId value) -> const SimOp & {
            for (const auto &op : model.operations())
                for (auto resultValue : model.results(op))
                    if (resultValue == value) return op;
            throw std::runtime_error("missing producer");
        };
        const auto sliceBounds = [&](const SimOp &op, int64_t &start, int64_t &end) {
            start = end = -1;
            for (const auto &parameter : model.parameters(op))
            {
                const auto *integer = std::get_if<int64_t>(&parameter.value);
                if (!integer) return;
                if (model.text(parameter.name) == "sliceStart") start = *integer;
                if (model.text(parameter.name) == "sliceEnd") end = *integer;
            }
        };
        if (outputs[identityPort.index] != xRead) return fail("identity concat did not fold to the source");
        if (outputs[rangePort.index] != outputs[twinPort.index])
            return fail("range fold slice was not shared with the identical existing slice");
        {
            const auto &op = producerOf(outputs[rangePort.index]);
            if (model.text(op.opType) != "core.compute.sliceStatic" || model.operands(op)[0] != xRead)
                return fail("range concat was not rewritten to a single sliceStatic");
            int64_t start, end; sliceBounds(op, start, end);
            if (start != 2 || end != 5) return fail("range fold slice bounds are wrong");
        }
        for (auto port : {gapPort, reversedPort, twoSourcePort, fourPort, signedPort})
            if (model.text(producerOf(outputs[port.index]).opType) != "core.compute.concat")
                return fail("a guarded concat was folded");
        {
            const auto &op = producerOf(outputs[multiUsePort.index]);
            if (model.text(op.opType) != "core.compute.sliceStatic" || model.operands(op)[0] != xRead)
                return fail("multi-use slice was removed with the identity concat");
            int64_t start, end; sliceBounds(op, start, end);
            if (start != 3 || end != 3) return fail("multi-use slice bounds changed");
        }
        const auto again = manager.run(model, diagnostics);
        if (!again.success || again.changed) return fail("concat-slice fold pass is not idempotent");
        std::stringstream serialized;
        if (!writeGrhSimJson(model, serialized, defaultDialectRegistry(), diagnostics) ||
            !readGrhSimJson(serialized, defaultDialectRegistry(), diagnostics))
            return fail("concat-slice fold output does not round-trip");
        return 0;
    }

    struct GenerateGroupTexts
    {
        std::string scope;
        std::string name;
        std::vector<std::string> symbols;
        bool operator==(const GenerateGroupTexts &) const = default;
    };

    std::vector<std::string> declaredSymbolTexts(const grhsim::GrhSimModel &model)
    {
        std::vector<std::string> texts;
        for (const auto symbol : model.declaredSymbols())
            texts.emplace_back(model.text(symbol));
        return texts;
    }

    std::vector<GenerateGroupTexts> generateGroupTexts(const grhsim::GrhSimModel &model)
    {
        std::vector<GenerateGroupTexts> groups;
        for (const auto &group : model.generateGroups())
        {
            GenerateGroupTexts texts{std::string(model.text(group.scope)),
                                     std::string(model.text(group.name)), {}};
            for (const auto symbol : group.symbols)
                texts.symbols.emplace_back(model.text(symbol));
            groups.push_back(std::move(texts));
        }
        return groups;
    }

    grh::Design makeDeclaredMetadataDesign()
    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        // internSymbol refuses texts already bound to an entity, so capture the
        // SymbolIds at creation time (the ingest pattern).
        const auto enableSym = graph.internSymbol("enable");
        const auto dataSym = graph.internSymbol("data");
        const auto sig0Sym = graph.internSymbol("gen_loop$0$sig");
        const auto sig1Sym = graph.internSymbol("gen_loop$1$sig");
        const auto enable = graph.createValue(enableSym, 1, false);
        graph.bindInputPort("enable", enable);
        const auto data = graph.createValue(dataSym, 8, false);
        const auto constant = graph.createOperation(grh::OperationKind::kConstant,
                                                    graph.internSymbol("data_const"));
        graph.setAttr(constant, "constValue", std::string("8'h00"));
        graph.addResult(constant, data);
        graph.bindOutputPort("data", data);
        design.markAsTop("top");

        graph.addDeclaredSymbol(enableSym);
        graph.addDeclaredSymbol(dataSym);
        graph.addDeclaredSymbol(sig0Sym);
        graph.addDeclaredSymbol(sig1Sym);
        // Duplicate adds collapse; the list keeps first-insertion order.
        graph.addDeclaredSymbol(dataSym);
        const std::size_t group = graph.addGenerateGroup(graph.internSymbol("gen_loop"),
                                                         graph.internSymbol("sig"));
        graph.addGenerateGroupSymbol(group, sig0Sym);
        graph.addGenerateGroupSymbol(group, sig1Sym);
        return design;
    }

    int runDeclaredSymbolMetadataTest(const std::filesystem::path &artifactDir)
    {
        const std::vector<std::string> expectedDeclared{"enable", "data",
                                                        "gen_loop$0$sig", "gen_loop$1$sig"};
        const std::vector<GenerateGroupTexts> expectedGroups{
            GenerateGroupTexts{"gen_loop", "sig", {"gen_loop$0$sig", "gen_loop$1$sig"}}};

        // (a) Lowering carries declaredSymbols and generateGroups into the model.
        auto design = makeDeclaredMetadataDesign();
        diag::Diagnostics diagnostics;
        grhsim::GrhToGrhSimOptions options;
        options.top = "top";
        options.logicDomain = grhsim::LogicDomain::TwoState;
        auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
        if (!model || diagnostics.hasError()) return fail("metadata GRH lowering failed");
        if (declaredSymbolTexts(*model) != expectedDeclared)
            return fail("lowered declaredSymbols do not match the GRH graph");
        if (generateGroupTexts(*model) != expectedGroups)
            return fail("lowered generateGroups do not match the GRH graph");
        if (!model->isDeclaredSymbol(model->strings().lookup("gen_loop$1$sig")) ||
            model->isDeclaredSymbol(model->strings().lookup("data_const")) ||
            model->isDeclaredSymbol(grhsim::StringId{}))
            return fail("isDeclaredSymbol disagrees with the declared set");

        // (b) keepDeclaredSymbols=false drops both lists.
        {
            auto stripped = makeDeclaredMetadataDesign();
            diag::Diagnostics stripDiagnostics;
            grhsim::GrhToGrhSimOptions stripOptions;
            stripOptions.top = "top";
            stripOptions.logicDomain = grhsim::LogicDomain::TwoState;
            stripOptions.keepDeclaredSymbols = false;
            auto strippedModel = grhsim::lowerGrhToGrhSim(stripped, stripOptions, stripDiagnostics);
            if (!strippedModel || stripDiagnostics.hasError())
                return fail("keepDeclaredSymbols=false lowering failed");
            if (!strippedModel->declaredSymbols().empty() || !strippedModel->generateGroups().empty())
                return fail("keepDeclaredSymbols=false retained metadata");
        }

        // (c) JSON round trip preserves both lists and stays byte stable.
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / "grhsim_declared.json";
        const auto secondPath = artifactDir / "grhsim_declared_roundtrip.json";
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(*model, firstPath, grhsim::defaultDialectRegistry(),
                                      storeDiagnostics))
            return fail("declared-symbol GrhSIM JSON store failed");
        if (readFile(firstPath).find("\"declaredSymbols\"") == std::string::npos ||
            readFile(firstPath).find("\"generateGroups\"") == std::string::npos)
            return fail("serialized JSON is missing the metadata keys");
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(),
                                              loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("declared-symbol JSON load failed");
        if (declaredSymbolTexts(*loaded) != expectedDeclared ||
            generateGroupTexts(*loaded) != expectedGroups)
            return fail("JSON round trip changed the declared metadata");
        if (!loaded->isDeclaredSymbol(loaded->strings().lookup("gen_loop$0$sig")))
            return fail("loaded model lost declared membership");
        if (loaded->semanticRevision() != 1 || loaded->metadataRevision() != 1)
            return fail("loaded model revisions must restart at one");
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return fail("declared-symbol round-trip store failed");
        if (readFile(firstPath) != readFile(secondPath))
            return fail("declared-symbol store/load/store did not produce stable bytes");

        // (d) A metadata-free checkpoint serializes without the trailing keys,
        // and a v1 format marker is rejected: v2 does not accept v1 checkpoints.
        {
            auto legacy = makeDeclaredMetadataDesign();
            diag::Diagnostics legacyDiagnostics;
            grhsim::GrhToGrhSimOptions legacyOptions;
            legacyOptions.top = "top";
            legacyOptions.logicDomain = grhsim::LogicDomain::TwoState;
            legacyOptions.keepDeclaredSymbols = false;
            auto legacyModel = grhsim::lowerGrhToGrhSim(legacy, legacyOptions, legacyDiagnostics);
            if (!legacyModel) return fail("legacy-format fixture lowering failed");
            std::stringstream serialized;
            diag::Diagnostics writeDiagnostics;
            if (!grhsim::writeGrhSimJson(*legacyModel, serialized, grhsim::defaultDialectRegistry(),
                                         writeDiagnostics))
                return fail("legacy-format fixture store failed");
            if (serialized.str().find("\"declaredSymbols\"") != std::string::npos)
                return fail("metadata-free model should serialize without the trailing keys");
            std::string v1Marked = serialized.str();
            const auto markerPos = v1Marked.find("wolvrix.grhsim.v2");
            if (markerPos == std::string::npos) return fail("stored v2 format marker is missing");
            v1Marked.replace(markerPos, std::string("wolvrix.grhsim.v2").size(), "wolvrix.grhsim.v1");
            std::stringstream v1Stream(v1Marked);
            diag::Diagnostics readDiagnostics;
            if (grhsim::readGrhSimJson(v1Stream, grhsim::defaultDialectRegistry(),
                                       readDiagnostics) || !readDiagnostics.hasError())
                return fail("v1-marked checkpoint was not rejected");
        }

        // (e) clone() preserves both lists.
        {
            auto copy = model->clone();
            if (declaredSymbolTexts(copy) != expectedDeclared ||
                generateGroupTexts(copy) != expectedGroups)
                return fail("clone() dropped the declared metadata");
            if (!copy.isDeclaredSymbol(copy.strings().lookup("enable")))
                return fail("clone() lost declared membership");
        }
        return 0;
    }

    grh::Design makeDeclProvenanceDesign()
    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        const auto enableSym = graph.internSymbol("enable");
        const auto dataSym = graph.internSymbol("data");
        const auto sig0Sym = graph.internSymbol("gen_loop$0$sig");
        const auto sig1Sym = graph.internSymbol("gen_loop$1$sig");
        const auto qSym = graph.internSymbol("q");
        const auto memSym = graph.internSymbol("mem");
        const auto dpiSym = graph.internSymbol("dpi_inc");
        const auto danglingSym = graph.internSymbol("dangling");

        const auto enable = graph.createValue(enableSym, 1, false);
        graph.bindInputPort("enable", enable);
        const auto data = graph.createValue(dataSym, 8, false);
        const auto dataConst = graph.createOperation(grh::OperationKind::kConstant,
                                                     graph.internSymbol("data_const"));
        graph.setAttr(dataConst, "constValue", std::string("8'h00"));
        graph.addResult(dataConst, data);
        graph.bindOutputPort("data", data);

        const auto sig0 = graph.createValue(sig0Sym, 4, false);
        const auto sig0Const = graph.createOperation(grh::OperationKind::kConstant,
                                                     graph.internSymbol("sig0_const"));
        graph.setAttr(sig0Const, "constValue", std::string("4'h0"));
        graph.addResult(sig0Const, sig0);
        const auto sig1 = graph.createValue(sig1Sym, 4, false);
        const auto sig1Const = graph.createOperation(grh::OperationKind::kConstant,
                                                     graph.internSymbol("sig1_const"));
        graph.setAttr(sig1Const, "constValue", std::string("4'h1"));
        graph.addResult(sig1Const, sig1);

        const auto q = graph.createOperation(grh::OperationKind::kRegister, qSym);
        graph.setAttr(q, "width", int64_t{8});
        graph.setAttr(q, "isSigned", false);
        graph.setAttr(q, "initValue", std::string("8'h01"));
        const auto mem = graph.createOperation(grh::OperationKind::kMemory, memSym);
        graph.setAttr(mem, "width", int64_t{8});
        graph.setAttr(mem, "row", int64_t{4});
        graph.setAttr(mem, "isSigned", false);
        graph.createOperation(grh::OperationKind::kDpicImport, dpiSym);
        // Declared but detached (no producer, no users, not a port): the value
        // lowering skips it, so it stays a bare anchor without a record.
        graph.createValue(danglingSym, 8, false);

        for (const auto sym : {enableSym, dataSym, sig0Sym, sig1Sym, qSym, memSym, dpiSym,
                               danglingSym})
            graph.addDeclaredSymbol(sym);
        const std::size_t group = graph.addGenerateGroup(graph.internSymbol("gen_loop"),
                                                         graph.internSymbol("sig"));
        graph.addGenerateGroupSymbol(group, sig0Sym);
        graph.addGenerateGroupSymbol(group, sig1Sym);
        design.markAsTop("top");
        return design;
    }

    const grhsim::DeclProvenance *findProvenance(const grhsim::GrhSimModel &model,
                                                 std::string_view name)
    {
        return model.findDeclProvenance(model.strings().lookup(name));
    }

    bool provenanceSlicesEqual(const grhsim::DeclProvenance &lhs,
                               const grhsim::DeclProvenance &rhs)
    {
        return lhs.width == rhs.width && lhs.shape == rhs.shape && lhs.slices == rhs.slices;
    }

    int runDeclProvenanceTest(const std::filesystem::path &artifactDir)
    {
        // (a) Lowering resolves every declared symbol to a Direct full-range slice.
        auto design = makeDeclProvenanceDesign();
        diag::Diagnostics diagnostics;
        grhsim::GrhToGrhSimOptions options;
        options.top = "top";
        options.logicDomain = grhsim::LogicDomain::TwoState;
        auto model = grhsim::lowerGrhToGrhSim(design, options, diagnostics);
        if (!model || diagnostics.hasError()) return fail("provenance GRH lowering failed");
        for (const auto symbol : model->declaredSymbols())
        {
            if (model->text(symbol) == "dangling") continue;
            if (!model->findDeclProvenance(symbol))
                return fail("lowered model lost the provenance of a declared symbol");
        }
        if (model->declProvenances().size() != 7)
            return fail("unexpected lowered provenance record count");
        const grhsim::DeclProvenance *enable = findProvenance(*model, "enable");
        if (!enable || enable->width != 1 || !enable->shape.empty() || enable->slices.size() != 1 ||
            enable->slices[0].kind != grhsim::DeclProvenanceKind::Direct ||
            enable->slices[0].target != grhsim::DeclProvenanceTarget::Value ||
            enable->slices[0].width != 1 || !enable->origin.valid())
            return fail("input port provenance is wrong");
        const auto &enableValue = model->values()[enable->slices[0].targetIndex - 1];
        if (model->text(enableValue.name) != "enable")
            return fail("input port provenance does not resolve to the port value");
        const grhsim::DeclProvenance *data = findProvenance(*model, "data");
        if (!data || data->width != 8 || data->slices.size() != 1 ||
            data->slices[0].target != grhsim::DeclProvenanceTarget::Value ||
            data->slices[0].width != 8)
            return fail("declared wire provenance is wrong");
        const grhsim::DeclProvenance *q = findProvenance(*model, "q");
        if (!q || q->width != 8 || !q->shape.empty() || q->slices.size() != 1 ||
            q->slices[0].target != grhsim::DeclProvenanceTarget::State ||
            q->slices[0].width != 8)
            return fail("register provenance is wrong");
        if (model->text(model->states()[q->slices[0].targetIndex - 1].name) != "q")
            return fail("register provenance does not resolve to the register state");
        const grhsim::DeclProvenance *mem = findProvenance(*model, "mem");
        if (!mem || mem->width != 8 || mem->shape != std::vector<uint64_t>{4} ||
            mem->slices.size() != 1 ||
            mem->slices[0].target != grhsim::DeclProvenanceTarget::State ||
            mem->slices[0].width != 32)
            return fail("memory provenance lost its array shape");
        const grhsim::DeclProvenance *dpi = findProvenance(*model, "dpi_inc");
        if (!dpi || dpi->width != 0 || !dpi->shape.empty() || dpi->slices.size() != 1 ||
            dpi->slices[0].target != grhsim::DeclProvenanceTarget::Function ||
            dpi->slices[0].width != 0)
            return fail("DPI import provenance is wrong");
        // Generate-group membership joins with provenance records by name.
        for (const auto &group : model->generateGroups())
            for (const auto member : group.symbols)
                if (!model->findDeclProvenance(member))
                    return fail("generate group member has no provenance record");
        // The detached declared value keeps its anchor but has no record.
        if (!model->isDeclaredSymbol(model->strings().lookup("dangling")) ||
            findProvenance(*model, "dangling") != nullptr)
            return fail("detached declared value should stay a bare anchor");

        // (b) keepDeclaredSymbols=false drops the provenance records as well.
        {
            auto stripped = makeDeclProvenanceDesign();
            diag::Diagnostics stripDiagnostics;
            grhsim::GrhToGrhSimOptions stripOptions;
            stripOptions.top = "top";
            stripOptions.logicDomain = grhsim::LogicDomain::TwoState;
            stripOptions.keepDeclaredSymbols = false;
            auto strippedModel = grhsim::lowerGrhToGrhSim(stripped, stripOptions, stripDiagnostics);
            if (!strippedModel || stripDiagnostics.hasError())
                return fail("keepDeclaredSymbols=false provenance lowering failed");
            if (!strippedModel->declProvenances().empty())
                return fail("keepDeclaredSymbols=false retained provenance records");
        }

        // (c) JSON round trip preserves the records and stays byte stable.
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / "grhsim_decl_provenance.json";
        const auto secondPath = artifactDir / "grhsim_decl_provenance_roundtrip.json";
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(*model, firstPath, grhsim::defaultDialectRegistry(),
                                      storeDiagnostics))
            return fail("provenance GrhSIM JSON store failed");
        if (readFile(firstPath).find("\"declProvenance\"") == std::string::npos)
            return fail("serialized JSON is missing the declProvenance key");
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(),
                                              loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("provenance JSON load failed");
        if (loaded->declProvenances().size() != model->declProvenances().size())
            return fail("JSON round trip changed the provenance record count");
        for (const auto &record : model->declProvenances())
        {
            const grhsim::DeclProvenance *other =
                loaded->findDeclProvenance(loaded->strings().lookup(model->text(record.symbol)));
            if (!other || !provenanceSlicesEqual(record, *other))
                return fail("JSON round trip changed a provenance record");
        }
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return fail("provenance round-trip store failed");
        if (readFile(firstPath) != readFile(secondPath))
            return fail("provenance store/load/store did not produce stable bytes");

        // (d) clone() preserves the records.
        {
            auto copy = model->clone();
            if (copy.declProvenances().size() != model->declProvenances().size())
                return fail("clone() dropped provenance records");
            const grhsim::DeclProvenance *copied = findProvenance(copy, "mem");
            if (!copied || !provenanceSlicesEqual(*copied, *mem))
                return fail("clone() changed a provenance record");
        }

        // (e) compact() remaps slice targets and drops slices whose target was
        // removed; verifier rejects corrupt records. Hand-built model: two
        // states + one DPI function, all declared.
        {
            grhsim::GrhSimModel hand("prov_model");
            hand.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto logic8 = hand.logicType(8, false, grhsim::LogicDomain::TwoState);
            const auto s1 = hand.addState("s1", logic8);
            const auto s2 = hand.addState("s2", logic8);
            const auto func = hand.addExternFunction("dpi_f", "core.dpi", "dpi_f", {}, {});
            for (const auto state : {s1, s2})
            {
                const std::array steps{grhsim::InitStep{hand.intern("core.init.const"), {0, 1}}};
                const std::array params{grhsim::Parameter{hand.intern("value"),
                                                          std::string("8'h0")}};
                hand.addInit(state, steps, params);
            }
            hand.addDeclaredSymbol(hand.strings().lookup("s1"));
            hand.addDeclaredSymbol(hand.strings().lookup("s2"));
            hand.addDeclaredSymbol(hand.strings().lookup("dpi_f"));
            auto directState = [](grhsim::StringId symbol, uint32_t target) {
                grhsim::DeclProvenance record;
                record.symbol = symbol;
                record.width = 8;
                record.slices.push_back(grhsim::DeclProvenanceSlice{
                    grhsim::DeclProvenanceKind::Direct, grhsim::DeclProvenanceTarget::State,
                    target, 0, 0, 8});
                return record;
            };
            hand.upsertDeclProvenance(directState(hand.strings().lookup("s1"), s1.index));
            hand.upsertDeclProvenance(directState(hand.strings().lookup("s2"), s2.index));
            {
                grhsim::DeclProvenance record;
                record.symbol = hand.strings().lookup("dpi_f");
                record.slices.push_back(grhsim::DeclProvenanceSlice{
                    grhsim::DeclProvenanceKind::Direct, grhsim::DeclProvenanceTarget::Function,
                    func.index, 0, 0, 0});
                hand.upsertDeclProvenance(std::move(record));
            }
            diag::Diagnostics handDiagnostics;
            if (!grhsim::verifyGrhSimModel(hand, grhsim::defaultDialectRegistry(), handDiagnostics))
                return fail("hand-built provenance model did not verify");

            // compact removes s1: its slice is dropped, s2 is renumbered to 1.
            std::vector<uint8_t> removeOps(hand.operations().size() + 1, 0);
            std::vector<uint8_t> removeStates(hand.states().size() + 1, 0);
            removeStates[s1.index] = 1;
            hand.compact(removeOps, removeStates);
            const grhsim::DeclProvenance *dropped = findProvenance(hand, "s1");
            const grhsim::DeclProvenance *remapped = findProvenance(hand, "s2");
            if (!dropped || !dropped->slices.empty())
                return fail("compact() did not drop the removed state's slice");
            if (!remapped || remapped->slices.size() != 1 || remapped->slices[0].targetIndex != 1)
                return fail("compact() did not remap the surviving state's slice");
            diag::Diagnostics compactDiagnostics;
            if (!grhsim::verifyGrhSimModel(hand, grhsim::defaultDialectRegistry(),
                                           compactDiagnostics))
                return fail("compacted provenance model did not verify");

            auto expectRejected = [&](const grhsim::DeclProvenance &record,
                                      const char *message) {
                hand.upsertDeclProvenance(record);
                diag::Diagnostics corruptDiagnostics;
                if (grhsim::verifyGrhSimModel(hand, grhsim::defaultDialectRegistry(),
                                              corruptDiagnostics))
                    return fail(message);
                hand.upsertDeclProvenance(directState(hand.strings().lookup("s2"), 1));
                return 0;
            };
            // Out-of-range target.
            if (const int status = expectRejected(
                    directState(hand.strings().lookup("s2"), 999),
                    "verifier accepted an out-of-range provenance target"))
                return status;
            // Slice range exceeds the target.
            {
                auto record = directState(hand.strings().lookup("s2"), 1);
                record.slices[0].targetOffset = 4;
                if (const int status =
                        expectRejected(record, "verifier accepted an overflowing target range"))
                    return status;
            }
            // Slice range exceeds the declaration.
            {
                auto record = directState(hand.strings().lookup("s2"), 1);
                record.slices[0].declOffset = 4;
                if (const int status = expectRejected(
                        record, "verifier accepted an overflowing declaration range"))
                    return status;
            }
            // Overlapping split ranges within one declaration.
            {
                auto record = directState(hand.strings().lookup("s2"), 1);
                record.slices[0].width = 4;
                record.slices.push_back(grhsim::DeclProvenanceSlice{
                    grhsim::DeclProvenanceKind::Direct, grhsim::DeclProvenanceTarget::State,
                    1, 4, 2, 4});
                if (const int status = expectRejected(
                        record, "verifier accepted overlapping declaration ranges"))
                    return status;
            }
            // Whole-object marker on a bit-carrying declaration and target.
            {
                auto record = directState(hand.strings().lookup("s2"), 1);
                record.slices[0].width = 0;
                if (const int status = expectRejected(
                        record, "verifier accepted a whole-object slice on bit-carrying entities"))
                    return status;
            }
            // A record for an undeclared symbol is rejected at the API.
            {
                grhsim::DeclProvenance record;
                record.symbol = hand.intern("ghost");
                bool thrown = false;
                try
                {
                    hand.upsertDeclProvenance(std::move(record));
                }
                catch (const std::invalid_argument &)
                {
                    thrown = true;
                }
                if (!thrown) return fail("upsert accepted a provenance for an undeclared symbol");
            }
        }
        return 0;
    }
}

namespace {
    int runBitwiseMuxGuardsTest() {
        using namespace grhsim;
        for (unsigned scenario = 0; scenario < 8; ++scenario) {
            GrhSimModel model("bitwise_mux_guards"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto type = model.logicType(scenario == 0 ? 8 : 1, scenario == 1,
                scenario == 2 ? LogicDomain::FourState : LogicDomain::TwoState);
            const auto input = [&](TypeId inputType) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), inputType);
                const auto value = model.addValue(inputType);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            const auto condition = input(scenario == 3 ? model.logicType(8, false, LogicDomain::TwoState) : type);
            const auto a = input(type), b = input(scenario == 4 ? model.logicType(1, true, LogicDomain::TwoState) : type);
            const auto result = model.addValue(type);
            std::vector<ValueId> args{condition, a, b};
            if (scenario == 7) args.pop_back();
            const std::array params{Parameter{model.intern("extension"), true}};
            const std::array refs{ObjectRef::input({1, 0})};
            model.addOperation("core.compute.mux", args, std::array{result},
                scenario == 6 ? std::span<const ObjectRef>(refs) : std::span<const ObjectRef>{},
                scenario == 5 ? std::span<const Parameter>(params) : std::span<const Parameter>{});
            PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
            manager.addPass(defaultPassRegistry().create("grhsim.bitwise-muxes", {}, error));
            const auto outcome = manager.run(model, diagnostics);
            if (!outcome.success || outcome.changed) return fail("bitwise mux ignored a type, shape or metadata guard");
        }
        for (unsigned defect = 0; defect < 7; ++defect) {
            GrhSimModel model("invalid_bit_select"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto type = model.logicType(defect == 4 ? 65 : 8, false,
                defect == 3 ? LogicDomain::FourState : LogicDomain::TwoState);
            const auto input = [&](TypeId inputType) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), inputType);
                const auto value = model.addValue(inputType);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            const auto mask = input(type), a = input(type), b = input(type);
            std::vector<ValueId> args{mask, a, b};
            if (defect == 0) args.pop_back();
            if (defect == 1) args.back() = input(model.logicType(8, true, LogicDomain::TwoState));
            const auto result = model.addValue(defect == 2 ? model.logicType(4, false, LogicDomain::TwoState) : type);
            const std::array params{Parameter{model.intern("extension"), true}};
            const std::array refs{ObjectRef::input({1, 0})};
            model.addOperation("core.compute.bitSelect", args, std::array{result},
                defect == 5 ? std::span<const ObjectRef>(refs) : std::span<const ObjectRef>{},
                defect == 6 ? std::span<const Parameter>(params) : std::span<const Parameter>{});
            diag::Diagnostics diagnostics;
            if (verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics))
                return fail("malformed bitSelect passed verification");
        }
        return 0;
    }
}

namespace {
    int runMuxChainFoldTest() {
        using namespace grhsim;
        const auto buildChain = [](GrhSimModel &model, unsigned links, TypeId resultType,
                                   const auto &input, ValueId dflt,
                                   std::vector<ValueId> *conds = nullptr, std::vector<ValueId> *arms = nullptr) {
            std::vector<ValueId> cs, as;
            for (unsigned i = 0; i < links; ++i) {
                cs.push_back(input(model.logicType(1, false, LogicDomain::TwoState)));
                as.push_back(input(resultType));
            }
            ValueId link = dflt;
            for (unsigned i = links; i-- > 0;) {
                const auto value = model.addValue(resultType);
                model.addOperation("core.compute.mux", std::array{cs[i], as[i], link}, std::array{value});
                link = value;
            }
            if (conds) *conds = cs;
            if (arms) *arms = as;
            return link;
        };
        // Positive: a four-link priority chain folds into one prioritySelect and
        // the pass is idempotent.
        {
            GrhSimModel model("mux_chain_fold"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto input = [&](TypeId type) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
                const auto value = model.addValue(type);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            std::vector<ValueId> conds, arms;
            const auto dflt = input(byte);
            const auto root = buildChain(model, 4, byte, input, dflt, &conds, &arms);
            (void)root;
            const auto output = model.addOutput("o", byte);
            model.addOperation("core.output.write", std::array{root}, {}, std::array{ObjectRef::output(output)});
            PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
            manager.addPass(defaultPassRegistry().create("grhsim.mux-chain-fold", {}, error));
            const auto first = manager.run(model, diagnostics);
            if (!first.success || !first.changed) return fail("mux chain fold missed a four-link chain");
            // compact() renumbers values; re-resolve the input reads by port.
            std::map<uint32_t, ValueId> byPort;
            unsigned selects = 0, muxes = 0;
            const SimOp *select = nullptr;
            for (const auto &op : model.operations()) {
                if (model.text(op.opType) == "core.input.read")
                    byPort.emplace(model.objectRefs(op)[0].index, model.results(op)[0]);
                else if (model.text(op.opType) == "core.compute.prioritySelect") { ++selects; select = &op; }
                else muxes += model.text(op.opType) == "core.compute.mux";
            }
            if (selects != 1 || muxes != 0 || byPort.size() != 9) return fail("mux chain fold left residual links");
            const auto operands = model.operands(*select);
            if (operands.size() != 9 || model.results(*select).size() != 1)
                return fail("prioritySelect arity mismatch");
            for (unsigned i = 0; i < 4; ++i)
                if (operands[i] != byPort[2 * i + 2] || operands[4 + i] != byPort[2 * i + 3] || operands[8] != byPort[1])
                    return fail("prioritySelect operand order changed");
            bool feedsOutput = false;
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.output.write" &&
                    model.operands(op)[0] == model.results(*select)[0]) feedsOutput = true;
            if (!feedsOutput) return fail("prioritySelect result lost its consumer");
            const auto second = manager.run(model, diagnostics);
            if (!second.success || second.changed) return fail("mux chain fold is not idempotent");
        }
        // Guards: a two-link chain never folds; a tapped middle link stops the
        // chain (only a long enough suffix folds); wide conditions and mismatched
        // link result types block the fold.
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            GrhSimModel model("mux_chain_guards"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto word = model.logicType(16, false, LogicDomain::TwoState);
            const auto input = [&](TypeId type) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
                const auto value = model.addValue(type);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            const auto dflt = input(byte);
            const auto muxInto = [&](ValueId c, ValueId a, ValueId b, TypeId type) {
                const auto value = model.addValue(type);
                model.addOperation("core.compute.mux", std::array{c, a, b}, std::array{value});
                return value;
            };
            unsigned expectedSelects = 0, expectedMuxes = 0;
            if (scenario == 0) {
                const auto root = buildChain(model, 2, byte, input, dflt);
                const auto output = model.addOutput("o", byte);
                model.addOperation("core.output.write", std::array{root}, {}, std::array{ObjectRef::output(output)});
                expectedMuxes = 2;
            } else if (scenario == 1) {
                // Four links, but the middle link's value is also observed: only
                // the three-link suffix below the tap may fold.
                const auto m4 = muxInto(input(bit), input(byte), muxInto(input(bit), input(byte), dflt, byte), byte);
                const auto m2 = muxInto(input(bit), input(byte), m4, byte);
                const auto m1 = muxInto(input(bit), input(byte), m2, byte);
                const auto m0 = muxInto(input(bit), input(byte), m1, byte);
                const auto tap = model.addOutput("tap", byte);
                model.addOperation("core.output.write", std::array{m2}, {}, std::array{ObjectRef::output(tap)});
                const auto output = model.addOutput("o", byte);
                model.addOperation("core.output.write", std::array{m0}, {}, std::array{ObjectRef::output(output)});
                expectedSelects = 1; expectedMuxes = 2;
            } else {
                // The deep link's condition is two bits wide and its result type
                // differs from the root's: no fold anywhere.
                const auto wide = model.logicType(2, false, LogicDomain::TwoState);
                const auto m2 = muxInto(input(wide), input(word), input(word), word);
                const auto m1 = muxInto(input(bit), input(byte), input(byte), byte);
                const auto m0 = muxInto(input(bit), input(byte), m1, byte);
                const auto output = model.addOutput("o", byte);
                model.addOperation("core.output.write", std::array{m0}, {}, std::array{ObjectRef::output(output)});
                (void)m2;
                expectedMuxes = 3;
            }
            PassManager manager(defaultDialectRegistry()); std::string error; diag::Diagnostics diagnostics;
            manager.addPass(defaultPassRegistry().create("grhsim.mux-chain-fold", {}, error));
            const auto result = manager.run(model, diagnostics);
            if (!result.success) return fail("mux chain fold rejected a guard model");
            if (scenario == 0 && result.changed) return fail("mux chain fold rewrote a two-link chain");
            unsigned selects = 0, muxes = 0;
            for (const auto &op : model.operations()) {
                selects += model.text(op.opType) == "core.compute.prioritySelect";
                muxes += model.text(op.opType) == "core.compute.mux";
            }
            if (selects != expectedSelects || muxes != expectedMuxes)
                return fail("mux chain fold ignored a multi-use, length, condition-width or type guard");
        }
        // The verifier rejects malformed prioritySelect shapes.
        for (unsigned defect = 0; defect < 7; ++defect) {
            GrhSimModel model("invalid_priority_select"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto input = [&](TypeId type) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
                const auto value = model.addValue(type);
                model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
                return value;
            };
            std::vector<ValueId> args;
            for (unsigned i = 0; i < 4; ++i)
                args.push_back(input(defect == 2 && i == 1 ? model.logicType(2, false, LogicDomain::TwoState) : bit));
            for (unsigned i = 0; i < 4; ++i)
                args.push_back(input(defect == 3 && i == 1 ? model.logicType(8, false, LogicDomain::FourState) :
                                     defect == 4 && i == 1 ? model.logicType(65, false, LogicDomain::TwoState) : byte));
            args.push_back(input(byte));
            if (defect == 0) args.pop_back();
            if (defect == 1) { args.erase(args.begin(), args.begin() + 2); args.erase(args.begin() + 2, args.begin() + 4); }
            const auto result = model.addValue(byte);
            const std::array params{Parameter{model.intern("extension"), true}};
            const std::array refs{ObjectRef::input({1, 0})};
            model.addOperation("core.compute.prioritySelect", args, std::array{result},
                defect == 5 ? std::span<const ObjectRef>(refs) : std::span<const ObjectRef>{},
                defect == 6 ? std::span<const Parameter>(params) : std::span<const Parameter>{});
            diag::Diagnostics diagnostics;
            if (verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics))
                return fail("malformed prioritySelect passed verification");
        }
        return 0;
    }
}

namespace {
    int runUsedBitsTest() {
        using namespace grhsim;
        const auto inputOf = [](GrhSimModel &model, TypeId type) {
            const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
            const auto value = model.addValue(type);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto outputOf = [](GrhSimModel &model, ValueId value, TypeId type) {
            const auto port = model.addOutput("o" + std::to_string(model.outputs().size()), type);
            model.addOperation("core.output.write", std::array{value}, {}, std::array{ObjectRef::output(port)});
        };
        const auto sliceOf = [](GrhSimModel &model, ValueId value, int64_t start, int64_t end, bool sign = false) {
            const auto type = model.logicType(static_cast<uint32_t>(end - start + 1), sign, LogicDomain::TwoState);
            const auto result = model.addValue(type);
            const std::array params{Parameter{model.intern("sliceStart"), start},
                                    Parameter{model.intern("sliceEnd"), end}};
            model.addOperation("core.compute.sliceStatic", std::array{value}, std::array{result}, {}, params);
            return result;
        };
        const auto widthOf = [](const GrhSimModel &model, ValueId value) {
            return model.types()[model.values()[value.index - 1].type.index - 1].width;
        };
        const auto runPass = [](GrhSimModel &model) {
            PassManager manager(defaultDialectRegistry());
            std::string error;
            manager.addPass(defaultPassRegistry().create("grhsim.used-bits", {}, error));
            diag::Diagnostics diagnostics;
            const auto result = manager.run(model, diagnostics);
            if (!result.success || diagnostics.hasError()) {
                for (const auto &message : diagnostics.messages())
                    std::cerr << "[grhsim-ir] used-bits diagnostic: " << message.context << ": " << message.message << '\n';
            }
            return result;
        };
        // Scalar chain: a 64-bit add observed through a 39-bit slice narrows.
        {
            GrhSimModel model("used_bits_scalar"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto wide = model.logicType(64, false, LogicDomain::TwoState);
            const auto a = inputOf(model, wide), b = inputOf(model, wide);
            const auto sum = model.addValue(wide);
            model.addOperation("core.compute.add", std::array{a, b}, std::array{sum});
            outputOf(model, sliceOf(model, sum, 0, 38), model.logicType(39, false, LogicDomain::TwoState));
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed a scalar narrowing");
            bool narrowed = false;
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.compute.add")
                    narrowed = widthOf(model, model.results(op)[0]) == 39;
            if (!narrowed) return fail("used-bits add was not narrowed to 39 bits");
            if (const auto again = runPass(model); !again.success || again.changed)
                return fail("used-bits scalar narrowing is not idempotent");
        }
        // Wide downgrade: a 128-bit and observed through 64 bits becomes scalar.
        {
            GrhSimModel model("used_bits_wide"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto wide = model.logicType(128, false, LogicDomain::TwoState);
            const auto a = inputOf(model, wide), b = inputOf(model, wide);
            const auto masked = model.addValue(wide);
            model.addOperation("core.compute.and", std::array{a, b}, std::array{masked});
            outputOf(model, sliceOf(model, masked, 0, 63), model.logicType(64, false, LogicDomain::TwoState));
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed a wide downgrade");
            bool narrowed = false;
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.compute.and")
                    narrowed = widthOf(model, model.results(op)[0]) == 64;
            if (!narrowed) return fail("used-bits wide and was not downgraded to 64 bits");
        }
        // Non-transparent op: lshr keeps its width; a transparent consumer that
        // narrows forces a boundary slice between them.
        {
            GrhSimModel model("used_bits_lshr"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto word = model.logicType(64, false, LogicDomain::TwoState);
            const auto data = inputOf(model, word), amount = inputOf(model, model.logicType(6, false, LogicDomain::TwoState));
            const auto other = inputOf(model, word);
            const auto shifted = model.addValue(word);
            model.addOperation("core.compute.lshr", std::array{data, amount}, std::array{shifted});
            const auto masked = model.addValue(word);
            model.addOperation("core.compute.and", std::array{shifted, other}, std::array{masked});
            outputOf(model, sliceOf(model, masked, 0, 19), model.logicType(20, false, LogicDomain::TwoState));
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed an lshr boundary");
            unsigned wideLshr = 0, boundarySlices = 0, narrowAnds = 0;
            for (const auto &op : model.operations()) {
                if (model.text(op.opType) == "core.compute.lshr")
                    wideLshr += widthOf(model, model.results(op)[0]) == 64;
                if (model.text(op.opType) == "core.compute.and")
                    narrowAnds += widthOf(model, model.results(op)[0]) == 20;
                if (model.text(op.opType) == "core.compute.sliceStatic" &&
                    widthOf(model, model.results(op)[0]) == 20)
                    for (const auto operand : model.operands(op))
                        boundarySlices += widthOf(model, operand) == 64;
            }
            if (wideLshr != 1 || narrowAnds != 1 || boundarySlices != 1)
                return fail("used-bits lshr boundary shape is wrong");
        }
        // Register narrowing: state, write port data/mask and reads all shrink;
        // the event history and init record survive.
        {
            GrhSimModel model("used_bits_state"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto word = model.logicType(64, false, LogicDomain::TwoState);
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto state = model.addState("q", word);
            const auto history = model.addState("q_clock_history", bit);
            const std::array initParams{Parameter{model.intern("value"), std::string("64'h0")}};
            const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(state, initSteps, initParams);
            const std::array historyInitParams{Parameter{model.intern("value"), std::string("1'h0")}};
            model.addInit(history, initSteps, historyInitParams);
            const auto clock = inputOf(model, bit), enable = inputOf(model, bit);
            const auto next = inputOf(model, word);
            const auto maskConst = model.addValue(word);
            const std::array maskParams{Parameter{model.intern("constValue"), std::string("64'hffffffffffffffff")}};
            model.addOperation("core.compute.constant", {}, std::array{maskConst}, {}, maskParams);
            const std::vector<std::string> edges{"posedge"};
            const std::array writeParams{Parameter{model.intern("event_edges"), edges}};
            model.addOperation("core.state.regWrite", std::array{enable, next, maskConst, clock}, {},
                std::array{ObjectRef::state(state), ObjectRef::state(history)}, writeParams);
            const auto read = model.addValue(word);
            model.addOperation("core.state.read", {}, std::array{read}, std::array{ObjectRef::state(state)});
            outputOf(model, sliceOf(model, read, 0, 15), model.logicType(16, false, LogicDomain::TwoState));
            const auto statesBefore = model.states().size();
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed a state narrowing");
            if (model.states().size() != statesBefore) return fail("used-bits changed the state count on narrowing");
            bool narrowState = false, historyKept = false;
            StateId narrowedState;
            for (const auto &entry : model.states())
                if (model.text(entry.name) == "q") {
                    narrowState = model.types()[entry.type.index - 1].width == 16;
                    narrowedState = entry.id;
                }
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.state.regWrite") {
                    const auto refs = model.objectRefs(op);
                    historyKept = refs.size() == 2 && refs[0].kind == ObjectKind::State &&
                                  refs[0].index == narrowedState.index;
                    const auto operands = model.operands(op);
                    if (widthOf(model, operands[1]) != 16 || widthOf(model, operands[2]) != 16)
                        return fail("used-bits regWrite data/mask did not follow the narrowed state");
                }
            if (!narrowState || !historyKept) return fail("used-bits state narrowing broke state/history wiring");
            unsigned initRecords = 0;
            for (const auto &record : model.initRecords())
                initRecords += record.state == narrowedState;
            if (initRecords != 1) return fail("used-bits lost the narrowed state's init record");
        }
        // Dead cone: unused compute chains, unread states and their write ports
        // disappear; live logic is untouched.
        {
            GrhSimModel model("used_bits_dead"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto a = inputOf(model, byte), b = inputOf(model, byte);
            const auto deadNot = model.addValue(byte);
            model.addOperation("core.compute.not", std::array{a}, std::array{deadNot});
            const auto deadAnd = model.addValue(byte);
            model.addOperation("core.compute.and", std::array{deadNot, b}, std::array{deadAnd});
            const auto deadState = model.addState("dead_reg", byte);
            const std::array initParams{Parameter{model.intern("value"), std::string("8'h00")}};
            const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(deadState, initSteps, initParams);
            const auto deadRead = model.addValue(byte);
            model.addOperation("core.state.read", {}, std::array{deadRead}, std::array{ObjectRef::state(deadState)});
            const auto live = model.addValue(byte);
            model.addOperation("core.compute.xor", std::array{a, b}, std::array{live});
            outputOf(model, live, byte);
            const auto opsBefore = model.operations().size();
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed the dead cone");
            if (model.operations().size() != opsBefore - 3)
                return fail("used-bits removed a live op or left dead ops behind");
            if (model.states().size() != 0)
                return fail("used-bits left the unread state behind");
            bool liveKept = false;
            for (const auto &op : model.operations())
                liveKept = liveKept || model.text(op.opType) == "core.compute.xor";
            if (!liveKept) return fail("used-bits removed the live cone");
            if (const auto again = runPass(model); !again.success || again.changed)
                return fail("used-bits dead-cone elimination is not idempotent");
        }
        // Concat straddle: a 24-bit concat observed through 12 bits keeps only
        // the low operands and truncates the straddler.
        {
            GrhSimModel model("used_bits_concat"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto a = inputOf(model, byte), b = inputOf(model, byte), c = inputOf(model, byte);
            const auto joined = model.addValue(model.logicType(24, false, LogicDomain::TwoState));
            model.addOperation("core.compute.concat", std::array{a, b, c}, std::array{joined});
            outputOf(model, sliceOf(model, joined, 0, 11), model.logicType(12, false, LogicDomain::TwoState));
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits missed a concat narrowing");
            bool narrowed = false;
            for (const auto &op : model.operations())
                if (model.text(op.opType) == "core.compute.concat") {
                    narrowed = widthOf(model, model.results(op)[0]) == 12;
                    const auto operands = model.operands(op);
                    if (operands.size() != 2 || widthOf(model, operands[0]) != 4 || widthOf(model, operands[1]) != 8)
                        return fail("used-bits concat kept the wrong segments");
                }
            if (!narrowed) return fail("used-bits concat was not narrowed to 12 bits");
        }
        // A concat may already truncate its operands. When its result stays
        // full width, its original high operand must remain available.
        {
            GrhSimModel model("used_bits_truncated_concat");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto word = model.logicType(32, false, LogicDomain::TwoState);
            const auto three = model.logicType(3, false, LogicDomain::TwoState);
            const auto high = sliceOf(model, inputOf(model, byte), 0, 1);
            const auto low = inputOf(model, word);
            const auto joined = model.addValue(three);
            model.addOperation("core.compute.concat", std::array{high, low}, std::array{joined});
            outputOf(model, joined, three);
            const auto result = runPass(model);
            if (!result.success) return fail("used-bits rejected a truncated concat");
            bool keptHigh = false, keptConcat = false;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) == "core.compute.sliceStatic") keptHigh = true;
                if (model.text(op.opType) == "core.compute.concat")
                    keptConcat = model.operands(op).size() == 2;
            }
            if (!keptHigh || !keptConcat)
                return fail("used-bits removed an operand of a surviving truncated concat");
            if (const auto again = runPass(model); !again.success || again.changed)
                return fail("used-bits truncated concat is not idempotent");
        }
        // Event cone retention: an edgeDet consumes its event value with no
        // data result; the producer cone must not be swept as dead.
        {
            GrhSimModel model("used_bits_event_cone"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto clkPort = model.addInput("clk", bit);
            const auto clk = model.addValue(bit);
            model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{clk},
                                                       std::array{ObjectRef::input(clkPort)}),
                                    SimPhase::Event);
            const auto gated = model.addValue(bit);
            model.setOperationPhase(model.addOperation("core.compute.not", std::array{clk}, std::array{gated}),
                                    SimPhase::Event);
            const std::array params{Parameter{model.intern("edge"), std::string("posedge")},
                                    Parameter{model.intern("act"), int64_t{0}},
                                    Parameter{model.intern("prev"), int64_t{0}},
                                    Parameter{model.intern("prevInit"), std::string("1'h0")}};
            model.setOperationPhase(model.addOperation("core.event.edgeDet", std::array{gated}, {}, {}, params),
                                    SimPhase::Event);
            const auto opsBefore = model.operations().size();
            const auto result = runPass(model);
            if (!result.success) return fail("used-bits rejected an edgeDet cone");
            if (model.operations().size() != opsBefore)
                return fail("used-bits swept the edgeDet event cone");
            diag::Diagnostics verifyDiagnostics;
            if (!verifyGrhSimModel(model, defaultDialectRegistry(), verifyDiagnostics))
                return fail("used-bits broke the edgeDet cone");
        }
        // Rebuilt computes and boundary slices stay inside their Event and
        // Output cones; narrowed register writes keep General-phase adaptors.
        {
            GrhSimModel model("used_bits_phase_cones"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto word = model.logicType(16, false, LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto addRead = [&](TypeId type, SimPhase phase) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
                const auto value = model.addValue(type);
                model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{value},
                                                       std::array{ObjectRef::input(port)}), phase);
                return value;
            };
            const auto addSlice = [&](ValueId value, uint32_t width, SimPhase phase) {
                const auto result = model.addValue(model.logicType(width, false, LogicDomain::TwoState));
                const std::array params{Parameter{model.intern("sliceStart"), int64_t{0}},
                                        Parameter{model.intern("sliceEnd"), int64_t(width - 1)}};
                model.setOperationPhase(model.addOperation("core.compute.sliceStatic", std::array{value},
                                                       std::array{result}, {}, params), phase);
                return result;
            };
            const auto eventInput = addRead(word, SimPhase::Event);
            const auto eventNot = model.addValue(word);
            model.setOperationPhase(model.addOperation("core.compute.not", std::array{eventInput},
                                                   std::array{eventNot}), SimPhase::Event);
            const auto eventBit = addSlice(eventNot, 1, SimPhase::Event);
            const std::array edgeParams{Parameter{model.intern("edge"), std::string("posedge")},
                                        Parameter{model.intern("act"), int64_t{0}},
                                        Parameter{model.intern("prev"), int64_t{0}},
                                        Parameter{model.intern("prevInit"), std::string("1'h0")}};
            model.setOperationPhase(model.addOperation("core.event.edgeDet", std::array{eventBit},
                                                   {}, {}, edgeParams), SimPhase::Event);

            const auto outputInput = addRead(word, SimPhase::Output);
            const auto amount = addRead(model.logicType(4, false, LogicDomain::TwoState), SimPhase::Output);
            const auto shifted = model.addValue(word);
            model.setOperationPhase(model.addOperation("core.compute.lshr", std::array{outputInput, amount},
                                                   std::array{shifted}), SimPhase::Output);
            const auto outputMask = addRead(word, SimPhase::Output);
            const auto masked = model.addValue(word);
            model.setOperationPhase(model.addOperation("core.compute.and", std::array{shifted, outputMask},
                                                   std::array{masked}), SimPhase::Output);
            const auto outValue = addSlice(masked, 8, SimPhase::Output);
            const auto outPort = model.addOutput("out", byte);
            model.setOperationPhase(model.addOperation("core.output.write", std::array{outValue}, {},
                                                   std::array{ObjectRef::output(outPort)}), SimPhase::Output);

            diag::Diagnostics before;
            if (!verifyGrhSimModel(model, defaultDialectRegistry(), before))
                return fail("used-bits phase-cone fixture rejected");
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits failed on phase cones");
            diag::Diagnostics after;
            if (!verifyGrhSimModel(model, defaultDialectRegistry(), after))
                return fail("used-bits crossed the Output phase barrier");
            std::vector<SimPhase> phases(model.values().size() + 1, SimPhase::None);
            for (const auto &op : model.operations())
                for (const auto value : model.results(op)) phases[value.index] = op.phase;
            bool eventNarrowed = false, outputNarrowed = false, outputBoundary = false;
            for (const auto &op : model.operations()) {
                for (const auto value : model.operands(op))
                    if ((op.phase == SimPhase::Event || op.phase == SimPhase::Output) &&
                        phases[value.index] != op.phase)
                        return fail("used-bits created a cross-phase operand");
                if (model.text(op.opType) == "core.compute.not" && op.phase == SimPhase::Event)
                    eventNarrowed = widthOf(model, model.results(op)[0]) == 1;
                if (model.text(op.opType) == "core.compute.and" && op.phase == SimPhase::Output)
                    outputNarrowed = widthOf(model, model.results(op)[0]) == 8;
                if (model.text(op.opType) == "core.compute.sliceStatic" && op.phase == SimPhase::Output)
                    for (const auto operand : model.operands(op))
                        outputBoundary |= widthOf(model, operand) == 16 &&
                                          widthOf(model, model.results(op)[0]) == 8;
            }
            if (!eventNarrowed || !outputNarrowed || !outputBoundary)
                return fail("used-bits phase-cone rewrites were not exercised");
        }
        {
            GrhSimModel model("used_bits_phase_state"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto word = model.logicType(16, true, LogicDomain::TwoState);
            const auto byte = model.logicType(8, true, LogicDomain::TwoState);
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto state = model.addState("q", word);
            const std::array initParams{Parameter{model.intern("value"), std::string("16'h0")}};
            const std::array initSteps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(state, initSteps, initParams);
            const auto addGeneralRead = [&](TypeId type) {
                const auto port = model.addInput("i" + std::to_string(model.inputs().size()), type);
                const auto value = model.addValue(type);
                model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{value},
                                                       std::array{ObjectRef::input(port)}), SimPhase::General);
                return value;
            };
            const auto enable = addGeneralRead(bit), data = addGeneralRead(word), mask = addGeneralRead(word);
            model.setOperationPhase(model.addOperation("core.state.regWrite", std::array{enable, data, mask},
                                                   {}, std::array{ObjectRef::state(state)}), SimPhase::General);
            const auto read = model.addValue(word);
            model.setOperationPhase(model.addOperation("core.state.read", {}, std::array{read},
                                                   std::array{ObjectRef::state(state)}), SimPhase::Output);
            const auto narrowed = model.addValue(byte);
            const std::array sliceParams{Parameter{model.intern("sliceStart"), int64_t{0}},
                                         Parameter{model.intern("sliceEnd"), int64_t{7}}};
            model.setOperationPhase(model.addOperation("core.compute.sliceStatic", std::array{read},
                                                   std::array{narrowed}, {}, sliceParams), SimPhase::Output);
            const auto port = model.addOutput("q_low", byte);
            model.setOperationPhase(model.addOperation("core.output.write", std::array{narrowed}, {},
                                                   std::array{ObjectRef::output(port)}), SimPhase::Output);
            const auto result = runPass(model);
            if (!result.success || !result.changed) return fail("used-bits failed on phased state narrowing");
            diag::Diagnostics diagnostics;
            if (!verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics))
                return fail("used-bits broke phased state narrowing");
            bool readOutput = false, writeGeneral = false, generalAdaptor = false;
            for (const auto &op : model.operations()) {
                const auto name = model.text(op.opType);
                if (name == "core.state.read") readOutput |= op.phase == SimPhase::Output;
                if (name == "core.state.regWrite") writeGeneral |= op.phase == SimPhase::General;
                if (name == "core.compute.sliceStatic" && op.phase == SimPhase::General &&
                    widthOf(model, model.results(op)[0]) == 8)
                    generalAdaptor = true;
            }
            if (!readOutput || !writeGeneral || !generalAdaptor)
                return fail("used-bits lost phases on rebuilt state operations or adaptors");
        }
        return 0;
    }

    // canonicalize-compute must respect the M1 phase barrier: cone clones and
    // their unphased twins share structure but serve different phase domains,
    // so CSE, identity-assign and concat folds must not merge across phases.
    int runCanonicalizePhaseBarrierTest()
    {
        using namespace grhsim;
        GrhSimModel model("canonicalize_phase_barrier");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto port = model.addInput("a", bit);
        const auto a = model.addValue(bit);
        model.addOperation("core.input.read", {}, std::array{a}, std::array{ObjectRef::input(port)});
        // Unphased twins of the Output-cone ops.
        const auto oneGeneral = model.addValue(bit);
        const std::array constParams{Parameter{model.intern("constValue"), std::string("1'h1")}};
        model.addOperation("core.compute.constant", {}, std::array{oneGeneral}, {}, constParams);
        const auto notGeneral = model.addValue(bit);
        model.addOperation("core.compute.not", std::array{a}, std::array{notGeneral});
        const auto outGeneral = model.addOutput("g", bit);
        model.addOperation("core.output.write", std::array{notGeneral}, {},
                           std::array{ObjectRef::output(outGeneral)});
        // Output cone: cloned input read, a same-literal constant and a not.
        const auto aOut = model.addValue(bit);
        model.setOperationPhase(model.addOperation("core.input.read", {}, std::array{aOut},
                                                   std::array{ObjectRef::input(port)}), SimPhase::Output);
        const auto oneOut = model.addValue(bit);
        model.setOperationPhase(model.addOperation("core.compute.constant", {}, std::array{oneOut}, {},
                                                   constParams), SimPhase::Output);
        const auto notOut = model.addValue(bit);
        model.setOperationPhase(model.addOperation("core.compute.not", std::array{aOut},
                                                   std::array{notOut}), SimPhase::Output);
        const auto gatedOut = model.addValue(bit);
        model.setOperationPhase(model.addOperation("core.compute.and", std::array{notOut, oneOut},
                                                   std::array{gatedOut}), SimPhase::Output);
        const auto out = model.addOutput("o", bit);
        model.setOperationPhase(model.addOperation("core.output.write", std::array{gatedOut}, {},
                                                   std::array{ObjectRef::output(out)}), SimPhase::Output);
        diag::Diagnostics preDiagnostics;
        if (!verifyGrhSimModel(model, defaultDialectRegistry(), preDiagnostics))
            return fail("phase-barrier fixture rejected");

        PassManager manager(defaultDialectRegistry()); std::string error;
        manager.addPass(defaultPassRegistry().create("grhsim.canonicalize-compute", {}, error));
        diag::Diagnostics diagnostics;
        const auto result = manager.run(model, diagnostics);
        if (!result.success || diagnostics.hasError())
            return fail("canonicalize-compute failed on the phase-barrier model");
        if (!verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics))
            return fail("canonicalize-compute crossed the phase barrier");
        unsigned constants = 0, outputConstants = 0, nots = 0;
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) == "core.compute.constant")
            {
                ++constants;
                if (op.phase == SimPhase::Output) ++outputConstants;
            }
            if (model.text(op.opType) == "core.compute.not") ++nots;
        }
        if (constants != 2 || outputConstants != 1 || nots != 2)
            return fail("canonicalize-compute merged ops across the phase barrier");
        return 0;
    }
}

namespace {
    int runEdgeDetPhaseTest(const std::filesystem::path &artifactDir) {
        using namespace grhsim;
        const auto inputBit = [](GrhSimModel &model, const char *name) {
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto port = model.addInput(name, bit);
            const auto value = model.addValue(bit, name);
            model.addOperation("core.input.read", {}, std::array{value}, std::array{ObjectRef::input(port)});
            return value;
        };
        const auto edgeDet = [](GrhSimModel &model, ValueId event, std::string edge,
                                int64_t act, int64_t prev, SimPhase phase = SimPhase::Event) {
            const std::array<Parameter, 4> params{Parameter{model.intern("edge"), std::move(edge)},
                                                  Parameter{model.intern("act"), act},
                                                  Parameter{model.intern("prev"), prev},
                                                  Parameter{model.intern("prevInit"), std::string("1'h0")}};
            const auto op = model.addOperation("core.event.edgeDet", std::array{event}, {}, {}, params);
            if (phase != SimPhase::None) model.setOperationPhase(op, phase);
            return op;
        };
        const auto verify = [](GrhSimModel &model) {
            diag::Diagnostics diagnostics;
            return verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics);
        };
        // Positive: deduplicated (event, edge) clusters — two regWrites share
        // (clk, posedge) while rst carries both edges — plus one op of every
        // phase-constrained kind; the model verifies and round-trips.
        {
            GrhSimModel model("edge_det_phase"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto word = model.logicType(8, false, LogicDomain::TwoState);
            const auto memType = model.arrayType(bit, 16);
            const auto clk = inputBit(model, "clk");
            const auto rst = inputBit(model, "rst");
            edgeDet(model, clk, "posedge", 0, 0);
            edgeDet(model, rst, "posedge", 1, 1);
            edgeDet(model, rst, "negedge", 2, 2);
            const auto constant = [&](TypeId type, std::string literal) {
                const auto value = model.addValue(type);
                const std::array params{Parameter{model.intern("constValue"), std::move(literal)}};
                model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
                return value;
            };
            const auto one = constant(bit, "1'h1");
            const auto next = constant(word, "8'h00");
            const auto mask = constant(word, "8'hff");
            const auto row = constant(memType, "16'h0000");
            const auto state = [&](const char *name, TypeId type) {
                const auto id = model.addState(name, type);
                const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
                const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
                model.addInit(id, steps, initParams);
                return id;
            };
            const auto q = state("q", word);
            const auto lq = state("lq", word);
            const auto mem = state("mem", memType);
            const auto memFill = state("mem_fill", memType);
            const auto memAssign = state("mem_assign", memType);
            const auto memSeq = state("mem_seq", memType);
            const auto edges = [&] {
                return std::array{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
            };
            model.setOperationPhase(model.addOperation("core.state.regWrite",
                std::array{one, next, mask, clk}, {}, std::array{ObjectRef::state(q)}, edges()),
                SimPhase::General);
            model.setOperationPhase(model.addOperation("core.state.latchWrite",
                std::array{one, next, mask}, {}, std::array{ObjectRef::state(lq)}), SimPhase::General);
            model.setOperationPhase(model.addOperation("core.state.memWrite",
                std::array{one, one, one, one, clk}, {}, std::array{ObjectRef::state(mem)}, edges()),
                SimPhase::Mem);
            model.setOperationPhase(model.addOperation("core.state.memFill",
                std::array{one, one, clk}, {}, std::array{ObjectRef::state(memFill)}, edges()),
                SimPhase::Mem);
            model.setOperationPhase(model.addOperation("core.state.memAssign",
                std::array{one, row, clk}, {}, std::array{ObjectRef::state(memAssign)}, edges()),
                SimPhase::Mem);
            model.setOperationPhase(model.addOperation("core.state.memWriteSeq",
                std::array{one, one, one, clk}, {}, std::array{ObjectRef::state(memSeq)}, edges()),
                SimPhase::Mem);
            const auto read = model.addValue(word);
            // The output cone is self-contained (M2b verifier): the read
            // feeding an Output-phase output.write must be Output itself.
            model.setOperationPhase(model.addOperation("core.state.read", {}, std::array{read},
                                                       std::array{ObjectRef::state(q)}),
                                    SimPhase::Output);
            const auto out = model.addOutput("o", word);
            model.setOperationPhase(model.addOperation("core.output.write",
                std::array{read}, {}, std::array{ObjectRef::output(out)}), SimPhase::Output);
            if (!verify(model)) return fail("edgeDet/phase fixture was rejected");
            std::filesystem::create_directories(artifactDir);
            const auto firstPath = artifactDir / "grhsim_edge_det.json";
            const auto secondPath = artifactDir / "grhsim_edge_det_roundtrip.json";
            diag::Diagnostics storeDiagnostics;
            if (!storeGrhSimModel(model, firstPath, defaultDialectRegistry(), storeDiagnostics))
                return fail("edgeDet GrhSIM JSON store failed");
            const auto bytes = readFile(firstPath);
            if (bytes.find("\"event\"") == std::string::npos ||
                bytes.find("\"general\"") == std::string::npos ||
                bytes.find("\"mem\"") == std::string::npos ||
                bytes.find("\"output\"") == std::string::npos)
                return fail("serialized operations lost the phase token");
            diag::Diagnostics loadDiagnostics;
            auto loaded = loadGrhSimModel(firstPath, defaultDialectRegistry(), loadDiagnostics);
            if (!loaded || loadDiagnostics.hasError()) return fail("edgeDet GrhSIM JSON load failed");
            unsigned dets = 0;
            for (const auto &op : loaded->operations())
            {
                if (loaded->text(op.opType) == "core.event.edgeDet") {
                    ++dets;
                    if (op.phase != SimPhase::Event) return fail("loaded edgeDet lost its phase");
                }
                if (loaded->text(op.opType) == "core.output.write" && op.phase != SimPhase::Output)
                    return fail("loaded output.write lost its phase");
            }
            if (dets != 3) return fail("loaded model lost an edgeDet op");
            diag::Diagnostics secondStoreDiagnostics;
            if (!storeGrhSimModel(*loaded, secondPath, defaultDialectRegistry(), secondStoreDiagnostics))
                return fail("edgeDet round-trip store failed");
            if (readFile(firstPath) != readFile(secondPath))
                return fail("edgeDet store/load/store did not produce stable bytes");
        }
        // Signature and clustering defects.
        {
            GrhSimModel model("edge_det_two_operands"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            const auto rst = inputBit(model, "rst");
            const std::array<Parameter, 4> params{Parameter{model.intern("edge"), std::string("posedge")},
                                                  Parameter{model.intern("act"), int64_t{0}},
                                                  Parameter{model.intern("prev"), int64_t{0}},
                                                  Parameter{model.intern("prevInit"), std::string("1'h0")}};
            model.setOperationPhase(model.addOperation("core.event.edgeDet", std::array{clk, rst}, {}, {}, params),
                                    SimPhase::Event);
            if (verify(model)) return fail("two-operand edgeDet passed verification");
        }
        {
            GrhSimModel model("edge_det_missing_edge"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            const std::array<Parameter, 3> params{Parameter{model.intern("act"), int64_t{0}},
                                                  Parameter{model.intern("prev"), int64_t{0}},
                                                  Parameter{model.intern("prevInit"), std::string("1'h0")}};
            model.setOperationPhase(model.addOperation("core.event.edgeDet", std::array{clk}, {}, {}, params),
                                    SimPhase::Event);
            if (verify(model)) return fail("edgeDet without an edge parameter passed verification");
        }
        {
            GrhSimModel model("edge_det_bad_edge"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            edgeDet(model, clk, "rising", 0, 0);
            if (verify(model)) return fail("edgeDet with an illegal edge value passed verification");
        }
        {
            GrhSimModel model("edge_det_act_duplicate"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            const auto rst = inputBit(model, "rst");
            edgeDet(model, clk, "posedge", 0, 0);
            edgeDet(model, rst, "posedge", 0, 1);
            if (verify(model)) return fail("edgeDet with a duplicate act index passed verification");
        }
        {
            GrhSimModel model("edge_det_prev_duplicate"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            const auto rst = inputBit(model, "rst");
            edgeDet(model, clk, "posedge", 0, 0);
            edgeDet(model, rst, "posedge", 1, 0);
            if (verify(model)) return fail("edgeDet with a duplicate prev index passed verification");
        }
        {
            GrhSimModel model("edge_det_cluster_duplicate"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            edgeDet(model, clk, "posedge", 0, 0);
            edgeDet(model, clk, "posedge", 1, 1);
            if (verify(model)) return fail("edgeDet with a duplicated (event, edge) cluster passed verification");
        }
        // Phase attribution defects.
        {
            GrhSimModel model("edge_det_phase_none"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            edgeDet(model, clk, "posedge", 0, 0, SimPhase::None);
            if (verify(model)) return fail("edgeDet without an event phase passed verification");
        }
        {
            GrhSimModel model("edge_det_phase_general"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            edgeDet(model, clk, "posedge", 0, 0, SimPhase::General);
            if (verify(model)) return fail("edgeDet with a non-event phase passed verification");
        }
        {
            GrhSimModel model("output_write_phase_general"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto clk = inputBit(model, "clk");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto out = model.addOutput("o", bit);
            model.setOperationPhase(model.addOperation("core.output.write",
                std::array{clk}, {}, std::array{ObjectRef::output(out)}), SimPhase::General);
            if (verify(model)) return fail("output.write with a general phase passed verification");
        }
        return 0;
    }

    // Run the M5d-6 six-phase CPU mapping pipeline end to end and check that the
    // produced namedStores/eventActivation/memWritePlan shells survive a JSON
    // store/load/store round trip byte-identically.
    int runSimRefactorMappingShellTest(const std::filesystem::path &artifactDir) {
        using namespace grhsim;
        GrhSimModel model("sim_refactor_shells"); model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto word = model.logicType(8, false, LogicDomain::TwoState);
        const auto clkPort = model.addInput("clk", bit);
        const auto clk = model.addValue(bit, "clk");
        model.addOperation("core.input.read", {}, std::array{clk}, std::array{ObjectRef::input(clkPort)});
        const auto constant = [&](TypeId type, std::string literal) {
            const auto value = model.addValue(type);
            const std::array params{Parameter{model.intern("constValue"), std::move(literal)}};
            model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
            return value;
        };
        const auto one = constant(bit, "1'h1");
        const auto next = constant(word, "8'h00");
        const auto mask = constant(word, "8'hff");
        const auto state = [&](const char *name, TypeId type) {
            const auto id = model.addState(name, type);
            const std::array initParams{Parameter{model.intern("value"), std::string("0")}};
            const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
            model.addInit(id, steps, initParams);
            return id;
        };
        const auto q = state("q", word);
        const auto history = state("q_clk_history", bit);
        const std::array edges{Parameter{model.intern("event_edges"), std::vector<std::string>{"posedge"}}};
        model.addOperation("core.state.regWrite",
            std::array{one, next, mask, clk}, {},
            std::array{ObjectRef::state(q), ObjectRef::state(history)}, edges);
        const auto read = model.addValue(word);
        model.addOperation("core.state.read", {}, std::array{read}, std::array{ObjectRef::state(q)});
        const auto out = model.addOutput("o", word);
        model.addOperation("core.output.write", std::array{read}, {}, std::array{ObjectRef::output(out)});
        PassManager manager(defaultDialectRegistry()); std::string error;
        // extract-output-cones clones the output cone into the Output phase
        // (six-phase cones are self-contained); the rest is the M5d-6
        // attribution + C-segment mapping chain.
        for (const char *name : {"grhsim.extract-output-cones",
                                 "grhsim.split-phases", "grhsim.select-state-stores",
                                 "cpu.st.build-general-nodes", "cpu.st.merge-general-supernodes",
                                 "cpu.st.layout-named-stores", "cpu.st.build-event-activation-map",
                                 "cpu.st.build-mem-write-plan", "cpu.st.pack-general-functions",
                                 "cpu.st.build-phase-schedule", "cpu.st.plan-translation-units"})
            manager.addPass(defaultPassRegistry().create(name, {}, error));
        diag::Diagnostics diagnostics;
        if (!manager.run(model, diagnostics).success || diagnostics.hasError())
            return fail("shell fixture six-phase CPU mapping failed");
        const auto *mapping = model.cpuMapping();
        if (!mapping || mapping->stage != CpuMappingStage::TranslationUnits)
            return fail("six-phase pipeline did not reach the translation-units stage");
        if (!mapping->dataLayout || !mapping->dataLayout->namedStores || !mapping->schedule ||
            !mapping->translationUnits)
            return fail("six-phase pipeline left the mapping shells incomplete");
        if (!verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics))
            return fail("six-phase pipeline mapping was rejected");
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / "grhsim_sim_refactor_shells.json";
        const auto secondPath = artifactDir / "grhsim_sim_refactor_shells_roundtrip.json";
        diag::Diagnostics storeDiagnostics;
        if (!storeGrhSimModel(model, firstPath, defaultDialectRegistry(), storeDiagnostics))
            return fail("shell GrhSIM JSON store failed");
        diag::Diagnostics loadDiagnostics;
        auto loaded = loadGrhSimModel(firstPath, defaultDialectRegistry(), loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("shell GrhSIM JSON load failed");
        const auto *loadedMapping = loaded->cpuMapping();
        if (!loadedMapping || !loadedMapping->dataLayout || !loadedMapping->schedule ||
            !loadedMapping->translationUnits)
            return fail("shell GrhSIM JSON load lost the CPU mapping");
        if (loadedMapping->dataLayout->namedStores != model.cpuMapping()->dataLayout->namedStores ||
            loadedMapping->schedule->eventActivation != model.cpuMapping()->schedule->eventActivation ||
            loadedMapping->schedule->memWritePlan != model.cpuMapping()->schedule->memWritePlan ||
            loadedMapping->translationUnits != model.cpuMapping()->translationUnits)
            return fail("shell fields did not survive the JSON round trip");
        diag::Diagnostics secondStoreDiagnostics;
        if (!storeGrhSimModel(*loaded, secondPath, defaultDialectRegistry(), secondStoreDiagnostics))
            return fail("shell round-trip store failed");
        if (readFile(firstPath) != readFile(secondPath))
            return fail("shell store/load/store did not produce stable bytes");
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
        if (const int status = runConcatSliceFoldTest(); status != 0) return status;
        if (const int status = runBitwisePredicatesTest(); status != 0) return status;
        if (const int status = runBitwiseMuxGuardsTest(); status != 0) return status;
        if (const int status = runMuxChainFoldTest(); status != 0) return status;
        if (const int status = runCanonicalizePhaseBarrierTest(); status != 0) return status;
        if (const int status = runUsedBitsTest(); status != 0) return status;
        if (const int status = runDeclaredSymbolMetadataTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR); status != 0)
            return status;
        if (const int status = runDeclProvenanceTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR); status != 0)
            return status;
        if (const int status = runEdgeDetPhaseTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR); status != 0) return status;
        if (const int status = runSimRefactorMappingShellTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR); status != 0)
            return status;
        return runHierarchyRejectionTest();
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
