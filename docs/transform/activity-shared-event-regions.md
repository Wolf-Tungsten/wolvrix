# Shared compute regions

`ActivityScheduleOptions::eventRegionsShared`, CLI `-event-regions-shared true`,
and Python `event_regions_shared=True` enable an experimental, default-off
extension of [event regions](activity-event-regions.md). It requires
`eventRegions` to be enabled explicitly. The first pass still forms the same
single-register-terminal regions; this extension then handles shared pure logic.

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
