# Four-way phase split into the six-phase partition tree

`cpu.st.split-phases` is the first pass of the M3 six-phase CPU mapping
pipeline (`pdocs/simulation-model-refactor`). It completes the op phase
attribution that the M2 lowering passes left unfinished and builds the root of
the new partition tree with four phase branches. The legacy
`cpu.st.split-phase` (compute/commit) is untouched; both pipelines coexist
until M5 removes the old one.

The pass has three steps, in order:

1. Attribute every phase-less op. `core.state.memWrite` / `core.state.memFill` /
   `core.state.memAssign` / `core.state.memWriteSeq` become `Mem`; a phase-less
   `core.output.write` becomes `Output`; every other `None` op becomes
   `General` (pure combinational logic, `latchWrite`, reads, and the
   non-event non-timeslot `system.task` / `dpi.call`). Ops already carrying
   `Event` / `General` / `Output` keep their phase. After this step no op is
   phase-less (total phase coverage; the verifier rejects a six-phase mapping
   over a model that still has one).
2. Build the partition tree: a root with exactly four `Phase` children in the
   fixed order `Event`, `General`, `Mem`, `Output` (the new `CpuPhase` values;
   `None`/`Compute`/`Commit` remain reserved for the legacy pipeline).
3. Fill the flat branches. `Event` holds every Event-phase op in execution
   order — a Kahn topological sort of the cone ops (smallest op id first)
   with all `core.event.edgeDet` detectors trailing, so the cone is evaluated
   before edges are detected. `Mem` holds the mem writes in op-id order (the
   static priority seed; M4's mem write plan finalizes priorities). `Output`
   holds the Output-phase ops in topological order. `General` stays an empty
   shell: `cpu.st.build-general-nodes` populates it from the model scan.

The mapping stage becomes `SplitPhases` (one of the new `CpuMappingStage`
values `SplitPhases` / `GeneralNodes` / `GeneralSupernodes` /
`GeneralFunctions`). The mapping stays `complete == false`; there is no data
layout or schedule payload in the six-phase pipeline yet.

For example, after M2 a model fragment

```text
op1 core.input.read -> %clk.ev   phase=event
op2 core.event.edgeDet [%clk.ev] phase=event  { act: 0, prev: 0, ... }
op3 core.input.read -> %d        phase=none
op4 core.state.regWrite [%one, %d, %one] phase=general { event_acts: [0] }
op5 core.state.memWrite  [%one, %one, %d, %one] phase=none
```

becomes

```text
root
├─ phase Event  ops=[op1, op2]        ; cone topo order, edgeDet last
├─ phase General ops=[]               ; shell, filled by build-general-nodes
├─ phase Mem    ops=[op5]             ; op-id order
└─ phase Output ops=[]
; op3 attributed none -> general, op5 none -> mem
```

Diagnostics: `event_ops`, `general_ops`, `mem_ops`, `output_ops`,
`attributed_mem`, `attributed_output`, `attributed_general`. The pass takes no
arguments and may be rerun: it rebuilds the tree from scratch (five
partitions) and discards any downstream six-phase structure, just like
re-running `cpu.st.split-phase` resets the legacy tree. It is a
`BackendMapping` pass, so the semantic revision and the mapping provenance
stay intact.
