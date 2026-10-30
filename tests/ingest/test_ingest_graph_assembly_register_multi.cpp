#include "ingest/sv.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-register-multi] " << message << '\n';
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
    argStorage.emplace_back("ingest-graph-assembly-register-multi");
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

int testGraphAssemblyRegisterMulti(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_register_multi");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_register_multi");
    if (!graph) {
        return fail("Missing graph_assembly_register_multi graph");
    }

    int regDecls = 0;
    int regReads = 0;
    int regWrites = 0;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kRegister:
            ++regDecls;
            break;
        case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            ++regReads;
            break;
        case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            ++regWrites;
            if (!op.attr("regSymbol")) {
                return fail("kRegisterWritePort missing regSymbol attribute");
            }
            if (!op.attr("eventEdge")) {
                return fail("kRegisterWritePort missing eventEdge attribute");
            }
            break;
        default:
            break;
        }
    }

    if (regDecls != 1) {
        return fail("Expected exactly one kRegister declaration");
    }
    if (regReads != 1) {
        return fail("Expected exactly one kRegisterReadPort");
    }
    if (regWrites != 2) {
        return fail("Expected two kRegisterWritePort operations for multi-write");
    }

    return 0;
}

} // namespace

#ifndef WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_MULTI_DATA_PATH
#error "WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_MULTI_DATA_PATH must be defined"
#endif

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_REGISTER_MULTI_DATA_PATH;
    return testGraphAssemblyRegisterMulti(sourcePath);
}
