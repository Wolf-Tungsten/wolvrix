#ifndef WOLVRIX_INGEST_MIXED_HPP
#define WOLVRIX_INGEST_MIXED_HPP

#include "ingest/ingest.hpp"

#include <unordered_set>

namespace wolvrix::lib::ingest {
namespace detail {
std::unordered_set<const grh::Graph*> compactablePrefixSources(const grh::Design& design);
}
IngestResult readMixedSources(const IngestRequest& request);
IngestResult readSpecializedSources(const IngestRequest& request, const ModuleSpecialization& specialization);
std::string parameterLiteral(const ParameterValue& value);
bool verifyLinkedDesign(grh::Design& design, std::vector<IngestDiagnostic>& diagnostics, std::size_t threads = 1);
std::string sourceIdentity(std::string_view text);
} // namespace wolvrix::lib::ingest
#endif
