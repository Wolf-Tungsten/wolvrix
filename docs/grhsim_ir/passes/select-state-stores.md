# Select state stores

`grhsim.select-state-stores` is the semantic store classification pass
(M5d-4, pipeline stage A7): the **single decision point** that assigns every
state its store class. It runs once at the end of the whole-graph
optimization stage, after `grhsim.simplify(scope=whole)`. The pass is a pure
annotation — it does not add, remove or rewrite any op, value, state or init
record — so it is registered as a `MetadataTransform`.

## Classes and the NBA contract

Each state carries a `StateStoreClass` (see the IR overview §3.5):

- `smallSink` — small states, scalar or array. Writes merge into a per-store **next
  buffer** (read-merge-write against next, so several masked writes in one
  round accumulate); readers always read the **current** buffer, so every
  read within a round sees the round-start value (old-value dependency holds
  by construction). `P_publish` commits with one unconditional whole-block
  copy `current = next` per round — the NBA boundary. Multi-round updates
  keep accumulating into next and are committed at each round's publish.
- `largeSink` — large logic states, scalar or array. Write parameters
  (enable/address/data/mask as applicable) are sampled during the
  Event/General compute phases; `P_large_sink` then applies writes **in
  place**. Later same-state writes have higher priority: the schedule runs
  lower priority first and higher priority last, then clusters by event
  signature and enable range. Scalar `write`/`latchWrite` and array
  `write`/`writeArray`/`fill`
  use the same large-state phase. Sparse array writes touch addressed cells
  instead of copying the whole store. General-phase readers see the store as
  of the previous round's `P_large_sink` commit, because this phase runs
  after `P_general` in every round.

Both classes therefore implement the same one-round-latency NBA semantics;
they differ only in *how* a commit happens (block copy vs in-place cell
update) and hence in cost structure. The class annotation is what allows the
layout and emitter to pick the mechanism — consumers must never re-derive
the decision from the state's type shape (e.g. whether the type carries a
shape).

Several event-free writers can temporarily change a state and restore its
original value every round. For states with multiple event-free writers and
General readers, activation therefore checks the final value: smallSink
compares its existing current/next buffers at `P_publish`; largeSink records
the original value of each changed cell and compares after `P_large_sink`.
No whole-RAM snapshot or priority claim masks are used.

## Classification policy

The decision uses the optimized (post stage-A) total size of each state:

- **size**: a `core.logic` state whose semantic byte size
  (`ceil(total width / 8)`; the physical four-state doubling is a backend
  concern) is at least `--mem-min-bytes` (default 64) becomes `largeSink`,
  whether or not it has a shape;
- smaller logic states and `real`/`string` states become `smallSink`.

Update cost is the rationale, not a separate classification signal. Above
the threshold, in-place commit avoids an `O(size)` publish copy; below it,
the next/current block copy stays cheap. The op family and the presence of
`shape` do not override the size decision.

Contrast example (default threshold):

```text
q:        core.logic<64>                      -> smallSink  (scalar, 8 B)
smallRam: core.logic<128>, shape=[4]          -> smallSink  (array, 16 B)
wideQ:    core.logic<512>                     -> largeSink  (scalar, 64 B)
bigRam:   core.logic<262144>, shape=[4096]    -> largeSink  (array, 32 KiB)
```

### Incremental classification

Classification is **total** once present (the verifier rejects a model where
only some states carry a class). Passes that create or rebuild states after
`select-state-stores` has run must therefore classify them at creation:

- `grhsim.migrate-timeslot-tasks` classifies its `__tslot_prev_*` monitoring
  states `smallSink` — they are small latch histories whose Output-phase
  write-back commit timing is preserved by the publish boundary;
- `grhsim.used-bits` lets a narrowed replacement state inherit the class of
  the state it replaces; a later explicit reclassification can recompute the
  size threshold if needed;
- `compact()` carries the class on surviving states automatically (it is a
  `StateObject` field), so passes that only delete states need no handling.

A pass running on a not-yet-classified model (`hasStateStoreClassification()
== false`) leaves new states unclassified; `select-state-stores` itself only
fills unclassified states unless `--reclassify true` is given.

## Consumers

- **B5 [`grhsim.split-phases`](grhsim-split-phases.md)** (M5d-5, wired): the
  first consumer. A write op's `P_large_sink` responsibility is decided by
  the *target state's class* — writes on `largeSink` states go to
  `P_large_sink`; writes on `smallSink` states stay on the
  next-buffer path (General phase).
- **C3 `cpu.st.layout-named-stores`** (M5d-6): pure physical layout — it
  consumes the annotation and makes zero classification decisions. The old
  implicit array-type classification lived in the legacy M3/M4 backend
  passes, which were deleted in M5d-6.

## Options and diagnostics

- `--mem-min-bytes N` (default 64): minimum semantic byte size for `largeSink` logic states.
- `--reclassify true|false` (default false): recompute classes even for
  already-classified states.
- `--report <path>`: per-state TSV (`state, name, class, kind, shape, bits,
  bytes, writes, reads`; `shape` is the outermost-first `x`-joined array
  dimensions or `-` for scalars), covering every state with its final class.

The info diagnostic retains the legacy keys `mem_min_bytes` and per-class
counts (`state_stores_reg_latch`/`state_stores_mem`), the number
of already-classified states kept (`state_stores_kept`), and the total
semantic bytes per class over all states (`reg_latch_bytes`/`mem_bytes`).

## Verifier rules

With any classification present, `verifyGrhSimModel` enforces:

1. totality — every state carries a class;
2. `largeSink` requires a `core.logic` state; it may be scalar or shaped,
   and scalar `latchWrite` is valid on that path.

The phase consistency between the classification and the six-phase write
attribution (writes on `largeSink` states carry `P_large_sink`, writes on `smallSink`
states carry P_general) is enforced for every attributed op by the
class-aware `verifyPhaseAttribution` (M5d-5), and the partition stage seals
with `grhsim.verify --seal semantic` (B8).

JSON: the class is the optional fifth element of a `states` row
(`[id, name, type, origin, class]`); it is written only once classified, so
unclassified checkpoints stay byte-compatible with the pre-M5d-4 schema and
classified checkpoints round-trip byte-stably.
