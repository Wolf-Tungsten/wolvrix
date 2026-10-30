#include "ingest/sv.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-stmt-lowerer-errors] " << message << '\n';
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
    driver.options.compilationFlags.at(slang::ast::CompilationFlags::AllowTopLevelIfacePorts) = true;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-stmt-lowerer-errors");
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

const slang::ast::InstanceSymbol* findTopInstance(slang::ast::Compilation& compilation,
                                                  const slang::ast::RootSymbol& root,
                                                  std::string_view moduleName) {
    for (const slang::ast::InstanceSymbol* instance : root.topInstances) {
        if (!instance) {
            continue;
        }
        if (instance->getDefinition().name == moduleName) {
            return instance;
        }
    }
    if (moduleName.empty() && root.topInstances.size() == 1 && root.topInstances[0]) {
        return root.topInstances[0];
    }
    if (const slang::ast::Symbol* symbol = root.find(moduleName)) {
        if (const auto* definition = symbol->as_if<slang::ast::DefinitionSymbol>()) {
            return &slang::ast::InstanceSymbol::createDefault(compilation, *definition);
        }
    }
    for (const slang::ast::Symbol* symbol : compilation.getDefinitions()) {
        if (!symbol) {
            continue;
        }
        if (const auto* definition = symbol->as_if<slang::ast::DefinitionSymbol>()) {
            if (definition->name == moduleName) {
                return &slang::ast::InstanceSymbol::createDefault(compilation, *definition);
            }
        }
    }
    return nullptr;
}

bool buildLoweringPlan(const std::filesystem::path& sourcePath, std::string_view topModule,
                       wolvrix::lib::ingest::ConvertDiagnostics& diagnostics,
                       wolvrix::lib::ingest::ModulePlan& outPlan,
                       wolvrix::lib::ingest::LoweringPlan& outPlanLowering) {
    auto bundle = compileInput(sourcePath, topModule);
    if (!bundle || !bundle->compilation) {
        return false;
    }
    auto& compilation = *bundle->compilation;
    const slang::ast::RootSymbol& root = compilation.getRoot();
    const slang::ast::InstanceSymbol* top = findTopInstance(compilation, root, topModule);
    if (!top) {
        return false;
    }

    wolvrix::lib::Logger logger;
    wolvrix::lib::ingest::PlanCache planCache;
    wolvrix::lib::ingest::PlanTaskQueue planQueue;
    planQueue.reset();

    wolvrix::lib::ingest::ConvertContext context{};
    context.compilation = &root.getCompilation();
    context.root = &root;
    context.diagnostics = &diagnostics;
    context.logger = &logger;
    context.planCache = &planCache;
    context.planQueue = &planQueue;

    wolvrix::lib::ingest::ModulePlanner planner(context);
    wolvrix::lib::ingest::StmtLowererPass stmtLowerer(context);

    outPlan = planner.plan(top->body);
    outPlanLowering = {};
    stmtLowerer.lower(outPlan, outPlanLowering);
    return true;
}

bool hasError(const wolvrix::lib::ingest::ConvertDiagnostics& diagnostics) {
    for (const auto& message : diagnostics.messages()) {
        if (message.kind == wolvrix::lib::ingest::ConvertDiagnosticKind::Error) {
            return true;
        }
    }
    return false;
}

int expectErrorNoWrites(const std::filesystem::path& sourcePath, std::string_view moduleName) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildLoweringPlan(sourcePath, moduleName, diagnostics, plan, lowering)) {
        return fail("Failed to build lowering plan for " + std::string(moduleName) +
                    " in " + sourcePath.string());
    }
    if (!hasError(diagnostics)) {
        return fail("Expected Convert diagnostics error for " + std::string(moduleName));
    }
    if (!lowering.writes.empty()) {
        return fail("Expected no write intents for " + std::string(moduleName) +
                    ", got " + std::to_string(lowering.writes.size()));
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        std::filesystem::path(WOLF_SV_INGEST_STMT_ERROR_DATA_PATH);

    if (!std::filesystem::exists(sourcePath)) {
        return fail("Missing stmt lowerer error input file at " + sourcePath.string());
    }

    if (int status = expectErrorNoWrites(sourcePath, "stmt_lowerer_pattern_if"); status != 0) {
        return status;
    }
    if (int status = expectErrorNoWrites(sourcePath, "stmt_lowerer_pattern_case"); status != 0) {
        return status;
    }
    if (int status = expectErrorNoWrites(sourcePath, "stmt_lowerer_while_stmt"); status != 0) {
        return status;
    }
    if (int status = expectErrorNoWrites(sourcePath, "stmt_lowerer_do_while_stmt"); status != 0) {
        return status;
    }
    return expectErrorNoWrites(sourcePath, "stmt_lowerer_forever_stmt");
}
