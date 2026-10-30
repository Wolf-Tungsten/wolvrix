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
    std::cerr << "[ingest-graph-assembly-basic] " << message << '\n';
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
    argStorage.emplace_back("ingest-graph-assembly-basic");
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

bool hasPort(std::span<const wolvrix::lib::grh::Port> ports, std::string_view name) {
    for (const auto& port : ports) {
        if (port.name == name) {
            return true;
        }
    }
    return false;
}

int testGraphAssemblyBasic(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_basic");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_basic") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_basic");
    if (!graph) {
        return fail("Missing graph_assembly_basic graph");
    }

    if (!hasPort(graph->inputPorts(), "clk") ||
        !hasPort(graph->inputPorts(), "a") ||
        !hasPort(graph->inputPorts(), "b") ||
        !hasPort(graph->inputPorts(), "en")) {
        return fail("Missing expected input ports");
    }

    if (!hasPort(graph->outputPorts(), "y") ||
        !hasPort(graph->outputPorts(), "q") ||
        !hasPort(graph->outputPorts(), "l")) {
        return fail("Missing expected output ports");
    }

    bool hasAssign = false;
    int regDecls = 0;
    int regReads = 0;
    int regWrites = 0;
    int latchDecls = 0;
    int latchReads = 0;
    int latchWrites = 0;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kAssign:
            hasAssign = true;
            break;
        case wolvrix::lib::grh::OperationKind::kRegister:
            ++regDecls;
            if (!op.operands().empty() || !op.results().empty()) {
                return fail("kRegister should not have operands or results");
            }
            if (!op.attr("width") || !op.attr("isSigned")) {
                return fail("kRegister missing width/isSigned attributes");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            ++regReads;
            if (!op.operands().empty() || op.results().size() != 1) {
                return fail("kRegisterReadPort should have 0 operands and 1 result");
            }
            if (!op.attr("regSymbol")) {
                return fail("kRegisterReadPort missing regSymbol attribute");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            ++regWrites;
            if (op.operands().size() < 3 || !op.results().empty()) {
                return fail("kRegisterWritePort missing operands or has results");
            }
            if (!op.attr("regSymbol") || !op.attr("eventEdge")) {
                return fail("kRegisterWritePort missing regSymbol/eventEdge attributes");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kLatch:
            ++latchDecls;
            if (!op.operands().empty() || !op.results().empty()) {
                return fail("kLatch should not have operands or results");
            }
            if (!op.attr("width") || !op.attr("isSigned")) {
                return fail("kLatch missing width/isSigned attributes");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            ++latchReads;
            if (!op.operands().empty() || op.results().size() != 1) {
                return fail("kLatchReadPort should have 0 operands and 1 result");
            }
            if (!op.attr("latchSymbol")) {
                return fail("kLatchReadPort missing latchSymbol attribute");
            }
            break;
        case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            ++latchWrites;
            if (op.operands().size() < 3 || !op.results().empty()) {
                return fail("kLatchWritePort missing operands or has results");
            }
            if (!op.attr("latchSymbol")) {
                return fail("kLatchWritePort missing latchSymbol attribute");
            }
            break;
        default:
            break;
        }
    }

    if (!hasAssign) {
        return fail("Missing expected assign operation");
    }
    if (regDecls != 1 || regReads != 1 || regWrites != 1) {
        return fail("Unexpected register declaration/read/write port count");
    }
    if (latchDecls != 1 || latchReads != 1 || latchWrites != 1) {
        return fail("Unexpected latch declaration/read/write port count");
    }

    return 0;
}

int testGraphAssemblyProceduralLocal(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_procedural_local");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("graph_assembly_procedural_local");
    if (!graph) {
        return fail("Missing graph_assembly_procedural_local graph");
    }

    if (!hasPort(graph->inputPorts(), "clk") ||
        !hasPort(graph->inputPorts(), "a") ||
        !hasPort(graph->inputPorts(), "b") ||
        !hasPort(graph->inputPorts(), "en") ||
        !hasPort(graph->outputPorts(), "y")) {
        return fail("Missing expected procedural local ports");
    }

    int regWrites = 0;
    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() == wolvrix::lib::grh::OperationKind::kRegisterWritePort) {
            ++regWrites;
        }
    }
    if (regWrites != 1) {
        return fail("Expected exactly one register write for procedural local case");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_GRAPH_ASSEMBLY_DATA_PATH;
    if (int status = testGraphAssemblyBasic(sourcePath); status != 0) {
        return status;
    }
    return testGraphAssemblyProceduralLocal(sourcePath);
}
