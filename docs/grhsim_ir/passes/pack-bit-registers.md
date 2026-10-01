# Pack bit registers

`grhsim.pack-bit-registers` combines groups of two to 64 ordinary unsigned,
two-state, one-bit registers into scalar words. Since M5d-3 the pass runs on the
un-partitioned whole graph (pipeline stage A5, before `grhsim.lower-edge-detect`
and any CPU mapping) and decides purely from the raw `event_edges` annotation
plus read/write reference analysis. It no longer reads a CPU schedule, a
quiescence projection, or private per-event history states — none of those exist
on the whole graph. It does not match object names.

Each candidate target must be an unsigned two-state one-bit logic state with
exactly one `core.state.regWrite` writer and at least one ordinary,
parameterless `core.state.read` of the exact state type. No other references to
the target are allowed (a state used anywhere except those plain reads and its
one write is rejected). The writer must have no results, exactly one object ref
(the target), operands `enable, data, mask` followed by one value per event, all
one-bit two-state unsigned, and `event_edges` as its only parameter (non-empty,
entries limited to `posedge`/`negedge`). Target initialization must be a single
`core.init.const` with a known two-state scalar literal.

The grouping key comprises the enable and mask ValueIds, the op phase, and the
ordered `(event ValueId, edge kind)` sequence. Data and target initial bits may
differ. Lanes of one group therefore share their control signature and update in
lockstep on the same event edges, which is what makes the merge
semantics-preserving; the write mask and enable apply to the whole word because
every lane shares them. A group's original write order assigns bits from low to
high; groups are chunked into words of at most 64 lanes.

Candidates that cannot merge are rejected explicitly and counted per reason in
the info diagnostics as `pack_bits_rejected_target_type` / `_init` /
`_multi_writer` / `_extra_refs` / `_no_reads` / `_write_shape` / `_write_params`;
safe candidates whose control signature has no partner are counted separately as
`pack_bits_singleton`. Rejection counters count write ports (a multi-written
state rejects each of its writers).

`--report <path>` is an optional diagnostic dump, disabled by default. When
enabled, the pass finishes by writing a tab-separated membership list with the
header row `packed_state	bit_index	member_name	init_bit` and one row per
packed register: the packed word name, the zero-based bit index, the original
register name and its initial bit (`0` or `1`). Rows within a word ascend by
bit index and words appear in creation order. Member names are cached before
compaction rebuilds dense state IDs; the dump is read-only on the model. A
failure to open or write the file is a diagnostics error. Without the option
the pass performs no I/O and behaves exactly as before.

For example, two registers with initial values `q0=0`, `q1=1`:

```text
regWrite(en, d0, mask, clk), refs=[q0], event_edges=[posedge]
regWrite(en, d1, mask, clk), refs=[q1], event_edges=[posedge]

=> initial packed = 2'd2
   data = concat(d1,d0)
   masks = replicate(mask, rep=2)
   regWrite(en, data, masks, clk), refs=[packed], event_edges=[posedge]
   read(q0) => sliceStatic(read(packed), sliceStart=0, sliceEnd=0)
   read(q1) => sliceStatic(read(packed), sliceStart=1, sliceEnd=1)
```

`regWrite` operands are enable, new data, write mask, then one value per event;
the object ref is the target. `concat` operands run from most to least
significant; `replicate` repeats its only operand; slice bounds are inclusive.
All new ops inherit the phase of the grouped writers (always `none` at the A5
position).

Declaration provenance is maintained: before compaction drops the member
states, each member declaration's slices are re-targeted to its bit slice of
the packed word with kind `Merged` (`targetOffset` = bit index, `width` = 1),
via the shared `mergeProvenanceStateSlices` helper. Whole-object markers are
dropped by that rule, as a packed declaration is realized by a bit slice, never
by the whole word.

Compaction removes the replaced states and writes, remapping all remaining
references. Reapplying the pass is idempotent: produced words have widths
greater than one. Diagnostics report `packed_register_bits` and
`packed_register_words` plus the rejection counters above.

The CPU emitter consumes the existing concat, scalar read, slice and
masked-write operations. A packed change wakes the union of bit readers, which
can trade fewer commits for more compute work; benchmark this tradeoff on the
target workload.
