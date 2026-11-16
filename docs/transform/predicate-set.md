# Predicate-set consumer fusion

`predicate-set -semantics 2state` replaces large disjunctions of same-source
equalities with exact unsigned interval membership. The pass is experimental and
is not added to `simplify` or any default pipeline. The default, including an
explicit `-semantics 4state`, leaves the graph unchanged. C++ callers opt in with
`PredicateSetOptions{.twoState = true}`.

## Representation

`kEq` has two operands, compared for equality, and one Boolean result. Their order
does not matter. `kConcat` lists its pieces from high to low; here all pieces must
be Boolean. `kReduceOr` consumes that concatenated value and returns whether any
bit is set. One-bit `kOr` and `kLogicOr` trees are handled as well. For example:

```text
reduce_or(concat(x == 16, x == 17, ..., x == 47, extra))
    -> (x >= 16 && x <= 47) || extra
```

The source is the exact same SSA value, unsigned Logic of width 2 through 64.
Constants must be known, unsigned, have the same width, and contain no X/Z.
Neither signed comparisons nor general four-state equality are approximated.
Opaque Boolean inputs remain in the disjunction. Multiple source families are
handled independently, so `x == c` and `y == c` are never conflated.

Each selected family's distinct constants are sorted and split into maximal
non-wrapping consecutive intervals. An interval uses unsigned `kGe` / `kLe` and
Boolean `kLogicAnd`; a singleton uses `kEq`. Intervals reaching zero or the maximum
value omit the redundant bound. A full-domain interval is constant true. The
construction performs no signed subtraction, does not overflow at `UINT64_MAX`,
and introduces no runtime lookup table or persistent cache. Identical new
constants, interval conditions and Boolean combinations are shared within a graph.

The fixed structural budget requires at least 16 distinct constants and at least
eight constants per interval on average. Traversal is bounded to 512 Boolean
leaves and 2,048 visits. These are compile-time eligibility limits, not parameters
selected from benchmark results. Sparse sets fall back to the original graph.

## Placement and preservation

Run this pass before `activity-schedule`, followed by normal dead-code elimination.
The original reduction result becomes a `kAssign` of the new membership result;
its value identity, public ports, declared name and existing users are preserved.
Original predicates and their other users are not rewritten or assumed dead.
Only DCE can remove operations that no longer have observable uses.

The pass does not move state, memory, event detection or effects. It only bypasses
pure Boolean OR/concat/reduction and known equality operations; operations marked
with effects are opaque. Reusing the same source SSA preserves its sampling
point. The scheduler subsequently derives new dependencies and activation from
the transformed graph. Applying this to an already scheduled graph without
rebuilding its schedule is unsupported.

Interval fusion reduces consumer expression work, but may add comparisons while
old predicates remain necessary elsewhere. Its new fanout can also change region
composition and the eligibility of other emitter optimizations. Operation count,
removed boundary count and source size alone do not establish a speedup.

## Validation and scope

`transform-predicate-set` compiles off/on models with an independent membership
oracle at O0 and O3 with ASan/UBSan. It covers the complete 12-bit source domain,
both residual Boolean values, a second permuted input domain, duplicate and
commuted predicates, independently observed predicate results, zero and 64-bit
maximum boundaries, and register behavior while clocks are held. Negative cases
cover default/four-state behavior, signed and over-64-bit sources, unknown
constants, sparse/small/oversized sets, effectful roots and cyclic Boolean cones.
Full-domain membership becoming constant true is also exercised. The transformed graph
must be idempotent after DCE; malformed CLI semantics are rejected.

The mechanism is independent of CPU or signal names. It can apply to XiangShan
CSR, opcode and control classification when sets are compressible, without
enumerating a 32-bit instruction domain. It does not interpret Chisel-specific
decode metadata. XiangShan performance has not been measured for this pass.
