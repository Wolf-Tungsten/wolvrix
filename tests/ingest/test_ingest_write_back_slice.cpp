#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/numeric/SVInt.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-write-back-slice] " << message << '\n';
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
    argStorage.emplace_back("ingest-write-back-slice");
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
                        wolvrix::lib::ingest::WriteBackPlan& outWriteBack,
                        bool lowerMemoryPorts = false) {
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
    wolvrix::lib::ingest::MemoryPortLowererPass memoryPortLowerer(context);

    outPlan = planner.plan(top->body);
    outLowering = {};
    stmtLowerer.lower(outPlan, outLowering);
    if (lowerMemoryPorts) {
        memoryPortLowerer.lower(outPlan, outLowering);
    }
    outWriteBack = writeBack.lower(outPlan, outLowering);
    return true;
}

const wolvrix::lib::grh::Port* findOutputPort(const wolvrix::lib::grh::Graph& graph,
                                              std::string_view name) {
    for (const auto& port : graph.outputPorts()) {
        if (port.name == name) {
            return &port;
        }
    }
    return nullptr;
}

const wolvrix::lib::grh::Port* findInputPort(const wolvrix::lib::grh::Graph& graph,
                                             std::string_view name) {
    for (const auto& port : graph.inputPorts()) {
        if (port.name == name) {
            return &port;
        }
    }
    return nullptr;
}

std::optional<int64_t> constLogicInt(const wolvrix::lib::grh::Graph& graph,
                                     wolvrix::lib::grh::ValueId value) {
    if (!value.valid()) {
        return std::nullopt;
    }
    const wolvrix::lib::grh::OperationId defId = graph.getValue(value).definingOp();
    if (!defId.valid()) {
        return std::nullopt;
    }
    const wolvrix::lib::grh::Operation op = graph.getOperation(defId);
    if (op.kind() != wolvrix::lib::grh::OperationKind::kConstant) {
        return std::nullopt;
    }
    const auto attr = op.attr("constValue");
    if (!attr) {
        return std::nullopt;
    }
    const auto* text = std::get_if<std::string>(&*attr);
    if (!text) {
        return std::nullopt;
    }
    try {
        slang::SVInt parsed = slang::SVInt::fromString(*text);
        if (parsed.hasUnknown()) {
            return std::nullopt;
        }
        if (auto valueInt = parsed.as<int64_t>()) {
            return *valueInt;
        }
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation& op,
                                                       std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<std::string>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::vector<int64_t>> getAttrInts(const wolvrix::lib::grh::Operation& op,
                                                std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::vector<int64_t>>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<int64_t> parseLogicLiteralInt(const std::string& text) {
    try {
        slang::SVInt parsed = slang::SVInt::fromString(text);
        if (parsed.hasUnknown()) {
            return std::nullopt;
        }
        if (auto value = parsed.as<int64_t>()) {
            return *value;
        }
    }
    catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

void collectMemoryReadAddresses(const wolvrix::lib::grh::Graph& graph,
                                wolvrix::lib::grh::ValueId value,
                                std::vector<std::optional<int64_t>>& addresses,
                                std::unordered_set<wolvrix::lib::grh::ValueId,
                                                   wolvrix::lib::grh::ValueIdHash>& seen) {
    if (!value.valid() || !seen.insert(value).second) {
        return;
    }
    const wolvrix::lib::grh::OperationId defId = graph.getValue(value).definingOp();
    if (!defId.valid()) {
        return;
    }
    const wolvrix::lib::grh::Operation op = graph.getOperation(defId);
    if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort) {
        if (op.operands().empty()) {
            addresses.push_back(std::nullopt);
        }
        else {
            addresses.push_back(constLogicInt(graph, op.operands().front()));
        }
        return;
    }
    for (wolvrix::lib::grh::ValueId operand : op.operands()) {
        collectMemoryReadAddresses(graph, operand, addresses, seen);
    }
}

int expectSingleMemoryReadAddress(const wolvrix::lib::grh::Graph& graph,
                                  std::string_view outputName,
                                  int64_t expectedAddress) {
    const wolvrix::lib::grh::Port* port = findOutputPort(graph, outputName);
    if (!port || !port->value.valid()) {
        return fail("Missing output port " + std::string(outputName));
    }
    std::vector<std::optional<int64_t>> addresses;
    std::unordered_set<wolvrix::lib::grh::ValueId, wolvrix::lib::grh::ValueIdHash> seen;
    collectMemoryReadAddresses(graph, port->value, addresses, seen);
    if (addresses.size() != 1) {
        return fail(std::string(outputName) + " should depend on exactly one memory row read, saw " +
                    std::to_string(addresses.size()));
    }
    if (!addresses.front() || *addresses.front() != expectedAddress) {
        return fail(std::string(outputName) + " should read packed aggregate row " +
                    std::to_string(expectedAddress));
    }
    return 0;
}

bool hasOp(const wolvrix::lib::ingest::LoweringPlan& lowering, wolvrix::lib::grh::OperationKind kind) {
    for (const auto& node : lowering.values) {
        if (node.kind == wolvrix::lib::ingest::ExprNodeKind::Operation && node.op == kind) {
            return true;
        }
    }
    return false;
}

bool exprHasOpWidth(const wolvrix::lib::ingest::LoweringPlan& lowering,
                    wolvrix::lib::ingest::ExprNodeId root,
                    wolvrix::lib::grh::OperationKind kind,
                    int32_t widthHint) {
    if (root == wolvrix::lib::ingest::kInvalidPlanIndex) {
        return false;
    }
    std::unordered_set<wolvrix::lib::ingest::ExprNodeId> seen;
    std::vector<wolvrix::lib::ingest::ExprNodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const wolvrix::lib::ingest::ExprNodeId current = stack.back();
        stack.pop_back();
        if (current == wolvrix::lib::ingest::kInvalidPlanIndex ||
            current >= static_cast<wolvrix::lib::ingest::ExprNodeId>(lowering.values.size())) {
            continue;
        }
        if (!seen.insert(current).second) {
            continue;
        }
        const auto& node = lowering.values[current];
        if (node.kind == wolvrix::lib::ingest::ExprNodeKind::Operation) {
            if (node.op == kind && node.widthHint == widthHint) {
                return true;
            }
            for (wolvrix::lib::ingest::ExprNodeId operand : node.operands) {
                stack.push_back(operand);
            }
        }
    }
    return false;
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

bool exprUsesSymbol(const wolvrix::lib::ingest::LoweringPlan& lowering,
                    wolvrix::lib::ingest::ExprNodeId root,
                    wolvrix::lib::ingest::PlanSymbolId target) {
    if (root == wolvrix::lib::ingest::kInvalidPlanIndex || !target.valid()) {
        return false;
    }
    std::unordered_set<wolvrix::lib::ingest::ExprNodeId> seen;
    std::vector<wolvrix::lib::ingest::ExprNodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const wolvrix::lib::ingest::ExprNodeId current = stack.back();
        stack.pop_back();
        if (current == wolvrix::lib::ingest::kInvalidPlanIndex ||
            current >= static_cast<wolvrix::lib::ingest::ExprNodeId>(lowering.values.size())) {
            continue;
        }
        if (!seen.insert(current).second) {
            continue;
        }
        const auto& node = lowering.values[current];
        if (node.kind == wolvrix::lib::ingest::ExprNodeKind::Symbol &&
            node.symbol.index == target.index) {
            return true;
        }
        if (node.kind != wolvrix::lib::ingest::ExprNodeKind::Operation) {
            continue;
        }
        for (wolvrix::lib::ingest::ExprNodeId operand : node.operands) {
            stack.push_back(operand);
        }
    }
    return false;
}

const wolvrix::lib::ingest::WriteBackPlan::Entry* findEntryForSymbol(
    const wolvrix::lib::ingest::ModulePlan& plan,
    const wolvrix::lib::ingest::WriteBackPlan& writeBack,
    std::string_view name) {
    for (const auto& entry : writeBack.entries) {
        if (!entry.target.valid()) {
            continue;
        }
        if (plan.symbolTable.text(entry.target) == name) {
            return &entry;
        }
    }
    return nullptr;
}

int testWriteBackSliceStatic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_slice_static", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back slice plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 write-back entry for static slices in " + sourcePath.string());
    }
    if (hasWarningMessage(diagnostics, "Write-back merge with slices")) {
        return fail("Unexpected slice warning in " + sourcePath.string());
    }
    if (!hasOp(lowering, wolvrix::lib::grh::OperationKind::kConcat)) {
        return fail("Missing kConcat in static slice write-back");
    }
    if (!hasOp(lowering, wolvrix::lib::grh::OperationKind::kSliceDynamic)) {
        return fail("Missing kSliceDynamic in static slice write-back");
    }
    return 0;
}

int testWriteBackSliceDynamic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_slice_dynamic", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back dynamic slice plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 write-back entry for dynamic slices in " + sourcePath.string());
    }
    if (!hasOp(lowering, wolvrix::lib::grh::OperationKind::kShl)) {
        return fail("Missing kShl in dynamic slice write-back");
    }
    return 0;
}

int testWriteBackSliceMember(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_slice_member", diagnostics, plan, lowering,
                            writeBack)) {
        return fail("Failed to build write-back member slice plan for " + sourcePath.string());
    }

    if (writeBack.entries.size() != 1) {
        return fail("Expected 1 write-back entry for member slices in " + sourcePath.string());
    }
    if (!hasOp(lowering, wolvrix::lib::grh::OperationKind::kConcat)) {
        return fail("Missing kConcat in member slice write-back");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testWriteBackWholeSelfSliceAcyclic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_whole_self_slice_acyclic", diagnostics,
                            plan, lowering, writeBack)) {
        return fail("Failed to build write-back whole self-slice acyclic plan for " +
                    sourcePath.string());
    }

    const auto* entry = findEntryForSymbol(plan, writeBack, "y");
    if (!entry) {
        return fail("Missing y write-back entry for whole self-slice acyclic");
    }
    if (exprUsesSymbol(lowering, entry->nextValue, entry->target)) {
        return fail("Acyclic whole self-slice write-back still depends on its target");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in whole self-slice acyclic");
    }
    return 0;
}

int testWriteBackWholeSelfSliceTrueLoop(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_whole_self_slice_true_loop", diagnostics,
                            plan, lowering, writeBack)) {
        return fail("Failed to build write-back whole self-slice true-loop plan for " +
                    sourcePath.string());
    }

    const auto* entry = findEntryForSymbol(plan, writeBack, "y");
    if (!entry) {
        return fail("Missing y write-back entry for whole self-slice true loop");
    }
    if (!exprUsesSymbol(lowering, entry->nextValue, entry->target)) {
        return fail("True whole self-slice loop was incorrectly removed");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in whole self-slice true loop");
    }
    return 0;
}

int testWriteBackPackedArraySelfSliceAcyclic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_packed_array_self_slice_acyclic",
                            diagnostics, plan, lowering, writeBack)) {
        return fail("Failed to build write-back packed array self-slice acyclic plan for " +
                    sourcePath.string());
    }

    const auto* entry = findEntryForSymbol(plan, writeBack, "y");
    if (!entry) {
        return fail("Missing y write-back entry for packed array self-slice acyclic");
    }
    if (exprUsesSymbol(lowering, entry->nextValue, entry->target)) {
        return fail("Acyclic packed array self-slice write-back still depends on its target");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in packed array self-slice acyclic");
    }
    return 0;
}

int testWriteBackMutualSelfSliceAcyclic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_mutual_self_slice_acyclic",
                            diagnostics, plan, lowering, writeBack)) {
        return fail("Failed to build write-back mutual self-slice acyclic plan for " +
                    sourcePath.string());
    }

    const auto* aEntry = findEntryForSymbol(plan, writeBack, "a");
    const auto* bEntry = findEntryForSymbol(plan, writeBack, "b");
    if (!aEntry || !bEntry) {
        return fail("Missing a/b write-back entries for mutual self-slice acyclic");
    }
    if (exprUsesSymbol(lowering, aEntry->nextValue, aEntry->target) ||
        exprUsesSymbol(lowering, aEntry->nextValue, bEntry->target) ||
        exprUsesSymbol(lowering, bEntry->nextValue, aEntry->target) ||
        exprUsesSymbol(lowering, bEntry->nextValue, bEntry->target)) {
        return fail("Acyclic mutual self-slice write-back still depends on its SCC");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in mutual self-slice acyclic");
    }
    return 0;
}

int testWriteBackIndirectMutualSelfSliceAcyclic(const std::filesystem::path& sourcePath) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, "write_back_indirect_mutual_self_slice_acyclic",
                            diagnostics, plan, lowering, writeBack)) {
        return fail("Failed to build write-back indirect mutual self-slice acyclic plan for " +
                    sourcePath.string());
    }

    const auto* aEntry = findEntryForSymbol(plan, writeBack, "a");
    const auto* bEntry = findEntryForSymbol(plan, writeBack, "b");
    if (!aEntry || !bEntry) {
        return fail("Missing a/b write-back entries for indirect mutual self-slice acyclic");
    }
    if (exprUsesSymbol(lowering, aEntry->nextValue, aEntry->target) ||
        exprUsesSymbol(lowering, aEntry->nextValue, bEntry->target) ||
        exprUsesSymbol(lowering, bEntry->nextValue, aEntry->target) ||
        exprUsesSymbol(lowering, bEntry->nextValue, bEntry->target)) {
        return fail("Acyclic indirect mutual self-slice write-back still depends on its SCC");
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in indirect mutual self-slice acyclic");
    }
    return 0;
}

int testOldestArbiterResetPackedArray(const std::filesystem::path& sourcePath,
                                      std::string_view moduleName,
                                      int32_t priorityWidth) {
    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
    wolvrix::lib::ingest::ModulePlan plan;
    wolvrix::lib::ingest::LoweringPlan lowering;
    wolvrix::lib::ingest::WriteBackPlan writeBack;
    if (!buildWriteBackPlan(sourcePath, moduleName, diagnostics, plan, lowering, writeBack,
                            /*lowerMemoryPorts=*/true)) {
        return fail("Failed to build OldestArbiter reset write-back plan for " +
                    sourcePath.string());
    }

    const wolvrix::lib::ingest::PlanSymbolId prioritySymbol =
        plan.symbolTable.lookup("priorityVecReg");
    if (!prioritySymbol.valid()) {
        return fail("Missing priorityVecReg symbol for " + std::string(moduleName));
    }

    // stage-04: the whole-array reset write is a write port with empty
    // addressIndices (kRegisterWritePort), not a memory fill.
    const wolvrix::lib::ingest::MemoryWritePort* priorityWholeWrite = nullptr;
    for (const auto& write : lowering.memoryWrites) {
        if (write.memory.index == prioritySymbol.index && write.addressIndices.empty()) {
            priorityWholeWrite = &write;
            break;
        }
    }
    if (!priorityWholeWrite) {
        return fail("Missing priorityVecReg whole-array memory write for " + std::string(moduleName));
    }
    if (priorityWholeWrite->eventEdges.size() != 2 ||
        priorityWholeWrite->eventOperands.size() != 2) {
        return fail("priorityVecReg whole write should preserve clock/reset edge events for " +
                    std::string(moduleName));
    }
    if (exprUsesSymbol(lowering, priorityWholeWrite->data, prioritySymbol)) {
        return fail("priorityVecReg reset whole write still depends on itself for " +
                    std::string(moduleName));
    }
    if (!exprHasOpWidth(lowering, priorityWholeWrite->data,
                        wolvrix::lib::grh::OperationKind::kConcat, priorityWidth)) {
        return fail("priorityVecReg whole write is missing packed-array concat width " +
                    std::to_string(priorityWidth) + " for " + std::string(moduleName));
    }

    std::size_t priorityWrites = 0;
    for (const auto& write : lowering.memoryWrites) {
        if (write.memory.index != prioritySymbol.index || write.addressIndices.empty()) {
            continue;
        }
        ++priorityWrites;
        if (write.eventEdges.size() != 2 || write.eventOperands.size() != 2) {
            return fail("priorityVecReg element write should preserve clock/reset edge events for " +
                        std::string(moduleName));
        }
    }
    if (priorityWrites == 0) {
        return fail("Missing priorityVecReg element memory writes for " + std::string(moduleName));
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in OldestArbiter reset case");
    }
    return 0;
}

int testOldestArbiterResetGraphWholeRead(const std::filesystem::path& sourcePath,
                                         std::string_view moduleName,
                                         int32_t expectedRows) {
    auto bundle = compileInput(sourcePath, moduleName);
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile OldestArbiter reset graph case for " +
                    std::string(moduleName));
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph = design.findGraph(moduleName);
    if (!graph) {
        return fail("Missing graph for " + std::string(moduleName));
    }

    const wolvrix::lib::grh::Port* priorityPort = findOutputPort(*graph, "priority_flat");
    if (!priorityPort || !priorityPort->value.valid()) {
        return fail("Missing priority_flat output port for " + std::string(moduleName));
    }
    const wolvrix::lib::grh::OperationId assignId =
        graph->getValue(priorityPort->value).definingOp();
    if (!assignId.valid()) {
        return fail("priority_flat output has no defining op for " + std::string(moduleName));
    }
    const wolvrix::lib::grh::Operation assignOp = graph->getOperation(assignId);
    if (assignOp.kind() != wolvrix::lib::grh::OperationKind::kAssign ||
        assignOp.operands().size() != 1) {
        return fail("priority_flat output should be assigned from aggregate whole read for " +
                    std::string(moduleName));
    }
    const wolvrix::lib::grh::ValueId aggregateValue = assignOp.operands().front();
    const wolvrix::lib::grh::OperationId readId =
        graph->getValue(aggregateValue).definingOp();
    if (!readId.valid()) {
        return fail("priority_flat aggregate whole read has no defining op for " +
                    std::string(moduleName));
    }
    // stage-04: whole read of an array-state signal is a single
    // kRegisterReadPort, not a per-row concat.
    const wolvrix::lib::grh::Operation readOp = graph->getOperation(readId);
    if (readOp.kind() != wolvrix::lib::grh::OperationKind::kRegisterReadPort) {
        return fail("priority_flat aggregate whole read should be a register read port for " +
                    std::string(moduleName));
    }
    (void)expectedRows;
    return 0;
}

int testOldestArbiterInitialResetMemoryInit(const std::filesystem::path& sourcePath,
                                            std::string_view moduleName,
                                            const std::vector<int64_t>& expectedRows) {
    auto bundle = compileInput(sourcePath, moduleName);
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile OldestArbiter initial reset graph case for " +
                    std::string(moduleName));
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph = design.findGraph(moduleName);
    if (!graph) {
        return fail("Missing graph for " + std::string(moduleName));
    }

    const wolvrix::lib::grh::Port* resetPort = findInputPort(*graph, "reset");
    if (!resetPort || !resetPort->value.valid()) {
        return fail("reset input port should be preserved for " + std::string(moduleName));
    }

    std::optional<wolvrix::lib::grh::Operation> priorityMemory;
    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister &&
            op.symbolText() == std::string_view("reset")) {
            return fail("reset input should not be converted to kRegister for " +
                        std::string(moduleName));
        }
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegister &&
            op.symbolText() == std::string_view("priorityVecReg") &&
            getAttrInts(op, "shape")) {
            priorityMemory = op;
        }
    }
    if (!priorityMemory) {
        return fail("Missing priorityVecReg array register for " + std::string(moduleName));
    }

    auto kinds = getAttrStrings(*priorityMemory, "initKind");
    auto values = getAttrStrings(*priorityMemory, "initValue");
    auto starts = getAttrInts(*priorityMemory, "initStart");
    auto lens = getAttrInts(*priorityMemory, "initLen");
    if (!kinds || !values || !starts || !lens) {
        return fail("priorityVecReg missing array init attrs for " + std::string(moduleName));
    }
    if (kinds->size() != values->size() || values->size() != starts->size() ||
        starts->size() != lens->size()) {
        return fail("priorityVecReg array init attr size mismatch for " +
                    std::string(moduleName));
    }

    std::map<int64_t, int64_t> rows;
    for (std::size_t i = 0; i < values->size(); ++i) {
        if ((*kinds)[i] != "literal") {
            return fail("priorityVecReg init should be literal for " + std::string(moduleName));
        }
        auto parsed = parseLogicLiteralInt((*values)[i]);
        if (!parsed) {
            return fail("priorityVecReg init literal did not parse for " +
                        std::string(moduleName));
        }
        if ((*starts)[i] < 0) {
            for (std::size_t row = 0; row < expectedRows.size(); ++row) {
                rows[static_cast<int64_t>(row)] = *parsed;
            }
            continue;
        }
        if ((*lens)[i] != 1) {
            return fail("priorityVecReg per-row init should have len=1 for " +
                        std::string(moduleName));
        }
        rows[(*starts)[i]] = *parsed;
    }

    if (rows.size() != expectedRows.size()) {
        return fail("priorityVecReg array init row count mismatch for " +
                    std::string(moduleName));
    }
    for (std::size_t row = 0; row < expectedRows.size(); ++row) {
        auto it = rows.find(static_cast<int64_t>(row));
        if (it == rows.end() || it->second != expectedRows[row]) {
            return fail("priorityVecReg array init row mismatch for " +
                        std::string(moduleName));
        }
    }
    return 0;
}

int testPackedAggregateBitSelectGraph(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "PackedAggregateBitSelectCase011");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile packed aggregate bit-select graph case");
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    const wolvrix::lib::grh::Graph* graph = design.findGraph("PackedAggregateBitSelectCase011");
    if (!graph) {
        return fail("Missing graph for PackedAggregateBitSelectCase011");
    }

    if (int status = expectSingleMemoryReadAddress(*graph, "row0", 0); status != 0) {
        return status;
    }
    if (int status = expectSingleMemoryReadAddress(*graph, "bit00", 0); status != 0) {
        return status;
    }
    if (int status = expectSingleMemoryReadAddress(*graph, "bit01", 0); status != 0) {
        return status;
    }
    if (int status = expectSingleMemoryReadAddress(*graph, "row1", 1); status != 0) {
        return status;
    }
    if (int status = expectSingleMemoryReadAddress(*graph, "bit10", 1); status != 0) {
        return status;
    }
    return expectSingleMemoryReadAddress(*graph, "bit11", 1);
}

} // namespace

#ifndef WOLF_SV_XS_BUGCASE_CASE009_DATA_PATH
#error "WOLF_SV_XS_BUGCASE_CASE009_DATA_PATH must be defined"
#endif

#ifndef WOLF_SV_XS_BUGCASE_CASE011_DATA_PATH
#error "WOLF_SV_XS_BUGCASE_CASE011_DATA_PATH must be defined"
#endif

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_WRITE_BACK_SLICE_DATA_PATH;
    if (int status = testWriteBackSliceStatic(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackSliceDynamic(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackSliceMember(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackWholeSelfSliceAcyclic(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackWholeSelfSliceTrueLoop(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackPackedArraySelfSliceAcyclic(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackMutualSelfSliceAcyclic(sourcePath); status != 0) {
        return status;
    }
    if (int status = testWriteBackIndirectMutualSelfSliceAcyclic(sourcePath); status != 0) {
        return status;
    }
    const std::filesystem::path oldestArbiterResetPath = WOLF_SV_XS_BUGCASE_CASE009_DATA_PATH;
    if (int status = testOldestArbiterResetPackedArray(
            oldestArbiterResetPath, "OldestArbiterReset2Case009", 4);
        status != 0) {
        return status;
    }
    if (int status = testOldestArbiterResetPackedArray(
            oldestArbiterResetPath, "OldestArbiterReset3Case009", 9);
        status != 0) {
        return status;
    }
    if (int status = testOldestArbiterResetGraphWholeRead(
            oldestArbiterResetPath, "OldestArbiterReset2Case009", 2);
        status != 0) {
        return status;
    }
    if (int status = testOldestArbiterResetGraphWholeRead(
            oldestArbiterResetPath, "OldestArbiterReset3Case009", 3);
        status != 0) {
        return status;
    }
    if (int status = testOldestArbiterInitialResetMemoryInit(
            oldestArbiterResetPath, "OldestArbiterInitialReset2Case009", {1, 2});
        status != 0) {
        return status;
    }
    if (int status = testOldestArbiterInitialResetMemoryInit(
            oldestArbiterResetPath, "OldestArbiterInitialReset3Case009", {1, 2, 4});
        status != 0) {
        return status;
    }
    return testPackedAggregateBitSelectGraph(WOLF_SV_XS_BUGCASE_CASE011_DATA_PATH);
}
