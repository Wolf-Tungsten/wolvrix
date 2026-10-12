# mem-to-reg

`mem-to-reg` replaces an eligible small `kMemory` with one `kRegister` per row,
address comparisons and a read mux. Run it explicitly before `repcut` to expose
register boundaries for selected memories; the selection option does not enable
the pass automatically in any pipeline.

```python
sess.run_pass(
    "mem-to-reg",
    design="design.main",
    row_limit=2,
    memory_symbols=["queue$ram_ext$Memory"],
)
```

| Option | Default | Meaning |
| --- | --- | --- |
| `row_limit` | `32` | Maximum memory depth eligible for lowering. |
| `strict_init` | `False` | Skip memories without explicit literal initialization attributes. |
| `memory_symbols` | unrestricted | Exact memory operation symbols to consider. Accepts a string or iterable of strings. |

An omitted selection or an empty list preserves the existing behavior: consider
all eligible memories in every graph. A nonempty selection restricts conversion
but does not bypass eligibility checks. Matching is exact, without wildcards;
symbols absent from the design do not produce an error. Inspect the pass log's
`converted` and `skipped_unselected` counters when selecting specific memories.
Flatten first if a unique hierarchical instance must be selected.

The corresponding pass arguments are `-row-limit 2` and repeated
`-memory-symbol <symbol>` or `-memory-symbol=<symbol>`. C++ callers can set
`MemToRegOptions::memorySymbols`.

## State and port mapping

`kMemoryReadPort(addr)` asynchronously reads one row. `kMemoryWritePort` operands
are `(enable, addr, data, mask, events...)`; `eventEdge` supplies the edge for each
event. For a two-row memory, the transformation creates `row0` and `row1` with
the original width, signedness and supported initialization:

```text
read(addr) -> (addr == 0) ? read(row0) :
             (addr == 1) ? read(row1) : 0
write(enable, addr, data, mask, events...)
    -> write(row0, enable & (addr == 0), data, mask, events...)
       write(row1, enable & (addr == 1), data, mask, events...)
```

Each row register write retains the original data, mask, events and `eventEdge`;
only the enable is qualified by the address match. The pass does not add a cycle
of latency or remove the queue's control logic.

## Current scope

The current implementation handles asynchronous single-address read ports and
write ports with constant masks. It is not a blanket four-state equivalence
transformation: the read mux defaults to zero, so unknown or out-of-range
addresses can behave differently from an array access. Two rows with a one-bit
known address cover all valid two-state addresses.

Memory fill ports are not lowered by the existing implementation. Select only
memories without such ports, and check initialization and reset assumptions for
the intended simulation mode. Preserving a random initialization sequence across
array-to-register lowering is not guaranteed.
