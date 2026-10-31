#include "ingest/sv.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-instance] " << message << '\n';
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
    argStorage.emplace_back("ingest-graph-assembly-instance");
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

bool expectStrings(const std::optional<std::vector<std::string>>& got,
                   const std::vector<std::string>& expected) {
    if (!got) {
        return false;
    }
    return *got == expected;
}

int testGraphAssemblyInstance(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_instance");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    std::string diagSummary;
    if (!driver.diagnostics().empty()) {
        diagSummary = " diagnostics=";
        for (const auto& message : driver.diagnostics().messages()) {
            diagSummary.append(message.message);
            diagSummary.push_back(';');
        }
    }

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_instance") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_instance");
    if (!graph) {
        return fail("Missing graph_assembly_instance graph");
    }

    bool childOk = false;
    bool inoutOk = false;
    bool blackboxOk = false;
    int childCount = 0;
    int inoutCount = 0;
    int blackboxCount = 0;
    std::vector<std::string> seenOps;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kInstance: {
            auto moduleName = getAttrString(op, "moduleName");
            if (!moduleName) {
                return fail("Instance op missing moduleName attribute");
            }
            seenOps.push_back("kInstance:" + *moduleName);
            if (*moduleName == "child") {
                ++childCount;
                if (!expectStrings(getAttrStrings(op, "inputPortName"), {"a"})) {
                    return fail("child instance inputPortName mismatch");
                }
                if (!expectStrings(getAttrStrings(op, "outputPortName"), {"y"})) {
                    return fail("child instance outputPortName mismatch");
                }
                if (!expectStrings(getAttrStrings(op, "inoutPortName"), {})) {
                    return fail("child instance inoutPortName mismatch");
                }
                if (op.operands().size() != 1 || op.results().size() != 1) {
                    return fail("child instance operand/result count mismatch");
                }
                auto instanceName = getAttrString(op, "instanceName");
                if (!instanceName || *instanceName != "u_child") {
                    return fail("child instanceName mismatch");
                }
                childOk = true;
            } else if (*moduleName == "child_inout") {
                ++inoutCount;
                if (!expectStrings(getAttrStrings(op, "inputPortName"), {"a"})) {
                    return fail("child_inout inputPortName mismatch");
                }
                if (!expectStrings(getAttrStrings(op, "outputPortName"), {"y"})) {
                    return fail("child_inout outputPortName mismatch");
                }
                if (!expectStrings(getAttrStrings(op, "inoutPortName"), {"io"})) {
                    return fail("child_inout inoutPortName mismatch");
                }
                if (op.operands().size() != 2 || op.results().size() != 3) {
                    return fail("child_inout operand/result count mismatch");
                }
                auto instanceName = getAttrString(op, "instanceName");
                if (!instanceName || *instanceName != "u_child_inout") {
                    return fail("child_inout instanceName mismatch");
                }
                inoutOk = true;
            } else {
                return fail("Unexpected kInstance moduleName: " + *moduleName);
            }
            break;
        }
        case wolvrix::lib::grh::OperationKind::kBlackbox: {
            auto moduleName = getAttrString(op, "moduleName");
            if (!moduleName) {
                return fail("Blackbox op missing moduleName attribute");
            }
            seenOps.push_back("kBlackbox:" + *moduleName);
            ++blackboxCount;
            if (*moduleName != "bb") {
                return fail("blackbox moduleName mismatch");
            }
            if (!expectStrings(getAttrStrings(op, "inputPortName"), {"din"})) {
                return fail("blackbox inputPortName mismatch");
            }
            if (!expectStrings(getAttrStrings(op, "outputPortName"), {"dout"})) {
                return fail("blackbox outputPortName mismatch");
            }
            if (op.operands().size() != 1 || op.results().size() != 1) {
                return fail("blackbox operand/result count mismatch");
            }
            if (!expectStrings(getAttrStrings(op, "parameterNames"), {"WIDTH"})) {
                return fail("blackbox parameterNames mismatch");
            }
            if (!expectStrings(getAttrStrings(op, "parameterValues"), {"32'sb00000000000000000000000000000100"})) {
                return fail("blackbox parameterValues mismatch");
            }
            auto instanceName = getAttrString(op, "instanceName");
            if (!instanceName || *instanceName != "u_bb") {
                return fail("blackbox instanceName mismatch");
            }
            blackboxOk = true;
            break;
        }
        default:
            break;
        }
    }

    if (childCount != 1 || inoutCount != 1 || blackboxCount != 1) {
        std::string message = "Unexpected instance/blackbox counts in graph_assembly_instance";
        message.append(" (child=");
        message.append(std::to_string(childCount));
        message.append(", child_inout=");
        message.append(std::to_string(inoutCount));
        message.append(", blackbox=");
        message.append(std::to_string(blackboxCount));
        message.append(") ops=");
        for (const auto& name : seenOps) {
            message.append(name);
            message.push_back(' ');
        }
        message.append(diagSummary);
        return fail(message);
    }
    if (!childOk || !inoutOk || !blackboxOk) {
        return fail("Missing expected instance/blackbox operations");
    }

    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_INSTANCE_DATA_PATH;
    return testGraphAssemblyInstance(sourcePath);
}
