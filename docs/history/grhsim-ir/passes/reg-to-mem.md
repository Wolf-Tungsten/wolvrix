> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../../grhsim.md).

# Recovering scalarized tables

> **RETIRED — 2026-10-10 (stage-07 sub-stage 7c-3).** The `grhsim.reg-to-mem`
> and `grhsim.reg-to-mem-analyze` passes, their tests, and the playground
> Makefile targets/scripts around them are removed. After stage-04/06
> preserve-aggregate carried arrays end-to-end from firtool RTL into GrhSIM
> IR, the frontend no longer scalarizes register families, so this pass had
> no input left; measured results also showed disabling it was never a
> regression. The preserve-aggregate design and old-route measurements are
> available in git history. The body below is kept verbatim as a
> historical record of the recognition rules, cost model, and
> naming/reporting contracts — it describes a pass that no longer exists.

`grhsim.reg-to-mem` is a semantic pass run after GRH-to-GrhSIM lowering and before CPU
split-phase and mapping. It creates array states (one shaped `core.logic` state per
family) from compatible scalar states without depending on RTL module names.

For example, with `hit_k(i) = enable_k && address_k == i`:

```text
q_i.next = hit_1(i) ? data_1 : hit_0(i) ? data_0 : q_i

=> core.state.writeArrayMask @table  (enable_0, address_0, data_0, maskAllOnes, clock)
     event_edges = ["posedge"]
   core.state.writeArrayMask @table  (enable_1, address_1, data_1, maskAllOnes, clock)
     event_edges = ["posedge"]
   ; emitted in this declaration order: the later write wins an address collision
```

Each recovered write carries an enable, an element address and an element data value,
plus a mask operand (a multi-write sequence shares one all-ones mask constant). The
writes are emitted in low-to-high priority order: op declaration order is the
application order (stage-06 semantics; `memWriteSeq` is retired), so different
addresses both update and the last enabled write to an address wins. All operands use
pre-commit state. The trailing clock operand carries the raw event annotation
(`event_edges`), independently of the write enables; `grhsim.lower-edge-detect` later
turns it into an edge detector.

The pass preserves a separate `write` update condition unless implication is
proved. It also preserves uncovered default-value branches. Identical broadcast
updates can become `fill(enable, data, events...)`, provided they do not overlap
other writes. With `--enable-row-constant-fill`, a default branch whose data is a
compile-time constant that differs per row (for example an identity-init reset
writing the row number) no longer fragments the family key; the branch expands to
one static-address `writeArray` per row at the lowest priority (first in
declaration order) instead of a
`fill`. The usual overlap proof still applies, so those writes stay exclusive
with every decoded write. A single indexed update uses `writeArray(enable, address,
data, mask, events...)`. Multi-bit masked sequences are rejected because the
ordered multi-write form shares one all-ones mask and cannot express per-port
masks.

For example, `if (reset) q[row] = row; else if (en && addr == row)
q[row] = data` becomes one array and one `writeArray` per recovered write —
`(reset, 0, 0), (reset, 1, 1), ..., (!reset && en, addr, data)` — emitted in that
declaration order. Each write
contains an enable, an array index relative to the recovered row base, and the
data to write; event operands follow. Later writes win on an
address collision. Overlapping fill branches may be combined only when their
data agree in every row; equality in the first row alone is insufficient.

With `--enable-or-write-merge`, the pass also recovers OR reductions of
zero-fallback muxes. These are simultaneous writes, so a collision must combine
data rather than choose one port. For three ports, with
`hit_j(row) = enable_j && address_j == row`, the original row is:

```text
if (hit_0(row) || hit_1(row) || hit_2(row))
    q[row] = (hit_0(row) ? data_0 : 0)
           | (hit_1(row) ? data_1 : 0)
           | (hit_2(row) ? data_2 : 0)
```

The replacement builds shared data expressions before mapping:

```text
merged_0 = data_0
merged_1 = data_1 | (enable_0 && address_0 == address_1 ? data_0 : 0)
merged_2 = data_2 | (enable_0 && address_0 == address_2 ? data_0 : 0)
                 | (enable_1 && address_1 == address_2 ? data_1 : 0)
writeArrayMask(table, enable_0, address_0, merged_0, maskAllOnes, clock)
writeArrayMask(table, enable_1, address_1, merged_1, maskAllOnes, clock)
writeArrayMask(table, enable_2, address_2, merged_2, maskAllOnes, clock)
; declaration order = priority
```

The last enabled port at each address contains all enabled data for that
address. Two colliding writes of 1 and 2 therefore produce 3. No enabled port
means no write; every operand still reads pre-commit state. Bounds and nonzero
row bases use the existing address normalization. The number of collision
expressions depends on the port count, not the number of rows.

Recognition requires two-state data, all-ones masks, address-decoded guards,
zero mux fallbacks, and an update condition equal to the OR of the mux guards.
`reduceOr(concat(one-bit guards))` is also recognized. Common global AND terms
are retained on every port. Extra/missing update disjuncts, priority blockers,
different address types, and more than 64 ports are rejected. The existing
initialization, ownership and cost checks still apply. The cost
estimate charges the pairwise collision logic; it is not a runtime prediction.

Tables can be discovered from writes without a packed read anchor, or from concat
read views. Compatible shared `sliceArray` users become indexed `readArray`.
`sliceDynamic` windows up to 64 bits and `sliceStatic(lshr(...))` windows become a
concat of indexed bit reads. For multi-bit elements, each bit offset is divided into
an element index and an intra-element bit offset; the latter selects a bit from the
memory read. Bounds are checked before offset arithmetic, so partial windows return
zero for each out-of-view bit without accessing invalid memory or wrapping an index.
Repeated sequential views share one array. Nonperiodic views use up to eight runs
of sequential rows or repetitions of one row; larger mappings retain packed access.
For example, low-to-high lanes `[q0,q0,q1,q2,q3,q3]` map to three runs rather than
using the incorrect formula `index % 4`. The index is clamped before memory access.
Extra scalar read users remain connected through fixed-address `readArray` ops. When
only the indexed read is optimized, original writes remain fixed-address `writeArray`
ops.

The initial implementation requires two-state element types and constant initializers.
Each array row retains its original initializer. Unknown state
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
| `--enable-or-write-merge` | `false` | Recover OR-of-zero-mux writes while preserving same-address data OR |
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

Declaration provenance is maintained for every merged row: before cleanup's
compaction drops the scalar row states, each row declaration's slices are
re-targeted to the row's element slice of the new table state with kind `Merged`
(`targetOffset` = `row * elementWidth`, `width` = element width), via the shared
`mergeProvenanceStateSlices` helper. Rows without provenance slices merge
structurally as before. Whole-object markers are dropped by the helper, as a
merged row is realized by an element slice, never by the whole table. The info
diagnostics report `provenance_records` (records re-targeted).

When every row of a merged family is a declared symbol whose hierarchical names
differ only in `_`-separated numeric tokens, the pass recovers the declared
family shape: the numeric token positions must sweep a complete rectangular grid
starting at zero, and the group's row order (decode addresses) must equal the
row-major linearization of the indices. A family `tbl_0..tbl_63` reports shape
`64`; a family `mat_0_0..mat_1_3` with decode order `(0,0),(0,1),...` reports
`2x4`. The new table state is then named `__reg_to_mem_<pattern>__<firstRowId>`
with `@` at the varying token positions (for example
`__reg_to_mem_cpu$rob$robEntries_@_vls__12345`); otherwise the name stays
`__reg_to_mem_<firstRowId>`. The shape only annotates naming and reporting — the
table type stays a flat one-dimensional array (a `core.logic` state whose shape is
`[rows]`) whose element order is the
row-major linearization of the declaration grid, matching the declProvenance
linearization convention. Diagnostics report `declared_families` and
`max_array_dims`; the TSV report carries a trailing `shape` column (`-` when no
pattern holds). Earlier columns are unchanged.


The playground Makefile provides `test_grhsim_reg_to_mem`, `analyze_grhsim_reg_to_mem`
and `rewrite_grhsim_reg_to_mem`. The last two accept `GRHSIM_AUDIT_MODEL` and
`GRHSIM_REG_TO_MEM_REPORT`. Tests compare scalar and rewritten state transitions,
including address collisions, global else-if guards, reset, old-state feedback,
event-driven writes, shared/repeated reads and invalid addresses.
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
