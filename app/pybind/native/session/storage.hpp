#ifndef WOLVRIX_APP_PYBIND_NATIVE_SESSION_STORAGE_HPP
#define WOLVRIX_APP_PYBIND_NATIVE_SESSION_STORAGE_HPP

#include <Python.h>

#include "core/diagnostics.hpp"
#include "core/logging.hpp"
#include "core/store.hpp"
#include "native/session/state.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace slang
{
    class SourceManager;
}

namespace wolvrix::app::pybind
{

    void destroySessionCapsule(PyObject *capsule);
    SessionHandle *getSessionHandle(PyObject *capsule);
    PyObject *makeSessionCapsule();

    bool readFileText(const std::string &path, std::string &out, std::string &error);

    std::vector<wolvrix::lib::diag::Diagnostic> singleDiagnostic(
        wolvrix::lib::diag::DiagnosticKind kind,
        std::string message,
        std::string passName = {},
        std::string context = {});

    bool sessionHasKey(const SessionHandle &session, std::string_view key);
    const char *sessionStorageKind(const SessionHandle &session, std::string_view key);
    std::string sessionValueKind(const SessionHandle &session, std::string_view key);
    const slang::SourceManager *sessionDesignSourceManager(const SessionHandle &session,
                                                           std::string_view key);
    wolvrix::lib::grh::Design *sessionDesign(SessionHandle &session, std::string_view key);

    bool ensureSessionInsertable(const SessionHandle &session,
                                 std::string_view key,
                                 bool replace,
                                 std::string &error);
    void sessionEraseKey(SessionHandle &session, std::string_view key);
    wolvrix::lib::transform::SessionStore cloneSessionStore(
        const wolvrix::lib::transform::SessionStore &source);

    bool parseStringList(PyObject *obj, std::vector<std::string> &out, std::string &error);
    wolvrix::lib::LogLevel parseLogLevel(std::string_view text, bool &ok);
    wolvrix::lib::store::JsonPrintMode parseJsonMode(std::string_view text, bool &ok);

} // namespace wolvrix::app::pybind

#endif // WOLVRIX_APP_PYBIND_NATIVE_SESSION_STORAGE_HPP
