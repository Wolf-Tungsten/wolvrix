# Shared Boolean input masks

The `grhsim-cpp` attribute `scalar_concat_reduction_masks=1` (environment:
`WOLVRIX_GRHSIM_SCALAR_CONCAT_REDUCTION_MASKS=1`) shares an input word across
repeated concat reductions in one compute supernode. The child rule and the
parent `scalar_concat_reduction` rule now default to enabled. Explicitly set
either the parent or child attribute/environment variable to `0` to disable it;
an explicit child attribute takes precedence over its environment variable. The
standard `config/grhsim32.json` preset records both settings explicitly. To turn
the rule off in that pipeline, set the parent or child key under `options` to
`"0"`.

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

The parent-rule default was enabled after an after/DefaultConfig single-model
GrhSIM comparison using an identical frozen graph, DiffTest traces, and Clang
23.1.2 build flags. On node030 CPU96, 8 paired runs measured 9.98% lower Host
time for default-on versus explicit-off (95% CI [-10.23%, -9.74%]); see
[`TNO0072`](../../../pdocs/kunminghu_v3_grhsim_merge_20261110/TNO0072_scalar_concat_reduction_parent_default_20261114.md).
