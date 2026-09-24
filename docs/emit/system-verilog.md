# SystemVerilog Emission

## Wide Logical Shift Followed by Bit-Zero Selection

The emitter recognizes a `kSliceStatic` with `sliceStart = sliceEnd = 0`
whose operand is the result of a `kLShr`. For this pattern:

```systemverilog
logic [127:0] shifted;
assign shifted = data >> index;
assign result = shifted[0];
```

it emits the result assignment as:

```systemverilog
assign result = ($unsigned(index) >= 32'd128) ? 1'b0 : data[$unsigned(index)];
```

The rewrite requires unsigned logic operands, a source wider than 64 bits,
a shift result of the same width as the source, a one-bit slice result, and
a source expression that is a simple identifier. Arithmetic shifts, signed
operands, scalar-width operations, differently sized shift results, and other
slices keep their existing emission. This applies to ordinary assignments and
eligible DPI-inline expressions. It does not modify the GRH or partitioning.

The full index is used in both the comparison and the selection. Out-of-range
logical shifts return zero, whereas a plain out-of-range selection would return
X; truncating the index before testing the range would also change behavior.
Unknown index bits remain unknown. If the original shifted value has other
users, its assignment remains available; otherwise the consuming backend can
eliminate it.

The purpose is to avoid computing a multiword shifted temporary when only one
bit is consumed. The width threshold is conservative: direct bit selection is
not necessarily faster for native scalar shifts. No target-specific weight
coefficients or workload names are involved. Static cost models must account
for such emission and backend simplification separately from raw GRH size.

`emit-sv-shift-low-bit` checks the generated form, excluded cases, shared shift
users, and four-state expression equivalence with Slang's `SVInt` arithmetic.

## Constant Left Shifts in Masked Memory Writes

A `kMemoryWritePort` has operands `(updateCond, address, data, mask, events...)`.
For a constant partial mask, the emitter writes the selected memory bits with
nonblocking assignments. When `data` comes from an unsigned constant
left shift, it can select the corresponding source bit directly:

```systemverilog
logic [27:0] tag;
logic [447:0] shifted;
assign shifted = {{420{1'b0}}, tag} << 32'd28;
// Before, for one selected mask bit:
Memory[address][28] <= shifted[28];
// After:
Memory[address][28] <= tag[0];
```

For output bit `b` and shift amount `s`, the emitted RHS is `source[b-s]`
when `0 <= b-s < sourceWidth`, otherwise `1'b0`. This preserves X/Z source
bits and the zeros introduced by widening and shifting. A scalar source uses
its identifier without a bit index. The constant is parsed with its literal
width and then resized to the GRH shift-amount width before use.

The rule requires unsigned `Logic` operands, a result wider than 64 bits,
`sourceWidth <= resultWidth == memoryWidth`, a known nonnegative constant
shift amount, and a simple source identifier. Signed, dynamic, unknown,
truncating and DPI-dependent writes retain their previous emission. Full and
dynamic masks are unchanged. The memory address, enable, mask, event control
and write priority are preserved. The original shifted
value remains available to any other consumers.

The same bit mapping applies to explicit one-bit `kSliceStatic` consumers of
constant left shifts when the source and shift result have equal widths. An
out-of-range static slice is left unchanged because its value is X, not zero.

This is an equivalent SV expression rewrite, not a GRH/partition change. It
avoids backend code that repeats a multiword left shift for every written bit;
it does not combine distinct memory-write operations. Tests cover
70/112/300/448-bit memory rows, four-state widening, boundary bits, fallback
cases and overlapping write priority.

## Contiguous Constant Memory Masks

Within one `kMemoryWritePort`, adjacent selected bits of a known constant mask
can use a single part-select nonblocking assignment:

```systemverilog
Memory[address][18:0] <= data[18:0];
```

The data width must match the memory row width, and neither the data nor the
write operation may depend on DPI side effects. A plain data identifier can
always supply the slice under these conditions. For eligible constant left
shifts, a run entirely within the original source uses that source slice
directly. Other runs use the data identifier's slice when available, or retain
individual bit assignments. Isolated selected bits retain the bit-write path.

This grouping stays within one operation's address, guard, event and priority
position. Pure dynamic addresses are allowed: all assignments in that run
already use the same address expression. Distinct write operations, including
overlapping writes, are never combined or reordered. Dynamic and unknown
masks retain their previous lowering. This reduces the number of temporary
values and indices that Verilator stages for nonblocking memory updates.
