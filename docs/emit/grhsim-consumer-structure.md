# Consumer structure lowering

Two independent, default-off emitter options address preparation outside its
consumer and local predicate consumption. Neither changes the IR, activity
schedule, state boundaries, or production defaults. Attributes override the
corresponding environment variables. Neither option reads a runtime profile.

## Conditional wide preparation

`conditional_mux_wide` / `WOLVRIX_GRHSIM_CONDITIONAL_MUX_WIDE` extends
[conditional mux cones](grhsim-conditional-mux-cones.md). The parent option
`conditional_mux_cones` must also be enabled.

The scalar-result mux planner may absorb private `Concat`, `Assign`,
`SliceStatic`, and `SliceDynamic` operations that require word lowering. Results
and operands are bounded to 4096 bits. The existing 16–256-operation arm budget
and pure-region, unique-use, storage-alias, event, public-output, and cross-region
checks still apply. Operations owned by another lowering are excluded. This
does not duplicate shared preparation, merge state views, move state writes,
or specialize a branch to the observed workload.

For example, a private wide concat followed by a 32-bit dynamic slice and scalar
logic formerly stayed before the mux, even when its result was needed in only
one arm. Both concat construction and slice now occur inside that arm. They use
the same word emitter as before; truncation, partial last words, and out-of-range
indices retain their original semantics. A publicly observable concat remains
eager. An oversized concat or effectful region keeps the previous lowering.

The root result is still assigned at its original position with the original
change notifications. The execution plan reports
`conditional_mux_wide_operations_upper` as selector-dependent work, rather than
charging every absorbed word operation on every invocation.

## Local predicate consumers

`local_predicate_sets` / `WOLVRIX_GRHSIM_LOCAL_PREDICATE_SETS` lowers dense sets
of local equalities at the consumer, without adding a persistent one-hot table.
It is an emitter counterpart to [predicate-set](../transform/predicate-set.md),
so it does not recreate activity boundaries or disrupt existing sparse caches.

Within a pure, topologically ordered compute region, the planner traverses
Boolean OR trees and ReduceOr of Boolean concatenations. A family consists of
local equality leaves sharing an unsigned scalar Logic source and known
constants of the same width. At least 16 distinct constants and an average of
at least 8 constants per maximal non-wrapping interval are required. Traversal
is limited to 512 leaves and 2048 visited values. Sources are inputs or
independently owned canonical scalar storage; direct state views, aliases and
event values are excluded. Pure-region ordering and exclusive source storage
ensure that reading the source at the consumer observes the same value.

For example:

```text
(address == 100) | ... | (address == 131) | other
    -> ((address >= 100) & (address <= 131)) | other
```

The unsigned bounds are retained even for a full declared domain, because input
storage can contain spare high bits. Adjacent intervals never wrap at UINT64_MAX.
Other sources form separate families; unrelated Boolean leaves remain intact.
Signed sources, sparse/small families, unknown constants and unsafe regions
retain ordinary expressions.

The replacement is emitted at the original consumer with its original storage
and notification behavior. Only pure local Eq/OR/concat/reduction operations
with no remaining emitted users are suppressed. Public, cross-region,
change-tracked and packing inputs stay intact. Shared equality caches and
conditional cones retain ownership of their operations. Local one-hot decoding,
priority-mux and Boolean group rewrites currently disable this option so their
overlapping ownership cannot silently change.

The execution plan exposes `local_predicate_set_roots`,
`local_predicate_set_constants`, `local_predicate_set_intervals` and
`local_predicate_set_dead_operations`. It does not apply the original generic
per-op price to the replacement expression. Family constant totals can overlap
between consumers; they are not a count of distinct eliminated equalities.

## Validation and applicability

The `emit-grhsim-conditional-wide` and `emit-grhsim-local-predicate-sets` tests
check emitted structure and independent functional oracles at Clang O0 and O3
with ASan/UBSan. They exercise held-clock updates, initialization, slice bounds,
observable intermediates, effect/budget fallback, multiple predicate sources,
shared predicates, full domains, spare storage bits and 64-bit interval ends.
Default/off identity, attribute precedence, fullpass behavior and execution-plan
accounting are checked separately.

These mechanisms can apply to XiangShan's wide CSR views and local decode
consumers under the same proofs. They neither cross partitions nor assume that
a source or branch remains inactive in other workloads. Reduced preparation or
fewer predicate expressions alone is not a performance guarantee.
