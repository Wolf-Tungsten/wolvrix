#include "native/module/methods.hpp"

#include "core/store.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_set>
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

    PyObject *py_session_semantic_stats(PyObject *, PyObject *args, PyObject *kwargs) {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        static const char *kwlist[] = {"session", "design", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os", const_cast<char **>(kwlist),
                                        &sessionObj, &designKey)) return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *design = sessionDesign(*session, designKey);
        if (!design) { PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey); return nullptr; }
        uint64_t trees = 0, nodes = 0, nameBytes = 0, valueRows = 0, operationRows = 0, semanticIds = 0;
        uint64_t semanticPoolIds = 0, semanticLists = 0;
        for (const auto &[symbol, graph] : design->graphs()) {
            (void)symbol;
            const auto payloadStats = graph->payloadStats();
            semanticPoolIds += payloadStats.semanticIds;
            semanticLists += payloadStats.semanticLists;
            if (const auto *tree = graph->semanticTree()) {
                ++trees;
                nodes += tree->size();
                nameBytes += tree->nameBytes();
            }
            for (const auto value : graph->values()) {
                const auto ids = graph->valueSemanticIds(value);
                valueRows += !ids.empty();
                semanticIds += ids.size();
            }
            for (const auto op : graph->operations()) {
                const auto ids = graph->opSemanticIds(op);
                operationRows += !ids.empty();
                semanticIds += ids.size();
            }
        }
        return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
                             "trees", static_cast<unsigned long long>(trees),
                             "nodes", static_cast<unsigned long long>(nodes),
                             "name_bytes", static_cast<unsigned long long>(nameBytes),
                             "value_rows", static_cast<unsigned long long>(valueRows),
                             "operation_rows", static_cast<unsigned long long>(operationRows),
                             "semantic_ids", static_cast<unsigned long long>(semanticIds),
                             "semantic_pool_ids", static_cast<unsigned long long>(semanticPoolIds),
                             "semantic_lists", static_cast<unsigned long long>(semanticLists));
    }

    PyObject *py_session_semantic_validate(PyObject *, PyObject *args, PyObject *kwargs) {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        static const char *kwlist[] = {"session", "design", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os", const_cast<char **>(kwlist),
                                        &sessionObj, &designKey)) return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *design = sessionDesign(*session, designKey);
        if (!design) { PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey); return nullptr; }

        uint64_t duplicatePaths = 0, symbolMismatches = 0, checkedRegisters = 0;
        std::vector<std::string> examples;
        const auto example = [&](std::string text) {
            if (examples.size() < 8) examples.push_back(std::move(text));
        };
        for (const auto &[symbol, graph] : design->graphs()) {
            const auto *tree = graph->semanticTree();
            if (!tree) continue;
            std::unordered_set<std::string> modulePaths;
            for (lib::grh::SemanticId id = 1; id <= tree->size(); ++id) {
                if (tree->node(id).kind != lib::grh::SemanticNodeKind::Module) continue;
                std::string path;
                for (const auto &component : tree->path(id)) {
                    if (!path.empty()) path.push_back('/');
                    path += component;
                }
                if (!modulePaths.insert(path).second) {
                    ++duplicatePaths;
                    example(symbol + ": duplicate module path " + path);
                }
            }
            std::unordered_set<uint32_t> declaredSymbols;
            for (const auto declared : graph->declaredSymbols()) declaredSymbols.insert(declared.value);
            for (const auto op : graph->operations()) {
                if (graph->opKind(op) != lib::grh::OperationKind::kRegister) continue;
                const auto opSymbol = graph->operationSymbol(op);
                if (!opSymbol.valid() || !declaredSymbols.contains(opSymbol.value)) continue;
                const std::string opName(graph->symbolText(opSymbol));
                if (opName.find('$') == std::string::npos) continue;
                ++checkedRegisters;
                for (const auto semantic : graph->opSemanticIds(op)) {
                    std::vector<lib::grh::SemanticId> lineage;
                    for (auto current = semantic; current; current = tree->node(current).parent)
                        lineage.push_back(current);
                    std::string instancePath;
                    for (auto at = lineage.rbegin(); at != lineage.rend(); ++at) {
                        if (*at == tree->root()) continue;
                        const auto &node = tree->node(*at);
                        if (node.kind != lib::grh::SemanticNodeKind::Module) continue;
                        if (!instancePath.empty()) instancePath.push_back('$');
                        instancePath += tree->name(*at);
                    }
                    if (instancePath.empty() || !opName.starts_with(instancePath + "$")) {
                        ++symbolMismatches;
                        example(symbol + ": " + opName + " lacks instance prefix " + instancePath);
                        break;
                    }
                }
            }
        }
        PyObject *exampleList = metadataValue(examples);
        if (!exampleList) return nullptr;
        PyObject *result = Py_BuildValue("{s:K,s:K,s:K,s:N}",
            "module_path_duplicates", static_cast<unsigned long long>(duplicatePaths),
            "register_symbol_mismatches", static_cast<unsigned long long>(symbolMismatches),
            "checked_registers", static_cast<unsigned long long>(checkedRegisters),
            "examples", exampleList);
        if (!result) Py_DECREF(exampleList);
        return result;
    }

    PyObject *py_session_store_json(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        const char *modeText = "pretty-compact";
        PyObject *topListObj = Py_None;
        int emitDefUsers = 0;
        int emitSourceLocations = 0;
        static const char *kwlist[] = {"session", "design", "output", "mode", "top",
                                       "emit_def_users", "emit_source_locations", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|sOpp", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &output, &modeText, &topListObj,
                                         &emitDefUsers, &emitSourceLocations))
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
        options.emitDefUsers = emitDefUsers != 0;
        options.emitSourceLocations = emitSourceLocations != 0;
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

    PyObject *py_session_store_text(PyObject *, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        PyObject *topListObj = Py_None;
        static const char *kwlist[] = {"session", "design", "output", "top", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|O", const_cast<char **>(kwlist),
                                        &sessionObj, &designKey, &output, &topListObj)) return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *design = sessionDesign(*session, designKey);
        if (!design) { PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey); return nullptr; }
        std::vector<std::string> tops;
        std::string error;
        if (!parseStringList(topListObj, tops, error)) {
            PyErr_SetString(PyExc_ValueError, error.c_str()); return nullptr;
        }
        const std::filesystem::path path(output);
        if (path.filename().empty()) {
            PyErr_SetString(PyExc_ValueError, "store_text(...) expects an output file path"); return nullptr;
        }
        wolvrix::lib::store::StoreDiagnostics diagnostics;
        wolvrix::lib::store::StoreText store(&diagnostics, &session->nativeValues);
        wolvrix::lib::store::StoreOptions options;
        options.outputFilename = path.filename().string();
        if (!path.parent_path().empty()) options.outputDir = path.parent_path().string();
        options.topOverrides = std::move(tops);
        const auto result = store.store(*design, options);
        return makeActionResult(result.success && !diagnostics.hasError(), diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

} // namespace wolvrix::app::pybind
