# Translation-unit planning

`cpu.st.plan-translation-units` is the eighth and terminal pass (C8) of the
CPU mapping C segment (M5d-7). Requires a `PhaseSchedule`-stage mapping;
produces the `TranslationUnits` stage — the mapping stays `complete=true` at
both terminal stages, but `cpu.st.emit-cpp` only accepts `TranslationUnits`.
Re-running on an already-planned (`TranslationUnits`) mapping discards and
replans deterministically (the C1 build-general-nodes rebuild semantics),
which is how checkpoint re-emission re-plans an old PhaseSchedule checkpoint.

The pass records a `CpuTranslationUnitPlan` on the mapping: an ordered chunk
stream bin-packed into size-bounded translation units. A chunk is one emitted
function (or the small fixed core group); its `(kind, offset, count)` range
indexes the kind's canonical stream:

| kind | stream | emitted member(s) |
| --- | --- | --- |
| `Core` | exactly one chunk | `init`/`eval`/`dumpState` drivers, `pInput`/`pEvent`/`pGeneral`/`pMem`/`pPublish`/`pOutput` drivers, the system-task driver |
| `Init` | flattened init steps, then the constant-boundary preloads (store offset order), then the prevEvent inits (act order), then the `regLatchStoreNext` sync | `cpu_init_<k>()` |
| `Event` | the Event branch's flat op list | `pEvent_c<k>(EventFrame &)` |
| `GeneralScan` | supernode ordinal ranges, aligned to the C6 EmitFunction intervals (an interval that alone exceeds the chunk cap splits at ordinal granularity) | `pGeneral_c<k>()` |
| `Supernode` | one chunk per General supernode (`offset` = C2 ordinal, `count` = 1). Unsplit supernodes hold the driver and all their C6 `helperChunks` member functions in the unit; a supernode whose estimate exceeds the unit cap (and has at least two helperChunks) splits (V3-M2): the chunk emits only the `sn_<i>` driver (wrapper) and the helpers go to `SupernodePart` chunks | `sn_<i>()` (+ `sn_<i>__c<j>(SnFrame<i> &)` when unsplit) |
| `SupernodePart` | V3-M2: one helperChunks slice of a split supernode (`offset` = ordinal, `count` = part index). Parts of one supernode tile its helperChunks in order; the part index -> helper range mapping is recomputed deterministically from the C6 helperChunks and the unit cap (`cpuSupernodePartRanges`, shared with the emitter) | `sn_<i>__c<j>(SnFrame<i> &)` for the slice's j range |
| `Mem` | the memWritePlan entries | `pMem_c<k>()` |
| `Output` | the Output branch's flat op list | `pOutput_c<k>(OutputFrame &)` |
| `Dump` | the dump item list (input ports, output ports, then the named-store fields in store order) | `cpu_dump_<k>(std::FILE *) const` |

Chunk sizes follow the shared `estimatedCpuOpLines` heuristic
(`include/grhsim/backend/cpu_phase_common.hpp`) capped by
`--chunk-max-estimated-lines` (default 2048); a chunk may exceed the cap only
when a single indivisible item does (one op, one init step's literal table).
Units accumulate consecutive chunks up to `--unit-max-estimated-lines`
(default 32768); the unit holding the Core chunk is `tu0`'s leading content
and unit names are `tu<N>` (emit writes `<prefix>_<name>.cpp`).

Spill frames are an emit-time ABI derived from the recorded chunk boundaries,
not a plan payload: values produced in one chunk of a supernode/Event/Output
op list and consumed in another chunk become fields of a nested frame struct
the driver stack-allocates and passes by reference (caller-provided buffer,
no temporaries — the AGENTS.md helper-performance route). Constants,
`input.read` results and `state.read` results are re-readable from their
stores in any chunk and never spill; `memRead` results DO spill like compute
values (the re-read form would re-evaluate the address operand, whose local
may be dead — found and fixed against the XS whole-core model). The Output
cone additionally frames every cone-produced latchWrite operand (the commit
point runs in the driver after all chunks) so the staged NBA write-back
observes the pre-commit values exactly like the single-function form did.

Verification replans with the recorded caps and requires an exact unit match
(the same recompute-and-compare pattern as the schedule tables). Diagnostics:
`units`, `chunks`, `max_unit_estimated_lines`, `max_chunk_estimated_lines`.
