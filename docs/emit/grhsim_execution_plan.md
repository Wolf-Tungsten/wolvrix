# GrhSIM static execution-plan analysis

`EmitGrhSimCpp::analyzeExecutionPlan(design, options, plan)` runs the normal
GrhSIM model builder and batch planner, then returns before creating, removing,
or writing output files. It requires the same activity-schedule session and
backend options as emission, but does not require `outputDir`. A failed call
leaves `plan` empty. Normal `emit()` uses the same preparation path and does not
build the analysis snapshot.

The API is experimental and does not change RepCut, Verilator, or GrhSIM
pipeline defaults. It provides a dispatch and activation skeleton, **not** a
complete lowering cost manifest or a time predictor.

## Snapshot

`emit/grhsim_execution_plan.hpp` records:

- Compute and commit groups, operation IDs, active IDs, and the actual batch /
  active-word / helper-chunk membership selected by the emitter.
- Input, intermediate-value and state-read activation frontiers. Activation
  targets are **active IDs**, not supernode IDs. Compute and commit are distinct:
  ordinary compute changes do not activate commit groups.
- Per-target input bit masks from `buildInputBitActivationMasks`, together with
  input-change-flag eligibility. Missing per-target bit masks mean whole-value
  activation; they do not mean no activity.
- Actual event operands and edge kinds, state writers and shadowing, row-selective
  memory-reader eligibility, and selected storage / dirty-word counts.
- Writer enable / mask value IDs, data width, and backend literal nonzero proofs.
  `Unknown` is not false: it covers dynamic expressions and X/Z literals. Memory
  fill has no explicit mask value and reports an implicit nonzero mask.
- Flags for fullpass variants, word bypasses and activation overlays, so a
  consumer cannot silently assume ordinary dispatch.

Groups and IDs belong to the supplied graph and schedule. Rebuild after graph
mutation, repartitioning or rescheduling. The snapshot is not a reusable weight
vector that can be moved unchanged from one partition to another. It can be
computed for a tentative partitioned GRH without emitting or compiling C++;
it is not yet an incremental search-inner-loop planner.

`estimatedLines` is the emitter's existing batching heuristic. It is not actual
generated lines, native instruction count, time or a new cost-model coefficient.
Likewise, recorded storage bytes are not cache traffic or changed bytes. Late
expression lowering, actual dirty writes, repeated delta rounds and native code
layout remain outside this version's coverage. The commit guard/body snapshot
below covers selected late grouping decisions, not every lowering operation.

### Shared commit guard/body plan

The snapshot now calls the same pure planners used by normal emission for:

- `commitEventChecks`: per-active-word event predicate clusters and the groups
  sharing each check. Literal `true` entries set flags without an event test.
- `commitWriteRuns`: consecutive writes with the same exact event expression,
  including concat-cache boundaries, homogeneous event dispatch, and enable
  groups formed from the actual resolved backend predicate expressions.
- Write body groups: single writes, lockstep writes sharing one change predicate,
  same-reader writes sharing activation, and ordered-memory affine loops.

Expressions are backend predicate identities, not signal-name matching rules or
measurements. Raw event values/edges remain in `events`; enable value IDs and
operation membership connect the lowered groups back to the GRH conditions.
A write body executes under its event and enable conditions; stores/activation
also depend on data changes. These identities must survive costing.

Counts are structural. An enable group is a logical predicate group, not a
native branch: ordered-memory lowering can put several groups in one loop,
constant guards may fold away, and one body can contain word/row loops. An event
check site is not a visit per simulation step. `Single` does not imply one machine
instruction, one comparison or the same cost for scalar and wide/memory writes.

Result-producing effects mixed with writes, activation overlays and the reserved
scalar-shadow compression path are explicitly listed in `unmodeledCommitWrites`
instead of silently approximated. Event-qualified continuation selection reuses
the normal backend proof/policy. Non-write effects and compute-expression lowering
are not made cost-free by their absence from this commit-only inventory.

### Conditional lowering work features

`CommitWriteBody::enabledWork` and `changedWork` separate source-level work
conditional on event+enable from work additionally conditional on a data change.
They are **not** machine-instruction counts or nanosecond estimates. A body can
contain loops; counts expand the relevant live words/rows, not just source sites.

| Feature | Unit / condition |
| --- | --- |
| `scalar_next_values`, `scalar_comparisons` | Prepared scalar values / comparisons after event+enable; one for a lockstep group, one per member for a same-reader group. Casts/preparation are not necessarily native instructions. |
| `word_next_values`, `word_comparisons_upper` | Live 64-bit words prepared / compared. Some helpers short-circuit comparisons; the comparison entry is an upper bound. |
| `mask_merge_words` | Scalar or wide word merges selected by the backend mask mode. |
| `unconditional_word_stores` | Live word stores in `assign_words` / masked-in-place helpers, executed even when the resulting state is unchanged. Still conditional on event+enable and valid addressing. |
| `scalar_store_slots_upper` | Scalar stores conditional on change. Same-reader groups store **all** members if **any** member changes; these are not per-member-change-only stores. |
| `fill_row_iterations`, `packed_fill_slice_words` | Memory-fill row iterations and packed-fill extraction width. A repeated row value is not a packed fill. |
| `memory_row_addresses`, `row_bounds_checks` | Address computations / bounds-test sites selected by the backend; not cache misses or actual in-range frequencies. |
| `reader_activation_calls_upper`, `reader_targets_upper` | Semantic reader-notification calls and target memberships, **not** physical bitmap writes. Row selection and repeated targets may reduce actual work. |
| `producer_dirty_bank_or_sites` | Dirty-bank OR sites, deduplicated within shared write groups using the actual producer-dirty map; not inter-partition copied bytes. |

`change_or_terms` describes the per-member conditions combined by same-reader
lowering. An ordered-memory body retains individual enable tests and writer
priority; `ordered_writer_iterations_upper` assumes all relevant writers are
enabled. Its changed-row notification bound uses at most
`min(writer_count, row_count)` different rows, with a separate row-scan count.

A literal-zero memory mask selects a backend-elided body. A zero register mask
can still select source-level merge/compare work, even though state-change
analysis proves no possible update. Native optimization may further remove
this work. Consequently an adapter must **not** gate every enabled feature by
`writeMasks`. Wide helpers also demonstrate why not all stores belong in
`changedWork`. Eager-scalar late activation is explicitly marked in
`unmodeledWork`, rather than silently treated as the normal conditional path.

`computeLowering` starts the compute-side inventory by reusing the actual local
ordering, priority-mux and bool-producer-reduction planners. It reports absorbed
operations, direct inverted state views, priority chain/bitmap choices and
tables, and bool reduction regions/producer iterations. Bool reduction retains
one comparison and one unconditional store per producer; its `bool_reduce_or_ops`
counts the two **per-producer** ORs, not helper-call/outer-region merge overhead.
Priority choice terms and bitmap groups are worst-path counts, not all executed
on every visit. Table entries describe static size, not data-cache traffic.

The generic compute inventory also records the backend shape selected by the
normal emitter. The read-only classifier reconstructs the local alias/use-count,
concat-cache, demanded-bit and state-read context; it does not emit or compile
C++. Scalar results are separated into inline/temporary/materialized forms and
helper/native/byte-bool expressions. Wide results expose word counts, helper
kind, demanded-bit/slice, packed-lane access, and static/dynamic slice helpers.
The helper/native classification checks actual helper-call syntax; a local or
storage identifier beginning with `grhsim_` is not a helper call. Slice helper
iterations count **output** words, even for scalar results extracted from a
wide input. Input-word inventory, possible cross-word merges and index
normalization remain separate features. Whole-input reductions still count
input words. These are structural loop bounds, not measured instruction counts.
Wide concat distinguishes ordinary `wide.concat_wordwise`, fallback
`wide.concat_insert`, and `wide.concat_producer_dirty` (the producer-dirty word
dispatch has a different control path even though both may use
`concat_wordwise_*` temporaries). State/memory reads and DPIC/system-task/
system-function schedule boundaries are explicit shape features. These labels
remain descriptive backend units: they are not instruction counts or timing
coefficients, and generic membership is retained for exact operation
conservation.

The same snapshot records first-order mechanism units alongside the shapes:
operand bit/word totals, scalar helper-call and ternary sites, wide helper input
words, and concat source words. They describe the amount of lowering exposed to
the backend (for example, how many 64-bit words a helper must touch), but do not
claim that every word becomes one instruction or one store. They should be
combined by helper family and publication/change-guard path; summing all units
would double count the same expression.

For wide helpers the inventory additionally exposes output-word/iteration upper
bounds, possible cross-word slice merges, dynamic-index normalization sites,
wide-reduction word iterations, and static replication-lane expansion. These
units have deliberately different meanings: a replication lane can be a
compile-time template expansion, while an index normalization is a run-time
guard path. Consumers must keep the helper kind and active phase with the
feature instead of applying one global word coefficient.

`bucketizeGrhSimWork(work)` is the first structural backend-cost adapter for
this inventory. It returns a `GrhSimBackendCostReport` with the following
properties:

- `conditionalFeatures` keeps the original activity mask and stores each raw
  feature under exactly one mutually exclusive family. The mask is a symbolic
  scenario/may-set, not a promise that the row is a clock phase; a caller must
  map its own scenario bits to rising/falling or other workload phases.
- `staticFeatures` is copied without reinterpretation. It remains dispatch or
  coverage metadata and must not be added to conditional work without an
  explicit policy.
- `unbucketedFeatures` is an explicit error surface. A newly introduced raw
  feature has to receive a family mapping before an adapter can use the report;
  it is never silently treated as zero or folded into a catch-all bucket. It
  is indexed by the original activity mask as well, so an audit can report the
  missing rule in its scenario context.

The current mutually exclusive families are `dispatch`, `commit.event`,
`commit.enable`, `commit.enabled`, `commit.changed`, `coverage`,
`compute.constant`, `compute.state`, `compute.memory`, `compute.side_effect`,
`compute.scalar`, `compute.wide.slice`, `compute.wide.reduce`,
`compute.wide.concat`, `compute.wide.replicate`, `compute.wide.wordwise`,
`compute.wide.shape`, `compute.special`, and `compute.generic`. For example,
cross-word merges and dynamic index normalization go to `compute.wide.slice`,
reduction-word iterations go to `compute.wide.reduce`, and ordinary helper
word iterations go to `compute.wide.wordwise`; the original feature name and
count remain available inside the family. This is a representation boundary,
not a set of fitted coefficients: family totals do not yet predict native
instructions, synchronization, transfer bytes, or walltime.

The adapter is deliberately separate from `summarizeGrhSimWork` so existing
consumers still receive the raw feature vector. Before using a bucket report in
an ASC/partition evaluator, a consumer should require an empty
`unbucketedFeatures`, verify raw-to-bucket conservation, and then apply a
backend-specific mechanism model that also retains phase and publication/
change-guard conditions. Adding a family or coefficient is therefore an
explicit model change rather than an accidental change in the generic
fallback.

`projectGrhSimMechanismUnits(backend)` is the next representation layer. It
maps each bucketed raw feature to one named mechanism unit and keeps a reverse
`conditionalAttribution` map from unit to raw feature. This makes the
projection auditable without pretending that units are commensurate. Examples
include:

- dispatch batch/word slots;
- commit predicate groups, scalar/word prepare and compare sites, stores,
  row/address checks, and reader/producer-dirty activation bounds;
- scalar operand bits/helper calls, wide helper input/output words and word
  iterations, slice cross-word merges/index normalizations, reduction-word
  iterations, and replication lanes;
- state/memory/side-effect shape units and generic operation/result coverage.

The report retains the activity mask for every unit and reports unmapped raw
features separately. A consumer must check that this map is empty and that
attribution conserves every `(mask, feature)` value before using the units.
Units are separate axes: for example, helper input words and helper output
words must not be added together as if they were one instruction count. No
coefficient, toggle probability, phase frequency, synchronization cost or
walltime estimate is introduced by this projection.

### Operation provenance before partitioning

`ComputeLowering::sites` retains the operations consumed by each compute
lowering step and that step's feature map. A fused region keeps its consumed
operations together. Summing site features reproduces the existing group
features, and site membership partitions `loweredOperations` plus
`genericOperations`. The enclosing supernode is still the lowering context:
for example, a priority-mux root's features depend on the absorbed chain even
though the root and absorbed-operation coverage have separate sites.

`attributeGrhSimWork(plan, activity)` uses the same conditional-summary path as
`summarizeGrhSimWork` and returns operation sets, supernodes and activity masks
for compute sites, inner event checks, enable checks, enabled bodies and
changed bodies. Shared checks/bodies remain one record; their work is not
divided evenly among writers. Zero and unknown masks are preserved. Static
dispatch and coverage remain in `summary.staticFeatures`.

For every `(mask, feature)`, the sum of attributed sites and
`unattributedFeatures` equals `summary.conditionalFeatures`. Missing compute
site provenance in older or manually constructed plans is reported in
`unattributedFeatures`. Partial or duplicated compute membership and
nonconserving feature maps are rejected rather than silently repaired.

This provides a provenance bridge to semantic operations/ASC pieces without
using partition totals or timing measurements as weights. It can run on an
unpartitioned graph, but its output is **not an assignment-invariant cost
model**. Materialization, coactivation, shared commit groups and local/remote
state phase delay can change with the assignment. A site that spans pieces
with different ASC memberships cannot be assigned to one piece by arbitrary
splitting. Even single-piece provenance does not prove its lowering context
will remain unchanged. Consumers must retain these interactions and replan
the affected contexts before treating the features as movable costs.

Operation IDs are those of the **scheduled** graph. The activity scheduler
clones source reads/constants, so not every ID exists in the raw ASC graph.
A raw-graph consumer must retain explicit ancestry through scheduling and
distinguish those copies from original semantic pieces. Source ancestry says
where a clone came from, not where its work must be charged: source copies
execute in consumer contexts, whose ownership may differ from the stored
state's writer. The diagnostic probe carries ancestry in copied, in-memory
attributes; this does not alter the production scheduler or generated models.

`loweredOperations` and `genericOperations` partition compute operation
membership. The latter is an **unpriced fallback**, with descriptive result
word/local/materialized/state-view counts only. Even the selected special
families are not a complete compute-body cost: result publication, region/helper
overhead, ordinary expression expansion, demanded-bit lowering, concat caches,
packed-array indexing, current-zero guards and deferred activation still need
their own cost adapters. Non-write commit effects, DPI, startup, repeated delta
rounds, code layout, synchronization and actual transfer bytes are not priced
by this feature inventory. An empty `unmodeledWork` means the supported variant
was recognized, **not** that all simulation work has been priced.

`summarizeGrhSimWork(plan, activity)` composes these features with an activity
slice. `staticFeatures` holds dispatch inventory and coverage counters;
`conditionalFeatures` is keyed by a **joint may mask**, retaining unknown and
phase overlap. It keeps event-check visits, dynamic enable predicates, enabled
body features, changed-state features and compute-group features separate.
Literal true/false guard groups are reported separately from dynamic tests.
An internal event check is qualified by the enclosing group's possible events,
not only by the event being tested. Mask zero is retained for reconciliation.
No enable/toggle probability or number of visits per simulation step is inferred.
Pair seeded-root and full-envelope summaries; this API is a feature vector, not
a complete scalar cost model suitable for replacing a production partitioner.

## Symbolic activity envelope

`analyzeGrhSimActivity(graph, plan, seeds)` propagates caller-specified scenario
masks. Bits 0–62 identify scenarios; bit 63 is `kGrhSimUnknownActivity`, an
uncertainty marker. Scenarios need not be mutually exclusive. Unknown is not
zero activity, a probability or another execution phase to add to known phases.

Input seeds are disjoint, ascending half-open bit ranges. For example, a 130-bit
boundary bus can contain `[0,64): A-rise`, `[64,128): B-fall`, and `[128,130):
unknown`. A static slice of bit 0 keeps only `A-rise`. Treating the whole bus as
one label would contaminate that slice with the other register groups' events.
Missing input seeds are unknown; an explicit empty range vector represents no
recurring input change in the specified scenario basis. Startup is excluded.

Event seeds independently specify posedge, negedge and anyedge masks for each
root. One-bit assign / bitwise-not / logical-not aliases resolve to those roots;
inversion swaps posedge and negedge. The legacy bool overload leaves unproven
generated/gated clocks unknown; the explicit options overload can propagate
their value-change dependencies as described below.
The library does not choose a dominant clock or assume unrelated clocks are
synchronous. The caller must explicitly map cross-partition arrival to scenarios:
swapping rise/fall is appropriate only for a specified alternating half-cycle
harness, not for arbitrary multi-clock simulation.

State-change envelopes are qualified by the actual event operands exported by
the backend. Enables, addresses and data may suppress a write, so a qualifying
event is a possibility, not proof that a state write occurs. Read-only state has
no recurring change. Memory state changes conservatively cover all readers; the
row-selective flag exposes where this bound could be tightened later.

Ranges survive concat, static slices and equal-width assign/not plumbing. Other
operations conservatively union operand activity. A worklist reaches a fixed
point instead of assuming that operation IDs are topological. Passing
`preserveBitRanges=false` provides a whole-value control for diagnostics.

The report keeps distinct quantities:

1. `operationMasks` / `values`: potential change (or event-qualified payload).
2. `eventMasks`: scenarios in which an event-qualified operation may be eligible.
   This does **not** count fixed visits to its event check.
3. `writeMasks`: scenarios in which a writer may update state, after supported
   guard proofs. This does not prove a store or a change in stored data.
4. `computeGroupMasks`: potential execution, using actual input, boundary and state
   frontiers. A group can run because one member's input changes even when another
   member's result cannot change in that scenario.

For an internal intermediate value the current backend can use a whole-value
activation frontier even when a downstream operation reads only a slice. The
analysis preserves that extra group execution while retaining narrow value-change
provenance. It does not invent bit-selective activation where the emitter has none.

These are envelopes, not fire counts. Fullpass and activation-overlay variants
are rejected by the current activity consumer. Ordinary word bypasses and
row-selective readers may make the envelope conservative. Event-qualified commit
payload must be counted separately from unconditional commit guard/dispatch
visits. Further cost adapters must also account for expression elision, shared
lowering, conditional stores and synchronization before predicting walltime.

## Factored event and write-guard analysis

Use `analyzeGrhSimActivity(graph, plan, seeds, GrhSimActivityOptions{})` for
derived-event propagation and proved write-guard suppression. The bool overload
is retained as the independent v1 opaque-event control. The `unseededEvents`
policy selects `OpaqueControl`, `ValueDependencies`, or `SeededRootsOnly`.
Literal write proofs and bit-range precision have independent options.
Neither API changes generated code or production pipeline defaults.

For an event expression not resolved to a seeded root, an edge is possible only
when the expression can change. The options overload propagates that dependency
through the same value/state fixed point. For example, if `qA` may change in
scenario A and `qB` in scenario B, `posedge(qA | qB)` is bounded by A or B, rather
than a newly invented unknown source. It does not prove which polarity occurs:
both posedge and negedge conservatively inherit the change set for a non-alias
expression. A genuine unseeded input remains unknown. Constant/explicitly held
values cannot produce recurring edges; this analysis excludes startup and
assumes a settled state, not an initially excited autonomous feedback loop.

Event, enable and mask are different predicates. Normalized writer operands are:

- Register/latch write: `[enable, data, mask, event0, ...]` (latches have no events).
- Memory write: `[enable, address, data, mask, event0, ...]`.
- Memory fill: `[enable, data, event0, ...]`, with an implicit full mask.

The snapshot reuses the backend's constant reader. A literal-zero enable or
all-zero mask suppresses **possible state changes**, but does not suppress event
checks in `eventMasks` or remove dispatch costs. Nonzero masks are checked across
all words, not only the low 64 bits. For example, a write on event A with a zero
mask has `eventMasks=A`, `writeMasks=0`. A stable unknown enable can be 1, so the
same write with that enable retains `writeMasks=A`; an empty enable-change set is
not a false predicate. Multiple writers union their remaining state-change sets.
Event-less/latch writes remain unknown unless a literal write proof disables them.

This version exposes dynamic predicate IDs but does not estimate their truth
probabilities, correlated guard selectivity, memory address validity, unchanged
data comparisons, native branch costs or late shared-guard lowering. A cost
adapter must keep fixed dispatch/checks, event-and-enable payload, and changed
store/activation/publish work separate. It must not multiply all three by a
single event frequency or charge all unknown work on every known clock phase.

### Conditional seeded-root slice

`GrhSimUnseededEvents::SeededRootsOnly` analyzes a conditional slice in which only
events resolved to caller-seeded roots fire. Unresolved event alternatives remain
in the execution plan and `residualEventReferences`; they are disabled **only in
this slice**, not removed from the generated simulation or full may envelope.
For a writer guarded by `(posedge clock OR posedge derived)`, the clock arm stays;
the derived arm remains an additional condition instead of making every phase
in which `derived` could change pay its entire body cost.

Pair this slice with `ValueDependencies` over the same graph/options. For a package,
propagate each slice across partition boundaries to its own fixed point. The
complete envelope must be a superset; work reachable only in the complete case
requires additional unseeded-event conditions. Shared operations in both must
not be charged twice. The slice is **not** a lower bound on execution time: enables,
unchanged data and group coactivation remain conservative, and it is **not** a
safe replacement for the full may analysis. No unseeded event is assumed rare,
inactive or zero-cost globally, including gated clocks and internal reset sources.
No probability or event frequency is inferred from this decomposition.

## Validation

`emit-grhsim-execution-plan` checks read-only behavior, deterministic subsequent
emission, group/batch membership, two independent clocks, inverted and gated
clocks, range boundaries crossing 64 bits, concat/slice round trips, whole-value
versus range propagation, group coactivation, explicit remote arrival, unknown
inputs and invalid ranges. Its emission-only mode supports byte-for-byte
comparison with a retained generator. The analysis has no runtime instrumentation.

`emit-grhsim-guard-activity` covers composite positive/negative events, genuine
unknown inputs, stable-but-unknown enables, literal-zero enables/masks, X enables,
multiple writers, event/state feedback, 130-bit masks and memory fills. Guard
proofs have an independent ablation so event attribution is not confused with
write suppression. It also checks direct-event alternatives in the conditional
slice, retention of residual conditions, and exactly-once enable/body coverage
for every fixture writer.
An ordered-memory fixture separately verifies that 16 logical enable groups
become one affine body group, not 16 independently emitted bodies.
