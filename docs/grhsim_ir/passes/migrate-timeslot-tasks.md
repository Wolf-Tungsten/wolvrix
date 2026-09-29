# Timeslot task migration

`grhsim.migrate-timeslot-tasks` is the fourth and last M2 lowering pass of
the six-phase simulation model (`pdocs/simulation-model-refactor`). It runs
after `grhsim.lower-edge-detect` and `grhsim.extract-output-cones`, and
migrates the time-slot system tasks — `core.system.task` ops whose `name`
parameter is `strobe` or `monitor` — into P_output. Every other system task
(display/write/fdisplay/fwrite/info/warning/error/fatal/finish/stop) keeps
its phase and is never touched.

The pass consumes the lowered event form: a task still carrying
`event_edges` is skipped untouched (that shape is `grhsim.lower-edge-detect`
input, not this pass's). All operand cones are cloned with the shared
[`extractCone`](../../include/grhsim/pass/cone_extract.hpp) helper in one
extraction per run, so cones shared between tasks are cloned once.

## Event-driven tasks (carry `event_acts`)

- The op gains an int64 `timeslotFlag` parameter — an index 0..K-1 assigned
  in ascending op-id order across the event-driven tasks migrated by this
  run (only event-driven tasks receive one).
- `event_acts` is kept unchanged: M4 builds the act-bit → timeslotFlag
  static map from it.
- The whole `[callCond, args...]` operand cone is cloned into P_output and
  the operands are rewired to the clones; the task itself becomes Output
  phase.

## Event-free tasks

Without an act bit, the task samples its operands at the end of the round.
For each operand slot `i` of `[callCond, args...]` on op id `N`:

- one history state `__tslot_prev_N_i` of the operand's type, with a
  same-width zero `core.init.const` literal (`"1'h0"`, `"8'h00"`, …);
- an Output-phase `core.state.read` of that prev state and a
  `core.compute.ne` comparing the current-value clone against the prev read;
  the ne bits fold left through `core.compute.or` into `changed` (a
  single-operand task uses its bare ne, no or);
- the guard (callCond operand) is rewritten to
  `core.compute.logicAnd(callCondClone, changed)` — logicAnd because
  callCond may be any-width logic, and the 1-bit result still satisfies the
  any-nonzero callCond rule of `core.system.task`;
- one Output-phase `core.state.latchWrite` per prev state writes the
  current-value clone back with enable `1'h1` and an all-ones mask of the
  slot width (`"8'hff"` style; past 64 bits the literal is `'f'`-padded).

For example, `$monitor("q=%b", q)` guarded by `%en` becomes:

```text
states: __tslot_prev_4_0 (logic<1>, init 1'h0), __tslot_prev_4_1 (logic<1>, init 1'h0)
%en.out      = core.input.read @en                       (phase: output)
%q.out       = core.state.read @q                        (phase: output)
%prev0       = core.state.read @__tslot_prev_4_0         (phase: output)
%prev1       = core.state.read @__tslot_prev_4_1         (phase: output)
%ne0         = core.compute.ne %en.out, %prev0           (phase: output)
%ne1         = core.compute.ne %q.out,  %prev1           (phase: output)
%changed     = core.compute.or %ne0, %ne1                (phase: output)
%guard       = core.compute.logicAnd %en.out, %changed   (phase: output)
core.system.task %guard, %q.out  { name: monitor, ... }  (phase: output)
core.state.latchWrite @__tslot_prev_4_0 <- %en.out       (phase: output, enable 1'h1, mask 1'h1)
core.state.latchWrite @__tslot_prev_4_1 <- %q.out        (phase: output, enable 1'h1, mask 1'h1)
```

The original cone is swept afterwards with the shared fixed-point DCE
(dual-use ops stay); the whole rewrite is followed by pool compaction.

## Execution-semantics contract (M5)

The ne comparisons and the latchWrite write-backs live in P_output with no
dependence edges between them; the IR deliberately does not order them. The
required execution order — within one P_output evaluation every prev read
observes the *previous* round's value and the write-backs commit only after
all P_output evaluation — is an M5 emit contract: the `__tslot_prev_*`
states are emitted as dedicated stores committed at the end of P_output.
Later passes must not "repair" the apparent read/write race by reordering
or merging these ops.

## Eligibility and idempotency

- Event-free tasks whose operands are not all `core.logic` values are
  skipped (ne/latchWrite are only defined on logic values); they keep their
  phase. This is the only eligibility filter besides the name list.
- Idempotency: event-driven tasks are skipped when `timeslotFlag` is
  already present; event-free tasks are skipped when every operand is
  already produced by an Output-phase op (exactly the migrated shape). The
  `__tslot_prev_N_i` names embed the pre-compaction op id and are not used
  as the marker.
- Diagnostics: `migrated_event_tasks`, `migrated_free_tasks` and
  `timeslot_flags` (indices handed out this run). With nothing to migrate
  the pass reports zeros and no change.

The matching verifier rules (`verifyOutputLowering`, unconditional):
`__tslot_prev_*` states may only be written by Output-phase
`core.state.latchWrite` ops, and `timeslotFlag` is only valid as a
non-negative int64 on an Output-phase `core.system.task` carrying
`event_acts`.
