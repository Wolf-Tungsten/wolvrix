# General-branch node formation

`cpu.st.build-general-nodes` is the second pass of the M3 six-phase CPU
mapping pipeline. It partitions the General-phase ops into `Node` partitions
under the General branch — the same reverse-topological cone absorption as the
legacy `cpu.st.build-compute-nodes`, extended so state writes anchor nodes
instead of forming a separate commit phase. Requires a `SplitPhases`-stage
mapping; produces the `GeneralNodes` stage.

Differences from the legacy pass:

1. The input is the model's General-phase op list (the General branch is an
   empty shell after `cpu.st.split-phases`), not the ops of a compute phase
   partition.
2. `core.state.regWrite` / `core.state.latchWrite` and the General-phase
   `core.system.task` / `core.dpi.call` are mergeable sinks: they anchor nodes
   (they are never absorbed — a write has no results) while their
   single-consumer operand cones absorb into the sink's node. This is the
   P_general "write ops fold into supernodes" boundary-reduction motive.
3. `core.compute.*`, `core.input.read`, `core.state.read` and
   `core.state.memRead` follow the legacy absorption rule: an op absorbs into
   its result's only user node; shared values (used by several nodes) and ops
   whose users include ops outside the General set (e.g. a mem write in the
   Mem branch) block absorption and anchor their own node, becoming boundary
   value candidates.
4. The capacity knob is unchanged: `--max-op-in-compute-node` (default 128).

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

Diagnostics: `general_nodes`, `boundary_value_targets` (cross-node value
edges), plus the standard `partitions` count from the pass wrapper.
