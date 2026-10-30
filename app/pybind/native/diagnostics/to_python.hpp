#ifndef WOLVRIX_APP_PYBIND_NATIVE_DIAGNOSTICS_TO_PYTHON_HPP
#define WOLVRIX_APP_PYBIND_NATIVE_DIAGNOSTICS_TO_PYTHON_HPP

#include <Python.h>

#include "core/diagnostics.hpp"
#include "ingest/ingest.hpp"

#include <vector>

namespace slang
{
    class SourceManager;
}

namespace wolvrix::app::pybind
{

    PyObject *makeActionResult(bool success,
                               const std::vector<wolvrix::lib::diag::Diagnostic> &messages,
                               const slang::SourceManager *sourceManager);
    PyObject *makeIngestActionResult(const wolvrix::lib::ingest::IngestResult &result);
    PyObject *makePassActionResult(bool success,
                                   bool changed,
                                   const std::vector<wolvrix::lib::diag::Diagnostic> &messages,
                                   const slang::SourceManager *sourceManager);

} // namespace wolvrix::app::pybind

#endif // WOLVRIX_APP_PYBIND_NATIVE_DIAGNOSTICS_TO_PYTHON_HPP
