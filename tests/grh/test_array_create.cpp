#include "core/grh.hpp"
#include "core/ingest.hpp"
#include "core/load.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"
#include "emit/system_verilog.hpp"
#include "transform/verify.hpp"
#include "transform/grhsim/grh-to-grhsim.hpp"
#include "transform/grhsim/simplify.hpp"
#include "transform/const_fold.hpp"
#include "transform/redundant_elim.hpp"
#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace wolvrix::lib;
namespace sim = transform::grhsim;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

bool verifyGrh(grh::Design &design) {
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::GrhVerifyPass>());
    transform::PassDiagnostics diagnostics;
    return manager.run(design, diagnostics).success && !diagnostics.hasError();
}

void runPass(grh::Design &design, std::unique_ptr<transform::Pass> pass) {
    transform::PassManager manager;
    manager.addPass(std::move(pass));
    transform::PassDiagnostics diagnostics;
    require(manager.run(design, diagnostics).success && !diagnostics.hasError(), "native pass failed");
}

void contractsAndRoundTrips() {
    for (int32_t width : {1, 8, 24, 96}) {
        for (bool isSigned : {false, true}) {
            grh::Design design;
            auto &graph = design.createGraph("array_create");
            design.markAsTop(graph.symbol());
            std::vector<grh::ValueId> elements;
            for (int i = 0; i < 4; ++i) {
                auto value = graph.createValue(width, isSigned);
                graph.bindInputPort("e" + std::to_string(i), value);
                elements.push_back(value);
            }
            const auto array = graph.createArray(elements);
            const auto singleton = graph.createArray(std::array{elements[0]});
            const auto nested = graph.createArray(std::array{array, array});
            graph.bindOutputPort("array", array);
            graph.bindOutputPort("singleton", singleton);
            graph.bindOutputPort("nested", nested);
            require(graph.valueWidth(nested) == 8 * width, "GRH total width inference");
            require(std::ranges::equal(graph.valueShape(nested), std::array<int64_t, 2>{2, 4}),
                    "GRH nested shape inference");
            require(std::ranges::equal(graph.valueShape(singleton), std::array<int64_t, 1>{1}),
                    "GRH singleton dimension lost");
            require(verifyGrh(design), "GRH array contract rejected");
            store::StoreJson writer;
            const auto text = writer.storeToString(design);
            require(text.has_value(), "GRH store failed");
            load::LoadJson reader;
            auto loaded = reader.load(*text);
            require(verifyGrh(loaded), "GRH loaded contract rejected");
            require(writer.storeToString(loaded) == text, "GRH roundtrip changed array construction");
            for (auto domain : {grh::LogicDomain::TwoState, grh::LogicDomain::FourState}) {
                auto converted = loaded.clone();
                runPass(converted, std::make_unique<sim::GrhToGrhsimPass>(
                    sim::GrhToGrhsimOptions{"array_create", domain}));
                require(verifyGrh(converted), "converted arrayCreate rejected");
                auto &native = *converted.findGraph("array_create");
                unsigned count = 0;
                for (const auto id : native.operations()) {
                    const auto op = native.getOperation(id);
                    if (op.kind() != grh::OperationKind::kArrayCreate) continue;
                    ++count;
                    const auto type = native.valueLogicalType(op.results()[0]);
                    require(type.domain == domain && type.isSigned == isSigned, "conversion lost element type");
                    require(type.shape[0] == static_cast<int64_t>(op.operands().size()), "conversion lost outer shape");
                }
                require(count == 3, "conversion flattened arrayCreate");
                const auto json = writer.storeToString(converted);
                require(json.has_value(), "converted GRH store failed");
                auto roundTrip = reader.load(*json);
                require(verifyGrh(roundTrip), "converted GRH roundtrip failed");
                require(writer.storeToString(roundTrip) == json, "roundtrip changed geometry or operand order");
            }
        }
    }
    require(grh::parseOperationKind("kArrayCreate") == grh::OperationKind::kArrayCreate,
            "GRH operation name not registered");
}

void invalidModels() {
    // Bypass the inferred constructor to independently exercise the verifier.
    for (unsigned bad = 0; bad < 14; ++bad) {
        grh::Design design;
        auto &graph = design.createGraph("invalid_array");
        grh::LogicalType byte{grh::ValueType::Logic, 8, false, grh::LogicDomain::TwoState, {}};
        auto other = byte;
        if (bad == 2) other.width = 16;
        if (bad == 3) other.isSigned = true;
        if (bad == 4) other.domain = grh::LogicDomain::FourState;
        if (bad == 5) other.shape = {2};
        if (bad == 6) { other.kind = grh::ValueType::Real; other.width = 64; }
        const auto a = graph.createValue(byte), b = graph.createValue(other);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        const auto op = graph.createOperation(grh::OperationKind::kArrayCreate);
        if (bad != 0) { graph.addOperand(op, a); graph.addOperand(op, b); }
        grh::LogicalType resultType{grh::ValueType::Logic, bad == 7 ? 24 : 16, bad == 8,
            bad == 12 ? grh::LogicDomain::FourState : grh::LogicDomain::TwoState,
            bad == 9 ? std::vector<int64_t>{4} :
            bad == 10 ? std::vector<int64_t>{2, 1} : bad == 11 ? std::vector<int64_t>{} : std::vector<int64_t>{2}};
        graph.addResult(op, graph.createValue(resultType));
        if (bad == 1) graph.addResult(op, graph.createValue(resultType));
        if (bad == 13) graph.setAttr(op, "unexpected", int64_t{1});
        if (verifyGrh(design))
            throw std::runtime_error("invalid arrayCreate case " + std::to_string(bad) + " passed GRH verifier");
    }
    grh::Design design;
    auto &graph = design.createGraph("inference");
    const grh::LogicalType type{grh::ValueType::Logic, 24, true, grh::LogicDomain::FourState, {3}};
    const auto e = graph.createValue(type);
    graph.bindInputPort("e", e);
    const auto array = graph.createArray(std::array{e, e});
    const auto result = graph.valueLogicalType(array);
    require(result.width == 48 && result.shape == std::vector<int64_t>({2, 3}) &&
            result.isSigned && result.domain == grh::LogicDomain::FourState, "inference lost full type");
    require(verifyGrh(design), "inferred full type rejected");
    bool rejected = false;
    try { graph.createArray({}); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "empty inferred array accepted");
    const auto wide = graph.createValue(std::numeric_limits<int32_t>::max(), false);
    rejected = false;
    try { graph.createArray(std::array{wide, wide}); } catch (const std::overflow_error &) { rejected = true; }
    require(rejected, "GRH constructor overflow accepted");
}

grh::ValueId grhConstant(grh::Graph &graph, int32_t width, const std::string &literal) {
    const auto value = graph.createValue(width, false);
    const auto op = graph.createOperation(grh::OperationKind::kConstant);
    graph.addResult(op, value);
    graph.setAttr(op, "constValue", literal);
    return value;
}

slang::SVInt outputConstant(const grh::Graph &graph, std::string_view name) {
    auto value = graph.outputPortValue(name);
    for (unsigned depth = 0; depth < 32; ++depth) {
        const auto op = graph.getOperation(graph.valueDef(value));
        if (op.kind() == grh::OperationKind::kConstant)
            return slang::SVInt::fromString(std::get<std::string>(*op.attr("constValue")));
        require((op.kind() == grh::OperationKind::kAssign) &&
                op.operands().size() == 1, "output did not fold to a constant");
        value = op.operands()[0];
    }
    throw std::runtime_error("constant output alias cycle");
}

void grhFoldingAndSvRoundTrip() {
    grh::Design design;
    auto &graph = design.createGraph("array_sv_roundtrip");
    std::vector<grh::ValueId> elements;
    for (const char *literal : {"8'h11", "8'h22", "8'h33", "8'h44"})
        elements.push_back(grhConstant(graph, 8, literal));
    const auto array = graph.createArray(elements);
    graph.bindOutputPort("array", array);
    graph.bindOutputPort("singleton", graph.createArray(std::array{elements[0]}));
    graph.bindOutputPort("nested", graph.createArray(std::array{array, array}));
    design.markAsTop("array_sv_roundtrip");
    const std::filesystem::path root = WOLVRIX_ARRAY_CREATE_ARTIFACT_DIR;
    emit::EmitOptions options;
    options.outputDir = root.string();
    options.outputFilename = "array.sv";
    emit::EmitDiagnostics diagnostics;
    emit::EmitSystemVerilog emitter(&diagnostics);
    require(emitter.emit(design, options).success && !diagnostics.hasError(), "arrayCreate SV emit failed");

    slang::driver::Driver driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    driver.options.topModules.emplace_back("array_sv_roundtrip");
    const std::string path = (root / "array.sv").string();
    const char *argv[]{"array-create-test", path.c_str()};
    require(driver.parseCommandLine(2, argv) && driver.processOptions() && driver.parseAllSources(),
            "emitted array SV could not be parsed");
    auto compilation = driver.createCompilation();
    driver.reportCompilation(*compilation, true);
    driver.runAnalysis(*compilation);
    ingest::ConvertDriver converter;
    auto loaded = converter.convert(compilation->getRoot());
    require(!converter.diagnostics().hasError() && verifyGrh(loaded), "emitted array SV ingest failed");
    for (grh::Design *candidate : {&design, &loaded}) {
        transform::PassManager manager;
        manager.addPass(std::make_unique<transform::ConstantFoldPass>());
        transform::PassDiagnostics foldDiagnostics;
        require(manager.run(*candidate, foldDiagnostics).success && verifyGrh(*candidate), "GRH array fold failed");
        const auto *folded = candidate->findGraph("array_sv_roundtrip");
        require(folded && outputConstant(*folded, "array").as<uint64_t>() == UINT64_C(0x44332211) &&
                outputConstant(*folded, "singleton").as<uint64_t>() == 0x11 &&
                outputConstant(*folded, "nested").as<uint64_t>() == UINT64_C(0x4433221144332211),
                "GRH fold or SV roundtrip reversed array elements");
    }
}

void grhRecovery() {
    grh::Design design;
    auto &graph = design.createGraph("grh_recover");
    std::vector<grh::ValueId> elements;
    for (unsigned i = 0; i < 4; ++i) {
        auto value = graph.createValue(8, false);
        graph.bindInputPort("e" + std::to_string(i), value);
        elements.push_back(value);
    }
    const auto flat = graph.createValue(32, false);
    const auto concat = graph.createOperation(grh::OperationKind::kConcat);
    for (auto i = elements.rbegin(); i != elements.rend(); ++i) graph.addOperand(concat, *i);
    graph.addResult(concat, flat);
    const auto array = graph.createValue(32, false, grh::ValueType::Logic, std::array<int64_t, 1>{4});
    const auto view = graph.createOperation(grh::OperationKind::kAssign);
    graph.addOperand(view, flat);
    graph.addResult(view, array);
    graph.bindOutputPort("flat", flat);
    const auto duplicate = graph.createArray(elements);
    const auto sum = graph.createValue(32, false, grh::ValueType::Logic, std::array<int64_t, 1>{4});
    const auto add = graph.createOperation(grh::OperationKind::kAdd);
    graph.addOperand(add, array);
    graph.addOperand(add, duplicate);
    graph.addResult(add, sum);
    graph.bindOutputPort("sum", sum);
    transform::PassManager manager;
    manager.addPass(std::make_unique<transform::RedundantElimPass>());
    transform::PassDiagnostics diagnostics;
    require(manager.run(design, diagnostics).success && verifyGrh(design), "GRH recovery failed");
    unsigned arrays = 0, concats = 0;
    for (auto id : graph.operations()) {
        const auto op = graph.getOperation(id);
        if (op.kind() == grh::OperationKind::kArrayCreate) {
            ++arrays;
            require(std::ranges::equal(op.operands(), elements), "GRH recovery reversed element order");
        }
        concats += op.kind() == grh::OperationKind::kConcat;
    }
    require(arrays == 1 && concats == 1, "GRH CSE lost shape or a real flat consumer");
}

void foldingAndRecovery() {
    for (auto domain : {grh::LogicDomain::TwoState, grh::LogicDomain::FourState}) {
        grh::Design design;
        auto &graph = design.createGraph("fold_array");
        design.markAsTop("fold_array");
        const auto a = grhConstant(graph, 8, domain == grh::LogicDomain::TwoState ? "8'h11" : "8'hxz");
        const auto b = grhConstant(graph, 8, "8'h22");
        const auto array = graph.createArray(std::array{a, b});
        graph.bindOutputPort("array", array);
        graph.bindOutputPort("nested", graph.createArray(std::array{array, array}));
        runPass(design, std::make_unique<sim::GrhToGrhsimPass>(
            sim::GrhToGrhsimOptions{"fold_array", domain}));
        runPass(design, std::make_unique<sim::SimplifyPass>());
        require(verifyGrh(design), "folded array lost full type");
        const auto type = graph.valueLogicalType(graph.outputPortValue("array"));
        require(type.width == 16 && type.shape == std::vector<int64_t>{2} && type.domain == domain,
                "folded array lost geometry or domain");
        const auto bits = outputConstant(graph, "array");
        const auto nested = outputConstant(graph, "nested");
        if (domain == grh::LogicDomain::FourState) {
            require(exactlyEqual(bits, slang::SVInt::fromString("16'h22xz")), "array fold changed X/Z positions");
            require(exactlyEqual(nested, slang::SVInt::fromString("32'h22xz22xz")), "nested array fold lost X/Z");
        } else {
            require(bits.as<uint64_t>() == 0x2211 && nested.as<uint64_t>() == 0x22112211,
                    "array fold reversed element order");
        }
    }
}
} // namespace

int main() {
    try {
        contractsAndRoundTrips();
        invalidModels();
        grhFoldingAndSvRoundTrip();
        grhRecovery();
        foldingAndRecovery();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[array-create] " << error.what() << '\n';
        return 1;
    }
}
