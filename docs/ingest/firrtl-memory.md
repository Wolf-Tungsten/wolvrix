# FIRRTL memory input

The FIRRTL frontend lowers standard `mem` and target CHIRRTL
`cmem`/`smem`/`mport` directly to ordinary GRH state without CIRCT.

## Types and ports

Elements may be UInt/SInt or recursively passive Bundles/Vectors. Bundle fields
become separate states. An element leaf with shape `[a,b]` has storage shape
`[depth,a,b]`; its element width remains unchanged. Zero-size data leaves are
elided. Depth and write latency must be positive; read latency may be zero.
Each latency is limited to 1024 to bound generated pipeline state. Constant
memory is read-only; explicit/inferred writers fail. ROM contents follow the
selected initialization policy; external ROM initialization is not implied.

Standard ports have `addr,en,clk`, plus `data` for readers, `data,mask` for
writers, or `rdata,wdata,wmask,wmode` for readwriters. Masks mirror the data
aggregate with one bit per ground element. Each bit covers that element's full
width. A depth-one standard memory has a zero-width address, internally index
zero. Standard readwriters read only when `en & !wmode`, independent of mask.

CHIRRTL's outermost Vector is depth. `cmem` has read latency zero; `smem` has
read latency one; both have write latency one. Direction is explicit for
read/write/rdwr and derived from actual source/sink uses for infer mports.
Inference includes uses in other ports' address/clock expressions. The parser
keeps access guards at their original statement location. Connected portions
form a bit mask; dynamic partial writes and last-connect priority within a port
are resolved before writing state. A read/write mport reads when accessed and
no bits are written, following the target CHIRRTL convention. Write mode is
shared across all Bundle leaves of one mport; writing one field disables reads
through every field of that port while preserving unwritten storage fields.

## GRH representation

Each storage `kRegister` keeps its source geometry in `firrtl.memoryDepth`,
`firrtl.memoryReadLatency` and `firrtl.memoryWriteLatency` integer attributes,
alongside `firrtl.memory` and `firrtl.readUnderWrite`. These describe the FIRRTL
memory before latency pipelines are expanded. For a `smem UInt<8>[4]`, the
attributes are depth 4, read latency 1 and write latency 1, with packed storage
width 32 and shape `[4]`. Read/write pipeline states remain attributed to their
source mport rather than becoming additional memory declarations.

```firrtl
cmem words : UInt<8>[16]
read mport r = words[addr], clock
when wen :
  write mport w = words[addr], clock
  connect w, data
connect out, r
```

This becomes:

```text
kRegister words_state: Logic<128>, shape=[16], leafWidth=8
kRegisterReadArrayPort [addr] -> readValue: Logic<8>
kRegisterWriteArrayMaskPort [writeEnable, addr, data, bitMask, clock]
```

`regSymbol` identifies the state. Write operands, in order, are effective
enable, row index, one complete row of data, a row-width bit mask, and the event
clock. `eventEdge=["posedge"]` describes that clock. Enable does not include a
duplicate frontend address-bounds comparison; indexed access bounds are handled
by the subsequent state read/write path. A complete row write has eight mask
bits set. Reader output is gated by access. State attrs `firrtl.memory` and
`firrtl.readUnderWrite` retain provenance. Latency uses ordinary registers tagged
`firrtl.memoryPipeline`; no backend memory opcode is added.

## Timing and undefined behavior

Write operands sample old state at the firing edge. Write latency L delays
address/data/mask/enable L-1 edges before committing. Read behavior is:

| Policy | Implementation |
| --- | --- |
| old | Read/gate data at request time, then delay by read latency. |
| new | Delay address/enable by read latency, then read current storage. |
| undefined | Delay only the address, then read current storage; emit a warning. This selects new-value collision behavior and leaves disabled/OOB results ungated for two-state simulation. |

Every writer has a nonnegative `writerOrder` that is unique within its target
state. Concurrent effective writers apply in ascending numeric order, so the
larger value wins overlapping bits. Graph/ops declaration order never selects
the winner. No clock-history or collision-token state is synthesized for
write-write conflicts.
Independent clocks use the same old/new composition as the shared-clock case;
there is no extra edge-history collision tracker. Out-of-bounds writes and zero
masks do nothing. For explicit old/new memories, disabled reads use
`FirrtlInvalidPolicy`; OOB reads use the state access path's safe result without
an additional frontend comparison. Undefined memories leave these reads ungated, so the
backend's safe array-read result is the selected don't-care value. Unknown remains
FourState GRH X where invalid policy applies; Zero is an explicit deterministic
policy. `FirrtlInitPolicy` independently controls memory and pipeline startup.
Both policies persist in graph metadata. The existing `grh-to-grhsim` pass
performs later explicit TwoState projection. No random initialization is invented.

## Validation

```sh
make test_firrtl_memory
make test_transform_memory_read_retime
make test_firrtl_target
```

Tests cover ordinary GRH, JSON, the production transforms and compiled CPU
models: latency/policy matrices, independent and aliased clocks, aggregate masks,
inferred dynamic partial writes, Bundle mport mode, depth-one readwriters, bounds, held enables, repeated
eval, initialization epochs, and FourState unknown observations. The optional
target test executes original Xiangshan Queue2 and masked 138-bit SRAM modules
from the full frozen file without blackboxes.

Fixtures: `tests/ingest/data/firrtl/memory/`. Models: `build/artifacts/firrtl-memory/`
and `firrtl-target/`. Logs: playground `ptmp/firrtl-ingest/F06/`. Whole-design
module linking remains F07.
