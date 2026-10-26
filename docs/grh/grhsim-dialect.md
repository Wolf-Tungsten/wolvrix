# GrhSIM operations on GRH graphs

The `grhsim` and `grhsim.cpu` dialects use the ordinary GRH Design, Graph,
Operation, Value, LogicalType, use-def and JSON infrastructure. Their kinds and
descriptors live in `transform/grhsim/ops.hpp` and its implementation; they are
not members of the base GRH operator set. There is no second simulation graph.

Conversion, unpartitioned optimizations, Event/Output extraction, activity/
sink partitioning and the C++ emitter form the production route. The independent
IR and its APIs have been removed. See the [simulation workflow](../grhsim.md)
for entry points and the distinction between GRH type support and CPU execution.

## Events and independent write ports

`grhsim.edge-detect(event) -> fire` takes one scalar Logic bit and returns one
unsigned TwoState bit. Required attributes are `edge=posedge|negedge|both`,
`prevInit` (one-bit literal), `sampleScope` (normally `event-round`) and
`historyUpdate=unconditional`. History is sampled and updated once per event
round even if every writer is disabled. TwoState history cannot contain X/Z.
The emitter allocates its physical history; the operation itself defines
the semantics before Event partitions exist.

| Operation | Ordered operands | Result |
| --- | --- | --- |
| `grhsim.write` | fire, enable, data | none |
| `grhsim.writeMask` | fire, enable, data, mask | none |
| `grhsim.writeArray` | fire, enable, index0 … indexK-1, data | none |
| `grhsim.writeArrayMask` | fire, enable, index0 … indexK-1, data, mask | none |
| `grhsim.fill` | fire, enable, data | none |
| `grhsim.latchWrite` | enable, data, mask | none |

Clocked writes reference the original register through `regSymbol`; latch
writes use `latchSymbol`. Every writer has nonnegative `writerOrder`, preserving
the original overlapping-write precedence. Equal explicit priorities retain
declaration order, including across rename/freeze. Enable is one scalar Logic bit.
Fire is an edge-detect result, a legal OR/assign of such results, or a simplified
zero pulse; a raw clock or constant-one clocked write is invalid.

Data and mask keep the original state access width/domain rules. Indices address
the first K dimensions; data covers the remaining subarray. Mask is a bit mask
over that addressed width. Fill accepts the existing leaf-width broadcast or
full-width packed form. An equal-width `assign` expresses a shape view without
changing the state or source type. State reads and declarations remain GRH ops.

```text
Before:
  registerWritePort @q(enable, data, clk) {eventEdge=[posedge]}
After grh-to-grhsim, still without partitions:
  fire = grhsim.edge-detect(clk) {
      edge=posedge, prevInit=1'h0,
      sampleScope=event-round, historyUpdate=unconditional
  }
  grhsim.write @q(fire, enable, data) {writerOrder=0}
```

The base GRH write/call contracts keep their raw event operands. They do not gain
TriggerSpec, pulse mode or simulation phase attributes.

## Tasks, functions and DPI

| Operation | Ordered operands/results | Required metadata |
| --- | --- | --- |
| `grhsim.system-task` | fire, callCond, arguments; no result | name, procKind, hasTiming, eventDriven |
| `grhsim.system-function` | arguments; one original typed result | name, effect; sampleId/sampleScope for non-pure functions |
| `grhsim.dpi-call` | fire, callCond, input arguments, inout inputs; return, output arguments, inout outputs | targetImportSymbol, procKind, hasTiming, eventDriven |

Arguments/results within each DPI direction retain their import declaration
order. A return precedes output results. Counts, kind, scalar width and
signedness are checked against the original GRH `kDpicImport`. Calls without
events have constant-one fire and `eventDriven=false`; event-driven calls consume
pulses. Guarding and held results remain part of execution semantics.

Known pure system functions can fold/CSE. Random functions have `effect=random`;
other non-pure functions have `effect=external`. Explicit `hasSideEffects=true`
prevents a normally pure function from being treated as pure. Non-pure functions
retain their sample identity/scope and cannot be removed or merged as ordinary
computation. Monitor/strobe are converted here; their Output extraction/history
construction belongs to the later split pass.

## Output histories and effect boundaries

`grhsim.history-write(data)` has one operand, no result and History effect. It
requires `regSymbol` referring to a shared GRH register declaration with
`grhsim.history=output-end`, plus `grhsim.outputEnd=true`. Data and declaration
must have the same complete LogicalType. The op belongs to `[output]`, cannot
be cloned/CSE'd, and has no ordinary fire/enable or writerOrder. Each history
has exactly one history-write and cannot have an ordinary General writer.

`grhsim-split-event-and-output` creates these histories for free monitor/strobe
tasks. A change in the original callCond or arguments enables the task when its
original callCond is true. Logic uses case-inequality (so X/Z changes count);
Real/String use their defined inequality. History registers initialize to typed
zero/empty, and all writes commit together after all Output observations.

For `monitor(1, q)`, a simplified result is:

```text
shared: @previousCond : bit = 0; @previousQ : type(q) = 0
[output]:
    oldCond = read @previousCond
    oldQ = read @previousQ
    changed = caseNe(1, oldCond) | caseNe(q, oldQ)
    grhsim.system-task(1, 1 && changed, q) {name=monitor, grhsim.timeslot=change, ...}
    grhsim.history-write(1) {regSymbol=previousCond, grhsim.outputEnd=true}
    grhsim.history-write(q) {regSymbol=previousQ, grhsim.outputEnd=true}
```

Event-driven monitor/strobe instead carry `grhsim.timeslot=event`. Their fire
is accumulated across every Event round in an eval; Output observes the sticky
trigger even if the last round's pulse is zero. Output-end writes never enter
the General convergence loop.

General external effects carry unique nonnegative `grhsim.effectOrder` after
extraction, preserving causal call order independently of pure-op placement.
An effectful result used by Event/Output is marked `grhsim.sampleBoundary=true`
on its General producer and consumed as a held sample. Pure/read cones are
extracted separately; an original DPI/random operation is not duplicated.
`grhsim.effectPolicy=single-original-call` graph metadata records this explicit
policy. One frozen legacy fixture duplicates a DPI call in Output; its logs are
retained as an explicitly approved behavior difference in the
[frozen-observation validation](../../../pdocs/remove-grhsim-ir/20261026-r5-r6-validation.md).

## Optimization operations

`grhsim.bitSelect(mask, whenSet, whenClear)` selects each bit separately:
`(mask & whenSet) | (~mask & whenClear)`. All operands and the result have the
same complete scalar TwoState Logic type, currently 1–64 bits. This is distinct
from a whole-value mux.

`grhsim.prioritySelect(c0 … cN-1, a0 … aN-1, default)` chooses the first true
condition, or default. N is 3–64. Conditions are unsigned scalar TwoState bits;
arms/results are scalar TwoState Logic of 1–64 bits, with the existing mux
normalization of a selected arm to its result type. Simplify only folds mux
chains whose link/result types and visibility permit this replacement.

`grhsim.cpu.maskedOr(mask, dataArray)` ORs every selected array element. For N
terms of W bits, mask is scalar width N, data has width N×W and shape `[N]`.
The `views` form uses explicit `(maskOperand, maskBit, dataOperand, dataLow)`
tuples referencing ordinary GRH values. Every offset is checked; the operation
does not contain a separate computation graph. Operands/results use TwoState
Logic. These operations are owned by their dialects, not base GRH.

## Graph stages and validity

`grh-to-grhsim` sets `grhsim.stage=converted` and `grhsim.logicDomain` metadata.
The converted graph has explicit state types/InitSpec, no raw write/call ops,
no hierarchy and **no `grhsim.partition` attributes**. Optimizations preserve this.

The split/partition passes assign string-vector `grhsim.partition` owner paths:

| Stage | Complete executable owner paths |
| --- | --- |
| `converted` | none |
| `event-output-split` | `[general]`, `[event]`, `[output]` |
| `nonsink-sink-split` | General becomes `[general,nonsink|small-sink|large-sink]` |
| `nonsink-partitioned` | nonsink becomes `[general,nonsink,supernodeKey,nodeKey]` |
| `sink-partitioned` | sinks become `[general,small-sink|large-sink,groupKey]` |

Event/Output remain single leaves. Shared declarations remain outside ownership.
Every executable op has exactly one complete leaf owner; a parent container
cannot directly own ops. Detectors belong to Event, ordinary effects to General
nonsink, timeslot/history operations to Output. Pure/read dependencies do not
cross Event/General/Output; explicit Event pulses and held General samples are
the checked exceptions. Sink-private computations cannot escape their owner.
Operation, leaf and supernode dependency graphs include effect-order arcs and
must be acyclic. Top outputs/inout bindings refer to extracted Output values,
raw inputs, Event pulses or marked held samples.

`grhsim-simplify` validates attribution and optimizes each complete leaf
separately. Bare IDs and SymbolIds are not durable keys across freeze/load.
Owner key strings are stored facts and survive symbol renaming; resolved IDs
are valid only for the current instance/top/revision. Store/load preserves graph
stages, types, configuration and attributes; SinkSchedule is rebuilt by
`grhsim-partition-sink`. See [transforms](../transform/grhsim-transforms.md) for
pass options, checkpoint reruns and the schedule API.
