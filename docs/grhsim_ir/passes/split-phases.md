# cpu.st.split-phases — removed in M5d-6

`cpu.st.split-phases` was the first pass of the interim M3 six-phase CPU
mapping pipeline: it attributed every phase-less op and initialized the
mapping with the four-branch (Event/General/Mem/Output) partition tree. Both
duties have since moved, and the pass was deleted in M5d-6:

- Phase attribution is a semantic-layer decision, completed once by
  [`grhsim.split-phases`](grhsim-split-phases.md) (B5) before any CPU mapping
  exists and sealed by B8 (`grhsim.verify --seal semantic`).
- Mapping initialization is
  [`cpu.st.build-general-nodes`](build-general-nodes.md) (C1), which builds
  the four-branch tree and the General nodes directly from the sealed
  attribution.

The `CpuMappingStage::SplitPhases` enum value is kept solely so old
checkpoints remain decodable; no pass produces it anymore.
