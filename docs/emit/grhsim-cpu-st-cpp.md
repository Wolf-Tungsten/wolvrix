# GRH-based single-thread CPU emitter

`grhsim-cpu-st-cpp` consumes one flattened GRH top at the
`sink-partitioned` stage and its current session's `SinkSchedule`. The graph
contains ordinary GRH operations and the [GrhSIM dialect](../grh/grhsim-dialect.md).
The implementation is `lib/emit/grhsim/cpu-st-cpp.cpp`; storage, activation
and code layout are private emitter analyses. The independent IR, CPU mapping
and planning passes have been removed. See the [simulation workflow](../grhsim.md)
for standard entries, supported/rejected forms and the validation index.

## Emission and execution

The emitter builds private storage/access/activation and C++ text structures.
It preserves P_input, P_event, P_general, P_large_sink, P_publish and P_output.
P_input captures input changes once per eval. The middle four phases repeat
until Publish reports convergence; exceeding `max-round` (default 100000)
raises an error before Output. P_output runs once after convergence and commits
its histories after all output observations. Event history updates each round;
monitor/strobe triggers remain sticky across the eval.
Small states use current/next storage. Large states sample write parameters
in General and commit them in the ordered LargeSink phase. DPI/random results
retain their held samples; independently ordered calls retain graph effect
order. Sink functions follow contiguous SinkSchedule runs without exchanging
writers. Pure payloads private to an enable can execute inside that enable;
shared controls and observable boundary values remain unconditional.

`max-function-lines` defaults to 1024 and budgets the actual generated lines
of every function, including dispatchers and diagnostic helpers. Cross-chunk
locals use that source function's `chunkFrame`. Chunking does not change GRH
partition attributes or SinkSchedule. `max-tu-bytes` defaults to 1048576.
Named stores always expose semantic fields as direct C++ members; only large
private chunk frames may group locals into typed `cells_*` arrays. A split
with no shared locals or return control has no frame or frame argument.
General always dispatches separate nonsink functions, followed by ordered
writer batches. Activity batches scan consecutive groups of 24 bitmap bytes,
reading each live bit immediately before its call. Later activations remain
visible within the same round, including across batch boundaries. Writer and
commit batches contain at most 128 contiguous SinkSchedule runs. Only these
bounded helpers own computation locals and split when their line budget
requires it; `pGeneral` and `pLargeSink` keep constant-size ordered call loops.
The splitter processes all oversized parts and scopes in each analysis pass.
Splitting only adds boundaries, so existing shared locals remain shared;
new shared locals are discovered before rendering. This bounds repeated
whole-function analysis for large helpers while preserving initializers,
loop lifetimes, return control and the final line limit.
Pure sink-run locals follow the legacy assignment policy: results, pack
accumulators, indices and writer temporaries are assigned before use along
their guarded paths, so their frame needs no implicit zero fill. Explicit
initializers, return-control and other functions retain their defaults. A
disabled writer cannot suppress a sink-local observable boundary refresh.
Large-state options are `mem-write-activate=on-change|on-write` and
`mem-enable-bitmap=on|off`; the emitter defaults to on-change/on, while the
production flow explicitly selects on-write/on.

Cross-chunk frames with more than 1024 logical fields group their
default-initialized fields by complete C++ type into indexed arrays. Named
stores retain their direct members at every size. The frame layout does not
change partition keys or scheduling. Explicit initializers and chunk
return-control members remain individual fields.
Generated store/frame constructors remain explicitly defaulted, with value
initialization at uses requiring zero defaults. Boundary reset clears the
physical fields; strings retain their constructors and string-aware reset.

The Python entry is `Session.emit_grhsim_cpu_st_cpp(...)`; the ordinary GRH
JSON checkpoint stores graph semantics and ownership. Reloading requires
rebuilding SinkSchedule through `grhsim-partition-sink`.

## Types, arrays and external ABI

CPU execution requires explicit TwoState Logic. FourState types remain valid
in GRH and JSON, but are rejected by this emitter; use the explicit conversion
policy rather than expecting an implicit X/Z projection. Scalar Real/String
use `double`/`std::string`. Unsupported live ops or call signatures fail before
the candidate output replaces an existing model.

Logic width is total width and shape is outermost-first. Nested arrays retain
their leaf width/signedness. State reads, row copies, equal-width views and
mixed packed consumers use private access analysis with an explicit snapshot
when a writer can overlap its source. Dynamic state access checks every indexed
dimension; `sliceArray` retains its separate flattened-offset contract.

Generated C++ requires little-endian Clang with `_BitInt`. Wide TwoState leaves
use native `unsigned _BitInt` expressions with logical-width masking. Existing
pointer/output-buffer strategies remain at DPI and task/helper boundaries;
wide operations do not introduce return-by-value array helper chains. InitSpec
drives constants, fills, readmem and seeded random initialization, including
reinitialization of already-used models. Runtime random currently supports
the no-argument scalar `$random` form up to 32 bits; other surviving system
functions are rejected.

## Optional native execution counts

`execution-counters=on` adds a diagnostic ABI. Its default is off; explicit
off and omission produce identical files. Python exposes
`execution_counters=False`, the reemit script has `--execution-counters`,
and `make reemit_grhsim` accepts `GRHSIM_REEMIT_EXECUTION_COUNTERS=1`.

The instrumented model exposes `dump_execution_counts(FILE*)`. A successful
`init()` resets counters; actual initialization operations after that reset
are included. `evals` counts eval calls, not hardware cycles or fixed-point
rounds. Sites identify native operation symbols, emission function labels and these
categories:

| Category | Counted action |
| --- | --- |
| operation | Visiting an emitted compute/read/task/call operation |
| effect | Actually executing a guarded DPI, task or random call |
| writer-sample | Visiting a General writer site, not successful writes |
| writer-commit | Visiting a LargeSink commit site, not successful writes |
| history-write | Visiting an Output history writer |

View-only assign operations are listed separately as elided operations.
Function labels describe the emission context before final inlining/chunking
and can differ between function budgets. Cross-budget checks compare the
same graph and counts by operation/category; each model retains its own
complete source binding. Attribution uses the graph's operation ownership.
Moving a pure payload inside an enable changes its visits without changing
the graph or successful write behavior.

`execution-counter-graph.json` contains the exact selected top. The manifest
records sites and a consistency binding covering that snapshot, sites and
all generated C++/header files; the dump tag itself is normalized when
computing the binding. FNV1a64 is a consistency check, not cryptographic
authentication. The reader additionally records SHA256 evidence.

`scripts/grhsim_execution_counts.py` requires two distinct independent logs
with identical counts/evals. It checks graph/source/site binding, complete
executable coverage, row order, totals, end markers and uint64 bounds.
It rejects truncated, stale or mismatched evidence. Use the project Make
targets `test_grhsim_execution_counts`, `inspect_remove_grhsim_ir_execution`
and `inspect_remove_grhsim_ir_rob_execution` to run these workflows.

Family/paired diagnostics optionally use these counts for native final-graph
writer data cones. They count shared operations once in the union, exclude
non-compute categories from compute pricing, and leave unavailable original
packed members unpriced. They never reuse legacy mapping IDs or claim
legacy dynamic normalization. Instrumented models are diagnostic candidates,
not runtime-performance candidates.
