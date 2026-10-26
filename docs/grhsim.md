# Simulation on unified GRH

The production simulator uses the ordinary GRH Design, Graph, LogicalType,
operations, symbols and JSON format throughout. Simulation writes, events and
calls are extensions in the `grhsim` dialect. The single-thread C++ emitter is
`grhsim-cpu-st-cpp`. The independent simulation IR, its JSON loader, pass
registry, CPU mapping and Python model APIs have been removed.

## Pipeline

The playground's `scripts/grhsim_flow.py` owns the shared flow used by XS,
HDLBits, bugcases and checkpoint re-emission. The XS frontend first resolves
hierarchy, recovers aggregate reads/writes, normalizes writes, flattens,
eliminates combinational loops, verifies and simplifies ordinary GRH.
The simulation stages then operate on that same design:

| Step | Result |
| --- | --- |
| `grh-to-grhsim` | Explicit logic domain, edge detectors and simulation writers/calls; no partitions |
| `grhsim-simplify`, bit-register packing, masked-OR recovery | Global optimization with state, effect and declaration identities preserved |
| `grhsim-split-event-and-output` | Event, General and Output owners |
| `grhsim-split-nonsink-and-sink` | General becomes NonSink, SmallSink and LargeSink |
| `grhsim-partition-nonsink`, leaf simplify | Activity partitions and complete leaf owners |
| `grhsim-partition-sink` | Ordered sink groups/runs and a typed `SinkSchedule` in the session |
| `emit_grhsim_cpu_st_cpp` | C++ sources, header, Makefile and manifest |

The production helper enables bit-register packing, masked-OR recovery and
leaf simplify by default. Pass options and their contracts are specified in
[GrhSIM transforms](transform/grhsim-transforms.md). Work limits control
activity partitions; function line limits control emitted C++ functions.

## Python and checkpoints

Simulation uses an ordinary `design` session key and `Session.run_pass`.
For a completed checkpoint:

```python
import wolvrix

with wolvrix.Session() as session:
    session.read_json_file("ptmp/model.json", out_design="design.main")
    # Use the checkpoint's stored sink configuration when rebuilding its schedule.
    info = session.graph_metadata(design="design.main", top="top")
    guard = int(info["metadata"]["grhsim.sinkConfig"])
    session.run_pass("grhsim-partition-sink", design="design.main", top="top",
                     enable_guard_min_size=guard)
    session.emit_grhsim_cpu_st_cpp(design="design.main", top=["top"],
                                 output="ptmp/model", max_function_lines=1024)
```

An earlier checkpoint resumes the remaining stages through the shared flow.
`make reemit_grhsim` is the supported playground entry for this operation.

Every completed stage uses [GRH JSON](grh/grh-json-spec.md), with per-graph
`formatVersion=2` and dialect versions `grh:2`, `grhsim:1` and, when used,
`grhsim.cpu:1`. Types, InitSpec, declaration/generate metadata, owner paths and
stage/configuration attributes survive store/load. Resolved schedules and
emitter storage/chunk plans are not serialized. Load/clone creates a new
instance; mutations, freeze and rename invalidate resolved schedules. Rebuild
SinkSchedule in the same session before emission. Missing or stale schedules,
unknown stages/dialects and retired independent-IR JSON are rejected.

The removed Python APIs are `lower_grhsim`, `run_grhsim_pass`,
`run_grhsim_pipeline`, `load_grhsim`, `store_grhsim` and
`list_grhsim_passes`; there is no `grhsim-model` session category. Diagnostics
may read frozen historical records, but those records cannot execute through
the current compiler.

## Execution and supported boundaries

An eval captures inputs once, iterates Event → General → LargeSink → Publish
to convergence, then computes Output once. The default limit is 100000
rounds; exceeding it fails the eval. Small states use current/next storage.
Large-state fire, enable, indices, data and masks are sampled before ordered
in-place commits. Output histories commit after all output observations.
This preserves old-value reads and overlapping writer precedence. See the
[emitter specification](emit/grhsim-cpu-st-cpp.md) for the six phases, ABI,
function splitting and diagnostics.

| Area | Supported contract and rejection boundary |
| --- | --- |
| Logic types | GRH/JSON and transforms represent TwoState/FourState, signedness and multidimensional shape. The CPU emitter requires explicit TwoState Logic and rejects FourState rather than projecting it silently. `grh-to-grhsim` performs the selected projection. |
| Arrays | Width is total width; shape is outermost-first. `arrayCreate(a,b)` puts a at element 0/low bits, unlike `concat(a,b)`. Full, partial and dynamic access, masked/whole/fill writes and alias-safe snapshots follow the GRH contracts. Invalid shape, domain mismatch, overflowing geometry and unequal-width assign are rejected. |
| Host ABI | Little-endian Clang with `_BitInt` support; Logic up to 64 bits uses integer containers, wider leaves use native unsigned `_BitInt` with width masking. Arrays use caller-owned nested storage; ABI/helper boundaries use explicit buffers. Real/String are scalar `double`/`std::string`. |
| Calls and tasks | Guarded DPI input/output/inout and held results, original effect order, monitor/strobe history, supported formatting/file tasks and finish/error behavior are executed. A live unsupported system function/task fails emission; runtime random supports the no-argument scalar `$random` form up to 32 bits with one held sample per eval. |
| Initialization | Constant/fill/readmem/random InitSpec, first eval and repeated init are covered. Unsupported timed initial behavior or invalid file/initialization contracts fail; this is not a full SystemVerilog time-wheel simulator. |
| Effects | One original call is retained; Event/Output consume held samples. The 18 frozen DPI differences approved during migration remain explicit exceptions, with no general exemption for changed arguments, triggers or results. |
| Partition/session | Each executable op has one complete owner; data/effect DAGs and writer order are checked. Wrong instance/top/revision/configuration or malformed owners reject reuse. |
| Function budgets | Every generated function includes scaffolding in the actual line budget. Locals crossing chunks use the source function's chunkFrame; changing the budget cannot change graph owners or writer ordering. An unsatisfiable budget fails before publishing output. |
| Observability | Declared FST values, phase/supernode profiles and optional native execution counters retain graph/source identity. Native counts price surviving final-graph operations; unavailable original packed members remain unpriced. |

## Standard Make workflows

Run these in the playground root. All build, test and installation work goes
through Make. Use fresh `ptmp/` directories for validation evidence.

```bash
make py_install
make test_wolvrix
make test_grhsim_cpu_st_cpp test_grhsim_execution_counts
make test_grhsim_flow test_remove_grhsim_ir_tooling
make run_all_hdlbits_grhsim_tests_report
make run_xs_bugcase_grhsim
make xs_grhsim_emu
make run_xs_grhsim_emu XS_SIM_MAX_CYCLE=100000
```

XS requires `preserve-aggregate=all`; the production build keeps
`-O3 -march=native -fno-strict-aliasing`, 24 jobs for migration acceptance,
and JSON roundtrip/schedule reconstruction. `measure_remove_grhsim_ir_resources`
with target `build_remove_grhsim_ir_xs_from_sv` measures the complete original
SV → linked emu interval against the 2100-second budget. A checkpoint-only
re-emission is not that measurement. The 100k run is a cycle window with NEMU
difftest, not full CoreMark completion.

`check_remove_grhsim_ir` audits current sources, retired trees, actual CMake
compile commands, core/binding symbols and optionally a generated model.
`REMOVE_GRHSIM_IR_AUDIT_MODEL` selects the latter. The current Make surface
uses the `grhsim` route directly; retired optimization options fail
explicitly, including when set to zero or an empty string.

The [implementation overview](../../pdocs/grhsim.md)
describes all six phases, transform passes, dialect operations and emitter layout. The
[retired documentation](history/grhsim-ir/overview.md) records the old design;
it does not define current APIs or production behavior. S9 array optimization
starts from the accepted R7/R8 version and is a separate task.
