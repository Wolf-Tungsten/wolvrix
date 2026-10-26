#include "core/ingest.hpp"
#include "core/transform.hpp"
#include "transform/verify.hpp"
#include "transform/const_fold.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"
#include "slang/numeric/SVInt.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-struct-array-fieldwise] " << message << '\n';
    return 1;
}

struct CompilationBundle {
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

std::unique_ptr<CompilationBundle> compileInput(const std::filesystem::path& sourcePath,
                                                std::string_view topModule) {
    auto bundle = std::make_unique<CompilationBundle>();
    auto& driver = bundle->driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-struct-array-fieldwise");
    argStorage.emplace_back(sourcePath.string());
    std::vector<const char*> argv;
    argv.reserve(argStorage.size());
    for (const std::string& arg : argStorage) {
        argv.push_back(arg.c_str());
    }

    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data())) {
        return nullptr;
    }
    if (!driver.processOptions()) {
        return nullptr;
    }
    if (!driver.parseAllSources()) {
        return nullptr;
    }

    bundle->compilation = driver.createCompilation();
    if (!bundle->compilation) {
        return nullptr;
    }
    driver.reportCompilation(*bundle->compilation, /* quiet */ true);
    driver.runAnalysis(*bundle->compilation);
    return bundle;
}

// Widest legitimate per-leaf value: max field width (64) x largest array
// extent (512). Whole-array flattens would be 36864 (512x72), 11520
// (160x72) or 5184 (72x72) and must never materialize.
constexpr int64_t kMaxLeafValueWidth = 32768;  // 512 x 64
constexpr int64_t kWholePadWidth = 36864;      // 512 x 72
constexpr int64_t kWholeRepWidth = 11520;      // 160 x 72
constexpr int64_t kPerLeafRepWidth = 10240;    // 160 x 64

int testFieldWiseStructArray(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "ingest_struct_array_fieldwise");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        std::ostringstream out;
        for (const auto& message : driver.diagnostics().messages()) {
            out << message.message << ';';
        }
        return fail("Unexpected ingest diagnostics: " + out.str());
    }

    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("ingest_struct_array_fieldwise");
    if (!graph) {
        return fail("Missing ingest_struct_array_fieldwise graph after ingest");
    }

    bool sawPerLeafReplicate = false;
    bool sawBankSlice = false;
    bool sawWaySlice = false;
    for (const wolvrix::lib::grh::ValueId valueId : graph->values()) {
        const wolvrix::lib::grh::Value value = graph->getValue(valueId);
        const int64_t width = value.width();
        if (width > kMaxLeafValueWidth) {
            return fail("Value wider than the per-leaf bound " +
                        std::to_string(kMaxLeafValueWidth) + ": " +
                        std::string(value.symbolText()) + " width=" + std::to_string(width));
        }
        if (width == kWholePadWidth || width == kWholeRepWidth) {
            return fail("Whole-array struct value materialized: width=" + std::to_string(width));
        }
    }
    for (const wolvrix::lib::grh::OperationId opId : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if ((op.kind() == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
             op.kind() == wolvrix::lib::grh::OperationKind::kSliceArray) &&
            op.operands().size() == 2 && op.results().size() == 1) {
            const int64_t baseWidth = graph->getValue(op.operands().front()).width();
            const int64_t resultWidth = graph->getValue(op.results().front()).width();
            if (op.kind() == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                baseWidth == 2048 && resultWidth == 256) {
                sawBankSlice = true;
                if (graph->getValue(op.operands()[1]).width() < 11) {
                    return fail("Bank index bit offset truncates before 7 * 256");
                }
            }
            if (baseWidth == 256 && resultWidth == 64) {
                sawWaySlice = true;
                const int64_t minIndexWidth =
                    op.kind() == wolvrix::lib::grh::OperationKind::kSliceDynamic ? 8 : 2;
                if (graph->getValue(op.operands()[1]).width() < minIndexWidth) {
                    return fail("Way index cannot address all four entries");
                }
            }
        }
        if (op.kind() != wolvrix::lib::grh::OperationKind::kReplicate) {
            continue;
        }
        for (const wolvrix::lib::grh::ValueId result : op.results()) {
            if (graph->getValue(result).width() == kPerLeafRepWidth) {
                sawPerLeafReplicate = true;
            }
        }
        // Invariant (stage-07 bug4): result width == rep x operand width. A
        // bare symbol leaf carries no plan-stage widthHint; the replicate
        // must still take the true operand width, never collapse to 1.
        const auto repAttr = op.attr("rep");
        if (!repAttr || op.operands().size() != 1 || op.results().size() != 1) {
            continue;
        }
        const auto* rep = std::get_if<int64_t>(&*repAttr);
        if (!rep || *rep <= 0) {
            continue;
        }
        const int64_t operandWidth = graph->getValue(op.operands().front()).width();
        const int64_t resultWidth = graph->getValue(op.results().front()).width();
        if (resultWidth != *rep * operandWidth) {
            return fail("kReplicate width mismatch: result=" + std::to_string(resultWidth) +
                        " rep=" + std::to_string(*rep) +
                        " operand=" + std::to_string(operandWidth));
        }
    }
    if (!sawPerLeafReplicate) {
        return fail("Missing per-leaf replicate of width " +
                    std::to_string(kPerLeafRepWidth) + " (160 x 64)");
    }
    if (!sawBankSlice || !sawWaySlice) {
        return fail("Missing bank/way reads of the split struct leaf");
    }
    return 0;
}

int testPartialStructWrites(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "ingest_struct_array_partial_write");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile partial struct writes");
    }
    wolvrix::lib::ingest::ConvertDriver driver;
    const auto design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        return fail("Unexpected partial struct write diagnostics");
    }
    const auto* graph = design.findGraph("ingest_struct_array_partial_write");
    if (!graph) {
        return fail("Missing partial struct write graph");
    }
    unsigned registers = 0;
    unsigned concats = 0;
    for (const auto id : graph->operations()) {
        const auto op = graph->getOperation(id);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister) {
            ++registers;
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kConcat) {
            ++concats;
            // The largest leaf is four 8-bit masks. No partial update may
            // concatenate a complete leaf in place of one addressed element.
            for (const auto result : op.results()) {
                if (graph->getValue(result).width() > 32) {
                    return fail("Partial struct write exceeds leaf storage: " +
                                std::string(op.symbolText()) + " width=" +
                                std::to_string(graph->getValue(result).width()));
                }
            }
        }
    }
    if (registers != 9 || concats == 0) {
        return fail("Missing static, dynamic or nested struct write coverage");
    }
    return 0;
}

int testArrayConstruction(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "ingest_array_create");
    if (!bundle || !bundle->compilation) return fail("Array construction fixture did not compile");
    wolvrix::lib::ingest::ConvertDriver driver;
    auto design = driver.convert(bundle->compilation->getRoot());
    if (driver.diagnostics().hasError()) return fail("Array construction ingest failed");
    wolvrix::lib::transform::PassManager verifier;
    verifier.addPass(std::make_unique<wolvrix::lib::transform::GrhVerifyPass>());
    wolvrix::lib::transform::PassDiagnostics diagnostics;
    if (!verifier.run(design, diagnostics).success) return fail("Raw array constructor contract is invalid");
    const auto *graph = design.findGraph("ingest_array_create");
    if (!graph) return fail("Missing array construction graph");
    unsigned scalarArrays = 0, nestedArrays = 0, singletons = 0;
    for (const auto id : graph->operations()) {
        const auto op = graph->getOperation(id);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kArrayCreate) continue;
        const auto value = graph->getValue(op.results()[0]);
        scalarArrays += value.shape().size() == 1 && value.shape()[0] == 4;
        nestedArrays += value.shape().size() == 2 && value.shape()[0] == 2 && value.shape()[1] == 2;
        singletons += value.shape().size() == 1 && value.shape()[0] == 1;
    }
    if (scalarArrays < 5 || nestedArrays < 2 || singletons == 0)
        return fail("Raw ingest lost struct, range-direction, pattern or sub-array construction: scalar=" +
                    std::to_string(scalarArrays) + " nested=" + std::to_string(nestedArrays) +
                    " singleton=" + std::to_string(singletons));
    wolvrix::lib::transform::PassManager folder;
    folder.addPass(std::make_unique<wolvrix::lib::transform::ConstantFoldPass>());
    if (!folder.run(design, diagnostics).success) return fail("Array order fixture failed to fold");
    for (const char *name : {"descending_bits", "ascending_bits", "nested_bits", "pattern_bits", "singleton_bits", "mixed_bits"}) {
        auto value = graph->outputPortValue(name);
        bool found = false;
        for (unsigned depth = 0; depth < 32; ++depth) {
            const auto op = graph->getOperation(graph->valueDef(value));
            if (op.kind() == wolvrix::lib::grh::OperationKind::kConstant) {
                const auto literal = std::get<std::string>(*op.attr("constValue"));
                const auto expected = std::string_view(name) == "singleton_bits" ? UINT64_C(0x11) :
                    std::string_view(name) == "mixed_bits" ? UINT64_C(0x443322112211) : UINT64_C(0x44332211);
                if (slang::SVInt::fromString(literal).as<uint64_t>() != expected)
                    return fail("Array constructor changed element order for " + std::string(name));
                found = true;
                break;
            }
            if ((op.kind() != wolvrix::lib::grh::OperationKind::kAssign) || op.operands().size() != 1)
                return fail("Array order fixture did not fold: " + std::string(name));
            value = op.operands()[0];
        }
        if (!found) return fail("Array order fixture alias cycle");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_STRUCT_ARRAY_FIELDWISE_DATA_PATH;
    if (testFieldWiseStructArray(sourcePath) != 0) {
        return 1;
    }
    if (testPartialStructWrites(sourcePath) != 0) return 1;
    return testArrayConstruction(sourcePath);
}
