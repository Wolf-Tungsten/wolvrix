// Directed tests for grhsim.const-fold and the unified grhsim.simplify
// (M5d-2): whole/phase scope, fixpoint termination, change summary,
// cross-phase shared states, DPI/side-effect preservation and declaration
// provenance maintenance.

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
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using namespace wolvrix::lib;

    int fail(const std::string &message)
    {
        std::cerr << "[grhsim-simplify] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
            throw std::runtime_error("pass failed: " + std::string(name));
        return messages;
    }

    bool verifies(grhsim::GrhSimModel &model)
    {
        diag::Diagnostics diagnostics;
        const bool ok = grhsim::verifyGrhSimModel(model, grhsim::defaultDialectRegistry(), diagnostics) &&
                        !diagnostics.hasError();
        if (!ok)
            for (const auto &message : diagnostics.messages())
                std::cerr << "[verify] " << message.context << ": " << message.message << '\n';
        return ok;
    }

    int roundTrip(grhsim::GrhSimModel &model, const std::filesystem::path &artifactDir,
                  const std::string &stem)
    {
        std::filesystem::create_directories(artifactDir);
        const auto firstPath = artifactDir / (stem + ".json");
        const auto secondPath = artifactDir / (stem + "_roundtrip.json");
        diag::Diagnostics storeDiagnostics;
        if (!grhsim::storeGrhSimModel(model, firstPath, grhsim::defaultDialectRegistry(),
                                      storeDiagnostics))
            return fail("store failed for " + stem);
        diag::Diagnostics loadDiagnostics;
        auto loaded = grhsim::loadGrhSimModel(firstPath, grhsim::defaultDialectRegistry(),
                                              loadDiagnostics);
        if (!loaded || loadDiagnostics.hasError()) return fail("load failed for " + stem);
        diag::Diagnostics secondStoreDiagnostics;
        if (!grhsim::storeGrhSimModel(*loaded, secondPath, grhsim::defaultDialectRegistry(),
                                      secondStoreDiagnostics))
            return fail("round-trip store failed for " + stem);
        if (readFile(firstPath) != readFile(secondPath))
            return fail("store/load/store is not byte stable for " + stem);
        return 0;
    }

    std::size_t countOps(const grhsim::GrhSimModel &model, std::string_view opType,
                         std::optional<grhsim::SimPhase> phase = std::nullopt)
    {
        std::size_t count = 0;
        for (const auto &op : model.operations())
        {
            if (model.text(op.opType) != opType) continue;
            if (phase && op.phase != *phase) continue;
            ++count;
        }
        return count;
    }

    const grhsim::SimOp *producerOf(const grhsim::GrhSimModel &model, grhsim::ValueId value)
    {
        for (const auto &op : model.operations())
            for (auto result : model.results(op))
                if (result == value) return &op;
        return nullptr;
    }

    // Numeric comparison through slang: parse both literals, resize to the
    // value's width, flatten, compare exactly.
    bool constantEquals(const grhsim::GrhSimModel &model, grhsim::ValueId value,
                        const std::string &expectedLiteral)
    {
        const auto *op = producerOf(model, value);
        if (!op || model.text(op->opType) != "core.compute.constant") return false;
        const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
        std::optional<std::string> actual;
        for (const auto &parameter : model.parameters(*op))
            if (model.text(parameter.name) == "constValue")
                actual = std::get<std::string>(parameter.value);
        if (!actual) return false;
        try
        {
            // Compare raw bit patterns: force both sides unsigned before the
            // resize so sign flags cannot distort the comparison.
            auto expected = slang::SVInt::fromString(expectedLiteral);
            expected.setSigned(false);
            expected = expected.resize(type.width);
            expected.flattenUnknowns();
            auto actualBits = slang::SVInt::fromString(*actual);
            actualBits.setSigned(false);
            actualBits = actualBits.resize(type.width);
            actualBits.flattenUnknowns();
            return exactlyEqual(expected, actualBits);
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    const grhsim::DeclProvenance *findProvenance(const grhsim::GrhSimModel &model,
                                                 std::string_view symbol)
    {
        for (const auto &record : model.declProvenances())
            if (model.text(record.symbol) == symbol) return &record;
        return nullptr;
    }

    bool messagesContain(const Messages &messages, const std::string &needle)
    {
        for (const auto &[context, message] : messages)
        {
            (void)context;
            if (message.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    grhsim::ValueId addConstant(grhsim::GrhSimModel &model, grhsim::TypeId type,
                                const std::string &literal, grhsim::SimPhase phase = grhsim::SimPhase::None,
                                std::string_view name = {})
    {
        const auto value = model.addValue(type, name);
        const std::array params{grhsim::Parameter{model.intern("constValue"), literal}};
        const auto op = model.addOperation("core.compute.constant", {}, std::array{value}, {}, params);
        if (phase != grhsim::SimPhase::None) model.setOperationPhase(op, phase);
        return value;
    }

    grhsim::ValueId addInputRead(grhsim::GrhSimModel &model, std::string_view name,
                                 grhsim::TypeId type, grhsim::SimPhase phase = grhsim::SimPhase::None)
    {
        const auto port = model.addInput(name, type);
        const auto value = model.addValue(type, name);
        const auto op = model.addOperation("core.input.read", {}, std::array{value},
                                           std::array{grhsim::ObjectRef::input(port)});
        if (phase != grhsim::SimPhase::None) model.setOperationPhase(op, phase);
        return value;
    }

    void addOutputWrite(grhsim::GrhSimModel &model, std::string_view name, grhsim::ValueId value,
                        grhsim::SimPhase phase = grhsim::SimPhase::None)
    {
        const auto port = model.addOutput(name, model.values()[value.index - 1].type);
        const auto op = model.addOperation("core.output.write", std::array{value}, {},
                                           std::array{grhsim::ObjectRef::output(port)});
        if (phase != grhsim::SimPhase::None) model.setOperationPhase(op, phase);
    }

    grhsim::StateId addStateWithInit(grhsim::GrhSimModel &model, std::string_view name,
                                     grhsim::TypeId type, const std::string &initLiteral)
    {
        const auto state = model.addState(name, type);
        const std::array params{grhsim::Parameter{model.intern("value"), initLiteral}};
        const std::array steps{grhsim::InitStep{model.intern("core.init.const"), {0, 1}}};
        model.addInit(state, steps, params);
        return state;
    }

    void declareValue(grhsim::GrhSimModel &model, std::string_view symbol, grhsim::ValueId value,
                      uint64_t width)
    {
        const auto id = model.strings().lookup(symbol);
        if (!id.valid()) throw std::runtime_error("declareValue: unknown symbol");
        model.addDeclaredSymbol(id);
        grhsim::DeclProvenance record;
        record.symbol = id;
        record.width = width;
        record.slices.push_back(grhsim::DeclProvenanceSlice{
            grhsim::DeclProvenanceKind::Direct, grhsim::DeclProvenanceTarget::Value,
            value.index, 0, 0, width});
        model.upsertDeclProvenance(std::move(record));
    }

    void declareState(grhsim::GrhSimModel &model, std::string_view symbol, grhsim::StateId state,
                      uint64_t width)
    {
        const auto id = model.strings().lookup(symbol);
        if (!id.valid()) throw std::runtime_error("declareState: unknown symbol");
        model.addDeclaredSymbol(id);
        grhsim::DeclProvenance record;
        record.symbol = id;
        record.width = width;
        record.slices.push_back(grhsim::DeclProvenanceSlice{
            grhsim::DeclProvenanceKind::Direct, grhsim::DeclProvenanceTarget::State,
            state.index, 0, 0, width});
        model.upsertDeclProvenance(std::move(record));
    }

    uint32_t typeWidth(const grhsim::GrhSimModel &model, grhsim::TypeId type)
    {
        return model.types()[type.index - 1].width;
    }

    int runConstFoldTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("const_fold");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto nibble = model.logicType(4, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto sbyte = model.logicType(8, true, LogicDomain::TwoState);
        const auto word = model.logicType(16, false, LogicDomain::TwoState);
        const auto wide = model.logicType(128, false, LogicDomain::TwoState);
        const auto byte4s = model.logicType(8, false, LogicDomain::FourState);

        const auto cA = addConstant(model, byte, "8'h10");
        const auto cB = addConstant(model, byte, "8'h04");
        const auto cZero = addConstant(model, byte, "8'h00");
        const auto cOne1 = addConstant(model, bit, "1'h1");
        const auto c1 = addConstant(model, byte, "8'h01");
        const auto c2 = addConstant(model, byte, "8'h02");
        const auto cNeg = addConstant(model, sbyte, "-8'sd16");
        const auto c2s = addConstant(model, sbyte, "8'sd2");
        const auto cWideMax = addConstant(model, wide,
            "128'hffffffffffffffffffffffffffffffff");
        const auto cWideOne = addConstant(model, wide, "128'h1");
        const auto c4s = addConstant(model, byte4s, "8'h10");

        const auto binary = [&](std::string_view op, ValueId a, ValueId b, TypeId result) {
            const auto value = model.addValue(result);
            model.addOperation(std::string(op), std::array{a, b}, std::array{value});
            return value;
        };
        const auto unary = [&](std::string_view op, ValueId a, TypeId result) {
            const auto value = model.addValue(result);
            model.addOperation(std::string(op), std::array{a}, std::array{value});
            return value;
        };

        const auto vAdd = binary("core.compute.add", cA, cB, byte);
        const auto vSub = binary("core.compute.sub", cA, cB, byte);
        const auto vMul = binary("core.compute.mul", cA, cB, byte);
        const auto vXor = binary("core.compute.xor", cA, cB, byte);
        const auto vAnd = binary("core.compute.and", cA, cB, byte);
        const auto vOr = binary("core.compute.or", cA, cB, byte);
        const auto vMod = binary("core.compute.mod", cA, cB, byte);
        const auto vLt = binary("core.compute.lt", cA, cB, bit);
        const auto vGe = binary("core.compute.ge", cA, cB, bit);
        const auto vEq = binary("core.compute.eq", cA, cB, bit);
        const auto vLogicAnd = binary("core.compute.logicAnd", cA, cB, bit);
        const auto vShl = binary("core.compute.shl", cA, c1, byte);
        const auto vLshr = binary("core.compute.lshr", cA, c2, byte);
        const auto vAshr = binary("core.compute.ashr", cNeg, c2s, sbyte);
        const auto vNot = unary("core.compute.not", cA, byte);
        const auto vLogicNot = unary("core.compute.logicNot", cA, bit);
        const auto vRedAnd = unary("core.compute.reduceAnd", cA, bit);
        const auto vRedOr = unary("core.compute.reduceOr", cA, bit);
        const auto vRedXor = unary("core.compute.reduceXor", cA, bit);
        const auto vMux = model.addValue(byte);
        model.addOperation("core.compute.mux", std::array{cOne1, cA, cB}, std::array{vMux});
        const auto vCat = model.addValue(word);
        model.addOperation("core.compute.concat", std::array{cA, cB}, std::array{vCat});
        const auto vSlice = model.addValue(nibble);
        {
            const std::array params{Parameter{model.intern("sliceStart"), int64_t{2}},
                                    Parameter{model.intern("sliceEnd"), int64_t{5}}};
            model.addOperation("core.compute.sliceStatic", std::array{cA}, std::array{vSlice}, {}, params);
        }
        const auto vRep = model.addValue(nibble);
        {
            const std::array params{Parameter{model.intern("rep"), int64_t{4}}};
            model.addOperation("core.compute.replicate", std::array{cOne1}, std::array{vRep}, {}, params);
        }
        const auto vWide = binary("core.compute.add", cWideMax, cWideOne, wide);
        const auto vDivZero = binary("core.compute.div", cA, cZero, byte);
        // Second-order fold: the add of a folded value must fold in one run.
        const auto vChain = binary("core.compute.add", vAdd, cB, byte);
        // Four-state values never fold.
        const auto v4s = binary("core.compute.add", c4s, c4s, byte4s);

        addOutputWrite(model, "o", vAdd);
        if (!verifies(model)) return fail("const-fold fixture rejected");

        const Messages messages = runPass(model, "grhsim.const-fold");
        if (!messagesContain(messages, "const_fold_ops=")) return fail("const-fold reported nothing");
        if (!verifies(model)) return fail("const-fold result rejected");

        const auto expect = [&](ValueId value, const std::string &literal, const char *what) {
            if (!constantEquals(model, value, literal))
                return fail(std::string("const-fold wrong value for ") + what);
            return 0;
        };
        if (const int s = expect(vAdd, "8'h14", "add")) return s;
        if (const int s = expect(vSub, "8'h0c", "sub")) return s;
        if (const int s = expect(vMul, "8'h40", "mul")) return s;
        if (const int s = expect(vXor, "8'h14", "xor")) return s;
        if (const int s = expect(vAnd, "8'h00", "and")) return s;
        if (const int s = expect(vOr, "8'h14", "or")) return s;
        if (const int s = expect(vMod, "8'h00", "mod")) return s;
        if (const int s = expect(vLt, "1'h0", "lt")) return s;
        if (const int s = expect(vGe, "1'h1", "ge")) return s;
        if (const int s = expect(vEq, "1'h0", "eq")) return s;
        if (const int s = expect(vLogicAnd, "1'h1", "logicAnd")) return s;
        if (const int s = expect(vShl, "8'h20", "shl")) return s;
        if (const int s = expect(vLshr, "8'h04", "lshr")) return s;
        if (const int s = expect(vAshr, "8'hfc", "ashr")) return s;
        if (const int s = expect(vNot, "8'hef", "not")) return s;
        if (const int s = expect(vLogicNot, "1'h0", "logicNot")) return s;
        if (const int s = expect(vRedAnd, "1'h0", "reduceAnd")) return s;
        if (const int s = expect(vRedOr, "1'h1", "reduceOr")) return s;
        if (const int s = expect(vRedXor, "1'h1", "reduceXor")) return s;
        if (const int s = expect(vMux, "8'h10", "mux")) return s;
        if (const int s = expect(vCat, "16'h1004", "concat")) return s;
        if (const int s = expect(vSlice, "4'h4", "sliceStatic")) return s;
        if (const int s = expect(vRep, "4'hf", "replicate")) return s;
        if (const int s = expect(vWide, "128'h0", "wide add wrap")) return s;
        if (const int s = expect(vChain, "8'h18", "second-order fold")) return s;
        // Division by a constant zero stays unfolded.
        if (constantEquals(model, vDivZero, "8'h00") ||
            countOps(model, "core.compute.div") != 1)
            return fail("const-fold folded a division by zero");
        // Four-state arithmetic stays unfolded; every two-state add folded.
        if (countOps(model, "core.compute.add") != 1)
            return fail("const-fold left a two-state add behind");
        const auto *op4s = producerOf(model, v4s);
        if (!op4s || model.text(op4s->opType) != "core.compute.add")
            return fail("const-fold folded a four-state op");
        return roundTrip(model, artifactDir, "grhsim_const_fold");
    }

    // Whole-scope simplify over a directed model: constant folding, assign
    // aliases, CSE with a declared duplicate, mux-chain folding, narrowing,
    // dead-cone elimination, equivalent-state merging and side-effect
    // preservation, with declaration provenance checked after every rewrite.
    int runSimplifyWholeTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        GrhSimModel model("simplify_whole");
        model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
        const auto bit = model.logicType(1, false, LogicDomain::TwoState);
        const auto byte = model.logicType(8, false, LogicDomain::TwoState);
        const auto word = model.logicType(32, false, LogicDomain::TwoState);

        const auto sel = addInputRead(model, "sel", bit);
        declareValue(model, "sel", sel, 1);
        const auto inA = addInputRead(model, "inA", byte);
        const auto inB = addInputRead(model, "inB", byte);
        const auto inWide = addInputRead(model, "inWide", word);
        const auto c0 = addInputRead(model, "c0", bit);
        const auto c1 = addInputRead(model, "c1", bit);
        const auto c2 = addInputRead(model, "c2", bit);
        const auto c3 = addInputRead(model, "c3", bit);
        const auto k10 = addConstant(model, byte, "8'h10");
        const auto k04 = addConstant(model, byte, "8'h04");
        const auto kZero = addConstant(model, byte, "8'h00");
        const auto kMask = addConstant(model, word, "32'h00ff00ff");
        const auto kZero32 = addConstant(model, word, "32'h0");
        const auto kFF32 = addConstant(model, word, "32'hffffffff");

        // wireW = assign(add(0x10, 0x04)): folds to a constant, then the
        // assign aliases away; the declared slice must redirect (Alias).
        const auto addT = model.addValue(byte, "addT");
        model.addOperation("core.compute.add", std::array{k10, k04}, std::array{addT});
        const auto wireW = model.addValue(byte, "wireW");
        model.addOperation("core.compute.assign", std::array{addT}, std::array{wireW});
        declareValue(model, "wireW", wireW, 8);
        addOutputWrite(model, "oW", wireW);

        // CSE: dup2 is declared and must alias onto dup1.
        const auto dup1 = model.addValue(byte, "dup1");
        model.addOperation("core.compute.xor", std::array{inA, inB}, std::array{dup1});
        addOutputWrite(model, "oDup1", dup1);
        const auto dup2 = model.addValue(byte, "dup2");
        model.addOperation("core.compute.xor", std::array{inA, inB}, std::array{dup2});
        declareValue(model, "dup2", dup2, 8);
        addOutputWrite(model, "oDup2", dup2);

        // Priority mux chain (4 links) -> prioritySelect.
        const auto m3 = model.addValue(byte, "m3");
        model.addOperation("core.compute.mux", std::array{c3, k04, kZero}, std::array{m3});
        const auto m2 = model.addValue(byte, "m2");
        model.addOperation("core.compute.mux", std::array{c2, k10, m3}, std::array{m2});
        const auto m1 = model.addValue(byte, "m1");
        model.addOperation("core.compute.mux", std::array{c1, inB, m2}, std::array{m1});
        const auto m0 = model.addValue(byte, "m0");
        model.addOperation("core.compute.mux", std::array{c0, inA, m1}, std::array{m0});
        addOutputWrite(model, "oMux", m0);

        // Narrowing: only the low 8 bits of narrowV are observable.
        const auto narrowV = model.addValue(word, "narrowV");
        model.addOperation("core.compute.and", std::array{inWide, kMask}, std::array{narrowV});
        declareValue(model, "narrowV", narrowV, 32);
        const auto narrowSlice = model.addValue(byte, "narrowSlice");
        {
            const std::array params{Parameter{model.intern("sliceStart"), int64_t{0}},
                                    Parameter{model.intern("sliceEnd"), int64_t{7}}};
            model.addOperation("core.compute.sliceStatic", std::array{narrowV}, std::array{narrowSlice},
                               {}, params);
        }
        addOutputWrite(model, "oNarrow", narrowSlice);

        // Dead cone: a folded constant feeding nothing, and a declared state
        // with a write but no readers.
        const auto deadV = model.addValue(byte, "deadV");
        model.addOperation("core.compute.add", std::array{k10, k04}, std::array{deadV});
        const auto deadQ = addStateWithInit(model, "deadQ", byte, "8'h00");
        declareState(model, "deadQ", deadQ, 8);
        model.addOperation("core.state.regWrite", std::array{sel, k04, addConstant(model, byte, "8'hff")},
                           {}, std::array{ObjectRef::state(deadQ)});

        // Equivalent declared states shareA/shareB merge (Merged provenance).
        const auto shareA = addStateWithInit(model, "shareA", byte, "8'h2a");
        declareState(model, "shareA", shareA, 8);
        const auto shareB = addStateWithInit(model, "shareB", byte, "8'h2a");
        declareState(model, "shareB", shareB, 8);
        const auto kFF = addConstant(model, byte, "8'hff");
        const auto k77 = addConstant(model, byte, "8'h77");
        for (const auto state : {shareA, shareB})
            model.addOperation("core.state.regWrite", std::array{sel, k77, kFF}, {},
                               std::array{ObjectRef::state(state)});
        const auto shareReadA = model.addValue(byte, "shareReadA");
        model.addOperation("core.state.read", {}, std::array{shareReadA},
                           std::array{ObjectRef::state(shareA)});
        addOutputWrite(model, "oShareA", shareReadA);
        const auto shareReadB = model.addValue(byte, "shareReadB");
        model.addOperation("core.state.read", {}, std::array{shareReadB},
                           std::array{ObjectRef::state(shareB)});
        addOutputWrite(model, "oShareB", shareReadB);

        // Side-effect roots: DPI call, system task, random system function.
        const auto dpiFn = model.addExternFunction("dpi_inc", "core.dpi", "dpi_inc", {}, byte);
        const auto dpiRet = model.addValue(byte, "dpiRet");
        model.addOperation("core.dpi.call", std::array{inA}, std::array{dpiRet},
                           std::array{ObjectRef::function(dpiFn)});
        addOutputWrite(model, "oDpi", dpiRet);
        {
            const std::array params{Parameter{model.intern("name"), std::string("display")}};
            model.addOperation("core.system.task", std::array{sel, inB}, {}, {}, params);
        }
        const auto randV = model.addValue(word, "randV");
        {
            const std::array params{Parameter{model.intern("name"), std::string("urandom")}};
            model.addOperation("core.system.function", {}, std::array{randV}, {}, params);
        }
        addOutputWrite(model, "oRand", randV);

        if (!verifies(model)) return fail("simplify whole fixture rejected");
        const Messages messages = runPass(model, "grhsim.simplify");
        if (!messagesContain(messages, "simplify_scope=whole"))
            return fail("simplify summary header missing");
        if (!messagesContain(messages, "simplify_converged=1"))
            return fail("simplify did not converge");
        if (!verifies(model)) return fail("simplify whole result rejected");

        // Constant folding + alias: wireW's declared slice redirects (Alias)
        // to the surviving constant value.
        const DeclProvenance *wireProv = findProvenance(model, "wireW");
        if (!wireProv || wireProv->slices.size() != 1 ||
            wireProv->slices[0].kind != DeclProvenanceKind::Alias ||
            wireProv->slices[0].target != DeclProvenanceTarget::Value ||
            wireProv->slices[0].width != 8)
            return fail("wireW provenance was not redirected as an alias");
        {
            const auto &target = model.values()[wireProv->slices[0].targetIndex - 1];
            const auto *op = producerOf(model, target.id);
            if (!op || model.text(op->opType) != "core.compute.constant" ||
                !constantEquals(model, target.id, "8'h14"))
                return fail("wireW alias target is not the folded constant");
        }
        // CSE: dup2's slice aliases dup1's surviving value.
        const DeclProvenance *dupProv = findProvenance(model, "dup2");
        if (!dupProv || dupProv->slices.size() != 1 ||
            dupProv->slices[0].kind != DeclProvenanceKind::Alias)
            return fail("dup2 provenance was not aliased");
        if (model.text(model.values()[dupProv->slices[0].targetIndex - 1].name) != "dup1")
            return fail("dup2 did not alias the dup1 value");
        if (countOps(model, "core.compute.xor") != 1)
            return fail("CSE did not eliminate the duplicate xor");
        // Mux chain folded to one prioritySelect; inner links are gone.
        if (countOps(model, "core.compute.mux") != 0 ||
            countOps(model, "core.compute.prioritySelect") != 1)
            return fail("mux chain was not folded to prioritySelect");
        // Narrowing: narrowV's slice clamps to the surviving 8-bit prefix.
        const DeclProvenance *narrowProv = findProvenance(model, "narrowV");
        if (!narrowProv || narrowProv->slices.size() != 1 ||
            narrowProv->slices[0].kind != DeclProvenanceKind::Direct ||
            narrowProv->slices[0].width != 8)
            return fail("narrowV provenance was not clamped to the narrowed prefix");
        if (typeWidth(model, model.values()[narrowProv->slices[0].targetIndex - 1].type) != 8)
            return fail("narrowV slice target is not the narrowed value");
        // Dead cone: deadV's producer is gone (post-compact ids shift, so
        // check by name) and deadQ's record is an empty anchor (declaration
        // known, currently unrealized).
        for (const auto &value : model.values())
            if (model.text(value.name) == "deadV")
                return fail("dead cone value survived");
        const DeclProvenance *deadProv = findProvenance(model, "deadQ");
        if (!deadProv || !deadProv->slices.empty())
            return fail("deadQ provenance did not degrade to an empty anchor");
        for (const auto &state : model.states())
            if (model.text(state.name) == "deadQ")
                return fail("dead state was not removed");
        // Equivalent states merged: shareB's slice points at shareA's state.
        const DeclProvenance *shareAProv = findProvenance(model, "shareA");
        const DeclProvenance *shareBProv = findProvenance(model, "shareB");
        if (!shareAProv || shareAProv->slices.size() != 1 ||
            shareAProv->slices[0].kind != DeclProvenanceKind::Direct)
            return fail("shareA provenance lost its direct slice");
        if (!shareBProv || shareBProv->slices.size() != 1 ||
            shareBProv->slices[0].kind != DeclProvenanceKind::Merged ||
            shareBProv->slices[0].targetIndex != shareAProv->slices[0].targetIndex)
            return fail("shareB provenance did not merge into shareA");
        std::size_t sharedStates = 0;
        for (const auto &state : model.states())
            if (model.text(state.name) == "shareA" || model.text(state.name) == "shareB")
                ++sharedStates;
        if (sharedStates != 1) return fail("equivalent states were not merged");
        // Side-effect roots survive with their operands fully observed.
        if (countOps(model, "core.dpi.call") != 1 || countOps(model, "core.system.task") != 1 ||
            countOps(model, "core.system.function") != 1)
            return fail("simplify dropped a side-effect root");
        if (countOps(model, "core.output.write") != 9)
            return fail("simplify dropped an output write");
        // The declared input read is an opaque root and keeps its Direct slice.
        const DeclProvenance *selProv = findProvenance(model, "sel");
        if (!selProv || selProv->slices.size() != 1 ||
            selProv->slices[0].kind != DeclProvenanceKind::Direct)
            return fail("input declaration provenance was not preserved");
        return roundTrip(model, artifactDir, "grhsim_simplify_whole");
    }

    // Phase scope: per-partition simplification preserves partition
    // interfaces — a state shared between Event and General is never narrowed
    // or removed by a local demand, while a phase-local state narrows. The
    // whole-scope run on the same model narrows the shared state (union
    // demand), proving the restriction is scope-driven.
    int runSimplifyPhaseTest(const std::filesystem::path &artifactDir)
    {
        using namespace grhsim;
        const auto buildModel = [] {
            GrhSimModel model("simplify_phase");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto bit = model.logicType(1, false, LogicDomain::TwoState);
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto word = model.logicType(32, false, LogicDomain::TwoState);
            const auto nibble = model.logicType(4, false, LogicDomain::TwoState);

            const auto kOne = addConstant(model, bit, "1'h1");
            const auto kFF32 = addConstant(model, word, "32'hffffffff");
            int64_t actIndex = 0;
            const auto edgeDet = [&](ValueId eventValue) {
                // The legal Event-phase sink: an edge detector per act cluster.
                const std::array<Parameter, 4> params{
                    Parameter{model.intern("edge"), std::string("posedge")},
                    Parameter{model.intern("act"), actIndex},
                    Parameter{model.intern("prev"), actIndex},
                    Parameter{model.intern("prevInit"), std::string("1'h0")}};
                ++actIndex;
                model.setOperationPhase(
                    model.addOperation("core.event.edgeDet", std::array{eventValue}, {}, {}, params),
                    SimPhase::Event);
            };
            // Event cone: a live chain sinking into an edgeDet, plus a dead xor.
            const auto evV = addInputRead(model, "ev", bit, SimPhase::Event);
            const auto evKeep = model.addValue(bit, "evKeep");
            model.setOperationPhase(
                model.addOperation("core.compute.not", std::array{evV}, std::array{evKeep}),
                SimPhase::Event);
            edgeDet(evKeep);
            const auto evDead = model.addValue(bit, "evDead");
            model.setOperationPhase(
                model.addOperation("core.compute.xor", std::array{evV, evKeep}, std::array{evDead}),
                SimPhase::Event);
            // General cone: a live chain sinking into an (unphased) output,
            // plus a dead xor.
            const auto gA = addInputRead(model, "gA", bit);
            const auto gB = addInputRead(model, "gB", bit);
            const auto gKeep = model.addValue(bit, "gKeep");
            model.setOperationPhase(
                model.addOperation("core.compute.not", std::array{gA}, std::array{gKeep}),
                SimPhase::General);
            addOutputWrite(model, "oG", gKeep);
            const auto gDead = model.addValue(bit, "gDead");
            model.setOperationPhase(
                model.addOperation("core.compute.xor", std::array{gA, gB}, std::array{gDead}),
                SimPhase::General);
            // Shared state: written in General, read in Event and General,
            // only the low 8 bits observed anywhere.
            const auto inD = addInputRead(model, "inD", word);
            const auto sharedQ = addStateWithInit(model, "sharedQ", word, "32'h0");
            declareState(model, "sharedQ", sharedQ, 32);
            model.setOperationPhase(
                model.addOperation("core.state.regWrite", std::array{kOne, inD, kFF32}, {},
                                   std::array{ObjectRef::state(sharedQ)}),
                SimPhase::General);
            const auto sliceParams = [&](GrhSimModel &m, int64_t end) {
                return std::array{Parameter{m.intern("sliceStart"), int64_t{0}},
                                  Parameter{m.intern("sliceEnd"), end}};
            };
            const auto evRead = model.addValue(word, "evRead");
            model.setOperationPhase(
                model.addOperation("core.state.read", {}, std::array{evRead},
                                   std::array{ObjectRef::state(sharedQ)}),
                SimPhase::Event);
            const auto evSlice = model.addValue(byte, "evSlice");
            model.setOperationPhase(
                model.addOperation("core.compute.sliceStatic", std::array{evRead}, std::array{evSlice},
                                   {}, sliceParams(model, 7)),
                SimPhase::Event);
            const auto evReduce = model.addValue(bit, "evReduce");
            model.setOperationPhase(
                model.addOperation("core.compute.reduceOr", std::array{evSlice}, std::array{evReduce}),
                SimPhase::Event);
            edgeDet(evReduce);
            const auto gRead = model.addValue(word, "gRead");
            model.setOperationPhase(
                model.addOperation("core.state.read", {}, std::array{gRead},
                                   std::array{ObjectRef::state(sharedQ)}),
                SimPhase::General);
            const auto gSlice = model.addValue(byte, "gSlice");
            model.setOperationPhase(
                model.addOperation("core.compute.sliceStatic", std::array{gRead}, std::array{gSlice},
                                   {}, sliceParams(model, 7)),
                SimPhase::General);
            addOutputWrite(model, "oSharedG", gSlice);
            // Phase-local state: written and read only in General, low 4 bits
            // observed; a distinct write data input keeps it unmergeable.
            const auto inD2 = addInputRead(model, "inD2", word);
            const auto localQ = addStateWithInit(model, "localQ", word, "32'h1");
            declareState(model, "localQ", localQ, 4 * 8);
            model.setOperationPhase(
                model.addOperation("core.state.regWrite", std::array{kOne, inD2, kFF32}, {},
                                   std::array{ObjectRef::state(localQ)}),
                SimPhase::General);
            const auto lRead = model.addValue(word, "lRead");
            model.setOperationPhase(
                model.addOperation("core.state.read", {}, std::array{lRead},
                                   std::array{ObjectRef::state(localQ)}),
                SimPhase::General);
            const auto lSlice = model.addValue(nibble, "lSlice");
            model.setOperationPhase(
                model.addOperation("core.compute.sliceStatic", std::array{lRead}, std::array{lSlice},
                                   {}, sliceParams(model, 3)),
                SimPhase::General);
            addOutputWrite(model, "oLocal", lSlice);
            return model;
        };

        // Single-phase restriction: only the scoped partition is simplified.
        {
            auto model = buildModel();
            if (!verifies(model)) return fail("phase fixture rejected");
            const Messages messages = runPass(model, "grhsim.simplify",
                                              std::array<std::string_view, 4>{"--scope", "phase",
                                                                              "--phase", "general"});
            if (!messagesContain(messages, "simplify_phases=general"))
                return fail("single-phase summary missing");
            if (!verifies(model)) return fail("single-phase simplify result rejected");
            if (countOps(model, "core.compute.xor", SimPhase::General) != 0)
                return fail("in-scope dead cone survived");
            if (countOps(model, "core.compute.xor", SimPhase::Event) != 1)
                return fail("out-of-scope dead cone was removed");
            if (countOps(model, "core.compute.xor") != 1)
                return fail("unexpected xor count after single-phase simplify");
        }
        // All phases: both dead cones go; the shared state keeps its width;
        // the phase-local state narrows; provenance stays valid throughout.
        {
            auto model = buildModel();
            const Messages messages = runPass(model, "grhsim.simplify",
                                              std::array<std::string_view, 2>{"--scope", "phase"});
            if (!messagesContain(messages, "simplify_converged=1"))
                return fail("phase-scope simplify did not converge");
            if (!verifies(model)) return fail("phase-scope simplify result rejected");
            if (countOps(model, "core.compute.xor") != 0)
                return fail("dead cones survived the all-phase simplify");
            const DeclProvenance *sharedProv = findProvenance(model, "sharedQ");
            if (!sharedProv || sharedProv->slices.size() != 1)
                return fail("shared state provenance lost its slice");
            const auto &sharedState = model.states()[sharedProv->slices[0].targetIndex - 1];
            if (typeWidth(model, sharedState.type) != 32)
                return fail("shared state was narrowed by a local demand");
            const DeclProvenance *localProv = findProvenance(model, "localQ");
            if (!localProv || localProv->slices.size() != 1 ||
                localProv->slices[0].kind != DeclProvenanceKind::Direct ||
                localProv->slices[0].width != 4)
                return fail("phase-local state provenance was not clamped");
            if (typeWidth(model, model.states()[localProv->slices[0].targetIndex - 1].type) != 4)
                return fail("phase-local state was not narrowed");
            if (const int status = roundTrip(model, artifactDir, "grhsim_simplify_phase"))
                return status;
        }
        // Whole scope on the same fixture narrows the shared state (the union
        // of both partitions' demands is 8 bits) and preserves the barrier.
        {
            auto model = buildModel();
            const Messages messages = runPass(model, "grhsim.simplify");
            if (!messagesContain(messages, "simplify_converged=1"))
                return fail("whole-scope simplify of the phase fixture did not converge");
            if (!verifies(model)) return fail("whole-scope simplify of the phase fixture rejected");
            const DeclProvenance *sharedProv = findProvenance(model, "sharedQ");
            if (!sharedProv || sharedProv->slices.size() != 1 ||
                sharedProv->slices[0].width != 8)
                return fail("whole-scope union demand did not narrow the shared state");
            if (typeWidth(model, model.states()[sharedProv->slices[0].targetIndex - 1].type) != 8)
                return fail("whole-scope shared state has the wrong narrowed width");
            if (const int status = roundTrip(model, artifactDir, "grhsim_simplify_phase_whole"))
                return status;
        }
        return 0;
    }

    int runSimplifyArgsTest()
    {
        using namespace grhsim;
        std::string error;
        if (defaultPassRegistry().create("grhsim.simplify",
                                         std::array<std::string_view, 2>{"--bogus", "1"}, error))
            return fail("simplify accepted an unknown option");
        if (defaultPassRegistry().create("grhsim.simplify",
                                         std::array<std::string_view, 2>{"--phase", "event"}, error))
            return fail("simplify accepted --phase without --scope phase");
        if (defaultPassRegistry().create("grhsim.simplify",
                                         std::array<std::string_view, 4>{"--scope", "phase",
                                                                         "--phase", "none"}, error))
            return fail("simplify accepted --phase none");
        if (defaultPassRegistry().create("grhsim.simplify",
                                         std::array<std::string_view, 2>{"--max-rounds", "0"}, error))
            return fail("simplify accepted --max-rounds 0");
        if (!defaultPassRegistry().create("grhsim.simplify",
                                          std::array<std::string_view, 4>{"--scope", "phase",
                                                                          "--phase", "mem"}, error))
            return fail("simplify rejected a valid scope/phase combination");
        if (defaultPassRegistry().create("grhsim.const-fold",
                                         std::array<std::string_view, 1>{"--bogus"}, error))
            return fail("const-fold accepted an argument");
        // Round cap: a changing model with --max-rounds 1 reports
        // non-convergence but still succeeds.
        {
            GrhSimModel model("simplify_cap");
            model.addDialect("core", "1", "wolvrix.grhsim.core.v1");
            const auto byte = model.logicType(8, false, LogicDomain::TwoState);
            const auto k10 = addConstant(model, byte, "8'h10");
            const auto wire = model.addValue(byte, "wire");
            model.addOperation("core.compute.assign", std::array{k10}, std::array{wire});
            addOutputWrite(model, "o", wire);
            const Messages messages = runPass(model, "grhsim.simplify",
                                              std::array<std::string_view, 2>{"--max-rounds", "1"});
            if (!messagesContain(messages, "simplify_converged=0"))
                return fail("round-cap run did not report non-convergence");
            if (!messagesContain(messages, "round cap"))
                return fail("round-cap run did not warn");
            if (!verifies(model)) return fail("round-cap result rejected");
        }
        return 0;
    }
    // A lowered whole-graph model: the unified simplify keeps every declared
    // symbol locatable (live slice or empty anchor) and the result verifies
    // and round-trips.
    int runSimplifyLoweredModelTest(const std::filesystem::path &artifactDir)
    {
        grh::Design design;
        auto &graph = design.createGraph("top");
        const auto clkSym = graph.internSymbol("clk");
        const auto in0Sym = graph.internSymbol("in0");
        const auto qSym = graph.internSymbol("q");
        const auto sumSym = graph.internSymbol("sum");
        const auto deadSym = graph.internSymbol("deadWire");

        const auto clk = graph.createValue(clkSym, 1, false);
        graph.bindInputPort("clk", clk);
        const auto in0 = graph.createValue(in0Sym, 8, false);
        graph.bindInputPort("in0", in0);
        const auto one = graph.createValue(graph.internSymbol("c_one"), 1, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("c_one_op"));
            graph.setAttr(op, "constValue", std::string("1'h1"));
            graph.addResult(op, one);
        }
        const auto kA = graph.createValue(graph.internSymbol("c_a"), 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("c_a_op"));
            graph.setAttr(op, "constValue", std::string("8'h10"));
            graph.addResult(op, kA);
        }
        const auto kB = graph.createValue(graph.internSymbol("c_b"), 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("c_b_op"));
            graph.setAttr(op, "constValue", std::string("8'h04"));
            graph.addResult(op, kB);
        }
        const auto kAdd = graph.createValue(graph.internSymbol("c_add"), 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kAdd,
                                                  graph.internSymbol("c_add_op"));
            graph.addOperand(op, kA);
            graph.addOperand(op, kB);
            graph.addResult(op, kAdd);
        }
        const auto sum = graph.createValue(sumSym, 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kAdd,
                                                  graph.internSymbol("sum_op"));
            graph.addOperand(op, in0);
            graph.addOperand(op, kAdd);
            graph.addResult(op, sum);
        }
        const auto mask = graph.createValue(graph.internSymbol("c_mask"), 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("c_mask_op"));
            graph.setAttr(op, "constValue", std::string("8'hff"));
            graph.addResult(op, mask);
        }
        {
            const auto reg = graph.createOperation(grh::OperationKind::kRegister, qSym);
            graph.setAttr(reg, "width", int64_t{8});
            graph.setAttr(reg, "isSigned", false);
            graph.setAttr(reg, "initValue", std::string("8'h01"));
            const auto read = graph.createOperation(grh::OperationKind::kRegisterReadPort,
                                                    graph.internSymbol("q_read"));
            graph.setAttr(read, "regSymbol", std::string("q"));
            const auto qValue = graph.createValue(graph.internSymbol("q_value"), 8, false);
            graph.addResult(read, qValue);
            const auto write = graph.createOperation(grh::OperationKind::kRegisterWritePort,
                                                     graph.internSymbol("q_write"));
            graph.setAttr(write, "regSymbol", std::string("q"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            graph.addOperand(write, one);
            graph.addOperand(write, sum);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            const auto y = graph.createValue(graph.internSymbol("y"), 8, false);
            const auto xorOp = graph.createOperation(grh::OperationKind::kXor,
                                                     graph.internSymbol("y_xor"));
            graph.addOperand(xorOp, qValue);
            graph.addOperand(xorOp, sum);
            graph.addResult(xorOp, y);
            graph.bindOutputPort("y", y);
        }
        // Declared but dead: folded away by the simplify, anchor remains.
        const auto dead = graph.createValue(deadSym, 8, false);
        {
            const auto op = graph.createOperation(grh::OperationKind::kConstant,
                                                  graph.internSymbol("dead_op"));
            graph.setAttr(op, "constValue", std::string("8'hab"));
            graph.addResult(op, dead);
        }
        for (const auto sym : {clkSym, in0Sym, qSym, sumSym, deadSym})
            graph.addDeclaredSymbol(sym);
        design.markAsTop("top");

        diag::Diagnostics lowerDiagnostics;
        grhsim::GrhToGrhSimOptions options;
        options.top = "top";
        options.logicDomain = grhsim::LogicDomain::TwoState;
        auto model = grhsim::lowerGrhToGrhSim(design, options, lowerDiagnostics);
        if (!model || lowerDiagnostics.hasError()) return fail("lowered-model lowering failed");
        if (model->declProvenances().empty()) return fail("lowered model has no provenance");
        const Messages messages = runPass(*model, "grhsim.simplify");
        if (!messagesContain(messages, "simplify_converged=1"))
            return fail("lowered-model simplify did not converge");
        if (!verifies(*model)) return fail("lowered-model simplify result rejected");
        for (const auto symbol : model->declaredSymbols())
            if (!model->findDeclProvenance(symbol))
                return fail("lowered-model simplify lost a provenance record");
        const grhsim::DeclProvenance *q = findProvenance(*model, "q");
        if (!q || q->slices.size() != 1 ||
            q->slices[0].target != grhsim::DeclProvenanceTarget::State ||
            q->slices[0].kind != grhsim::DeclProvenanceKind::Direct)
            return fail("register provenance did not survive whole simplify");
        const grhsim::DeclProvenance *deadProv = findProvenance(*model, "deadWire");
        if (!deadProv || !deadProv->slices.empty())
            return fail("dead declaration did not degrade to an empty anchor");
        const grhsim::DeclProvenance *sumProv = findProvenance(*model, "sum");
        if (!sumProv || sumProv->slices.empty())
            return fail("live wire provenance lost its slice");
        return roundTrip(*model, artifactDir, "grhsim_simplify_lowered");
    }
} // namespace

int main()
{
    try
    {
        if (const int status = runConstFoldTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR)) return status;
        if (const int status = runSimplifyWholeTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR)) return status;
        if (const int status = runSimplifyPhaseTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR)) return status;
        if (const int status = runSimplifyLoweredModelTest(WOLVRIX_GRHSIM_TEST_ARTIFACT_DIR)) return status;
        return runSimplifyArgsTest();
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("unexpected exception: ") + ex.what());
    }
}
