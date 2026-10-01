# Packing combinational lanes

`grhsim.comb-pack` is a whole-graph semantic pass (pipeline stage A4, before
partitioning and CPU mapping) that packs groups of isomorphic combinational
lanes into wider pointwise logic. It is the GrhSIM port of the GRH
`comb-lane-pack` algorithm, extended to use declaration provenance and
declared-name structure for lane discovery and ordering.

## Roots

Candidates roots come from three sources, each individually switchable:

| Source | Option | Roots |
| --- | --- | --- |
| declarations | `--enable-declared-roots` (default `true`) | values targeted by a declProvenance slice |
| outputs | `--enable-output-roots` (default `true`) | the operand of each `core.output.write` |
| storage data | `--enable-storage-data-roots` (default `true`) | the data operand of `core.state.regWrite`/`latchWrite` (operand 1), `memWrite` (operand 2), `memFill`/`memAssign` (operand 1) and every `memWriteSeq` data triple |

A root must be produced by a supported internal op; pure leaves never form a
group alone. A root whose minimal packed width (`laneWidth × min-group-size`)
falls outside `[--min-packed-width, --max-packed-width]` is skipped up front.

## Matching conditions

The analysis recursively decomposes each root into a tree of supported internal
ops; everything else is an opaque leaf. Supported internal ops:

- `core.compute.not` / `core.compute.assign` (unary, operand and result same type),
- `core.compute.and` / `or` / `xor` / `xnor` (binary, both operands and the
  result same type),
- `core.compute.mux` (1-bit select, arms and result same type) — only under
  two-state semantics (`--enable-mux`, default `true`): a four-state select has
  X-merge behaviour that the masked-select rewrite does not reproduce, so mux
  nodes are internal only when the result and select are two-state.

Signatures compare the op name, the structural type key (width, signedness,
logic domain) and sorted parameters at every node; leaves compare only the type
key, so two lanes may read unrelated leaf values. Tree size is capped by
`--max-tree-nodes` (default 64). Non-logic values (real/string/array) are not
leaves and invalidate the whole tree.

Lanes are bucketed by signature. Within a bucket, candidates are ordered by
their declared-name family (names that differ only in `_`-separated numeric
tokens share a family pattern; indices sort row-major) and then by anchor op
position, so lanes from one declared array land adjacent in array order.
Candidates whose anchor ops sit more than `--max-root-gap` (default 128)
positions apart split into separate segments. Groups of `--min-group-size`
(default 4) to `--max-group-size` (default 16) lanes are carved greedily.

A group is accepted only when all of these hold:

- the packed width stays within `[--min-packed-width, --max-packed-width]`;
- no lane's raw def-cone depends on another lane root of the same group
  (rejected as `cross_root`: packing would create a combinational cycle);
- every lane root producer has the same op phase, and every consumer of a lane
  root is unattributed or in that phase (`phase`); at the A4 position all ops
  are unattributed, so this never fires;
- every declProvenance slice targeting a lane root is a `Direct` full-coverage
  slice (`provenance`), so the slices can be re-targeted exactly to the lane's
  bit range of the packed value.

Groups that fail a check are retried one lane shorter until they fall below the
minimum size; each rejected candidate position is counted once under
`comb_pack_rejected_<reason>` in the info diagnostics.

## Rewrite

The packed tree concatenates lane leaves most-significant first (lane 0 is the
low slice), widens internal ops to `laneWidth × lanes`, and lowers mux nodes to
masked selects `(t & mask) | (f & ~mask)` with `mask` the concatenated
per-lane `replicate` of the select. Every lane root's uses are redirected to
`core.compute.sliceStatic` of the packed result with inclusive bounds
`[lane*laneWidth, (lane+1)*laneWidth - 1]`, keeping the original result value
alive under the slice. All new ops inherit the group phase and the lane
producers' origins. Declaration slices of packed lanes are re-targeted to the
lane's bit range of the packed root value with kind `Merged`
(`mergeProvenanceValueSlices`). Finally a fixed-point sweep removes the dead
original cones (ops whose results still carry a provenance slice are kept as
live anchors) and compaction remaps IDs.

For example, four declared 8-bit lanes `lane_0..lane_3 = and(or(a_i, b_i), c_i)`
feeding outputs become:

```text
packedA = concat(a_3, a_2, a_1, a_0)
packedB = concat(b_3, b_2, b_1, b_0)
packedC = concat(c_3, c_2, c_1, c_0)
packed  = and(or(packedA, packedB), packedC)      ; 32-bit
lane_i  => sliceStatic(packed, sliceStart=i*8, sliceEnd=i*8+7)
declProvenance lane_i: {kind=merged, target=packed, targetOffset=i*8, width=8}
```

## Diagnostics and report

Info diagnostics report `comb_pack_candidates`, `comb_pack_groups`,
`comb_pack_lanes`, `comb_pack_created_ops` and the rejection counters
`comb_pack_rejected_width` / `_cross_root` / `_provenance` / `_phase` /
`_build` (signature drift between collection and rewrite). `--report <path>`
writes a TSV with header `group	lanes	lane_width	packed_width	source	family`
and one row per packed group; `family` is the declared-name pattern (`@` at
index tokens) or `-`.

The pass invalidates backend mappings through the pass manager. It runs before
`grhsim.simplify(scope=whole)` (stage A6), which fixed-point-folds the packed
forms (for example duplicate shared-leaf concats) afterwards.
