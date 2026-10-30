#include "ingest/sv.hpp"

#include <filesystem>
#include <initializer_list>
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
    std::cerr << "[ingest-type-resolver] " << message << '\n';
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
    driver.options.compilationFlags.at(slang::ast::CompilationFlags::AllowTopLevelIfacePorts) = true;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-type-resolver");
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

const wolvrix::lib::ingest::PortInfo* findPort(const wolvrix::lib::ingest::ModulePlan& plan,
                                         std::string_view name) {
    const wolvrix::lib::ingest::PlanSymbolId id = plan.symbolTable.lookup(name);
    if (!id.valid()) {
        return nullptr;
    }
    for (const auto& port : plan.ports) {
        if (port.symbol.index == id.index) {
            return &port;
        }
    }
    return nullptr;
}

const wolvrix::lib::ingest::SignalInfo* findSignal(const wolvrix::lib::ingest::ModulePlan& plan,
                                             std::string_view name) {
    const wolvrix::lib::ingest::PlanSymbolId id = plan.symbolTable.lookup(name);
    if (!id.valid()) {
        return nullptr;
    }
    for (const auto& signal : plan.signals) {
        if (signal.symbol.index == id.index) {
            return &signal;
        }
    }
    return nullptr;
}

bool matchesDims(const std::vector<int32_t>& actual, std::initializer_list<int32_t> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    std::size_t idx = 0;
    for (int32_t value : expected) {
        if (actual[idx] != value) {
            return false;
        }
        ++idx;
    }
    return true;
}

bool matchesUnpackedDims(const std::vector<wolvrix::lib::ingest::UnpackedDimInfo>& actual,
                         std::initializer_list<int32_t> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    std::size_t idx = 0;
    for (int32_t value : expected) {
        if (actual[idx].extent != value) {
            return false;
        }
        ++idx;
    }
    return true;
}

int testPackedPorts(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "top_module");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to build compilation for " + sourcePath.string());
    }

    auto& compilation = *bundle->compilation;
    const slang::ast::RootSymbol& root = compilation.getRoot();
    const slang::ast::InstanceSymbol* top = findTopInstance(compilation, root, "top_module");
    if (!top) {
        return fail("Top instance not found for " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
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
    wolvrix::lib::ingest::ModulePlan plan = planner.plan(top->body);

    const auto* in = findPort(plan, "in");
    if (!in || in->width != 100 || in->isSigned) {
        return fail("Expected input 'in' width=100 unsigned in " + sourcePath.string());
    }
    const auto* outBoth = findPort(plan, "out_both");
    if (!outBoth || outBoth->width != 99 || outBoth->isSigned) {
        return fail("Expected output 'out_both' width=99 unsigned in " + sourcePath.string());
    }
    const auto* outAny = findPort(plan, "out_any");
    if (!outAny || outAny->width != 99 || outAny->isSigned) {
        return fail("Expected output 'out_any' width=99 unsigned in " + sourcePath.string());
    }
    const auto* outDiff = findPort(plan, "out_different");
    if (!outDiff || outDiff->width != 100 || outDiff->isSigned) {
        return fail("Expected output 'out_different' width=100 unsigned in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testPackedSignalDims(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "top_module");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to build compilation for " + sourcePath.string());
    }

    auto& compilation = *bundle->compilation;
    const slang::ast::RootSymbol& root = compilation.getRoot();
    const slang::ast::InstanceSymbol* top = findTopInstance(compilation, root, "top_module");
    if (!top) {
        return fail("Top instance not found for " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
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
    wolvrix::lib::ingest::ModulePlan plan = planner.plan(top->body);

    const auto* y = findPort(plan, "y");
    if (!y || y->width != 3) {
        return fail("Expected input 'y' width=3 in " + sourcePath.string());
    }
    const auto* y2 = findPort(plan, "Y2");
    if (!y2 || y2->width != 1) {
        return fail("Expected output 'Y2' width=1 in " + sourcePath.string());
    }

    const auto* signal = findSignal(plan, "Y");
    if (!signal) {
        return fail("Expected signal 'Y' in " + sourcePath.string());
    }
    if (signal->width != 3 || signal->isSigned) {
        return fail("Expected signal 'Y' width=3 unsigned in " + sourcePath.string());
    }
    if (!matchesDims(signal->packedDims, {3}) || !signal->unpackedDims.empty()) {
        return fail("Expected signal 'Y' packedDims={3} and no unpackedDims in " +
                    sourcePath.string());
    }
    if (signal->memoryRows != 0) {
        return fail("Expected signal 'Y' memoryRows=0 in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testMemoryDims(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "top_module");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to build compilation for " + sourcePath.string());
    }

    auto& compilation = *bundle->compilation;
    const slang::ast::RootSymbol& root = compilation.getRoot();
    const slang::ast::InstanceSymbol* top = findTopInstance(compilation, root, "top_module");
    if (!top) {
        return fail("Top instance not found for " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDiagnostics diagnostics;
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
    wolvrix::lib::ingest::ModulePlan plan = planner.plan(top->body);

    const auto* pht = findSignal(plan, "PHT");
    if (!pht) {
        return fail("Expected signal 'PHT' in " + sourcePath.string());
    }
    if (pht->width != 2 || pht->memoryRows != 128) {
        return fail("Expected signal 'PHT' width=2 memoryRows=128 in " + sourcePath.string());
    }
    if (!matchesDims(pht->packedDims, {2}) || !matchesUnpackedDims(pht->unpackedDims, {128})) {
        return fail("Expected signal 'PHT' packedDims={2} unpackedDims={128} in " +
                    sourcePath.string());
    }

    const auto* ghr = findSignal(plan, "GHR");
    if (!ghr) {
        return fail("Expected signal 'GHR' in " + sourcePath.string());
    }
    if (ghr->width != 7 || ghr->memoryRows != 0) {
        return fail("Expected signal 'GHR' width=7 memoryRows=0 in " + sourcePath.string());
    }
    if (!matchesDims(ghr->packedDims, {7})) {
        return fail("Expected signal 'GHR' packedDims={7} in " + sourcePath.string());
    }

    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path dutDir =
        std::filesystem::path(WOLF_SV_INGEST_HDLBITS_DUT_DIR);
    const std::filesystem::path packedPortsPath = dutDir / "dut_060.v";
    const std::filesystem::path packedSignalPath = dutDir / "dut_145.v";
    const std::filesystem::path memoryPath = dutDir / "dut_162.v";

    if (!std::filesystem::exists(packedPortsPath) ||
        !std::filesystem::exists(packedSignalPath) ||
        !std::filesystem::exists(memoryPath)) {
        return fail("Missing HDLBits input files in " + dutDir.string());
    }

    if (int result = testPackedPorts(packedPortsPath); result != 0) {
        return result;
    }
    if (int result = testPackedSignalDims(packedSignalPath); result != 0) {
        return result;
    }
    if (int result = testMemoryDims(memoryPath); result != 0) {
        return result;
    }
    return 0;
}
