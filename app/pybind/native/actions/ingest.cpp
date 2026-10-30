#include "native/module/methods.hpp"

#include "ingest/ingest.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::app::pybind
{

    PyObject *py_session_read_sv(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        PyObject *pathObj = nullptr;
        const char *outDesign = nullptr;
        PyObject *slangArgsObj = Py_None;
        int replace = 0;
        const char *logLevelText = "info";
        static const char *kwlist[] = {
            "session", "path", "out_design", "slang_args", "replace", "log_level", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OOs|Ops", const_cast<char **>(kwlist),
                                         &sessionObj, &pathObj, &outDesign,
                                         &slangArgsObj, &replace, &logLevelText))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, outDesign, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }

        const char *path = nullptr;
        if (pathObj == Py_None)
        {
            path = nullptr;
        }
        else if (PyUnicode_Check(pathObj))
        {
            path = PyUnicode_AsUTF8(pathObj);
            if (!path)
            {
                return nullptr;
            }
            if (path[0] == '\0')
            {
                PyErr_SetString(PyExc_ValueError, "path must be a non-empty string or None");
                return nullptr;
            }
        }
        else
        {
            PyErr_SetString(PyExc_ValueError, "path must be a string or None");
            return nullptr;
        }

        std::vector<std::string> slangArgs;
        std::string error;
        if (!parseStringList(slangArgsObj, slangArgs, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }

        bool ok = false;
        const wolvrix::lib::LogLevel logLevel = parseLogLevel(logLevelText, ok);
        if (!ok)
        {
            PyErr_SetString(PyExc_ValueError, "unknown log_level");
            return nullptr;
        }

        wolvrix::lib::ingest::IngestRequest request;
        if (path)
        {
            request.sources.push_back({wolvrix::lib::ingest::SourceLanguage::SystemVerilog, path, {}});
        }
        request.slangArgs = std::move(slangArgs);
        request.logLevel = logLevel;
        request.logSink = [](const wolvrix::lib::LogEvent &event) {
            std::string message;
            if (!event.tag.empty())
            {
                message.append(event.tag);
                message.append(": ");
            }
            message.append(event.message);
            std::cerr << message << '\n';
        };
        auto result = wolvrix::lib::ingest::readSources(request);
        PyObject *action = makeIngestActionResult(result);
        if (!action)
        {
            return nullptr;
        }
        if (result.success())
        {
            session->designs.insert_or_assign(
                std::string(outDesign),
                DesignHandle{std::move(*result.design), std::move(result.sourceOwners)});
            session->nativeValues.erase(outDesign);
            session->pythonValues.erase(outDesign);
        }
        return action;
    }

    PyObject *py_session_clone_design(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *design = nullptr;
        const char *outDesign = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "design", "out_design", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &design, &outDesign, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto srcIt = session->designs.find(design);
        if (srcIt == session->designs.end())
        {
            PyErr_Format(PyExc_KeyError, "design key not found: %s", design);
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, outDesign, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }
        DesignHandle clone{srcIt->second.design.clone(), srcIt->second.sourceOwners};
        session->designs.insert_or_assign(std::string(outDesign),
                                          std::move(clone));
        session->nativeValues.erase(outDesign);
        session->pythonValues.erase(outDesign);
        return makeActionResult(true, {}, sessionDesignSourceManager(*session, outDesign));
    }

} // namespace wolvrix::app::pybind
