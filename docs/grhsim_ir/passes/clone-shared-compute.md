# Boundary-aware shared compute cloning (B7)

`grhsim.clone-shared-compute` is the last rewrite pass of the semantic layer
(M5d-5, plan 归位决议 1). It clones cheap shared bijection producers per compute
consumer **only when the clone eliminates a predicted P_general supernode
boundary** — a boundary value costs a boundaryValueStore slot, a change
comparison and consumer activations, so deleting it is the win; the clones
themselves are absorbed into their consumer's node and never become boundaries.

Two hard constraints pin the pass position: it is a semantic rewrite, so it
must run before the single final CPU mapping (no "mapping → semantic rewrite →
mapping" round trip), and it must follow the last CSE-bearing simplify (B6,
`grhsim.simplify --scope phase`) or CSE would merge the clones back.

## Boundary prediction (shared helper)

The predicted boundaries come from `predictGeneralBoundaries`
(`include/grhsim/pass/general_boundaries.hpp`), a static simulation of
`cpu.st.build-general-nodes`' cone absorption that needs no CPU mapping:
absorbable ops (`core.compute.*`, `core.input.read`, `core.state.read`,
`core.state.memRead`) join the single agreed node of their results' users;
shared values and commit-boundary consumers force a fresh node; a General
value crossing two predicted nodes — or sampled by a `Mem`-phase write — is a
predicted boundary. The helper is shared with the C1 rework (M5d-6) so both
passes apply one rule; the Event/Output cones are self-contained by
construction (copy + strip), so no cross-partition shared compute exists and
only General-phase candidates are ever cloned. The node size cap mirrors
`--max-op-in-compute-node` (default 128).

## Candidates and gating

The candidate definition is unchanged: single-result two-state scalar
bijections with no parameters or object references —

| Operation | Operands and result | Type restriction |
| --- | --- | --- |
| `core.compute.not` | `x` → bitwise inversion of `x` | Same type, width 1–64 |
| `core.compute.logicNot` | `x` → Boolean zero test of `x` | Both widths exactly 1 |
| `core.compute.xor` | `x, c` or `c, x` → bitwise XOR | All the same type, width 1–64 |
| `core.compute.add` | `x, c` or `c, x` → sum modulo 2^width | All the same type, width 1–64 |
| `core.compute.sub` | `x, c` → difference; `c, x` → reversed difference | All the same type, width 1–64 |

Exactly one binary operand has a `core.compute.constant` producer, and the
varying source already has at least two distinct consumers (so deleting the
shared instance cannot merely move the boundary to a previously local input).
These functions are bijections on normalized bit patterns: `f(x)` changes if
and only if `x` changes, so the deleted boundary comparison cannot filter any
source transitions.

A candidate is cloned only when **all** hold:

1. the live consumer count is in `[2, --max-fanout]` (default 8);
2. every live consumer is a same-phase `core.compute.*` op — a `Mem`-phase
   write sampling the value, a read, a side-effecting op or a cross-phase
   consumer keeps the boundary alive and blocks the clone (DPI calls, random
   sampling and side-effecting system tasks are therefore never cloned);
3. the result is a **predicted boundary** (its consumers land in different
   predicted nodes);
4. the remaining `--max-clones` budget (default 250000) covers the clones.

Each consumer gets one clone (multi-position uses share it), tagged with the
source op's `SimPhase`; the source dies and its boundary value is eliminated.
Candidate chains are processed from consumers toward producers, tracking live
users and rechecking every gate after expansion; candidate cycles are left
unchanged.

```text
%t = not(%x)              ; %t feeds two nodes  =>  boundary value
%y = and(%t, %a)  (node 1)      %ty = not(%x); %y = and(%ty, %a)  (node 1)
%z = or(%t, %b)   (node 2)      %tz = not(%x); %z = or(%tz, %b)   (node 2)
                                ; %t gone, no boundary slot for it
```

On a model without phase attribution (B5 has not run) the predicted General
op set is empty and the pass is a no-op.

## Diagnostics

`shared_compute_candidates`, `boundary_values_predicted`, `boundary_hits`,
`cloned`, `boundary_values_eliminated`, `dead_sources_removed`,
`skipped_fanout`, `skipped_non_compute_consumer`, `skipped_local` (result is
not a predicted boundary), `skipped_budget`, `cyclic`, and `idle_reason`
(`none` when clones happened, else the first applicable of
`no_general_ops` / `no_candidates` / `no_boundary_candidates` /
`budget_exhausted`). The idle reason and the elimination counters are always
reported, so a zero-hit run still records why.

The pass is a `SemanticTransform`; the pass manager invalidates existing
backend mappings and increments the semantic revision. XiangShan supports
`XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE=0` for a control run and
`XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE_MAX_CLONES` for its budget. The Python
entry point also accepts `--no-clone-shared-compute`.
