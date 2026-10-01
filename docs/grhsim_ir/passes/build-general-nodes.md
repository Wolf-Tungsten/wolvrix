# General-branch node formation (C1)

`cpu.st.build-general-nodes` is the first pass (C1) of the CPU mapping
C segment and the pipeline's single mapping-initialization point: it requires
**no** preceding mapping stage and builds the `CpuBackendMapping` from
scratch — rerunning it discards and rebuilds any existing mapping. The input
is a semantically sealed model (B8: total phase attribution, no `event_edges`
residue); the pass only consumes `SimOp::phase` and never rewrites the
semantics. Produces the `GeneralNodes` stage.

The pass errors out on any `SimPhase::None` op, pointing at
[`grhsim.split-phases`](grhsim-split-phases.md) (B5). It then:

1. Builds the partition-tree root with exactly four flat `Phase` children in
   the fixed order `Event`, `General`, `Mem`, `Output`.
2. Fills the flat branches. `Event` holds every Event-phase op in cone
   topological order (Kahn, smallest op id first) with all
   `core.event.edgeDet` detectors trailing, so the cone is evaluated before
   edges are detected. `Mem` holds the `Mem`-phase write ops in op-id order
   (the static priority seed; C5's mem write plan finalizes priorities).
   `Output` holds the Output-phase ops in topological order.
3. Forms `Node` partitions under the General branch over **all**
   General-phase ops — including the General-phase mem writes on
   regLatch-class arrays that B5's class-aware attribution keeps out of
   P_mem. The rule is the reverse-topological cone absorption:
   - `core.state.regWrite` / `core.state.latchWrite`, the General-phase
     `core.state.memWrite` / `memFill` / `memAssign` / `memWriteSeq`, and the
     General-phase `core.system.task` / `core.dpi.call` are mergeable sinks:
     they anchor nodes (a write has no results and is never absorbed) while
     their single-consumer operand cones absorb into the sink's node. This is
     the P_general "write ops fold into supernodes" boundary-reduction
     motive.
   - `core.compute.*`, `core.input.read`, `core.state.read` and
     `core.state.memRead` follow the absorption rule: an op absorbs into its
     result's only user node; shared values (used by several nodes) and ops
     whose users include ops outside the General set (e.g. a mem write in the
     Mem branch) block absorption and anchor their own node, becoming
     boundary value candidates.
   - The capacity knob is `--max-op-in-compute-node` (default 128).

Event-carrying ops (`event_acts` on writes / tasks / DPI calls) are always
anchors, so a node never holds more than one of them; rule 1 of the
event-domain constraint therefore holds trivially at node granularity.

For example, with `w = regWrite(cond=%c, next=%n, mask=%one)` where `%c` and
`%n` are single-use:

```text
op3 core.compute.and  -> %c     ; feeds only w
op4 core.compute.not  -> %n     ; feeds only w
op5 core.compute.and  -> %mw_c  ; feeds a Mem-branch memWrite
op6 core.state.regWrite [%c, %n, %one] {@q} { event_acts: [0] }
```

becomes two nodes (op3/op4 absorb into the write's node; op5's only user
sits outside the General set, so it anchors alone):

```text
node { op3, op4, op6 }   ; cone absorbs into the write's node
node { op5 }             ; user outside the General set blocks absorption
```

Diagnostics: `event_ops`, `general_nodes`, `mem_ops`, `output_ops`,
`boundary_value_targets` (cross-node value edges).

The semantic boundary-prediction helper `predictGeneralBoundaries`
(`include/grhsim/pass/general_boundaries.hpp`) mirrors this pass's
cone-absorption rules over the sealed model without building a mapping; the
two implementations are kept aligned, and the B7 tests
(`grhsim-split-phases-tests`) check that the predicted boundary set matches
the node boundaries this pass actually forms.
