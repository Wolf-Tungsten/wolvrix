# Conditional evaluation of private scalar mux cones

`conditional_mux_cones` / `WOLVRIX_GRHSIM_CONDITIONAL_MUX_CONES` is an
experimental, default-off GrhSIM emitter option. It does not modify the IR or
activity schedule.

The optional [input cache](grhsim-conditional-mux-input-cache.md) can reuse a
cone's persistent result when its exact read set is unchanged. It defaults to
off and retains the purity and ownership requirements below.

A scalar mux has operands `(condition, true_value, false_value)`. The ordinary
lowering emits both operand producers before a mask-select helper. For example:

```cpp
auto a = expensive_a(inputs);
auto b = expensive_b(inputs);
auto result = grhsim_mux_u64(condition, a, b);
```

The option moves eligible private producers inside a conditional at the original
mux position:

```cpp
uint32_t result;
if (condition) {
    // Private producers of a, in their original order.
    result = expensive_a(inputs);
} else {
    // Private producers of b, in their original order.
    result = expensive_b(inputs);
}
```

Either or both arms can qualify. An ineligible arm keeps its original eager
computation. Shared producers stay eager; the rule does not duplicate them.
Regular scalar lowering retains truncation, signed extension, and oversized
shift semantics inside each branch. The C++ compiler remains free to simplify
or if-convert the result; source branching alone is not proof of a runtime gain.

Eligibility is independent of signal names and profiles:

- The root and its arms have equal scalar Logic widths. Each moved operation
  has one scalar result and scalar operands, and supports ordinary scalar
  lowering. Constants and register reads remain leaves.
- Each moved value has exactly one operand use in the emitted supernode and no
  cross-supernode consumer. Dead unscheduled IR users do not count as runtime
  uses. Materialized, tracked, public output/inout, event, and storage-aliased
  values remain eager. Waveform-observed values retain their required storage.
- The entire compute group must contain only pure operations and register
  reads. Memory, transparent latches, effects, reg-to-mem intents and behavior
  groups cause fallback. Canonical storage must have a unique producer and no
  read before its producer. The rule never moves work into commit or across a
  state write.
- Existing packed reduction inputs/results, indexed concat slices, and prefix
  cache inputs stay eager. Priority-mux and Boolean group lowerings are not
  combined with this experiment.
- Each arm has 16–256 private operations. Reverse schedule order gives outer
  muxes precedence over nested muxes. Oversized arms are rejected whole; their
  smaller nested roots can independently qualify. There is no tunable size
  search in the CV32E40P experiment.

The execution-plan snapshot reports conditional roots and absorbed operations,
and marks their cost as selector-dependent rather than pricing every operation
as unconditional work. Runtime diagnostics must count branch executions before
converting these upper bounds into executed operation counts.

The block is emitted directly in the compute function. A capturing lambda can
be outlined by the host compiler, introducing closure construction, pointer
loads and spills around a new call boundary. The initial CV32E40P implementation
exhibited this failure; the direct block retains the same structural plan.

Regression target: `emit-grhsim-conditional-mux`. It compares default/off source,
checks both branches against an independent arithmetic/state oracle at Clang
O0 and O3 with ASan/UBSan, and exercises protected/shared values, cross-group
uses and structural bounds. The option remains off until workload measurements
and broader composability evidence justify adoption.
