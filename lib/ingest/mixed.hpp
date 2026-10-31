#ifndef WOLVRIX_INGEST_MIXED_HPP
#define WOLVRIX_INGEST_MIXED_HPP

#include "ingest/ingest.hpp"

namespace wolvrix::lib::ingest {
IngestResult readMixedSources(const IngestRequest& request);
IngestResult readSpecializedSources(const IngestRequest& request, const ModuleSpecialization& specialization);
std::string parameterLiteral(const ParameterValue& value);
bool verifyLinkedDesign(grh::Design& design, std::vector<IngestDiagnostic>& diagnostics);
std::string sourceIdentity(std::string_view text);
} // namespace wolvrix::lib::ingest
#endif
