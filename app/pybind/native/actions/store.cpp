#include "native/module/methods.hpp"

#include "core/store.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace wolvrix::app::pybind
{

    namespace {
    template<typename T>
    PyObject *metadataValue(const T &value) {
        if constexpr (std::is_same_v<T, bool>) return PyBool_FromLong(value);
        else if constexpr (std::is_same_v<T, int64_t>) return PyLong_FromLongLong(value);
        else if constexpr (std::is_same_v<T, double>) return PyFloat_FromDouble(value);
        else if constexpr (std::is_same_v<T, std::string>) return PyUnicode_FromStringAndSize(value.data(), value.size());
        else {
            PyObject *list = PyList_New(value.size());
            if (!list) return nullptr;
            for (std::size_t i = 0; i < value.size(); ++i) {
                PyObject *item = metadataValue(typename T::value_type(value[i]));
                if (!item) { Py_DECREF(list); return nullptr; }
                PyList_SET_ITEM(list, i, item);
            }
            return list;
        }
    }
    }

    PyObject *py_session_graph_metadata(PyObject *, PyObject *args, PyObject *kwargs) {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *top = nullptr;
        static const char *kwlist[] = {"session", "design", "top", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os|z", const_cast<char **>(kwlist),
                                        &sessionObj, &designKey, &top)) return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *design = sessionDesign(*session, designKey);
        if (!design) { PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey); return nullptr; }
        if (!top && design->topGraphs().size() != 1) {
            PyErr_SetString(PyExc_ValueError, "graph_metadata requires exactly one top or an explicit top"); return nullptr;
        }
        auto *graph = design->findGraph(top ? std::string(top) : design->topGraphs()[0]);
        if (!graph) { PyErr_SetString(PyExc_KeyError, "top graph not found"); return nullptr; }
        PyObject *metadata = PyDict_New();
        if (!metadata) return nullptr;
        for (const auto &attr : graph->metadata()) {
            PyObject *value = std::visit([](const auto &value) { return metadataValue(value); }, attr.value);
            if (!value || PyDict_SetItemString(metadata, attr.key.c_str(), value) < 0) {
                Py_XDECREF(value); Py_DECREF(metadata); return nullptr;
            }
            Py_DECREF(value);
        }
        return Py_BuildValue("{s:s,s:N}", "symbol", graph->symbol().c_str(), "metadata", metadata);
    }

    PyObject *py_session_store_json(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        const char *modeText = "pretty-compact";
        PyObject *topListObj = Py_None;
        static const char *kwlist[] = {"session", "design", "output", "mode", "top", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|sO", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &output, &modeText, &topListObj))
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

        bool ok = false;
        const auto mode = parseJsonMode(modeText, ok);
        if (!ok)
        {
            PyErr_SetString(PyExc_ValueError, "unknown json mode");
            return nullptr;
        }

        std::vector<std::string> topNames;
        std::string error;
        if (!parseStringList(topListObj, topNames, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }

        const std::filesystem::path outPath(output);
        const auto filename = outPath.filename().string();
        if (filename.empty())
        {
            PyErr_SetString(PyExc_ValueError, "store_json(...) expects an output file path");
            return nullptr;
        }

        wolvrix::lib::store::StoreDiagnostics diagnostics;
        wolvrix::lib::store::StoreJson store(&diagnostics);
        wolvrix::lib::store::StoreOptions options;
        options.jsonMode = mode;
        options.topOverrides = std::move(topNames);
        options.outputFilename = filename;
        if (!outPath.parent_path().empty())
        {
            options.outputDir = outPath.parent_path().string();
        }
        const auto result = store.store(*design, options);
        return makeActionResult(result.success && !diagnostics.hasError(),
                                diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

} // namespace wolvrix::app::pybind
