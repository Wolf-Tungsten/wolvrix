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
visible within the same round, including across batch boundaries.

Nonsink owners with no External operations or String operands/results can
group boundary publications of scalar TwoState Logic values up to 64 bits.
Values represented in the small-sink enable bitmap retain immediate publication.
For each complete nonempty nonsink fanout shared by at least two emitted
publication sites, a local dirty flag accumulates each old-snapshot comparison;
the snapshot copy and small-sink boundary synchronization stay at that site's
original position. For example, `oldA` and `oldB` are stored snapshots and
`newA` and `newB` are the newly computed values:

```cpp
changed = false;                 // owner entry
changed |= oldA != newA;
oldA = newA;                     // synchronize A's small-sink copies here
changed |= oldB != newB;
oldB = newB;                     // synchronize B's small-sink copies here
if (changed) activate(fanout);   // owner exit, preserving current/next routing
```

The flag uses ordinary locals/chunkFrame storage and is explicitly reset before
the owner's operations. Empty fanout needs only the original-position copy and
synchronization, without a comparison. Nonempty fanout with one publication
site, other types, Event/fire publications and excluded owners retain their
original path. Group counts use the operation emitter's elisions and are checked
against actual publication calls. Each site's comparison/copy/synchronization
retains its operation origin; the combined flush is owner bookkeeping.

General right-hand sides can use a typed literal for a direct scalar Logic
`kConstant` up to 64 bits. This does not look through assigns or held samples.
The defining operation, its counter, boundary copy, role synchronization and
activity publication remain at the original site. Cross-owner substitution
requires an earlier nonsink whose constant precedes every potentially throwing
operation in that owner's topological order. The safe prefix contains only
Logic Pure/ReadState operations and excludes system functions. An external call
or string operation ends the prefix, preserving zero boundaries when a caller
catches an exception before publication and invokes `eval()` again. Event,
Output and address-taking paths keep their stored views.

Large-writer captures can also use direct scalar TwoState Logic constants,
including wide `_BitInt` values. Eligibility is decided at the original General
capture: the defining constant belongs to the same writer owner, or it is in an
earlier nonsink's safe prefix described above. The emitter omits that argument's
capture copy and uses its exact typed literal in LargeSink. The defining
operation, publication and counters remain. Assigns, held samples and shaped
constants keep their capture path. Declared capture fields and model layout are
retained; an omitted copy leaves its unused field without semantic significance.
This capture-specific rule does not extend general wide-operand substitution.

In bitmap mode, LargeSink may read one original capture bit for writers proven
to have the same stable effective capture condition. Controls must be physically
stored scalar Logic Input, Event, or nonsink General values. Groups match latch
kind and the ValueId identities of the fresh run fire, saved fire/enable and
ordered guards, and use the smallest original writer bit. General still clears
the full bitmap and sets every writer's own bit after its original capture;
it does not precompute or compact enables. The proof includes the expanded run fire
gate as well as the saved fire/enable reads: a saved OR/Assign fire can remain
true after an Event pulse clears. Original coarse word guards, batch boundaries,
counter sites, payload snapshots and writer order remain. Thus, for example,
two equivalent writers still capture separate data and mark bits 3 and 70,
while their final commit tests can both read bit 3. General must finish before
LargeSink runs; an exception aborts that transition, and the next General clears
the bitmap again. Index validity, masks, on-write touches and before-images keep
their existing behavior, including valid zero-mask and same-value writes.

The code map distinguishes these cases from physical capture storage. Both the
writer's `captures` argument reference and `captured-argument-i` storage role
describe the actual literal when its copy is omitted. In bitmap mode, the original
`enable_bit` and `captured-enable` still identify that writer's own completion bit;
`commit_enable_bit` and `captured-commit-enable` identify the final commit read.
With `mem-enable-bitmap=off`, `captured-enable` names the writer's bool field,
the own and commit ordinals are equal, and no `captured-commit-enable` role is emitted.
Retained field declarations
do not imply that a constant argument is read from that unused field.

For an unsigned logical-right-shift result narrower than 64 bits whose source
was replaced by a General scalar literal, the zero guard uses the existing `uint64_t`
shift container's width, 64. The normalized source is already zero above its
logical width, so intermediate counts naturally produce zero. The complete
count is compared before conversion to `size_t`, including counts wider than
64 bits. This equivalent expression avoids a Clang 22 vector instruction
selection failure; signed results and other shift paths keep their guards.

String constants use an immutable source helper returning a reference to a
function-local `static const std::string`. The original operation calls it first; definitions
alone allocate nothing. Stored strings still publish into the resettable
boundary, and no-store strings retain an explicit original-site call. Helpers
share immutable contents across instances and repeated `init()`, while instance
stores still reset. Construction preserves the existing C-string behavior for
embedded NUL bytes; it does not promise identical allocation-failure frequency.
Task arguments always have addressable storage, including packed shaped
constants, instead of taking the address of a literal.
The General scalar/string source mechanism was validated on XiangShan CoreMark 100k with
NEMU difftest: six fresh alternating runs on one CPU, without PGO, waveform
or extra counters, improved throughput by 51.210858% over the retained NO2
baseline. The full original-SV build took 2182.178070 seconds with 32 jobs.
This measures the combined scalar/string mechanism and does not assign the
gain to either component separately.

Writer and commit batches contain at most 128 contiguous SinkSchedule runs. Only these
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
`large-capture-specialization=on|off` defaults to on and controls constant
capture-copy elimination and commit-enable aliases together. Setting it off
retains the original capture/commit path for comparison. With bitmap mode off,
constant specialization still applies while enables remain per-writer bools.
The manifest records the option as `large_capture_specialization`.

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

The XiangShan SV flow exposes `--[no-]readable-names`,
`--[no-]source-comments`, `--[no-]readable-dump` and
`--[no-]readable-frames`, all enabled by default. Its Makefile variables are
`XS_GRHSIM_READABLE_NAMES`, `XS_GRHSIM_SOURCE_COMMENTS`,
`XS_GRHSIM_READABLE_DUMP` and `XS_GRHSIM_READABLE_FRAMES` (default 1).
Disabling names, comments and the text dump reduces generated artifact size;
code map generation remains enabled. Frame layout is controlled independently.
`XS_GRHSIM_MAX_OUTPUT_FILE_BYTES` passes `--max-output-file-bytes`, a positive
uint64 with the existing 4 GiB default. Large code maps can use a larger explicit
budget; this only changes the artifact size check, not generated C++ contents.

## Types, arrays and external ABI

Scalar TwoState Logic `kMux` and `kPrioritySelect` results up to 64 bits use
`numeric_select(condition, yes, no)`. Both data arguments are passed by value
after the existing sign extension, truncation and width cleanup. The helper
returns `__builtin_unpredictable(condition) ? yes : no`, allowing a conditional
move without making the operand loads conditional. For a priority selection,
the first true condition wins; all false conditions select the default value.
Producers keep their original guards and evaluation order. This changes no
effect calls, writer sampling, publication or clock behavior. Real, String,
wide and shaped results retain their existing paths, as do bounds, division
and shift guards.

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
