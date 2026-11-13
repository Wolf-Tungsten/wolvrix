# Wide local concat reductions

The `grhsim-cpp` attribute `wide_concat_reduction=1` (environment variable
`WOLVRIX_GRHSIM_WIDE_CONCAT_REDUCTION=1`) extends the existing
`scalar_concat_reduction=1` rule to concat results wider than 64 bits. The wide
extension is enabled by default when the parent rule is enabled; disabling the
parent rule disables both widths. Set `wide_concat_reduction=0` or its environment
variable to `0` to retain the original wide lowering. Explicit attributes take
precedence over environment variables. The standard `config/grhsim32.json`
preset also records the enabled setting. This extension is independent of shared
Boolean input masks.

For a concat of 86 Boolean predicates used only by OR/NOR reductions, the emitter
evaluates a Boolean OR at the original concat position instead of constructing
two words and immediately reducing them. For mixed-width scalar fields, every
field is truncated to its declared width before aggregation. AND/NAND use the
OR of each field's missing bits; OR/NOR use the OR of its set bits. The consumer
applies the appropriate polarity. A 64-bit field uses an all-ones mask without
shifting by 64.

The concat must be a local untracked result. Each operand must be a non-event
logic value of 1–64 bits. Every user must be a later unary reduction in the same
compute scope, belonging to one family: AND/NAND or OR/NOR. Numeric users,
mixed families, parity, external/tracked results, side-effecting operations,
wide input fields and cross-scope uses keep the existing lowering. Inputs may be
local expressions; they are evaluated at the original concat position, and the
Boolean snapshot is not deferred to its consumers. This preserves intervening
stores and the original activation boundaries.

The rule changes only local C++ representation. It does not change the GRH,
model layout, outputs, change detection, state or scheduling. Static execution
plans report `wide.concat_reduction` and `wide.concat_reduced_consumer` rather
than charging the eliminated wide concat and reduce helpers.

The focused test is `emit-grhsim-cpp-wide-concat-reduction`, selected directly
with `WOLVRIX_TEST_WIDE_CONCAT_REDUCTION=1`. It covers 65/86/110/111/638-bit
Boolean concats, repeated fields, mixed signed scalar fields including 64-bit
inputs, both reduction polarities, numeric/mixed/parity/tracked/cross-scope
fallbacks, an unsupported 65-bit input field, explicit effect annotations,
default/on identity, explicit off and attribute/environment controls, and parent-off isolation.
An independent bit-by-bit reference checks directed patterns and 12,000 fixed
seed vectors with repeated eval, at Clang `-O0` and `-O3` under ASan/UBSan.
