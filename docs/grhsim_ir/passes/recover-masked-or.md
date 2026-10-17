# Mask/array OR recovery (B7)

`cpu.st.recover-masked-or` is a `SemanticTransform`. It restores an expanded
selection reduction as [`cpu.compute.maskedOr`](../dialects/cpu.md#2-cpucomputemaskedor),
preserving zero-hit and multi-hit OR behavior. It does not require a one-hot
selector.

Run it after `grhsim.split-phases` and the phase simplify, before
`grhsim.verify --seal semantic` and C1. Every input op must have a phase;
matching is confined to Event, General, and Output separately. A transformed
model invalidates existing mappings, so archived mapped checkpoints must go
through semantic verification and C1–C8 again.

## Operands and example

The result is two-state logic with total width W>=1. Signedness and shape
are retained. Dense recovery uses two operands:

1. The original two-state N-bit scalar selector, N>=2.
2. A two-state rank-one array, total width N*W and shape [N].

For each canonical index i, the original tree must contain exactly one of:

```text
and(sliceStatic(mask, i, i), readArray(@rows, constant(i)))   // W=1
mux(sliceStatic(mask, i, i), sliceArray(data, constant(i)), zero<W>)
and(replicate(sliceStatic(mask, i, i), W), sliceArray(data, constant(i)))
```

Either array element form (`readArray` or `sliceArray`) works in each pattern;
the operands of AND can be swapped. One-bit `bitSelect` is also accepted.
When all terms use the same mask/array, cover 0..N-1 once and pair mask bit i
with element i, the compact dense encoding is used. OR order may differ.

```text
%a = and(sliceStatic(%mask, 0, 0), readArray(@rows, 0))
%b = and(sliceStatic(%mask, 1, 1), readArray(@rows, 1))
%c = and(sliceStatic(%mask, 2, 2), readArray(@rows, 2))
%d = and(sliceStatic(%mask, 3, 3), readArray(@rows, 3))
%r = or(or(%a, %b), or(%c, %d))
=>
%data = core.state.read {@rows}  // same phase, shape [4]
%r = cpu.compute.maskedOr(%mask, %data)
```

Other layouts use the same op with a static `views` int-vector parameter.
Each tuple is `[selectOperand, selectBit, dataOperand, dataOffset]`; it selects
one bit and W consecutive data bits from the op's existing value operands.
The [dialect contract](../dialects/cpu.md#2-cpucomputemaskedor) defines every
operand and offset. No runtime data array is assembled.

The matcher normalizes equal-width assign, rep=1, full slices and shape/sign
views. Bit ranges trace through static slices, constant multidimensional
array indices, concat and slices contained in one replication. It accepts
partial arrays, rotations, arbitrary permutations, repeated indices,
independent selectors/states and computed data. Wide AND recognizes both
replicate(bit,W) and signed one-bit assignment extended to W bits. Broadcast
masks nested inside associative ANDs are factored out; the remaining data
factors are combined with same-phase core AND ops. For example,
`(a & replicate(s,W)) & b` becomes a term selecting `a & b` with bit `s`.
No data array is introduced, and shared/observed original factors survive.
Data wider than the result is viewed through its low W bits, preserving
truncation at mux/AND results. Narrower data requiring extension is retained.
A mux
must have a one-bit selector and zero false arm; arbitrary multi-bit
bitSelect masks are not treated as Boolean selectors.

N and W may exceed 64. Selectors are scanned in 64-bit segments. Existing
computed producers still run normally; their cost does not disappear merely
because the following reduction uses a sparse mask.

For state element reads, the pass adds a whole-state read per region. It
does not share this new aggregate value across regions, which would create
an aggregate boundary. CPU code generation can index the stable state rows
directly; General observes the old state before commit and Output observes
the state for that phase. Existing array value operands retain their original
producer and snapshot.

## Preservation and cleanup

The matcher builds maximal same-bit-width OR trees using producer/use indices
and an explicit stack. Shape and signedness may differ when bit widths match.
Validity is computed bottom up; failed outer regions are searched for maximal
valid subreductions without repeatedly flattening the whole prefix chain.
Only single-use, unobserved internal OR/alias values are absorbed.
Shared prefixes and every `DeclProvenance` Value target form boundaries;
their values and associations remain intact. A shared/observed prefix can be
recovered independently while its users retain that value. No view is traced
across a phase boundary. Four-state logic, width-changing OR semantics and
nonzero mux false arms are not absorbed.

The root value, phase, and origin remain. Afterwards only now-unused pure
producers and reads reached from the replaced region are removed, within
the same phase. Shared or observed ANDs, slices, and reads survive. State
writes, events, calls, and tasks are never absorbed or deleted. Compaction
updates IDs/provenance; the pass manager commits the semantic revision.
A no-match invocation leaves the semantic revision unchanged.

CPU pure-compute classification recognizes the new op. C1/C2 charge 2N-1
primitive operations for capacity estimates, preventing one recovered region
from bypassing the compute size cap. C6/C8 additionally account for its loop
in emitted-size estimates. Used-bits treats its operands/results as fully
used; constant folding and CSE may conservatively leave it opaque.

## Options and reporting

| Option | Default | Meaning |
| --- | --- | --- |
| `--min-terms` | 16 | Minimum number of terms, in [2,2147483647]; not OR-node count |
| `--report` | absent | Accepted-region TSV and `<path>.candidates.tsv` decisions |

Diagnostics report maximal OR candidates inspected, recovered regions/terms,
regions per phase, removed ops and removed element reads. Candidate count
includes ordinary small ORs that do not meet the mask/array pattern. The region
TSV includes N, W, dense/views mode and distinct selector/data sources. The
candidate TSV includes every maximal tree at/above the threshold, its
structural rejection category and recovered subregion count. An unmatched
leaf can be unmasked logic or a preserved shared/observed OR boundary. TSV IDs
refer to the input before compaction. The independent playground audit verifies
all accepted trees and candidate decisions against that original graph.

The playground flow enables B7 by default after the preserve-aggregate CoreMark
100k validation (three alternating pairs: mean Host time 350.391 → 340.013 s).
Set `XS_WOLF_GRHSIM_IR_MASKED_OR=0` to disable recovery for diagnostics.
`XS_WOLF_GRHSIM_IR_MASKED_OR_MIN_TERMS` defaults to 16 and
`XS_WOLF_GRHSIM_IR_MASKED_OR_REPORT` selects the TSV path. The Python entry
accepts `--recover-masked-or` / `--no-recover-masked-or`,
`--masked-or-min-terms`, and `--masked-or-report`.

For archived checkpoints use the playground Makefile:

```sh
make reemit_grhsim_ir \
  GRHSIM_REEMIT_MODEL=build/xs/grhsim-ir-aggr-global-enhanced/xiangshan_grhsim_ir.json \
  GRHSIM_REEMIT_FLOW=ptmp/masked-or-xs GRHSIM_REEMIT_MASKED_OR=1
```

This writes `masked-or.tsv`, remaps once after all selected transforms, and
emits C++. Functional tests use `make test_grhsim_masked_or`; the isolated
array benchmark uses `make bench_grhsim_masked_or`. Sparse scanning reduces
selected row loads, but dense masks can regress; fewer IR ops or C++ lines
alone do not establish a simulation speedup.

On the initial, narrower matcher, 220 Xiangshan regions were recovered
(Event 20, General 200), all N=48 and W=1. All 220 emitted scans directly
reference the state arrays. A subsequent C++ census reduced the number of
serial Boolean chains with >=16 operators from 2,108 to 1,892, including
Event 21 to 1. IR regions and serial C++ chains have different counting rules.

With view/width/AND-factor recovery, the same checkpoint recovers 4,217
regions (Event 25, General 4,192), including 641 with N>64 and 27 with W>64.
All 3,440 structural candidates from the original 16..64-term, W<=64 audit
are covered, either directly or inside a larger normalized region. This is
an IR candidate denominator, not a Chisel call-site coverage percentage.
The generated C++ has 636 serial Boolean chains with >=16 operators
(Event 0, General 632, Output 4). Full-workload performance remains unmeasured.
