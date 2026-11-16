# Narrow windows of wide shifts

`slice-demand -semantics 2state` is an experimental pass that computes only the
bits requested by a static slice of a wide expression. It is not part of any
default pipeline. Without explicit two-state semantics, it leaves the graph
unchanged. C++ callers opt in with `SliceDemandOptions{.twoState = true}`.

## Accepted expression and representation

The root is a `kSliceStatic` with one source and one 2–64-bit Logic result.
`sliceStart` is the lowest requested bit; `sliceEnd` is the highest, inclusive.
The source width must be 128–4096 bits, and the requested range must fit entirely
within it. One-bit roots are deliberately outside this first implementation;
lowering a complete wide bus one bit at a time can greatly increase work.

The pass propagates this fixed window through unsigned wide `kMux`, bitwise
`kAnd`/`kOr`/`kXor`/`kNot`, equal-width `kAssign`, and concatenation. A mux has
operands `[condition, true_value, false_value]`; bitwise binary operations have
`[left, right]`. Concat operands run from the most significant piece to the least
significant. Relevant intersections with each concat piece are extracted and
assembled in their original bit positions.

At a logical shift (`kShl` or `kLShr`, operands `[data, amount]`), the data must be
provably zero outside one span of at most 64 bits. Supported proofs are known
constants and zero-padded concatenations of scalar payloads. Several adjacent
pieces are allowed if their combined span fits 64 bits. A scalar payload remains
a reference to the same SSA value; it may contain arbitrary bits.

For example:

```text
slice((concat(992'b0, data32) << amount), 111:96)
    -> low16(amount <= 96 ? uint64(data32) >> (96 - amount)
                          : uint64(data32) << (amount - 96))
```

Both scalar shifts use the IR's zero-on-overshift semantics. A C++ implementation
must not execute an unchecked native shift by 64 or more.

Before using the general formula, the planner now checks for an aligned scalar
displacement. A zero-padded concat can prove `amount = unsigned(index) * 2^k`
without overflow. The payload span must fit within the stride, and the complete
requested window must fit inside exactly one translated payload. For example:

```text
slice(concat(992'b0, data32) << concat(23'b0, index5, 5'b0), 111:96)
    -> index5 == 3 ? low16(data32) : 0
```

Equal-width bit-preserving assigns, single-operand concats and same-width
`0 - amount` can carry this proof. At least one leading zero is required: both
the positive magnitude and its negative must fit the signed displacement type.
The signed-direction form `negative < signed(0) ? data << -negative : data >> negative`
also reduces when all three displacement expressions are proven to reference
the same index, stride and width. At index zero both arms shift by zero;
otherwise the signed test selects the left shift by its magnitude. No hardware
address or specific field name is recognized. General, misaligned, cross-field,
different-index and unproved cases retain the existing window lowering.

The pass reports `aligned_fields` separately from general shift windows. The
Boolean equality is shared across compatible mask/data windows; subsequent
compiler simplification can combine the masked update into a field selection.

More generally, a payload at bit offset `b` requested at bit `l` has displacement
`l - b - amount` for a left shift and `l - b + amount` for a right shift. Positive
displacement selects a right shift of the scalar payload, negative displacement
selects a left shift. The implementation splits cases without forming a signed
sum of an arbitrary 64-bit shift amount. Where a sum is necessary, an explicit
`amount < 64` guard prevents overflow from turning an enormous shift into a small
one. Signed scalar inputs are first viewed as their unsigned bit patterns before
zero extension; arithmetic right shifts are not lowered.

The resulting plan contains only operations on at most 64 bits. Masks, selects,
and inversions are performed at the demanded width. No lookup table, special CPU
signal, runtime profile or hardware address is involved.

## Preservation and bounds

Run before `activity-schedule` and follow with dead-code elimination. The root
becomes `kAssign` of the new narrow result while retaining its original value,
signedness, ports, name and users. Other consumers of the wide expression retain
the original expression. Existing declared bits may also keep the wide IR alive;
the scheduler and emitter decide which operations are executed.

A candidate is first planned without modifying the graph. Unsupported operations,
effects, cycles, unknown constants, too-wide scalar spans/shift amounts, or a
failed budget leave that root unchanged. Bounds are fixed at depth 32, 256 plan
expressions and 64 concat pieces. At least one wide shift must be eliminated from
the requested path; the pass does not broadly rewrite arbitrary slices. Plans
share identical emitted scalar expressions within a graph.

No register, memory access, event or side effect is moved. Input and payload SSA
references retain their original sampling semantics. Rebuild the schedule after
the transform; applying it while retaining a stale schedule is unsupported.

## Verification and limitations

`transform-slice-demand` compiles off/on models against an independent per-bit
oracle at O0 and O3 with ASan/UBSan. Cases cover logical left/right shifts,
positive/negative window offsets, 2/16/32/64-bit results, signed scalar data and
shift bit patterns, sparse wide constants, multi-piece payloads, crossing concat
boundaries, mux/bitwise chains, and shift amounts up to `UINT64_MAX`. Additional
cases cover aligned left/right windows, the bounded signed-direction wrapper,
mask/data updates, 56-bit indices, cross-field windows and mismatched indices. An
output observes every bit of an original wide result, and a register checks
commit and held-clock behavior. Fallbacks include four-state mode, unknown bits,
arithmetic shifts, effects, cycles, excessive depth/width and malformed slices.

Fewer wide operations do not automatically establish faster simulation. The
scalar guards may add work, existing compilers may already eliminate part of the
wide expression, and graph changes can affect scheduling and register allocation.
Measure generated machine code and the complete workload before enabling a path.

The rule is independent of CV32E40P. It could apply to XiangShan's wide masks,
packed-field updates and padded vectors when only narrow slices are consumed.
Dense payloads, arithmetic shifts and large demanded outputs remain unsupported;
this pass has no measured XiangShan performance result.
