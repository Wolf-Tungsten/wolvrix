# Event regions in activity-schedule

`ActivityScheduleOptions::eventRegions` (`-event-regions true`, Python
`event_regions=True`) is a default-off schedule transformation. It coalesces a
pure terminal compute cone whose complete downstream use is one register commit
group with a uniform event signature. The input graph and register write lists
are unchanged.

The separate [shared-region extension](activity-shared-event-regions.md),
`eventRegionsShared`, is also default-off and handles pure logic with multiple
endpoints after this terminal-region pass.

The purpose is to remove unnecessary intermediate value storage and old/new
comparisons. After a boundary becomes internal to one compute supernode, the
existing GrhSIM emitter can use local values instead of persistent activity
slots. This is a schedule change, not a change to the per-value OR helper or to
the number of supernodes packed in a C++ file.

## Eligibility and construction

The transform uses complete compute nodes from the existing schedule:

1. Classify terminal uses as one commit group, multiple/incompatible terminals,
   or a barrier. A commit group is eligible only when all its operations are
   `kRegisterWritePort` with identical nonempty event values and edge kinds.
2. Mark public output/inout values, event expressions, non-Logic values,
   side effects, memory/latch reads and reg-to-mem intent nodes as barriers.
   An entire original compute supernode containing observable compute effects
   is protected, preserving its activation context and operation order.
3. Propagate terminal classifications backwards through the compute-node DAG.
   Collect all nodes whose only terminal is the same eligible commit group.
   Mixed-terminal logic remains on the existing activity path.
4. Coalesce a collected region only if it crosses at least two original
   supernodes and contains at most 8,192 operations. This fixed bound controls
   code-generation size. Oversize regions fall back as a whole; there is no
   profile-based selection or parameter search.
5. Order a new region from its externally consumed results backwards: visit
   pure dependencies at first use and emit each shared producer once. This
   can avoid long intermediate lifetimes of a breadth-first topological layer
   order. Retain all operations and validate producer-before-consumer order.
   The local def-to-last-use distance is recorded for diagnosis, not used as a
   performance admission rule; it does not model compiler register allocation.
6. Retain all other compute-node groups and rebuild value fanout, DAG,
   input/state activation information. Verify the exact scheduled operation
   set. Copy commit groups and their baseline execution order without changing
   any write operation. Unchanged compute groups retain their exact op order.

The complete single-terminal set is closed towards compute consumers. A region
cannot have a compute successor outside that set: such a successor would either
have the same sole terminal and belong to the region, or add another terminal /
barrier to the producer. This property prevents a new quotient-DAG cycle when
the set is coalesced. A final topological rebuild still validates the result.

The transformation requires whole compute-node ownership; schedules using
oversize-node splitting are rejected when this option is enabled. Other
schedule rewrites that split a node's membership likewise cannot be combined
with this path.

## Runtime semantics

The region is a compute supernode with the existing activity-triggered entry.
It is **not** a new unconditional clock-only execution mode. Boundary input and
state changes activate the region; internal pure computation executes in
topological order. Values used by commit remain persistent as before.

All compute executes before all commits, preserving the old-state snapshot.
No next-state expression is interleaved with a state update. Clock edge
detection, latch transparency, asynchronous reset, commit write conflict rules,
event clearing, and fixed-point iteration use the existing implementation.
No memory intent or partition communication boundary is fused by this option.

For example, `r0_next = f(r1, x)` and `r1_next = g(r0, x)` with the same event
may share one region. Both functions read the old `r0/r1`; the existing commit
phase then writes both next states. A public combinational output or a second
clock consuming part of the expression keeps the corresponding producer on
the shared boundary path.

## Validation

`emit-grhsim-event-regions` generates default/off/on models and checks
default/off source identity. Its independent C++ oracle exercises two clocks,
a transparent clock-gate latch, asynchronous reset, public combinational
outputs, changing inputs with clocks held, and reciprocal state dependencies.
Generated off/on models run at O0 and O3 with ASan/UBSan. `CXX` selects the
compiler used for these generated-model checks.

Schedule-only checks also verify that an effect-containing original group keeps
its complete membership and order while other regions still form; an 8,300-op
terminal cone falls back without changing groups, fanout or topological order;
actual oversize-node splitting is rejected; and malformed CLI Boolean options
fail explicitly.

The option is experimental and remains default-off. A workload measurement
must account for extra recomputation and register pressure in larger regions,
not just the decrease in boundary values.

Fusion can also internalize previously cached decoders. The slot-based shared
equality lowering then becomes ineligible. [Local equality decoding](../emit/grhsim-local-equality-decode.md)
is a separate default-off way to preserve reuse without rebuilding activity
boundaries. Always check both boundary work and repeated local computation.

The diagnostic `local_value_live_steps_*` sums def-to-last-local-use distances
for non-constant, non-register-read results before and after region ordering.
It is an IR lifetime proxy, not a count of machine registers, spills or cycles.
