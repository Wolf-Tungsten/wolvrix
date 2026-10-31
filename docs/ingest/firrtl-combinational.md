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
behavioral lowering visits selected modules. Instances and mixed FIRRTL/SV
binding are reserved for F07, so a selected module containing an instance fails
with a located stage diagnostic. A multi-top failure publishes no design, even
if an earlier top was lowered successfully.

Use explicit logical source units for persistent node identity. The default is
the relative input path, or the basename for an absolute input path. Duplicate
units are rejected; basename collisions need explicit distinct units. Source
locations retain the full filename independently of the persistent node key.

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
the suffix shape after consuming those indices. Bounds checks select the chosen
invalid value on an out-of-range read. The operation count scales with rank,
independently of the number of slots. Width adaptation of whole arrays uses
element slices and `kArrayCreate` when element widths differ.

Connects first form ordered connection intents. Bundle flips reverse the
appropriate ground connections. Later whole connects replace earlier drivers;
later partial connects replace only their indexed subarray. Conditional connects
use muxes. Reads of wires and output ports observe final connectivity, regardless
of the read's textual position.

For combinational partial writes, the frontend computes a row-major bit offset
and merges a shifted data/mask pair into the current driver. An out-of-range write
preserves that driver. The result returns to the original shaped type through an
equal-width view. This uses O(rank) operations without per-slot equality/mux
decoding. Register writes instead use ordered indexed state ports; see
[firrtl-state.md](firrtl-state.md).

A Boolean decision DAG checks initialization over disjoint static bit intervals,
including when/else and complementary one-bit Boolean predicates. Predicate keys
include literal types and widths. Hardware declarations are unconditional; only
conditions of blocks not containing the destination declaration control its
connects. Local wires therefore need complete coverage under these interleaving
conditions. Dynamic writes need a complete default or sufficient static
coverage; no value is silently supplied for a missing driver. Combinational
cycles are rejected before publishing the design.

## Logic domain and node metadata

All materialized values initially use FourState, like ordinary GRH inputs.
`FirrtlInvalidPolicy::Unknown` is the default: invalidate, out-of-bounds reads and
division by zero yield X. The explicit `Zero` policy yields zero. The graph records
`firrtl.invalidPolicy`; source unit and language version are also recorded.
The existing `grh-to-grhsim` pass performs the later TwoState projection. Compare
SV and FIRRTL simulation with a fixed, recorded invalid-value policy.

Each materialized node leaf emits schema v1 members/origins on its internal
operations and a root mapping on its named result's transparent assign carrier.
Aliases reuse their original producers. Referencing another node reuses its named
result; it neither clones nor relabels the referenced expression. `nodeAnnotations`
can explicitly disable these attributes. See [node-attributes.md](node-attributes.md)
for verification and CSE isolation: the first ordinary simplify pass can share
within a node, while cross-node internal expressions remain separate. Whole-node
GSIM-style merging and nonsink consumption are later stages.

## Verification

Run through the playground Makefile:

```sh
make test_firrtl_comb
make test_firrtl_aggregate
make test_firrtl_widths
make test_array_create
make test_transform_shape_mapping
make test_grhsim_cpu_st_cpp
make test_wolvrix
```

The FIRRTL suites compare ordinary GRH with independent arithmetic and geometric
expectations before/after simplify and JSON reload. They also check node ownership,
source locations, staged errors, resets, zero widths, port selection and failed-read
atomicity. `test_firrtl_comb` emits, compiles and executes three GrhSIM models:
basic node/when logic, Bundle/Vector dynamic accesses and writes, and signed
arithmetic. Generated models live in `build/artifacts/firrtl-comb`; build and test
logs use playground `ptmp/firrtl-ingest/F04`.
