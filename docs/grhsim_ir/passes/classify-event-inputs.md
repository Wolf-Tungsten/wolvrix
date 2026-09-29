# Event input classification

`grhsim.classify-event-inputs` is the first M2 lowering pass of the six-phase
simulation model (`pdocs/simulation-model-refactor`). It runs after
GRH-to-GrhSIM lowering and before `grhsim.lower-edge-detect`, and marks pure
event inputs so the P_input stage can skip data-activity tracking for them.

A `core.input.read` is a pure event input when its result is used **only** in
event operand positions — the trailing `len(event_edges)` operand slots of
event-sensitive ops (`core.state.regWrite`, the four mem writes,
`core.system.task`, `core.dpi.call`). Such an op gains a bool parameter
`event_only=true`:

```text
core.input.read @clk                 core.input.read @clk
  (used only as regWrite event)   =>   parameters: { event_only: true }
```

Inputs with any data-path use stay unmarked — for example `rst` appearing as
both a `regWrite` event and a mux select keeps its P_input change detection,
so repeated asynchronous resets are not swallowed by edge gating. Unused
inputs are not marked either. The pass only ever adds `event_only=true`; it
never writes an explicit `false`.

The pass is idempotent: an already-marked input is left untouched and the
second run reports no change. Diagnostics report `event_only_inputs` — the
number of inputs newly marked by this run. Rewritten ops are followed by pool
compaction, so the stored checkpoint stays byte-stable across runs.

The marker is metadata for later passes and backends:
`grhsim.lower-edge-detect` clones the cone (parameter included) into P_event
and sweeps the original read when it becomes dead; P_event then detects edges
on pure event inputs instead of P_input setting `dataActiveFlag` for them.
