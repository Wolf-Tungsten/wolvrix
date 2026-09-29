# Output cone extraction

`grhsim.extract-output-cones` is the third M2 lowering pass of the six-phase
simulation model (`pdocs/simulation-model-refactor`). It runs after
`grhsim.lower-edge-detect` and before `grhsim.migrate-timeslot-tasks`, and
moves the logic feeding the DUT outputs into P_output.

For every `core.output.write` the pass clones the producer-side cone of its
operand into P_output with the shared
[`extractCone`](../../include/grhsim/pass/cone_extract.hpp) helper (the same
helper `grhsim.lower-edge-detect` uses for P_event), rewires the operand to
the cloned value and tags the `output.write` itself Output:

```text
core.state.read -> %q      (phaseless)     core.state.read -> %q.out   (phase: output)
core.output.write %q                       core.output.write %q.out    (phase: output)
```

The original cone is then swept with the shared fixed-point DCE
(`sweepDeadConeOps`): a cone op whose results lost every user — the read
above when nothing else consumed `%q` — is removed, while dual-use ops (a
state read that also feeds a `regWrite`) stay on the general side. Only pure
`core.compute.*` ops and the read-only roots (`core.input.read` /
`core.state.read` / `core.state.memRead`) are ever removed, so the sweep
cannot delete side-effecting logic.

The pass is idempotent: an `output.write` whose operand is already produced
by an Output-phase op is considered extracted, so a second run reports no
change. Diagnostics report `cloned_ops` (cone ops cloned this run) and
`removed_ops` (original cone ops swept). With no `core.output.write` in the
model the pass is a no-op (`cloned_ops=0 removed_ops=0`, unchanged).

The verifier mirrors the new shape (`verifyOutputLowering`, gated on the
presence of any Output-phase op): every Output-phase op's operands must be
produced by Output-phase ops, and General-phase ops must not read
Output-phase values.
