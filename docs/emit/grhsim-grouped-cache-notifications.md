# Grouping change-cache notifications at producer region exits

`conditional_mux_change_grouped` /
`WOLVRIX_GRHSIM_CONDITIONAL_MUX_CHANGE_GROUPED` is experimental and defaults to
off. The attribute overrides the environment. It requires the existing
[change cache](grhsim-conditional-mux-change-cache.md) and its conditional-mux
parent. It does not change cache eligibility, sources, result storage, region
membership, input checks or state-commit invalidation.

## Rule

For each producing compute supernode, collect the exact scalar value changes
that invalidate each cache. Where at least two sources share a target cache,
accumulate their change predicates locally and update its persistent dirty bit
once at the region exit. Two targets with identical source sets share the same
local accumulator. No runtime profile or cache hit frequency selects targets.

```text
before:
    dirty[target] |= changed(a)
    ...
    dirty[target] |= changed(b)

after:
    local_changed = false
    local_changed |= changed(a)
    ...
    local_changed |= changed(b)
    region exit: dirty[target] |= local_changed
```

If an existing ordinary deferred-activation predicate has exactly the same
source set, reuse it at the exit instead of adding another reduction. Subsets
and supersets are not equivalent: they could respectively miss an invalidation
or change cache execution counts, so this rule does not reuse them. Packed and
direct-accumulator activation representations fall back to a dedicated local
predicate.

Some lowering paths report a change immediately rather than through deferred
activation. Even for a reused predicate, those changes go into a separate local
fallback accumulator. The final cache update uses the OR of both. It never
modifies the existing scheduler predicate just to satisfy the cache, preserving
the scheduler's notification behavior. Unused fallback locals can be removed
by the host compiler.

## Ordering and lifetime

- If a cache consumer is in the producing region, all notifications to that
  consumer stay immediate. The consumer might run before the region exit.
- Cross-region consumers execute only after the producing region exits; the
  exit flush precedes further dispatch, including another node in the same batch.
- The existing dirty bit remains sticky until computation. Local accumulators
  start false on each region execution. Initialization still marks all caches.
- Original source ownership, scalar purity, aliases and cache safety proofs
  remain prerequisites. Single-source, input and state-commit paths keep their
  existing behavior. Fullpass already disables the underlying change cache.
- Operation handles must outlive spans returned by `results()` while building
  the plan; copying a span out of a temporary operation is invalid.

## Validation and cost

`emit-grhsim-mux-grouped-notify` covers a shared two-source producer feeding
different clock domains, exact deferred-predicate reuse, held clocks, isolated
input changes, simultaneous and independent edges, reinitialization and
default/off/parent/environment behavior. Generated models are compared with an
independent oracle at O0 and O3 with ASan/UBSan. The underlying conditional-mux,
exact-key-cache and change-cache suites remain applicable.

Counts must report local accumulation separately from persistent dirty updates.
Cache visits, misses, root executions, result changes and scheduler counts must
remain identical to the ungrouped cache. Fewer stores do not establish a speedup:
local reductions still cost instructions and may affect register allocation.
The CV32E40P experiment uses one frozen rule and also retains the previously
accepted baseline as a separate control.

The rule is graph based and may apply to XiangShan. Identical notification sets
and cross-region ordering must be proven there as well; no XiangShan performance
result is implied by this implementation.

The optional [joint notification factoring](grhsim-joint-cache-notifications.md)
shares exact overlapping source subsets with ordinary scheduler predicates.
Its static cost gate includes target reconstruction and uses no runtime data.
