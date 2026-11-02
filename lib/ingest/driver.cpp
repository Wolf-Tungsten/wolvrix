#include "ingest/ingest.hpp"

#include "ingest/firrtl.hpp"
#include "ingest/sv.hpp"
#include "mixed.hpp"

#include <algorithm>
#include <tuple>

namespace wolvrix::lib::ingest {

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
        return readMixedSources(request);
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
    return result;
}

IngestResult specializeModule(const IngestRequest& request,
                             const ModuleSpecialization& specialization) {
    return readSpecializedSources(request, specialization);
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
