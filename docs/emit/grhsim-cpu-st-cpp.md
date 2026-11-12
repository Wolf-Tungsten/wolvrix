# GRH-based single-thread CPU emitter

`grhsim-cpu-st-cpp` consumes one flattened GRH top at the
`sink-partitioned` stage and its current session's `SinkSchedule`. The graph
contains ordinary GRH operations and the [GrhSIM dialect](../grh/grhsim-dialect.md).
The implementation is `lib/emit/grhsim/cpu-st-cpp.cpp`; storage, activation
and code layout are private emitter analyses. The independent IR, CPU mapping
and planning passes have been removed. See the [simulation workflow](../grhsim.md)
for standard entries, supported/rejected forms and the validation index.

## Emission and execution

FIRRTL printf writes with `grhsim.literalFormat` use a read-only character
literal and its length as the first private `TaskArg`. No String value, model
slot, getter construction or boundary comparison is needed for the format.
The existing fire/enable guard encloses argument preparation and the call.
Dynamic and other legacy String formats use the same parser through a borrowed
`std::string_view`; their storage and scheduling rules are unchanged.

The attribute retains all decoded bytes, while the CPU format length stops at
the first NUL to match the previous constant getter's C-string `std::string`
construction. Empty formats remain present and produce no text even with
surplus payload arguments. Escaping, format parsing and output/effect order
follow the ordinary task path. The nonsink argument budget counts the synthetic
literal `TaskArg` as well as the payload operands.

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

The writer caller groups consecutive batches only when their complete fresh
fire OR trees have the same nonempty set of EventEdge ValueIds. Typed scalar
TwoState EventEdge, Assign, Or and false constants establish that key; leaf
order, duplicate leaves and false padding do not change it. Unclocked latch
writers, unsupported forms and batches with nontrivial entry locals
stay on their original caller path and separate adjacent groups. Entry locals
must have ordinary empty initialization and plain numeric types or arrays of
those types; in particular, a String constructor/destructor remains observable
at its original root entry even when the root's body fire is false.

Each group contains at least two batches. Its caller guard reads the actual
EventEdge pulses immediately before that group's original dispatch position,
after all preceding nonsink and writer calls. For example, consecutive batch
fires `a | b` and `b | (false | a)` can be called inside `if (a || b)`, while
both batches retain their own original fire guards and full bodies. No held
Or/Assign snapshot, payload enable or activity bitmap is sampled into this new
guard. With all leaves false, those original outer fire guards admit no body
work, and the qualified plain entry locals have no effects. With any leaf true,
the complete original batch sequence still executes. Batches with the same
key on opposite sides of a fallback or different key remain in separate groups.
Large capture reset and nonsink dispatch keep their original position. Small
function budgets can split this driver into helpers; the guard still encloses
the guarded loop and no driver spill frame is introduced.

The nonsink partitioner separates operations depending on an EventEdge clock
input or fire result from independent data operations. These materialized
boundaries retain ordinary change publication and state-reader subscriptions.
For nonsink DPI calls and system tasks, the emitter additionally subscribes
the effect owner directly to every EventEdge leaf in its verified fire-control
Or/Assign tree. A held join may stay true between consecutive real edges, so
its value-change notification alone is insufficient after splitting owners.
The effect still checks its fresh fire and enable at its original ordered
application site. This subscription does not change held fire-as-data values,
reorder effects, or replay a consumed edge on an unchanged eval after an
exception. Event timeslot tasks retain their existing sticky-trigger path.
`activation-stats.json` records effect owners and newly added leaf fanout edges.
The DPI-trigger regression now requires an unclocked call depending only on
`a` to retain its call count when another call's `d` or clock changes. Its former
expectations counted extra calls from sharing that other call's coarse owner.
The clocked call's pulse count, return values and held results remain checked;
effect order constrains the order of active calls without activating a call
whose inputs did not change.

Nonsink owners with no External operations or String operands/results can
group boundary publications of scalar TwoState Logic values up to 64 bits.
Values represented in the small-sink enable bitmap retain immediate publication.
For each complete nonempty publication key shared by at least two emitted
publication sites, a local dirty flag accumulates each old-snapshot comparison.
The key contains both the full nonsink subscriber set and the exact small-run
packed masks. Equal nonsink sets with different small-run masks form different
groups; an empty nonsink set with nonempty small-run masks can still group.
The emitter borrows the existing immutable set/mask storage for key comparison,
without copying subscriber sets or expanding masks into run sets. During execution,
the snapshot copy and small-sink boundary synchronization stay at that site's
original position. For example, `oldA` and `oldB` are stored snapshots and
`newA` and `newB` are newly computed values with the same complete key:

```cpp
changed = false;                 // owner entry
changed |= oldA != newA;
oldA = newA;                     // synchronize A's small-sink copies here
changed |= oldB != newB;
oldB = newB;                     // synchronize B's small-sink copies here
if (changed) {                  // owner exit, preserving current/next routing
    activate(nonsinkFanout);
    sinkDirty[word] |= runMask;  // each exact packed word in the shared key
}
```

The flag uses ordinary locals/chunkFrame storage and is explicitly reset before
the owner's operations. The shared flush emits each nonsink activation mask and
small-run invalidation mask once for the group. Writers execute only after all
nonsink owners, so delaying these invalidations to this owner's exit cannot
skip a writer between publications. Empty keys need only the original-position
copy and synchronization. Nonempty keys with one publication site, other types,
Event/fire publications and excluded owners retain their immediate paths.
Group counts use the operation emitter's elisions and are checked
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
the bitmap again. Index validity and masks retain their behavior, including
valid zero-mask and same-value writes. An on-write commit marks the existing
state dirty bit; Publish activates all state readers without a before-image,
address subscription or touched-leaf bitmap. On-change states retain a sparse
`largeBeforeImageStore` to detect the final value after multiple ordered writes.
Whole-array on-change writes additionally retain their aggregate before-image.

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

Within an activity-eligible small run, consecutive private scalar Logic muxes
with the same exact payload demand can form bounded expressions. A producer
is folded only into its sole data-operand occurrence; shared values, selectors,
stored/observed results, non-mux operations and demand boundaries stay
materialized. For example, `u = mux(a, x, y); v = mux(b, u, z)` can emit one
assignment to `v` with nested `numeric_select`, without a local or frame slot
for `u`. The existing width/sign conversions and eager scalar argument
evaluation remain intact. No read, effect, writer application or guard moves
across the region. Execution counters retain their original demand points,
and the final statement records every folded operation in helper provenance.
Each expression has at most 16 mux nodes and depth 8. A deferred expression
larger than 32KiB is materialized; this byte bound applies to carried text,
not the final assignment's total length. `private-mux-stats.json`, referenced
by the manifest, reports actual folded nodes, removed local assignments and
the largest actual expression tree. These are generation statistics, not
runtime savings.

A writer batch whose every run already qualifies for small-run activity also
checks the union of those exact-run bits before entering its split body. Its
existing batch fire gate runs first. The sparse probe reads `sinkDirty` at that
batch's original position; it never snapshots dirtiness at phase entry. When
all bits are clean, no run in this batch can execute and invalidate another run,
so the whole batch can skip. Otherwise every original run-level dirty test and
clear remains live, observing earlier target changes in the same batch. Later
batches likewise observe earlier batches' writes. Mixed batches retain their
existing path, including all fallback runs. The original 128-run and 16-distinct-
fire batch boundaries, eligibility, bitmap initialization and clear lifecycle
are unchanged.

Small Logic writer runs with private deterministic payloads have an additional
instance-local `sinkDirty` bitmap. Each bit identifies one exact contiguous
`SinkSchedule::generalRuns` entry, including when several entries share the same
partition key. A clear bit means the run previously completed, its external
enable/index/data/mask/guard snapshots are unchanged, and its pending small
targets retain the values left by that completion. Fixed full, masked and
indexed assignments compose to an idempotent fixed-bit update under those
conditions. The emitter can skip that run at its original position; its fresh
clock fire and existing enable/guard probes still enclose the activity test.
The bit clears after the complete original body, so an interrupted execution
does not establish the clean invariant. A skipped outer probe does not clear it.

Eligibility requires small TwoState Logic targets without history roles and
clocked writers whose operand 0 is the group's identical verified fire value.
That value can be an EventEdge or a finite Or/Assign tree of EventEdges and
zero constants, with at least one edge leaf. Eligible writer application uses
the same fresh edge expression as its outer run guard; it does not sample a
held Or/Assign boundary as fire. Every fallback writer retains its existing
sampling path. Latches, dynamic fire used as data, sampled/effectful operations,
non-Logic values and observed local results retain their original path.
Private current-state reads of TwoState Logic registers/latches without history
roles can qualify, including indexed reads and large current source arrays;
their dynamic index dependencies must qualify through the same analysis.
Local identity/snapshot/array views,
leaf elisions and leaf plans also retain that path. Each external dependency
must have materialized input storage or a non-elided single-result nonsink
publication; sampled/system-function/DPI frontiers are excluded. The analysis
stops at those actual boundary values instead of expanding nonsink ancestors.

An actual Input change sets its subscribed run bits inside the existing
snapshot comparison. Boundary publications compare before the stored snapshot
or role views are overwritten, then invalidate immediately or at the complete-key
group's owner-exit flush described above. Every actual
small pending-shadow change also sets all eligible runs targeting that state,
including self and competitors whose data did not change. Whole-array and
fallback writes retain their changed-shadow guard. An eligible small scalar
TwoState Logic writer of width 1..64 instead compares its sampled replacement
with the preceding pending value, producing a uint64 zero/all-ones change mask.
It writes the replacement and immediately ORs the original state dirty bit and
each complete target fanout word, ANDed with that mask. For example:

```cpp
change_mask = UINT64_C(0) - static_cast<std::uint64_t>(replacement != next);
next = replacement;
dirtyStateFlags[state_word] |= static_cast<std::uint8_t>(change_mask) & state_bit;
sinkDirty[subscriber_word] |= change_mask & subscriber_bits;
```

`replacement` is the original sampled payload, including any mask applied to
the old pending value. `next` is the same small pending-shadow field as before.
`state_word/state_bit` identify that state's dirty flag; every
`subscriber_word/subscriber_bits` pair comes from its original complete target
fanout, including self. OR with zero preserves preexisting dirty bits; all
notifications still occur at this writer, before the original exact-run clear.
There is no run-level notification accumulator. Scalar masked writes use the
same route; arrays, latches, large writes, and rejected runs retain their
existing implementation. This avoids a generated per-field change branch but
adds same-value native stores and zero-mask RMW traffic. It is not by itself a
runtime performance claim.

For example,
ordered A/B/A runs writing one state cannot skip the last A after B changes the
shadow, even if that A's data is unchanged. Changes that restore the original
current value still invalidate competitors before Publish sees the final value.
Only the exact current run clears its bit after completion. Already-passed runs
remain dirty until a later legitimate clock event; these bits never create a
new fixed-point round. `init()` marks all bits dirty, while eval and round
boundaries preserve them. Large capture/commit, state dirty flags, sampled
stores, Event and Output histories retain their existing behavior.

Private state reads additionally subscribe their exact run to the source's
current value. A small source invalidates readers only when Publish actually
copies a different next value into current. Competing pending writes that
cancel before publication do not invalidate these readers. A large source
changes current in LargeSink, so each actual in-place store invalidates its
readers immediately after the assignment, before a later allocation/commit
can throw. This covers whole-array, scalar, indexed and masked store paths;
the existing on-write policy also invalidates after same-value stores. Large
Publish does not emit those masks again. A large A/B/A sequence conservatively
invalidates even when its final current value is unchanged. These reader
notifications only set existing run bits and do not change fire, priority,
pending state or phase order. Source-state readers participate in the existing
convergence requirement, independently of nonsink subscriptions.

Fanouts are cached as packed 64-bit mask vectors with subscriber counts, built
in ascending exact-run ordinal order. Publication and writer sites consume the
cached masks without rebuilding subscriber sets. `sink-activity-stats.json`,
linked from the manifest as `sink_activity_stats`, reports eligible/rejected run
and writer counts, rejection reasons, value/run and target/run pairs, maximum
fanout and mask-word counts, bitmap bytes, and actual emitted Input/publication/
shadow invalidation mask-site counts. `scalar_shadow_change_mask_writer_sites`
counts actual writer sites using the per-writer zero/all-ones route.
These are static generated-code counts,
not execution counts; high fanout can increase emitted mask code even with a
small bitmap. `eligible_joined_fire_runs` and `eligible_current_state_runs`
report expanded coverage. `subscribed_current_states` (with small/large
breakdowns), `unique_current_state_run_pairs`, `current_state_mask_words`,
`max_current_state_fanout` and `max_current_state_mask_words` report the new
generation-side subscription tables. `small_current_publication_mask_sites`
and `large_current_write_mask_sites` separately count the emitted extra OR
statements, including one static site inside a repeated leaf loop. Readers
reuse `sinkDirty`; there is no second runtime bitmap or retained payload cache.
`current_state_change_sources` counts source states with an emitted change
route, which may exclude immutable large states with no writers.
`publication_change_mask_sites_before_grouping` sums the packed
mask words at each actual publication site before aggregation;
`publication_change_mask_sites` counts emitted invalidation OR statements after
aggregation. `publication_aggregation_groups/sites` count complete-key groups
with a shared flag and their publication sites; the corresponding
`publication_aggregation_sink_groups/sites` fields count the subset with small-run
subscribers. `writer_batches` counts original writer batches,
`writer_batches_all_eligible` and `writer_batches_with_activity_guard` count the
all-eligible batches and their added guards, and
`writer_batch_activity_mask_words/max_mask_words` report the total and maximum
number of packed words read by those coarse probes.
`writer_dispatch_fire_groups` counts added consecutive caller guards,
`writer_dispatch_fire_batches` counts the original batch roots enclosed by
them, and `writer_dispatch_fire_max_events` is the largest complete leaf set
among those groups. These are static generation counts, not dynamic calls,
costs or savings. Generation checks that every subscribed value has an emitted
Input or publication change route. Skipped operations do not increment their
ordinary execution counters.

Publish snapshots and clears each dirty byte once when all eight state slots
have Logic or Real types, then publishes its set bits in state ordinal order.
These publications cannot create new dirty bits or throw. The snapshot is an
ordinary local preserved by the chunker's liveness analysis across helpers.
Bytes containing String states retain per-state clears so a throwing copy
leaves later dirty states pending. Reserved output-history bits are consumed
without publishing their states.
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
Declaration-budget spills that are used in only one non-repeated part can
instead use a part-local aggregate. This requires the function producer's
explicit `localizePrivateNumericSpills` guarantee that no local address
escapes the part. Pure numeric nonsink owners and writer batches without
external operations provide that guarantee. Only default-initialized numeric
scalars and arrays qualify; actual cross-part values, loop-carried values,
parameters, return control and nontrivial or explicitly initialized locals
retain the shared frame. Liveness is recomputed after splitting, so a value
that acquires another part consumer becomes shared. This keeps the same line
budget while allowing the compiler to remove private intermediate stores.
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

Private concat/replicate/mux assembly into shaped values may write the result's
leaves directly. A private wide TwoState constant supplies sliced literal leaf
patterns; adjacent equal patterns use one bounded loop, including explicit zero
writes when a frame is reused. Storage, boundary, output and saved scalar
consumers retain their materialized snapshots. Only operations whose complete
data-use set is covered by these plans are elided; a reverse-use worklist finds
the same coverage closure without repeated candidate scans. Source terminals
and mux controls remain live. Statically single-element copies and writes emit
one assignment while keeping the original guards and bookkeeping order.

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
targets `test_grhsim_execution_counts` and `inspect_remove_grhsim_ir_execution`
to run these workflows.

Family/paired diagnostics optionally use these counts for native final-graph
writer data cones. They count shared operations once in the union, exclude
non-compute categories from compute pricing, and leave unavailable original
packed members unpriced. They never reuse legacy mapping IDs or claim
legacy dynamic normalization. Instrumented models are diagnostic candidates,
not runtime-performance candidates.
