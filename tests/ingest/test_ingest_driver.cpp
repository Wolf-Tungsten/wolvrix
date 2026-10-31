#include "ingest/firrtl.hpp"
#include "ingest/ingest.hpp"
#include "ingest/sv.hpp"

#include "slang/text/SourceManager.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace wolvrix::lib::ingest;

int fail(const std::string& message) {
    std::cerr << "[ingest-driver] " << message << '\n';
    return 1;
}

bool unsupported(const IngestResult& result) {
    return !result.success() && !result.design && !result.diagnostics.empty() &&
           result.diagnostics.front().kind == IngestSeverity::Error &&
           result.diagnostics.front().message.find("not supported yet") != std::string::npos;
}

} // namespace

int main() {
    IngestRequest request;
    request.inputRoot = WOLVRIX_INGEST_TEST_DATA_DIR;
    request.sources.push_back({SourceLanguage::SystemVerilog, "graph_assembly_basic.sv", "fixture"});
    request.tops = {"graph_assembly_basic"};
    request.logLevel = wolvrix::lib::LogLevel::Off;
    auto result = readSources(request);
    if (!result.success() || !result.design->findGraph("graph_assembly_basic") ||
        result.design->topGraphs() != std::vector<std::string>{"graph_assembly_basic"}) {
        return fail("public SV request did not elaborate the selected top");
    }
    SourceOwners owners = result.sourceOwners;
    const auto* sv = dynamic_cast<const SvSourceOwner*>(owners.front().get());
    if (!sv || !sv->compilation || !sv->driver || !svSourceManager(owners)) {
        return fail("SV result must own both compilation and driver/source manager");
    }
    auto clone = result.design->clone();
    result = {};
    const auto* sourceManager = svSourceManager(owners);
    bool hasSource = false;
    for (auto buffer : sourceManager->getAllBuffers()) {
        hasSource |= sourceManager->getSourceText(buffer).find("module graph_assembly_basic") !=
                     std::string_view::npos;
    }
    if (!clone.findGraph("graph_assembly_basic") || !hasSource) {
        return fail("cloned design/source owners lost their original source after result release");
    }

    IngestRequest argsOnly;
    argsOnly.slangArgs = {(request.inputRoot / "graph_assembly_basic.sv").string(),
                         "--top", "graph_assembly_basic"};
    argsOnly.logLevel = wolvrix::lib::LogLevel::Off;
    if (!readSources(argsOnly).success()) {
        return fail("SV sources passed through slang arguments must remain supported");
    }

    request.sources.front().path = "stmt_lowerer_errors.sv";
    request.tops = {"stmt_lowerer_while_stmt"};
    auto failed = readSources(request);
    if (failed.success() || failed.design || failed.diagnostics.empty()) {
        return fail("lowering errors must not return a publishable partial design");
    }
    failed.sourceOwners.clear();
    bool located = false;
    for (const auto& diagnostic : failed.diagnostics) {
        if (diagnostic.location && diagnostic.location->file.filename() == "stmt_lowerer_errors.sv" &&
            diagnostic.location->line && diagnostic.location->column &&
            !diagnostic.location->sourceLine.empty()) {
            located = true;
        }
    }
    if (!located) {
        return fail("lowering diagnostics must own their source positions and snippets");
    }

    IngestRequest firrtl;
    firrtl.inputRoot = WOLVRIX_INGEST_TEST_DATA_DIR;
    firrtl.sources.push_back({SourceLanguage::Firrtl, "firrtl/parser.fir", "firrtl"});
    const auto rejected = [](const IngestResult& result) {
        return !result.success() && !result.design && !result.diagnostics.empty() &&
            result.diagnostics.front().message.find("annotation") != std::string::npos;
    };
    if (!scanFirrtlInterfaces(firrtl).success || !scanInterfaces(firrtl).success ||
        !rejected(readFirrtl(firrtl)) || !rejected(readSources(firrtl))) {
        return fail("FIRRTL syntax scanning must not grant execution to unknown annotations");
    }
    firrtl.sources.push_back(request.sources.front());
    if (readSources(firrtl).design || specializeModule(firrtl, {}).design || linkModules(firrtl, {}).design) {
        return fail("failed mixed elaboration/specialization/linking returned a partial design");
    }
    return 0;
}
