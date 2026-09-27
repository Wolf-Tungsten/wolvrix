#include "native/module/methods.hpp"

#include "grhsim/convert/grh_to_grhsim.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/io/json.hpp"
#include "grhsim/pass/pass.hpp"
#include "native/diagnostics/to_python.hpp"
#include "native/session/storage.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wolvrix::app::pybind
{

    namespace
    {
        std::optional<wolvrix::lib::grhsim::LogicDomain> parseDomain(std::string_view value)
        {
            if (value == "2state" || value == "2-state")
                return wolvrix::lib::grhsim::LogicDomain::TwoState;
            if (value == "4state" || value == "4-state")
                return wolvrix::lib::grhsim::LogicDomain::FourState;
            return std::nullopt;
        }
    } // namespace

    PyObject *py_session_lower_grhsim(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *outModel = nullptr;
        const char *top = nullptr;
        const char *domainText = "4-state";
        int keepOrigins = 1;
        int keepDeclaredSymbols = 1;
        int consume = 0;
        int replace = 0;
        static const char *kwlist[] = {"session", "design", "out_model", "top", "logic_domain",
                                       "keep_origins", "keep_declared_symbols", "consume",
                                       "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|zspppp", const_cast<char **>(kwlist),
                                         &sessionObj, &designKey, &outModel, &top, &domainText,
                                         &keepOrigins, &keepDeclaredSymbols, &consume, &replace))
            return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *design = sessionDesign(*session, designKey);
        if (!design)
        {
            PyErr_Format(PyExc_KeyError, "design key not found: %s", designKey);
            return nullptr;
        }
        std::string insertError;
        if (!ensureSessionInsertable(*session, outModel, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }
        const auto domain = parseDomain(domainText);
        if (!domain)
        {
            PyErr_SetString(PyExc_ValueError, "logic_domain must be one of: 2-state, 4-state");
            return nullptr;
        }

        wolvrix::lib::diag::Diagnostics diagnostics;
        wolvrix::lib::grhsim::GrhToGrhSimOptions options;
        if (top) options.top = top;
        options.logicDomain = *domain;
        options.keepOrigins = keepOrigins != 0;
        options.keepDeclaredSymbols = keepDeclaredSymbols != 0;
        auto model = wolvrix::lib::grhsim::lowerGrhToGrhSim(*design, options, diagnostics);
        const bool success = model && !diagnostics.hasError();
        if (success)
        {
            if (replace) sessionEraseKey(*session, outModel);
            session->grhsimModels.insert_or_assign(std::string(outModel), std::move(model));
            if (consume && std::string_view(designKey) != std::string_view(outModel))
                sessionEraseKey(*session, designKey);
        }
        return makeActionResult(success, diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

    PyObject *py_session_run_grhsim_pass(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *passName = nullptr;
        const char *modelKey = nullptr;
        PyObject *passArgsObj = Py_None;
        static const char *kwlist[] = {"session", "name", "model", "args", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|O", const_cast<char **>(kwlist),
                                         &sessionObj, &passName, &modelKey, &passArgsObj))
            return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *model = sessionGrhSimModel(*session, modelKey);
        if (!model)
        {
            PyErr_Format(PyExc_KeyError, "GrhSIM model key not found: %s", modelKey);
            return nullptr;
        }
        std::vector<std::string> argumentStorage;
        std::string error;
        if (!parseStringList(passArgsObj, argumentStorage, error))
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }
        std::vector<std::string_view> arguments;
        arguments.reserve(argumentStorage.size());
        for (const std::string &argument : argumentStorage) arguments.push_back(argument);
        auto pass = wolvrix::lib::grhsim::defaultPassRegistry().create(passName, arguments, error);
        if (!pass)
        {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }
        wolvrix::lib::diag::Diagnostics diagnostics;
        wolvrix::lib::grhsim::PassManager manager(
            wolvrix::lib::grhsim::defaultDialectRegistry());
        manager.addPass(std::move(pass));
        const auto result = manager.run(*model, diagnostics);
        return makePassActionResult(result.success && !diagnostics.hasError(), result.changed,
                                    diagnostics.messages(), nullptr);
    }

    PyObject *py_session_load_grhsim(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *path = nullptr;
        const char *outModel = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "path", "out_model", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &path, &outModel, &replace))
            return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        std::string insertError;
        if (!ensureSessionInsertable(*session, outModel, replace != 0, insertError))
        {
            PyErr_SetString(PyExc_KeyError, insertError.c_str());
            return nullptr;
        }
        wolvrix::lib::diag::Diagnostics diagnostics;
        auto model = wolvrix::lib::grhsim::loadGrhSimModel(
            std::filesystem::path(path), wolvrix::lib::grhsim::defaultDialectRegistry(), diagnostics);
        const bool success = model && !diagnostics.hasError();
        if (success)
        {
            if (replace) sessionEraseKey(*session, outModel);
            session->grhsimModels.insert_or_assign(std::string(outModel), std::move(model));
        }
        return makeActionResult(success, diagnostics.messages(), nullptr);
    }

    PyObject *py_session_store_grhsim(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *modelKey = nullptr;
        const char *output = nullptr;
        int pretty = 0;
        static const char *kwlist[] = {"session", "model", "output", "pretty", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &modelKey, &output, &pretty))
            return nullptr;
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session) return nullptr;
        auto *model = sessionGrhSimModel(*session, modelKey);
        if (!model)
        {
            PyErr_Format(PyExc_KeyError, "GrhSIM model key not found: %s", modelKey);
            return nullptr;
        }
        wolvrix::lib::diag::Diagnostics diagnostics;
        const bool success = wolvrix::lib::grhsim::storeGrhSimModel(
            *model, std::filesystem::path(output),
            wolvrix::lib::grhsim::defaultDialectRegistry(), diagnostics, pretty != 0);
        return makeActionResult(success && !diagnostics.hasError(), diagnostics.messages(), nullptr);
    }

    PyObject *py_list_grhsim_passes(PyObject * /*self*/, PyObject * /*args*/)
    {
        const auto passes = wolvrix::lib::grhsim::defaultPassRegistry().names();
        PyObject *list = PyList_New(static_cast<Py_ssize_t>(passes.size()));
        if (!list) return nullptr;
        for (std::size_t i = 0; i < passes.size(); ++i)
        {
            PyObject *item = PyUnicode_FromString(passes[i].c_str());
            if (!item)
            {
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
        }
        return list;
    }

} // namespace wolvrix::app::pybind
