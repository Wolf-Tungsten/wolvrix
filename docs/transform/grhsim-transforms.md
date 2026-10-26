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
| `grhsim-split-event-and-output` | `-top` | the unique top |
| `grhsim-split-nonsink-and-sink` | `-top`, `-mem-min-bytes` | the unique top, 64 |
| `grhsim-partition-nonsink` | `-top`, `-max-node-work`, `-max-supernode-work`, `-segment-penalty`, `-semantic-nodes` | the unique top, 128, 128, 1, 0 |
| `grhsim-partition-nonsink` clone options | `-clone-shared`, `-clone-max-fanout`, `-max-clones`, `-clone-lossy-compare` | 1, 8, 250000, 1 |
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

`grhsim-partition-nonsink` requires `nonsink-sink-split`. It forms bounded
work nodes, coarsens chains/inputs/siblings only with an acyclic quotient, and
uses dynamic programming over topological segments to minimize distinct incoming
activation values weighted by rate plus `segment-penalty`. Raw inputs and Event
pulses participate in boundary costs. A producer's nonnegative integer
`grhsim.activity` supplies its weight; absent/zero rates use uniform weight 1.
Cost overflow is diagnosed before graph mutation.

Work is at least one per op and grows with Logic result words; mul/div/mod cost
four times that, external function/DPI calls eight times. A single indivisible
node may exceed a budget. `semantic-nodes=1` keeps declaration anchors from
being absorbed. These work limits control activation granularity, not source
lines. Owners become `[general,nonsink,sn_<key>,node_<key>]`; the initial key is
derived from an anchor symbol, then remains literal attribute text across rename
or ID reindexing. Leaf and supernode quotients must remain acyclic, including
effect-order arcs.

One bounded optional clone round considers cheap scalar TwoState operations
whose varying input is already a boundary, state read or graph input. It copies
eligible not/assign and constant xor/add/sub operations, and optionally lossy
eq/ne comparisons, into consuming nonsink leaves. Clone count limits primary
copies; supporting constants are copied to the destination too. Clones keep
types/sources and receive new symbols. `grhsim-simplify` can then simplify each
complete leaf separately; it cannot merge clones across owners.

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
ROB and XS. The [capability index](../../../pdocs/remove-grhsim-ir/capability-evidence.md)
keeps those evidence boundaries explicit.

The [implementation plan](../../../pdocs/remove-grhsim-ir/passes-and-emitter.md)
defines ownership, sink-order and emitter boundaries. Chunking is
exclusively an emitter function-size option; it is never a virtual partition or
a parameter to these transforms.
