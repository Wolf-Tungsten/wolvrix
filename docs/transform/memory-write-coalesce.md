# memory-write-coalesce

`memory-write-coalesce` combines independent constant-mask memory writes whose
enables are structurally equivalent. It is an explicit GRH transformation, not
an emitter rewrite or a module-name heuristic. The generic library does not run
it implicitly. The XiangShan RepCut flow enables it after partitioning by default;
set `XS_REPCUT_MEMORY_WRITE_COALESCE=0` to disable it in that flow.

```python
sess.run_pass("memory-write-coalesce", design="design.main")
```

The pass takes no arguments. Run it after flattening and normalization have
exposed the write ports, before emitting the design. In a partitioned design it
operates within each graph; it does not move storage or change partition ports.

## Operation and example

`kMemoryWritePort(enable, address, data, mask, events...)` conditionally updates
the selected bits of a memory row. The `memSymbol` attribute identifies the
memory; `eventEdge` identifies the edge of each event operand. Data and mask have
the full memory width, and enable is one bit.

For example, let `m = {4{select}}`. Four independent operations

```text
write(enable & m[i], address, zero_extend(data[i]) << i, 4'b0001 << i, clock)
    for i = 0, 1, 2, 3
```

can become

```text
write(enable & select, address, data, 4'b1111, clock)
```

The pass traces bits through same-width assignments, static slices, replication,
concatenation and supported unsigned constant shifts. It recognizes identical
scalar boolean expression trees after this routing normalization. It does not
use two-state identities such as `x & ~x == 0`. Data is reconstructed with slices
and concatenation, preserving X and Z rather than combining payloads with OR.
Sparse masks are allowed; unselected memory bits are left unchanged.

## Conservative boundaries

For a memory to be considered, all of its write ports must have known constant
masks that do not overlap, even across different enables, addresses or events.
The pass skips memories with priority attributes, fill ports, other unrecognized
writers, unsupported write attributes, or unsupported dependencies. This avoids
changing write conflict semantics or side-effect ordering.

Within an eligible memory, a group requires the same normalized enable, the same
address value (possibly through assignments of identical width and signedness),
and exactly the same event values and edge strings. Different groups remain
separate. When declared symbols are preserved, declared write operations are
excluded from groups but still participate in the whole-memory safety checks.

Supported combinational dependencies are checked recursively. Register and latch
reads are stable state leaves; memory reads also require pure address expressions.
DPI calls, instances, system functions, unknown operations and unknown attributes
are rejected. The exact packed-array layout annotations produced by ingest are
accepted on concatenations as advisory metadata; those annotated concatenations
are retained intact. Depth limits conservatively reduce matching coverage.

The pass retains one representative write per group, changes its data and mask,
and removes the remaining writes. It does not remove unused combinational nodes;
a subsequent cleanup pass or the simulator backend may remove them.

## Diagnostics and verification

Each graph reports `ports_before`, `ports_after`, `groups`, `removed`, and
`skipped_<reason>` counts. Removed write operations are a structural metric, not
an estimate of dynamic speedup. Verify generated code and end-to-end runtime with
the chosen simulator.

The `transform-memory-write-coalesce` test covers four-state data and enables,
several widths, sparse masks, constant shifts, advisory metadata, idempotence,
and rejection of unsafe cases. It also emits before/after SystemVerilog fixtures
for simulator-level comparisons.
