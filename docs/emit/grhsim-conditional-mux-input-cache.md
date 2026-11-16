# Input caching inside conditional compute regions

`conditional_mux_input_cache` / `WOLVRIX_GRHSIM_CONDITIONAL_MUX_INPUT_CACHE`
is experimental and defaults to off. The emit attribute overrides the environment.
It requires [conditional mux cones](grhsim-conditional-mux-cones.md). Turning off
the parent disables this extension as well.

Region merging removes many intermediate stored values and notifications, but
also combines activation conditions. A region can execute for a frequently
changing input while an expensive expression inside it has unchanged inputs.
This extension retains the existing region and caches the exact read set of an
already selected conditional cone. Its existing persistent result serves as the
cached output; local temporaries never survive across calls.

For `kMux`, operands are `[condition, true_value, false_value]`, and the single
result selects the true value when the condition is nonzero. For example:

```text
before: result = condition ? private_chain_a(state, input) : private_chain_b(state, input)
after:
    keys = exact_read_set(condition, private_chain_a, private_chain_b)
    if invalid or keys differ from previous_keys:
        result = condition ? private_chain_a(state, input) : private_chain_b(state, input)
        compare/store/notify result using the existing lowering
        previous_keys = keys
        valid = true
    consumers read the original result slot
```

The key includes dependencies of both arms, including the currently unselected
arm. Local pure expressions are followed to their persistent inputs, model
inputs, or ordinary register reads. Constants need no key. Two local, operandless
reads of the same register with the same width and signedness share a key only
inside the already proven pure region: no write can occur between them. This
handles scheduler source clones without treating unrelated states as equal.

## Safety and fixed bounds

- Existing conditional cone membership, purity, alias, private-use and scalar
  lowering proofs remain prerequisites. Each selected arm has 16–256 operations.
- The root must own one exclusive, canonical persistent scalar slot. Direct
  state views, inverted views, shared storage and local-only results are excluded.
- The complete input tuple has 1–4 exact scalar values, each at most 64 bits.
  There is no hash or collision assumption. Dependency expansion stops after
  32 levels or 256 visited values, falling back if proof exceeds those bounds.
- A hit skips both the conditional body and its redundant result change check.
  Since the slot cannot have another writer, no result change or notification
  is lost. On a miss the original conditional body and notification logic run.
- `init()` invalidates all caches. Ordinary and fullpass bodies share the cache;
  their existing downstream activation policy remains unchanged.
- Local compute reordering, priority mux chains, Boolean producer reduction and
  Boolean zero-group guards disable this extension until their plans compose.

This does not restore every pre-merge activation condition or split a region.
Expressions without an existing persistent result remain ineligible. Checking
and updating keys costs time, so eligibility alone does not establish a speedup.
The bounds are fixed structural rules, not selected using runtime profiles.

## Validation and measurement

`emit-grhsim-mux-input-cache` compares generated code to an independent oracle
with held clocks, input changes, state updates, repeated inputs and reinitialization.
It checks cache hits under fullpass and covers default/off identity, parent
disable, environment precedence, distinct register dependencies, effects,
observable intermediates, small cones and split regions. Generated code runs at
O0 and O3 with ASan/UBSan. The original conditional mux suite also applies.

The execution plan reports `conditional_mux_input_cache_roots` and
`conditional_mux_input_cache_keys`, marking input-dependent work as unmodeled.
Diagnostic counts must distinguish visits, misses, selected arm executions and
result changes. IR operations are not machine instruction counts.

The rule is independent of CV32E40P signal names. XiangShan control cones with
stable read sets and exclusive stored outputs may qualify under the same proof.
Cross-partition scheduling and memory accesses remain outside this extension;
there is no measured XiangShan performance claim.
