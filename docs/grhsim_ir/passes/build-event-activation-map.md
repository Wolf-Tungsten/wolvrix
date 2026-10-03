# Event activation map

`cpu.st.build-event-activation-map` is the fourth pass (C4) of the CPU
mapping C segment. Requires a `LayoutNamedStores`-stage mapping; produces
the `EventActivationMap` stage by filling `schedule.eventActivation`. (The
pre-v2 `cpu.st.build-event-bitmaps` produced sink-facing bitmaps over the
M3 influence closure to rebuild `eventActiveFlag`; V2-M2 removed both the
flag and the influence machinery.) The legacy schedule payload fields
(roundSeeds/inputShadows/quiescenceProjection/…) were removed in M5d-6.

**Supernode ordinal.** The index space shared by the activation map and the
ActiveFlags bit words (V3-M1: bit per supernode) is exactly the one fixed by
C2: the General branch's
direct supernode children in partition order, numbered 0..N-1 (see
[merge-general-supernodes](merge-general-supernodes.md); C6's emit functions
only record intervals over it and never renumber). Partition ids never reach
the emitter.

**Coverage.** Per event act a (an edgeDet `act` cluster index), the entry
maps a to the NON-SINK supernodes that hold an op carrying a in their
`event_acts` — i.e. the supernodes whose `attrs.eventActs` annotation
contains a:

```
eventActivation[a] = { sn : sn is NonSink ∧ a ∈ sn.eventActs }
```

`supernodeWords` holds ⌈N/64⌉ words with bit i encoding ordinal i. Acts
with no non-sink carrier get no entry (the map is sparse). Sink supernodes
never appear: a SinkEvent supernode gates directly on its eventActStore
signature at its P_general call site, and a SinkEscape supernode fires
unconditionally every round — neither needs P_event activation. The
coverage is complete by construction and the verifier enforces it: a
missed non-sink carrier would leave its event op unexecuted on its edge
round.

**Runtime use.** P_event ORs every fired act's words straight into
`dataActiveFlag` (bit i raises supernode i), so a non-sink supernode
holding an event op fires on the edge round even when no data operand
changed (the typical members are event-gated DPI/system calls with return
values). Non-sink supernodes without event ops receive no entry and fire
purely on data-driven `dataActiveFlag` as before.

The verifier recomputes the map from the partition tree and requires an
exact match — which is simultaneously the "covers every non-sink carrier"
and the "no sink supernode appears" check.
