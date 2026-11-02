#include "native/module/methods.hpp"

#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"
#include "core/load.hpp"

#include <exception>
#include <string>

namespace wolvrix::app::pybind
{

    PyObject *py_session_read_json_file(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *path = nullptr;
        const char *outDesign = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "path", "out_design", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &path, &outDesign, &replace))
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

        try
        {
            wolvrix::lib::load::LoadJson loader;
            auto design = loader.loadFile(path);
            if (replace)
            {
                sessionEraseKey(*session, outDesign);
            }
            session->designs.insert_or_assign(std::string(outDesign),
                                              DesignHandle{std::move(design), {}});
            return makeActionResult(true, {}, nullptr);
        }
        catch (const std::exception &ex)
        {
            return makeActionResult(false,
                                    singleDiagnostic(wolvrix::lib::diag::DiagnosticKind::Error,
                                                     ex.what(), "read_json_file"),
                                    nullptr);
        }
    }

    PyObject *py_session_load_json_text(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *text = nullptr;
        const char *outDesign = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "text", "out_design", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &text, &outDesign, &replace))
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

        try
        {
            auto design = wolvrix::lib::grh::Design::fromJsonString(text);
            if (replace)
            {
                sessionEraseKey(*session, outDesign);
            }
            session->designs.insert_or_assign(std::string(outDesign),
                                              DesignHandle{std::move(design), {}});
            return makeActionResult(true, {}, nullptr);
        }
        catch (const std::exception &ex)
        {
            return makeActionResult(false,
                                    singleDiagnostic(wolvrix::lib::diag::DiagnosticKind::Error,
                                                     ex.what(), "load_json_text"),
                                    nullptr);
        }
    }

} // namespace wolvrix::app::pybind
