#include "native/module/methods.hpp"

#include "ingest/ingest.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wolvrix::app::pybind
{
    namespace {
    namespace ingest = wolvrix::lib::ingest;
    std::string stringValue(PyObject* value, const char* label) {
        if (!value || !PyUnicode_Check(value)) throw std::invalid_argument(std::string(label) + " must be a string");
        const char* text = PyUnicode_AsUTF8(value);
        if (!text) throw std::invalid_argument(std::string("invalid UTF-8 in ") + label);
        return text;
    }
    PyObject* field(PyObject* dictionary, const char* key, bool required = false) {
        if (!PyDict_Check(dictionary)) throw std::invalid_argument("request entries must be dictionaries");
        auto* value = PyDict_GetItemString(dictionary, key);
        if (!value && required) throw std::invalid_argument(std::string("missing request field: ") + key);
        return value;
    }
    std::string stringField(PyObject* dictionary, const char* key, const std::string& fallback = {}) {
        const auto value = field(dictionary, key); return value ? stringValue(value, key) : fallback;
    }
    bool boolean(PyObject* value, const char* name) {
        if (!PyBool_Check(value)) throw std::invalid_argument(std::string(name) + " must be a bool");
        return value == Py_True;
    }
    void strings(PyObject* value, std::vector<std::string>& output) {
        std::string error; if (!parseStringList(value, output, error)) throw std::invalid_argument(error);
    }
    ingest::SourceLanguage language(const std::string& name) {
        if (name == "firrtl") return ingest::SourceLanguage::Firrtl;
        if (name == "systemverilog" || name == "sv") return ingest::SourceLanguage::SystemVerilog;
        throw std::invalid_argument("unknown source language: " + name);
    }
    void knownFields(PyObject* dictionary, std::initializer_list<std::string_view> keys) {
        if (!PyDict_Check(dictionary)) throw std::invalid_argument("options must be a dictionary");
        Py_ssize_t position = 0; PyObject *key, *value;
        while (PyDict_Next(dictionary, &position, &key, &value)) {
            const auto name = stringValue(key, "option name");
            if (std::find(keys.begin(), keys.end(), name) == keys.end()) throw std::invalid_argument("unknown option: " + name);
        }
    }
    ingest::ParameterValue parameter(PyObject* value) {
        if (PyUnicode_Check(value)) return stringValue(value, "parameter");
        if (PyFloat_Check(value)) return PyFloat_AsDouble(value);
        knownFields(value, {"kind", "width", "signed", "bits", "text"});
        const auto kind = stringField(value, "kind", "integer");
        if (kind == "raw_string") return ingest::RawStringParameter{stringValue(field(value, "text", true), "text")};
        if (kind != "integer") throw std::invalid_argument("unknown parameter kind: " + kind);
        const auto width = field(value, "width", true);
        if (!PyLong_Check(width) || PyBool_Check(width)) throw std::invalid_argument("parameter width must be an integer");
        const auto bits = PyLong_AsUnsignedLongLong(width);
        if (PyErr_Occurred() || !bits || bits > UINT32_MAX) throw std::invalid_argument("parameter width out of range");
        const auto sign = field(value, "signed");
        return ingest::IntegerParameter{static_cast<uint32_t>(bits), sign ? boolean(sign, "signed") : false,
            stringValue(field(value, "bits", true), "bits")};
    }
    void options(ingest::IngestRequest& request, PyObject* firrtl, PyObject* bindings) {
        if (firrtl != Py_None) {
            knownFields(firrtl, {"expected_version", "node_annotations", "lower_threads", "invalid_policy", "init_policy", "enabled_layers", "annotation_files", "resource_roots"});
            if (const auto version = field(firrtl, "expected_version")) request.firrtl.expectedVersion = stringValue(version, "expected_version");
            if (const auto nodes = field(firrtl, "node_annotations")) request.firrtl.nodeAnnotations = boolean(nodes, "node_annotations");
            if (const auto threads = field(firrtl, "lower_threads")) {
                if (!PyLong_Check(threads) || PyBool_Check(threads)) throw std::invalid_argument("lower_threads must be an integer");
                const auto count = PyLong_AsUnsignedLongLong(threads);
                if (PyErr_Occurred() || count > SIZE_MAX) throw std::invalid_argument("lower_threads out of range");
                request.firrtl.lowerThreads = static_cast<std::size_t>(count);
            }
            if (const auto layers = field(firrtl, "enabled_layers")) strings(layers, request.firrtl.enabledLayers);
            if (const auto paths = field(firrtl, "annotation_files")) strings(paths, request.firrtl.annotationFiles);
            if (const auto paths = field(firrtl, "resource_roots")) strings(paths, request.firrtl.resourceRoots);
            const auto invalid = stringField(firrtl, "invalid_policy", "unknown");
            const auto init = stringField(firrtl, "init_policy", "unknown");
            if ((invalid != "unknown" && invalid != "zero") || (init != "unknown" && init != "zero"))
                throw std::invalid_argument("FIRRTL policies must be 'unknown' or 'zero'");
            request.firrtl.invalidPolicy = invalid == "zero" ? ingest::FirrtlInvalidPolicy::Zero : ingest::FirrtlInvalidPolicy::Unknown;
            request.firrtl.initPolicy = init == "zero" ? ingest::FirrtlInitPolicy::Zero : ingest::FirrtlInitPolicy::Unknown;
        }
        if (bindings == Py_None) return;
        if (!PyList_Check(bindings)) throw std::invalid_argument("external_bindings must be a list");
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(bindings); ++i) {
            const auto entry = PyList_GET_ITEM(bindings, i);
            knownFields(entry, {"source_unit", "module", "implementation", "ports"});
            ingest::ExternalBinding binding; binding.sourceUnit = stringField(entry, "source_unit");
            binding.moduleName = stringValue(field(entry, "module", true), "module");
            const auto implementation = field(entry, "implementation", true);
            knownFields(implementation, {"language", "source_unit", "name", "parameters"});
            binding.implementation.language = language(stringField(implementation, "language", "systemverilog"));
            binding.implementation.sourceUnit = stringField(implementation, "source_unit");
            binding.implementation.implementationName = stringValue(field(implementation, "name", true), "name");
            if (const auto parameters = field(implementation, "parameters")) {
                if (!PyDict_Check(parameters)) throw std::invalid_argument("parameters must be a dictionary of typed values");
                Py_ssize_t at = 0; PyObject *name, *value;
                while (PyDict_Next(parameters, &at, &name, &value))
                    binding.implementation.parameters.push_back({stringValue(name, "parameter name"), parameter(value)});
            }
            if (const auto ports = field(entry, "ports")) {
                if (!PyDict_Check(ports)) throw std::invalid_argument("ports must map source leaf names to implementation leaf names");
                Py_ssize_t at = 0; PyObject *source, *target;
                while (PyDict_Next(ports, &at, &source, &target))
                    binding.ports.push_back({stringValue(source, "source port"), stringValue(target, "implementation port")});
            }
            request.externalBindings.push_back(std::move(binding));
        }
    }
    PyObject* readRequest(SessionHandle& session, const char* output, bool replace, ingest::IngestRequest request) {
        std::string error;
        if (!ensureSessionInsertable(session, output, replace, error)) { PyErr_SetString(PyExc_KeyError, error.c_str()); return nullptr; }
        auto result = ingest::readSources(request);
        auto* action = makeIngestActionResult(result); if (!action) return nullptr;
        if (result.success()) {
            session.designs.insert_or_assign(std::string(output), DesignHandle{std::move(*result.design), std::move(result.sourceOwners)});
            session.nativeValues.erase(output); session.pythonValues.erase(output);
        }
        return action;
    }
    PyObject* readMixed(PyObject* args, PyObject* kwargs, bool firrtlOnly) {
        PyObject *sessionObj, *inputs, *tops = Py_None, *slangArgs = Py_None, *firrtlOptions = Py_None, *bindings = Py_None;
        const char *output, *root = "", *level = "info"; int replace = 0; PyObject* svSources = Py_None;
        static const char* sourceKeys[] = {"session", "sources", "out_design", "tops", "input_root", "slang_args", "firrtl_options", "external_bindings", "replace", "log_level", nullptr};
        static const char* firrtlKeys[] = {"session", "path", "out_design", "tops", "input_root", "slang_args", "firrtl_options", "external_bindings", "replace", "log_level", "sv_sources", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, firrtlOnly ? "OOs|OsOOOpsO" : "OOs|OsOOOps",
                const_cast<char**>(firrtlOnly ? firrtlKeys : sourceKeys), &sessionObj, &inputs, &output, &tops, &root,
                &slangArgs, &firrtlOptions, &bindings, &replace, &level, &svSources)) return nullptr;
        auto* session = getSessionHandle(sessionObj); if (!session) return nullptr;
        try {
            ingest::IngestRequest request; request.inputRoot = root;
            bool ok; request.logLevel = parseLogLevel(level, ok); if (!ok) throw std::invalid_argument("unknown log_level");
            strings(tops, request.tops); strings(slangArgs, request.slangArgs); options(request, firrtlOptions, bindings);
            if (firrtlOnly) {
                request.sources.push_back({ingest::SourceLanguage::Firrtl, stringValue(inputs, "path"), {}});
                std::vector<std::string> paths; strings(svSources, paths);
                for (const auto& path : paths) request.sources.push_back({ingest::SourceLanguage::SystemVerilog, path, {}});
            } else {
                if (!PyList_Check(inputs)) throw std::invalid_argument("sources must be a list");
                for (Py_ssize_t i = 0; i < PyList_GET_SIZE(inputs); ++i) {
                    const auto input = PyList_GET_ITEM(inputs, i); knownFields(input, {"language", "path", "unit"});
                    request.sources.push_back({language(stringValue(field(input, "language", true), "language")),
                        stringValue(field(input, "path", true), "path"), stringField(input, "unit")});
                }
            }
            return readRequest(*session, output, replace, std::move(request));
        } catch (const std::exception& failure) {
            if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, failure.what()); return nullptr;
        }
    }
    } // namespace

    PyObject* py_session_read_sources(PyObject*, PyObject* args, PyObject* kwargs) { return readMixed(args, kwargs, false); }
    PyObject* py_session_read_firrtl(PyObject*, PyObject* args, PyObject* kwargs) { return readMixed(args, kwargs, true); }

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
