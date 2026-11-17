# Covered register views at compute boundaries

GrhSIM can remove a scalar boundary's change detection when an existing
register notification already activates its producer and every consumer. This
is a static proof over the scheduled graph and emitter activation map. No
execution profile, measured frequency, or workload data is an input.

Two options expose the rewrite in separate stages; both default to **off**:

| Attribute | Environment override | Effect |
| --- | --- | --- |
| `covered_state_view_notifications` | `WOLVRIX_GRHSIM_COVERED_STATE_VIEW_NOTIFICATIONS` | Remove redundant value comparison and fanout notification; keep its stored value. |
| `covered_state_view_inline` | `WOLVRIX_GRHSIM_COVERED_STATE_VIEW_INLINE` | With the parent enabled, replace uses with a typed register expression and remove the assignment and storage slot. |

Explicit attributes take precedence over environment variables. Enabling only
the second option has no effect. The execution-plan feature counters are
`covered_state_view_notifications` and `covered_state_view_inlined_definitions`.

## Proof and scope

Without the composition extension, the result must be one scalar `SliceStatic`, `Not`, `LogicNot`, or `Assign`
directly consuming a local, unmaterialized `RegisterReadPort`. Its register must
have exactly one scheduled commit writer, matching width and signedness, and
must not use reg-to-mem intent storage. The result must have a canonical,
exclusive scalar slot and a nonempty boundary fanout.

The producer and all emitted consumers must be pure compute regions. The read
precedes its definition; local uses follow it; external consumers follow the
producer in active execution order. The existing register activation frontier
must already contain **all** those regions. The external use set must exactly
match the value's fanout. The proof freezes this frontier before considering any
candidate, so one deleted edge cannot justify deleting another.

No region, operation order, register notification target, or event is added or
merged. When a register changes, its original notification schedules the
producer before its consumers. When only another consumer input changes, the
register and its view remain unchanged. Initial evaluation still runs the
original regions. Commit users are excluded because reading current state there
would need a separate proof about the old-state snapshot.

The second stage uses the existing scalar assignment renderer, including width
masks, signed conversion and Boolean normalization. This is an inexpensive
one-operation view, not arbitrary recomputation across regions. Storage layout
is rebuilt only after the same candidate set has been proved.

## Composable state expressions

Two additional attributes, both **off by default**, extend the same proof:

| Attribute | Environment override | Effect |
| --- | --- | --- |
| `covered_state_view_composition` | `WOLVRIX_GRHSIM_COVERED_STATE_VIEW_COMPOSITION` | Prove a local scalar expression DAG and remove its redundant notification. Requires `covered_state_view_notifications`. |
| `covered_state_view_composition_inline` | `WOLVRIX_GRHSIM_COVERED_STATE_VIEW_COMPOSITION_INLINE` | Also remove storage for bounded expressions. Requires composition and `covered_state_view_inline`. |

The original one-operation views keep their previous decisions. Each additional
view records its own storage decision, so a notification-only composite does
not lose its slot just because the original view-inline option is enabled.

The DAG may contain assign, static slice, bitwise/logical not, equality,
bitwise/logical and/or, concat and reduce-or. All nonconstant definitions must
be in the producer region, precede their uses, be scalar, and ultimately read
the same register symbol with matching width and signedness. Separate SSA reads
of that register share the state origin. Unknown literals, external inputs,
multiple state symbols, memory/latch reads and unsupported operations stop the
proof. A protected cache key or result anywhere in the DAG also stops it.

Fixed bounds are 32 distinct non-read/nonconstant operations and depth 16.
Inlining additionally requires at most four expanded expression-tree operations,
expanded operations times scheduled operand uses at most 16, and rendered text
at most 4096 bytes. Constants and direct register leaves have zero operation
cost; repeated subexpressions count repeatedly in the expanded-tree budget.
Longer proved expressions retain their computation and storage while losing
only redundant notifications. These limits never consume runtime measurements.

Every node is rendered using the existing typed scalar lowering, including
intermediate truncation. The register frontier, root fanout, ownership, pure
consumer and execution-order requirements above still apply. Proofs are all
completed before removing fanouts or rebuilding storage; the plan does not use
previously deleted edges as evidence. `covered_state_view_composition` in the
execution plan counts additional proved roots, including those retaining slots.

`emit-grhsim-state-expressions` tests `Not(Slice(q))`, `Slice(Not(q))`, equality,
concat/reduction with multiple reads of the same register, and a longer chain
that must keep its slot. It compares independent O0/O3 sanitizer oracles and
region counts across the original, notification-only and bounded-inline stages;
negative fixtures protect unknown constants, public values, different states
and cache keys.

The existing `compute_batch_dispatch_guard` is a separate caller-side activity
check. It is not part of state-expression selection. The
`emit-grhsim-compute-entry-guard` regression places a producer and receiver in
different dispatch words and checks same-round propagation with held inputs,
two clocks, reset and reinitialization. A guard must read activity immediately
before its batch call; commit/event dispatch cannot be replaced by this mask.

## Conservative fallbacks

Keep the original boundary for public ports, event values, waveform-observed
values, aliases, packed views, memory/effectful users, multiple writers, missing
independent activation, or reversed execution order. Waveform and runtime
profiling modes disable this rewrite. Shared equality cache keys/results, local
decode keys and conditional input-cache keys/results are protected even where
their emitter-generated uses are absent from ordinary IR use lists.

Unsupported combinations retain the original implementation: full-pass
specialization, direct Boolean state views, producer dirty-word tracking,
change-trigger mux caches, proved-zero elision, local compute reordering,
priority-mux chains, Boolean producer reduction and Boolean zero guards.
Activation cohort/cofire policies and active-mask gap packing also disable the
rewrite. Compute words with an additional pure-event execution guard are
excluded individually.

`emit-grhsim-covered-state-views` checks default/off identity, option precedence,
execution-plan coverage and unchanged state frontiers. Independent simulation
oracles cover 1-, 17- and 64-bit state, signed input state, masked writes,
asynchronous reset, two clocks, unchanged clocks, reinitialization and repeated
evaluation at Clang O0/O3 with ASan/UBSan. Instrumented region counts must agree
between the original, notification-only and direct-view forms. Negative fixtures
cover missing readers, public/effectful/commit/event uses, multiple writers and
decode-cache keys. A waveform request must either retain the original lowering
or be rejected by a build without libfst.

The rule also applies to other designs, including XiangShan, when the same
conditions hold. Inter-partition transfers and memory-backed registers do not
inherit this proof; measured benefit on one design does not establish coverage
or speedup on another.

## Cross-region expression proof

The separate default-off emitter attribute `covered_state_view_cross_region`
(environment `WOLVRIX_GRHSIM_COVERED_STATE_VIEW_CROSS_REGION`) extends composition
through earlier pure compute groups. It requires the existing notifications and
composition options. It preserves the fixed operation/depth and inline budgets.
All nonconstant dependency edges must follow local operation order or increasing
compute active IDs. Register sources remain non-materialized single-writer reads;
commits occur after compute, so the expression uses one register snapshot.

The producer and every consumer must still belong to the original independent
register activation frontier. The proof neither adds activation heads nor uses
removed candidate edges as evidence for another candidate. Cache keys/results,
events, aliases, waveform/public values, effect groups and commit uses retain
the existing protection. A cross-group expression by itself does not establish
activation coverage. Diagnostic comments mark only proofs actually crossing a
group; the option-off output is unchanged.

The focused `emit-grhsim-cross-region-views` regression checks cross-group
slice/not/reduction chains, notify-only and bounded inline forms, three widths
including signed and 64-bit, two clocks, asynchronous reset, masked writes,
repeated evaluation and unchanged group execution counts at O0/O3 with
sanitizers. Missing independent state reads, effects, public values and decode
keys are rejected.
