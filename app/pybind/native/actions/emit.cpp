#include "native/module/methods.hpp"

#include "emit/system_verilog.hpp"
#include "emit/verilator_repcut_package.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wolvrix::app::pybind
{

    PyObject *py_session_emit_sv(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        PyObject *topListObj = Py_None;
        int splitModules = 0;
        static const char *kwlist[] = {"session", "design", "output", "top", "split_modules", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|Op", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &output, &topListObj, &splitModules))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *design = sessionDesign(*session, designKey);
        if (!design)
        {
            PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey);
            return nullptr;
        }

        std::vector<std::string> topNames;
        std::string error;
        if (!parseStringList(topListObj, topNames, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }

        wolvrix::lib::emit::EmitDiagnostics diagnostics;
        wolvrix::lib::emit::EmitSystemVerilog emitter(&diagnostics);
        wolvrix::lib::emit::EmitOptions options;
        options.splitModules = splitModules != 0;
        const std::filesystem::path outPath(output);
        if (options.splitModules)
        {
            if (outPath.has_extension() && outPath.extension() == ".sv")
            {
                PyErr_SetString(PyExc_ValueError,
                                "emit_sv(..., split_modules=True) expects an output directory, not a .sv file path");
                return nullptr;
            }
            options.outputDir = outPath.string();
        }
        else
        {
            const auto filename = outPath.filename().string();
            if (filename.empty())
            {
                PyErr_SetString(PyExc_ValueError,
                                "emit_sv(..., split_modules=False) expects an output file path");
                return nullptr;
            }
            options.outputFilename = filename;
            if (!outPath.parent_path().empty())
            {
                options.outputDir = outPath.parent_path().string();
            }
        }
        options.topOverrides = std::move(topNames);

        const auto result = emitter.emit(*design, options);
        return makeActionResult(result.success && !diagnostics.hasError(),
                                diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

    PyObject *py_session_emit_verilator_repcut_package(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        PyObject *topListObj = Py_None;
        static const char *kwlist[] = {"session", "design", "output", "top", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|O", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &output, &topListObj))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        auto *design = sessionDesign(*session, designKey);
        if (!design)
        {
            PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey);
            return nullptr;
        }

        std::vector<std::string> topNames;
        std::string error;
        if (!parseStringList(topListObj, topNames, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }

        wolvrix::lib::emit::EmitDiagnostics diagnostics;
        wolvrix::lib::emit::EmitVerilatorRepCutPackage emitter(&diagnostics);
        wolvrix::lib::emit::EmitOptions options;
        const std::filesystem::path outPath(output);
        if (outPath.empty())
        {
            PyErr_SetString(PyExc_ValueError,
                            "emit_verilator_repcut_package(...) expects an output directory");
            return nullptr;
        }
        options.outputDir = outPath.string();
        options.topOverrides = std::move(topNames);

        const auto result = emitter.emit(*design, options);
        return makeActionResult(result.success && !diagnostics.hasError(),
                                diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

} // namespace wolvrix::app::pybind
