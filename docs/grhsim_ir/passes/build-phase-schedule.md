# Phase schedule

`cpu.st.build-phase-schedule` is the seventh pass (C7) of the CPU mapping C
segment. Requires a `GeneralFunctions`-stage mapping (M5d-6 moved function
packing ahead of scheduling); produces the `PhaseSchedule` stage — a terminal
stage at which the mapping becomes `complete` (M5d-7 appends C8
[`plan-translation-units`](plan-translation-units.md) → `TranslationUnits`,
also `complete`; emit accepts only the latter).

Fanout tables (targets sorted by the C2 supernode ordinal, deduplicated):

- `inputFanout`: a General-phase `input.read` result maps to its owning
  supernode. Reads marked `event_only` produce no entry — P_input does not
  set `dataActiveFlag` for pure-event inputs; P_event covers them. Further
  cross-supernode propagation of an input's value is handled by
  `computeSupernodeFanout`, not this table.
- `computeSupernodeFanout`: a boundary value maps to its consumer
  supernodes; a real change sets their `dataActiveFlag`. Mem-write consumers
  are excluded (P_mem runs every round).
- `commitStateFanout`: a reg/latch state maps to its General reader
  supernodes (P_publish activation); every reader is covered. General-phase
  `memRead` ops whose target is a **regLatch-class** array join this table
  exactly like scalar readers — their writers commit through the NBA
  regLatch next buffer inside General supernodes. Mem-class array readers
  stay on the P_mem write plan's reader tables.

`timeslotTriggers` maps a firing event act cluster to a timeslot flag: the
Output-phase timeslot tasks' (`core.system.task` with `timeslotFlag`)
timeslotFlag × event_acts Cartesian expansion, in task op-id order with acts
ascending inside one task.

Task sequence (single numa node, single core, 1-based task ids):
P_event (`AlwaysScanCommit`) → one `EventDataGated` task per General
emit-function leaf (the task executes the leaf's `supernodeRange` ordinal
interval) → P_mem (`AlwaysScanCommit`) → P_output (`EvalEnd`, outside the
round loop). Init semantics (dataActiveFlag all-ones,
regLatchStoreNext == regLatchStore, prev = prevInit) are emit concerns and
need no schedule tables.

The verifier recomputes all three fanout tables, the task sequence and the
trigger map from the model and partition tree and requires an exact match.
The legacy plan payload fields (roundSeeds/inputShadows/
quiescenceProjection/demonitor flags) no longer exist — they were deleted
from `CpuSchedulePlan` in M5d-6.
