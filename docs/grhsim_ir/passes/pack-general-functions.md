# General function packing (helper chunks and emit functions, C6)

`cpu.st.pack-general-functions` is the sixth pass (C6) of the CPU mapping
C segment. It runs **after** `cpu.st.build-mem-write-plan` (C5) and **before**
`cpu.st.build-phase-schedule` (C7): the pipeline order is deliberately not
the `CpuMappingStage` numeric order — `GeneralFunctions` (numeric 11) sits
between `MemWritePlan` (14) and `PhaseSchedule` (15) in rank terms (see
`cpuMappingStageRank` in `include/grhsim/ir/model.hpp`). Requires a
`MemWritePlan`-stage mapping; produces the `GeneralFunctions` stage.

Two steps:

1. Helper chunks: every General supernode gets `helperChunks` ranges split by
   the legacy `estimatedLines` heuristic against
   `--helper-max-estimated-lines` (default 2048). Ranges index into the
   supernode's flattened op order and must cover it contiguously. The
   six-phase model gates whole supernodes through the ActiveFlags byte
   arrays; the partition tree never contains `ActiveWord` (or `EventDomain`)
   partitions.
2. Emit functions: the General branch's supernodes are **not** remounted
   (resolution 2 — supernode ordinals are decoupled from function packing).
   Each `EmitFunction` is a trailing leaf appended after the supernodes and
   only records the contiguous supernode ordinal interval it holds in
   `attrs.supernodeRange = {offset, count}`; the intervals tile `[0, N)` in
   C2 order. Batching uses the legacy thresholds `--batch-max-ops` (2048),
   `--batch-max-estimated-lines` (8192) and `--target-batch-count` (64; with
   a nonzero target the effective limits are `max(limit, total/target)` over
   the General branch). The flat `Event`, `Mem` and `Output` branches each
   collapse into exactly one `EmitFunction` leaf holding the branch's ops —
   one task per branch — even when the branch is empty.

The resulting tree shape:

```text
root
├─ phase Event   └─ EmitFunction { flat event ops }      ; cone, then edgeDets
├─ phase General ├─ Supernode+ └─ Node+                  ; direct children, C2 order
│                └─ EmitFunction+ { supernodeRange }     ; trailing leaves, tile [0,N)
├─ phase Mem     └─ EmitFunction { mem writes }          ; op-id order
└─ phase Output  └─ EmitFunction { flat output ops }
```

The `eventActs` annotations produced by
`cpu.st.merge-general-supernodes` stay on the supernodes. Diagnostics: the
standard `partitions` count only.
