# Aligned scalar array-element reads

`aligned_array_slice=1`, or `WOLVRIX_GRHSIM_ALIGNED_ARRAY_SLICE=1`, specializes
ordinary word-backed `kSliceArray` reads. The option defaults to off; an explicit
emit attribute takes precedence over the environment.

`kSliceArray` has two operands: the packed source bits and an element index.
Its `sliceWidth` attribute is the stride in bits between elements. This rule
requires that stride to equal the scalar result width and divide 64, so the
eligible widths are 1, 2, 4, 8, 16, 32 and 64 bits. The source must be wider than
64 bits and the index must be scalar. Other widths, wide indices/results and
ordinary bit-offset `kSliceDynamic` retain their previous lowering.

For example, a 128-bit source containing 32 four-bit elements has
`start = index * 4`. Every element is contained in one 64-bit source word:
`(start % 64) + 4 <= 64`. The generated helper reads that word, shifts and masks
the result. It does not fetch the next word or combine a pair of words.

Existing packed-lane and reg-to-memory intent specializations retain their
precedence. This rule uses the element semantics even when concat simplification
has made the operand list incompatible with the packed-lane representation.
It does not reconstruct a byte-lane array or change the source's construction,
storage, layout, schedule, snapshot point, change detection or notifications.

The emitter preserves the previous index multiplication and `size_t` conversion
expression exactly, including its existing unsigned wrap behavior. The helper
checks the selected word against the original rounded source storage extent;
out-of-storage reads return zero. It also retains the existing source padding
and scalar result truncation behavior. This is not a change to index arithmetic
or an attempt to repair other indexing semantics. Because every eligible width
divides the unsigned machine modulus, wrapping a defined unsigned multiplication
does not invalidate the word-alignment proof.

The scalar helper is always inlined and uses the existing `_words` expression
category. The execution planner reports `wide.aligned_array_slice` and one
potential source-word load. It does not claim a new runtime operation count or
modify batch-size estimates. No model name, trace, profile or measured timing
enters the decision.

`emit-grhsim-aligned-array-slice` compares off/on output with an independent
bit-extraction oracle at O0 and O3 with ASan/UBSan. It covers every eligible width,
non-divisor and wide-result fallbacks, a mixed 16 x 4 + 64-bit concat carrying
32 x 4 lane metadata, separate producer/consumer groups, non-word-sized source
padding, signed results, repeated evaluation and input-only changes. Index cases
include out-of-range values, spare port-storage bits, large unsigned indices
and the existing wrap behavior. Default/off and environment/attribute precedence
are checked, as is unchanged model storage layout.
