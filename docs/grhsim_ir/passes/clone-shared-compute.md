# Shared scalar compute localization

`grhsim.clone-shared-compute` runs after `grhsim.canonicalize-compute` and before
CPU partitioning. It trades a small amount of repeated arithmetic for fewer
shared intermediate values. Reverse-topological cone absorption otherwise stops
at a producer whose consumers have different owners; its result then needs a
boundary slot, a change comparison, and consumer notifications.

The pass accepts single-result, two-state scalar bijections with no parameters
or object references. For a varying operand `x` and a constant operand `c`:

| Operation | Operands and result | Type restriction |
| --- | --- | --- |
| `core.compute.not` | `x` → bitwise inversion of `x` | Same type, width 1–64 |
| `core.compute.logicNot` | `x` → Boolean zero test of `x` | Both widths exactly 1 |
| `core.compute.xor` | `x, c` or `c, x` → bitwise XOR | All the same type, width 1–64 |
| `core.compute.add` | `x, c` or `c, x` → sum modulo 2^width | All the same type, width 1–64 |
| `core.compute.sub` | `x, c` → difference; `c, x` → reversed difference | All the same type, width 1–64 |

Exactly one binary operand must have a `core.compute.constant` producer. The
varying source must already have at least two distinct consumers. These functions
are bijections on normalized bit patterns: `f(x)` changes if and only if `x`
changes. Their comparisons cannot filter any source transitions. Source sharing
also avoids simply moving a boundary upstream to a previously local value.
Multi-bit logical negation, narrowing and two-varying-input arithmetic do not
have these properties and are excluded. The transformation uses types, operation
semantics and use counts; names and origins are metadata only.

No opcode, operand ordering or type conversion changes:

```text
x = state.read(q)
t = not(x)
y = and(t, a)
z = or(t, b)
raw = output(x)

=> x = state.read(q)
   ty = not(x); y = and(ty, a)
   tz = not(x); z = or(tz, b)
   raw = output(x)
```

Each distinct compute consumer gets one clone, even when it uses the result in
multiple operand positions. Output, commit, event, memory and external-call
consumers continue using the original producer. For example,
`regWrite(enable, t, mask, clock)` keeps `t` and its pre-commit snapshot; these
four operands are the enable, data, write mask and event value. Only a root with
no remaining uses is removed. State objects, initialization and read operations
remain intact. Each clone depends on the same source values, so CPU mapping builds
ordinary dependency and notification edges for it without changing publication
or event-history semantics.

Options are `--max-fanout` (default 8, distinct consumers including non-compute
consumers) and `--max-clones` (default 250000, total new operations). Both require
positive integers. A producer with fewer than two consumers is skipped; a root
that would exceed the remaining clone budget is skipped as a whole. Candidate
chains are processed from consumers toward producers. For example, when shared
`t = not(x)` feeds shared `u = add(t, c)`, the pass first clones `u`, then clones
`t` into the new live consumers. It tracks current users, skips deleted roots,
and rechecks fanout/budget after expansion. Candidate cycles remain unchanged.
Only roots made dead by this pass are removed; every rewrite is followed by pool
compaction, including when all original roots retain non-compute users.

Reads, wide/four-state values and parameterized operations are never cloned.
The pass is a `SemanticTransform`; the pass manager
invalidates existing backend mappings and increments the semantic revision.
It must follow CSE, since CSE would merge the clones back together.

Diagnostics report candidate roots, created clones, removed dead roots, roots
skipped by the current fanout/clone budget, and unprocessed cyclic candidates.
The default XiangShan and HDLBits IR pipelines
include the pass. XiangShan supports `XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE=0`
for a control run and `XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE_MAX_CLONES` for its
budget. The Python entry point also accepts `--no-clone-shared-compute`.
