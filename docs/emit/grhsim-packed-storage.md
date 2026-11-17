# Packed state layout and private array lifetimes

Four default-off emitter options preserve useful data layout and reduce
temporary-array work when activity scheduling combines compute regions. None
reads execution profiles or changes activity-region membership.

## State layout

`state_layout_packed_access=1`, or
`WOLVRIX_GRHSIM_STATE_LAYOUT_PACKED_ACCESS=1`, keeps scalar state fields in the
lane order of an existing packed-array view. An explicit emitter attribute
takes precedence over the environment variable.

The existing state-storage ordering first compares reference-demand classes,
exact reader/writer reference-unit signatures and demand. The new rule changes
only the final graph-order tie-break. A bundle must contain distinct scalar
states of the same storage type and element width, with identical nonempty
reference signatures and demand. A lane must directly read its state; both
cross-region direct reads and ordinary nonmaterialized register/latch reads are
recognized. Repeated lanes, padding, other expressions and incompatible
signatures fall back to the existing order.

Bundles are considered in graph operation order. The first disjoint bundle is
retained when views overlap. Its position is the earliest original graph
position of its members; its internal order is the packed view's lane order.
Unbundled states retain their original relative order. There is no frequency
estimate or tuned threshold.

For example, combining regions can make the reference signatures of two fields
equal, leaving graph order `a[0], b[0], a[1], b[1], ...`. A view that consumes all
of `a` can retain `a[0], a[1], ...` contiguously, allowing the host compiler to
load consecutive fields together. State identity, field names, operations and
notifications remain unchanged; only declarations within state storage move.

The emitter prints `GRHSIM_PACKED_STATE_LAYOUT` counts for accepted bundles,
states, incompatible views and overlapping views when this option is enabled.

## Private array scope

`packed_array_local_scope=1`, or
`WOLVRIX_GRHSIM_PACKED_ARRAY_LOCAL_SCOPE=1`, gives a private packed array a
lexical lifetime ending immediately after its sole scalar selection.

For a packed concat `a = {lanes...}` and a later array slice `r = a[index]`, the
generated code constructs `a` and selects one lane inside an immediately
invoked lambda at the original concat position. It keeps only the scalar
selection after that scope. Assignment of `r`, its truncation/sign handling,
change detection and activation remain at the original slice position.

This requires all of the following:

- The packed view is local, non-event and has exactly one supported scalar
  array-slice user later in the same pure compute region.
- The index is an unchanged public input or externally stored scalar. Local
  definitions, direct state aliases, event inputs and writes to the same
  canonical value or physical storage cause fallback.
- Concat, optional view wrapper and slice are outside absorbed conditional,
  priority, predicate and boolean-reduction plans.
- The region has no commit, state-write bypass, behavior compute group or
  side-effecting operation.

Construction still reads lanes at the original point. Only selection moves
earlier under the immutable-index proof. Out-of-range indices still select
zero. Observable or persistent arrays, multiple users and locally computed
indices retain the existing code. This rule does not merge equivalent views
or enable direct indexed state reads.

## Complete state-array overwrites

`packed_array_no_init=1`, or `WOLVRIX_GRHSIM_PACKED_ARRAY_NO_INIT=1`, omits the
zero initialization before a complete packed-lane overwrite. Local arrays are
declared without an initializer; persistent arrays retain their storage but
skip the assignment of an empty array before their lane writes.

The rule requires a complete, one-to-one destination lane mapping and direct
scalar register/latch reads of exactly the element width and storage type.
Source state storage is separate from the packed destination. Materialized
read snapshots (unless already proven direct state reads), reg-to-memory intent
storage, other expressions, effects and mismatched types fall back. Repeated
source states are allowed; every destination lane is still fully assigned.
Reads and writes remain at their original concat position, and result changes
and notifications are unchanged. This is a conservative direct-state rule,
not a blanket removal of initialization from arbitrary arrays.

## Private selected state reads

`packed_array_state_select=1`, or
`WOLVRIX_GRHSIM_PACKED_ARRAY_STATE_SELECT=1`, replaces a private packed state
array and its single `kSliceArray` use with a scalar state selection. The same
pure-region, unchanged-index, physical-alias and absorbed-plan exclusions used
by private array scope apply. Every lane must additionally satisfy the direct
state-source proof above. Observable, persistent or multi-user arrays retain
their original representation.

At the original concat point, the emitter selects a typed pointer to a member
of `state_logic_storage_t` from a static table, reads that state and truncates
it to the element width. It carries this scalar snapshot to the original slice
point, where ordinary result assignment and activation take place. Independent
struct fields are never treated as a C++ array via pointer arithmetic.

For example, if lanes 0–159 read distinct states and lanes 160–255 repeat lane
0, the table contains the first 160 members. Valid indices in the repeated
suffix select member 0. Indices outside the original 256-lane view return zero;
repeated padding is not treated as zero. The exact suffix is derived from
member identity, without a size threshold or execution-frequency estimate.

When both selected reads and local scope are enabled, a proven selected read
takes precedence; remaining eligible arrays retain the scoped construction.
No-init independently applies to remaining direct-state constructions. State
layout can be combined with either rule without changing the pointer-to-member
semantics or introducing a new physical state alias.

## Validation

`emit-grhsim-packed-state-layout` checks physical lane offsets as well as
functional behavior; `emit-grhsim-packed-array-scope` checks selection lifetime
and conservative fallbacks. Both compare default/off/attribute/environment
settings. Generated-model oracles exercise two clocks, asynchronous reset,
masked writes, repeated evaluation, input-only index changes and out-of-range
indices at O0 and O3 with address/undefined-behavior sanitizers. Array tests
include 8-, 17- and 64-bit elements.

`emit-grhsim-packed-array-no-init` covers local and cross-region persistent
arrays, repeated sources and conservative fallbacks. The
`emit-grhsim-packed-array-state-select` oracle covers ordinary/stored indices,
repeated trailing states, signed element widths, masking, independent clocks,
reset and repeated evaluation, plus the fixed combination with layout/no-init/
scope. Both check default-off and attribute/environment precedence; generated
oracles run at O0 and O3, with ASan/UBSan at O3.

Performance comparisons must declare independent options and any combined
configuration before measurement, then retain source/ELF identities. A model
with no eligible packed views should reproduce the previous generated source
exactly; timing variation in that case is not
an optimization benefit.

When comparing across the accompanying graph-view ownership fix, use a separate
control with that fix and both storage options disabled. `Value::users()` and
`Operation::operands()/results()` return spans owned by their respective wrapper
objects. Keep the returned `Value` or `Operation` alive while traversing its
span; a range-for over `graph.getValue(id).users()` outlives the temporary in
C++20. The fixed scheduler/emitter binds these objects before traversal. This
correctness repair can change previously unstable local scheduling order and
must not be attributed to either storage optimization.
