# Mem write plan

`cpu.st.build-mem-write-plan` is the fifth pass (C5) of the CPU mapping
C segment. Requires an `EventBitmaps`-stage mapping; produces the
`MemWritePlan` stage by filling `schedule.memWritePlan` for P_mem.

- One entry per **Mem-phase** write op (`memWrite`/`memFill`/`memAssign`/
  `memWriteSeq`), collected strictly by `op.phase == Mem` in op-id order —
  the write set targets mem-class states only. General-phase writes on
  regLatch-class arrays are not in the plan: they commit through the NBA
  regLatch next buffer inside their owning supernodes. `priority` counts the
  writes per target memory in op-id order, so a same-address collision lets
  the later write override the earlier one — the source program order.
- `readers`: the target memory's General-phase readers, restricted to
  **mem-class** targets by the A7 `storeClass` annotation (not by
  `TypeKind`) — `memRead` ops (`staticRow` is engaged when the address
  operand is a `core.compute.constant` (the parsed literal, an exact static
  match) and empty otherwise (dynamic address, conservative)) and
  whole-array `core.state.read` ops of the target (no address operand:
  always dynamic readers, activated on any write — this is what keeps a
  memAssign's array source fresh). Readers of regLatch-class arrays are not
  listed here; they are covered by `commitStateFanout` like scalar readers.
  Event/Output-phase memRead clones are not listed — P_event runs every
  round and P_output runs once per eval unconditionally. Duplicate
  (owner, staticRow) pairs merge.
- `eventFree`: set when the write carries no `event_acts`; such writes run
  every round and convergence relies on cell change detection.

The verifier requires every Mem-phase write op to be covered and recomputes
the reader tables from the model and partition tree for an exact match.
