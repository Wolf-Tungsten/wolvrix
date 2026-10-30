#include "ingest/firrtl.hpp"

namespace wolvrix::lib::ingest {

InterfaceScanResult scanFirrtlInterfaces(const IngestRequest& /*request*/) {
    InterfaceScanResult result;
    result.diagnostics.push_back({IngestSeverity::Error,
                                  "FIRRTL interface scanning is not supported yet",
                                  {}, "ingest-firrtl"});
    return result;
}

IngestResult readFirrtl(const IngestRequest& /*request*/) {
    IngestResult result;
    result.diagnostics.push_back({IngestSeverity::Error,
                                  "FIRRTL lowering is not supported yet",
                                  {}, "ingest-firrtl"});
    return result;
}

} // namespace wolvrix::lib::ingest
