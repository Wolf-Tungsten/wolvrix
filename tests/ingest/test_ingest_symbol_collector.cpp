#include "ingest/sv.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/ast/symbols/InstanceSymbols.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-symbol-collector] " << message << '\n';
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
    argStorage.emplace_back("ingest-symbol-collector");
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

bool hasPort(const wolvrix::lib::ingest::ModulePlan& plan, std::string_view name,
             wolvrix::lib::ingest::PortDirection direction) {
    for (const auto& port : plan.ports) {
        if (plan.symbolTable.text(port.symbol) == name && port.direction == direction) {
            return true;
        }
    }
    return false;
}

bool hasSignal(const wolvrix::lib::ingest::ModulePlan& plan, std::string_view name,
               wolvrix::lib::ingest::SignalKind kind) {
    for (const auto& signal : plan.signals) {
        if (plan.symbolTable.text(signal.symbol) == name && signal.kind == kind) {
            return true;
        }
    }
    return false;
}

bool hasInstance(const wolvrix::lib::ingest::ModulePlan& plan, std::string_view instanceName,
                 std::string_view moduleName) {
    for (const auto& instance : plan.instances) {
        if (plan.symbolTable.text(instance.instanceSymbol) == instanceName &&
            plan.symbolTable.text(instance.moduleSymbol) == moduleName) {
            return true;
        }
    }
    return false;
}

std::string describePorts(const wolvrix::lib::ingest::ModulePlan& plan) {
    std::string result;
    for (const auto& port : plan.ports) {
        if (!result.empty()) {
            result.append(", ");
        }
        std::string_view name = plan.symbolTable.text(port.symbol);
        std::string_view dir = "unknown";
        switch (port.direction) {
        case wolvrix::lib::ingest::PortDirection::Input:
            dir = "input";
            break;
        case wolvrix::lib::ingest::PortDirection::Output:
            dir = "output";
            break;
        case wolvrix::lib::ingest::PortDirection::Inout:
            dir = "inout";
            break;
        }
        result.append(std::string(name)).append("(").append(dir).append(")");
    }
    if (result.empty()) {
        result = "<none>";
    }
    return result;
}

std::string describeAstPorts(const slang::ast::InstanceBodySymbol& body) {
    std::string result;
    for (const slang::ast::Symbol* portSymbol : body.getPortList()) {
        if (!portSymbol) {
            continue;
        }
        if (!result.empty()) {
            result.append(", ");
        }
        if (const auto* port = portSymbol->as_if<slang::ast::PortSymbol>()) {
            std::string_view dir = slang::ast::toString(port->direction);
            result.append(std::string(port->name)).append("(").append(dir).append(")");
        }
        else if (const auto* multi = portSymbol->as_if<slang::ast::MultiPortSymbol>()) {
            std::string_view dir = slang::ast::toString(multi->direction);
            result.append(std::string(multi->name)).append("(multi ").append(dir).append(")");
        }
        else if (const auto* iface = portSymbol->as_if<slang::ast::InterfacePortSymbol>()) {
            result.append(std::string(iface->name)).append("(iface)");
        }
        else {
            result.append(std::string(portSymbol->name)).append("(unknown)");
        }
    }
    if (result.empty()) {
        result = "<none>";
    }
    return result;
}

int testPorts(const std::filesystem::path& sourcePath) {
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

    if (!hasPort(plan, "in", wolvrix::lib::ingest::PortDirection::Input)) {
        return fail("Expected input port 'in' in " + sourcePath.string() +
                    "; ports=" + describePorts(plan) +
                    "; ast_ports=" + describeAstPorts(top->body));
    }
    if (!hasPort(plan, "out", wolvrix::lib::ingest::PortDirection::Output)) {
        return fail("Expected output port 'out' in " + sourcePath.string() +
                    "; ports=" + describePorts(plan) +
                    "; ast_ports=" + describeAstPorts(top->body));
    }
    if (!plan.instances.empty()) {
        return fail("Did not expect child instances in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testSignals(const std::filesystem::path& sourcePath) {
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

    if (!hasSignal(plan, "counter", wolvrix::lib::ingest::SignalKind::Variable)) {
        return fail("Expected internal variable 'counter' in " + sourcePath.string());
    }
    if (diagnostics.hasError()) {
        return fail("Unexpected Convert diagnostics errors in " + sourcePath.string());
    }
    return 0;
}

int testInstances(const std::filesystem::path& sourcePath) {
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

    if (!hasInstance(plan, "inst", "mod_a")) {
        return fail("Expected child instance inst:mod_a in " + sourcePath.string());
    }
    if (plan.instances.size() != 1) {
        return fail("Expected exactly one child instance in " + sourcePath.string());
    }

    wolvrix::lib::ingest::PlanKey key;
    std::size_t queueCount = 0;
    bool foundChild = false;
    while (planQueue.tryPop(key)) {
        ++queueCount;
        if (key.body && key.body->getDefinition().name == "mod_a") {
            foundChild = true;
        }
    }
    if (!foundChild) {
        return fail("Expected mod_a PlanKey enqueued in " + sourcePath.string());
    }
    if (queueCount != 1) {
        return fail("Expected exactly one queued PlanKey in " + sourcePath.string());
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
    const std::filesystem::path portsPath = dutDir / "dut_003.v";
    const std::filesystem::path signalsPath = dutDir / "dut_159.v";
    const std::filesystem::path instancesPath = dutDir / "dut_020.v";

    if (!std::filesystem::exists(portsPath) ||
        !std::filesystem::exists(signalsPath) ||
        !std::filesystem::exists(instancesPath)) {
        return fail("Missing HDLBits input files in " + dutDir.string());
    }

    if (int result = testPorts(portsPath); result != 0) {
        return result;
    }
    if (int result = testSignals(signalsPath); result != 0) {
        return result;
    }
    if (int result = testInstances(instancesPath); result != 0) {
        return result;
    }
    return 0;
}
