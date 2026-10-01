# Event bitmaps

`cpu.st.build-event-bitmaps` is the fourth pass (C4) of the CPU mapping
C segment. Requires a `LayoutNamedStores`-stage mapping; produces the
`EventBitmaps` stage by filling `schedule.eventBitmaps`. The legacy schedule
payload fields (roundSeeds/inputShadows/quiescenceProjection/…) were removed
in M5d-6.

**Supernode ordinal.** The index space shared by the event bitmaps and the
ActiveFlags byte arrays is exactly the one fixed by C2: the General branch's
direct supernode children in partition order, numbered 0..N-1 (see
[merge-general-supernodes](merge-general-supernodes.md); C6's emit functions
only record intervals over it and never renumber). Partition ids never reach
the emitter.

**Act sets.** The pass reuses the M3 `computeCpuEventDomainSets` influence
graph (op-level value fanout closure over `event_acts`, including the
General→P_mem operand sink edges, excluding state write→read edges):
S(sn) = the union of its member ops' influence sets, so a mem write's acts
reach the supernodes producing its operands through the sink edges. A
supernode with S(sn)=∅ is event-free (exempt) and appears in no bitmap — emit
treats it as always active.

**Bitmaps.** Per (event,edge) cluster c (the edgeDet `act` parameters,
ascending): `eventBitmaps[c] = { sn : c ∈ S(sn) }`, with `supernodeWords`
holding ⌈N/64⌉ words and bit i encoding ordinal i.

The bitmaps drive P_event's per-round rebuild of `eventActiveFlag`. The
P_general firing gate is narrower than the bitmap: a supernode is
event-gated only when it *contains* an event-carrying op (`event_acts`
non-empty); such supernodes satisfy S(sn)==K (the merge rules), so the bitmap
sets their flag exactly on a domain edge. Supernodes covered only through
downstream influence (write-operand producers, level-sensitive readers) are
not event-gated: their firing is data-driven and idempotent, and P_mem's
reader re-activation (`dataActiveFlagNext`) must be able to fire them within
the same eval. S(sn)=∅ supernodes appear in no bitmap and fire purely on
`dataActiveFlag` as before.

The verifier recomputes the bitmaps from the model and partition tree and
requires an exact match, and the bitmap count to equal the edge detector
count.
