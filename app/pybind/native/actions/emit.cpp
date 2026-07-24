#include "native/module/methods.hpp"

#include "emit/grhsim_cpp.hpp"
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

    PyObject *py_session_emit_grhsim_cpp(PyObject * /*self*/, PyObject *args, PyObject *kwargs)
    {
        PyObject *sessionObj = nullptr;
        const char *designKey = nullptr;
        const char *output = nullptr;
        PyObject *topListObj = Py_None;
        PyObject *maxCppFileBytesObj = Py_None;
        PyObject *schedBatchMaxOpsObj = Py_None;
        PyObject *schedBatchMaxEstimatedLinesObj = Py_None;
        PyObject *schedBatchTargetCountObj = Py_None;
        PyObject *schedBatchesPerCppObj = Py_None;
        PyObject *emitParallelismObj = Py_None;
        const char *waveformMode = nullptr;
        const char *perfMode = nullptr;
        PyObject *inputFullpassSpecializationObj = Py_None;
        PyObject *posedgeFullpassSpecializationObj = Py_None;
        PyObject *fullActiveWordConsumeObj = Py_None;
        PyObject *directSingleWriterStateReadsObj = Py_None;
        PyObject *pureEventComputeWordBypassObj = Py_None;
        PyObject *pureEventComputeWordProfileObj = Py_None;
        const char *pureEventWordPackPolicy = nullptr;
        PyObject *pureEventWordPackMaxMovedSupernodePpmObj = Py_None;
        PyObject *pureEventWordPackMaxChangedWordPpmObj = Py_None;
        const char *activeMaskGapPackPolicy = nullptr;
        const char *deferredActivationForwardPolicy = nullptr;
        const char *deferredActivationForwardProfilePath = nullptr;
        const char *sameBatchActivationCohortPolicy = nullptr;
        const char *sameBatchActivationCohortProfilePath = nullptr;
        const char *commitExactEventPolicy = nullptr;
        static const char *kwlist[] = {"session",
                                       "design",
                                       "output",
                                       "top",
                                       "max_cpp_file_bytes",
                                       "sched_batch_max_ops",
                                       "sched_batch_max_estimated_lines",
                                       "sched_batch_target_count",
                                       "sched_batches_per_cpp",
                                       "emit_parallelism",
                                       "waveform",
                                       "perf",
                                       "input_fullpass_specialization",
                                       "posedge_fullpass_specialization",
                                       "full_active_word_consume",
                                       "pure_event_compute_word_bypass",
                                       "pure_event_compute_word_profile",
                                       "pure_event_word_pack_policy",
                                       "pure_event_word_pack_max_moved_supernode_ppm",
                                       "pure_event_word_pack_max_changed_word_ppm",
                                       "direct_single_writer_state_reads",
                                       "active_mask_gap_pack_policy",
                                       "deferred_activation_forward_policy",
                                       "deferred_activation_forward_profile_path",
                                       "same_batch_activation_cohort_policy",
                                       "same_batch_activation_cohort_profile_path",
                                       "commit_exact_event_policy",
                                       nullptr};
        if (!PyArg_ParseTupleAndKeywords(args,
                                         kwargs,
                                         "Oss|OOOOOOOssOOOOOsOOOzzzzzz",
                                         const_cast<char **>(kwlist),
                                         &sessionObj,
                                         &designKey,
                                         &output,
                                         &topListObj,
                                         &maxCppFileBytesObj,
                                         &schedBatchMaxOpsObj,
                                         &schedBatchMaxEstimatedLinesObj,
                                         &schedBatchTargetCountObj,
                                         &schedBatchesPerCppObj,
                                         &emitParallelismObj,
                                         &waveformMode,
                                         &perfMode,
                                         &inputFullpassSpecializationObj,
                                         &posedgeFullpassSpecializationObj,
                                         &fullActiveWordConsumeObj,
                                         &pureEventComputeWordBypassObj,
                                         &pureEventComputeWordProfileObj,
                                         &pureEventWordPackPolicy,
                                         &pureEventWordPackMaxMovedSupernodePpmObj,
                                         &pureEventWordPackMaxChangedWordPpmObj,
                                         &directSingleWriterStateReadsObj,
                                         &activeMaskGapPackPolicy,
                                         &deferredActivationForwardPolicy,
                                         &deferredActivationForwardProfilePath,
                                         &sameBatchActivationCohortPolicy,
                                         &sameBatchActivationCohortProfilePath,
                                         &commitExactEventPolicy))
        {
            return nullptr;
        }
        if (activeMaskGapPackPolicy != nullptr)
        {
            const std::string policy(activeMaskGapPackPolicy);
            if (policy != "off" && policy != "probe" && policy != "targeted-direct" &&
                policy != "targeted-table-contiguous" && policy != "targeted-table-gap")
            {
                PyErr_SetString(PyExc_ValueError,
                                "active_mask_gap_pack_policy must be one of: "
                                "off, probe, targeted-direct, targeted-table-contiguous, "
                                "targeted-table-gap");
                return nullptr;
            }
        }
        if (deferredActivationForwardPolicy != nullptr)
        {
            const std::string policy(deferredActivationForwardPolicy);
            if (policy != "off" && policy != "probe" && policy != "cofire-probe" &&
                policy != "cofire-strict" && policy != "cofire-strict-extended")
            {
                PyErr_SetString(PyExc_ValueError,
                                "deferred_activation_forward_policy must be one of: off, probe, cofire-probe, cofire-strict, cofire-strict-extended");
                return nullptr;
            }
        }
        if (sameBatchActivationCohortPolicy != nullptr)
        {
            const std::string policy(sameBatchActivationCohortPolicy);
            if (policy != "off" && policy != "probe" && policy != "strict")
            {
                PyErr_SetString(PyExc_ValueError,
                                "same_batch_activation_cohort_policy must be one of: "
                                "off, probe, strict");
                return nullptr;
            }
        }
        if (commitExactEventPolicy != nullptr)
        {
            const std::string policy(commitExactEventPolicy);
            if (policy != "off" && policy != "targeted-cold-layout")
            {
                PyErr_SetString(PyExc_ValueError,
                                "commit_exact_event_policy must be one of: "
                                "off, targeted-cold-layout");
                return nullptr;
            }
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
        wolvrix::lib::emit::EmitGrhSimCpp emitter(&diagnostics);
        wolvrix::lib::emit::EmitOptions options;
        const std::filesystem::path outPath(output);
        if (outPath.empty())
        {
            PyErr_SetString(PyExc_ValueError,
                            "emit_grhsim_cpp(...) expects an output directory");
            return nullptr;
        }
        options.outputDir = outPath.string();
        options.topOverrides = std::move(topNames);
        options.session = &session->nativeValues;
        if (options.topOverrides.size() == 1)
        {
            options.sessionPathPrefix = options.topOverrides.front();
        }
        if (maxCppFileBytesObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(maxCppFileBytesObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError, "max_cpp_file_bytes must be a non-negative integer");
                return nullptr;
            }
            options.maxOutputFileBytes = static_cast<std::uint64_t>(parsed);
        }
        if (schedBatchMaxOpsObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(schedBatchMaxOpsObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError, "sched_batch_max_ops must be a non-negative integer");
                return nullptr;
            }
            options.attributes["sched_batch_max_ops"] = std::to_string(parsed);
        }
        if (schedBatchMaxEstimatedLinesObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(schedBatchMaxEstimatedLinesObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError,
                                "sched_batch_max_estimated_lines must be a non-negative integer");
                return nullptr;
            }
            options.attributes["sched_batch_max_estimated_lines"] = std::to_string(parsed);
        }
        if (schedBatchTargetCountObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(schedBatchTargetCountObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError,
                                "sched_batch_target_count must be a non-negative integer");
                return nullptr;
            }
            options.attributes["sched_batch_target_count"] = std::to_string(parsed);
        }
        if (schedBatchesPerCppObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(schedBatchesPerCppObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError,
                                "sched_batches_per_cpp must be a non-negative integer");
                return nullptr;
            }
            options.attributes["sched_batches_per_cpp"] = std::to_string(parsed);
        }
        if (emitParallelismObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(emitParallelismObj);
            if (PyErr_Occurred())
            {
                PyErr_SetString(PyExc_ValueError, "emit_parallelism must be a non-negative integer");
                return nullptr;
            }
            options.attributes["emit_parallelism"] = std::to_string(parsed);
        }
        if (waveformMode != nullptr)
        {
            options.attributes["waveform"] = waveformMode;
        }
        if (perfMode != nullptr)
        {
            options.attributes["perf"] = perfMode;
        }
        if (inputFullpassSpecializationObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(inputFullpassSpecializationObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["input_fullpass_specialization"] = enabled != 0 ? "1" : "0";
        }
        if (posedgeFullpassSpecializationObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(posedgeFullpassSpecializationObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["posedge_fullpass_specialization"] = enabled != 0 ? "1" : "0";
        }
        if (fullActiveWordConsumeObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(fullActiveWordConsumeObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["full_active_word_consume"] = enabled != 0 ? "1" : "0";
        }
        if (directSingleWriterStateReadsObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(directSingleWriterStateReadsObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["direct_single_writer_state_reads"] = enabled != 0 ? "1" : "0";
        }
        if (pureEventComputeWordBypassObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(pureEventComputeWordBypassObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["pure_event_compute_word_bypass"] = enabled != 0 ? "1" : "0";
        }
        if (pureEventComputeWordProfileObj != Py_None)
        {
            const int enabled = PyObject_IsTrue(pureEventComputeWordProfileObj);
            if (enabled < 0)
            {
                return nullptr;
            }
            options.attributes["pure_event_compute_word_profile"] = enabled != 0 ? "1" : "0";
        }
        if (pureEventWordPackPolicy != nullptr)
        {
            options.attributes["pure_event_word_pack_policy"] = pureEventWordPackPolicy;
        }
        if (pureEventWordPackMaxMovedSupernodePpmObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(pureEventWordPackMaxMovedSupernodePpmObj);
            if (PyErr_Occurred() || parsed > 1000000ULL)
            {
                PyErr_SetString(PyExc_ValueError,
                                "pure_event_word_pack_max_moved_supernode_ppm must be an integer between 0 and 1000000");
                return nullptr;
            }
            options.attributes["pure_event_word_pack_max_moved_supernode_ppm"] = std::to_string(parsed);
        }
        if (pureEventWordPackMaxChangedWordPpmObj != Py_None)
        {
            const unsigned long long parsed = PyLong_AsUnsignedLongLong(pureEventWordPackMaxChangedWordPpmObj);
            if (PyErr_Occurred() || parsed > 1000000ULL)
            {
                PyErr_SetString(PyExc_ValueError,
                                "pure_event_word_pack_max_changed_word_ppm must be an integer between 0 and 1000000");
                return nullptr;
            }
            options.attributes["pure_event_word_pack_max_changed_word_ppm"] = std::to_string(parsed);
        }
        if (activeMaskGapPackPolicy != nullptr)
        {
            options.attributes["active_mask_gap_pack_policy"] = activeMaskGapPackPolicy;
        }
        if (deferredActivationForwardPolicy != nullptr)
        {
            options.attributes["deferred_activation_forward_policy"] =
                deferredActivationForwardPolicy;
        }
        if (deferredActivationForwardProfilePath != nullptr)
        {
            options.attributes["deferred_activation_forward_profile_path"] =
                deferredActivationForwardProfilePath;
        }
        if (sameBatchActivationCohortPolicy != nullptr)
        {
            options.attributes["same_batch_activation_cohort_policy"] =
                sameBatchActivationCohortPolicy;
        }
        if (sameBatchActivationCohortProfilePath != nullptr)
        {
            options.attributes["same_batch_activation_cohort_profile_path"] =
                sameBatchActivationCohortProfilePath;
        }
        if (commitExactEventPolicy != nullptr)
        {
            options.attributes["commit_exact_event_policy"] = commitExactEventPolicy;
        }

        const auto result = emitter.emit(*design, options);
        return makeActionResult(result.success && !diagnostics.hasError(),
                                diagnostics.messages(),
                                sessionDesignSourceManager(*session, designKey));
    }

} // namespace wolvrix::app::pybind
