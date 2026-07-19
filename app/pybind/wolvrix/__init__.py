from __future__ import annotations

from dataclasses import dataclass
import sys
from typing import Any

from . import _wolvrix as _native
from .adapters import adapt_session_value

__all__ = [
    "OpaqueValue",
    "Session",
    "format_diagnostics",
    "list_passes",
    "print_diagnostics",
]


@dataclass(frozen=True)
class OpaqueValue:
    key: str
    kind: str

    def __repr__(self) -> str:
        return f"OpaqueValue(key={self.key!r}, kind={self.kind!r})"


class Session:
    def __init__(self) -> None:
        self._capsule = _native.create_session()
        self._diagnostics_print_min_level = "warning"
        self._diagnostics_raise_min_level = "error"
        self._log_level = "warn"
        self._history: list[dict[str, Any]] = []

    def __enter__(self) -> "Session":
        self._ensure_open()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def close(self) -> None:
        if self._capsule is None:
            return
        _native.session_close(self._capsule)
        self._capsule = None

    def __contains__(self, key: str) -> bool:
        self._ensure_open()
        return bool(_native.session_contains(self._capsule, key))

    def keys(self, prefix: str | None = None, kind: str | None = None) -> list[str]:
        self._ensure_open()
        return list(_native.session_keys(self._capsule, prefix=prefix, kind=kind))

    def kind(self, key: str) -> str:
        self._ensure_open()
        return str(_native.session_kind(self._capsule, key))

    def get(self, key: str):
        self._ensure_open()
        storage, kind, payload = _native.session_get_value(self._capsule, key)
        adapted = adapt_session_value(self, key=key, storage=storage, kind=kind, payload=payload)
        if adapted is not None:
            return adapted
        if storage == "python":
            return payload
        return OpaqueValue(key=key, kind=kind)

    def put(self, key: str, value, *, kind: str, replace: bool = False) -> None:
        self._ensure_open()
        if isinstance(value, OpaqueValue):
            raise TypeError("opaque session values cannot be re-put from Python; use copy/rename instead")
        _native.session_put_python(self._capsule, key=key, value=value, kind=kind, replace=replace)

    def delete(self, key: str) -> None:
        self._ensure_open()
        _native.session_delete(self._capsule, key)

    def rename(self, src: str, dst: str, *, replace: bool = False) -> None:
        self._ensure_open()
        _native.session_rename(self._capsule, src=src, dst=dst, replace=replace)

    def copy(self, src: str, dst: str, *, replace: bool = False) -> None:
        self._ensure_open()
        _native.session_copy(self._capsule, src=src, dst=dst, replace=replace)

    @property
    def diagnostics_print_min_level(self) -> str:
        return self._diagnostics_print_min_level

    @diagnostics_print_min_level.setter
    def diagnostics_print_min_level(self, level: str) -> None:
        self._diagnostics_print_min_level = _normalize_diagnostics_print_min_level(level)

    @property
    def diagnostics_raise_min_level(self) -> str:
        return self._diagnostics_raise_min_level

    @diagnostics_raise_min_level.setter
    def diagnostics_raise_min_level(self, level: str) -> None:
        self._diagnostics_raise_min_level = _normalize_diagnostics_raise_min_level(level)

    @property
    def log_level(self) -> str:
        return self._log_level

    @log_level.setter
    def log_level(self, level: str) -> None:
        self._log_level = _normalize_log_level(level)

    def history(self) -> list[dict]:
        return [dict(item) for item in self._history]

    def format_diagnostics(self, diagnostics: list[dict], *, min_level: str = "debug") -> str:
        return format_diagnostics(diagnostics, min_level=min_level)

    def print_diagnostics(self, diagnostics: list[dict], *, min_level: str = "debug", file=None) -> None:
        print_diagnostics(diagnostics, min_level=min_level, file=file)

    def read_sv(
        self,
        path: str | None,
        *,
        out_design: str = "design.main",
        slang_args: list[str] | None = None,
        replace: bool = False,
    ) -> list[dict]:
        self._ensure_open()
        success, diagnostics = _native.session_read_sv(
            self._capsule,
            path=path,
            out_design=out_design,
            slang_args=slang_args or [],
            replace=replace,
            log_level=self._log_level,
        )
        return self._complete_action(
            "read_sv",
            diagnostics,
            success=bool(success),
            out_design=out_design,
            path=path,
        )

    def read_json_file(
        self,
        path: str,
        *,
        out_design: str = "design.main",
        replace: bool = False,
    ) -> list[dict]:
        self._ensure_open()
        success, diagnostics = _native.session_read_json_file(
            self._capsule,
            path=path,
            out_design=out_design,
            replace=replace,
        )
        return self._complete_action(
            "read_json_file",
            diagnostics,
            success=bool(success),
            out_design=out_design,
            path=path,
        )

    def load_json_text(
        self,
        text: str,
        *,
        out_design: str = "design.main",
        replace: bool = False,
    ) -> list[dict]:
        self._ensure_open()
        success, diagnostics = _native.session_load_json_text(
            self._capsule,
            text=text,
            out_design=out_design,
            replace=replace,
        )
        return self._complete_action(
            "load_json_text",
            diagnostics,
            success=bool(success),
            out_design=out_design,
        )

    def clone_design(self, *, design: str, out_design: str, replace: bool = False) -> list[dict]:
        self._ensure_open()
        success, diagnostics = _native.session_clone_design(
            self._capsule,
            design=design,
            out_design=out_design,
            replace=replace,
        )
        return self._complete_action(
            "clone_design",
            diagnostics,
            success=bool(success),
            design=design,
            out_design=out_design,
        )

    def run_pass(
        self,
        name: str,
        *,
        design: str,
        args: list[str] | None = None,
        dryrun: bool = False,
        keep_declared_symbols: bool = True,
        **named_args,
    ) -> list[dict]:
        self._ensure_open()
        canonical_name, pass_args = _compile_run_pass(name, args or [], named_args)
        success, changed, diagnostics = _native.session_run_pass(
            self._capsule,
            name=canonical_name,
            design=design,
            args=pass_args,
            dryrun=dryrun,
            log_level=self._log_level,
            keep_declared_symbols=bool(keep_declared_symbols),
        )
        return self._complete_action(
            "run_pass",
            diagnostics,
            success=bool(success),
            changed=bool(changed),
            name=canonical_name,
            design=design,
            dryrun=dryrun,
        )

    def store_json(
        self,
        *,
        design: str,
        output: str,
        mode: str = "pretty-compact",
        top: list[str] | None = None,
    ) -> list[dict]:
        self._ensure_open()
        success, diagnostics = _native.session_store_json(
            self._capsule,
            design=design,
            output=output,
            mode=mode,
            top=top or [],
        )
        return self._complete_action(
            "store_json",
            diagnostics,
            success=bool(success),
            design=design,
            output=output,
        )

    def emit_sv(
        self,
        *,
        design: str,
        output: str,
        top: list[str] | None = None,
        split_modules: bool = False,
        **named_args,
    ) -> list[dict]:
        self._ensure_open()
        _compile_emit_sv_kwargs(named_args)
        success, diagnostics = _native.session_emit_sv(
            self._capsule,
            design=design,
            output=output,
            top=top or [],
            split_modules=split_modules,
        )
        return self._complete_action(
            "emit_sv",
            diagnostics,
            success=bool(success),
            design=design,
            output=output,
        )

    def emit_grhsim_cpp(
        self,
        *,
        design: str,
        output: str,
        top: list[str] | None = None,
        max_cpp_file_bytes: int | None = None,
        sched_batch_max_ops: int | None = None,
        sched_batch_max_estimated_lines: int | None = None,
        sched_batch_target_count: int | None = None,
        sched_batches_per_cpp: int | None = None,
        emit_parallelism: int | None = None,
        perf: str | None = None,
        input_fullpass_specialization: bool | None = None,
        posedge_fullpass_specialization: bool | None = None,
        full_active_word_consume: bool | None = None,
        direct_single_writer_state_reads: bool | None = None,
        pure_event_compute_word_bypass: bool | None = None,
        pure_event_compute_word_profile: bool | None = None,
        pure_event_word_pack_policy: str | None = None,
        pure_event_word_pack_max_moved_supernode_ppm: int | None = None,
        pure_event_word_pack_max_changed_word_ppm: int | None = None,
        active_mask_gap_pack_policy: str | None = None,
        deferred_activation_forward_policy: str | None = None,
        deferred_activation_forward_profile_path: str | None = None,
        same_batch_activation_cohort_policy: str | None = None,
        same_batch_activation_cohort_profile_path: str | None = None,
        **named_args,
    ) -> list[dict]:
        self._ensure_open()
        if (max_cpp_file_bytes is not None or sched_batch_max_ops is not None or
                sched_batch_max_estimated_lines is not None or sched_batch_target_count is not None or
                sched_batches_per_cpp is not None or emit_parallelism is not None or perf is not None or
                input_fullpass_specialization is not None or posedge_fullpass_specialization is not None or
                full_active_word_consume is not None or direct_single_writer_state_reads is not None or
                pure_event_compute_word_bypass is not None or
                pure_event_compute_word_profile is not None or pure_event_word_pack_policy is not None or
                pure_event_word_pack_max_moved_supernode_ppm is not None or
                pure_event_word_pack_max_changed_word_ppm is not None or
                active_mask_gap_pack_policy is not None or
                deferred_activation_forward_policy is not None or
                deferred_activation_forward_profile_path is not None or
                same_batch_activation_cohort_policy is not None or
                same_batch_activation_cohort_profile_path is not None):
            named_args = dict(named_args)
        if max_cpp_file_bytes is not None:
            named_args["max_cpp_file_bytes"] = max_cpp_file_bytes
        if sched_batch_max_ops is not None:
            named_args["sched_batch_max_ops"] = sched_batch_max_ops
        if sched_batch_max_estimated_lines is not None:
            named_args["sched_batch_max_estimated_lines"] = sched_batch_max_estimated_lines
        if sched_batch_target_count is not None:
            named_args["sched_batch_target_count"] = sched_batch_target_count
        if sched_batches_per_cpp is not None:
            named_args["sched_batches_per_cpp"] = sched_batches_per_cpp
        if emit_parallelism is not None:
            named_args["emit_parallelism"] = emit_parallelism
        if perf is not None:
            named_args["perf"] = perf
        if input_fullpass_specialization is not None:
            named_args["input_fullpass_specialization"] = input_fullpass_specialization
        if posedge_fullpass_specialization is not None:
            named_args["posedge_fullpass_specialization"] = posedge_fullpass_specialization
        if full_active_word_consume is not None:
            named_args["full_active_word_consume"] = full_active_word_consume
        if direct_single_writer_state_reads is not None:
            named_args["direct_single_writer_state_reads"] = direct_single_writer_state_reads
        if pure_event_compute_word_bypass is not None:
            named_args["pure_event_compute_word_bypass"] = pure_event_compute_word_bypass
        if pure_event_compute_word_profile is not None:
            named_args["pure_event_compute_word_profile"] = pure_event_compute_word_profile
        if pure_event_word_pack_policy is not None:
            named_args["pure_event_word_pack_policy"] = pure_event_word_pack_policy
        if pure_event_word_pack_max_moved_supernode_ppm is not None:
            named_args["pure_event_word_pack_max_moved_supernode_ppm"] = pure_event_word_pack_max_moved_supernode_ppm
        if pure_event_word_pack_max_changed_word_ppm is not None:
            named_args["pure_event_word_pack_max_changed_word_ppm"] = pure_event_word_pack_max_changed_word_ppm
        if active_mask_gap_pack_policy is not None:
            named_args["active_mask_gap_pack_policy"] = active_mask_gap_pack_policy
        if deferred_activation_forward_policy is not None:
            named_args["deferred_activation_forward_policy"] = deferred_activation_forward_policy
        if deferred_activation_forward_profile_path is not None:
            named_args["deferred_activation_forward_profile_path"] = deferred_activation_forward_profile_path
        if same_batch_activation_cohort_policy is not None:
            named_args["same_batch_activation_cohort_policy"] = same_batch_activation_cohort_policy
        if same_batch_activation_cohort_profile_path is not None:
            named_args["same_batch_activation_cohort_profile_path"] = same_batch_activation_cohort_profile_path
        _compile_emit_grhsim_cpp_kwargs(named_args)
        success, diagnostics = _native.session_emit_grhsim_cpp(
            self._capsule,
            design=design,
            output=output,
            top=top or [],
            **named_args,
        )
        return self._complete_action(
            "emit_grhsim_cpp",
            diagnostics,
            success=bool(success),
            design=design,
            output=output,
        )

    def emit_verilator_repcut_package(
        self,
        *,
        design: str,
        output: str,
        top: list[str] | None = None,
        **named_args,
    ) -> list[dict]:
        self._ensure_open()
        _compile_emit_verilator_repcut_package_kwargs(named_args)
        success, diagnostics = _native.session_emit_verilator_repcut_package(
            self._capsule,
            design=design,
            output=output,
            top=top or [],
        )
        return self._complete_action(
            "emit_verilator_repcut_package",
            diagnostics,
            success=bool(success),
            design=design,
            output=output,
        )

    def _complete_action(self, action: str, diagnostics: list[dict], *, success: bool, **details) -> list[dict]:
        result = list(diagnostics or [])
        entry = {"action": action, "success": bool(success), "diagnostics": result}
        entry.update(details)
        self._history.append(entry)
        if not success and not result:
            exc = RuntimeError(f"{action} failed")
            setattr(exc, "diagnostics", result)
            raise exc
        if result:
            print_diagnostics(result, min_level=self._diagnostics_print_min_level)
        if _should_raise(result, self._diagnostics_raise_min_level):
            _raise_with_diagnostics(result)
        return result

    def _ensure_open(self) -> None:
        if self._capsule is None:
            raise RuntimeError("session is closed")


def list_passes() -> list[str]:
    return list(_native.list_passes())


def _compile_run_pass(name: str, args: list[str], named: dict[str, Any]) -> tuple[str, list[str]]:
    canonical_name = _canonical_pass_name(name)
    compiled = list(args)
    if canonical_name == "hier-flatten":
        compiled.extend(_compile_hier_flatten_kwargs(named))
    elif canonical_name == "comb-lane-pack":
        compiled.extend(_compile_comb_lane_pack_kwargs(named))
    elif canonical_name == "comb-loop-elim":
        compiled.extend(_compile_comb_loop_elim_kwargs(named))
    elif canonical_name == "mem-to-reg":
        compiled.extend(_compile_mem_to_reg_kwargs(named))
    elif canonical_name == "reg-to-mem":
        compiled.extend(_compile_reg_to_mem_kwargs(named))
    elif canonical_name == "simplify":
        compiled.extend(_compile_simplify_kwargs(named))
    elif canonical_name == "stats":
        compiled.extend(_compile_stats_kwargs(named))
    elif canonical_name == "repcut":
        compiled.extend(_compile_repcut_kwargs(named))
    elif canonical_name == "activity-schedule":
        compiled.extend(_compile_activity_schedule_kwargs(named))
    elif named:
        keys = ", ".join(sorted(named))
        raise TypeError(f"{name} does not accept named pass parameters: {keys}")
    return canonical_name, compiled


def _canonical_pass_name(name: str) -> str:
    value = str(name).strip()
    return value.lower().replace("_", "-")


def _pop_named(mapping: dict[str, Any], key: str, default: Any = None) -> Any:
    if key in mapping:
        return mapping.pop(key)
    return default


def _ensure_no_extra_named(pass_name: str, named: dict[str, Any]) -> None:
    if not named:
        return
    keys = ", ".join(sorted(named))
    raise TypeError(f"{pass_name} got unexpected named parameters: {keys}")


def _compile_hier_flatten_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    if _pop_named(local, "preserve_modules", False):
        out.append("-preserve-modules")
    sym_protect = _pop_named(local, "sym_protect", None)
    if sym_protect is not None:
        out.extend(["-sym-protect", str(sym_protect)])
    _ensure_no_extra_named("hier-flatten", local)
    return out


def _compile_comb_loop_elim_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    max_analysis_nodes = _pop_named(local, "max_analysis_nodes", None)
    if max_analysis_nodes is not None:
        out.extend(["-max-analysis-nodes", str(max_analysis_nodes)])
    num_threads = _pop_named(local, "num_threads", None)
    if num_threads is not None:
        out.extend(["-num-threads", str(num_threads)])
    fix_false_loops = _pop_named(local, "fix_false_loops", None)
    if fix_false_loops is True:
        out.append("-fix-false-loops")
    elif fix_false_loops is False:
        out.append("-no-fix-false-loops")
    max_fix_iterations = _pop_named(local, "max_fix_iterations", None)
    if max_fix_iterations is not None:
        out.extend(["-max-fix-iterations", str(max_fix_iterations)])
    if _pop_named(local, "fail_on_true_loop", False):
        out.append("-fail-on-true-loop")
    output_key = _pop_named(local, "out_comb_loop_report", None)
    if output_key is not None:
        out.extend(["-output-key", str(output_key)])
    _ensure_no_extra_named("comb-loop-elim", local)
    return out


def _compile_comb_lane_pack_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    min_group_size = _pop_named(local, "min_group_size", None)
    if min_group_size is not None:
        out.extend(["-min-group-size", str(min_group_size)])
    max_group_size = _pop_named(local, "max_group_size", None)
    if max_group_size is not None:
        out.extend(["-max-group-size", str(max_group_size)])
    min_packed_width = _pop_named(local, "min_packed_width", None)
    if min_packed_width is not None:
        out.extend(["-min-packed-width", str(min_packed_width)])
    max_packed_width = _pop_named(local, "max_packed_width", None)
    if max_packed_width is not None:
        out.extend(["-max-packed-width", str(max_packed_width)])
    max_tree_nodes = _pop_named(local, "max_tree_nodes", None)
    if max_tree_nodes is not None:
        out.extend(["-max-tree-nodes", str(max_tree_nodes)])
    max_root_gap = _pop_named(local, "max_root_gap", None)
    if max_root_gap is not None:
        out.extend(["-max-root-gap", str(max_root_gap)])
    require_declared_roots = _pop_named(local, "require_declared_roots", None)
    if require_declared_roots is not None:
        out.extend(["-require-declared-roots", "true" if require_declared_roots else "false"])
    enable_declared_roots = _pop_named(local, "enable_declared_roots", None)
    if enable_declared_roots is not None:
        out.extend(["-enable-declared-roots", "true" if enable_declared_roots else "false"])
    enable_storage_data_roots = _pop_named(local, "enable_storage_data_roots", None)
    if enable_storage_data_roots is not None:
        out.extend(["-enable-storage-data-roots", "true" if enable_storage_data_roots else "false"])
    enable_mux = _pop_named(local, "enable_mux", None)
    if enable_mux is not None:
        out.extend(["-enable-mux", "true" if enable_mux else "false"])
    output_key = _pop_named(local, "out_comb_lane_pack_report", None)
    if output_key is not None:
        out.extend(["-output-key", str(output_key)])
    _ensure_no_extra_named("comb-lane-pack", local)
    return out


def _compile_mem_to_reg_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    row_limit = _pop_named(local, "row_limit", None)
    if row_limit is not None:
        out.extend(["-row-limit", str(row_limit)])
    strict_init = _pop_named(local, "strict_init", None)
    if strict_init is True:
        out.append("-strict-init")
    elif strict_init is False:
        out.append("-no-strict-init")
    _ensure_no_extra_named("mem-to-reg", local)
    return out


def _compile_reg_to_mem_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    intent = _pop_named(local, "intent", None)
    if intent is True:
        out.append("-intent")
    elif intent is False:
        out.append("-no-intent")
    true_merge = _pop_named(local, "true_merge", None)
    if true_merge is True:
        out.append("-true-merge")
    elif true_merge is False:
        out.append("-no-true-merge")
    ordered_writes = _pop_named(local, "ordered_writes", None)
    if ordered_writes is True:
        out.append("-ordered-writes")
    elif ordered_writes is False:
        out.append("-no-ordered-writes")
    decoded_write_storage = _pop_named(local, "decoded_write_storage", None)
    if decoded_write_storage is True:
        out.append("-decoded-write-storage")
    elif decoded_write_storage is False:
        out.append("-no-decoded-write-storage")
    min_element_count = _pop_named(local, "min_element_count", None)
    if min_element_count is not None:
        out.extend(["-min-element-count", str(min_element_count)])
    _ensure_no_extra_named("reg-to-mem", local)
    return out


def _compile_simplify_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    max_iter = _pop_named(local, "max_iter", None)
    if max_iter is not None:
        out.extend(["-max-iter", str(max_iter)])
    x_fold = _pop_named(local, "x_fold", None)
    if x_fold is not None:
        out.extend(["-x-fold", str(x_fold)])
    semantics = _pop_named(local, "semantics", None)
    if semantics is not None:
        out.extend(["-semantics", str(semantics)])
    _ensure_no_extra_named("simplify", local)
    return out


def _compile_stats_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    stats_key = _pop_named(local, "out_stats", None)
    if stats_key is not None:
        out.extend(["-output-key", str(stats_key)])
    _ensure_no_extra_named("stats", local)
    return out


def _compile_repcut_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []
    path = _pop_named(local, "path", None)
    if path is not None:
        out.extend(["-path", str(path)])
    partition_count = _pop_named(local, "partition_count", None)
    if partition_count is not None:
        out.extend(["-partition-count", str(partition_count)])
    imbalance_factor = _pop_named(local, "imbalance_factor", None)
    if imbalance_factor is not None:
        out.extend(["-imbalance-factor", str(imbalance_factor)])
    work_dir = _pop_named(local, "work_dir", None)
    if work_dir is not None:
        out.extend(["-work-dir", str(work_dir)])
    partitioner = _pop_named(local, "partitioner", None)
    if partitioner is not None:
        out.extend(["-partitioner", str(partitioner)])
    preset = _pop_named(local, "mtkahypar_preset", None)
    if preset is not None:
        out.extend(["-mtkahypar-preset", str(preset)])
    threads = _pop_named(local, "mtkahypar_threads", None)
    if threads is not None:
        out.extend(["-mtkahypar-threads", str(threads)])
    keep_intermediate_files = _pop_named(local, "keep_intermediate_files", None)
    if keep_intermediate_files:
        out.append("-keep-intermediate-files")
    _ensure_no_extra_named("repcut", local)
    return out


def _compile_activity_schedule_kwargs(named: dict[str, Any]) -> list[str]:
    local = dict(named)
    out: list[str] = []

    string_options = [
        ("path", "-path"),
        ("export_compute_dag", "-export-compute-dag"),
        ("final_topo_policy", "-final-topo-policy"),
        ("final_fanin_pullback_policy", "-final-fanin-pullback-policy"),
        ("final_terminal_pushforward_policy", "-final-terminal-pushforward-policy"),
        ("final_terminal_pushforward_profile_path", "-final-terminal-pushforward-profile-path"),
        ("final_shared_input_peer_policy", "-final-shared-input-peer-policy"),
        ("final_shared_input_peer_profile_path", "-final-shared-input-peer-profile-path"),
        ("final_sibling_fusion_policy", "-final-sibling-fusion-policy"),
        ("post_dp_refine_policy", "-post-dp-refine-policy"),
        ("kahn_level_pack_policy", "-kahn-level-pack-policy"),
        ("local_shared_compute_common_owner_policy", "-local-shared-compute-common-owner-policy"),
    ]
    size_options = [
        ("max_op_in_compute_supernode", "-max-op-in-compute-supernode"),
        ("max_op_in_compute_node", "-max-op-in-compute-node"),
        ("max_op_in_commit_supernode", "-max-op-in-commit-supernode"),
        ("local_shared_compute_max_fanout", "-local-shared-compute-max-fanout"),
        ("local_shared_compute_max_width", "-local-shared-compute-max-width"),
        ("local_shared_compute_max_clones", "-local-shared-compute-max-clones"),
        ("local_shared_compute_max_cloned_op_ppm", "-local-shared-compute-max-cloned-op-ppm"),
        ("local_shared_compute_common_owner_max_clones", "-local-shared-compute-common-owner-max-clones"),
        ("local_shared_compute_common_owner_max_cloned_op_ppm", "-local-shared-compute-common-owner-max-cloned-op-ppm"),
        ("split_oversize_compute_node_max_ops", "-split-oversize-compute-node-max-ops"),
        ("dp_segment_penalty_ppm", "-dp-segment-penalty-ppm"),
        ("final_fanin_pullback_max_node_ops", "-final-fanin-pullback-max-node-ops"),
        ("final_fanin_pullback_max_value_width", "-final-fanin-pullback-max-value-width"),
        ("final_fanin_pullback_min_gain", "-final-fanin-pullback-min-gain"),
        ("final_fanin_pullback_max_moves", "-final-fanin-pullback-max-moves"),
        ("final_fanin_pullback_max_moved_op_ppm", "-final-fanin-pullback-max-moved-op-ppm"),
        ("final_terminal_pushforward_max_node_ops", "-final-terminal-pushforward-max-node-ops"),
        ("final_terminal_pushforward_max_inputs", "-final-terminal-pushforward-max-inputs"),
        ("final_terminal_pushforward_max_outputs", "-final-terminal-pushforward-max-outputs"),
        ("final_terminal_pushforward_max_value_width", "-final-terminal-pushforward-max-value-width"),
        ("final_terminal_pushforward_min_bae_gain", "-final-terminal-pushforward-min-bae-gain"),
        (
            "final_terminal_pushforward_min_boundary_value_gain",
            "-final-terminal-pushforward-min-boundary-value-gain",
        ),
        ("final_terminal_pushforward_max_moves", "-final-terminal-pushforward-max-moves"),
        ("final_terminal_pushforward_max_moved_op_ppm", "-final-terminal-pushforward-max-moved-op-ppm"),
        (
            "final_terminal_pushforward_profile_min_source_fire",
            "-final-terminal-pushforward-profile-min-source-fire",
        ),
        ("final_shared_input_peer_max_node_ops", "-final-shared-input-peer-max-node-ops"),
        ("final_shared_input_peer_max_inputs", "-final-shared-input-peer-max-inputs"),
        ("final_shared_input_peer_max_outputs", "-final-shared-input-peer-max-outputs"),
        ("final_shared_input_peer_max_value_width", "-final-shared-input-peer-max-value-width"),
        ("final_shared_input_peer_max_peers", "-final-shared-input-peer-max-peers"),
        ("final_shared_input_peer_max_candidates", "-final-shared-input-peer-max-candidates"),
        ("final_shared_input_peer_max_moves", "-final-shared-input-peer-max-moves"),
        ("final_shared_input_peer_max_moved_op_ppm", "-final-shared-input-peer-max-moved-op-ppm"),
        (
            "final_shared_input_peer_profile_min_source_fire",
            "-final-shared-input-peer-profile-min-source-fire",
        ),
        ("final_sibling_fusion_min_gain", "-final-sibling-fusion-min-gain"),
        ("final_sibling_fusion_max_pairs", "-final-sibling-fusion-max-pairs"),
        ("final_sibling_fusion_max_fused_op_ppm", "-final-sibling-fusion-max-fused-op-ppm"),
        ("post_dp_refine_max_rounds", "-post-dp-refine-max-rounds"),
        ("post_dp_refine_max_moves", "-post-dp-refine-max-moves"),
        ("post_dp_refine_max_moved_op_ppm", "-post-dp-refine-max-moved-op-ppm"),
        ("post_dp_refine_max_regression_ppm", "-post-dp-refine-max-regression-ppm"),
        ("kahn_level_pack_max_moves", "-kahn-level-pack-max-moves"),
        ("kahn_level_pack_max_moved_op_ppm", "-kahn-level-pack-max-moved-op-ppm"),
        ("kahn_level_pack_max_regression_ppm", "-kahn-level-pack-max-regression-ppm"),
    ]
    bool_options = [
        ("enable_coarsen", "-enable-coarsen"),
        ("enable_chain_merge", "-enable-chain-merge"),
        ("enable_local_shared_compute", "-enable-local-shared-compute"),
        ("commit_guard_event_buckets", "-commit-guard-event-buckets"),
        ("split_oversize_compute_nodes", "-split-oversize-compute-nodes"),
        ("declared_value_compute_node_boundary", "-declared-value-compute-node-boundary"),
    ]

    for key, arg in string_options:
        value = _pop_named(local, key, None)
        if value is not None:
            out.extend([arg, str(value)])
    for key, arg in size_options:
        value = _pop_named(local, key, None)
        if value is not None:
            if not isinstance(value, int) or value < 0:
                raise ValueError(f"activity-schedule {key} must be a non-negative integer")
            out.extend([arg, str(value)])
    for key, arg in bool_options:
        value = _pop_named(local, key, None)
        if value is not None:
            out.extend([arg, "true" if value else "false"])

    _ensure_no_extra_named("activity-schedule", local)
    return out


def _compile_emit_sv_kwargs(named: dict[str, Any]) -> None:
    local = dict(named)
    _ensure_no_extra_named("emit_sv", local)


def _compile_emit_verilator_repcut_package_kwargs(named: dict[str, Any]) -> None:
    local = dict(named)
    _ensure_no_extra_named("emit_verilator_repcut_package", local)


def _compile_emit_grhsim_cpp_kwargs(named: dict[str, Any]) -> None:
    local = dict(named)
    max_cpp_file_bytes = local.pop("max_cpp_file_bytes", None)
    if max_cpp_file_bytes is not None:
        if not isinstance(max_cpp_file_bytes, int) or max_cpp_file_bytes < 0:
            raise ValueError("emit_grhsim_cpp max_cpp_file_bytes must be a non-negative integer")
    sched_batch_max_ops = local.pop("sched_batch_max_ops", None)
    if sched_batch_max_ops is not None:
        if not isinstance(sched_batch_max_ops, int) or sched_batch_max_ops < 0:
            raise ValueError("emit_grhsim_cpp sched_batch_max_ops must be a non-negative integer")
    sched_batch_max_estimated_lines = local.pop("sched_batch_max_estimated_lines", None)
    if sched_batch_max_estimated_lines is not None:
        if not isinstance(sched_batch_max_estimated_lines, int) or sched_batch_max_estimated_lines < 0:
            raise ValueError("emit_grhsim_cpp sched_batch_max_estimated_lines must be a non-negative integer")
    sched_batch_target_count = local.pop("sched_batch_target_count", None)
    if sched_batch_target_count is not None:
        if not isinstance(sched_batch_target_count, int) or sched_batch_target_count < 0:
            raise ValueError("emit_grhsim_cpp sched_batch_target_count must be a non-negative integer")
    sched_batches_per_cpp = local.pop("sched_batches_per_cpp", None)
    if sched_batches_per_cpp is not None:
        if not isinstance(sched_batches_per_cpp, int) or sched_batches_per_cpp < 0:
            raise ValueError("emit_grhsim_cpp sched_batches_per_cpp must be a non-negative integer")
    emit_parallelism = local.pop("emit_parallelism", None)
    if emit_parallelism is not None:
        if not isinstance(emit_parallelism, int) or emit_parallelism < 0:
            raise ValueError("emit_grhsim_cpp emit_parallelism must be a non-negative integer")
    waveform = local.pop("waveform", None)
    if waveform is not None:
        if not isinstance(waveform, str):
            raise ValueError("emit_grhsim_cpp waveform must be a string")
        if waveform not in {"off", "declared-symbols"}:
            raise ValueError("emit_grhsim_cpp waveform must be one of: off, declared-symbols")
    perf = local.pop("perf", None)
    if perf is not None:
        if not isinstance(perf, str):
            raise ValueError("emit_grhsim_cpp perf must be a string")
        if perf not in {"off", "eval"}:
            raise ValueError("emit_grhsim_cpp perf must be one of: off, eval")
    input_fullpass_specialization = local.pop("input_fullpass_specialization", None)
    if input_fullpass_specialization is not None:
        if not isinstance(input_fullpass_specialization, bool):
            raise ValueError("emit_grhsim_cpp input_fullpass_specialization must be a bool")
    posedge_fullpass_specialization = local.pop("posedge_fullpass_specialization", None)
    if posedge_fullpass_specialization is not None:
        if not isinstance(posedge_fullpass_specialization, bool):
            raise ValueError("emit_grhsim_cpp posedge_fullpass_specialization must be a bool")
    full_active_word_consume = local.pop("full_active_word_consume", None)
    if full_active_word_consume is not None:
        if not isinstance(full_active_word_consume, bool):
            raise ValueError("emit_grhsim_cpp full_active_word_consume must be a bool")
    direct_single_writer_state_reads = local.pop("direct_single_writer_state_reads", None)
    if direct_single_writer_state_reads is not None:
        if not isinstance(direct_single_writer_state_reads, bool):
            raise ValueError("emit_grhsim_cpp direct_single_writer_state_reads must be a bool")
    pure_event_compute_word_bypass = local.pop("pure_event_compute_word_bypass", None)
    if pure_event_compute_word_bypass is not None:
        if not isinstance(pure_event_compute_word_bypass, bool):
            raise ValueError("emit_grhsim_cpp pure_event_compute_word_bypass must be a bool")
    pure_event_compute_word_profile = local.pop("pure_event_compute_word_profile", None)
    if pure_event_compute_word_profile is not None:
        if not isinstance(pure_event_compute_word_profile, bool):
            raise ValueError("emit_grhsim_cpp pure_event_compute_word_profile must be a bool")
    pure_event_word_pack_policy = local.pop("pure_event_word_pack_policy", None)
    if pure_event_word_pack_policy is not None:
        if not isinstance(pure_event_word_pack_policy, str):
            raise ValueError("emit_grhsim_cpp pure_event_word_pack_policy must be a string")
        if pure_event_word_pack_policy not in {"off", "probe", "targeted"}:
            raise ValueError(
                "emit_grhsim_cpp pure_event_word_pack_policy must be one of: off, probe, targeted"
            )
    for key in (
        "pure_event_word_pack_max_moved_supernode_ppm",
        "pure_event_word_pack_max_changed_word_ppm",
    ):
        value = local.pop(key, None)
        if value is not None:
            if isinstance(value, bool) or not isinstance(value, int) or value < 0 or value > 1_000_000:
                raise ValueError(f"emit_grhsim_cpp {key} must be an integer between 0 and 1000000")
    active_mask_gap_pack_policy = local.pop("active_mask_gap_pack_policy", None)
    if active_mask_gap_pack_policy is not None:
        if not isinstance(active_mask_gap_pack_policy, str):
            raise ValueError("emit_grhsim_cpp active_mask_gap_pack_policy must be a string")
        if active_mask_gap_pack_policy not in {
            "off",
            "probe",
            "targeted-direct",
            "targeted-table-contiguous",
            "targeted-table-gap",
        }:
            raise ValueError(
                "emit_grhsim_cpp active_mask_gap_pack_policy must be one of: "
                "off, probe, targeted-direct, targeted-table-contiguous, targeted-table-gap"
            )
    deferred_activation_forward_policy = local.pop("deferred_activation_forward_policy", None)
    if deferred_activation_forward_policy is not None:
        if not isinstance(deferred_activation_forward_policy, str):
            raise ValueError("emit_grhsim_cpp deferred_activation_forward_policy must be a string")
        if deferred_activation_forward_policy not in {
            "off",
            "probe",
            "cofire-probe",
            "cofire-strict",
            "cofire-strict-extended",
        }:
            raise ValueError(
                "emit_grhsim_cpp deferred_activation_forward_policy must be one of: "
                "off, probe, cofire-probe, cofire-strict, cofire-strict-extended"
            )
    deferred_activation_forward_profile_path = local.pop(
        "deferred_activation_forward_profile_path", None
    )
    if deferred_activation_forward_profile_path is not None:
        if not isinstance(deferred_activation_forward_profile_path, str):
            raise ValueError(
                "emit_grhsim_cpp deferred_activation_forward_profile_path must be a string"
            )
    same_batch_activation_cohort_policy = local.pop(
        "same_batch_activation_cohort_policy", None
    )
    if same_batch_activation_cohort_policy is not None:
        if not isinstance(same_batch_activation_cohort_policy, str):
            raise ValueError(
                "emit_grhsim_cpp same_batch_activation_cohort_policy must be a string"
            )
        if same_batch_activation_cohort_policy not in {"off", "probe", "strict"}:
            raise ValueError(
                "emit_grhsim_cpp same_batch_activation_cohort_policy must be one of: "
                "off, probe, strict"
            )
    same_batch_activation_cohort_profile_path = local.pop(
        "same_batch_activation_cohort_profile_path", None
    )
    if same_batch_activation_cohort_profile_path is not None:
        if not isinstance(same_batch_activation_cohort_profile_path, str):
            raise ValueError(
                "emit_grhsim_cpp same_batch_activation_cohort_profile_path must be a string"
            )
    _ensure_no_extra_named("emit_grhsim_cpp", local)


def _normalize_diagnostics_print_min_level(level: str) -> str:
    name = str(level).lower()
    if name == "warn":
        name = "warning"
    if name not in {"none", "warning", "error"}:
        raise ValueError("diagnostics print min level must be one of: none, warning, error")
    return name


def _normalize_diagnostics_raise_min_level(level: str) -> str:
    name = str(level).lower()
    if name == "warn":
        name = "warning"
    if name not in {"none", "warning", "error"}:
        raise ValueError("diagnostics raise min level must be one of: none, warning, error")
    return name


def _normalize_log_level(level: str) -> str:
    name = str(level).lower()
    if name == "warning":
        name = "warn"
    if name not in {"trace", "debug", "info", "warn", "error", "off"}:
        raise ValueError("log level must be one of: trace, debug, info, warn, error, off")
    return name


def _level_rank(level: str) -> int | None:
    if level is None:
        return None
    name = str(level).lower()
    if name in {"off", "none"}:
        return None
    if name == "warn":
        name = "warning"
    ranks = {
        "debug": 0,
        "info": 1,
        "warning": 2,
        "error": 3,
    }
    return ranks.get(name)


def _should_raise(diags: list[dict], threshold: str) -> bool:
    rank = _level_rank(threshold)
    if rank is None:
        return False
    for diag in diags or []:
        kind = str(diag.get("kind", "")).lower()
        if kind == "warn":
            kind = "warning"
        kind_rank = _level_rank(kind)
        if kind_rank is not None and kind_rank >= rank:
            return True
    return False


def _format_diagnostics(diags: list[dict]) -> str:
    return format_diagnostics(diags) or "wolvrix failed"


def format_diagnostics(diags: list[dict], *, min_level: str = "debug") -> str:
    threshold = _diagnostics_display_rank(min_level)
    if threshold is None:
        return ""
    parts: list[str] = []
    for diag in diags or []:
        kind = str(diag.get("kind", "")).lower()
        if kind == "warn":
            kind = "warning"
        rank = _level_rank(kind)
        if rank is None or rank < threshold:
            continue
        text = str(diag.get("text", "")).strip()
        if text:
            parts.append(text)
    return "\n".join(parts)


def print_diagnostics(diags: list[dict], *, min_level: str = "debug", file=None) -> None:
    text = format_diagnostics(diags, min_level=min_level)
    if not text:
        return
    stream = sys.stderr if file is None else file
    stream.write(text)
    if not text.endswith("\n"):
        stream.write("\n")
    if hasattr(stream, "flush"):
        stream.flush()


def _diagnostics_display_rank(level: str) -> int | None:
    name = str(level).lower()
    if name in {"off", "none"}:
        return None
    if name == "warn":
        name = "warning"
    if name not in {"debug", "info", "warning", "error"}:
        raise ValueError("diagnostics print level must be one of: debug, info, warning, error, none")
    return _level_rank(name)


def _raise_with_diagnostics(diags: list[dict]) -> None:
    exc = RuntimeError(_format_diagnostics(diags))
    setattr(exc, "diagnostics", list(diags))
    raise exc
