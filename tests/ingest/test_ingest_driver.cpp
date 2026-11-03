#include "ingest/firrtl.hpp"
#include "ingest/ingest.hpp"
#include "ingest/sv.hpp"
#include "core/store.hpp"

#include "slang/text/SourceManager.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
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

std::string diagnosticFile(const SourcePosition& position, bool normalizeGenerated) {
    auto file = position.file.generic_string();
    constexpr std::string_view transactions = "/ptmp/firrtl-ingest/transactions/";
    if (normalizeGenerated) {
        const auto start = file.find(transactions);
        if (start != std::string::npos) {
            const auto begin = start + transactions.size();
            const auto end = file.find('/', begin);
            if (end != std::string::npos) file.replace(begin, end - begin, "<transaction>");
        }
    }
    return file;
}

bool sameDiagnostics(const IngestResult& left, const IngestResult& right,
                     bool normalizeGenerated = false) {
    if (left.diagnostics.size() != right.diagnostics.size()) return false;
    for (std::size_t index = 0; index < left.diagnostics.size(); ++index) {
        const auto& a = left.diagnostics[index];
        const auto& b = right.diagnostics[index];
        if (std::tie(a.kind, a.message, a.context, a.passName, a.originSymbol) !=
            std::tie(b.kind, b.message, b.context, b.passName, b.originSymbol) ||
            a.location.has_value() != b.location.has_value()) return false;
        if (a.location &&
            (std::tie(a.location->unit, a.location->line, a.location->column,
                     a.location->sourceLine, a.location->sourceInfo) !=
             std::tie(b.location->unit, b.location->line, b.location->column,
                      b.location->sourceLine, b.location->sourceInfo) ||
             diagnosticFile(*a.location, normalizeGenerated) !=
             diagnosticFile(*b.location, normalizeGenerated))) return false;
    }
    return true;
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

    IngestRequest parallel;
    parallel.inputRoot = WOLVRIX_INGEST_TEST_DATA_DIR;
    parallel.sources.push_back({SourceLanguage::Firrtl, "firrtl/comb/parallel.fir", "comb"});
    parallel.tops = {"top"};
    parallel.logLevel = wolvrix::lib::LogLevel::Off;
    auto serial = readFirrtl(parallel);
    if (!serial.success() || !serial.design || serial.design->graphs().size() != 6)
        return fail("parallel FIRRTL fixture did not publish all six reachable graphs");
    wolvrix::lib::store::StoreJson store;
    const auto expectedJson = store.storeToString(*serial.design);
    for (const std::size_t threads : {2, 4, 2, 4}) {
        parallel.firrtl.lowerThreads = threads;
        auto concurrent = readFirrtl(parallel);
        if (!concurrent.success() || !concurrent.design ||
            store.storeToString(*concurrent.design) != expectedJson ||
            !sameDiagnostics(serial, concurrent))
            return fail("parallel FIRRTL lowering changed deterministic design JSON or diagnostics");
    }

    parallel.sources.front().path = "firrtl/comb/parallel_symbols.fir";
    parallel.firrtl.lowerThreads = 1;
    auto serialSymbols = readFirrtl(parallel);
    if (!serialSymbols.success() || !serialSymbols.design ||
        serialSymbols.design->graphs().size() != 3)
        return fail("symbol lookup fixture did not publish all reachable graphs");
    serialSymbols.sourceOwners.clear();
    constexpr std::string_view sharedName =
        "shared_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules";
    constexpr std::string_view forwardName =
        "forward_reference_name_that_exceeds_small_string_storage_and_is_reused_across_modules";
    for (const auto& [module, width] : {std::pair{"first", 8}, std::pair{"second", 13}}) {
        const auto* graph = serialSymbols.design->findGraph(module);
        if (!graph || graph->valueWidth(graph->findValue(sharedName)) != width ||
            graph->valueWidth(graph->findValue(forwardName)) != width ||
            graph->valueWidth(graph->findValue("_val_0")) != width ||
            !graph->findValue("_op_0").valid() || graph->findOperation("_op_0").valid())
            return fail("long declaration lookup crossed module scope or internal symbol collision");
    }
    const auto symbolsJson = store.storeToString(*serialSymbols.design);
    if (!symbolsJson) return fail("symbol lookup fixture could not serialize its design");
    for (const std::size_t threads : {2, 4, 2, 4}) {
        parallel.firrtl.lowerThreads = threads;
        auto concurrent = readFirrtl(parallel);
        if (!concurrent.success() || !concurrent.design ||
            store.storeToString(*concurrent.design) != symbolsJson ||
            concurrent.dependencies != serialSymbols.dependencies ||
            !sameDiagnostics(serialSymbols, concurrent))
            return fail("symbol lookup changed serial/parallel graph identity or diagnostics");
    }

    parallel.sources.front().path = "firrtl/comb/parallel_symbols_forward_error.fir";
    parallel.firrtl.lowerThreads = 1;
    const auto serialForwardError = readFirrtl(parallel);
    if (serialForwardError.success() || serialForwardError.design ||
        serialForwardError.diagnostics.empty() ||
        serialForwardError.diagnostics.front().message.find(forwardName) == std::string::npos)
        return fail("forward declaration fixture did not retain the original semantic error");
    for (const std::size_t threads : {2, 4, 2, 4}) {
        parallel.firrtl.lowerThreads = threads;
        const auto concurrent = readFirrtl(parallel);
        if (concurrent.success() || concurrent.design ||
            !sameDiagnostics(serialForwardError, concurrent))
            return fail("forward declaration lookup changed serial/parallel error order");
    }

    parallel.sources.front().path = "firrtl/comb/parallel_error.fir";
    parallel.firrtl.lowerThreads = 1;
    auto serialError = readFirrtl(parallel);
    if (serialError.success() || serialError.design || serialError.diagnostics.empty() ||
        serialError.diagnostics.front().message.find("z_missing") == std::string::npos)
        return fail("parallel FIRRTL error fixture did not fail in dependency order");
    for (const std::size_t threads : {2, 4, 2, 4}) {
        parallel.firrtl.lowerThreads = threads;
        auto concurrentError = readFirrtl(parallel);
        if (concurrentError.success() || concurrentError.design ||
            !sameDiagnostics(serialError, concurrentError))
            return fail("parallel FIRRTL lowering changed source-aware error order");
    }

    parallel.sources.front().path = "firrtl/comb/parallel_diagnostics.fir";
    parallel.firrtl.lowerThreads = 1;
    const auto serialWarnings = readFirrtl(parallel);
    if (!serialWarnings.success() || !serialWarnings.design ||
        serialWarnings.design->graphOrder() != std::vector<std::string>{"z_loop", "a_loop", "top"})
        return fail("parallel diagnostic fixture did not publish graphs in dependency order");
    std::size_t loopWarnings = 0;
    for (const auto& diagnostic : serialWarnings.diagnostics)
        if (diagnostic.passName == "comb-loop-elim" &&
            diagnostic.message.find("resolved false loops") != std::string::npos)
            ++loopWarnings;
    if (loopWarnings != 2)
        return fail("parallel diagnostic fixture did not resolve both module-local false loops");
    const auto warningJson = store.storeToString(*serialWarnings.design);
    if (!warningJson) return fail("parallel diagnostic fixture could not serialize its design");
    for (const std::size_t threads : {2, 4, 2, 4}) {
        parallel.firrtl.lowerThreads = threads;
        auto concurrent = readFirrtl(parallel);
        if (!concurrent.success() || !concurrent.design ||
            concurrent.design->graphOrder() != serialWarnings.design->graphOrder() ||
            store.storeToString(*concurrent.design) != warningJson ||
            !sameDiagnostics(serialWarnings, concurrent))
            return fail("parallel FIRRTL graph publication changed verification diagnostic order");
    }

    IngestRequest mixed;
    mixed.inputRoot = WOLVRIX_INGEST_TEST_DATA_DIR;
    mixed.sources = {{SourceLanguage::Firrtl, "firrtl/comb/parallel_mixed.fir", "fir"},
                     {SourceLanguage::SystemVerilog, "firrtl/comb/parallel_mixed.sv", "sv"}};
    mixed.tops = {"top"};
    mixed.logLevel = wolvrix::lib::LogLevel::Off;
    for (const bool firrtlImplementation : {false, true}) {
        auto request = mixed;
        if (firrtlImplementation) {
            request.sources.pop_back();
            ExternalBinding binding;
            binding.sourceUnit = "fir";
            binding.moduleName = "External";
            binding.implementation = {SourceLanguage::Firrtl, "fir", "implementation", {}};
            request.externalBindings.push_back(std::move(binding));
        }
        const auto serialMixed = readSources(request);
        if (!serialMixed.success() || !serialMixed.design ||
            !serialMixed.design->findGraph("implementation") ||
            serialMixed.design->graphOrder().front() != "before" ||
            serialMixed.design->graphOrder().back() != "top")
            return fail("parallel mixed fixture did not resolve its alternating hierarchy");
        const auto serialJson = store.storeToString(*serialMixed.design);
        if (!serialJson) return fail("parallel mixed fixture could not serialize its design");
        for (const std::size_t threads : {2, 4, 2, 4}) {
            request.firrtl.lowerThreads = threads;
            auto concurrent = readSources(request);
            if (!concurrent.success() || !concurrent.design ||
                concurrent.design->graphOrder() != serialMixed.design->graphOrder() ||
                store.storeToString(*concurrent.design) != serialJson ||
                concurrent.dependencies != serialMixed.dependencies ||
                !sameDiagnostics(serialMixed, concurrent, true))
                return fail("parallel mixed lowering changed publication order, JSON, dependencies or diagnostics");
        }
    }
    return 0;
}
