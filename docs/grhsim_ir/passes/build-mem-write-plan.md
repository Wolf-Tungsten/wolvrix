# Mem write plan

`cpu.st.build-mem-write-plan` is the fifth pass (C5) of the CPU mapping
C segment. Requires an `EventActivationMap`-stage mapping; produces the
`MemWritePlan` stage by filling `schedule.memWritePlan` for P_mem.

- One entry per **Mem-phase** write op (`write`/`writeArray`/`fill`),
  collected strictly by `op.phase == Mem` in op-id order —
  the write set targets mem-class states only. General-phase writes on
  regLatch-class arrays are not in the plan: they commit through the NBA
  regLatch next buffer inside their owning supernodes. The plan is generated
  in **declaration order** (stage-06 semantics: same-state writes apply in
  declaration order, later writes overwrite earlier ones — the in-place P_mem
  commit is naturally ordered); `priority` records the write's
  declaration-order rank among the target state's Mem-phase writes, kept for
  checkpoint/debug readability.
- `readers`: the target memory's General-phase readers, restricted to
  **mem-class** targets by the A7 `storeClass` annotation (not by
  `TypeKind`) — `readArray` ops (`staticRow` is engaged only when the index
  vector is full-rank and all-constant — the `core.compute.constant` parsed
  literals folded row-major into the linear row number, an exact static
  match — and empty otherwise: a partial index vector reads a sub-array
  spanning rows and any non-constant index is dynamic, both conservative) and
  whole-array `core.state.read` ops of the target (no index operands:
  always dynamic readers, activated on any write — this is what keeps a
  whole-array `write`'s array source fresh). Readers of regLatch-class arrays are not
  listed here; they are covered by `commitStateFanout` like scalar readers.
  Event/Output-phase readArray clones are not listed — P_event runs every
  round and P_output runs once per eval unconditionally. Duplicate
  (owner, staticRow) pairs merge.
- `eventFree`: set when the write carries no `event_acts`; such writes run
  every round and convergence relies on cell change detection.

The verifier requires every Mem-phase write op to be covered and recomputes
the reader tables from the model and partition tree for an exact match.
