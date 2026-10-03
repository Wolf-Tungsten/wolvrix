# Boundary-aware shared compute cloning (C2.5)

`cpu.st.clone-shared-boundaries` clones cheap shared bijection producers **once
per consuming supernode** when the clone eliminates a real P_general supernode
boundary — a boundary value costs a boundaryValueStore slot, a per-round change
comparison and consumer activations, so deleting it is the win; the clones are
absorbed into their consuming supernode and never become boundaries.

This is the C-segment landing (V3-M3) of the removed semantic-layer pass
`grhsim.clone-shared-compute` (B7). B7 ran between B6 and the seal and cloned
against *predicted* C1 node boundaries (`predictGeneralBoundaries`, a static
simulation of C1's cone absorption). The prediction systematically over-counted:
C2's supernode merge (mode 1 absorbs single-predecessor targets) collapses most
node-level boundaries, so B7 paid clones for boundaries that never materialized.
The pass now runs immediately after `cpu.st.merge-general-supernodes` (C2) where
the **actual** supernode boundaries are known (`sixPhaseBoundaryValues` on the
C2 tree), so every clone pays for a boundary that really exists. Downstream
mapping passes (C3–C8) run afterwards and simply see the adjusted model; the
only mapping payload maintained in place is the partition tree.

## Candidates and gating

The candidate definition is unchanged from B7: single-result two-state scalar
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
2. every live consumer outside the source's own supernode is a same-phase
   `core.compute.*` op inside a non-sink General supernode — a `Mem`-phase
   write sampling the value, a sink op, an Event/Output branch consumer or a
   side-effecting op keeps the boundary alive and blocks the clone (DPI calls,
   random sampling and side-effecting system tasks are therefore never
   cloned). Consumers local to the source's supernode simply keep the source;
3. the result is a **real supernode boundary** (its consumers land in
   different C2 supernodes, or it is sampled by a `Mem`-phase write — case 2
   already vetoes that);
4. inside every target supernode the clone can be placed ahead of its
   earliest consumer while following every locally produced operand
   (`skipped_placement` otherwise — conservative, the boundary simply stays);
5. the remaining `--max-clones` budget (default 250000) covers the clones.

Each consuming supernode gets **one** clone, shared by all its consumer ops in
that supernode (multi-position uses share it), inserted right before the
earliest consumer so the local topological order is preserved; the clone is
tagged with the source op's `SimPhase`. When nothing references the source
result afterwards, the source op dies, its tree node (and an empty supernode
shell, if any) is removed, and its boundary value is eliminated. Candidate
chains are processed from consumers toward producers, tracking live users and
rechecking every gate after expansion; candidate cycles are left unchanged.

```text
%t = not(%x)              ; %t feeds two supernodes  =>  boundary value
%y = and(%t, %a)  (sn 1)      %ty = not(%x); %y = and(%ty, %a)  (sn 1)
%z = or(%t, %b)   (sn 2)      %tz = not(%x); %z = or(%tz, %b)   (sn 2)
                              ; %t gone, no boundary slot for it
```

On a mapping without General supernodes the pass is a documented no-op
(`idle_reason=no_candidates`).

## Framework contract (the seal relaxation)

The pass is registered as `PassKind::BackendMapping` at stage
`GeneralSupernodes` (in-place rewrite; the stage does not advance). It is the
sole registered C-segment semantic micro-adjustment: it ends with
`commitSemanticMicroMutation()` (semantic revision bump **without** the mapping
wipe) after updating the partition tree itself — clone attachment, dead-op
removal with the same dense op-id remap `compact()` applies, and empty
node/supernode shell pruning with a dense partition-id remap — and re-stamps
the mapping via `setCpuMapping`. The post-pass model verify enforces total
phase attribution and full partition coverage as the hard guard rail. The
semantic seal contract therefore reads: after the seal, only registered
C-segment micro-adjustment passes (currently this one) may touch the semantics;
all other C-segment passes stay mapping-only.

## Diagnostics

`clone_shared_candidates`, `boundary_hits`, `cloned`,
`boundary_values_eliminated`, `dead_sources_removed`, `skipped_fanout`,
`skipped_non_compute_consumer`, `skipped_local` (all consumers are local to the
source's supernode), `skipped_placement`, `skipped_budget`, `cyclic`, and
`idle_reason` (`none` when clones happened, else the first applicable of
`no_candidates` / `no_boundary_candidates` / `budget` / `gated`). The idle
reason and the elimination counters are always reported, so a zero-hit run
still records why.

XiangShan supports `XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE=0` for a control run
and `XS_WOLF_GRHSIM_IR_CLONE_SHARED_COMPUTE_MAX_CLONES` for its budget. The
Python entry point also accepts `--no-clone-shared-compute`.
