# Recovering scalarized tables

`grhsim.reg-to-mem` is a semantic pass run after GRH-to-GrhSIM lowering and before CPU
split-phase and mapping. It creates core array states from compatible scalar states;
it does not consume legacy `regToMem.intent.*` attributes or depend on RTL module names.

For example, with `hit_k(i) = enable_k && address_k == i`:

```text
q_i.next = hit_1(i) ? data_1 : hit_0(i) ? data_0 : q_i

=> core.state.memWriteSeq @table, @history
     (enable_0, address_0, data_0, enable_1, address_1, data_1, clock)
     event_edges = ["posedge"]
```

Each triple is an enable, element address and element data value. Triples execute in
low-to-high priority order: different addresses both update; the last enabled write
to an address wins. All operands use pre-commit state. The final clock operand and
history object detect the event independently of the write enables.

The pass preserves a separate `regWrite` update condition unless implication is
proved. It also preserves uncovered default-value branches. Identical broadcast
updates can become `memFill(enable, data, events...)`, provided they do not overlap
other writes. With `--enable-row-constant-fill`, a default branch whose data is a
compile-time constant that differs per row (for example an identity-init reset
writing the row number) no longer fragments the family key; the branch expands to
one static-address sequence triple per row at the lowest priority instead of a
`memFill`. The usual overlap proof still applies, so those triples stay exclusive
with every decoded write. A single indexed update uses `memWrite(enable, address,
data, mask, events...)`. Multi-bit masked sequences are rejected because
`memWriteSeq` has no mask.

For example, `if (reset) q[row] = row; else if (en && addr == row)
q[row] = data` becomes one array and a `memWriteSeq` with triples
`(reset, 0, 0), (reset, 1, 1), ..., (!reset && en, addr, data)`. Each triple
contains an enable, an array index relative to the recovered row base, and the
data to write; event operands follow the triples. Later triples win on an
address collision. Overlapping fill branches may be combined only when their
data agree in every row; equality in the first row alone is insufficient.

Tables can be discovered from writes without a packed read anchor, or from concat
read views. Compatible shared `sliceArray` users become indexed `memRead`.
`sliceDynamic` windows up to 64 bits and `sliceStatic(lshr(...))` windows become a
concat of indexed bit reads. For multi-bit elements, each bit offset is divided into
an element index and an intra-element bit offset; the latter selects a bit from the
memory read. Bounds are checked before offset arithmetic, so partial windows return
zero for each out-of-view bit without accessing invalid memory or wrapping an index.
Repeated sequential views share one array. Nonperiodic views use up to eight runs
of sequential rows or repetitions of one row; larger mappings retain packed access.
For example, low-to-high lanes `[q0,q0,q1,q2,q3,q3]` map to three runs rather than
using the incorrect formula `index % 4`. The index is clamped before memory access.
Extra scalar read users remain connected through fixed-address memory reads. When
only the indexed read is optimized, original writes remain fixed-address memory writes.

The initial implementation requires two-state element types and constant initializers.
Each array row retains its original initializer. Eliminating event histories requires
private histories with matching initial values and event definitions. Unknown state
users, unproved priority constraints and unsupported read mappings retain the original
representation. Mappings exceeding the run budget, windows wider than 64 bits and
arbitrary masked priority updates remain outside the current read/write compression.

Factory options take explicit values:

| Option | Default | Effect |
| --- | --- | --- |
| `--min-element-count` | `4` | Minimum group size, at least two |
| `--enable-read-rewrite` | `true` | Compress supported indexed read views |
| `--enable-write-merge` | `true` | Consolidate compatible writes |
| `--enable-same-address-fusion` | `true` | Combine adjacent sequence writes with identical addresses |
| `--enable-cost-selection` | `true` | Skip plans whose estimated access/computation savings are nonpositive |
| `--enable-row-constant-fill` | `false` | Merge families whose fill data is a per-row constant, expanding the fill to static-address sequence triples |
| `--report` | unset | Write a candidate TSV report |

Mandatory fixed-address reference replacement still happens when read compression is
disabled. `grhsim.reg-to-mem-analyze` accepts the same options and plans candidates
without modifying the model. Both modes share candidate ownership selection and
finish planning before mutation. Its `eligible` status is a planning result; `merged` and
`indexed-read` indicate committed transformations in the semantic pass report.
Report rows with source `excluded-state` aggregate states rejected before discovery;
their `rows` field is a state count and `first_state` is one example. They are not
table candidates. Reasons include type, initialization and unknown state users.

Cost selection compares writes only, reads only and both against unchanged scalar
storage. The report exposes `write_savings`, `read_savings` and `combined_savings`.
Old computation is credited only when all its users disappear; replacement inputs
stay live. New bounds, row mapping, reads, fills and fixed-address read overhead are
charged in estimated operation units. Identical packed-source/index/bit requests
share the estimated read cost. A view retained for other users is not credited as
removed. Activity/fanout costs and sharing between distinct equivalent packed values
are not fully modeled; these estimates require backend timing validation. Disabling
cost selection allows semantic testing and measurement of otherwise rejected plans.

The semantic pass invalidates backend mappings through the pass manager. Rejected
candidates leave the model unchanged. Model compaction remaps surviving operation,
value, state and initialization references, retaining origins and external interfaces.

The playground Makefile provides `test_grhsim_reg_to_mem`, `analyze_grhsim_reg_to_mem`
and `rewrite_grhsim_reg_to_mem`. The last two accept `GRHSIM_AUDIT_MODEL` and
`GRHSIM_REG_TO_MEM_REPORT`. Tests compare scalar and rewritten state transitions,
including address collisions, global else-if guards, reset, old-state feedback,
initial event history, shared/repeated reads and invalid addresses.
`test_grhsim_reg_to_mem_generated` compares generated CPU C++ against scalar traces
with address/undefined-behavior sanitizers, including partial windows and maximal
64-bit indices.

`test_grhsim_reg_to_mem_rtl` runs explicit scalar PHR/TAGE/FTQ/RenameTable fixtures
against Verilator. Small semantic fixtures explicitly disable cost selection to
exercise the rewrite; separate tests check profitability rejection. The Xiangshan
and HDLBits IR flows enable the pass before CPU mapping, with cost selection enabled
by default. Xiangshan defaults to `XS_WOLF_GRHSIM_IR_REG_TO_MEM=1`; set it to `0`
to disable the complete pass. `summarize_grhsim_reg_to_mem` reports actual remaining
fixed/dynamic reads, writes, sequences, triples and fills from the output checkpoint.
`benchmark_grhsim_reg_to_mem` runs two alternating, CPU-pinned 50k comparisons and
records binary hashes, commands, terminal checks and timings. Set
`GRHSIM_REG_TO_MEM_BENCH_ENABLED`, `GRHSIM_REG_TO_MEM_BENCH_DISABLED` and a fresh
`GRHSIM_REG_TO_MEM_BENCH_OUTPUT` for the exact pair of models being validated.
`measure_grhsim_reg_to_mem_build` copies generated sources into a fresh
`GRHSIM_REG_TO_MEM_BUILD_METRICS` directory and builds disabled/enabled model
libraries sequentially with four compiler jobs. Each directory contains `build.log`
and GNU time's `time.txt`. The maximum RSS is the largest child process value, not
the sum of concurrent compiler memory; these timings exclude lowering and the
difftest harness link.
