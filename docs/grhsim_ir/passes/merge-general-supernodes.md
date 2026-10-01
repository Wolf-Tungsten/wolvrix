# General supernode merging with the event-domain constraint

`cpu.st.merge-general-supernodes` is the second pass (C2) of the CPU mapping
C segment. It merges the General-branch nodes into supernodes with the
legacy frame — cluster initialization, the op DAG, three coarsen modes over a
union-find, the quotient DAG check, and the DP segmentation cost model
(`--max-op-in-compute-supernode`, default 128) — and adds the P_general
event-domain merge prohibition. Requires a `GeneralNodes`-stage mapping;
produces the `GeneralSupernodes` stage.

**Supernode ordinals are fixed here (resolution 2).** The resulting
supernodes stay direct children of the General branch in cluster
(partition-result) order, and that child order **is** the final supernode
ordinal space (0..N-1) consumed by the layout, event bitmaps, mem write
plan, schedule and emitter. Later passes never renumber or remount the
supernodes: function packing (C6) only records contiguous ordinal intervals
over them.

Event-domain definitions:

- `A(op)`: the op's `event_acts` cluster indices, sorted and deduplicated;
  empty for event-free ops.
- The influence graph spans the General-phase ops plus the Mem-phase write
  ops. Edges are value fanout only (the General→Mem write-operand edges
  included, with the mem write ops as sinks). State write→read edges are
  **excluded**: state readers consume `S` (regLatchStore/memStore) directly
  and their data activation is driven by P_publish's stateFanout, not by
  eventActiveFlag — a write's domain must not leak into its readers' domains,
  or a legal two-domain design (domain {0} writes Q, domain {1} reads Q)
  would put `{0,1} ≠ {0}` on the writer's supernode and fail rule 2.
  Event- and Output-phase ops are outside the graph: their cones are
  self-contained, and the Output timeslot tasks' acts do not gate supernodes.
- `E(op)`: the union of `A(·)` over every event-carrying op reachable from
  the op in the influence graph, its own acts included. Computed once per
  model as a worklist least fixed point (`computeCpuEventDomainSets`) and
  aggregated per node, then per candidate cluster during merging.

The merge prohibition: a merged supernode that **contains an event-carrying
op** must satisfy

1. rule 1 — all its event-carrying ops share the same act set `K`;
2. rule 2 — its downstream closure introduces no other acts: the union of
   `E(·)` over all its ops (combinational ones included) equals `K`;
3. rule 3 — no member op may be event-obligation-free (`A(op)=∅` and
   `E(op)=∅`): such an op (an event-free system task/DPI sink, a
   level-sensitive read chain, an input read feeding only event-free
   consumers) is data-driven by nature, and the domain gate would suppress
   its execution when data changes without an edge. Event-free cone members
   with `E(·)=K` belong to the domain and merge freely.

Violating any rule forbids the merge (coarsen candidates and multi-cluster
DP spans alike; rejected attempts are counted in `event_domain_blocked`).
Pure combinational / latch-only clusters without event-carrying ops are
exempt — downstream asymmetry cannot strand a sticky data-activation flag
when there is no write to lose. Single nodes / single-cluster spans are never
rejected: node formation already fixed their contents, so a lone
domain-straddling node still becomes its own supernode and is flagged by the
verifier (an event-carrying supernode must satisfy all three rules) instead of
making the model unpartitionable.

Every resulting General supernode records the sorted union of its ops' acts
in the `eventActs` partition attribute — engaged, with an empty array for
event-free supernodes — serialized in the partition's positional JSON attr
tail after the helper-chunk array (`[chunks], [acts…]?, [supernodeRange]?`).

For example (two writes, one shared cone):

```text
x = not(d) feeds w1 {event_acts:[0]} and w2 {event_acts:[1]}
```

- `node{d,x}` has `A = ∅`, `E = {0,1}`; merging it with `node{one1,w1}`
  (`A = E = {0}`) breaks rule 2 — forbidden;
- merging `node{one1,w1}` with `node{one2,w2}` breaks rule 1 — forbidden;
- result: three supernodes with `eventActs` `[]`, `[0]`, `[1]`.

Diagnostics: `coarsen_iterations`, `coarsened_clusters`, `general_supernodes`,
`boundary_value_targets`, `event_domain_blocked`, plus the standard
`partitions` count.
