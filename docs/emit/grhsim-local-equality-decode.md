# Local equality decoding

`local_equality_decode` / `WOLVRIX_GRHSIM_LOCAL_EQUALITY_DECODE` is an experimental,
default-off extension of [shared equality decoding](grhsim-shared-equality-decode.md).
The emit attribute overrides the environment. `shared_equality_decode=0` disables
both paths.

Combining compute regions can make a family of stable address comparisons local.
The existing sparse-update lowering requires independent persistent result slots,
so it cannot reuse those local predicates. Local decoding separates reuse of the
computation from activity-boundary storage. It does not recreate value slots,
change notifications, or scheduled operations.

## Selection and representation

Within one pure compute supernode, select 16–256 local equalities with at least
16 distinct constants, sharing one scalar Logic source of width 2–12. Equalities
may be interleaved with their consumers. Constants must be known, have the source
width, and have no X/Z bits. Operand order does not matter.

The source must be an input or an independently owned canonical scalar slot
defined outside this supernode. Event sources/results, aliased source slots,
direct state views, and sources computed inside the body are excluded. The entire
body must contain only pure compute and register reads; effects, memory/latch
operations and reg-to-mem bypass operations reject it. This proves that taking
one source snapshot at body entry is safe. Local compute reordering, priority-mux
chains and Boolean producer/guard rewrites currently cause fallback.

Each selected family owns a `2^width` Boolean table and a previous-source value:

```text
on region entry:
    if source differs from previous:
        clear table[previous], if previous is in range
        set table[source], if source is in range
        remember source
at each original equality:
    use table[constant]
```

`init()` clears the table and invalidates the previous source. Both normal and
fullpass bodies maintain the same invariant. Out-of-domain storage bits are
preserved when sampling the source; an out-of-range value produces no match,
rather than wrapping the table index. This agrees with the existing scalar
equality path, including non-power-of-two storage widths.

The table uses `std::array<bool, N>` (ordinary Boolean objects, not packed
`vector<bool>` proxies). Initialization and updates write only `false`/`true`.
This preserves the 0/1 type contract across compute calls; the former byte
table forced a load/test/setne normalization at many consumers. Selection,
source checks, update frequency, and activity boundaries are unchanged.

On the tested x86-64 ABI tables cost up to 4 KiB per family plus the previous value. There is no hardware
signal name, runtime profile, or workload-specific selection. The local table
does not replace the existing sparse cache for predicates that actually cross
activity boundaries. More tables can increase cache pressure, so applicability
alone is not a performance claim and this option remains default-off.

## Validation and accounting

`emit-grhsim-cpp-local-equality-decode` checks interleaved consumers, repeated and
unmatched addresses, changed consumers with stable source, reinitialization,
inputs with spare high bits, stored external sources, fullpass, parent disable,
default/off identity, and small/wide/duplicate/effect/local-source fallbacks.
Generated models are checked against an independent oracle with Clang O0/O3 and
ASan/UBSan. A port named `address` is included to catch temporary-name shadowing.

The execution planner reports `local_equality_decode_groups`,
`local_equality_decode_table_entries` and `local_equality_decode_predicates`.
It marks source-dependent update work as unmodeled rather than assigning a fixed
per-equality price. Fullpass retains the planner's existing unmodeled boundary.

This representation may help large XiangShan regions containing narrow CSR/enum
decoders. It does not expand 32-bit instruction domains or cross partition,
memory-intent or side-effect boundaries. Performance on XiangShan has not been
measured for this feature.
