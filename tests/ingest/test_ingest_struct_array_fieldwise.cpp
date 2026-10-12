#include "core/ingest.hpp"

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
        if (op.kind() != wolvrix::lib::grh::OperationKind::kReplicate) {
            continue;
        }
        for (const wolvrix::lib::grh::ValueId result : op.results()) {
            if (graph->getValue(result).width() == kPerLeafRepWidth) {
                sawPerLeafReplicate = true;
            }
        }
    }
    if (!sawPerLeafReplicate) {
        return fail("Missing per-leaf replicate of width " +
                    std::to_string(kPerLeafRepWidth) + " (160 x 64)");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_STRUCT_ARRAY_FIELDWISE_DATA_PATH;
    return testFieldWiseStructArray(sourcePath);
}
