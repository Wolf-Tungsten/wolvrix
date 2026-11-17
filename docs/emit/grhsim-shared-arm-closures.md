# Shared scalar computations within an existing mux arm

`conditional_mux_shared_arm` / `WOLVRIX_GRHSIM_CONDITIONAL_MUX_SHARED_ARM`
extends an existing [conditional mux cone](grhsim-conditional-mux-cones.md).
It defaults to off and requires `conditional_mux_cones`. Selection uses only
the scheduled graph and existing emitter ownership; no runtime profile is read.

For example, `a = f(x); b = g(a); c = h(a); mux(sel, b ^ c, other)` previously
left `a` eager because it has two uses. If both uses belong to the same existing
true arm, the extension moves `a` and its eligible ancestry into that arm,
emitting each shared result once. It does not duplicate `a` in both branches.

The original roots and arms are planned first with their existing 16–256
operation limits. Only nonempty arms can grow. Backward traversal constructs
a closure of eligible local scalar producers, then reverse topological pruning
removes every new producer with a scheduled use outside that exact arm. Uses
as the selector, in the other arm, or elsewhere keep a value eager. The selected
root's matching data operand is the only allowed use outside the closure.
Duplicate operands count as distinct uses; dead unscheduled IR users do not.

The closure inherits the original purity, single canonical producer, no storage
alias read-before-write, same-supernode, non-observable, non-materialized and
non-tracked requirements. Existing covered state views, shared/local equality
caches, packed reductions, prefix caches and indexed concat lowerings keep their
ownership. Additional operations are scalar only. Input/change-cache roots and
their keys/results are protected; cache plans are always built from the original
cones. Roots and already owned arms cannot be absorbed into another arm.

If a complete expanded arm exceeds the original 256-operation upper bound, its
original arm is retained. There is no partial selection by frequency or measured
cost. Accepted additions retain the original topological order. The ordinary
scalar renderer preserves widths, signed shifts and truncation; multiply used
locals cannot be substituted into multiple consumers. Computation stays in the
same compute region and state snapshot; no activation edge, persistent slot,
cache site or commit operation is changed.

`conditional_mux_shared_arm_audit` /
`WOLVRIX_GRHSIM_CONDITIONAL_MUX_SHARED_ARM_AUDIT` defaults to off. With the
optimization disabled it reports the same candidate membership in C++ comments
while preserving all original executable source. Comments report original arm
size, backward-reachable additions, outside-use rejections, accepted operation
IDs, shared results, cached-root fallback and oversized-closure fallback. These
are static counts, not executed work or a predicted speedup.

`emit-grhsim-shared-arm` checks audit/source identity, audit/implementation
membership, repeated operands, external/cross-arm/selector uses, public and
cross-region observations, cache and size fallbacks. Independent O0 and O3
ASan/UBSan oracles exercise 17/32/64-bit values, signed and oversized shifts,
two clocks, masked writes, asynchronous reset, input-only changes and repeated
evaluation. Options also have default-off and environment/attribute precedence
checks. The whole-model experiment separately verifies unchanged original roots,
cache/activation interfaces and exact source identity of the audit control.
