#include "ingest/ingest.hpp"

#include "ingest/firrtl.hpp"
#include "ingest/sv.hpp"
#include "mixed.hpp"
#include "core/transform.hpp"
#include "transform/blackbox_guard.hpp"
#include "transform/latch_transparent_read.hpp"
#include "transform/memory_read_retime.hpp"
#include "transform/multidriven_guard.hpp"
#include "transform/mux_tree_fold.hpp"
#include "transform/register_write_normalize.hpp"
#include "transform/xmr_resolve.hpp"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::ingest {

namespace detail {
std::unordered_set<const grh::Graph*> compactablePrefixSources(const grh::Design& design) {
    struct References { bool candidate = false, named = true; };
    std::unordered_map<const grh::Graph*, References> references;
    const std::unordered_set<std::string> tops(design.topGraphs().begin(), design.topGraphs().end());
    for (const auto& [name, parent] : design.graphs()) for (const auto op : parent->operations()) {
        const auto kind = parent->opKind(op);
        if (kind != grh::OperationKind::kInstance && kind != grh::OperationKind::kBlackbox) continue;
        const auto attrs = parent->opAttrRange(op);
        const auto module = attrs.find("moduleName");
        const auto target = module ? module->value().get<std::string>() : std::nullopt;
        const auto* source = target ? design.findGraph(*target) : nullptr;
        if (!source || tops.contains(source->symbol())) continue;
        auto& entry = references[source];
        entry.candidate |= kind == grh::OperationKind::kInstance;
        const auto named = [&](std::string_view key, const auto& ports) {
            const auto attr = attrs.find(key);
            const auto names = attr ? attr->value().get<std::vector<std::string>>() : std::nullopt;
            if (!names) return ports.empty();
            if (names->size() != ports.size()) return false;
            std::unordered_set<std::string_view> unique;
            for (const auto& port : *names) unique.insert(port);
            return unique.size() == ports.size() && std::all_of(ports.begin(), ports.end(),
                [&](const auto& port) { return unique.contains(port.name); });
        };
        entry.named &= named("inputPortName", source->inputPorts()) &&
            named("outputPortName", source->outputPorts()) && named("inoutPortName", source->inoutPorts());
    }
    std::unordered_set<const grh::Graph*> eligible;
    for (const auto& [graph, entry] : references) if (entry.candidate && entry.named) eligible.insert(graph);
    return eligible;
}
} // namespace detail

namespace {
IngestResult completePipeline(IngestResult result, const IngestRequest& request) {
    if (request.pipeline == IngestPipeline::Ingest || !result.success()) return result;
    namespace transform = wolvrix::lib::transform;
    result.ingestDiagnosticCount = result.diagnostics.size();
    const auto started = std::chrono::steady_clock::now();
    transform::PassManagerOptions options;
    options.graphThreads = std::max(std::size_t{1}, request.firrtl.lowerThreads);
    options.keepDeclaredSymbols = true;
    options.logLevel = request.logLevel;
    if (request.logSink) options.logSink = [&](auto level, auto tag, auto message) {
        request.logSink({level, std::string(tag), std::string(message)});
    };
    transform::PassDiagnostics diagnostics;
    bool success = true;
    const auto append = [&](transform::PassManagerResult stages) {
        success &= stages.success;
        for (auto& item : stages.stages) {
            IngestPrefixStage stage{item.pass, {}, item.wallSeconds, item.workerSeconds, item.overlapped};
            for (const auto& entry : item.diagnostics) {
                const auto kind = entry.kind == diag::DiagnosticKind::Error ? IngestSeverity::Error :
                    entry.kind == diag::DiagnosticKind::Warning ? IngestSeverity::Warning :
                    entry.kind == diag::DiagnosticKind::Info ? IngestSeverity::Info : IngestSeverity::Debug;
                std::optional<SourcePosition> position;
                if (entry.sourcePosition) position = SourcePosition{{}, entry.sourcePosition->file,
                    entry.sourcePosition->line, entry.sourcePosition->column, {}, {}};
                stage.diagnostics.push_back({kind, entry.message, entry.context, entry.passName,
                    entry.originSymbol, std::move(position)});
            }
            result.diagnostics.insert(result.diagnostics.end(), stage.diagnostics.begin(), stage.diagnostics.end());
            result.prefixStages.push_back(std::move(stage));
        }
    };
    // XMR can create cross-graph ports and writers. The local normalization
    // kernels must observe those changes, so publication is a real barrier.
    try {
    transform::PassManager crossGraph(options);
    crossGraph.addPass(std::make_unique<transform::XmrResolvePass>());
    append(crossGraph.run(*result.design, diagnostics));
    if (success) {
        transform::PassManager local(options);
        local.addPass(std::make_unique<transform::MuxTreeFoldPass>());
        local.addPass(std::make_unique<transform::RegisterWriteNormalizePass>(true));
        local.addPass(std::make_unique<transform::MuxTreeFoldPass>(true));
        local.addPass(std::make_unique<transform::MemoryReadRetimePass>());
        append(local.runGraphPipeline(*result.design, diagnostics));
    }
    if (success) {
        // Inout drive summaries and blackbox stub publication depend on the
        // complete catalog. Keep their original ordering between local chains.
        transform::PassManager guards(options);
        guards.addPass(std::make_unique<transform::MultiDrivenGuardPass>());
        guards.addPass(std::make_unique<transform::BlackboxGuardPass>());
        append(guards.run(*result.design, diagnostics));
    }
    if (success) {
        auto tailOptions = options;
        const auto eligible = std::make_shared<const std::unordered_set<const grh::Graph*>>(
            detail::compactablePrefixSources(*result.design));
        tailOptions.compactGraph = [eligible](const auto& graph) { return eligible->contains(&graph); };
        tailOptions.prepareFlattenSources = true;
        transform::PassManager tail(tailOptions);
        tail.addPass(std::make_unique<transform::LatchTransparentReadPass>());
        append(tail.runGraphPipeline(*result.design, diagnostics, true));
    }
    } catch (const std::exception& failure) {
        result.diagnostics.push_back({IngestSeverity::Error, failure.what(), {}, "ingest-prefix"});
        success = false;
    }
    result.prefixWallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!success) result.design.reset();
    return result;
}
} // namespace

bool IngestResult::success() const noexcept {
    return design.has_value() &&
           std::none_of(diagnostics.begin(), diagnostics.end(), [](const IngestDiagnostic& diagnostic) {
               return diagnostic.kind == IngestSeverity::Error;
           });
}

InterfaceScanResult scanInterfaces(const IngestRequest& request) {
    const bool onlyFirrtl = !request.sources.empty() &&
                           std::all_of(request.sources.begin(), request.sources.end(),
                                       [](const IngestSource& source) {
                                           return source.language == SourceLanguage::Firrtl;
                                       });
    if (onlyFirrtl) {
        return scanFirrtlInterfaces(request);
    }
    if (std::any_of(request.sources.begin(), request.sources.end(),
                    [](const auto& source) { return source.language == SourceLanguage::Firrtl; })) {
        auto parsed = readMixedSources(request);
        InterfaceScanResult result;
        result.success = parsed.success();
        result.sourceOwners = std::move(parsed.sourceOwners);
        result.modules = std::move(parsed.modules);
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }
    return scanSvInterfaces(request);
}

IngestResult readSources(const IngestRequest& request) {
    const bool hasFirrtl = std::any_of(request.sources.begin(), request.sources.end(),
                                      [](const IngestSource& source) {
                                          return source.language == SourceLanguage::Firrtl;
                                      });
    if (hasFirrtl) {
        return completePipeline(readMixedSources(request), request);
    }
    if (!request.externalBindings.empty()) {
        IngestResult result;
        result.diagnostics.push_back({IngestSeverity::Error,
                                      "external module binding is not supported yet",
                                      {}, "ingest"});
        return result;
    }
    auto result = readSystemVerilog(request);
    if (result.success()) {
        // Parallel SV conversion publishes in completion order. Canonicalize
        // request-level lists while preserving every graph's values and ops.
        auto& design = *result.design;
        design.sortGraphOrder();
        std::vector<grh::SymbolId> declared(design.declaredSymbols().begin(), design.declaredSymbols().end());
        std::sort(declared.begin(), declared.end(), [&](auto left, auto right) {
            return design.symbolText(left) < design.symbolText(right);
        });
        design.clearDeclaredSymbols();
        for (const auto symbol : declared) design.addDeclaredSymbol(symbol);
        std::sort(result.modules.begin(), result.modules.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        std::sort(result.dependencies.begin(), result.dependencies.end());
        const SourcePosition empty;
        const auto diagnosticKey = [&](const IngestDiagnostic& entry) {
            const auto& location = entry.location ? *entry.location : empty;
            return std::tuple{entry.location.has_value(),
                std::tie(location.file.native(), location.line, location.column, location.unit,
                         location.sourceLine, location.sourceInfo, entry.kind, entry.passName,
                         entry.context, entry.originSymbol, entry.message)};
        };
        std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& left, const auto& right) {
            return diagnosticKey(left) < diagnosticKey(right);
        });
    }
    return completePipeline(std::move(result), request);
}

IngestResult specializeModule(const IngestRequest& request,
                             const ModuleSpecialization& specialization) {
    return completePipeline(readSpecializedSources(request, specialization), request);
}

IngestResult linkModules(const IngestRequest& request, std::vector<IngestResult> modules) {
    IngestResult result;
    grh::Design design;
    bool failed = modules.empty();
    for (const IngestResult& module : modules) {
        result.sourceOwners.insert(result.sourceOwners.end(),
                                   module.sourceOwners.begin(), module.sourceOwners.end());
        result.diagnostics.insert(result.diagnostics.end(), module.diagnostics.begin(), module.diagnostics.end());
        result.modules.insert(result.modules.end(), module.modules.begin(), module.modules.end());
        result.dependencies.insert(result.dependencies.end(), module.dependencies.begin(), module.dependencies.end());
        if (!module.success()) { failed = true; continue; }
        for (const auto& name : module.design->graphOrder()) {
            if (design.findGraph(name)) {
                result.diagnostics.push_back({IngestSeverity::Error, "duplicate implementation graph: " + name, name, "ingest-link"});
                failed = true; continue;
            }
            design.importGraph(*module.design->findGraph(name), name);
        }
    }
    for (const auto& top : request.tops) {
        if (design.findGraph(top)) design.markAsTop(top);
        else { result.diagnostics.push_back({IngestSeverity::Error, "missing linked top: " + top, top, "ingest-link"}); failed = true; }
    }
    if (request.tops.empty()) for (const auto& module : modules) if (module.design)
        for (const auto& top : module.design->topGraphs()) if (design.findGraph(top)) design.markAsTop(top);
    for (const auto& name : design.graphOrder()) {
        const auto& graph = *design.findGraph(name);
        for (const auto op : graph.operations()) {
            const auto operation = graph.getOperation(op);
            if (graph.opKind(op) != grh::OperationKind::kInstance) continue;
            const auto module = operation.attr("moduleName");
            if (!module || !std::holds_alternative<std::string>(*module) || !design.findGraph(std::get<std::string>(*module))) {
                result.diagnostics.push_back({IngestSeverity::Error, "unresolved linked instance in " + name, name, "ingest-link"}); failed = true;
            }
        }
    }
    if (!failed && verifyLinkedDesign(design, result.diagnostics)) result.design.emplace(std::move(design));
    else if (result.diagnostics.empty()) result.diagnostics.push_back({IngestSeverity::Error, "no modules to link", {}, "ingest-link"});
    return result;
}

} // namespace wolvrix::lib::ingest
