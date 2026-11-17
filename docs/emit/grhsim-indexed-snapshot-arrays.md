# Compact snapshots for indexed local concat reads

`indexed_concat_snapshot_array` /
`WOLVRIX_GRHSIM_INDEXED_CONCAT_SNAPSHOT_ARRAY` enables compact snapshot arrays
by default. Set the emit attribute to `0`, or the environment variable to `0`,
to retain the original word-reader closure. Explicit attributes override the
environment. It requires an existing
[indexed concat slice](grhsim-indexed-concat-slices.md), retaining that lowering's
source ownership, snapshot lifetime, intervening-operation and result-use proof.

A word-reader closure can capture a separate pointer for each named scalar
snapshot. For example, 256 Boolean snapshots can create 2048 bytes of pointer
captures on a 64-bit host even when the result reads a single bit. The option
stores these snapshots as `std::array<bool, 256>` at the original concat point.
A one-bit result then reads one element; no word-reader closure is needed.

Eligibility is structural:

- All concat operands are scalar Logic values with the same width (1–64 bits).
- Every nonconstant operand is already preserved as a local scalar snapshot.
  Persistent fields, top-level inputs and covered state views keep the existing
  lazy reader; the option does not add eager loads of those fields.
- The array's native lane storage must be no larger than one host pointer per
  distinct nonconstant snapshot. This excludes large repeated-field concats,
  where a small closure already reuses a few values. Constants are included in
  the array cost but do not increase the reference cost.

No site names, model names or runtime data participate. Native lane widths are
8/16/32/64 bits, with Boolean lanes represented by ordinary `std::array<bool>`
(the standard array template, not `std::vector<bool>`). Values are masked before
storage. Concat operands are reversed so element zero contains the least
significant source field. The slice index remains an unsigned bit offset.

The helper first performs the existing capped scalar/wide-index check. When the
result fits one lane, it reads one lane and, only when necessary, the next one;
unaligned reads can straddle fields. Wider results gather bounded fragments from
successive lanes. Missing source bits are zero, signed lanes retain their bit
patterns, and no shift by 64 is evaluated. Result change detection, activation,
state layout, caches and conditional-mux plans remain unchanged.

This does not eliminate the original scalar producers or defer them across a
state write. It replaces closure representation and result extraction. A more
compact object or fewer instructions at a selected site alone does not establish
whole-model performance benefit.

`emit-grhsim-cpp-indexed-snapshot-array` extends the independent bit-by-bit oracle
of the original lowering, with uniform 1/3/7/8/16/29/32/63/64-bit fields and
1/32/64-bit results. Tests include 129-bit indices, noncanonical scalar padding,
source-end crossings, out-of-range values, repeated operands, mixed fields,
public/multiple/cross-region users, effects, clocked register writes, repeated
evaluation and initialization. Default/on source identity, both directions of
explicit/environment precedence, disabling the parent indexed-concat lowering,
planner coverage, helper splitting and runtime-profile generation are checked;
O0/O3 generated models run under ASan/UBSan.
