# Phase schedule

`cpu.st.build-phase-schedule` is the eighth and final pass of the six-phase
CPU mapping pipeline (the fourth M4 pass). Requires a `MemWritePlan`-stage
mapping; produces the `PhaseSchedule` stage — the terminal stage of the
six-phase pipeline, at which the mapping becomes `complete`.

Fanout tables (targets sorted by the M4 supernode ordinal, deduplicated):

- `inputFanout`: a General-phase `input.read` result maps to its owning
  supernode. Reads marked `event_only` produce no entry — P_input does not
  set `dataActiveFlag` for pure-event inputs; P_event covers them. Further
  cross-supernode propagation of an input's value is handled by
  `computeSupernodeFanout`, not this table.
- `computeSupernodeFanout`: a boundary value maps to its consumer
  supernodes; a real change sets their `dataActiveFlag`. Mem-write consumers
  are excluded (P_mem runs every round).
- `commitStateFanout`: a reg/latch state maps to its General reader
  supernodes (P_publish arming); every reader is covered. Mem states have no
  rows here — the write plan's reader table activates their readers.

`timeslotTriggers` maps a firing event act cluster to a timeslot flag: the
Output-phase timeslot tasks' (`core.system.task` with `timeslotFlag`)
timeslotFlag × event_acts Cartesian expansion, in task op-id order with acts
ascending inside one task.

Task sequence (single numa node, single core, 1-based task ids):
P_event (`AlwaysScanCommit`) → one `EventDataGated` task per General emit
function → P_mem (`AlwaysScanCommit`) → P_output (`EvalEnd`, outside the
round loop). Init semantics (dataActiveFlag all-ones,
regLatchStoreNext == regLatchStore, prev = prevInit) are emit concerns and
need no schedule tables.

The verifier recomputes all three fanout tables, the task sequence and the
trigger map from the model and partition tree and requires an exact match;
legacy plan payload (roundSeeds/inputShadows/quiescenceProjection/demonitor
flags) is rejected on the six-phase stages.
