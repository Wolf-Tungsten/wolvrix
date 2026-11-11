# GRH-based GrhSIM transforms

These passes run through the ordinary PassManager on one flattened top Graph.
They use the [GrhSIM dialect](../grh/grhsim-dialect.md) on the same GRH design.
Conversion, unpartitioned optimization, all four split/partition passes and
the [C++ emitter](../emit/grhsim-cpu-st-cpp.md) form the production route.
The independent model, pass registry and CPU mapping have been removed.
See the [simulation workflow](../grhsim.md) for standard entries and evidence.

## Pipeline and options

Run the GRH frontend and ordinary GRH transforms first. The F22/F23 boundary is
explicit and stays before conversion:

```text
dead-code-elim (keep_declared_symbols=false)
  → expr-opt → used-bits → split-nodes
  → dead-code-elim (keep_declared_symbols=false)
  → constant-analysis
  → dead-code-elim (keep_declared_symbols=false)
  → alias-analysis → pattern-detect → comm-expr
  → dead-code-elim (keep_declared_symbols=false)
  → grhsim-dialect-translate
  → split-event-and-output        (first virtual partitions)
  → split-nonsink-and-sink
  → partition-nonsink
  → partition-sink                (first SinkSchedule artifact)
  → emit/grhsim/cpu-st-cpp.cpp
```

| Pass | Options | Defaults |
| --- | --- | --- |
| `grhsim-dialect-translate` | `-top`, `-logic-domain=2state|4state` | the unique top, 2state |
| `expr-opt` | `-top` | the unique top; declaration anchors off |
| `used-bits` | `-top` | the unique top; declaration anchors off |
| `split-nodes` | `-top` | the unique top; declaration anchors off |
| `constant-analysis` | `-top` | the unique top; declaration anchors off |
| `alias-analysis` | `-top` | the unique top; declaration anchors off |
| `pattern-detect` | `-top` | the unique top; declaration anchors off |
| `comm-expr` | `-top` | the unique top; declaration anchors off |
| `split-event-and-output` | `-top` | the unique top |
| `split-nonsink-and-sink` | `-top`, `-mem-min-bytes` | the unique top, 64 |
| `partition-nonsink` | `-top`, `-max-supernode-nodes`, `-segment-penalty`, `-semantic-nodes` | the unique top, 128, 1, 0 |
| `partition-nonsink` clone options | `-clone-shared`, `-clone-max-fanout`, `-max-clones`, `-clone-lossy-compare` | 1, 8, 250000, 1 |
| `partition-nonsink` hint/cost options | `-firrtl-nodes=off|prefer|auto`, `-cost-model=legacy|bytes`, `-report` | auto, legacy, no report |
| `partition-sink` | `-top`, `-enable-guard-min-size` | the unique top, 8 (0 disables hoisting) |

Factory arguments use `-key=value`, as with other ordinary passes. An explicitly selected top must
exist; without `-top` there must be exactly one top. Reports are diagnostics,
not executable plans.

## grh-to-grhsim

Conversion replaces the six writers and task/function/DPI calls in place,
preserving result identities, declarations, common computation and arrays.
It creates detectors for raw events and joins multiple pulses with one-bit OR.
Writer priority, process/timing data, call effects and sample identities become
explicit. Logic-domain selection updates real types/constants/InitSpec rather
than leaving a hidden emitter projection.

The result is `grhsim.stage=converted`, with no virtual ownership or session
artifact. Event/Output extraction, monitor history and small/large selection
do not happen here. Repeating conversion on a valid converted graph with the
same domain is a no-op. Unsupported source contracts are checked before
mutation. An unexpected failure after mutation marks `conversion-failed`,
which is not a valid checkpoint and must not continue into optimization/emit.

## expr-opt

This standalone pass is the GRH counterpart of GSIM's first `ExprOpt`. It
operates on an unpartitioned ordinary GRH graph before `grh-to-grhsim` and
performs the same-arm MUX expression rewrite. The caller runs
`dead-code-elim` before this pass, matching GSIM's explicit
`RemoveDeadNodes → ExprOpt` boundary. ExprOpt itself does not run DCE or
semantic-tree pruning. It uses an empty declaration anchor set by default and
leaves any newly dead alias rows for the caller's next explicit DCE.

## used-bits

This standalone pass is the GRH counterpart of GSIM's `UsedBits`. It runs
backward demand analysis and state/value prefix narrowing on the whole
ordinary GRH graph before conversion; DCE remains the caller's explicit pass. Declared symbols and
generate-group members are ignored as liveness roots by default; ports,
state/effect roots, and externally used values remain observable. SemanticTree
IDs are preserved on surviving operations and values; the pass does not prune
the semantic tree.

Indexed state reads demand every bit of each index operand, independently of
the bits demanded from the selected row. Arithmetic prefix narrowing for
`add`, `sub` and `mul` requires TwoState operands and results: in FourState,
an unknown discarded high input bit can make even the low result bits unknown.

## split-nodes

`split-nodes` is the first F23 ordinary-GRH structural pass. It consumes the
static slice demand exposed by `used-bits` and recursively clones the safe
scalar expression subset (`assign`, bitwise operations, `not`, `mux`, `concat`
and static slices) at the demanded ranges. The resulting narrow producers are
rewired into the existing slice users. Unsigned scalar pure expressions are
eligible; signed values, arrays, registers, state ports, arithmetic and
effectful operations are left unchanged until their GSIM-equivalent handling is
implemented.

The pass never performs liveness deletion or calls `dead-code-elim`. The old
wide producer remains in the graph so the caller can inspect the transformation;
the explicit `dead-code-elim` immediately after this pass is responsible for
reclaiming it when it has no remaining users.

## constant-analysis

`constant-analysis` is the next standalone ordinary-GRH pass in the F23 flow.
It walks the acyclic graph in producer order and evaluates pure logic
expressions whose operands are already represented by constants. Supported
arithmetic, comparisons, logic, mux, concatenation, replication and slice
operations are rewritten in place as `kConstant` while
retaining the result identity and source metadata. Division and modulo by a
proven-zero divisor are left unchanged so the existing runtime contract is
preserved. Pure system functions supported by the shared evaluator (such as
`clog2`) are folded; sampled or effectful system functions stay opaque.

Producer order uses a Kahn traversal with a dense pending-input counter per
operation. It counts each operand use whose value has a producer, then
decrements once per matching result-user entry. Repeated inputs and multiple
results from one producer retain their full use multiplicity. The ready queue
also stores the final order, so the traversal visits each operation and edge
once without rescanning a high-fanin consumer's complete operand list.

The pass does not remove dead operations, run `simplify`, or reclaim
SemanticTree rows. A following explicit `dead-code-elim` owns that liveness
boundary; this keeps constant propagation independently measurable and lets
callers choose declaration retention explicitly. It requires an unpartitioned
ordinary GRH graph and accepts only `-top=<graph>` when a design has more than
one top graph.

## alias-analysis

`alias-analysis` redirects type-identical scalar `assign` and full static-slice
views to their source values. It updates users and source-node roots while
leaving the alias rows for the caller's explicit DCE boundary. Arrays, shaped
or type-incompatible views, and effectful operations remain unchanged.

## pattern-detect

`pattern-detect` recognizes the safe ordinary-GRH pattern
`(concat(high, low) == constant)` and rewrites it to two narrower equality
tests joined by `and`. The rewritten result and generated comparisons retain
source and SemanticTree metadata; dead concat rows are reclaimed only by the
following explicit DCE.

## common-expr

`common-expr` shares structurally identical pure scalar expression trees. It
uses the GSIM thresholds (at least five copies, a width above 256 bits, or a
duplicate with multiple distinct consumer operations), rewires duplicate users to one representative,
and leaves duplicate rows for the final explicit DCE. Effectful, port-bound,
aggregate and constant producers are excluded from this pass. Declared symbols
do not become liveness roots; they can still participate in a proven pure
expression share.

Expression signatures contain only the local operation kind, full result type,
exact non-provenance attributes and ordered canonical IDs of the operands.
Interning compares the complete signature after hashing; a hash collision cannot
establish expression equality. An explicit postorder stack computes these IDs,
so shared DAGs do not expand into repeated expression strings and deep chains do
not consume the C++ call stack. Floating attribute payloads retain their exact
bits. Sharing unions operation/value SemanticIds and origin/node metadata before
redirecting users; provenance remains outside the functional signature.
Consumer fanout is captured before any replacements; repeated operand uses by
one operation count as one consumer, matching GSIM's distinct downstream nodes.

Bit-level narrowing and structural splitting are performed by the ordinary-GRH
`used-bits` and `split-nodes` passes before conversion. No converted-graph
bit-register packing pass is part of this pipeline.

## Region extraction and state sinks

`grhsim-split-event-and-output` requires `converted`. It computes backward
demands from detectors, output/inout bindings, monitor/strobe and ordinary
effects. Exclusive pure computations/reads move; computations needed by more
than one region get one copy per destination. State declarations remain shared,
including dynamic-address array reads that still refer to their original state.
The result is `event-output-split`, with owners `[general]`, `[event]`, `[output]`.

Detectors and histories are not cloned. General effects have explicit causal
`grhsim.effectOrder`; pure producers appended by rewrites cannot reorder
independent calls. Cross-region call/random results use a held
`grhsim.sampleBoundary`, retaining the original effect/sample identity. Graph
metadata `grhsim.effectPolicy=single-original-call` records this choice. This
explicitly differs from the frozen legacy DPI fixture that invokes the same
call again in Output; it is not evidence of legacy C++ call-count equivalence.

Event monitor/strobe tasks receive `grhsim.timeslot=event`, requiring sticky
trigger accumulation throughout an eval. Free tasks receive `timeslot=change`:
typed zero-initialized history registers, comparisons over callCond/arguments,
and `grhsim.history-write` operations in Output. All history writes commit
together after Output and outside the General convergence loop.

`grhsim-split-nonsink-and-sink` requires `event-output-split`. Logic states with
`ceil(totalWidth/8) >= mem-min-bytes` use large storage; remaining states and
Output histories use small storage. Shape does not decide the policy. General
writers acquire `[general,small-sink]` or `[general,large-sink]`. A pure parameter
cone moves with a writer only when every use belongs to that one writer;
cross-writer computations and effects stay `[general,nonsink]`. Large writer
parameters are sampled in General, then committed later. No schedule, physical
layout or persistent membership tree is published by either split.

## Activity partitioning

`grhsim-partition-nonsink` requires `nonsink-sink-split`. It first builds
execution nodes, then partitions their Node DAG into supernodes. Each nonempty
node contributes exactly one to `max-supernode-nodes`, regardless of its op
count, result width or diagnostic work. Node identities survive supernode
merging. No op/work capacity splits a node.

### Execution node construction

With `firrtl-nodes=prefer|auto`, SemanticTree associations determine hinted
regions. Field and Element IDs normalize to their nearest Declaration or
Expression ancestor; independent flattened instances retain different IDs.
An operation's direct associations take precedence, with result associations
as a fallback. The complete source expression forms one node, rather than one
node per primitive operation. Legacy schema-v2 member associations remain a
fallback for checkpoints without SemanticTree coverage. Their identity includes
source unit, circuit, module, instance path and node name. Bundle field paths
normalize to their source family; arrays
retain shape. Roots are not required and origins never manufacture members.
Identical membership sets form one execution node. A shared CSE operation with
multiple families receives one shared owner, distinct from the private
regions of those families. Overlapping sets do not authorize transitive union
or reconstruction by cloning.

The provisional quotient includes actual data and ordered-effect arcs. Cyclic
components identify conflicting/nonconvex groups; only nonsingleton groups in
those components lose grouping and fall back to MFFC. Unaffected hints remain
usable. State reads and effectful operations stay explicit boundary nodes.

Remaining operations form maximal fanout-free cones, traversing reverse
operation topology. An upstream pure operation joins a cone only when all
its real consumers are already in that same unhinted cone. Fanout that
reconverges within one cone is allowed; consumers outside it, graph outputs,
hinted nodes and state/effect boundaries prevent absorption. There is no
op-count or work cutoff. `semantic-nodes=1` additionally retains declaration
result anchors as MFFC boundaries, including declarations represented only by
SemanticIds. Internal operation provenance alone is not a declaration-result
anchor. `firrtl-nodes=off` uses MFFC for all ordinary
logic while preserving source attrs.

Simplify/CSE before partitioning remain annotation-transparent. No source
root is kept alive or recreated. Owners become
`[general,nonsink,sn_<key>,node_<key>]`; keys retain literal attribute text
across rename/reindexing. Actual node and supernode quotients must be acyclic.
Subsequent simplify operates within these actual execution owners.

### Supernode construction

The upper partitioner receives only Node DAG edges and unit node counts.
Legacy mode coarsens chains/inputs/siblings with an acyclic quotient, then
uses dynamic programming over topological intervals. Bytes mode skips greedy
coarsening. Both minimize distinct incoming boundary values plus a per-group
penalty: `sum(rate * payload) + segment-penalty`. Payload is 1 in legacy
mode, or actual boundary bytes in bytes mode. A producer's nonnegative integer
`grhsim.activity` supplies its rate; absent/zero uses 1. Raw inputs and Event
pulses participate. Cost overflow is diagnosed before mutation. Op work is
never read by the upper partitioner and firing-times-work is not part of its
objective. Neither heuristic promises a globally optimal partition.

Greedy ties use descending link weight, then ascending source/target pair.
Equal predecessor lists merge in node order, advancing the anchor when a
merge exceeds capacity; member order and topological ready-queue order stay
deterministic. Sorted quotient edges supply link counts directly, while
content hashes only look up predecessor groups and never choose merge order.
`WOLVRIX_SIMPLIFY_PHASE_TIMING` reports quotient, candidate, merge and reorder
time for each coarsening attempt.

A 180-op FIRRTL node, a 180-op MFFC node and a one-op node each cost one unit
of capacity. Capacity K can hold K whole nodes; a K+1th node goes into another
supernode. Compiler function/file limits remain independent emitter concerns.

### Optional cloning and configuration

One bounded optional clone round considers cheap scalar TwoState transforms
whose varying input already reaches the destination execution group. Eligible
not/assign and constant xor/add/sub operations, and optionally eq/ne, copy into
an existing destination node. Supporting constants follow. Cloning adds ops,
not node count; it neither reconstructs FIRRTL expressions nor exceeds the
separate clone-count/fanout limits. Actual dependency/schedule verification
still applies. Cross-owner simplify cannot undo execution ownership.

`grhsim.nonsinkConfig` is
`v4/maxSupernodeNodes/segmentPenalty/semanticNodes/cloneShared/cloneMaxFanout/maxClones/cloneLossyCompare/firrtlNodes/costModel/semantic-tree-mffc-v1`.
Old work limits, leaf fusion and pre-v4 completed checkpoints require
an explicit restart at nonsink-sink-split. Thresholds are never converted from
op/work units to node counts. Schema 1 source checkpoints require re-ingest.
Python uses `max_supernode_nodes`; Make exposes
`XS_GRHSIM_MAX_SUPERNODE_NODES` and
`GRHSIM_REEMIT_MAX_SUPERNODE_NODES`. Defaults are auto/legacy/128 nodes: use
available hints, otherwise MFFC. Explicit off remains an ablation option.

`grhsim.nonsinkHintStats` and the optional text report are a snapshot after
partition/clone and before later simplify. They separately report source
coverage, FIRRTL/shared/MFFC/boundary node counts, cyclic conflict fallback,
node counts per supernode, actual boundary/consumer bytes and optional clone
counts. Work totals/maxima are diagnostic only. Boundary census includes
sink/event/output users. Replay can re-emit the same snapshot; it never
reinterprets it as the later physical layout. `hint-analysis-us` covers node
analysis, partitioning and reporting, rather than an isolated indexing timer.

The post-clone report also records `node-data-edges` and
`supernode-data-edges`: distinct directed producer/consumer pairs within the
nonsink DAG. Repeated operands and multiple values between the same pair count
once. `*-dependency-edges` additionally include ordered-effect arcs.
`nonsink-internal-boundary-value-targets` counts distinct `(value, target
supernode)` pairs crossing nonsink supernodes; the external counterpart counts
inputs and values produced outside nonsink. Their sum is
`nonsink-boundary-value-targets`. These are structural partition snapshots,
including constant boundaries, before emitter elisions and activation policy.
They must not be substituted for final runtime change-detection edge counts.

## Sink groups and SinkSchedule

`grhsim-partition-sink` requires `nonsink-partitioned`. It refines small/large
sinks by canonical clock/fire signature, per-state priority rank and eligible
shared enables. Private parameter cones follow their writer. Owners become
`[general,small-sink,group_<key>]` or the large-sink equivalent.

The pass publishes the first cross-pass artifact in the existing SessionStore:
`transform/grhsim/sink-schedule.hpp`. `SinkSchedule` has a DesignStamp, sink
configuration hash, completeness flag, `sinkGroups`, `generalRuns` and
`largeCommitRuns`. Groups contain their PartitionKey, fire (empty for latch)
and hoisted guards. Each run contains its PartitionKey, ordered original writer
IDs and `directNonsinkRoots`. General prerequisites include transitive data and
effect-order dependencies; large commit runs consume the snapshots already
sampled in General and have no prerequisite roots.

The schedule stores the prerequisite graph once, rather than copying a full
ancestor closure into every interleaved writer run:

- `nonsinkKeys[i]` is the unique logical leaf PartitionKey at topological ordinal
  `i`. It retains all four owner components, including the enclosing supernode
  and leaf; it is not a GRH OperationId or an emitter execution index.
- `nonsinkPrerequisiteOffsets` has `nonsinkKeys.size() + 1` entries, starts at zero
  and ends at `nonsinkPrerequisites.size()`. Its half-open range `[offsets[i],
  offsets[i+1])` selects leaf `i`'s immediate prerequisites in the CSR edge array.
- Each `nonsinkPrerequisites[e]` is a leaf ordinal strictly smaller than its
  target row `i`. Data dependencies and explicit effect-order dependencies both
  contribute edges.
- Each run's `directNonsinkRoots` is a sorted unique list of the first nonsink
  leaves reached from that run's writers through their sink-local parameter
  cones. It is the input to closure expansion, not a list claiming to contain
  every required leaf.

For example, keys `[A,B,C]`, offsets `[0,0,1,2]`, and prerequisites `[0,1]` encode
`B requires A` and `C requires B`. A run with roots `[2]` requires the full ordered
closure `[0,1,2]`. `requiredNonsinkOrdinals(schedule, run)` returns that complete
closure in topological leaf order; `requiredNonsinkKeys(schedule, run)` resolves
the same ordinals to copied keys. Queries expand one run at a time. The stored
artifact uses space proportional to the shared DAG and direct roots, even when
many runs share the same long ancestor chain. The readable dump prints this
same key dictionary, immediate prerequisite rows and per-run roots, with the
closure rule stated in the dump; it does not repeat every transitive key for
every run. Schedule verification reconstructs and compares the complete shared
DAG and canonical roots as well as groups/writer order, rejecting forged edges
or roots without materializing all run closures.

Explicit `writerOrder` values are preserved across clock groups: A(clock0),
B(clock1), C(clock0) may require the clock0 group to recur. Groups may not mix
clocks, priority ranks or hoisted/local guard policies. The schedule holds no
duplicate writer operands, Output history plan, nonsink execution operations,
physical layout or chunks. `writerOrder` must be unique within each target state
write group; ops order, symbol spelling and rename/freeze never decide the winning write.

Keep the same externally owned session through pass and emit. Consumers call
`requireSinkSchedule(session, graph)`, which verifies slot type, instance/top/
revision/config and the schedule against current ownership/order/dependencies.
Missing, forged or stale artifacts are rejected. Publication is atomic after
final graph verification; a failed sink pass removes the previous slot for that
design/top. Clearing the session is recoverable by rerunning sink partitioning.

## Checkpoints and reruns

Each completed stage is unified GRH JSON. Owner paths and semantic/configuration
attributes persist; SinkSchedule and emitter data do not. Store/load and clone
preserve graph facts but get a new instance identity. Freeze/reindex, rename,
symbol transfer and any graph mutation invalidate resolved schedules.

Repeating a completed pass with the same configuration verifies and leaves the
graph unchanged; sink partitioning rebuilds its artifact. A changed state-size
threshold requires the Event/Output checkpoint, changed activity options require
the General-split checkpoint, and changed sink options require the nonsink
checkpoint. Failed partially mutated stages are marked invalid and must restart
from a completed checkpoint. Dry runs use a cloned design and isolated session.

## Validation

Run `make test_grhsim_transforms` for foundation, dialect, conversion and
execution-oracle suites. `make test_grhsim_virtual_partitions` or
`make test_grhsim_sink_schedule` adds both R4 suites; `make test_wolvrix` runs
the public regression.
The oracle compares logical outputs, declared-state projections and ordered
task/DPI/random effects over multiple evals and reinitializations. It compares
the sequence of projected state changes while permitting unchanged internal
rounds to disappear after dead-state elimination. R4's candidate
interpreter consumes the actual owners, leaf dependencies and SinkSchedule,
while the reference remains unpartitioned. FourState/Real/String history tests
verify IR/checkpoint contracts; this test interpreter executes the TwoState
Logic subset. Generated C++ ABI, activation and execution are separately
tested by `make test_grhsim_cpu_st_cpp`, frozen observations, HDLBits,
ROB and XS. The [implementation overview](../../../pdocs/grhsim.md)
describes the current execution model and validation boundaries.

The [implementation overview](../../../pdocs/grhsim.md)
describes ownership, sink-order and emitter boundaries. Chunking is
exclusively an emitter function-size option; it is never a virtual partition or
a parameter to these transforms.
