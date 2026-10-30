#include "ingest/sv.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-instance-dedup] " << message << '\n';
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
    argStorage.emplace_back("ingest-graph-assembly-instance-dedup");
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

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::string>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

int testGraphAssemblyInstanceDedup(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_instance_dedup");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        return fail("Unexpected diagnostics while converting " + sourcePath.string());
    }

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_instance_dedup") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_instance_dedup");
    if (!graph) {
        return fail("Missing graph_assembly_instance_dedup graph");
    }

    std::unordered_map<std::string, std::string> instanceModules;
    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance) {
            continue;
        }

        auto moduleName = getAttrString(op, "moduleName");
        auto instanceName = getAttrString(op, "instanceName");
        if (!moduleName || !instanceName) {
            return fail("Instance op missing moduleName/instanceName");
        }
        instanceModules.emplace(*instanceName, *moduleName);
    }

    if (instanceModules.size() != 6) {
        return fail("Expected 6 instances in graph_assembly_instance_dedup");
    }

    const auto dff0 = instanceModules.find("u_dff0");
    const auto dff1 = instanceModules.find("u_dff1");
    const auto dff2 = instanceModules.find("u_dff2");
    if (dff0 == instanceModules.end() || dff1 == instanceModules.end() ||
        dff2 == instanceModules.end()) {
        return fail("Missing my_dff8 instances in graph_assembly_instance_dedup");
    }
    if (dff0->second != "my_dff8" || dff1->second != "my_dff8" ||
        dff2->second != "my_dff8") {
        return fail("Expected my_dff8 instances to share moduleName");
    }

    const auto p0 = instanceModules.find("u_param0");
    const auto p1 = instanceModules.find("u_param1");
    const auto p2 = instanceModules.find("u_param2");
    if (p0 == instanceModules.end() || p1 == instanceModules.end() ||
        p2 == instanceModules.end()) {
        return fail("Missing my_param instances in graph_assembly_instance_dedup");
    }
    if (p0->second != p1->second) {
        return fail("Expected same-parameter instances to share moduleName");
    }
    if (p2->second == p0->second) {
        return fail("Expected different-parameter instances to use distinct moduleName");
    }

    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_INSTANCE_DEDUP_DATA_PATH;
    return testGraphAssemblyInstanceDedup(sourcePath);
}
