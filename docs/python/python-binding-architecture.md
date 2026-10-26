# Python Binding Architecture

This document explains how the Wolvrix Python binding is structured today.

It is written for readers who already know that Wolvrix has a Python API, but do not yet know how the API is organized internally or why it is centered around a `Session` object.

## Scope

This document is about the Python binding layer.

It does not try to teach GRH itself. If you are new to Wolvrix, read these documents first:

1. `docs/grh/grh-ir.md`
2. `docs/transform/README.md`

Once those are familiar, this document explains how the Python API exposes that system.

## Design Goal

The Python binding should provide one consistent workflow for:

- reading designs
- loading serialized designs
- running transform passes
- inspecting intermediate values
- storing files
- emitting files

That workflow is built around a single object:

```python
with wolvrix.Session() as sess:
    ...
```

## Why The API Uses A Session

At first glance, it might seem simpler to pass `Design` objects around directly in Python. That works for very small examples, but it breaks down as soon as the pipeline produces more than one kind of intermediate result.

For example:

- one pass may produce a schedule
- another pass may want to consume that schedule
- Python may want to inspect it for debugging
- an emitter may later want to consume that same value

If each of those results lives in a separate side channel, the API becomes fragmented. The session model avoids that problem by making all intermediate results named values in one shared workspace.

## The Public Model

The public model is intentionally small:

```text
key -> value
```

Optionally, a value also has a `kind`:

```text
key -> (kind, value)
```

That is the main idea. The public API should not force users to understand internal wrapper objects, payload records, or versioned metadata schemas just to use the binding.

## What Lives In A Session

A session can hold many kinds of values:

- designs
- statistics
- scheduling results
- partitioning results
- debug-only values

Examples of keys:

- `design.main`
- `design.flat`
- `stats.main`
- `loops.main`

Examples of kinds:

- `design`
- `stats`
- `comb-loop.reports`

The key point is that all of these are ordinary session values. They do not use separate storage mechanisms in the public API.

## Why Session Values Matter Beyond Python

Session values are not only for Python-side debugging.

They are also the intended handoff mechanism between native stages.

Example:

```python
sess.run_pass("stats", design="design.dut", out_stats="stats.dut")
stats_value = sess.get("stats.dut")
```

In this example, `stats.dut` is produced by one native stage and then consumed by Python through the ordinary session API.

## Core Workflow

Simulation follows this same workflow on an ordinary `design` key. Use
`run_pass("grh-to-grhsim", ...)`, the simulation transforms and
`emit_grhsim_cpu_st_cpp(...)` in the same session. `grhsim-partition-sink`
publishes a typed SinkSchedule bound to the current design/top/revision;
load, clone or mutation requires rebuilding it. It is not a separate model
category and is not stored in JSON. The independent `lower_grhsim`,
`run_grhsim_pass`, `run_grhsim_pipeline`, `load_grhsim`, `store_grhsim` and
`list_grhsim_passes` APIs have been removed from both Python and native exports.
See the [simulation workflow](../grhsim.md) for stages, checkpoints and options.

The normal workflow looks like this:

1. read or load a design into the session
2. run passes against a named design
3. optionally create additional session values
4. emit or store files from the session

Example:

```python
import wolvrix

with wolvrix.Session() as sess:
    sess.log_level = "info"
    sess.diagnostics_print_min_level = "warning"
    sess.diagnostics_raise_min_level = "error"

    sess.read_sv(
        "path/to/top.sv",
        out_design="design.main",
        slang_args=["--top", "top"],
    )

    sess.run_pass("xmr-resolve", design="design.main")
    sess.run_pass("simplify", design="design.main")
    sess.run_pass("stats", design="design.main", out_stats="stats.main")

    sess.store_json(design="design.main", output="build/main.json")
    sess.emit_sv(design="design.main", output="build/main.sv")
```

## Naming Rules

The Python API follows a consistent naming convention.

Use:

- `out_design=...` when an action creates or loads a design
- `design=...` for the design currently being operated on
- `in_*` for extra session inputs
- `out_*` for extra session outputs

Examples:

```python
sess.read_sv("top.sv", out_design="design.main")
sess.clone_design(design="design.main", out_design="design.copy")
sess.run_pass("stats", design="design.main", out_stats="stats.main")
```

This keeps calls readable:

- first identify the design
- then identify extra session flow
- then provide ordinary configuration options

## Main API Surface

### Session Basics

```python
class Session:
    def __enter__(self) -> "Session": ...
    def __exit__(self, exc_type, exc, tb) -> None: ...
    def close(self) -> None: ...

    def __contains__(self, key: str) -> bool: ...
    def keys(self, prefix: str | None = None, kind: str | None = None) -> list[str]: ...
    def kind(self, key: str) -> str: ...

    def get(self, key: str): ...
    def put(self, key: str, value, *, kind: str, replace: bool = False) -> None: ...

    def delete(self, key: str) -> None: ...
    def rename(self, src: str, dst: str, *, replace: bool = False) -> None: ...
    def copy(self, src: str, dst: str, *, replace: bool = False) -> None: ...

    @property
    def diagnostics_print_min_level(self) -> str: ...
    @diagnostics_print_min_level.setter
    def diagnostics_print_min_level(self, level: str) -> None: ...

    @property
    def diagnostics_raise_min_level(self) -> str: ...
    @diagnostics_raise_min_level.setter
    def diagnostics_raise_min_level(self, level: str) -> None: ...

    @property
    def log_level(self) -> str: ...
    @log_level.setter
    def log_level(self, level: str) -> None: ...

    def history(self) -> list[dict]: ...
```

### Design Ingress

```python
sess.read_sv(path, *, out_design="design.main", slang_args=None, replace=False) -> list[dict]
sess.read_json_file(path, *, out_design="design.main", replace=False) -> list[dict]
sess.load_json_text(text, *, out_design="design.main", replace=False) -> list[dict]
```

### Design Branching

```python
sess.clone_design(*, design: str, out_design: str, replace: bool = False) -> list[dict]
```

### Transform

```python
sess.run_pass(
    name: str,
    *,
    design: str,
    args: list[str] | None = None,
    dryrun: bool = False,
    **named_args,
) -> list[dict]
```

There is no separate pipeline API. A pipeline is simply multiple `run_pass(...)` calls in sequence.

`run_pass("grh.verify")` runs the structural GRH verifier (array shape invariants, declaration
op attributes, instance port width/shape matching). It is read-only and reports violations as
diagnostics.

### Store And Emit

```python
sess.store_json(*, design: str, output: str, mode="pretty-compact", top=None) -> list[dict]
sess.emit_sv(*, design: str, output: str, top=None, split_modules=False) -> list[dict]
sess.emit_verilator_repcut_package(*, design: str, output: str, top=None) -> list[dict]
```

Large outputs still go to disk by default. The session is not meant to be a multi-gigabyte artifact cache.

## Diagnostics

Each action returns diagnostics directly:

```python
diags = sess.read_sv("top.sv", out_design="design.main")
diags = sess.run_pass("simplify", design="design.main")
diags = sess.emit_sv(design="design.main", output="build/main.sv")
```

There is no extra `ActionResult` wrapper.

### Diagnostic Record Shape

In Python, diagnostics are returned as:

```python
list[dict]
```

Each diagnostic record currently has this shape:

```python
{
    "kind": str,      # "debug" | "info" | "warning" | "error"
    "pass": str,      # pass name, or "" when not applicable
    "message": str,   # core message text
    "context": str,   # optional extra context, often ""
    "origin": str,    # optional origin symbol, often ""
    "text": str,      # fully formatted display text

    # present only when source location is available
    "file": str,
    "line": int,
    "column": int,
}
```

Notes:

- `kind`, `pass`, `message`, `context`, `origin`, and `text` are always present
- `file`, `line`, and `column` only appear when the native side can resolve a source location
- `text` is for display
- `message`, `kind`, `pass`, and location fields are the fields you should use for programmatic analysis

### How To Analyze Diagnostics In Python

The most important rule is:

Use the structured fields for logic, and use `text` only for display.

Good:

```python
warnings = [d for d in diags if d["kind"] == "warning"]
stats_diags = [d for d in diags if d["pass"] == "stats"]
with_location = [d for d in diags if "file" in d]
```

Avoid building tools that parse `d["text"]` unless you are generating display-only output. The formatted text is convenient for humans, but it is not the right field for structured analysis.

### Common Analysis Patterns

Count diagnostics by level:

```python
from collections import Counter

counts = Counter(d["kind"] for d in diags)
```

Check whether anything severe happened:

```python
has_blocking = any(d["kind"] in {"warning", "error"} for d in diags)
```

Filter diagnostics from one pass:

```python
comb_loop_diags = [d for d in diags if d["pass"] == "comb-loop-elim"]
```

Keep only diagnostics with source locations:

```python
located = [d for d in diags if "file" in d]
```

Group diagnostics by file:

```python
from collections import defaultdict

by_file = defaultdict(list)
for d in diags:
    if "file" in d:
        by_file[d["file"]].append(d)
```

Extract a short machine-friendly summary:

```python
summary = [
    {
        "kind": d["kind"],
        "pass": d["pass"],
        "message": d["message"],
        "file": d.get("file"),
        "line": d.get("line"),
        "column": d.get("column"),
    }
    for d in diags
]
```

### Formatting Helpers

For display, use the provided helpers:

```python
text = wolvrix.format_diagnostics(diags, min_level="info")
wolvrix.print_diagnostics(diags, min_level="warning")
```

They filter on the already-structured `kind` field and print the preformatted `text` field.

Display levels:

- `debug`
- `info`
- `warning`
- `error`
- `none`

### Diagnostics Print And Raise Levels

The session controls both diagnostic display and control flow.

`diagnostics_print_min_level`:

- `none`: do not print diagnostics automatically
- `warning`: print warning and error diagnostics automatically
- `error`: print only error diagnostics automatically

`diagnostics_raise_min_level`:

- `none`: never raise because of diagnostics
- `warning`: raise on warning or error
- `error`: raise on error

Defaults:

- `sess.diagnostics_print_min_level = "warning"`
- `sess.diagnostics_raise_min_level = "error"`

Suggested action flow:

1. run the action
2. collect diagnostics
3. auto-print diagnostics according to `diagnostics_print_min_level`
4. apply the session raise threshold
5. return diagnostics when no exception is raised

If an exception is raised by policy, the exception still carries the diagnostics:

```python
try:
    sess.run_pass("comb-loop-elim", design="design.main")
except RuntimeError as ex:
    diags = ex.diagnostics
```

This means most scripts can simply call actions directly without wrapping each call in an explicit
`print_diagnostics(...)` helper, while still keeping the structured return value available.

## Logging

Logging is configured at the session level:

```python
sess.log_level = "warn"
```

For now, logging stays in C++:

- it is printed from native code
- it is not returned to Python
- it is separate from diagnostics

## Python Adapters

Not every session value needs a rich Python representation.

The expected behavior is:

- if a kind has a Python adapter, `sess.get(...)` returns that adapter object
- otherwise, Python gets an opaque handle

This keeps the binding compatible with native-only values while still allowing rich debugging support where it is useful.

## Summary

The architecture is built around one principle:

The session is the shared workspace for the full Python workflow.

That is why the public API stays small:

- one session
- named keys
- optional kinds
- explicit `design`, `in_*`, and `out_*` arguments

This keeps the API understandable for users and extensible for future passes and emitters.
