# Semantic phase split (B5)

`grhsim.split-phases` completes the six-phase op attribution at the semantic
layer (M5d-5). It is split out of the legacy `cpu.st.split-phases` mapping
pass: the model's phase attribution is decided here, once, before any CPU
mapping exists; the mapping passes only consume `SimOp::phase`.

Position in the target pipeline: after B1-B4
([`classify-event-inputs`](classify-event-inputs.md),
[`lower-edge-detect`](lower-edge-detect.md),
[`extract-output-cones`](extract-output-cones.md),
[`migrate-timeslot-tasks`](migrate-timeslot-tasks.md)) and before the B6
per-partition [`grhsim.simplify --scope phase`](simplify.md). The B8 seal
(`grhsim.verify --seal semantic`, see below) closes the partition stage right
after B7.

## Attribution table

Every op still at `SimPhase::None` is attributed; ops already tagged by B2/B3/B4
keep their phase:

| Op | Phase |
| --- | --- |
| `core.event.edgeDet` | `Event` (defensive; B2 already tags them) |
| `core.output.write` | `Output` (defensive; B3 already tags them) |
| `core.state.regWrite` | `General` |
| `core.state.latchWrite` | `General` (B4's timeslot write-backs stay `Output`) |
| `core.state.memWrite/memFill/memAssign/memWriteSeq` | **class-aware**, see below |
| everything else (compute, reads, event-free `system.task`/`dpi.call`) | `General` |

The P_mem write duty follows the **target state's store class** (A7
[`grhsim.select-state-stores`](select-state-stores.md)):

- target classified `mem` → `Mem` (P_mem samples the write parameters and
  commits in place);
- target classified `regLatch` → `General` (the write joins the supernode
  fabric and commits through the next-buffer/publish path — this includes mem
  ops on small arrays);
- no classification present, or an unresolvable target ref → legacy fallback
  `Mem` (pre-A7 flows and hand-built test models are unchanged).

The pass requires B3 to have run when outputs exist: tagging an
`output.write` `Output` while its operand is still General-produced violates
the Output cone containment check in the standing verifier (run
`extract-output-cones` first, as the production pipeline does).

`verifyPhaseAttribution` enforces the same class-aware table for every
attributed op on a classified model (both directions: a `Mem`-phase write to a
`regLatch` state and a `General`-phase write to a `mem` state are rejected), so
the attribution stays honest under later rewrites.

## Diagnostics and options

The pass takes no arguments. The info line reports `attributed`,
`phase_event`/`phase_general`/`phase_mem`/`phase_output`,
`mem_writes_reglatch` (mem writes that took the General path via the class
rule) and `already_attributed`. A second run is a no-op.

## Legacy backend coexistence (M5d-5 compat)

The old M3/M4 six-phase backend still schedules **by op type**: its Mem branch,
mem write plan, boundary sampling and operand slot naming collect every
`memWrite/memFill/memAssign/memWriteSeq` regardless of the op's phase, and its
node formation excludes those types. A regLatch-class mem write therefore still
commits in the generated P_mem — which is correct under that backend's layout
(the array physically lives in the single-instance memStore, so the P_mem
in-place commit *is* its NBA mechanism). The class-aware attribution is the
semantic contract consumed by the M5d-6 final mapping (C1/C3/C5), which removes
the compat shims together with the old passes.

## B8 semantic seal

`grhsim.verify --seal semantic` certifies the end of the partition stage (after
it, no semantic rewrite may run). On top of the standing verification it
requires:

1. **total phase attribution** — no `SimPhase::None` op remains;
2. **lowered event form** — no `event_edges` parameter survives (B2 has run);
3. **P_mem sampling locality** — every `Mem`-phase write operand is produced by
   a `General`-phase op (Event cone values never leave P_event; P_output runs
   after the round loop).

The seal is an explicit option of `grhsim.verify` because the standing
per-pass verification must keep tolerating a partially attributed
mid-pipeline model.
