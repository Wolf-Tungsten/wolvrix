#ifndef WOLVRIX_INGEST_FIRRTL_HPP
#define WOLVRIX_INGEST_FIRRTL_HPP

#include "ingest/ingest.hpp"

namespace wolvrix::lib::ingest {

InterfaceScanResult scanFirrtlInterfaces(const IngestRequest& request);
IngestResult readFirrtl(const IngestRequest& request);

} // namespace wolvrix::lib::ingest

#endif // WOLVRIX_INGEST_FIRRTL_HPP
