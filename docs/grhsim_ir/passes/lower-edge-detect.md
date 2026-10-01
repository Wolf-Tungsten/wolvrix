# Edge-detect lowering into P_event

`grhsim.lower-edge-detect` is the second M2 lowering pass of the six-phase
simulation model (`pdocs/simulation-model-refactor`). It migrates the raw
event annotation form — an `event_edges` string-array parameter whose entries
match the consumer's trailing event operands one to one — into the P_event
form: one deduplicated `core.event.edgeDet` per `(event, edge)` cluster, an
Event-phase clone of the event logic cone, and `event_acts` cluster indices
on the consumers. Run `grhsim.classify-event-inputs` first so pure event
inputs are already marked. (Pre-M5 GRH lowering also attached one `__event_*`
history state per event slot as a trailing object ref; that is gone — refs
carry no event slots.)

The pass has nine steps, in order:

1. Collect every op carrying an `event_edges` parameter (the event-sensitive
   kinds: `core.state.regWrite`, `core.state.memWrite`, `core.state.memFill`,
   `core.state.memAssign`, `core.state.memWriteSeq`, `core.system.task`,
   `core.dpi.call`; `latchWrite` has no events). Anything else carrying the
   parameter, a non-string-array value, an edge outside
   `posedge`/`negedge`/`both`, or more edges than operands fails the pass.
2. Deduplicate `(event value, edge)` pairs into clusters. Ops are scanned in
   ascending id and event slots in operand order; first appearance assigns the
   cluster index, which is both the `act` bit index and the `prev` slot index.
3. Extract the event cone: the transitive producer closure of all cluster
   event values is cloned into P_event (every clone gets phase `event`) with
   the shared [`extractCone`](../../include/grhsim/pass/cone_extract.hpp)
   helper, which `grhsim.extract-output-cones` and
   `grhsim.migrate-timeslot-tasks` reuse for their own cones. Roots
   (`core.input.read` / `core.state.read` / `core.state.memRead` /
   `core.compute.constant`) are cloned like any other cone op; object refs,
   parameters, name (plus a `.ev` suffix) and origin are copied unchanged.
4. Create one `core.event.edgeDet` per cluster on the cloned event value, with
   `edge` copied from the consumer spelling and `act`/`prev` set to the
   cluster index; phase `event`.
5. Compute `prevInit`, the event signal's init value, by static evaluation of
   the original cone: `core.state.read` resolves to its state's
   `core.init.const` literal (an explicit-width literal must match the state
   width, otherwise the value falls back to zero), `core.input.read` is `0`,
   `core.compute.constant` is its literal, and `not`/`and`/`or`/`xor`/`xnor`/
   `logicAnd`/`logicOr`/`logicNot`/`mux`/`sliceStatic`/`bitSelect`/`concat`
   evaluate recursively up to 64 bits. Anything else (random init, memRead,
   arithmetic, unknown ops) falls back to a same-width zero literal and is
   counted in `prev_init_fallbacks`. Literals render in the `init.const`
   `constValue` style (`1'h0`, `8'h00`).
6. Rewire each consumer: drop the trailing event operands (object refs carry
   no event slots: `core.system.task` has no refs, `core.dpi.call` keeps the
   leading Function ref, state writes keep the target state), delete
   the `event_edges` parameter and append `event_acts` (int64 array of cluster
   indices in the original event order). `regWrite`, `system.task` and
   `dpi.call` get phase `general`; the four mem writes stay phase-less (`None`)
   until the semantic split ([`grhsim.split-phases`](grhsim-split-phases.md), B5,
   M5d-5) attributes them class-aware — P_mem for `mem`-class targets, P_general
   for `regLatch`-class targets.
7. Sweep any unreferenced `__event_*` history state left over from a pre-M5
   checkpoint together with its InitRecord, and sweep the dead original cone
   ops to a fixed point (`sweepDeadConeOps`): only `core.compute.*` and the
   read-only roots `input.read`/`state.read`/`memRead` with no remaining users
   are removed — dual-use logic whose results still feed live consumers stays
   put. One `model.compact` rebuilds dense ids.
8. The verifier's M2 checks hold on the result: no `event_edges` survives,
   `event_acts` entries resolve into the edgeDet act set, consumers carry
   their event-free shapes, the P_event cone is self-contained, and
   General-phase ops never read Event-phase values.
9. Diagnostics: `clusters`, `edge_dets`, `rewritten_ops`,
   `removed_history_states`, `removed_cone_ops`, `prev_init_fallbacks`.

For example, the dut_081 fragment

```text
op1 core.input.read -> %clk                 (pure event input)
op2 core.input.read -> %d
op4 core.compute.constant -> %one (1'b1)
op5 core.state.regWrite operands=[%one, %d, %one, %clk]
    object_refs=[@q]  parameters={ event_edges: [posedge] }
```

becomes

```text
op1' core.input.read -> %clk.ev   phase=event   (clone, keeps event_only)
op7  core.event.edgeDet operands=[%clk.ev]  phase=event
     parameters={ edge: posedge, act: 0, prev: 0, prevInit: 1'h0 }
op5  core.state.regWrite operands=[%one, %d, %one]  phase=general
     object_refs=[@q]  parameters={ event_acts: [0] }
; op1 is swept.
```

Two consumers of the same `(clk, posedge)` share one detector; one `rst`
consumed as both posedge and negedge gets two edgeDets on the same cloned
value with independent `act`/`prev` slots. The pass is idempotent: with no
`event_edges` left, a second run reports all-zero counts and no change.

The pass is a `SemanticTransform`; the pass manager invalidates existing
backend mappings and increments the semantic revision. GRH lowering emits the
raw annotation form directly (the per-op `__event_*` history states were
removed in M5; only pre-M5 JSON checkpoints still carry them, and the pass
sweeps any that survive unreferenced).
