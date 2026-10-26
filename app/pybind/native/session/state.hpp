#ifndef WOLVRIX_APP_PYBIND_NATIVE_SESSION_STATE_HPP
#define WOLVRIX_APP_PYBIND_NATIVE_SESSION_STATE_HPP

#include <Python.h>

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace slang::ast
{
    class Compilation;
}

namespace wolvrix::app::pybind
{

    inline constexpr const char *kSessionCapsuleName = "wolvrix.Session";

    struct DesignHandle
    {
        wolvrix::lib::grh::Design design;
        std::shared_ptr<slang::ast::Compilation> compilation;
    };

    struct PythonSessionValue
    {
        std::string kind;
        PyObject *object = nullptr;

        PythonSessionValue() = default;

        PythonSessionValue(std::string kindValue, PyObject *pyObject)
            : kind(std::move(kindValue)), object(pyObject)
        {
        }

        PythonSessionValue(const PythonSessionValue &) = delete;
        PythonSessionValue &operator=(const PythonSessionValue &) = delete;

        PythonSessionValue(PythonSessionValue &&other) noexcept
            : kind(std::move(other.kind)), object(other.object)
        {
            other.object = nullptr;
        }

        PythonSessionValue &operator=(PythonSessionValue &&other) noexcept
        {
            if (this == &other)
            {
                return *this;
            }
            Py_XDECREF(object);
            kind = std::move(other.kind);
            object = other.object;
            other.object = nullptr;
            return *this;
        }

        ~PythonSessionValue()
        {
            Py_XDECREF(object);
        }
    };

    struct SessionHandle
    {
        std::unordered_map<std::string, DesignHandle> designs;
        wolvrix::lib::transform::SessionStore nativeValues;
        std::unordered_map<std::string, PythonSessionValue> pythonValues;

        void clear()
        {
            designs.clear();
            nativeValues.clear();
            pythonValues.clear();
        }
    };

} // namespace wolvrix::app::pybind

#endif // WOLVRIX_APP_PYBIND_NATIVE_SESSION_STATE_HPP
