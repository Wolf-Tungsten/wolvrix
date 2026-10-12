#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-port-array-elem-read] " << message << '\n';
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
    argStorage.emplace_back("ingest-port-array-elem-read");
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

int testPortArrayElemRead(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "ingest_port_array_elem_read");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        std::string out;
        for (const auto& message : driver.diagnostics().messages()) {
            out += message.message;
            out += ';';
        }
        return fail("Unexpected ingest diagnostics: " + out);
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("ingest_port_array_elem_read");
    if (!graph) {
        return fail("Missing graph after ingest");
    }

    bool sawArrayRead = false;
    for (const wolvrix::lib::grh::OperationId opId : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister) {
            return fail("Phantom register created for an input port leaf: " +
                        std::string(op.symbolText()));
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kSliceArray) {
            sawArrayRead = true;
        }
    }
    if (!sawArrayRead) {
        return fail("Expected the port element read to lower to kSliceArray");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_PORT_ARRAY_ELEM_READ_DATA_PATH;
    return testPortArrayElemRead(sourcePath);
}
