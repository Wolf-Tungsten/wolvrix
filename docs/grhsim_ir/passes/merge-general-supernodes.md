# General supernode merging (C2): sink clustering and the non-sink merge frame

`cpu.st.merge-general-supernodes` is the second pass (C2) of the CPU mapping
C segment. Requires a `GeneralNodes`-stage mapping; produces the
`GeneralSupernodes` stage. Since V2-M1 the General branch's nodes split into
two classes that are supernoded by entirely different rules:

- **Non-sink nodes** (value-producing ops, event-carrying ones included) go
  through the legacy merge frame — cluster initialization, the op DAG, three
  coarsen modes over a union-find, the quotient DAG check, and the DP
  segmentation cost model (`--max-op-in-compute-supernode`, default 128).
  There is **no event-domain merge prohibition** (V2-M1 removed it): a
  non-sink supernode fires data-driven, and its event-carrying ops self-guard
  on `eventActStore` inside the emitted body, so mixed act sets are legal.
- **Sink nodes** (no-result ops, one op each from C1) never enter the frame.
  They cluster by **canonical event signature** — the op's sorted
  `event_acts` set — one supernode per signature: the empty signature forms
  the escape class (`SinkEscape`, fires unconditionally every round; the
  data-gated latch write is the typical member), nonempty signatures form
  `SinkEvent` supernodes. Sink supernodes never merge with non-sink
  supernodes or with each other.

### A1: shared-enable subdivision inside a signature cluster

A nonempty signature cluster is further subdivided by **shared write
enable** (`--sink-enable-guard-min-size`, default 8; `0` disables the
subdivision and restores the pre-A1 mapping):

- A sink node qualifies when it holds exactly one op, that op is
  `core.state.regWrite(en, next, mask)`, and the producer of `en`
  (`operands[0]`) is **not** a `core.compute.constant`. Qualifying nodes
  group by the value id of `en`.
- Each group with at least `--sink-enable-guard-min-size` members becomes
  its own `SinkEvent` supernode: same `eventActs` signature, plus the
  **`enableGuard`** attribute (int64, the value index of the shared `en`).
  The emitter ANDs one read of that value into the call-site gate
  (`if(actBitsGuard(sig) && read(enableGuard))`), so the whole cluster is
  skipped when the shared enable is low — sound because every member write
  is individually guarded by exactly that value.
- Everything else — non-`regWrite` sinks, constant-enable writes, groups
  below the threshold — keeps the previous behavior and lands in the
  signature's ordinary supernode (no `enableGuard`). The escape class
  (empty signature) is never subdivided.

Within one signature the guard supernodes come first (ordered by enable
value id), then the ordinary supernode; all remain sink supernodes after
the non-sink prefix, so the ordinal invariants below are unchanged.

**Supernode ordinals are fixed here (resolution 2).** The resulting
supernodes stay direct children of the General branch, **non-sink supernodes
first** (frame/topological order), **sink supernodes after** (lexicographic
signature order, the escape class first among sinks). That child order **is**
the final supernode ordinal space (0..N-1) consumed by the layout, event
bitmaps, mem write plan, schedule and emitter. Later passes never renumber
or remount the supernodes: function packing (C6) only records contiguous
ordinal intervals over them. Because sink ops produce no values, every sink
operand is produced by a non-sink op, so every sink boundary producer has a
strictly smaller ordinal — this is a `verifyCpuMapping` invariant.

Every resulting General supernode records two annotations:

- `eventActs` (engaged, possibly empty): the sorted union of its ops' acts.
  On a sink supernode this **is** the event signature (all member ops share
  it exactly; empty for the escape class). Because the sharing is exact, the
  emitter gates a `SinkEvent` supernode once at the call site and drops the
  per-op event guards inside its body (V2-M3).
- `supernodeCategory` (engaged): `NonSink`, `SinkEscape` or `SinkEvent`.
- `enableGuard` (optional, A1): only on a guard-subdivided `SinkEvent`
  supernode — the value index of the members' shared write enable. The
  verifier requires every op in such a supernode to carry the guard value
  among its operands. The attribute is a **value reference**: any pass that
  renumbers values must remap it — `cpu.st.clone-shared-boundaries` (C2.5)
  rewrites it with `compact()`'s dense value renumbering when it removes
  migrated shared sources.

The verifier replays the classification: sink supernodes must hold only
no-result ops (SinkEvent with one common nonempty signature, SinkEscape with
no event acts at all), non-sink supernodes only value-producing ops, and no
non-sink supernode may trail a sink supernode in the branch order. Both
annotations serialize in the partition's positional JSON attr tail
(`[chunks], [acts…]?, ([supernodeRange] | category)` — the range array and
the scalar category are mutually exclusive; a guard-subdivided SinkEvent
supernode trails one more scalar, the `enableGuard` value index, after the
category). Checkpoints written before
V2-M1 lack the category element and are rejected by the verifier.

For example (two writes, one shared cone):

```text
x = not(d) feeds w1 {event_acts:[0]} and w2 {event_acts:[1]}
```

- `w1` and `w2` carry different signatures, so they form two sink
  supernodes with `eventActs` `[0]` and `[1]` (category `SinkEvent`);
- the cone (`d`, `x`, the constants) batches into one non-sink supernode
  with empty `eventActs` — its ops are data-driven and never wait on an
  edge;
- result: three supernodes, ordinals `[non-sink, sink{0}, sink{1}]`.

Diagnostics: `coarsen_iterations`, `coarsen_cap` (the effective coarsen
merge weight cap), `coarsened_clusters`,
`nonsink_supernodes`, `sink_supernodes`, `sink_escape_supernodes`,
`sink_event_supernodes`, `sink_guard_supernodes` / `sink_guard_ops` (A1:
supernodes carved out by the enable subdivision and the writes they hold),
`sink_ops`, `boundary_value_targets`, plus the
standard `partitions` count.

Integration knob: the XS/HDLBits pipeline scripts read
`XS_WOLF_GRHSIM_IR_SINK_GUARD_MIN` (default `8`, `0` disables) and pass it
as `--sink-enable-guard-min-size`; the XS script also accepts the
`--sink-enable-guard-min-size` CLI override. For boundary-reduction
exploration (plan
`pdocs/perf-optimization/20261005-170704-nonsink-semantic-partition-plan.md`)
the scripts also read `XS_WOLF_GRHSIM_IR_COARSEN_MAX_OP` and pass it as
`--coarsen-max-op` (default `0` = the coarsen merge weight cap follows
`--max-op-in-compute-supernode`, the legacy behavior; a large value
effectively lifts the cap so chain/sibling absorption is limited by
structure, not size — the DP window still caps segment size and oversized
clusters become singleton segments).
