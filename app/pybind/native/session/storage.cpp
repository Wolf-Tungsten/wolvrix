#include "native/session/storage.hpp"

#include "slang/ast/Compilation.h"

#include <cctype>
#include <fstream>
#include <string>

namespace wolvrix::app::pybind
{

    void destroySessionCapsule(PyObject *capsule)
    {
        auto *handle = static_cast<SessionHandle *>(PyCapsule_GetPointer(capsule, kSessionCapsuleName));
        delete handle;
    }

    SessionHandle *getSessionHandle(PyObject *capsule)
    {
        return static_cast<SessionHandle *>(PyCapsule_GetPointer(capsule, kSessionCapsuleName));
    }

    PyObject *makeSessionCapsule()
    {
        auto *handle = new SessionHandle();
        return PyCapsule_New(handle, kSessionCapsuleName, destroySessionCapsule);
    }

    bool readFileText(const std::string &path, std::string &out, std::string &error)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            error = "open failed: " + path;
            return false;
        }
        input.seekg(0, std::ios::end);
        const std::streamoff endPos = input.tellg();
        if (endPos <= 0)
        {
            error = "empty file: " + path;
            return false;
        }
        out.resize(static_cast<std::size_t>(endPos));
        input.seekg(0, std::ios::beg);
        input.read(out.data(), static_cast<std::streamsize>(out.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != out.size())
        {
            error = "read failed: " + path;
            return false;
        }
        if (out.empty())
        {
            error = "empty file: " + path;
            return false;
        }
        return true;
    }

    std::vector<wolvrix::lib::diag::Diagnostic> singleDiagnostic(
        wolvrix::lib::diag::DiagnosticKind kind,
        std::string message,
        std::string passName,
        std::string context)
    {
        std::vector<wolvrix::lib::diag::Diagnostic> diagnostics;
        diagnostics.emplace_back(kind, std::move(passName), std::move(message), std::move(context));
        return diagnostics;
    }

    bool sessionHasKey(const SessionHandle &session, std::string_view key)
    {
        const std::string ownedKey(key);
        return session.designs.find(ownedKey) != session.designs.end() ||
               session.nativeValues.find(ownedKey) != session.nativeValues.end() ||
               session.pythonValues.find(ownedKey) != session.pythonValues.end();
    }

    const char *sessionStorageKind(const SessionHandle &session, std::string_view key)
    {
        const std::string ownedKey(key);
        if (session.designs.find(ownedKey) != session.designs.end())
        {
            return "design";
        }
        if (session.nativeValues.find(ownedKey) != session.nativeValues.end())
        {
            return "native";
        }
        if (session.pythonValues.find(ownedKey) != session.pythonValues.end())
        {
            return "python";
        }
        return nullptr;
    }

    std::string sessionValueKind(const SessionHandle &session, std::string_view key)
    {
        const std::string ownedKey(key);
        if (session.designs.find(ownedKey) != session.designs.end())
        {
            return "design";
        }
        if (auto it = session.nativeValues.find(ownedKey); it != session.nativeValues.end())
        {
            return std::string(it->second ? it->second->kind() : std::string_view("opaque"));
        }
        if (auto it = session.pythonValues.find(ownedKey); it != session.pythonValues.end())
        {
            return it->second.kind;
        }
        return {};
    }

    const slang::SourceManager *sessionDesignSourceManager(const SessionHandle &session,
                                                           std::string_view key)
    {
        const std::string ownedKey(key);
        auto it = session.designs.find(ownedKey);
        if (it == session.designs.end() || !it->second.compilation)
        {
            return nullptr;
        }
        return it->second.compilation->getSourceManager();
    }

    wolvrix::lib::grh::Design *sessionDesign(SessionHandle &session, std::string_view key)
    {
        const std::string ownedKey(key);
        auto it = session.designs.find(ownedKey);
        if (it == session.designs.end())
        {
            return nullptr;
        }
        return &it->second.design;
    }

    bool ensureSessionInsertable(const SessionHandle &session,
                                 std::string_view key,
                                 bool replace,
                                 std::string &error)
    {
        if (key.empty())
        {
            error = "key must be non-empty";
            return false;
        }
        if (!replace && sessionHasKey(session, key))
        {
            error = "session key already exists: " + std::string(key);
            return false;
        }
        return true;
    }

    void sessionEraseKey(SessionHandle &session, std::string_view key)
    {
        const std::string ownedKey(key);
        session.designs.erase(ownedKey);
        session.nativeValues.erase(ownedKey);
        session.pythonValues.erase(ownedKey);
    }

    wolvrix::lib::transform::SessionStore cloneSessionStore(
        const wolvrix::lib::transform::SessionStore &source)
    {
        wolvrix::lib::transform::SessionStore clone;
        for (const auto &[key, slot] : source)
        {
            if (!slot)
            {
                continue;
            }
            clone.insert_or_assign(key, slot->clone());
        }
        return clone;
    }

    bool parseStringList(PyObject *obj, std::vector<std::string> &out, std::string &error)
    {
        if (obj == Py_None)
        {
            return true;
        }
        PyObject *seq = PySequence_Fast(obj, "expected a list of strings");
        if (!seq)
        {
            error = "expected a list of strings";
            return false;
        }
        const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
        PyObject **items = PySequence_Fast_ITEMS(seq);
        out.reserve(static_cast<std::size_t>(count));
        for (Py_ssize_t i = 0; i < count; ++i)
        {
            PyObject *item = items[i];
            if (!PyUnicode_Check(item))
            {
                Py_DECREF(seq);
                error = "expected string item in list";
                return false;
            }
            const char *text = PyUnicode_AsUTF8(item);
            if (!text)
            {
                Py_DECREF(seq);
                error = "invalid string item in list";
                return false;
            }
            out.emplace_back(text);
        }
        Py_DECREF(seq);
        return true;
    }

    wolvrix::lib::LogLevel parseLogLevel(std::string_view text, bool &ok)
    {
        ok = true;
        std::string lowered(text);
        for (char &ch : lowered)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lowered == "trace")
        {
            return wolvrix::lib::LogLevel::Trace;
        }
        if (lowered == "debug")
        {
            return wolvrix::lib::LogLevel::Debug;
        }
        if (lowered == "info")
        {
            return wolvrix::lib::LogLevel::Info;
        }
        if (lowered == "warn" || lowered == "warning")
        {
            return wolvrix::lib::LogLevel::Warn;
        }
        if (lowered == "error")
        {
            return wolvrix::lib::LogLevel::Error;
        }
        if (lowered == "off")
        {
            return wolvrix::lib::LogLevel::Off;
        }
        ok = false;
        return wolvrix::lib::LogLevel::Warn;
    }

    wolvrix::lib::store::JsonPrintMode parseJsonMode(std::string_view text, bool &ok)
    {
        ok = true;
        std::string lowered(text);
        for (char &ch : lowered)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lowered == "compact")
        {
            return wolvrix::lib::store::JsonPrintMode::Compact;
        }
        if (lowered == "pretty")
        {
            return wolvrix::lib::store::JsonPrintMode::Pretty;
        }
        if (lowered == "pretty-compact" || lowered == "pretty_compact")
        {
            return wolvrix::lib::store::JsonPrintMode::PrettyCompact;
        }
        ok = false;
        return wolvrix::lib::store::JsonPrintMode::PrettyCompact;
    }

} // namespace wolvrix::app::pybind
