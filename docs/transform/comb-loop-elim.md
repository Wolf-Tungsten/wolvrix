# comb-loop-elim

`comb-loop-elim` detects combinational loops and, when possible, repairs false loops.

The important point is that this pass is now diagnostics-first:

- if it fixes a false loop, it reports that as `info`
- if it finds a loop that it cannot safely resolve, it reports that as `warning`
- it only writes a loop report to the session when you explicitly ask for one

Recent behavior change:

- before this update, resolvable false loops were repaired mainly by generic value splitting
- after this update, the pass first tries a cheaper slice-retarget fast path for loop entrances such as `slice(assign(concat(...)), const)`
- only loops that still remain after retargeting fall back to the generic split-and-rewrite path

## Python Example

```python
with wolvrix.Session() as sess:
    sess.read_sv("top.sv", out_design="design.main")
    diags = sess.run_pass(
        "comb-loop-elim",
        design="design.main",
        out_comb_loop_report="loops.main",
        fix_false_loops=True,
        max_fix_iterations=100,
        fail_on_true_loop=False,
    )
    wolvrix.print_diagnostics(diags, min_level="info")
```

If you omit `out_comb_loop_report`, the pass still returns diagnostics, but it does not silently create a session value.

## What Counts As A Loop

At a high level, the pass:

1. builds a dependency graph over values
2. runs Tarjan SCC detection
3. treats constants, register read ports (including `kRegisterReadArrayPort`), latch read ports, and DPI-C calls as boundaries
4. performs a finer-grained bit-range analysis for possible false loops

If the apparent loop is only caused by non-overlapping bit usage, the pass can split values and repair it. That is considered a false loop.

## Array Values And Fragmentation

Array values (values with a non-empty `shape`) participate in loops like any
other value: shape is a pure structural view, so the bit-range machinery works
on the flattened bits. The array-specific rules are:

- `kSliceArray` with constant in-bounds indices is analyzed exactly: the
  selected element/sub-array maps to a row-major bit range of the base value,
  so cross-element false loops such as `a[i] -> b[i] -> a[j]` (with disjoint
  elements) are classified and repaired like scalar bit-slice false loops.
- `kSliceArray` with a dynamic (or out-of-bounds) index is modeled
  conservatively: the read may touch **any** element, so the base array
  collapses to a single whole-array interval in the range analysis. A loop
  that still shows a range-level cycle under this model is reported as
  undecidable (`dynamic-array-read` note) instead of being split blindly.
- When such a loop is acyclic at range granularity (the read result never
  feeds back into the base array), the split repairs it: the dynamic read
  result stays one whole fragment and the read is re-pointed at the
  reassembled full view of the split base ("whole fragment + dynamic read
  merge"). This is recorded in the stats as `dyn-array=N`.

Fragmentation details:

- fragments of scalar values are bit ranges (the pre-existing mechanism);
  element boundaries of arrays simply become cut points
- a fragment that still covers a shaped value as a whole keeps the array
  shape, and reassembled full-view values inherit the original shape, so
  downstream `kSliceArray` users and port bindings stay verifier-legal
- `max-fragments-per-value` (default 4096) bounds fragmentation; a loop that
  would exceed the cap is left intact and reported unresolved
  (`frag-capped=N` in the stats line)
- `-no-array-fragmentation` restores the pre-stage-05 behavior (any
  `kSliceArray` aborts the loop analysis as incomplete);
  `-no-fix-dynamic-array-loops` keeps the analysis but never rewrites loops
  that contain a dynamic array read

Splitting is always equivalence-preserving; a true loop whose bits genuinely
feed back survives splitting and is re-detected on the next iteration, then
reported as a true loop per `-fail-on-true-loop`.

## False-Loop Repair Strategy

The pass now repairs false loops in two stages:

1. retarget loop-entry slices
2. if that is insufficient, split loop values into bit-range fragments and rewrite the affected ops

The first stage handles slice-like entrances in an already identified false loop:

- `kSliceStatic`
- `kSliceDynamic` with a constant index
- `kSliceArray` with in-bounds constant indices
- retarget through `kAssign`
- retarget through `kConcat`

When the selected slice fully covers a concat operand, the pass can rewrite:

- to `kAssign` if the whole operand is selected
- to `kSliceStatic` if only part of the operand is selected

For static and dynamic bit slices, retargeting rejects a concat operand that
already depends on the result. A constant array read may select such an operand:
the dependency was already present through the concat, so selecting the same
element directly does not add one. A direct self-assignment is still rejected.

For example, if `a` has shape `[2]` and contains two 6-bit elements,
`a = concat(hi, lo)`, then `kSliceArray(a, 1)` selects bits `[11:6]` and can
become `kAssign(hi)`. If an intervening `kAssign` connects `a` to a flat
12-bit value without the same shape, the array read keeps `a` as its base and
becomes `kSliceStatic(a, 6, 11)` inside this false-loop repair. This local
fallback does not change dynamic array reads or normalize constant array
reads elsewhere in the graph.

## Carry-Chain Example

This pattern appears in HDLBits `043` after `hier-flatten`:

```text
cout_temp = concat(stage100_cout, ..., stage2_cout, stage1_cout)
stage2_cin = slice(cout_temp, 0)
stage3_cin = slice(cout_temp, 1)
...
stage100_cin = slice(cout_temp, 98)
```

At whole-value granularity this looks cyclic:

```text
cout_temp -> slice -> stageN_cin -> ... -> stageN_cout -> concat -> cout_temp
```

But it is actually a false loop, because each `stageN_cin` only depends on one disjoint bit of `cout_temp`, and that bit comes from the previous stage's `cout`.

## What Actually Changed

The key difference is not "both rewrite the loop". The key difference is **where** the rewrite happens.

- before the fix: rewrite the **whole SCC representation**
- after the fix: rewrite only the **loop entrance slices**

For the same carry-chain false loop, the two strategies look like this.

### Before: Rewrite The Whole SCC

Input shape:

```text
cout_temp = concat(cout_100, ..., cout_2, cout_1)
cin_2 = slice(cout_temp, 0)
cin_3 = slice(cout_temp, 1)
...
```

Old repair strategy:

```text
tmp_cout_temp_bit_0 = cout_1
tmp_cout_temp_bit_1 = cout_2
...
cin_2 = tmp_cout_temp_bit_0
cin_3 = tmp_cout_temp_bit_1
...
cout_temp_full_view = concat(..., tmp_cout_temp_bit_1, tmp_cout_temp_bit_0)
```

That is, the old path created new temporary values for bus fragments, rewired the loop to use those temporaries, and then rebuilt a full-bus view with another `concat` if needed.

So the repair touched:

- the bus being read from
- the bus being written back
- many intermediate values in the SCC
- extra reassembly `concat` ops when a full-value view was still needed

### After: Rewrite Only The Entrances

Input shape:

```text
cout_temp = concat(cout_100, ..., cout_2, cout_1)
cin_2 = slice(cout_temp, 0)
cin_3 = slice(cout_temp, 1)
...
```

New repair strategy:

```text
cout_temp = concat(cout_100, ..., cout_2, cout_1)
cin_2 = cout_1
cin_3 = cout_2
...
```

That is, the pass does **not** first split `cout_temp` into fragments. It directly rewrites each loop-entry slice to the specific concat operand that actually feeds that bit.

So the repair touches only:

- the `slice(cout_temp, const)` entrances

and does **not** rewrite the whole SCC into fragment form.

### Short Version

If the previous wording felt like "the same thing", use this rule of thumb:

- before: "break the bus apart, then rebuild the loop around fragments"
- after: "leave the bus alone, only change `cin_i` from `slice(bus, i)` to the exact source bit producer"

## Concrete Two-Stage Example

Here is the smallest useful example that matches the HDLBits `043` carry-chain shape.

Assume the flattened IR is conceptually:

```text
cout_1 = f(cin)
cin_2 = slice(cout_temp, 0)
cout_2 = f(cin_2)
cout_temp = concat(cout_2, cout_1)
```

### Why This Looks Like A Loop

At whole-value granularity, the dependency graph looks like:

```text
cout_temp -> cin_2 -> cout_2 -> cout_temp
```

So a loop detector that only sees whole values will say:

```text
"there is a loop through cout_temp"
```

### Why It Is Actually A False Loop

The important detail is that `cin_2` does **not** read all of `cout_temp`.
It only reads bit 0:

```text
cout_temp[0] = cout_1
cout_temp[1] = cout_2
cin_2       = cout_temp[0]
```

So in reality:

```text
cin_2 = cout_1
cout_2 = f(cout_1)
```

There is no real combinational self-feedback. The apparent loop only comes from representing the two bits as one whole bus value.

### Before The Fix

The old repair path handled this by fragmenting the whole loop:

```text
tmp_cout_temp_bit_0 = new temporary value for bit 0 of cout_temp
tmp_cout_temp_bit_1 = new temporary value for bit 1 of cout_temp

tmp_cout_temp_bit_0 = cout_1
tmp_cout_temp_bit_1 = cout_2

cin_2 = tmp_cout_temp_bit_0
```

And if some later user still needed the full bus, it could also create:

```text
tmp_cout_temp_full = concat(tmp_cout_temp_bit_1, tmp_cout_temp_bit_0)
```

This is the important part:

- the pass created new temporary values for bus fragments
- then rewired the loop to use those temporaries
- so even though only `cin_2 = slice(cout_temp, 0)` was problematic, the old repair path rewrote the bus representation itself

### After The Fix

The new repair path looks at the same entrance:

```text
cin_2 = slice(cout_temp, 0)
cout_temp = concat(cout_2, cout_1)
```

and directly notices:

```text
bit 0 of cout_temp comes entirely from cout_1
```

So it rewrites only that entrance:

```text
cout_1 = f(cin)
cin_2 = cout_1
cout_2 = f(cin_2)
cout_temp = concat(cout_2, cout_1)
```

No fragmentation of `cout_temp` is needed.

### Why This Matters For `043`

`043` is just this same pattern repeated many times:

```text
cin_2  = previous cout bit
cin_3  = previous cout bit
...
cin_100 = previous cout bit
```

So:

- the old path rewrote a large false-loop SCC into fragment form
- the new path rewrites each entrance directly to the exact previous-stage `cout`

That is why the old behavior reported:

```text
split-values=794 split-ops=7031
```

while the new behavior reports:

```text
retargeted-slices=297 split-values=0 split-ops=0
```

### Behavior Before The Fix

Previously `comb-loop-elim` repaired this kind of loop by generic fragmentation:

- split loop values into many bit-range fragments
- rewrite the carry cone into fragment-level ops
- let later passes try to clean up the expanded IR

That means the pass did not preserve the original carry-chain shape. Instead of keeping:

```text
stage2_cin = slice(cout_temp, 0)
stage3_cin = slice(cout_temp, 1)
...
```

and then simplifying those entrances directly, it first decomposed loop values such as `cout_temp` into many bit-range fragments:

```text
tmp_cout_temp_bit_0 = bit 0 of cout_temp
tmp_cout_temp_bit_1 = bit 1 of cout_temp
...
```

Then it rewrote the loop-related ops so they operated on fragments instead of on the original values:

- `slice` became fragment-to-fragment copies or smaller static slices
- `concat` became fragment assembly ops
- `assign` was duplicated at fragment granularity
- when a full-value view was still needed, the pass created a new `concat` to stitch all fragments back together

For a carry chain like HDLBits `043`, this is much heavier than necessary. The false loop is only at the bus entry points, but the generic split path rewrote a large part of the carry cone as if the entire SCC needed fragment-level normalization.

For DUT `043`, that produced diagnostics like:

```text
split-values=794 split-ops=7031
```

### Behavior After The Fix

Now the pass first rewrites the loop entrances directly:

```text
stage2_cin = stage1_cout
stage3_cin = stage2_cout
...
```

So the false loop is removed without fragmenting the whole carry chain. For the same DUT `043`, the pass now reports:

```text
retargeted-slices=297 split-values=0 split-ops=0
```

In practice this means:

- `comb-loop-elim` no longer causes the main IR blow-up for this pattern
- `simplify` has much less cleanup work afterward
- later passes such as `activity-schedule` start from a much smaller graph

## Session Output

Optional output:

- `out_comb_loop_report`
  - kind `comb-loop.reports`

This is mainly for later inspection or debugging. The primary output of the pass is still diagnostics.

## Key Options

| Option | Default | Meaning |
| --- | --- | --- |
| `-max-analysis-nodes` | `0` | maximum nodes analyzed per graph, `0` means unlimited |
| `-num-threads` | `0` | analysis thread count, `0` uses hardware concurrency |
| `-fix-false-loops` / `-no-fix-false-loops` | true | whether to repair resolvable false loops |
| `-max-fix-iterations` | `100` | maximum repair iterations |
| `-fail-on-true-loop` | false | treat true loops as pass failure |
| `-enable-array-fragmentation` / `-no-array-fragmentation` | true | array-aware loop analysis (`kSliceArray`); off restores the legacy "uncertain" behavior |
| `-fix-dynamic-array-loops` / `-no-fix-dynamic-array-loops` | true | attempt whole-fragment repair of loops containing a dynamic-index `kSliceArray`; off reports them as undecidable |
| `-max-fragments-per-value` | `4096` | fragment cap per value while splitting; `0` means unlimited; capped loops are left intact |
| `-output-key` | empty | session key for an explicit loop report |

The Python bindings expose the same knobs as `enable_array_fragmentation`,
`fix_dynamic_array_loops`, and `max_fragments_per_value` named arguments.

## Diagnostic Raise-Level Interaction

This pass works especially well with the session-level diagnostic raise threshold.

For example:

- with `sess.diagnostics_raise_min_level = "warning"`, any unresolved loop warning becomes an exception
- with `sess.diagnostics_raise_min_level = "error"`, warnings are returned but do not automatically raise

That keeps the pass behavior easy to reason about:

- diagnostics describe what happened
- session raise level decides whether that should stop execution

## Notes

- large designs may need `max_analysis_nodes` tuning
- repairing false loops rewrites the IR
- false-loop repair is no longer synonymous with value splitting; diagnostics may now show `retargeted-slices > 0` with `split-values = 0`
- a true combinational loop usually indicates a real design problem
