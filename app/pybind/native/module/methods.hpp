#ifndef WOLVRIX_APP_PYBIND_NATIVE_MODULE_METHODS_HPP
#define WOLVRIX_APP_PYBIND_NATIVE_MODULE_METHODS_HPP

#include <Python.h>

namespace wolvrix::app::pybind
{

    PyObject *py_create_session(PyObject *self, PyObject *args);
    PyObject *py_session_close(PyObject *self, PyObject *args);
    PyObject *py_session_contains(PyObject *self, PyObject *args);
    PyObject *py_session_keys(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_kind(PyObject *self, PyObject *args);
    PyObject *py_session_get_value(PyObject *self, PyObject *args);
    PyObject *py_session_export(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_put_python(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_delete(PyObject *self, PyObject *args);
    PyObject *py_session_copy(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_rename(PyObject *self, PyObject *args, PyObject *kwargs);

    PyObject *py_session_read_sv(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_read_json_file(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_load_json_text(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_clone_design(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_run_pass(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_lower_grhsim(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_run_grhsim_pass(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_load_grhsim(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_store_grhsim(PyObject *self, PyObject *args, PyObject *kwargs);

    PyObject *py_session_store_json(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_emit_sv(PyObject *self, PyObject *args, PyObject *kwargs);
    PyObject *py_session_emit_verilator_repcut_package(PyObject *self, PyObject *args, PyObject *kwargs);

    PyObject *py_list_passes(PyObject *self, PyObject *args);
    PyObject *py_list_grhsim_passes(PyObject *self, PyObject *args);

} // namespace wolvrix::app::pybind

#endif // WOLVRIX_APP_PYBIND_NATIVE_MODULE_METHODS_HPP
