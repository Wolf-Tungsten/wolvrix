# Sparse equality families across region composition

`shared_equality_families` / `WOLVRIX_GRHSIM_SHARED_EQUALITY_FAMILIES` is an
experimental, default-off extension of [shared equality decoding](grhsim-shared-equality-decode.md).
The emit attribute takes precedence over the environment. The parent
`shared_equality_decode=0` disables both paths.

Region fusion can make an address source local, while its predicates retain
independent persistent slots. Consumer rewrites can also interleave comparisons
with other operations. The original contiguous-run matcher cannot retain reuse
in these cases. This extension groups eligible predicates by their source SSA
value within one pure compute supernode.

## Maintenance point and preserved behavior

For example, `kEq` has operands `[source, constant]` (either order is accepted)
and one one-bit result. A region may contain:

```text
source = slice(instruction, 6:0)
p19 = source == 19
a = p19 xor noise
p23 = source == 23
...
```

The source is computed at its original position. At the first eligible equality,
the emitter maintains all selected predicate slots:

```text
first execution: compute all selected predicates, compare/store/notify
source unchanged: reuse all selected predicate slots
source changed: clear the old matching slot and set the new matching slot
```

Consumers, including the interleaved `a`, execute at their original positions.
Later selected equalities emit no duplicate computation. Other operations are
not reordered. Each family owns one previous-address/valid cache, not a one-hot
array. Every changed result uses its original scalar change and activation logic.
`init()` invalidates the cache; normal and fullpass bodies share it while retaining
their respective notification policies.

## Proof and fallback

- The whole region must be pure except for ordinary register reads. Effects,
  transparent storage, memory accesses and reg-to-mem intents reject the family
  extension for that region. Existing contiguous-run handling remains available.
- Sources have Logic width 2–12 and are not events. An internal producer must
  precede the first comparison. External sources require an input or persistent
  value. Aliased materialized sources are rejected.
- Each family contains 16–256 distinct constants of the same width as the source,
  with no unknown bits. Duplicate or oversized families fall back; there is no
  profile-based subset selection.
- Every selected result has one independent canonical Boolean slot, is not an
  event or direct state view, and has no alternative slot owner. Local-only
  results remain on their existing path.
- Local compute reordering, priority-mux chains, Boolean producer reductions and
  Boolean zero-group guards cause the shared decoder paths to fall back. Private
  conditional-mux cones can coexist because stored predicates cannot be absorbed
  as private producers.

SSA dependencies ensure that no consumer of a later predicate precedes its
definition. Moving its pure calculation to the first comparison therefore makes
the same value available earlier without changing any earlier consumer. The
source has already been computed and cannot change during this region. Exclusive
result storage preserves cache validity across executions; an interleaved local
consumer reads the updated slot. No callback or transparent storage observes
intermediate writes within the accepted region.

This does not change region membership, value fanout, state-read activation or
clock/commit order. It does not restore the finer activation conditions that
region fusion removed, cache local-only consumers, or construct a compact
decoder output word. A source check and sparse dispatch still have runtime cost.

## Validation and accounting

`emit-grhsim-cpp-shared-equality-families` checks interleaved consumers, internal
sources dependent on clocked state, input sources, two interleaved source families,
unmatched and repeated addresses, spare storage bits, reinitialization and input
changes with clocks held. Small/duplicate families and effect-containing regions
fall back. Default/off identity, parent disable, environment precedence, split
emission, fullpass, shared notification and conditional-mux modes are covered.
Generated models run against a separate oracle at O0/O3 with ASan/UBSan. The
original shared/local equality suites cover their existing boundaries.

The execution plan counts `shared_equality_family_groups` and
`shared_equality_family_predicates`, and includes them in shared decoder totals.
It marks address-dependent update costs as unmodeled. Dynamic diagnosis should
count source visits and changes separately from predicate comparisons, and verify
both per-result changes and region execution counts. Fewer comparisons do not
establish a walltime improvement.

The rule is independent of CV32E40P signal names. Narrow CSR or enum families in
XiangShan may qualify, but ownership, effects and partition boundaries still
apply. There is no measured XiangShan benefit for this extension.
