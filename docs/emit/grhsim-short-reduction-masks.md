# Shared masks for short Boolean conditions

`short_concat_reduction_masks=1`, or
`WOLVRIX_GRHSIM_SHORT_CONCAT_REDUCTION_MASKS=1`, shares one input word across
dense short concat reductions in a compute supernode. It is enabled by default
when the parent `scalar_concat_reduction` rule is enabled, independently of the
long-field mask and wide-concat reduction switches. The parent rule now also
defaults to enabled. Set the parent or child attribute/environment variable to
`0` to disable it; an explicit child attribute takes precedence over its
environment variable. The standard preset records both settings explicitly.

Eligible concats contain 4–15 one-bit fields. A family must have at least 16
distinct predicates, at most 64 distinct stable inputs, and at least four times
as many input uses across distinct predicates as distinct inputs. Repeated
fields or reordered fields describe the same mask and do not inflate that
profitability count. A predicate is identified by its mask and AND/OR family.

Families with at least 128 distinct predicates retain the validated dense
path. Smaller families also need `C >= 2 * P + I`, where `P` is the number of
distinct predicates, `I` is the number of captured inputs, and `C` estimates
the two-input Boolean work after common-factor reuse. The estimate processes
AND and OR separately, greedily shares the most frequent pair, and counts
one operation for that pair plus the remaining operations. Ordered pair IDs
break ties deterministically. For example, `(a & b & c)` and `(a & b & d)`
cost three operations after sharing `a & b`, instead of four independently.
These are heuristic cost units, not measured host instructions or cycles.
The estimate only runs on 16–127 predicates over at most 64 inputs and does
not rewrite the Boolean expressions. This admits moderately sized complex
families while giving the ordinary lowering credit for existing reuse.

The input word is captured inside the original activity guard. Inputs must be
ports or materialized Boolean slots with no canonical or physical-slot writer
in that compute scope. Local definitions, event values, direct state views,
and candidates after unknown side effects are excluded. Each concat must be
untracked and local; all users must be later reductions in the same scope and
one family (AND/NAND or OR/NOR). Numeric, mixed-family, parity and cross-scope
uses keep the original lowering.

Unlike the long-field snapshot lowering, the short rule elides the original
concat and uses the immutable packed input word at each reduction. The compare
can remain inline with its Boolean consumers, avoiding a long-lived scalar
temporary for each cheap concat. AND uses `(word & mask) == mask`, OR uses
`(word & mask) != 0`, and NAND/NOR invert the predicate. The representation of
outputs and their change propagation is unchanged.

Static execution-plan counters are `scalar.short_reduction_input_packs`,
`scalar.short_reduction_input_bits`, `scalar.short_reduction_concat_elided`, and
`scalar.short_reduction_mask`. No graph operations or model fields are added.

The focused CTest is `emit-grhsim-cpp-short-concat-reduction-masks`, selected
directly with `WOLVRIX_TEST_SHORT_CONCAT_REDUCTION_MASKS=1`. It checks both
families and polarities, stable external inputs, internal writes, effects,
numeric/mixed-family uses, 64/65 input limits, factored-cost/dense paths around
127/128 predicates, accepted/rejected 32-predicate families, the distinct
predicate floor, rejected
3/16 fields, accepted 4/15 fields, repeated fields and predicates,
attribute/environment precedence and parent disable. Directed and fixed-seed
random inputs run repeated eval at Clang O0/O3 with ASan/UBSan.

For a default-policy-only change, `WOLVRIX_TEST_REDUCTION_SOURCE_ONLY=1` checks
all option combinations without rebuilding the runtime harnesses. Runtime
results can be reused only after comparing every generated C++ source and
header against the previously tested explicit modes. Normal CTest runs the
complete runtime checks.
