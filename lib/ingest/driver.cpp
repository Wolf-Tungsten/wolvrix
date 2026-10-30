#include "ingest/ingest.hpp"

#include "ingest/firrtl.hpp"
#include "ingest/sv.hpp"

#include <algorithm>

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
    return scanSvInterfaces(request);
}

IngestResult readSources(const IngestRequest& request) {
    const bool hasFirrtl = std::any_of(request.sources.begin(), request.sources.end(),
                                      [](const IngestSource& source) {
                                          return source.language == SourceLanguage::Firrtl;
                                      });
    const bool hasSv = std::any_of(request.sources.begin(), request.sources.end(),
                                  [](const IngestSource& source) {
                                      return source.language == SourceLanguage::SystemVerilog;
                                  });
    if (hasFirrtl && (hasSv || !request.slangArgs.empty() || !request.externalBindings.empty())) {
        IngestResult result;
        result.diagnostics.push_back({IngestSeverity::Error,
                                      "mixed FIRRTL/SystemVerilog elaboration and linking are not supported yet",
                                      {}, "ingest"});
        return result;
    }
    if (hasFirrtl) {
        return readFirrtl(request);
    }
    if (!request.externalBindings.empty()) {
        IngestResult result;
        result.diagnostics.push_back({IngestSeverity::Error,
                                      "external module binding is not supported yet",
                                      {}, "ingest"});
        return result;
    }
    return readSystemVerilog(request);
}

IngestResult specializeModule(const IngestRequest& /*request*/,
                             const ModuleSpecialization& specialization) {
    IngestResult result;
    result.diagnostics.push_back({IngestSeverity::Error,
                                  "module specialization requests are not supported yet",
                                  specialization.implementationName, "ingest"});
    return result;
}

IngestResult linkModules(const IngestRequest& /*request*/, std::vector<IngestResult> modules) {
    IngestResult result;
    for (const IngestResult& module : modules) {
        result.sourceOwners.insert(result.sourceOwners.end(),
                                   module.sourceOwners.begin(), module.sourceOwners.end());
    }
    result.diagnostics.push_back({IngestSeverity::Error,
                                  "cross-language module linking is not supported yet",
                                  {}, "ingest"});
    return result;
}

} // namespace wolvrix::lib::ingest
