# Named store layout

`cpu.st.layout-named-stores` is the fifth pass of the six-phase CPU mapping
pipeline (the first M4 pass). Requires a `GeneralFunctions`-stage mapping;
produces the `LayoutNamedStores` stage by filling `mapping.dataLayout` with
the physical type table and the seven named stores. The legacy layout fields
(objects/values/localFrames/runtime and their byte totals) stay empty on the
six-phase pipeline.

The stores, in fixed `CpuNamedStoreKind` order:

1. `regLatch` — one named field per non-array state. Physical type: a 2-state
   1-bit unsigned logic maps to Bool, ≤64-bit logic to UInt/SInt 8/16/32/64
   (width rounded up), wider logic to `Array(UInt64, ceil(w/64))`, four-state
   logic to `Array(base, 2)`; Real → F64; String → String; arrays recurse.
   Emit declares two struct instances (`regLatchStore`, `regLatchStoreNext`)
   from this one store.
2. `mem` — one named array field per array state (element type × count).
3. `boundary` — one field per input port (aux = port index), then the
   cross-supernode / General→Mem operand value set. Mem write operand slots
   are named `<mem>__w<idx>__<enable|addr|data|mask>` (per-mem write index in
   op-id order; memWriteSeq triples use `enable<j>`/`addr<j>`/`data<j>`; a
   value shared by several writes keeps its first writer's slot). All other
   entries use the source signal name. Event/Output cones are self-contained
   (they read stores) and never extend this set.
4. `prevEvent` — one slot per (event,edge) cluster, named
   `<signal>__<posedge|negedge|both>`, typed after the event value,
   aux = cluster index.
5. `eventAct` — one byte-packed Bool bit per cluster (offset = act/8,
   aux = act index, sizeBytes = maxAct/8 + 1); field names reuse the prevEvent
   base and are uniquified.
6. `timeslotTrigger` — one Bool byte per event-carrying timeslot task
   (Output-phase `core.system.task` with a `timeslotFlag` parameter),
   offset = aux = flag index, named after the task's `name` parameter.
7. `activeFlags` — the `eventActiveFlag` / `dataActiveFlag` /
   `dataActiveFlagNext` byte arrays, each `Array(UInt8, max(N,1))` with N =
   the General supernode count; aux carries N (byte index == the M4 supernode
   ordinal defined in [build-event-bitmaps](build-event-bitmaps.md)).

Naming is centralized in this pass; emit only consumes the results:

- Prefer the StateObject/SimValue name; sanitize: characters outside
  `[A-Za-z0-9_]` become `_`, a leading digit gains a `_` prefix, a C++
  keyword gains a `_` suffix.
- Unnamed values fall back to the producer op category prefix plus the value
  id (`readR_`/`readS_`/`readM_`/`slice_`/`cnst_`/`not_` etc.).
- All store fields share one global namespace: a conflict deterministically
  appends `_<id>`, then `_<id>_<seq>`. Diagnostics report `renamed_count`.

`verifyCpuDataLayout` forks on `namedStores`: when present it runs structural
checks only — dense type ids with forward array element references, the fixed
seven-store order, globally unique non-empty field names, aligned and
non-overlapping offsets whose total matches `sizeBytes`, a state↔field
bijection with the physical types, boundary coverage recomputed from the
graph, one prevEvent/eventAct entry per (event,edge) cluster, a timeslot flag
set matching the Output tasks, and the activeFlags shape. An engaged-but-empty
`helperReadCaches` shell is tolerated because the positional v2 JSON tail
materializes it on load. When `namedStores` is absent the legacy canonical
rebuild comparison runs unchanged (old pipeline unaffected).
