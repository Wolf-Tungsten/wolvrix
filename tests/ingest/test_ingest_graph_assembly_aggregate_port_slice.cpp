#include "ingest/sv.hpp"
#include "core/transform.hpp"
#include "transform/activity_schedule.hpp"
#include "transform/hier_flatten.hpp"
#include "transform/simplify.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-aggregate-port-slice] " << message << '\n';
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
    argStorage.emplace_back("ingest-graph-assembly-aggregate-port-slice");
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

std::string diagnosticsText(const wolvrix::lib::transform::PassDiagnostics& diags) {
    std::ostringstream out;
    for (const auto& message : diags.messages()) {
        out << message.message << ';';
    }
    return out.str();
}

std::optional<int64_t> intAttr(const wolvrix::lib::grh::Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<int64_t>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::string locText(const wolvrix::lib::grh::Operation& op) {
    const auto& loc = op.srcLoc();
    if (!loc) {
        return "<no-src-loc>";
    }
    std::ostringstream out;
    out << loc->file << ':' << loc->line << ':' << loc->column;
    return out.str();
}

int assertNoDynamicSlices(const wolvrix::lib::grh::Design& design) {
    for (const auto& entry : design.graphs()) {
        const wolvrix::lib::grh::Graph& graph = *entry.second;
        for (wolvrix::lib::grh::OperationId opId : graph.operations()) {
            const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kSliceDynamic) {
                return fail("Expected constant packed lane selects to emit kSliceStatic, found kSliceDynamic in " +
                            graph.symbol() + " at " + locText(op));
            }
        }
    }
    return 0;
}

bool hasStaticSlice(const wolvrix::lib::grh::Graph& graph, int64_t start, int64_t end) {
    for (wolvrix::lib::grh::OperationId opId : graph.operations()) {
        const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kSliceStatic) {
            continue;
        }
        const auto sliceStart = intAttr(op, "sliceStart");
        const auto sliceEnd = intAttr(op, "sliceEnd");
        if (sliceStart && sliceEnd && *sliceStart == start && *sliceEnd == end) {
            return true;
        }
    }
    return false;
}

std::string describeStaticSlices(const wolvrix::lib::grh::Graph& graph) {
    std::ostringstream out;
    bool first = true;
    for (wolvrix::lib::grh::OperationId opId : graph.operations()) {
        const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kSliceStatic) {
            continue;
        }
        const auto sliceStart = intAttr(op, "sliceStart");
        const auto sliceEnd = intAttr(op, "sliceEnd");
        if (!first) {
            out << ", ";
        }
        first = false;
        if (sliceStart && sliceEnd) {
            out << '[' << *sliceStart << ':' << *sliceEnd << ']';
        } else {
            out << "[missing attrs]";
        }
    }
    return first ? "<none>" : out.str();
}

int testGraphAssemblyAggregatePortSlice(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_aggregate_port_slice");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        std::ostringstream out;
        for (const auto& message : driver.diagnostics().messages()) {
            out << message.message << ';';
        }
        return fail("Unexpected ingest diagnostics: " + out.str());
    }

    if (design.topGraphs().size() != 1 ||
        design.topGraphs().front() != "graph_assembly_aggregate_port_slice") {
        return fail("Unexpected top graph after ingest");
    }
    if (int status = assertNoDynamicSlices(design); status != 0) {
        return status;
    }
    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("graph_assembly_aggregate_port_slice");
    if (!graph) {
        return fail("Missing graph_assembly_aggregate_port_slice graph after ingest");
    }
    if (!hasStaticSlice(*graph, 12, 17)) {
        return fail("Missing top kSliceStatic [12:17] for packed_req_net[2'h2]; saw " +
                    describeStaticSlices(*graph));
    }

    wolvrix::lib::transform::SessionStore session;
    wolvrix::lib::transform::PassManager manager;
    manager.options().session = &session;
    manager.addPass(std::make_unique<wolvrix::lib::transform::HierFlattenPass>());
    manager.addPass(std::make_unique<wolvrix::lib::transform::SimplifyPass>());
    manager.addPass(std::make_unique<wolvrix::lib::transform::ActivitySchedulePass>(
        wolvrix::lib::transform::ActivityScheduleOptions{
            .path = "graph_assembly_aggregate_port_slice",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));

    wolvrix::lib::transform::PassDiagnostics diags;
    const wolvrix::lib::transform::PassManagerResult result = manager.run(design, diags);
    if (!result.success || diags.hasError()) {
        return fail("Expected flattened aggregate port slice schedule to be acyclic; diagnostics=" +
                    diagnosticsText(diags));
    }

    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath =
        WOLF_SV_INGEST_GRAPH_ASSEMBLY_AGGREGATE_PORT_SLICE_DATA_PATH;
    return testGraphAssemblyAggregatePortSlice(sourcePath);
}
