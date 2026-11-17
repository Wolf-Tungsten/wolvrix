# Shared compute regions

`ActivityScheduleOptions::eventRegionsShared`, CLI `-event-regions-shared true`,
and Python `event_regions_shared=True` enable an experimental, default-off
extension of [event regions](activity-event-regions.md). It requires
`eventRegions` to be enabled explicitly. The first pass still forms the same
single-register-terminal regions; this extension then handles shared pure logic.

For a static admission check, also enable
`ActivityScheduleOptions::eventRegionsSharedCostGuard`, CLI
`-event-regions-shared-cost-guard true`, or Python
`event_regions_shared_cost_guard=True`. This guard is default-off and requires
`eventRegionsShared`. It snapshots the schedule immediately before the shared
pass, then compares the rebuilt candidate on four structural maintenance
dimensions: compute group count, boundary value count, boundary activation edge
count, and state-read activation edge count. The candidate is accepted only when
every dimension is no larger and at least one is strictly smaller. Otherwise the
complete pre-shared schedule, including its commit order and derived data, is
restored. The decision uses only the static schedule and emits before/after
metrics; it does not read runtime traces, profiles, or workload measurements.
The guard counts state-read activation edges as the sum of the distinct group
sets in `stateReadSupernodes`. This differs from the older schedule-summary
field with the same name, which counts fanout of state-read result values.
The existing shared-region summary describes the constructed candidate even
when it is rejected; use the guard decision and final schedule summary to
identify the retained schedule.

## Local cycle refinement

`eventRegionsSharedCycleRefinement` (CLI `-event-regions-shared-cycle-refinement`,
Python `event_regions_shared_cycle_refinement`) is a separate default-off option,
requiring shared regions. It selects the same endpoint classes and operation
limits as the original pass. Unmoved nodes initially retain their original group.
An iterative SCC walk checks the quotient graph; only original remainders inside
nontrivial SCCs are partitioned by exact endpoint set. Acyclic descendants of an
SCC are retained. If no further refinement is possible, only merged classes in
cyclic SCCs are withdrawn, and the partition is rebuilt from the original groups.
Each iteration either refines a mixed remainder or permanently withdraws a class.

The final schedule still undergoes full operation coverage, dependency, topology,
and commit-order reconstruction. Effects, state snapshots and complete compute
nodes obey the existing rules. No model names, execution counts or runtime
profiles affect decisions. The original global refinement remains unchanged
when this option is off. This is independent of the experimental cost guard;
comparison runs leave that guard off.

## Endpoints and hard boundaries

Each complete compute node receives the sorted set of endpoints reachable through
pure combinational users. Endpoints include commit data/enable/address consumers,
public output/inout values, and protected compute groups. No terminal is used to
clock-qualify execution: a region runs when its inputs or read state change, even
if no clock edge occurs.

Memory/latch operations, event producers, non-Logic results, side effects and
reg-to-mem intent nodes are hard boundaries. The entire original compute group
containing a hard boundary is retained verbatim. Its input logic may still be
shared: it sees a unique endpoint representing that group. Propagating a barrier
as a ban on every upstream node would exclude virtually the whole combinational
fanin of a memory address, even when the memory access itself does not move.

## Region construction

1. Traverse the compute-node DAG backwards, accumulating endpoint sets. There is
   a fixed limit of 32 endpoints per node; overflowing sets fall back rather than
   selecting an incomplete subset of endpoints.
2. Collect complete pure nodes with the same set of **multiple** endpoints. Merge
   a class only when it spans at least two original groups and has at most 8,192
   operations. Neither bound is selected from runtime measurements.
3. Split any remaining mixed pure group into fragments with one endpoint set
   each. Leaving mixed fragments can introduce a cycle in the contracted group
   DAG, even though the original operation DAG was acyclic.
4. Order by first use of external results, as in the existing terminal-region
   pass. Unchanged groups retain their exact operation order;
   fragments retain their original relative order.
5. Rebuild the group DAG, value fanout and state-read activation. Verify the exact
   scheduled operation multiset. Copy commit groups and their order unchanged.

Along a path of unprotected nodes, endpoint sets can only shrink. A path between
two nodes with the same set therefore stays within that class. A protected group
is a unique endpoint on its upstream side and cannot be crossed and then revisited
in the original group DAG. Contracting classes and refining mixed remainders
preserves acyclicity; the final strict rebuild also checks it explicitly.

If no class qualifies, the schedule is returned unchanged. Regions above the
operation bound are not partially selected. All compute still runs before commit,
so register and memory next-state expressions read the pre-commit snapshot.
Memory reads retain the existing memory-write reactivation behavior. The graph,
IR semantics, input change detection and fixed-point loop are unchanged.

## Validation and performance limits

`emit-grhsim-event-regions` includes shared chains feeding public outputs and two
clock domains, transparent clock gates, asynchronous reset, changing inputs while
clocks are held, reciprocal register dependencies, and masked memory writes with
old-memory-dependent register updates. Generated off/on models are checked using
an independent oracle at O0 and O3 with ASan/UBSan. Schedule checks cover protected
group membership, oversized classes, endpoint-set overflow, unsupported node
splitting and option validation. The original scheduler suite remains applicable.

Fewer boundaries do not guarantee faster simulation. Shared regions can execute
more often, retain more live temporaries, and make an existing stored-predicate
decoder ineligible. Measure complete workloads and actual generated machine code;
do not infer savings from removed change checks alone. The separate experimental
local-equality cache is not automatically enabled by this transformation.

The IR def-to-last-use distance is diagnostic only. A CV32E40P experiment that
rejected first-use ordering when this sum increased reduced the sum, but worsened
generated stack traffic and complete-workload walltime. That guard was withdrawn;
the metric is not a valid performance admission rule. Coarser activation
conditions and decoder-cache eligibility also require separate treatment.

The mechanism is independent of CV32E40P. XiangShan may have similar shared control
cones, but partition communication, memory intents, effects, larger endpoint sets
and long live ranges constrain applicability. This option has no measured
XiangShan benefit at present.
# Connected candidates and exact activation frontiers

`eventRegionsSharedConnected` (`-event-regions-shared-connected`, Python
`event_regions_shared_connected`) is an experimental, default-off alternative
to pooling an entire equal-terminal class. It requires `eventRegionsShared`.

The rule keeps whole compute nodes and all existing effect, event, intent,
terminal-count and region-size protections. Within each terminal class it joins
nodes through actual computational def-use edges, excluding constant and
register-read edges as evidence of shared computation. Read-only nodes can
follow a component when all outgoing edges belong to that component.

Disconnected components may share an entry only if their complete static
activation frontiers are equal: the exact external nonconstant ValueIds and
direct register state symbols. Equal terminal sets or partial overlap of state
sources are insufficient. These keys are computed before schedule rebuilding;
there are no runtime counts, model names or profile inputs. Initialization and
compute-before-commit snapshot semantics remain unchanged.

A candidate must contain computational nodes from multiple original groups
and either internalize a previously crossing computational edge or merge equal
activation frontiers. Attaching source reads alone does not satisfy that rule.
The existing 8192-operation bound is unchanged. Unselected nodes retain their
original groups. The local quotient-SCC refinement is used automatically;
only cyclic remainders are split, with unresolved cyclic classes withdrawn.

The diagnostic `shared connected components` reports initial work components,
equal-frontier joins, components lacking a contraction, and retained classes.
These are structural counts, not predicted time savings. In particular, state
view eligibility and cache ownership can depend on the resulting read frontier
and must be checked on each generated model. The default-off path preserves
the prior schedule and generated source.
