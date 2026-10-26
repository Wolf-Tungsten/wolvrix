> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../../grhsim.md).

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
   the fixed order `Event`, `General`, `LargeSink`, `Output`.
2. Fills the flat branches. `Event` holds every Event-phase op in cone
   topological order (Kahn, smallest op id first) with all
   `core.event.edgeDet` detectors trailing, so the cone is evaluated before
   edges are detected. `LargeSink` holds the large-sink write ops in op-id order
   (the static priority seed; C5's mem write plan finalizes priorities).
   `Output` holds the Output-phase ops in topological order.
3. Forms `Node` partitions under the General branch — over the **nonSink**
   General-phase ops only (V2-M1). The General-phase ops split by shape:
   - **SmallSink ops** (no results: `core.state.write` / `writeMask` / `latchWrite`, the
     General-phase `core.state.writeArray` / `writeArrayMask` / `fill` (and
     `write`/`writeMask` on arrays)
     on smallSink-class arrays, and no-result
     `core.system.task` / `core.dpi.call`) never enter node formation. Each
     anchors a singleton `Node` appended in op-id order after the nonSink
     nodes, and C2 clusters them into smallSink supernodes by priority and event signature.
   - **NonSink ops** (value-producing, event-carrying ones included) go
     through the reverse-topological cone absorption: `core.compute.*`,
     `cpu.compute.maskedOr`,
     `core.input.read`, `core.state.read` and `core.state.readArray` absorb
     into their result's only user node; shared values (used by several
     nodes) and ops whose users include ops outside the nonSink set (a smallSink
     op, or a write op in the LargeSink branch) block absorption and anchor their
     own node, becoming boundary value candidates. SmallSink operand cones stay in
     this nonSink frame — their values become enable/payload boundary values sampled by the
     smallSink supernodes.
   - The capacity knob is `--max-op-in-compute-node` (default 128).
     Capacity uses estimated primitive work: `maskedOr` with N elements costs
     2N-1, other ops cost 1. C2 uses the same weights for merge capacity.
   - `--semantic-nodes` (default 0, S1 exploration): every value targeted by
     a `DeclProvenance` Value slice (a source-level declared signal) anchors
     its own node — its producer op is never absorbed downstream — and the
     128-op node size cap is dropped, so node shape follows declaration
     boundaries instead of dataflow shape. Unanchored ops keep the legacy
     cone absorption above, so values outside provenance coverage fall back
     to the old rule. Diagnostics then also report `anchored_values` and
     `semantic_nodes`.

For example, with `w = write(cond=%c, next=%n, mask=%one)` where `%c` and
`%n` are single-use:

```text
op3 core.compute.and  -> %c     ; feeds only w (a sink)
op4 core.compute.not  -> %n     ; feeds only w
op5 core.compute.and  -> %mw_c  ; feeds a LargeSink-branch writeArray
op6 core.state.writeMask [%c, %n, %one] {@q} { event_acts: [0] }
```

becomes four nodes (every cone op's only user is outside the non-sink set,
so nothing absorbs; the write anchors a singleton sink node):

```text
node { op3 }   ; cone op — user is a sink, absorption blocked
node { op4 }   ; cone op — user is a sink, absorption blocked
node { op5 }   ; cone op — user sits in the LargeSink branch, absorption blocked
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
