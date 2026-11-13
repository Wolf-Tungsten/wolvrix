# Shared Boolean input masks

The `grhsim-cpp` attribute `scalar_concat_reduction_masks=1` (environment:
`WOLVRIX_GRHSIM_SCALAR_CONCAT_REDUCTION_MASKS=1`) shares an input word across
repeated concat reductions in one compute supernode. It is enabled by default
when the parent rule `scalar_concat_reduction=1` is enabled. The parent rule's
default is unchanged. Set the attribute or environment variable to `0` to
disable mask sharing; an explicit attribute takes precedence over the environment.
The standard `config/grhsim32.json` preset records the enabled setting explicitly.
For that pipeline, set its `options.WOLVRIX_GRHSIM_SCALAR_CONCAT_REDUCTION_MASKS`
entry to `"0"` for an off/on comparison.

For example, a concat of Boolean inputs `a, b, d` followed by AND reduction can
use `(inputs & 0xb) == 0xb`, where `inputs` contains `a, b, c, d` in bits 0–3.
OR reduction uses `(inputs & 0xb) != 0`. NAND/NOR invert the corresponding result.
Repeated operands are idempotent for these reductions; the input word uses each
value once. Bit 63 is handled with an unsigned 64-bit shift.

The rule accepts at least 16 distinct predicates, each containing at least 16 one-bit
fields, with at most 64 distinct inputs across the accepted family. Inputs must
be ports or materialized Boolean slots that have no writer in the supernode;
both canonical value identity and allocated slot identity are checked. Direct
state views, local definitions, event values, and predicates after unknown
effects are excluded. The capture happens inside the original activity guard.

Distinct predicates are counted by the input mask and reduction family, not by
the number of concat operations. Repeated fields and reordered concatenations
can describe the same predicate, which the host compiler already shares without
input packing. Families with fewer than 16 different predicates retain the
original path. Scopes with fewer than 16 potentially eligible concat operations
are skipped before constructing the detailed plan.

The existing concat reduction proof still requires all concat users to occur
later in the same compute scope and to belong to the same reduction family
(AND/NAND or OR/NOR). Numeric uses, parity, mixed families and tracked concat
results retain their existing lowering. Mask sharing does not cover wider
families; [wide local concat fusion](grhsim-wide-concat-reductions.md) is a
separate rule for reduction-only wide results. All output stores,
change detection, assertions, state, and scheduling remain in place. The rule
does not inspect hierarchy names or instruction opcodes, and changes no GRH IR.

Static work reports include `scalar.concat_reduction_mask`,
`scalar.concat_reduction_input_packs`, and `scalar.concat_reduction_input_bits`.
The focused emitter test is selected with
`WOLVRIX_TEST_SCALAR_CONCAT_REDUCTION_MASKS=1`; it checks default/on source
identity, explicit disabling and attribute/environment precedence, disabled-parent
isolation, the 15/16 distinct-predicate boundary, repeated operands, 64/65 input
limits, all-zero/all-one and walking-bit cases, deterministic random inputs,
and repeated eval with Clang at `-O0`/`-O3` under ASan/UBSan.
