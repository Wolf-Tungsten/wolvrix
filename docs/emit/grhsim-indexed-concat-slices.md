# Indexed slices of local concats

GrhSIM enables `indexed_concat_slice` by default. Set the emit attribute to `0`
or `WOLVRIX_GRHSIM_INDEXED_CONCAT_SLICE=0` to retain the ordinary lowering.
The attribute takes precedence over the environment. This option is independent
of the concat reduction options.

## Operation and motivation

A lowered register bank can appear as a wide `kConcat` followed by a narrow
`kSliceDynamic`. For example, eight 32-bit registers form:

```text
packed = kConcat(r7, r6, r5, r4, r3, r2, r1, zero32)  // 256 bits
result = kSliceDynamic(packed, bit_index)              // sliceWidth = 32
```

Concat operand zero is the most significant field; the last operand supplies
the least significant bits. Dynamic slice operand zero is the packed source,
operand one is the unsigned bit offset, and the `sliceWidth` attribute is the
number of bits in the result. Thus `bit_index = 32` reads `r1`; unaligned
indices may span fields. This is a bit offset, not an array element index.

The ordinary lowering assembles the entire packed value before selecting the
result. The indexed lowering can emit a local word-reader lambda. Its switch evaluates
only the selected 64-bit word, or two words for a crossing slice. The runtime
helper combines those words and masks the result. Missing source bits are zero;
an index outside the source returns zero, including a wide index with nonzero
upper words. No shift by 64 is evaluated. Eligible equal-width local snapshots
use the default-enabled [compact array representation](grhsim-indexed-snapshot-arrays.md),
which reads the requested lanes directly.

Known wide constant fields are supported by the experimental
`indexed_concat_wide_constants` attribute, or
`WOLVRIX_GRHSIM_INDEXED_CONCAT_WIDE_CONSTANTS=1`. This extension defaults to
**off**; it also requires `indexed_concat_slice`. Explicit attributes override
the corresponding environment variables. CV32E40P's measured version reduced
source-level temporary arrays but did not improve walltime, so it is not part
of the default path.

For example,
`kConcat(zero1792, r7, r6, r5, r4, zero64, r1, r0)` has 2048 bits but only
six variable 32-bit fields. The emitter extracts constant fragments at generation
time and combines them with the corresponding scalar fragments in each selected
word. Padding-only zero words use the reader's zero default; no padding array is
captured or constructed by the indexed view. Nonzero constants, signed literal
extension, unaligned fields and reads across constant/scalar boundaries use the
same bit layout as the ordinary concat.

## Eligibility and snapshot rules

The emitter requires all of the following:

- At least 256 source bits, with scalar logic operands of 1–64 bits or, when
  explicitly enabled, known wide logic constants, whose widths sum exactly to the concat width. Wide
  constants must have a valid literal, no X/Z bits at their declared width,
  and no side effects. The threshold ensures the ordinary path
  assembles at least four words, while a scalar slice needs at most two.
- Exactly one complete IR use: a 1–64-bit `kSliceDynamic` later in the same
  compute supernode. Its declared slice width must match its result width.
- A local concat that is neither an event, persistent value, output nor a value
  requiring change tracking. Existing packed-array representations remain on
  their existing path.
- No intervening effect, event production, unsupported operation or write to a
  captured physical value slot. Alias identities and physical slot reuse are
  included in this check. Direct state views are conservatively excluded.

Local scalar operands retain named snapshots at their original definition
points, rather than deferring their expression trees. The slice result also
retains its original evaluation point, so it cannot be inlined past a later
write. The reader closure never escapes its compute scope. The default-enabled
[compact snapshot array](grhsim-indexed-snapshot-arrays.md) replaces this closure
for equal-width named scalar snapshots when its static storage cost fits. Persistent results
still use ordinary change detection and reader activation.

The IR, activity graph, commit ordering and fixed-point loop do not change.
Eligibility uses structure and lifetime proofs, without module names, profiles
or workload-specific values. Multi-user concats, nonconstant wider operands, escaping
values and uncertain lifetimes retain ordinary lowering. The execution planner
reports `wide.indexed_concat_view` and `scalar.indexed_concat_slice` shapes.

This does not reconstruct a state array or remove per-bit producer updates.
A bank flattened into Boolean slots still assembles the selected word from those
slots. Preserving arrays earlier in the pipeline is a separate transformation.

## Validation

`emit-grhsim-cpp-indexed-concat-slice` checks default/on/off and environment
precedence, split schedule helpers, runtime-profile generation, and generated
O0/O3 models with ASan/UBSan. Planner and emitter coverage must agree.
An independent bit-by-bit oracle covers mixed signed field widths, noncanonical
scalar padding, 1024 Boolean fields, unaligned and partial reads, 64-bit results,
129-bit indices, out-of-range indices and changing data at a fixed index. Wide
zero/nonzero padding, partial constant words, signed constant extension and
constant/scalar boundary crossings are checked against the same independent oracle.
Negative cases cover full outputs, multiple users, cross-supernode values,
effects, dynamic wide fields, X/Z padding and small concats.

A clocked eight-entry bank adds two read ports, an immutable zero entry,
one read port padded to 2048 bits, selected writes with write enable, repeated evaluation without an edge, and
reinitialization. Complete CV32E40P CoreMark bus-trace equivalence is additionally
checked by the playground fixture; its performance evidence is kept outside
this generic emitter documentation.
