# FIRRTL combinational input

`readFirrtl` and the common `readSources` driver lower pure FIRRTL 6.0.0
combinational designs into ordinary GRH. The frontend uses its own type analysis
and lowering in `lib/ingest/firrtl/{semantic,lower}.cpp`; it does not call CIRCT,
firtool, or the SV frontend. Syntax support and executable support have separate
boundaries; see [firrtl-parser.md](firrtl-parser.md). Register and reset support
extends this frontend in [firrtl-state.md](firrtl-state.md).

```cpp
#include "ingest/ingest.hpp"
using namespace wolvrix::lib::ingest;
IngestRequest request;
request.sources.push_back({SourceLanguage::Firrtl, "comb.fir", "my-design"});
request.tops = {"top"};
request.firrtl.invalidPolicy = FirrtlInvalidPolicy::Unknown;
auto result = readSources(request);
if (result.success()) {
    // Keep result.sourceOwners with the published design.
}
```

An empty top list selects each source's circuit main module. Explicit tops must
be unique and unambiguous across sources. Syntax validation checks every module;
behavioral lowering visits selected modules and their dependencies. The shared
driver resolves instances and mixed FIRRTL/SV implementations through indexed
interfaces. A multi-top failure publishes no design, even if an earlier top was
lowered successfully.

Use explicit logical source units for stable source provenance. The default is
the relative input path, or the basename for an absolute input path. Duplicate
units are rejected; basename collisions need explicit distinct units. Source
locations retain the full filename independently. Semantic Node IDs identify
objects within their Graph; they are not source keys or GSIM numeric IDs.

## Types and operations

The semantic pass resolves UInt/SInt widths, Clock, abstract Reset, AsyncReset,
const qualifiers, Bundle fields, Vector dimensions and connection flow before
creating GRH values. Width constraints propagate from every driver, including
overridden connects. Fixed destination widths do not resize the source expression.
Unknown widths, growing cyclic constraints, incompatible resets and total widths
outside the GRH int32 range are errors with source locations. Bounded cyclic
width constraints may converge even when earlier cyclic connects are overridden.
The solver limits evaluations to `max(1,000,000, 64 * constraintCount)` per module;
exhaustion reports a resource limit, without claiming every slow cycle is
mathematically unbounded. Reset inference
uses connected types; `asReset` creates a neutral inference boundary.

| FIRRTL operation | GRH mapping |
| --- | --- |
| `add` / `sub` | Extend both operands to `max(widths)+1`, then `kAdd` / `kSub` |
| `mul` | `kMul` with original operand widths and a sum-width result |
| `div` / `rem` | Align signedness and sufficient operand widths, compute, then adapt to numerator width (+1 for signed div) / minimum input width |
| Comparisons | Extend operands using their signedness; produce UInt<1> |
| `and/or/xor/not` | Extend as needed; reinterpret the result as UInt, including SInt inputs |
| `pad/cvt/neg`, casts | Explicit concatenation, sign replication, slices and equal-width assign views |
| Shifts | Explicit result sizing, `kShl/kLShr/kAShr`, or static slices |
| `cat/bits/head/tail` | Explicit bit layout through concat or slices |
| Reductions | `kReduceAnd/kReduceOr/kReduceXor`; zero-width identities are 1/0/0 |
| `mux` | Per-field width adaptation followed by shaped `kMux` |

Primitive argument types follow the pinned FIRRTL 6 specification. In particular,
`cat` accepts zero or more homogeneous UInt or SInt arguments. Primitive operands
are ground types: `eq(Bundle,Bundle)` and `asUInt(Vector)` are rejected. Aggregate
equality and packing use legal ground field/element expressions; construction
uses field/element connects. No aggregate primitive language extension is implied.

Arbitrary precision literals are parsed independently of host integer width.
Explicit widths must represent the literal. Zero-width values and zero-extent
leaves are eliminated before GRH construction. `firrtl.elidedLeaves` records their
flattened names. Signatures retain their geometry and mark those leaves as needing
ABI resolution; they cannot be treated as materialized cross-language ports.

## Connections and arrays

Bundle fields flatten using `__DOT__`; all Vector dimensions migrate to each
leaf's shape in outer-to-inner order. Name collisions are errors. A Vector of
Bundles has one node identity per Bundle leaf, irrespective of element count.
Singleton dimensions remain explicit.

With node annotations enabled, sibling field paths reuse their common
SemanticTree Paths within the same declaration. For `{bits : {a : UInt<1>,
b : UInt<1>}}`, `bits.a` and `bits.b` are leaf Nodes under the shared `bits`
Path. Paths contain no operations, and Nodes have no children. Another
declaration receives its own independent path. Arrays remain one Node per data
leaf before a future split pass; no Node is created for each array element.

```firrtl
input packet : { valid : UInt<1>, data : UInt<4>[2] }[3]
node saved = packet
```

This produces `packet__DOT__valid` with width 3, shape `[3]`, and
`packet__DOT__data` with width 24, shape `[3,2]`. The corresponding `saved` leaves
have the same geometry. GRH array element zero occupies the least significant
bits; `cat(packet[2].data[1], ..., packet[0].data[0])` gives the explicit packed
data image. Bundle declaration order does not silently become an SV packed layout.

`packet[i].data[j]` uses `kSliceArray(base,i,j)`: operand zero is the flattened
data leaf, subsequent operands are indices in source order, and the result has
the suffix shape after consuming those indices. A dynamic dimension contributes
an unsigned `kLt(index,extent)` bounds check; the extent is represented at a width
that cannot truncate it. Multiple dynamic dimensions combine their checks with
`kAnd`. The read then uses `kMux [inBounds, selected, invalid]`: operand zero is
the one-bit condition, operand one is the selected array value, and operand two
is the invalid value of the same width and suffix shape. Invalid means X under
`FirrtlInvalidPolicy::Unknown` and zero under `Zero`.

Bare nonnegative integer indices and UInt literal payloads of at most 62 bits
are recognized directly by lower. A recognized in-range index remains an index
operand but contributes no runtime check. An entirely fixed valid read needs no
bounds mux. Other expression forms retain dynamic checks. Bare integer OOB
syntax such as `a[2]` for a two-element Vector is a semantic error; an expression
index such as `a[UInt<2>(2)]` is well typed and lowers directly to invalid when
its literal is recognized as OOB. Any such OOB dimension determines the whole
selected result, including when other dimensions are dynamic.

For example, `input a : UInt<4>[3][2]` has total width 24 and shape `[2,3]`:
the final FIRRTL suffix is the outer dimension. With `input i : UInt<2>`:

| FIRRTL expression | Result | Lowered read, omitting assign views |
| --- | --- | --- |
| `a[1][2]` | Width 4, scalar | `kSliceArray [a,1,2]`, no bounds check |
| `a[i]` | Width 12, shape `[3]` | Select row `i`, guarded by `i < 2` |
| `a[i][UInt<2>(2)]` | Width 4, scalar | `kSliceArray [a,i,2]`, guarded only by `i < 2` |
| `a[i][UInt<2>(3)]` | Width 4, scalar | Invalid constant; no array slice |

The operation count scales with rank, independently of the number of slots.
Width adaptation of whole arrays uses element slices and `kArrayCreate` when
element widths differ.

Connects first form ordered connection intents. Bundle flips reverse the
appropriate ground connections. Later whole connects replace earlier drivers;
later partial connects replace only their indexed subarray. Conditional connects
use muxes. Reads of wires and output ports observe final connectivity, regardless
of the read's textual position.

Normalized path guards share actual implementation operations across connects
and effects. Every consuming Node includes those operations in its membership,
including consumers added after a cache hit and consumers reached through nested
guard dependencies. For example, `when c: connect x, a; connect y, b` gives both
Nodes the shared guard operations. Register-update consumers are the next-state
Nodes. Membership may overlap; the shared IR operations still execute once.

`firrtl.guardSemanticPolicy="consumer-nodes-v2"` records this policy. A `when`
occurrence does not create a separate `when@...` Node. Its internal guard
calculation belongs to its consumers. A real FIRRTL declaration named `guard_N`
remains an ordinary named Node.

For combinational partial writes with static indices, the frontend splices the
updated slice between the unchanged high and low slices. A conditional static
write selects only that slice before splicing it; untouched bits do not depend
on its guard. For example, reading `m[1]` to conditionally update `m[0]` does not
make `m[1]` depend on that condition. This also avoids selecting the full array
for each static partial write in the generated simulator.
Consecutive static partial writes share a packed accumulator, avoiding repeated
conversion of every array element. The final assign restores the declaration's
shape; CHIRRTL memory write data restores its leaf shape at the memory boundary.
The accumulator keeps disjoint bit ranges. A guarded write obtains its old arm
only from pieces intersecting the destination range, in high-to-low bit order.
For example, after writing `a[0]`, `when en: connect a[1], d` selects between `d`
and the old `a[1]` piece; it does not slice a newly rebuilt whole-array concat.
A range crossing several pieces concatenates only those pieces. The updated
whole driver is still materialized after the splice, and dynamic writes keep
their existing packed-driver route.
For a partial write containing dynamic indices, lower computes a row-major bit
offset and merges a shifted data/mask pair into the current driver. Only dynamic
dimensions contribute bounds checks. An OOB dynamic index preserves the driver;
a recognized OOB literal skips the write entirely, even in a mixed access. A
skipped write supplies no initialization coverage. Probe definitions retain
their requirement for a valid static component destination. The result returns
to the original shaped type through an equal-width view. This uses O(rank)
operations without per-slot equality/mux decoding. Register writes instead use
ordered indexed state ports; see [firrtl-state.md](firrtl-state.md).

For the same `[2,3]` array, this writes only the last cell of a valid row:

```firrtl
wire w : UInt<4>[3][2]
connect w, a
connect w[i][UInt<2>(2)], data
connect w[i][UInt<2>(3)], UInt<4>(15)
```

Here `data` is UInt<4>. The valid write has bit offset `12*i + 8`, and a local
data-merge mask of `0xf << (12*i + 8)` within the 24-bit driver. It replaces those
four bits when `i < 2`; all other bits retain `a`. The final OOB connect changes
nothing. Whole and static wire/output connects need no memory write mask;
dynamic wire/output connects retain the local mask consumed by their data merge.
CHIRRTL mport connects additionally accumulate a bit mask of written portions,
with the guard and dynamic bounds gating that accumulation. That mask feeds
the memory writer; ordinary wire/output connects create no such extra coverage
mask. Standard memory ports continue to use their explicit `mask`/`wmask`
fields; see [firrtl-memory.md](firrtl-memory.md).

A Boolean decision DAG checks initialization over disjoint static bit intervals,
including when/else and complementary one-bit Boolean predicates. Predicate keys
include literal types and widths. Hardware declarations are unconditional; only
conditions of blocks not containing the destination declaration control its
connects. Local wires therefore need complete coverage under these interleaving
conditions. Dynamic writes need a complete default or sufficient static
coverage; no value is silently supplied for a missing driver. Combinational
cycle analysis is deferred until after `hier-flatten`, with the deferral retained
in `firrtl.pendingCombLoopCheck`. Whole-array cycles involving partial element
dependencies are checked by the shared range analysis there. For example
`m[0][1] = a < b; m[1][0] = not(m[0][1])` has no element cycle; range analysis
removes the apparent whole-array cycle while preserving the public array shape.
A mutual dependency between both elements is rejected by `comb-loop-elim` with
`fail_on_true_loop=True` before simulation conversion.

## Logic domain and SemanticTree Nodes

All materialized values initially use FourState, like ordinary GRH inputs.
`FirrtlInvalidPolicy::Unknown` is the default: invalidate, out-of-bounds reads and
division by zero yield X. The explicit `Zero` policy yields zero. The graph records
`firrtl.invalidPolicy`; source unit and language version are also recorded.
The existing `grh-to-grhsim` pass performs the later TwoState projection. Compare
SV and FIRRTL simulation with a fixed, recorded invalid-value policy.

With `nodeAnnotations` enabled, normal lowering publishes one SemanticTree
(`firrtl.semanticNodePolicy="leaf-op-members-v1"`). Internal entries are Paths;
every named Node is a leaf containing actual GRH operation references and any
necessary actual ValueId anchors. All Nodes use the same structure. Type, shape,
state, event and expression semantics are read from the referenced IR. The old
six kinds, source binding roles, projections and duplicated source-shape schema
are not part of this model. `shapeFieldDepths` remains interface ABI information
in `PortLeafSignature`; see [firrtl-parser.md](firrtl-parser.md).

Node-to-op membership is authoritative. Graph APIs maintain its reverse op-to-Node
index and value anchors together. Lowering registers members when it creates or
shares operations. Before publishing a FIRRTL module, it checks every live op
has valid Node membership and reports the concrete op if membership is missing.
This check does not assign unowned operations to a root or module container.

For `node y = mux(c, add(a,b), d)`, Node `y` owns the generated add, mux and
necessary adaptation/result operations. Named `a`, `b`, `c` and `d` are input
boundaries; their producers remain in their own Nodes. A named alias has its own
Node and boundary even when it shares a value. Neither circuits nor identity
operations are duplicated to maintain the inventory. Input Nodes may have no
ops and use their real input value as an anchor. Zero-width leaves and empty
arrays retain empty Nodes without fabricated executable values.

A register has distinct current and next Nodes with the same structure. The
current Node owns the state declaration and initial full-state read. The next
Node contains every ordered writer and the operations lowering creates for its
data, enable, indices, clock and reset expressions. Each writer retains its real
operands, `writerOrder` and `eventEdge`, so one writer's data value is never used
as a substitute for complete next-state behavior. A register with no writer
has an empty-op next Node anchored to the actual current value, expressing hold.
Names such as `r$NEXT` are producer-assigned display names; IDs and IR references
carry identity, and generated-name collisions are resolved explicitly.

Memory storage Nodes own actual storage declarations under a `$storage` path
component. Memory port Nodes own their generated access computations. Each
latency pipeline stage has separate current/next Nodes containing its real
declaration, read and write operations. An effect Node named `effect@<source line>`
owns its actual task and supporting computation. Storage and effects need no
fabricated result value.

`hier-flatten` relocates Node members and anchors with the actual op/value maps,
then attaches every Node directly to the root Path. Names contain the escaped,
qualified instance/field path, such as `u.packet.data`. Formal-port-to-Node maps
published by ingest identify instance placeholders, including elided ports, so
flatten does not duplicate parent placeholders and child port Nodes. Internal
SV operations without a SemanticTree receive no FIRRTL membership merely because
they implement a FIRRTL external port.

JSON v5 stores explicit Path/Node IDs and actual `ops`/`anchors` references.
Ordinary export needs no reconstruction script. `make test_f28_semantic_tree`
checks core lifecycle, ingest, XMR, both flatten paths, empty endpoints, shared
membership and JSON roundtrip. Later passes whose Node evolution has not been
migrated reject annotated graphs without changing them. Functional execution
tests explicitly ingest with `nodeAnnotations=false` when exercising those
passes. Node correspondence alone does not establish expression, state-transition
or event equivalence.

## Verification

Run through the playground Makefile:

```sh
make test_firrtl_comb
make test_firrtl_aggregate
make test_firrtl_widths
make test_f28_semantic_tree
make test_array_create
make test_transform_shape_mapping
make test_grhsim_cpu_st_cpp
make test_wolvrix
```

The FIRRTL suites compare ordinary GRH with independent arithmetic and geometric
expectations and JSON reload. The dedicated F28 suite checks annotated Node
membership through ingest, XMR and flatten; execution suites explicitly opt out
of annotations for later simplify and GrhSIM conversion. They also check source
locations, staged errors, resets, zero widths, port selection and failed-read
atomicity. `test_firrtl_comb` emits, compiles and executes three GrhSIM models:
basic node/when logic, Bundle/Vector dynamic accesses and writes, and signed
arithmetic. Generated models live in `build/artifacts/firrtl-comb`; build and test
logs use playground `ptmp/firrtl-ingest/F04`.
