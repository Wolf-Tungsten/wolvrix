> Historical independent-IR design, retired on 2026-10-26. APIs, pass names and
> implementation paths below describe that archived version. For the current
> unified GRH simulator, see the [simulation workflow](../../../grhsim.md).

# Named store layout

`cpu.st.layout-named-stores` is the third pass (C3) of the CPU mapping
C segment. Requires a `GeneralSupernodes`-stage mapping (lowered in M5d-6 —
layout no longer waits for function packing); produces the
`LayoutNamedStores` stage by filling `mapping.dataLayout` with the physical
type table and the 12 named stores. `CpuDataLayout` carries exactly these
two payloads; the legacy layout fields (objects/values/localFrames/runtime
and their byte totals) were removed in M5d-6.

The pass makes **zero classification decisions**: each state's store follows
the A7 [`grhsim.select-state-stores`](select-state-stores.md) `storeClass`
annotation (`smallSink` / `largeSink`) and nothing else — the old implicit
array-type split is gone. A state still at `StateStoreClass::None`
is an error that points at `grhsim.select-state-stores`.

The stores, in fixed `CpuNamedStoreKind` order:

1. `smallSink` — one named field per **smallSink-class** state, scalar or
   array. A 2-state 1-bit unsigned logic maps to Bool, ≤64-bit logic to
   UInt/SInt 8/16/32/64 (rounded up), and wider 2-state logic to a single
   UInt(width), emitted as a native `_BitInt`. A shaped logic type nests one
   `Array(element, extent)` layer **per shape dimension**, preserving rank
   and every extent (for example, `[2,3]` becomes
   `Array(Array(element, 3), 2)`). Four-state physical layout is representable
   here, but the CPU emitter rejects four-state aggregate code generation.
   Emit declares `smallSinkStore` and its same-layout NBA buffer
   `smallSinkStoreNext`; P_publish copies next to current.
2. `largeSink` — one named field per **largeSink-class** state, scalar or
   array, with the same physical type mapping. `P_large_sink` updates this
   store in place.
3. `input` — one field per external input port (aux = port index), stored
   separately from computed boundary values.
4. `nonSinkBoundary` — values passed between nonSink supernodes; their real
   changes drive `computeSupernodeFanout`.
5. `smallSinkEnableBoundary` — write enables passed from nonSink producers
   to smallSink supernodes; eligible 1-bit enables also receive dense
   `enableBitValues` ordering for an emitted `enabledWords` bitmap.
6. `smallSinkPayloadBoundary` — corresponding data, mask, address and other
   write operands, kept separate from enable probes.
7. `largeSinkEnableBoundary` — write enables consumed by `P_large_sink`,
   with an independent optional `enabledWords` bitmap.
8. `largeSinkPayloadBoundary` — corresponding data, mask and address operands.
   Large-sink write slots use `<state>__w<idx>__<role>` (per-state write
   index in op-id order): `enable`/`data`/`mask` for `write` and scalar
   `latchWrite`; `enable`/`addr`/`data`/`mask` for one-index `writeArray`/
   `writeArrayMask`,
   `idx<j>` instead of `addr` for multi-index writes; and `enable`/`data`
   for `fill`. A value used by several consumer classes has a field in each
   required store.
9. `prevEvent` — one slot per (event,edge) cluster, named
   `<signal>__<posedge|negedge|both>`, typed after the event value,
   aux = cluster index.
10. `eventAct` — one byte-packed Bool bit per cluster (offset = act/8,
   aux = act index, sizeBytes = maxAct/8 + 1); field names reuse the prevEvent
   base and are uniquified.
11. `timeslotTrigger` — one Bool byte per event-carrying timeslot task
   (Output-phase `core.system.task` with a `timeslotFlag` parameter),
   offset = aux = flag index, named after the task's `name` parameter.
12. `activeFlags` — the `nonSinkActiveFlag` / `nonSinkActiveFlagNext` byte arrays
   (gsim-compatible packing: bit `i % 8` of byte `i / 8` is the flag for
   supernode `i`; the P_general scan skips silent bytes whole. V2-M2 removed
   `eventActiveFlag`: nonSink supernodes fire on `nonSinkActiveFlag` alone,
   smallSink supernodes gate on their eventActStore signature or fire
   unconditionally), each `Array(UInt8, max(ceil(N/8),1))` with N = the
   General supernode count; aux carries N (bit index == the supernode ordinal
   fixed by C2, see [merge-general-supernodes](merge-general-supernodes.md)).

Naming is centralized in this pass; emit only consumes the results:

- Prefer the StateObject/SimValue name; sanitize: characters outside
  `[A-Za-z0-9_]` become `_`, a leading digit gains a `_` prefix, a C++
  keyword gains a `_` suffix.
- Unnamed values fall back to the producer op category prefix plus the value
  id (`readR_`/`readS_`/`readM_`/`slice_`/`cnst_`/`not_` etc.).
- All store fields share one global namespace: a conflict deterministically
  appends `_<id>`, then `_<id>_<seq>`. Diagnostics report `renamed_count`.

`verifyCpuDataLayout` runs structural checks on the named stores — dense type
ids with forward array element references, the fixed 12-store order,
globally unique non-empty field names, aligned and non-overlapping offsets
whose total matches `sizeBytes`, a state↔field bijection per store class with
the physical types, input and boundary coverage recomputed from the graph, one
prevEvent/eventAct entry per (event,edge) cluster, a timeslot flag set
matching the Output tasks, and the activeFlags shape. The legacy
canonical-rebuild comparison was removed in M5d-6 together with the legacy
layout fields.
