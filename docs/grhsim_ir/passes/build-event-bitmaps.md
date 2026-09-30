# Event bitmaps

`cpu.st.build-event-bitmaps` is the sixth pass of the six-phase CPU mapping
pipeline (the second M4 pass). Requires a `LayoutNamedStores`-stage mapping;
produces the `EventBitmaps` stage by filling `schedule.eventBitmaps`. Every
legacy schedule field (roundSeeds/inputShadows/quiescenceProjection/…) stays
empty on the six-phase pipeline.

**Supernode ordinal.** The index space shared by the event bitmaps and the
ActiveFlags byte arrays: the General branch flattened in tree order — emit
function child order, then each function's supernode child order — numbered
0..N-1. Partition ids never reach the emitter.

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

The verifier recomputes the bitmaps from the model and partition tree and
requires an exact match, and the bitmap count to equal the edge detector
count.
