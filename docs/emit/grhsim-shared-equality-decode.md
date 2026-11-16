# Shared equality decoding

GrhSIM's `shared_equality_decode` option defaults to `true`. The emit attribute takes
precedence over `WOLVRIX_GRHSIM_SHARED_EQUALITY_DECODE`; setting either to `0` disables
the lowering. It changes generated computation, not the IR or activity schedule.

The lowering combines a contiguous run of ordinary `kEq` operations comparing one
scalar source with distinct constants. A single cache stores the source observed on
the last execution of that run. The Boolean result slots remain independent and
keep their original consumers and activation sets.

Region-local predicates use a separate, default-off
[local equality decoding](grhsim-local-equality-decode.md) path. This allows
compute-region fusion and decoder reuse to compose without restoring the
original per-predicate activity boundaries.

Stored predicates whose source becomes local or whose comparisons become
interleaved can use the separate, default-off
[semantic family extension](grhsim-shared-equality-families.md). It maintains the
same sparse result slots after the source is available, at the first comparison.

For example, this repeated computation:

```cpp
p0 = address == 0;
p3 = address == 3;
// ... at least 16 distinct constants
```

is implemented as:

```text
first execution: compute all predicates, compare/store/notify as before
same address: no predicate can change
different address:
    clear the predicate for the previous address, if present
    set the predicate for the new address, if present
    use each affected predicate's original change and notification logic
remember the address
```

Each group has its own previous-address and valid fields. It does not allocate a
`2^width` table or share mutable state between supernodes. `init()` invalidates all
caches. Normal and fullpass variants update the same cache; fullpass continues to
use its existing propagation suppression policy. Runtime profiling counts the
original IR work approximately; it is not an instruction counter for the cache.

## Static eligibility

- One compute supernode, contiguous pure comparisons, 16–256 distinct constants.
- Identical source SSA value, Logic width 2–12, already stored or a model input.
- Constant Logic operands have the same width; unknown constants are rejected.
  Either operand order is supported. Signed values retain equality of their bit
  representations, with no new sign extension or input truncation.
- Each result is a stored one-bit Logic value, with its own canonical Boolean
  storage slot. No aliased/reused slot, direct state view, event result or event
  source is accepted.
- No intervening operation, effect, source redefinition, or consumer is moved.
  A pending concat-prefix declaration bounds the emitted run.
- Local compute reordering, priority-mux chains, Boolean producer reduction and
  Boolean zero-group guards currently cause this rule to fall back. They require
  a separate proof of the final operation order.

The width and count limits are fixed structural policy. There is no runtime
profile, signal-name special case, candidate search, or workload-based constant
removal. Unmatched addresses clear any old match and produce no new match. A
repeated address also skips work when other inputs activate the same supernode.

## Correctness and planning

The scalar source cannot alias a Boolean destination. Only comparisons occur in
the run, so a source snapshot remains stable and no observer can see intermediate
stores. After the first execution exactly the matching result, if any, is true;
only old and new matches can change thereafter. The initial execution compares
against actual storage rather than assuming a particular Boolean initialization.

Updates call the existing scalar change/activation emitter, preserving direct,
deferred, shared and batch-trigger propagation. Events are excluded. Independent
result storage is required because another writer must not invalidate the cache's
invariant between group executions.

`analyzeExecutionPlan` reports `shared_equality_decode_groups` and
`shared_equality_decode_predicates` using the same selection as emission. It marks
the data-dependent sparse update cost as unmodeled, rather than assigning it a
constant per-IR-operation cost. The planner retains its existing explicit
unmodeled boundary for fullpass/activation-overlay variants.

The `emit-grhsim-cpp-shared-equality-decode` CTest covers initialization/reinit,
matching and unmatched/repeated addresses, signed input, spare storage bits,
clocked state changes, multiple consumer scopes, option precedence, split
emission, runtime profiling, shared deferred activation and fullpass. Negative
cases include small groups, wide domains, mixed sources, effects, event results,
duplicate or unknown constants, unequal widths and separate compute scopes.
Generated models run with Clang at O0/O3 under ASan/UBSan against independent
predicate and register-state checks.

For XiangShan this is a possible lowering for narrow CSR or enum decoders.
It does not expand a full 32-bit instruction domain, consume Chisel decode-table
metadata, or imply a measured improvement on that design.
