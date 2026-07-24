#include <Python.h>

#include "native/module/methods.hpp"

static PyMethodDef WolvrixMethods[] = {
    {"create_session", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_create_session), METH_NOARGS,
     "create_session() -> Session capsule"},
    {"session_close", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_close), METH_VARARGS,
     "session_close(session)"},
    {"session_contains", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_contains), METH_VARARGS,
     "session_contains(session, key) -> bool"},
    {"session_keys", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_keys), METH_VARARGS | METH_KEYWORDS,
     "session_keys(session, prefix=None, kind=None) -> list[str]"},
    {"session_kind", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_kind), METH_VARARGS,
     "session_kind(session, key) -> str"},
    {"session_get_value", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_get_value), METH_VARARGS,
     "session_get_value(session, key) -> (storage, kind, payload)"},
    {"session_export", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_export), METH_VARARGS | METH_KEYWORDS,
     "session_export(session, key, view='text') -> object"},
    {"session_put_python", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_put_python), METH_VARARGS | METH_KEYWORDS,
     "session_put_python(session, key, value, kind, replace=False)"},
    {"session_delete", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_delete), METH_VARARGS,
     "session_delete(session, key)"},
    {"session_copy", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_copy), METH_VARARGS | METH_KEYWORDS,
     "session_copy(session, src, dst, replace=False)"},
    {"session_rename", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_rename), METH_VARARGS | METH_KEYWORDS,
     "session_rename(session, src, dst, replace=False)"},
    {"session_read_sv", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_read_sv), METH_VARARGS | METH_KEYWORDS,
     "session_read_sv(session, path, out_design, slang_args=None, replace=False, log_level='info') -> (success, diagnostics)"},
    {"session_read_json_file", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_read_json_file), METH_VARARGS | METH_KEYWORDS,
     "session_read_json_file(session, path, out_design, replace=False) -> (success, diagnostics)"},
    {"session_load_json_text", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_load_json_text), METH_VARARGS | METH_KEYWORDS,
     "session_load_json_text(session, text, out_design, replace=False) -> (success, diagnostics)"},
    {"session_clone_design", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_clone_design), METH_VARARGS | METH_KEYWORDS,
     "session_clone_design(session, design, out_design, replace=False) -> (success, diagnostics)"},
    {"session_run_pass", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_run_pass), METH_VARARGS | METH_KEYWORDS,
     "session_run_pass(session, name, design, args=None, dryrun=False, log_level='warn') -> (success, changed, diagnostics)"},
    {"session_store_json", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_store_json), METH_VARARGS | METH_KEYWORDS,
     "session_store_json(session, design, output, mode='pretty-compact', top=None) -> (success, diagnostics)"},
    {"session_emit_sv", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_emit_sv), METH_VARARGS | METH_KEYWORDS,
     "session_emit_sv(session, design, output, top=None, split_modules=False) -> (success, diagnostics)"},
    {"session_emit_grhsim_cpp",
     reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_emit_grhsim_cpp),
     METH_VARARGS | METH_KEYWORDS,
     "session_emit_grhsim_cpp(session, design, output, top=None, max_cpp_file_bytes=None, sched_batch_max_ops=None, sched_batch_max_estimated_lines=None, sched_batch_target_count=None, sched_batches_per_cpp=None, emit_parallelism=None, waveform=None, perf=None, input_fullpass_specialization=None, posedge_fullpass_specialization=None, full_active_word_consume=None, pure_event_compute_word_bypass=None, pure_event_compute_word_profile=None, pure_event_word_pack_policy=None, pure_event_word_pack_max_moved_supernode_ppm=None, pure_event_word_pack_max_changed_word_ppm=None, direct_single_writer_state_reads=None, active_mask_gap_pack_policy=None, deferred_activation_forward_policy=None, deferred_activation_forward_profile_path=None, same_batch_activation_cohort_policy=None, same_batch_activation_cohort_profile_path=None, commit_exact_event_policy=None) -> (success, diagnostics)"},
    {"session_emit_verilator_repcut_package",
     reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_session_emit_verilator_repcut_package),
     METH_VARARGS | METH_KEYWORDS,
     "session_emit_verilator_repcut_package(session, design, output, top=None) -> (success, diagnostics)"},
    {"list_passes", reinterpret_cast<PyCFunction>(wolvrix::app::pybind::py_list_passes), METH_NOARGS,
     "list_passes() -> list[str]"},
    {nullptr, nullptr, 0, nullptr},
};

static PyModuleDef WolvrixModule = {
    PyModuleDef_HEAD_INIT,
    "_wolvrix",
    "Wolvrix native bindings",
    -1,
    WolvrixMethods,
};

PyMODINIT_FUNC PyInit__wolvrix(void)
{
    return PyModule_Create(&WolvrixModule);
}
