# GRH-based GrhSIM transforms

These passes run through the ordinary PassManager on one flattened top Graph.
They use the [GrhSIM dialect](../grh/grhsim-dialect.md) on the same GRH design.
Conversion, unpartitioned optimization, all four split/partition passes and
the [C++ emitter](../emit/grhsim-cpu-st-cpp.md) form the production route.
The independent model, pass registry and CPU mapping have been removed.
See the [simulation workflow](../grhsim.md) for standard entries and evidence.

## Pipeline and options

Run the existing GRH frontend and ordinary `simplify` first, then:

```text
grh-to-grhsim
  → optional grhsim-simplify / grhsim-pack-bit-registers / grhsim-recover-masked-or
  → grhsim-node-cse                     (annotated FIRRTL only)
  → grhsim-split-event-and-output        (first virtual partitions)
  → grhsim-split-nonsink-and-sink
  → grhsim-partition-nonsink
  → grhsim-partition-sink                (first SinkSchedule artifact)
  → emit/grhsim/cpu-st-cpp.cpp
```

| Pass | Options | Defaults |
| --- | --- | --- |
| `grh-to-grhsim` | `-top`, `-logic-domain=2state|4state` | the unique top, 2state |
| `grhsim-simplify` | `-top`, `-max-rounds` | the unique top, 8 |
| `grhsim-pack-bit-registers` | `-top`, `-report` | the unique top, no TSV |
| `grhsim-recover-masked-or` | `-top`, `-min-terms`, `-report` | the unique top, 16, no TSV |
| `grhsim-node-cse` | `-top`, `-firrtl-node-cse=off|gsim`, `-report` | the unique top, gsim, no report |
| `grhsim-split-event-and-output` | `-top` | the unique top |
| `grhsim-split-nonsink-and-sink` | `-top`, `-mem-min-bytes` | the unique top, 64 |
| `grhsim-partition-nonsink` | `-top`, `-max-supernode-nodes`, `-segment-penalty`, `-semantic-nodes` | the unique top, 128, 1, 0 |
| `grhsim-partition-nonsink` clone options | `-clone-shared`, `-clone-max-fanout`, `-max-clones`, `-clone-lossy-compare` | 1, 8, 250000, 1 |
| `grhsim-partition-nonsink` hint/cost options | `-firrtl-nodes=off|prefer|auto`, `-cost-model=legacy|bytes`, `-report` | auto, legacy, no report |
| `grhsim-partition-sink` | `-top`, `-enable-guard-min-size` | the unique top, 8 (0 disables hoisting) |

Factory arguments use `-key=value`, as with other ordinary passes. There are no
`role` or `scope` options on the new simplify. An explicitly selected top must
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

## grhsim-simplify

This is an independent simplify driver. It shares pure operation evaluation and
type rules with GRH but does not invoke or add simulation modes to the ordinary
GRH simplify. It performs constant folding, structural/bitwise rewrites,
type-aware CSE, DCE, prefix used-bit narrowing, conservative equivalent-state
merging and eligible mux-chain folding.

On an unpartitioned graph it works globally. Equivalent detectors with the
same input/type/edge/history initialization and sample/update policy share one
result/history; effectful calls and random samples do not undergo this CSE.
On a fully attributed graph it works separately inside each complete leaf
owner. It preserves exported values/types and cannot merge clones across
owners. Shared-state narrowing/merging only runs before partitioning.
FIRRTL source associations impose no extra CSE or structural inlining scope.
`grhsim-node-cse` compares complete pure scalar expressions and applies the
GSIM count/width/fanout thresholds; see [its contract](../ingest/node-cse.md).

Full LogicalType, including shape/domain, participates in aliases and CSE.
ArrayCreate and sliceArray keep the established bit order and flattened
sliceArray index behavior. Peeling a multidimensional construction requires
proof that the remaining indices are in bounds. Register readArray keeps its
separate state-access bounds behavior; optimizations cannot interchange the two
to bypass those rules. Division/modulo by zero is not folded.

## Packing and masked-OR recovery

Packing requires the unpartitioned converted graph. It combines eligible
unsigned TwoState one-bit registers with constant initialization, full reads,
one writer each and shared fire/enable/mask controls, up to 64 bits per word.
Old state names become typed views of the packed state; declaredSymbols,
generate groups, existing read result identities and sources remain valid.
Other state/control/access forms are left unchanged.

Masked-OR recovery matches eligible mux-to-zero/AND-mask OR reductions and
static array views. It preserves each complete owner if already partitioned;
matching does not cross owners. New operations reference the original values
and carry their source annotations. S9 access optimizations are separate work.

Production bit-register packing remains enabled by default. The default-off
legacy comb-pack pass, implementation, registry entry and switches are removed.

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

With `firrtl-nodes=prefer|auto`, current FIRRTL member associations determine
hinted regions. Identity includes source unit, circuit, module, instance path
and node name. Bundle field paths normalize to their source family; arrays
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
anchors as MFFC boundaries. `firrtl-nodes=off` uses MFFC for all ordinary
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
`v3/maxSupernodeNodes/segmentPenalty/semanticNodes/cloneShared/cloneMaxFanout/maxClones/cloneLossyCompare/firrtlNodes/costModel/hint-mffc-v1`.
Old work limits, leaf fusion and v2/eight-field completed checkpoints require
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
IDs and necessary nonsink leaf prerequisites. General prerequisites include
transitive data and effect-order dependencies; large commit runs consume the
snapshots already sampled in General.

Writer order is preserved across clock groups: A(clock0), B(clock1), C(clock0)
may require the clock0 group to recur. Groups may not mix clocks, priority ranks
or hoisted/local guard policies. The schedule holds no duplicate writer operands,
Output history plan, nonsink execution graph, physical layout or chunks.
Equal explicit writerOrder values keep declaration order; symbol spelling does
not decide the winning write, and rename/freeze preserve this tie order.

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
