# Phase schedule

`cpu.st.build-phase-schedule` is the seventh pass (C7) of the CPU mapping C
segment. Requires a `GeneralFunctions`-stage mapping (M5d-6 moved function
packing ahead of scheduling); produces the `PhaseSchedule` stage — a terminal
stage at which the mapping becomes `complete` (M5d-7 appends C8
[`plan-translation-units`](plan-translation-units.md) → `TranslationUnits`,
also `complete`; emit accepts only the latter).

Fanout tables (targets sorted by the C2 supernode ordinal, deduplicated).
V2 (M2): every table's targets are **nonSink supernodes only** (boundary
2) — smallSink supernodes fire on their eventActStore signature
(SmallSinkEvent) or unconditionally every round (SmallSinkEscape), never on
`nonSinkActiveFlag`:

- `inputFanout`: a General-phase `input.read` result maps to its owning
  supernode. Reads marked `event_only` produce no entry — P_input does not
  set `nonSinkActiveFlag` for pure-event inputs; P_event covers them. Further
  cross-supernode propagation of an input's value is handled by
  `computeSupernodeFanout`, not this table.
- `computeSupernodeFanout`: a value in `nonSinkBoundaryStore` maps to its
  consumer supernodes; a real change sets their `nonSinkActiveFlag`.
  Large-sink write consumers are excluded (`P_large_sink` runs every round),
  and smallSink consumers are filtered out.
- `commitStateFanout`: a smallSink state maps to its General nonSink readers
  (next-round activation). Scalar and shaped `read`/`readArray` readers use
  the same fanout table; their writers commit through `smallSinkStoreNext`.
  LargeSink readers stay on the `P_large_sink` write plan's reader tables.

`timeslotTriggers` maps a firing event act cluster to a timeslot flag: the
Output-phase timeslot tasks' (`core.system.task` with `timeslotFlag`)
timeslotFlag × event_acts Cartesian expansion, in task op-id order with acts
ascending inside one task.

Task sequence (single numa node, single core, 1-based task ids):
P_event (`AlwaysScanCommit`) → one `DataGated` task per General
emit-function leaf (the task executes the leaf's `supernodeRange` ordinal
interval; the pre-v2 name was `EventDataGated`, before the eventActiveFlag
half of the dual gate was removed) → P_large_sink (`AlwaysScanCommit`) → P_output
(`EvalEnd`, outside the round loop). Init semantics (`nonSinkActiveFlag`
all-ones, `smallSinkStoreNext == smallSinkStore`, prev = prevInit) are emit
concerns and need no schedule tables.

Within smallSink supernodes and `P_large_sink`, writes run in ascending
priority order (lower priority first); each priority groups by `eventAct`,
then uses the relevant small/large enable boundary bitmap to skip disabled runs before reading
payload. The schedule must not merge event clusters across priorities.

The verifier recomputes all three fanout tables, the task sequence and the
trigger map from the model and partition tree and requires an exact match.
The legacy plan payload fields (roundSeeds/inputShadows/
quiescenceProjection/demonitor flags) no longer exist — they were deleted
from `CpuSchedulePlan` in M5d-6.
