# FIRRTL state input

The FIRRTL 6.0.0 frontend supports `reg` and `regreset` in selected modules.
It uses the same ordinary GRH state declarations and ports as SystemVerilog;
no CIRCT dependency or parallel executable state IR is introduced. Combinational
typing, node metadata and read transaction rules remain as described in
[firrtl-combinational.md](firrtl-combinational.md).

## Types and initialization

Register data must be a non-const UInt/SInt, or a recursively storable
Vector/Bundle without flips. Clock, Reset and AsyncReset are signal types, not
register data. The clock expression must have Clock type. Reset expressions
must have UInt<1>, Reset or AsyncReset type; abstract Reset follows the existing
constraint solver. Reset data must have an equivalent type, allowing width
extension/truncation under the normal FIRRTL rules. Reset data and connects both
constrain inferred register widths. AsyncReset data must be const typed, which
also permits const input ports; it need not be a literal. Synchronous reset data
can depend on inputs or old state, including the register itself.

Bundle fields become separate named `kRegister` declarations. Vector dimensions
remain on each leaf's LogicalType, including arrays within Bundle arrays.
Zero-width and zero-extent leaves are elided. A named state declaration owns its
source symbol; its whole-state read uses an internal value symbol.

```cpp
request.firrtl.initPolicy = FirrtlInitPolicy::Unknown; // default
request.firrtl.invalidPolicy = FirrtlInvalidPolicy::Unknown; // default
```

`Unknown` initialization writes an explicit X Constant InitSpec; `Zero` chooses
zero. The graph records `firrtl.initPolicy` independently of `firrtl.invalidPolicy`.
A regreset's reset value is not its simulation initial value. Invalidation and
out-of-bounds reads follow invalidPolicy; out-of-bounds writes do nothing.
Initial GRH values and state retain FourState regardless of either choice.
The existing `grh-to-grhsim` pass performs its explicit TwoState projection,
where X becomes zero. These FIRRTL options do not modify SV input behavior.

## Ordered reads and writes

Connections first form ordered intents. Each register connect becomes one
writer per flattened field, preserving source order. Unconnected state holds
its previous value; it does not need combinational initialization coverage.
Hardware declarations are unconditional, including inside when blocks. Only
containing blocks that do not contain the destination declaration gate a connect.

```firrtl
reg table : UInt<8>[32], clock
when en0 :
  connect table[addr0], data0
when en1 :
  connect table[addr1], table[addr0]
```

This produces one `kRegister` with width=256 and shape=[32], plus two
`kRegisterWriteArrayPort` operations in that order:

| Writer | Operands | Meaning |
| --- | --- | --- |
| First | `[en0 & bound0, addr0, data0, clock]` | Write data0 at addr0 on clock posedge |
| Second | `[en1 & bound1, addr1, oldRead, clock]` | Write old table[addr0] at addr1 on clock posedge |

Both carry regSymbol=table and eventEdge=[posedge]. A whole write uses
`kRegisterWritePort [enable,data,clock]`. Indexed reads use
`kRegisterReadArrayPort [index0,...]`, with regSymbol and a suffix-shaped result;
whole reads use `kRegisterReadPort []`. Index operands correspond to shape
dimensions from outermost to innermost; row-major element zero occupies low bits.

The later enabled writer wins only in its overlapping region. Whole writes,
subarray writes and element writes can appear in either order. Every writer's
enable, indices and data sample old state before any writer commits. The frontend
does not copy the whole array and commit each element, nor synthesize per-slot
decoders or bit-offset arithmetic for state writes.

## Reset barrier and node boundaries

Ordinary writes are gated with !reset. A final whole-state writer supplies reset
data with enable=reset, overriding ordinary writes independently of source
declaration placement. Synchronous reset writers all have events=[clock] and
eventEdge=[posedge]. AsyncReset adds the reset expression to **every** writer
of that register: events=[clock,reset], eventEdge=[posedge,posedge]. This shares
one event domain and implements both clock updates and reset assertion without
letting normal data write on reset edges. Reset deassertion is not an event.

Node calculations retain attributes across reads of old state, simplification,
JSON and phase extraction. State declarations, state read ports and writers stay
outside node ownership. The General and Output copies of a calculation retain
the same source identity and distinct execution owners, with explicit clone
provenance as defined in [node-attributes.md](node-attributes.md). Labels do not
move calculations across state sampling or event phases.

## Verification

Run `make test_firrtl_state` through the playground Makefile. It checks semantic
errors, reset inference, init/invalid policies, signed extension, self-reset,
conditional declaration semantics, empty state and node ownership. It compares
ordinary GRH, JSON reload and simplify against equivalent SV and independent
multi-cycle scoreboards. Both FIRRTL and SV models are then converted, partitioned,
emitted, compiled and run against those scoreboards for three init/eval epochs.

RenameTable covers 384 cycles x 4 phases; priority covers 512 cycles x 5 phases.
The latter includes asynchronous assertion without a clock edge, deassertion at
high clock, held reset, old-state indices, nested aggregate geometry and both
whole/partial priority directions. SV reference outputs explicitly concatenate
array slots into packed observation ports because direct SV aggregate state
forwarding currently leaves the aggregate output undriven. The reference state
and all writes remain arrays; tests reject undriven reference outputs.

Fixtures: `tests/ingest/data/firrtl/state/`. Generated models and checkpoints:
`build/artifacts/firrtl-state/`. Playground logs: `ptmp/firrtl-ingest/F05/`.
Memory/effects are F06; mixed-language linking and Python FIRRTL options are F07;
whole-node CSE and node-aware nonsink grouping remain F08/F09.
