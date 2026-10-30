#ifndef WOLVRIX_INGEST_FIRRTL_HPP
#define WOLVRIX_INGEST_FIRRTL_HPP

#include "ingest/ingest.hpp"
#include "ingest/firrtl_syntax.hpp"

namespace wolvrix::lib::ingest {

// Produces owning shared-driver leaves from the typed interface index. Inferred
// and non-Logic source semantics remain explicitly marked for later resolution.
ModuleSignature describeFirrtlModule(const firrtl::Source& source, const firrtl::Module& module);
InterfaceScanResult scanFirrtlInterfaces(const IngestRequest& request);
IngestResult readFirrtl(const IngestRequest& request);

} // namespace wolvrix::lib::ingest

#endif // WOLVRIX_INGEST_FIRRTL_HPP
