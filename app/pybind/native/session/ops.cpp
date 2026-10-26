#include "native/module/methods.hpp"

#include "native/diagnostics/to_python.hpp"
#include "native/session/adapters/export.hpp"
#include "native/session/storage.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::app::pybind
{

    PyObject *py_create_session(PyObject * /*self*/, PyObject * /*args*/)
    {
        return makeSessionCapsule();
    }

    PyObject *py_session_close(PyObject * /*self*/, PyObject *args)
    {
        PyObject *sessionObj = nullptr;
        if (!PyArg_ParseTuple(args, "O", &sessionObj))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        session->clear();
        Py_RETURN_NONE;
    }

    PyObject *py_session_contains(PyObject * /*self*/, PyObject *args)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        if (!PyArg_ParseTuple(args, "Os", &sessionObj, &key))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        return PyBool_FromLong(sessionHasKey(*session, key) ? 1 : 0);
    }

    PyObject *py_session_keys(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *prefix = nullptr;
        PyObject *kindObj = Py_None;
        static const char *kwlist[] = {"session", "prefix", "kind", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|zO", const_cast<char **>(kwlist),
                                         &sessionObj, &prefix, &kindObj))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }

        std::optional<std::string> kindFilter;
        if (kindObj != Py_None)
        {
            if (!PyUnicode_Check(kindObj))
            {
                PyErr_SetString(PyExc_ValueError, "kind must be a string or None");
                return nullptr;
            }
            const char *kindText = PyUnicode_AsUTF8(kindObj);
            if (!kindText)
            {
                return nullptr;
            }
            kindFilter = std::string(kindText);
        }

        std::vector<std::string> keys;
        auto appendKey = [&](const std::string &key) {
            if (prefix && !std::string_view(key).starts_with(prefix))
            {
                return;
            }
            if (kindFilter && sessionValueKind(*session, key) != *kindFilter)
            {
                return;
            }
            keys.push_back(key);
        };

        for (const auto &[key, value] : session->designs)
        {
            (void)value;
            appendKey(key);
        }
        for (const auto &[key, value] : session->nativeValues)
        {
            (void)value;
            appendKey(key);
        }
        for (const auto &[key, value] : session->pythonValues)
        {
            (void)value;
            appendKey(key);
        }

        std::sort(keys.begin(), keys.end());
        PyObject *list = PyList_New(static_cast<Py_ssize_t>(keys.size()));
        if (!list)
        {
            return nullptr;
        }
        for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(keys.size()); ++i)
        {
            PyObject *item = PyUnicode_FromString(keys[static_cast<std::size_t>(i)].c_str());
            if (!item)
            {
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SET_ITEM(list, i, item);
        }
        return list;
    }

    PyObject *py_session_kind(PyObject * /*self*/, PyObject *args)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        if (!PyArg_ParseTuple(args, "Os", &sessionObj, &key))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        const std::string kind = sessionValueKind(*session, key);
        if (kind.empty())
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", key);
            return nullptr;
        }
        return PyUnicode_FromString(kind.c_str());
    }

    PyObject *py_session_get_value(PyObject * /*self*/, PyObject *args)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        if (!PyArg_ParseTuple(args, "Os", &sessionObj, &key))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }

        const char *storage = sessionStorageKind(*session, key);
        if (!storage)
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", key);
            return nullptr;
        }
        const std::string kind = sessionValueKind(*session, key);
        PyObject *result = PyTuple_New(3);
        if (!result)
        {
            return nullptr;
        }
        PyObject *storageObj = PyUnicode_FromString(storage);
        PyObject *kindObj = PyUnicode_FromString(kind.c_str());
        if (!storageObj || !kindObj)
        {
            Py_XDECREF(storageObj);
            Py_XDECREF(kindObj);
            Py_DECREF(result);
            return nullptr;
        }
        PyTuple_SET_ITEM(result, 0, storageObj);
        PyTuple_SET_ITEM(result, 1, kindObj);
        if (std::string_view(storage) == "python")
        {
            PyObject *payload = session->pythonValues.at(key).object;
            Py_INCREF(payload);
            PyTuple_SET_ITEM(result, 2, payload);
        }
        else
        {
            Py_INCREF(Py_None);
            PyTuple_SET_ITEM(result, 2, Py_None);
        }
        return result;
    }

    PyObject *py_session_export(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        const char *view = "text";
        static const char *kwlist[] = {"session", "key", "view", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Os|s", const_cast<char **>(kwlist),
                                         &sessionObj, &key, &view))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        return exportSessionNativeValue(*session, key, view);
    }

    PyObject *py_session_put_python(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        PyObject *value = nullptr;
        const char *kind = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "key", "value", "kind", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OsOs|p", const_cast<char **>(kwlist),
                                         &sessionObj, &key, &value, &kind, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        std::string error;
        if (!ensureSessionInsertable(*session, key, replace != 0, error))
        {
            PyErr_SetString(PyExc_KeyError, error.c_str());
            return nullptr;
        }
        if (replace)
        {
            sessionEraseKey(*session, key);
        }
        Py_INCREF(value);
        session->pythonValues.insert_or_assign(std::string(key),
                                               PythonSessionValue(std::string(kind), value));
        Py_RETURN_NONE;
    }

    PyObject *py_session_delete(PyObject * /*self*/, PyObject *args)
    {
        PyObject *sessionObj = nullptr;
        const char *key = nullptr;
        if (!PyArg_ParseTuple(args, "Os", &sessionObj, &key))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        if (!sessionHasKey(*session, key))
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", key);
            return nullptr;
        }
        sessionEraseKey(*session, key);
        Py_RETURN_NONE;
    }

    PyObject *py_session_copy(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *src = nullptr;
        const char *dst = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "src", "dst", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &src, &dst, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        if (!sessionHasKey(*session, src))
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", src);
            return nullptr;
        }
        std::string error;
        if (!ensureSessionInsertable(*session, dst, replace != 0, error))
        {
            PyErr_SetString(PyExc_KeyError, error.c_str());
            return nullptr;
        }
        if (replace)
        {
            sessionEraseKey(*session, dst);
        }

        const std::string srcKey(src);
        const std::string dstKey(dst);
        if (auto it = session->designs.find(srcKey); it != session->designs.end())
        {
            session->designs.insert_or_assign(
                dstKey,
                DesignHandle{it->second.design.clone(), it->second.compilation});
        }
        else if (auto it = session->nativeValues.find(srcKey); it != session->nativeValues.end())
        {
            session->nativeValues.insert_or_assign(dstKey, it->second ? it->second->clone() : nullptr);
        }
        else if (auto it = session->pythonValues.find(srcKey); it != session->pythonValues.end())
        {
            PyObject *object = it->second.object;
            Py_INCREF(object);
            session->pythonValues.insert_or_assign(dstKey, PythonSessionValue(it->second.kind, object));
        }
        else
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", src);
            return nullptr;
        }
        Py_RETURN_NONE;
    }

    PyObject *py_session_rename(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *src = nullptr;
        const char *dst = nullptr;
        int replace = 0;
        static const char *kwlist[] = {"session", "src", "dst", "replace", nullptr};
        if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Oss|p", const_cast<char **>(kwlist),
                                         &sessionObj, &src, &dst, &replace))
        {
            return nullptr;
        }
        SessionHandle *session = getSessionHandle(sessionObj);
        if (!session)
        {
            return nullptr;
        }
        if (!sessionHasKey(*session, src))
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", src);
            return nullptr;
        }
        std::string error;
        if (!ensureSessionInsertable(*session, dst, replace != 0, error))
        {
            PyErr_SetString(PyExc_KeyError, error.c_str());
            return nullptr;
        }
        if (replace)
        {
            sessionEraseKey(*session, dst);
        }
        const std::string srcKey(src);
        const std::string dstKey(dst);
        if (auto node = session->designs.extract(srcKey); !node.empty())
        {
            node.key() = dstKey;
            session->designs.insert(std::move(node));
        }
        else if (auto node = session->nativeValues.extract(srcKey); !node.empty())
        {
            node.key() = dstKey;
            session->nativeValues.insert(std::move(node));
        }
        else if (auto node = session->pythonValues.extract(srcKey); !node.empty())
        {
            node.key() = dstKey;
            session->pythonValues.insert(std::move(node));
        }
        else
        {
            PyErr_Format(PyExc_KeyError, "session key not found: %s", src);
            return nullptr;
        }
        Py_RETURN_NONE;
    }

} // namespace wolvrix::app::pybind
