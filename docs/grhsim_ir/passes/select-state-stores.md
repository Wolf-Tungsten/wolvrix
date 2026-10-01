# Select state stores

`grhsim.select-state-stores` is the semantic store classification pass
(M5d-4, pipeline stage A7): the **single decision point** that assigns every
state its store class. It runs once at the end of the whole-graph
optimization stage, after `grhsim.simplify(scope=whole)`. The pass is a pure
annotation — it does not add, remove or rewrite any op, value, state or init
record — so it is registered as a `MetadataTransform`.

## Classes and the NBA contract

Each state carries a `StateStoreClass` (see the IR overview §3.5):

- `regLatch` — small/scalar states. Writes merge into a per-store **next
  buffer** (read-merge-write against next, so several masked writes in one
  round accumulate); readers always read the **current** buffer, so every
  read within a round sees the round-start value (old-value dependency holds
  by construction). `P_publish` commits with one unconditional whole-block
  copy `current = next` per round — the NBA boundary. Multi-round updates
  keep accumulating into next and are committed at each round's publish.
- `mem` — large contiguous arrays. Write parameters (enable/address/data/
  mask) are sampled during the Event/General compute phases; `P_mem` then
  applies the writes **in place** in static priority order (op order;
  `memWriteSeq` keeps its internal operand order). Only addressed cells are
  touched, so partial and sparse writes cost proportionally — no whole-block
  copy. Readers in the compute phases see the store as of the previous
  round's `P_mem` commit; `P_mem` runs after `P_general` inside every round,
  so within-round reads always observe the old value.

Both classes therefore implement the same one-round-latency NBA semantics;
they differ only in *how* a commit happens (block copy vs in-place cell
update) and hence in cost structure. The class annotation is what allows the
layout and emitter to pick the mechanism — consumers must never re-derive
the decision from the state's type (e.g. `TypeKind::Array`).

## Classification policy

The decision uses the optimized (post stage-A) shape of each state:

- **contiguity**: only `core.array` states are eligible for `mem`;
- **size**: an array whose linear byte size (`ceil(row-major bits / 8)`,
  semantic bits — the physical 4-state doubling is a backend concern) is at
  least `--mem-min-bytes` (default 64) becomes `mem`;
- everything else — scalars of any width, small arrays, `real`/`string`
  states — becomes `regLatch`.

Update cost is the rationale, not a separate signal: a `mem`-class array is
written through addressed mem ops whose per-round touched-cell count is
typically far below the array size, so in-place commit avoids the
`O(size)` publish copy; below the threshold the block copy is a single cheap
`memcpy`, and keeping the state in regLatch keeps it on the uniform
next/current path. Whole-array writes (`memFill`/`memAssign`) do not change
the class: applying them in place still avoids materializing a whole-array
boundary value.

Contrast example (default threshold):

```text
q:        core.logic<64>              -> regLatch   (scalar, 8 B)
smallRam: core.array<logic<32>, 4>    -> regLatch   (16 B, publish copy is cheap)
bigRam:   core.array<logic<64>, 4096> -> mem        (32 KiB, sparse addressed
                                                     writes commit in place)
```

### Incremental classification

Classification is **total** once present (the verifier rejects a model where
only some states carry a class). Passes that create or rebuild states after
`select-state-stores` has run must therefore classify them at creation:

- `grhsim.migrate-timeslot-tasks` classifies its `__tslot_prev_*` monitoring
  states `regLatch` — they are small latch histories whose Output-phase
  write-back commit timing is preserved by the publish boundary;
- `grhsim.used-bits` lets a narrowed replacement state inherit the class of
  the state it replaces (narrowing only applies to scalar two-state logic,
  so the inherited class is always `regLatch`);
- `compact()` carries the class on surviving states automatically (it is a
  `StateObject` field), so passes that only delete states need no handling.

A pass running on a not-yet-classified model (`hasStateStoreClassification()
== false`) leaves new states unclassified; `select-state-stores` itself only
fills unclassified states unless `--reclassify true` is given.

## Consumers

- **B5 [`grhsim.split-phases`](grhsim-split-phases.md)** (M5d-5, wired): the
  first consumer. A write op's P_mem responsibility is decided by the *target
  state's class* — mem ops on `mem`-class states go to P_mem; writes on
  `regLatch` states (including mem ops on small arrays) stay on the
  next-buffer path (General phase).
- **C3 `cpu.st.layout-named-stores`** (M5d-6): pure physical layout — it
  consumes the annotation and makes zero classification decisions.
- The legacy M3/M4 passes still classify implicitly by `TypeKind::Array`;
  they ignore this annotation, so wiring `select-state-stores` into the
  current production pipeline does not change their behavior.

## Options and diagnostics

- `--mem-min-bytes N` (default 64): minimum linear byte size for `mem`.
- `--reclassify true|false` (default false): recompute classes even for
  already-classified states.
- `--report <path>`: per-state TSV (`state, name, class, kind, shape, bits,
  bytes, writes, reads`; `shape` is the outermost-first `x`-joined array
  dimensions or `-` for scalars), covering every state with its final class.

The info diagnostic reports `mem_min_bytes`, the per-class counts of newly
classified states (`state_stores_reg_latch`/`state_stores_mem`), the number
of already-classified states kept (`state_stores_kept`), and the total
semantic bytes per class over all states (`reg_latch_bytes`/`mem_bytes`).

## Verifier rules

With any classification present, `verifyGrhSimModel` enforces:

1. totality — every state carries a class;
2. `mem` requires a `core.array` state;
3. a `mem`-class state may not be targeted by `core.state.regWrite` or
   `core.state.latchWrite` (its writes must be mem ops so P_mem can own the
   commit).

The phase consistency between the classification and the six-phase write
attribution (mem ops on `mem` states carry P_mem, writes on `regLatch`
states carry P_general) is enforced for every attributed op by the
class-aware `verifyPhaseAttribution` (M5d-5), and the partition stage seals
with `grhsim.verify --seal semantic` (B8).

JSON: the class is the optional fifth element of a `states` row
(`[id, name, type, origin, class]`); it is written only once classified, so
unclassified checkpoints stay byte-compatible with the pre-M5d-4 schema and
classified checkpoints round-trip byte-stably.
