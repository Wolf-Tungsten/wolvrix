# Mem write plan

`cpu.st.build-mem-write-plan` is the fifth pass (C5) of the CPU mapping
C segment. Requires an `EventActivationMap`-stage mapping; produces the
`MemWritePlan` stage by filling `schedule.memWritePlan` for `P_large_sink`.

- One entry per **LargeSink-phase** write op
  (`write`/`writeMask`/`writeArray`/`writeArrayMask`/`fill`/`latchWrite`), collected by
  `op.phase == LargeSink`. Both wide scalars and arrays can take this path;
  smallSink writes instead commit through `smallSinkStoreNext` in their
  General supernodes. `priority` is the declaration-order rank among writes
  to the same state. The plan is stable-sorted by **ascending priority**;
  within each priority it groups equal sorted `event_acts` signatures.
  The emitter then checks contiguous enable bitmap runs inside each event
  group. Because the plan executes from lower to higher priority, later
  same-state writes overwrite earlier writes directly, including overlapping
  masked bits.
- `readers`: the target largeSink state's General-phase readers, restricted
  by the A7 `storeClass` annotation — `readArray` ops (`staticRow` is engaged
  only when the index
  vector is full-rank and all-constant — the `core.compute.constant` parsed
  literals folded row-major into the linear row number, an exact static
  match — and empty otherwise: a partial index vector reads a sub-array
  spanning rows and any non-constant index is dynamic, both conservative) and
  `core.state.read` ops of the target (scalar or whole-array, always dynamic
  readers, activated on any change). Readers of smallSink states are not
  listed here; they are covered by `commitStateFanout`.
  Event/Output-phase readArray clones are not listed — P_event runs every
  round and P_output runs once per eval unconditionally. Duplicate
  (owner, staticRow) pairs merge.
- `eventFree`: set when the write carries no `event_acts`; such writes run
  every round and convergence relies on cell change detection. If a state
  has multiple event-free writers and General readers, reader activation
  compares the round-start value with the final value after all writes.
  The emitter saves only the first old value of each changed cell in a sparse
  row map, then clears it after the comparison. For example, writing `1`
  followed by `2` to a cell already holding `2` must not activate readers
  forever. This bookkeeping detects convergence; it does not mask or reorder
  writes. States with at most one event-free writer need no row map.

The verifier requires every LargeSink-phase write op to be covered and recomputes
the reader tables from the model and partition tree for an exact match.
