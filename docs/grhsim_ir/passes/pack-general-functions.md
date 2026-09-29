# General function packing (helper chunks and emit functions)

`cpu.st.pack-general-functions` is the fourth pass of the M3 six-phase CPU
mapping pipeline. It replaces the legacy `cpu.st.pack-active-words` +
`cpu.st.pack-emit-functions` pair. Requires a `GeneralSupernodes`-stage
mapping; produces the `GeneralFunctions` stage — the last six-phase mapping
stage in M3.

Two steps:

1. Helper chunks: every General supernode gets `helperChunks` ranges split by
   the legacy `estimatedLines` heuristic against
   `--helper-max-estimated-lines` (default 2048). Ranges index into the
   supernode's flattened op order and must cover it contiguously. There is no
   `ActiveWord` layer: the six-phase model gates whole supernodes through a
   per-supernode byte array instead of packing eight supernodes per 64-bit
   activity word, and the partition tree never contains `ActiveWord` (or
   `EventDomain`) partitions.
2. Emit functions: the General branch batches its supernodes into
   `EmitFunction` partitions with the legacy thresholds
   `--batch-max-ops` (2048), `--batch-max-estimated-lines` (8192) and
   `--target-batch-count` (64; with a nonzero target the effective limits are
   `max(limit, total/target)` over the General branch). The flat `Event`,
   `Mem` and `Output` branches each collapse into exactly one `EmitFunction`
   leaf holding the branch's ops — one task per branch — even when the branch
   is empty.

The resulting tree shape:

```text
root
├─ phase Event  └─ EmitFunction { flat event ops }        ; cone, then edgeDets
├─ phase General└─ EmitFunction+ └─ Supernode+ └─ Node+   ; batched by thresholds
├─ phase Mem    └─ EmitFunction { mem writes }            ; op-id order
└─ phase Output └─ EmitFunction { flat output ops }
```

The `eventActs` annotations produced by `cpu.st.merge-general-supernodes`
stay on the supernodes. Diagnostics: the standard `partitions` count only.
