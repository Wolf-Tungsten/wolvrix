# Pack bit registers

`grhsim.pack-bit-registers` combines groups of two to 64 ordinary unsigned,
two-state, one-bit registers into scalar words. It requires a completed CPU
schedule so it can separate states by quiescence projection membership. The
semantic transform invalidates that schedule; run all eight CPU mapping passes
again before emission. It has no options and does not match object names.

Each candidate must have one `core.state.regWrite` and at least one ordinary,
parameterless `core.state.read` of the exact state type. No other references to
the target are allowed. Each event history must be an unsigned two-state bit,
referenced only by that write. Target and history initialization must each be a
single `core.init.const` with a known scalar string literal. Random or unknown
initialization, shared/observed histories, multiple writers, signed and four-state
types are excluded.

The grouping key comprises enable and mask ValueIds, ordered event ValueIds,
edge kinds, corresponding history initial bits and target projection membership.
Data and target initial bits may differ. Both bit enable and bit mask must have
the exact unsigned two-state type. A group's original write order assigns bits
from low to high; a single remaining bit stays unchanged.

For example, two registers with initial values `q0=0`, `q1=1`:

```text
regWrite(en, d0, mask, clk), refs=[q0,h0], event_edges=[posedge]
regWrite(en, d1, mask, clk), refs=[q1,h1], event_edges=[posedge]

=> initial packed = 2'd2
   data = concat(d1,d0)
   masks = replicate(mask, rep=2)
   regWrite(en, data, masks, clk), refs=[packed,h0], event_edges=[posedge]
   read(q0) => sliceStatic(read(packed), sliceStart=0, sliceEnd=0)
   read(q1) => sliceStatic(read(packed), sliceStart=1, sliceEnd=1)
```

`regWrite` operands are enable, new data, write mask, then one value per event;
references are the target followed by one history per event. An event edge is
tested against its old history, regardless of enable. Every history is sampled
on the same rounds before and after packing, so identical private histories can
share the first representative. `concat` operands run from most to least
significant; `replicate` repeats its only operand; slice bounds are inclusive.

The CPU emitter consumes the existing concat, scalar read, slice and masked-write
operations. Slice results used by commit retain compute-phase snapshots; the
packed read can use the existing compute-only state alias. All data bits are
computed before any commit. Packing does not introduce wide return-value helpers
or alter the caller-provided-buffer ABI. A packed change wakes the union of bit
readers, which can trade fewer commits for more compute work; benchmark this
tradeoff on the target workload.

Compaction removes replaced states, writes and redundant histories, remapping
all remaining references. Reapplying after remapping is idempotent: produced
words have widths greater than one. Diagnostics report `packed_register_bits`
and `packed_register_words`.
