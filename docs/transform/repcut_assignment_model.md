# Experimental RepCut assignment accounting

`transform/repcut_assignment_model.hpp` evaluates an entire assignment of semantic
atoms to partitions. It is an independent experimental API: the RepCut pass, its
solver, Verilator cost models, and pipeline defaults do not call it.

The first use is GrhSIM partition research. The representation separates work
sharing from directed boundary transfers; it does not predict nanoseconds or
choose a partition automatically.

## Inputs

`RepcutAssignmentModel` has a versioned `adapter` name, an atom count, named
feature columns, work pieces, and directed values. Atom and piece IDs are dense
indices; value keys only identify values and define stable bundle member order.

An atom is an indivisible placement unit. The current experiment uses complete
ASCs. A `RepcutWorkPiece` contains the atoms that require it and a vector of
features supplied by a backend adapter. Features are counted once per partition
containing at least one referencing atom. Duplicate atom references are ignored.
An unreferenced piece contributes no executed work.

For example, a piece with feature `work=10` referenced by atoms 0 and 1 contributes
10 when both atoms reside in one partition, and 20 when they reside in different
partitions. Permanently assigning 5 to each atom cannot express both outcomes.

A `RepcutDirectedValue` contains a unique `key`, a positive bit `width`, the
`writerAtom`, and `readerAtoms`. The producer is unique. Repeated readers and
readers in the producer's partition cause no extra transfer. For writer-free
register/latch storage, `RepcutInferredOwner` retains the referencing atoms of
each read-user operation. The owner is the partition with the most such users,
counting a replicated user once per partition and breaking ties by the lowest
partition ID. All read ports of that storage share this decision, matching
Phase E. This owner must be recomputed after movement; it is not a fabricated
writer ASC. External top-level IO and unsupported storage ownership policies
must be reported separately by the adapter.

Feature columns can represent different work quantities or event scenarios. The
evaluator does **not** infer activation, probability, exclusivity, or phase delay.
An event-aware model builder must account for assignment-dependent state arrival
before providing scenario costs. Summing the columns is not a valid time model.

## Directed transfers

Values with the same producer partition and exactly the same remote destination
set are bundled. Members are ordered by value key, with the first member at the
most significant end. This matches the current RepCut boundary bundle policy;
tests compare against `buildRepcutBoundaryBundles`.

`RepcutTransferModel` is a separately named backend callback. The initial adapter,
`grhsim-copy-v1`, describes the current native scalar and 64-bit-word port storage:

| Bit width | Storage / comparison shape |
| --- | --- |
| 1–8 | 1-byte scalar |
| 9–16 | 2-byte scalar |
| 17–32 | 4-byte scalar |
| 33–64 | 8-byte scalar |
| More than 64 | `ceil(width / 64)` 64-bit words, compared in groups of 8 words plus a tail |

The report includes per-partition sender and receiver calls, scalar/wide calls,
checked storage bytes, full 64-byte comparison groups, tail calls/bytes, and
distinct peer counts. It also retains bundle members and bit offsets. These are
structural features, not actual changed bytes, physical cache lines, bandwidth,
or coherence transactions. Conditional dirty hooks and port addresses are not
predicted by this version.

Sender and receiver totals describe the same transfers; adding both would count
each transfer twice. Equal global transfer volume can still have different
maximum sender loads, which matters for a barrier after each update phase.

## Evaluation and movement

`RepcutAssignmentEvaluator::evaluate(assignment, K)` performs a complete rebuild.
Every atom must have one partition ID below positive `K`; empty partitions are
allowed. It reports per-partition feature sums, referenced unique work, total
replicated work, and the transfer plan. For each feature, duplication equals
total work minus referenced unique work.

`RepcutAssignmentState::moveAtom` updates piece loads through first/last occupancy
counts and rebuilds communication. Both invalid moves and arithmetic overflow
leave the state unchanged. The evaluator must remain alive and unchanged while
the state uses it. Communication rebuilding is deliberately a correctness
reference; it must be made incremental before use in a large search inner loop.

The constructor rejects invalid references, ambiguous value keys, and mismatched
feature dimensions. Aggregate arithmetic is checked. Changing iteration order
does not change report contents. The transfer estimator must be deterministic
and provide consistent nonnegative feature counts.

## Current boundaries

This API establishes set accounting and a checkable transfer plan. It is not yet
a GrhSIM lowering/activity cost model, an independent partitioner, or a new
default. Subsequent model builders need event domains, unknown activity, schedule
groups, materialization/commit work, and state-layout features. They must share
rules with the backend planner rather than fit per-partition timing coefficients.

The standalone `transform-repcut-assignment-model` test covers shared work,
first/last reference moves, multiple feature columns, native widths and tails,
bundle equivalence, equal-total/different-sender examples, backend substitution,
256 deterministic randomized moves, malformed inputs, and overflow rollback.

The separate [GrhSIM execution-plan analysis](../emit/grhsim_execution_plan.md)
now exposes the actual pre-emission dispatch/activation skeleton and symbolic
bit-range event envelopes. It requires a tentative partitioned and scheduled
GRH; it is not yet connected to this evaluator's search costs. In particular,
moving an ASC invalidates its old backend grouping and arrival relationships.

## GrhSIM proposal and exchange evaluator

`transform/grhsim_assignment_proposal.hpp` adds an opt-in proposal layer on top
of an already evaluated assignment. Its six feature columns are named
`rising/falling.{compute_core,commit_publish,wide_helper}`. The score keeps the
two phases separate and records, for each phase, the maximum of each of the
three axes across partitions. It also records maximum per-part sender and
receiver transfer features/peer counts and the global transfer totals.

`grhsimProposalDominates` compares these independent quantities component by
component. A candidate must not be dominated by the baseline or another
candidate to remain in the proposal set. The optional selected move is chosen
from that set by a documented lexicographic order, with `(atom,destination)` as
the final stable tie-break. This is a deterministic policy, not a weighted
walltime predictor; phases are never added together and communication sender
and receiver totals are not added to each other.

`proposeGrhSimMoves` requires an explicit candidate-atom span. The current
prototype re-evaluates each one-atom move completely, so its complexity is
`O(candidate_atoms * K * assignment_evaluation)` and it must not be pointed at
all 300k XiangShan ASCs accidentally. An incremental communication/phase cache
is required before using it as a large search inner loop. The test
`transform-grhsim-assignment-proposal` covers axis maxima, transfer tails,
dominance, deterministic selection, duplicate/out-of-range candidates and
missing feature columns.

`withGrhSimPhaseFeatures(base, pieceFeatures)` is the explicit adapter boundary
for supplying those six columns. The backend must provide one six-element
array per semantic piece; the function appends the columns while preserving
atoms, values, and existing structural features. It does not derive piece
features from a partitioned report. That distinction is required because the
current `mechanism_cost.csv` is emitted after an assignment and contains
partition-level coactivation/materialization results. Feeding those totals
back into ASC pieces would make the cost model depend on the assignment it is
supposed to evaluate. A future emitter attribution pass must produce
piece/ASC-level phase units before the proposal evaluator can drive a complete
XiangShan search.

The backend now exposes operation-set provenance through
`attributeGrhSimWork` (see the execution-plan documentation). This closes the
loss of source identity in partition-level summaries; it does not make those
features assignment-invariant. Shared guards/fused bodies spanning different
piece memberships must remain interactions. Even single-piece sites retain
their scheduling context, and local/remote state delivery can change their
phase masks. The six-column adapter is valid only for features the caller has
proved stable under its proposed moves, or after rebuilding the affected
contexts. It is not an automatic converter from a frozen lowering snapshot.

## Replay and assignment-dependent GrhSIM work

The experimental C++ backend `partitioner="assignment-replay"` takes a typed
`RepcutReplayAssignment` from the session key `<path>.repcut.replay_assignment`:

```cpp
SessionStore session;
session.emplace("top.repcut.replay_assignment",
    std::make_unique<SessionSlotValue<RepcutReplayAssignment>>(
        RepcutReplayAssignment{atomPartitions}));
RepcutOptions options;
options.path = "top";
options.partitionCount = 3;
options.partitioner = "assignment-replay";
// Set manager.options().session = &session before running RepcutPass(options).
```

The labels must use the ASC order of the exact input graph. Replay requires
one in-range partition label per ASC; missing or wrongly typed session values
fail without falling back to a solver. It retains Phase A–C construction and
guards and the normal Phase E rebuild, but bypasses both the solver and closure
refinement. The ordinary Mt-KaHyPar path and its defaults remain unchanged.
This is a rebuild reference for explicit candidate assignments, not a new
allocator or a public command-line assignment-file loader.

`emit/grhsim_partition_work.hpp` summarizes a rebuilt, scheduled partition
using its real execution plan and activity report. `GrhSimPartitionWork` keeps:

- Source-read views for constants, local state and local memory, with the actual
  consumer-side site and schedule context.
- Explicit primary, deferred-state, early-effect or unknown input bindings,
  their arrival ranges, bit-level activation and direct IR users.
- Actual shared write bodies and their enabled/changed site references; the
  body is not divided into fixed independent costs for each writer.

These are indexed views over `attributeGrhSimWork` sites, not extra charges;
the conditional feature totals remain identical. Missing bindings stay
unknown, and invalid/duplicate inputs or inconsistent remote writers fail.
The caller supplies the activity scenario. In particular, the register-only
test's swap of rising/falling arrival masks models its alternating-clock
deferred-delivery harness; it is not a universal remote-input timing rule.

The `emit-grhsim-partition-work` test moves complete ASCs through this path.
It verifies local-read to remote-input conversion, changed arrival and group
coactivation, dissolution/formation of same-reader bodies, and deterministic
generated C++. It also shows why a future incremental cache cannot invalidate
only the source and destination partitions: repacked boundary buses change a
third consumer's generated code even when no ASC moves into or out of it.
Full rebuild remains the reference until such dependent interfaces and
activation contexts are tracked explicitly.

`repcutAssignmentDirectImpact(beforeAssignment, beforeReport,
afterAssignment, afterReport)` supplies the initial reconstruction scope for
two assignments of the same raw model. It returns a sorted set containing the
moved atoms' source/destination partitions and every sender/reader of a bundle
whose existence, width, members or bit offsets changed. It compares the bundle
layout, not just communication totals: swapping two equally wide payloads can
leave all per-part byte/call totals unchanged while still changing an unmoved
receiver's interface. Unrelated links are excluded.

Both reports must come from the same model's evaluator. The helper validates
assignment/partition dimensions and normalized transfer endpoints, and does
not modify either report. It is independent of backend timing coefficients
and is not called by the ordinary RepCut solver. Its tests cover bundle splits,
equal-byte payload replacement, local-only moves, unchanged/reordered reports,
unrelated partitions and malformed inputs.

This set is deliberately **direct impact only**. After rebuilding those graphs
and plans, changed output arrival ranges can still affect otherwise unchanged
downstream partitions. An activity cache must propagate those changes to a
fixed point; this helper does not authorize reusing their old activity reports.
