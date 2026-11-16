# Joint factoring of scheduler and cache notifications

`conditional_mux_change_joint` /
`WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_JOINT` defaults to **off**. An explicit
attribute overrides the environment. It requires the existing
[grouped cache notifications](grhsim-grouped-cache-notifications.md), change
cache and conditional-mux parent. Cache selection, source dependencies, result
storage, region membership and persistent dirty semantics remain unchanged.

## Static rule

For a producer region, regard each ordinary deferred scheduler predicate and
each non-reused cache predicate as a destination. Give each eligible source the
exact list of destinations that use its change predicate. Sources with the
same destination list form a disjoint bucket. Accumulate each source once in
its bucket, then reconstruct each destination by ORing its buckets at the
region exit.

For example, a scheduler predicate needs `{a,b,c,d,e}` and a cache predicate
needs `{a,b,c,d}`. The buckets are `{a,b,c,d}` and `{e}`:

```text
common = changed(a) | changed(b) | changed(c) | changed(d)
private = changed(e)
scheduler_changed = common | private
cache_changed = common
```

A change to `e` must not invalidate the cache. Subsets and supersets are not
interchangeable; target reconstruction must reproduce each exact source set.
The plan never reuses a broader scheduler predicate as a cache predicate.

The static cost gate counts source-to-destination OR contributions before
factoring, and source-to-bucket plus bucket-to-destination contributions after
factoring. Factoring is enabled only when the latter is strictly smaller.
Fixed bounds are 64 destinations, 1,024 eligible sources and 64 buckets per
producer. These are compile-time structural rules. The planner does not read
execution counts, profile files, test workload results or timing measurements.

## Composition and correctness

Eligible sources are canonical materialized scalar logic values with tracked
changes, pure producers and no event, state-view or inverted-state-view alias.
State commits and external input notifications retain the original paths.
Grouping still keeps all same-region cache consumers on immediate notification.

The joint buckets receive changes only on the existing deferred-notification
path. Immediate notification paths keep their original direct scheduler update
and cache fallback accumulator. This avoids introducing duplicate activations
or silently removing changes from lowering paths that do not use deferred
activation. Unselected sources retain their original contributions.

Reconstruction happens after the producer body and before predicate
materialization, activation flush or any subsequent region dispatch. Original
predicate locals also retain unsupported-path contributions; reconstruction
ORs into them. Dirty state remains sticky across region visits and is cleared
only when its consumer computes.

Packed/direct deferred representations, active shared-cohort plans and strict
activation overlays retain their previous implementation. Existing exact
deferred-predicate reuse remains handled by the grouped notification layer.
The joint planner does not alter that mechanism.

## Validation

`emit-grhsim-mux-joint-notify` includes independent clocks, simultaneous edges,
held-clock changes and reinitialization. Its scheduler-only source is changed
separately while checking that cache misses do not increase. Generated models
run against an independent oracle at O0 and O3 with ASan/UBSan. It also checks
default/off source identity, environment precedence, parent/grouping disable
and fallback when static contributions do not decrease.

The CV32E40P comparison independently reconstructs the original and factored
source sets from generated code before compiling the candidate. All consumers,
cache visits/misses, dirty predicates and scheduler execution counts must match
the grouped baseline. Diagnostic counters include both source reductions and
exit reconstruction. IR contributions are not machine instructions, and fewer
contributions do not by themselves imply lower walltime or fewer spills.

No measured result feeds a subsequent planner decision, site selection or
threshold adjustment. One frozen candidate is evaluated against the preceding
grouped implementation, the retained baseline and Verilator. This feature
remains experimental and is not a production default.

The mechanism uses graph semantics and may apply to XiangShan notification
fanout, but that requires its own structural proof and validation. No XiangShan
performance result follows from CV32E40P measurements.
