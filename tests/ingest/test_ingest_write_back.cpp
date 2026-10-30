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
    std::cerr << "[ingest-write-back] " << message << '\n';
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
    argStorage.emplace_back("ingest-write-back");
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

bool buildWriteBackPlan(const std::filesystem::path& sourcePath, std::string_view topModule,
                        wolvrix::lib::ingest::ConvertDiagnostics& diagnostics,
                        wolvrix::lib::ingest::ModulePlan& outPlan,
                        wolvrix::lib::ingest::LoweringPlan& outLowering,
                        wolvrix::lib::ingest::WriteBackPlan& outWriteBack) {
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
    wolvrix::lib::ingest::WriteBackPass writeBack(context);

    outPlan = planner.plan(top->body);
    outLowering = {};
    stmtLowerer.lower(outPlan, outLowering);
    outWriteBack = writeBack.lower(outPlan, outLowering);
    return true;
}

bool hasWarningMessage(const wolvrix::lib::ingest::ConvertDiagnostics& diagnostics,
                       std::string_view needle) {
    for (const auto& message : diagnostics.messages()) {
        if (message.kind == wolvrix::lib::ingest::ConvertDiagnosticKind::Warning &&
            message.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int testWriteBackSeq(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_seq", diagnostics, plan, lowering, writeBack)) {
        return fail("Failed to build write-back plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 write-back entry in " + sourcePath.string());
    }
    const auto& entry = writeBack.entries.front();
    if (!entry.target.valid() ||
        plan.symbolTable.text(entry.target) != std::string_view("q")) {
        return fail("Unexpected write-back target in " + sourcePath.string());
    }
    if (entry.domain != wolvrix::lib::ingest::ControlDomain::Sequential) {
        return fail("Unexpected write-back domain in " + sourcePath.string());
    }
    if (entry.eventEdges.size() != 1 || entry.eventOperands.size() != 1) {
        return fail("Unexpected write-back event binding in " + sourcePath.string());
    }
    if (entry.eventEdges.front() != wolvrix::lib::ingest::EventEdge::Posedge) {
        return fail("Unexpected write-back event edge in " + sourcePath.string());
    }
    if (entry.eventOperands.front() >= lowering.values.size()) {
        return fail("Write-back event operand index out of range in " + sourcePath.string());
    }
    const auto& eventNode = lowering.values[entry.eventOperands.front()];
    if (eventNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(eventNode.symbol) != std::string_view("clk")) {
        return fail("Unexpected write-back event operand in " + sourcePath.string());
    }
    if (entry.updateCond == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.updateCond >= lowering.values.size()) {
        return fail("Missing write-back update condition in " + sourcePath.string());
    }
    if (entry.nextValue == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.nextValue >= lowering.values.size()) {
        return fail("Missing write-back next value in " + sourcePath.string());
    }
    const auto& updateNode = lowering.values[entry.updateCond];
    if (updateNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        updateNode.op != wolvrix::lib::grh::OperationKind::kLogicOr) {
        return fail("Unexpected write-back update condition in " + sourcePath.string());
    }
    const auto& nextNode = lowering.values[entry.nextValue];
    if (nextNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        nextNode.op != wolvrix::lib::grh::OperationKind::kMux) {
        return fail("Unexpected write-back next value in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testWriteBackLatch(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_latch", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back latch plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 latch write-back entry in " + sourcePath.string());
    }
    const auto& entry = writeBack.entries.front();
    if (entry.domain != wolvrix::lib::ingest::ControlDomain::Latch) {
        return fail("Unexpected latch write-back domain in " + sourcePath.string());
    }
    if (!entry.eventEdges.empty() || !entry.eventOperands.empty()) {
        return fail("Unexpected latch write-back event binding in " + sourcePath.string());
    }
    if (entry.updateCond == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.updateCond >= lowering.values.size()) {
        return fail("Missing latch write-back update condition in " + sourcePath.string());
    }
    const auto& updateNode = lowering.values[entry.updateCond];
    if (updateNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(updateNode.symbol) != std::string_view("en")) {
        return fail("Unexpected latch update condition in " + sourcePath.string());
    }
    if (entry.nextValue == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.nextValue >= lowering.values.size()) {
        return fail("Missing latch write-back next value in " + sourcePath.string());
    }
    const auto& nextNode = lowering.values[entry.nextValue];
    if (nextNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(nextNode.symbol) != std::string_view("d")) {
        return fail("Unexpected latch write-back next value in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testWriteBackComb(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_comb", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back comb plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 comb write-back entry in " + sourcePath.string());
    }
    const auto& entry = writeBack.entries.front();
    if (entry.domain != wolvrix::lib::ingest::ControlDomain::Combinational) {
        return fail("Unexpected comb write-back domain in " + sourcePath.string());
    }
    if (entry.updateCond == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.updateCond >= lowering.values.size()) {
        return fail("Missing comb write-back update condition in " + sourcePath.string());
    }
    if (entry.nextValue == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.nextValue >= lowering.values.size()) {
        return fail("Missing comb write-back next value in " + sourcePath.string());
    }
    const auto& nextNode = lowering.values[entry.nextValue];
    if (nextNode.kind == wolvrix::lib::ingest::ExprNodeKind::Operation &&
        nextNode.op == wolvrix::lib::grh::OperationKind::kMux) {
        return fail("Unexpected comb write-back mux next value in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testWriteBackCaseComb(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_case_comb", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back case comb plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 case comb write-back entry in " + sourcePath.string());
    }
    const auto& entry = writeBack.entries.front();
    if (entry.domain != wolvrix::lib::ingest::ControlDomain::Combinational) {
        return fail("Unexpected case comb write-back domain in " + sourcePath.string());
    }
    if (entry.updateCond == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.updateCond >= lowering.values.size()) {
        return fail("Missing case comb write-back update condition in " + sourcePath.string());
    }
    if (entry.nextValue == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.nextValue >= lowering.values.size()) {
        return fail("Missing case comb write-back next value in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testWriteBackMissingEdge(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_bad_seq", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back missing-edge plan for " + sourcePath.string());
    }

    if (!writeBack.entries.empty()) {
        return fail("Expected missing-edge write-back to be dropped in " + sourcePath.string());
    }
    if (!hasWarningMessage(diagnostics, "edge-sensitive")) {
        return fail("Expected missing-edge warning in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_WRITE_BACK_DATA_PATH;
    if (sourcePath.empty()) {
        return fail("Missing write-back fixture path");
    }
    if (int result = testWriteBackSeq(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteBackLatch(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteBackComb(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteBackCaseComb(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteBackMissingEdge(sourcePath); result != 0) {
        return result;
    }
    return 0;
}
