#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-memory-port-lowerer] " << message << '\n';
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
    argStorage.emplace_back("ingest-memory-port-lowerer");
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

bool buildMemoryPlan(const std::filesystem::path& sourcePath, std::string_view topModule,
                     wolvrix::lib::ingest::ConvertDiagnostics& diagnostics,
                     wolvrix::lib::ingest::ModulePlan& outPlan,
                     wolvrix::lib::ingest::LoweringPlan& outLowering) {
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
    wolvrix::lib::ingest::MemoryPortLowererPass memLowerer(context);

    outPlan = planner.plan(top->body);
    outLowering = {};
    stmtLowerer.lower(outPlan, outLowering);
    memLowerer.lower(outPlan, outLowering);
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

std::size_t countOpInExpr(const wolvrix::lib::ingest::LoweringPlan& plan,
                          wolvrix::lib::ingest::ExprNodeId root,
                          wolvrix::lib::grh::OperationKind op) {
    if (root == wolvrix::lib::ingest::kInvalidPlanIndex || root >= plan.values.size()) {
        return 0;
    }
    std::unordered_set<wolvrix::lib::ingest::ExprNodeId> visited;
    std::vector<wolvrix::lib::ingest::ExprNodeId> stack;
    stack.push_back(root);
    std::size_t count = 0;
    while (!stack.empty()) {
        wolvrix::lib::ingest::ExprNodeId current = stack.back();
        stack.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        const auto& node = plan.values[current];
        if (node.kind == wolvrix::lib::ingest::ExprNodeKind::Operation) {
            if (node.op == op) {
                ++count;
            }
            for (wolvrix::lib::ingest::ExprNodeId operand : node.operands) {
                if (operand != wolvrix::lib::ingest::kInvalidPlanIndex &&
                    operand < plan.values.size()) {
                    stack.push_back(operand);
                }
            }
        }
    }
    return count;
}

int testReadComb(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_comb", diagnostics, plan, lowering)) {
        return fail("Failed to build comb memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected 1 memory read entry in " + sourcePath.string());
    }
    const auto& entry = lowering.memoryReads.front();
    if (entry.isSync) {
        return fail("Unexpected sync flag for comb memory read");
    }
    if (!entry.eventEdges.empty() || !entry.eventOperands.empty()) {
        return fail("Unexpected event binding for comb memory read");
    }
    if (entry.addressIndices.size() != 1 ||
        entry.addressIndices.front() >= lowering.values.size()) {
        return fail("Missing comb memory read address");
    }
    const auto& addrNode = lowering.values[entry.addressIndices.front()];
    if (addrNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(addrNode.symbol) != std::string_view("addr")) {
        return fail("Unexpected comb memory read address symbol");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in comb read");
    }
    return 0;
}

int testReadSeq(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_seq", diagnostics, plan, lowering)) {
        return fail("Failed to build seq memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected 1 sync memory read entry in " + sourcePath.string());
    }
    const auto& entry = lowering.memoryReads.front();
    if (!entry.isSync) {
        return fail("Expected sync flag for seq memory read");
    }
    if (entry.eventEdges.size() != 1 || entry.eventOperands.size() != 1) {
        return fail("Unexpected event binding for sync memory read");
    }
    if (entry.eventEdges.front() != wolvrix::lib::ingest::EventEdge::Posedge) {
        return fail("Unexpected sync memory read edge");
    }
    const auto& clkNode = lowering.values[entry.eventOperands.front()];
    if (clkNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(clkNode.symbol) != std::string_view("clk")) {
        return fail("Unexpected sync memory read clock operand");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in seq read");
    }
    return 0;
}

int testReadSeqEnable(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_seq_en", diagnostics, plan, lowering)) {
        return fail("Failed to build seq enable memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected 1 sync memory read entry for enable");
    }
    const auto& entry = lowering.memoryReads.front();
    if (entry.updateCond == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.updateCond >= lowering.values.size()) {
        return fail("Missing sync read enable condition");
    }
    const auto& condNode = lowering.values[entry.updateCond];
    if (condNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(condNode.symbol) != std::string_view("en")) {
        return fail("Unexpected sync read enable condition");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in seq enable read");
    }
    return 0;
}

int testReadSeqSelfHold(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_seq_self_hold", diagnostics, plan, lowering)) {
        return fail("Failed to build seq self-hold memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected 1 memory read entry in seq self-hold");
    }
    const auto& entry = lowering.memoryReads.front();
    if (entry.isSync) {
        return fail("Expected comb read for seq self-hold");
    }
    if (entry.updateCond != wolvrix::lib::ingest::kInvalidPlanIndex) {
        return fail("Unexpected update condition for seq self-hold read");
    }
    if (!entry.eventEdges.empty() || !entry.eventOperands.empty()) {
        return fail("Unexpected event binding for seq self-hold read");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in seq self-hold read");
    }
    return 0;
}

int testReadElementBit(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_element_bit", diagnostics, plan, lowering)) {
        return fail("Failed to build element bit memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.empty()) {
        return fail("Expected memory read entries in element bit read");
    }
    bool sawElementReplacement = false;
    for (const auto& entry : lowering.memoryReads) {
        if (entry.replacement == wolvrix::lib::ingest::kInvalidPlanIndex ||
            entry.replacement >= lowering.values.size()) {
            return fail("Missing element read replacement expression");
        }
        if (entry.data != entry.replacement) {
            sawElementReplacement = true;
        }
        const auto& replacement = lowering.values[entry.replacement];
        if (replacement.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
            replacement.op != wolvrix::lib::grh::OperationKind::kSliceDynamic) {
            return fail("Unexpected element read replacement node");
        }
        if (entry.addressIndices.size() != 1) {
            return fail("Unexpected element bit read address index count");
        }
        const auto& addrNode = lowering.values[entry.addressIndices.front()];
        if (addrNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
            plan.symbolTable.text(addrNode.symbol) != std::string_view("addr")) {
            return fail("Unexpected element bit read address symbol");
        }
    }
    if (!sawElementReplacement) {
        return fail("Expected bit selects to remain above the memory read expression");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in element bit read");
    }
    return 0;
}

int testReadCast(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_read_cast", diagnostics, plan, lowering)) {
        return fail("Failed to build cast memory plan for " + sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected one cast-wrapped memory read");
    }
    const auto& entry = lowering.memoryReads.front();
    if (entry.addressIndices.size() != 1 ||
        entry.addressIndices.front() >= lowering.values.size()) {
        return fail("Missing cast read address");
    }
    const auto& addrNode = lowering.values[entry.addressIndices.front()];
    if (addrNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(addrNode.symbol) != std::string_view("addr")) {
        return fail("Unexpected cast read address symbol");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in cast read");
    }
    return 0;
}

int testPackedAggregateWholeReg(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "packed_aggregate_whole_reg", diagnostics, plan, lowering)) {
        return fail("Failed to build packed aggregate whole-reg plan for " + sourcePath.string());
    }

    if (!lowering.memoryReads.empty() || !lowering.memoryWrites.empty() ||
        !lowering.memoryFills.empty()) {
        return fail("Expected whole packed aggregate registers to avoid memory ports");
    }
    if (diagnostics.hasError()) {
        std::string errors;
        for (const auto& message : diagnostics.messages()) {
            if (message.kind == wolvrix::lib::ingest::ConvertDiagnosticKind::Error) {
                if (!errors.empty()) {
                    errors.append("; ");
                }
                errors.append(message.message);
            }
        }
        return fail("Unexpected Convert diagnostics errors in packed aggregate whole-reg: " +
                    errors);
    }
    return 0;
}

int testPackedAggregateIndexedMem(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "packed_aggregate_indexed_mem", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build packed aggregate indexed-memory plan for " +
                    sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected one indexed read from packed aggregate memory");
    }
    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected one indexed write to packed aggregate memory");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in packed aggregate indexed-memory");
    }
    return 0;
}

int testPackedAggregatePriorityRowWrite(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "packed_aggregate_priority_row_write", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build packed aggregate priority row-write plan for " +
                    sourcePath.string());
    }

    if (lowering.memoryReads.size() != 1) {
        return fail("Expected one indexed read from priority row-write memory");
    }
    // stage-04: same-address writes are no longer merged into one port; the
    // if / else-if chain stays two ports in statement order (later port has
    // overriding priority by declaration order), and element writes without a
    // data slice carry no mask.
    if (lowering.memoryWrites.size() != 2) {
        return fail("Expected priority writes to stay two write ports in declaration order");
    }
    const auto& firstWrite = lowering.memoryWrites[0];
    const auto& secondWrite = lowering.memoryWrites[1];
    if (firstWrite.mask != wolvrix::lib::ingest::kInvalidPlanIndex ||
        secondWrite.mask != wolvrix::lib::ingest::kInvalidPlanIndex) {
        return fail("Expected unmasked element writes for priority row-write");
    }
    if (firstWrite.addressIndices.size() != 1 || secondWrite.addressIndices.size() != 1) {
        return fail("Expected one address index per priority row-write port");
    }
    const auto& firstData = lowering.values[firstWrite.data];
    const auto& secondData = lowering.values[secondWrite.data];
    if (firstData.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(firstData.symbol) != std::string_view("d0") ||
        secondData.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(secondData.symbol) != std::string_view("d1")) {
        return fail("Unexpected priority row-write data symbols");
    }
    const auto& firstCond = lowering.values[firstWrite.updateCond];
    if (firstCond.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol ||
        plan.symbolTable.text(firstCond.symbol) != std::string_view("a")) {
        return fail("Unexpected first priority row-write update condition");
    }
    if (countOpInExpr(lowering, secondWrite.updateCond,
                      wolvrix::lib::grh::OperationKind::kLogicNot) == 0) {
        return fail("Expected else-if guard negation in second priority row-write");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in packed aggregate priority row-write");
    }
    return 0;
}

int testWriteDynamicUp(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_up", diagnostics, plan, lowering)) {
        return fail("Failed to build dynamic up memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in dynamic up");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for dynamic up");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected dynamic up mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in dynamic up");
    }
    return 0;
}

int testWriteDynamicDown(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_down", diagnostics, plan, lowering)) {
        return fail("Failed to build dynamic down memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in dynamic down");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for dynamic down");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected dynamic down mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in dynamic down");
    }
    return 0;
}

int testWriteDynamicParamWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_param_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build param width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in param width");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for param width");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected param width mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in param width");
    }
    return 0;
}

int testWriteDynamicExprWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_expr_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build expr width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in expr width");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for expr width");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected expr width mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in expr width");
    }
    return 0;
}

int testWriteDynamicPkgWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_pkg_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build package width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in package width");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for package width");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected package width mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in package width");
    }
    return 0;
}

int testWriteDynamicPkgQualified(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_pkg_qualified", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build package qualified memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in package qualified");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for package qualified");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected package qualified mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in package qualified");
    }
    return 0;
}

int testWriteDynamicExprComplex(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_expr_complex", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build complex expr memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in complex expr");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for complex expr");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Operation ||
        maskNode.op != wolvrix::lib::grh::OperationKind::kShl) {
        return fail("Unexpected complex expr mask op");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in complex expr");
    }
    return 0;
}

int testWriteDynamicConcatWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_concat_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build concat width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in concat width");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for concat width");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in concat width");
    }
    return 0;
}

int testWriteDynamicConcatExprWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_concat_expr_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build concat expr width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in concat expr width");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in concat expr width");
    }
    return 0;
}

int testWriteDynamicReplWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_repl_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build replicate width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in replicate width");
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write for replicate width");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in replicate width");
    }
    return 0;
}

int testWriteDynamicReplExprWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_repl_expr_width", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build replicate expr width memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in replicate expr width");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in replicate expr width");
    }
    return 0;
}

int testWriteDynamicBaseWarning(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_base_warn", diagnostics, plan,
                         lowering)) {
        return fail("Failed to build dynamic base warning plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in dynamic base warning");
    }
    if (!hasWarningMessage(diagnostics,
                           "Indexed part-select base is dynamic; bounds check skipped")) {
        return fail("Expected warning for dynamic base");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in dynamic base warning");
    }
    return 0;
}

int testWriteMultiDim(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_multi_dim", diagnostics, plan, lowering)) {
        return fail("Failed to build multi-dim memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in multi-dim");
    }
    const auto& entry = lowering.memoryWrites.front();
    // stage-04: multi-dim writes keep per-dim indices (no linearization).
    if (entry.addressIndices.size() != 2) {
        return fail("Expected two per-dim address indices in multi-dim write");
    }
    for (const auto indexId : entry.addressIndices) {
        const auto& indexNode = lowering.values[indexId];
        if (indexNode.kind != wolvrix::lib::ingest::ExprNodeKind::Symbol) {
            return fail("Unexpected multi-dim address index kind");
        }
    }
    if (plan.symbolTable.text(lowering.values[entry.addressIndices[0]].symbol) !=
            std::string_view("idx0") ||
        plan.symbolTable.text(lowering.values[entry.addressIndices[1]].symbol) !=
            std::string_view("idx1")) {
        return fail("Unexpected multi-dim address index symbols");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in multi-dim");
    }
    return 0;
}

int testWriteMultiDimOffset(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_multi_dim_offset", diagnostics, plan, lowering)) {
        return fail("Failed to build offset multi-dim memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in offset multi-dim");
    }
    const auto& entry = lowering.memoryWrites.front();
    // stage-04: per-dim indices carry their own bound normalization.
    if (entry.addressIndices.size() != 2) {
        return fail("Expected two per-dim address indices in offset multi-dim write");
    }
    if (countOpInExpr(lowering, entry.addressIndices[0],
                      wolvrix::lib::grh::OperationKind::kSub) < 1 ||
        countOpInExpr(lowering, entry.addressIndices[1],
                      wolvrix::lib::grh::OperationKind::kSub) < 1) {
        return fail("Expected address normalization for offset multi-dim write");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in offset multi-dim");
    }
    return 0;
}

int testWriteRangeOutOfBounds(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_range_oob", diagnostics, plan, lowering)) {
        return fail("Failed to build oob range memory plan for " + sourcePath.string());
    }

    if (!lowering.memoryWrites.empty()) {
        return fail("Expected no memory write entry for oob range");
    }
    if (!hasWarningMessage(diagnostics, "Memory range mask exceeds memory width")) {
        return fail("Expected warning for oob range");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in oob range");
    }
    return 0;
}

int testWriteDynamicBadWidth(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_bad_width", diagnostics, plan, lowering)) {
        return fail("Failed to build bad width memory plan for " + sourcePath.string());
    }

    if (!lowering.memoryWrites.empty()) {
        return fail("Expected no memory write entry for bad width");
    }
    if (!lowering.loweredStmts.empty() &&
        !hasWarningMessage(diagnostics, "Indexed part-select width must be constant")) {
        return fail("Expected warning for bad width");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in bad width");
    }
    return 0;
}

int testWriteDynamicOutOfBoundsUp(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_oob_up", diagnostics, plan, lowering)) {
        return fail("Failed to build out-of-bounds up memory plan for " + sourcePath.string());
    }

    if (!lowering.memoryWrites.empty()) {
        return fail("Expected no memory write entry for out-of-bounds up");
    }
    if (!hasWarningMessage(diagnostics, "Indexed part-select exceeds memory width")) {
        return fail("Expected warning for out-of-bounds up");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in out-of-bounds up");
    }
    return 0;
}

int testWriteDynamicOutOfBoundsDown(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_dynamic_oob_down", diagnostics, plan, lowering)) {
        return fail("Failed to build out-of-bounds down memory plan for " + sourcePath.string());
    }

    if (!lowering.memoryWrites.empty()) {
        return fail("Expected no memory write entry for out-of-bounds down");
    }
    if (!hasWarningMessage(diagnostics, "Indexed part-select exceeds memory width")) {
        return fail("Expected warning for out-of-bounds down");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in out-of-bounds down");
    }
    return 0;
}

int testMaskedWrite(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    if (!buildMemoryPlan(sourcePath, "mem_write_mask", diagnostics, plan, lowering)) {
        return fail("Failed to build masked memory plan for " + sourcePath.string());
    }

    if (lowering.memoryWrites.size() != 1) {
        return fail("Expected 1 memory write entry in " + sourcePath.string());
    }
    const auto& entry = lowering.memoryWrites.front();
    if (!entry.isMasked) {
        return fail("Expected masked write flag");
    }
    if (entry.mask == wolvrix::lib::ingest::kInvalidPlanIndex ||
        entry.mask >= lowering.values.size()) {
        return fail("Missing memory write mask");
    }
    const auto& maskNode = lowering.values[entry.mask];
    if (maskNode.kind != wolvrix::lib::ingest::ExprNodeKind::Constant ||
        maskNode.literal != "8'b00001111") {
        return fail("Unexpected memory write mask literal");
    }
    if (entry.eventEdges.size() != 1 || entry.eventOperands.size() != 1) {
        return fail("Unexpected memory write event binding");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in masked write");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_MEMORY_PORT_DATA_PATH;
    if (sourcePath.empty()) {
        return fail("Missing memory port fixture path");
    }
    if (int result = testReadComb(sourcePath); result != 0) {
        return result;
    }
    if (int result = testReadSeq(sourcePath); result != 0) {
        return result;
    }
    if (int result = testReadSeqEnable(sourcePath); result != 0) {
        return result;
    }
    if (int result = testReadSeqSelfHold(sourcePath); result != 0) {
        return result;
    }
    if (int result = testReadElementBit(sourcePath); result != 0) {
        return result;
    }
    if (int result = testReadCast(sourcePath); result != 0) {
        return result;
    }
    if (int result = testPackedAggregateWholeReg(sourcePath); result != 0) {
        return result;
    }
    if (int result = testPackedAggregateIndexedMem(sourcePath); result != 0) {
        return result;
    }
    if (int result = testPackedAggregatePriorityRowWrite(sourcePath); result != 0) {
        return result;
    }
    if (int result = testMaskedWrite(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicUp(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicDown(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicParamWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicExprWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicPkgWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicPkgQualified(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicExprComplex(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicConcatWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicConcatExprWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicReplWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicReplExprWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicBaseWarning(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteMultiDim(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteMultiDimOffset(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicBadWidth(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicOutOfBoundsUp(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteDynamicOutOfBoundsDown(sourcePath); result != 0) {
        return result;
    }
    if (int result = testWriteRangeOutOfBounds(sourcePath); result != 0) {
        return result;
    }
    return 0;
}
