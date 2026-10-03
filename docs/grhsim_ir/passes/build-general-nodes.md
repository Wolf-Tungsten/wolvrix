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
3. Forms `Node` partitions under the General branch — over the **non-sink**
   General-phase ops only (V2-M1). The General-phase ops split by shape:
   - **Sink ops** (no results: `core.state.regWrite` / `latchWrite`, the
     General-phase `core.state.memWrite` / `memFill` / `memAssign` /
     `memWriteSeq` on regLatch-class arrays, and no-result
     `core.system.task` / `core.dpi.call`) never enter node formation. Each
     anchors a singleton `Node` appended in op-id order after the non-sink
     nodes, and C2 clusters them into sink supernodes by event signature.
   - **Non-sink ops** (value-producing, event-carrying ones included) go
     through the reverse-topological cone absorption: `core.compute.*`,
     `core.input.read`, `core.state.read` and `core.state.memRead` absorb
     into their result's only user node; shared values (used by several
     nodes) and ops whose users include ops outside the non-sink set (a sink
     op, or a mem write in the Mem branch) block absorption and anchor their
     own node, becoming boundary value candidates. Sink operand cones stay in
     this non-sink frame — their values become boundary values sampled by the
     sink supernodes.
   - The capacity knob is `--max-op-in-compute-node` (default 128).

For example, with `w = regWrite(cond=%c, next=%n, mask=%one)` where `%c` and
`%n` are single-use:

```text
op3 core.compute.and  -> %c     ; feeds only w (a sink)
op4 core.compute.not  -> %n     ; feeds only w
op5 core.compute.and  -> %mw_c  ; feeds a Mem-branch memWrite
op6 core.state.regWrite [%c, %n, %one] {@q} { event_acts: [0] }
```

becomes four nodes (every cone op's only user is outside the non-sink set,
so nothing absorbs; the write anchors a singleton sink node):

```text
node { op3 }   ; cone op — user is a sink, absorption blocked
node { op4 }   ; cone op — user is a sink, absorption blocked
node { op5 }   ; cone op — user sits in the Mem branch, absorption blocked
node { op6 }   ; singleton sink node
```

Diagnostics: `event_ops`, `general_nodes`, `sink_nodes`, `mem_ops`,
`output_ops`, `boundary_value_targets` (cross-node value edges).

The semantic boundary-prediction helper `predictGeneralBoundaries` was
removed in V3-M3: its node-level prediction systematically over-counted the
boundaries that survive C2's supernode merge. Boundary-aware cloning now
consumes the real supernode boundaries after C2
(`cpu.st.clone-shared-boundaries`, see
[clone-shared-boundaries](clone-shared-boundaries.md)).
