# Packed scalar bit-slice stores

`packed_scalar_slices` is enabled by default. Set the emit attribute to `0` or
`WOLVRIX_GRHSIM_PACKED_SCALAR_SLICES=0` to use ordinary scalar stores. An explicit
attribute takes precedence over the environment. The option is independent of
`indexed_concat_slice` and the concat-reduction rules.

## Pattern

The emitter combines eight consecutive `kSliceStatic` operations when each
extracts one bit from the same resolved scalar expression into consecutive
Boolean value slots. Each operation has one operand (the source), one result,
and equal `sliceStart` / `sliceEnd` bit positions. Source widths are 8–64 bits;
the selected bits need not be consecutive or increasing.

For example, `b0 = src[7]`, `b1 = src[6]`, …, `b7 = src[0]` previously loaded,
compared and stored eight bool slots separately. The new helper constructs eight
canonical Boolean bytes, reads the old bytes using `memcpy`, stores the new bytes,
and returns their XOR. Each changed bit retains its own activation predicate.
If none changed, the emitter skips the entire notification block. This is not a
union of all reader sets and does not add false reader activations.

The fast path requires one-byte bool objects with false/true representations
0/1, little-endian byte order and an eight-byte uint64. It accesses exactly eight
actual adjacent objects using `memcpy`, without type-punning, alignment or
padding assumptions. Other layouts use a scalar fallback. Both paths report
changes in the same eight byte lanes; callers test whether each lane is nonzero.
No ISA-specific intrinsics or native-only runtime requirements are introduced.

## Safety and compatibility

All destinations must be distinct canonical persistent bool slots. Events,
direct state aliases, effects, wide sources, nonadjacent slots and mixed sources
fall back. A group cannot cross a supernode, concat-prefix initialization,
consumer, commit or other operation. Source expressions are accepted only after
checking their actual inlined leaves for destination-slot aliases. Named locals
remain their original snapshots; visible register reads stay within the same
compute region. The source is evaluated once before any destination write.

The optimization changes storage operations within that region, not the IR,
schedule or commit order. It reuses the normal per-value notification emitter,
including deferred, shared and direct activation. Event classification remains
on the original path. Reinitialization needs no extra cache or valid bit.
The execution planner uses the same matcher and reports
`packed_scalar_slice_groups` and `packed_scalar_slice_values`.

## Tests and applicability

`emit-grhsim-cpp-packed-scalar-slices` checks default/on/off/environment
precedence, unaligned groups, arbitrary bit order through bit 63, named snapshots,
inlined sources, changing registers, repeated eval without edges, reinit and
negative effect/event/mixed-source cases. Generated ordinary, split, profile and
shared-activation models run at O0/O3 with ASan/UBSan. An independent helper
oracle validates fast and forced-portable results, precise per-bit change masks,
and adjacent sentinels. The test also compares planner and emitter match counts.

The pattern occurs in lowered datapaths that split integers into individual
persistent Boolean values. It is present in CV32E40P and in frozen XiangShan
single-thread sources. Source-shape counts are only a feasibility estimate:
actual eligibility also depends on alias, event and scheduling proofs. An
end-to-end speedup in one design does not establish the same speedup in another.
The playground retains workload-specific functional and performance evidence.
