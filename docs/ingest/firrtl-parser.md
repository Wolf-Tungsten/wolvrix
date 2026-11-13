# FIRRTL syntax and interface indexing

The FIRRTL frontend uses its own C++ lexer and recursive descent parser. It does
not link CIRCT/MLIR, invoke firtool, or translate FIRRTL into SV. The accepted
preamble is `FIRRTL version 6.0.0`; other versions and missing preambles are
diagnosed. `FirrtlOptions.expectedVersion` adds an exact version assertion.
The reference is [FIRRTL v6.0.0, revision
5ab37a0](https://github.com/chipsalliance/firrtl-spec/blob/5ab37a032d822c8e3975fb9f7f36e52a019c34ec/spec.md).

The target Chisel text also uses CHIRRTL `cmem`/`smem`/`mport`, variadic `cat`,
`intmodule`, layer output directory strings, and named `stop`. These forms have
explicit syntax representations. This is a parser for that input dialect;
accepting its syntax does not establish type correctness or executable behavior.
Legacy unversioned FIRRTL, `<=`/`<-` connects, `validif`, and older removed types
are rejected instead of being silently rewritten.

## Validation boundaries

`ingest/firrtl_syntax.hpp` exposes three operations:

| Operation | Checks | Retained representation |
| --- | --- | --- |
| `firrtl::indexFile` / `indexText` | Preamble, lexical/indentation structure, circuit declarations, interfaces, parameters and type-alias references | Shared source owner, module ranges, interned interface types, layers, aliases, formal metadata and annotation JSON |
| `firrtl::parseModule` | One module's statement/expression syntax and declaration references | A module arena with typed types/expressions/statements and the source owner |
| `firrtl::validate` | All module bodies using `parseModule` | Diagnostics and counters; each body arena is released before parsing the next module |

An index success is explicitly an interface result. An unknown statement in a
body can pass indexing and must fail body validation. `scanFirrtlInterfaces`
and the shared `scanInterfaces` entry point perform both operations before
reporting success. `readFirrtl` performs the same checks, then lowers selected
modules as described in [firrtl-combinational.md](firrtl-combinational.md),
[firrtl-state.md](firrtl-state.md), [firrtl-memory.md](firrtl-memory.md), and
[firrtl-effects.md](firrtl-effects.md).
Unsupported behavioral constructs return located errors and no design. Parsing
alone does not publish an executable design or create a SemanticTree.

```cpp
#include "ingest/firrtl.hpp"

namespace fir = wolvrix::lib::ingest::firrtl;
auto indexed = fir::indexFile("SimTop.fir", "xiangshan");
if (indexed.success()) {
    auto id = indexed.source->findModule("SimTop");
    if (id) {
        auto parsed = fir::parseModule(indexed.source, *id);
        if (parsed.success()) {
            // parsed.body->statements contains the module's root statement IDs.
            // Keep parsed.source alive while using any syntax string_view.
        }
    }
}
```

## Syntax and references

The lexer emits significant newlines, INDENT and DEDENT, suppressing newlines
inside balanced delimiters. Spaces establish each block's indentation; tabs
are errors. Comments, escaped strings, numeric literal identifiers such as
`` `0` ``, and escaped source info are distinguished before interpreting
delimiters. Annotation arrays are checked with a streaming JSON parser and
retained as source slices, including unknown annotation classes and parameters.
Their behavior is not interpreted by this stage.

The typed syntax covers the following families:

| Family | Representation |
| --- | --- |
| Types | UInt/SInt/Analog widths, Clock/Reset/AsyncReset, Bundle fields/flips, nested Vectors, const, Probe/RWProbe with layer color, aliases and enums; property types including List and Inst |
| Declarations | Modules, external/intrinsic modules, class/extclass, layers, aliases, formal units, ports, nodes, wires, registers and memories |
| Expressions | References, subfields, static/dynamic indices, arbitrary-width integer literal text, ordered primitive operands, mux, probe/read, intrinsic parameters/result type, enum and property expressions |
| Statements | Connect/invalidate/attach/define/propassign, when/else and shorthand, match, layerblock, CHIRRTL ports, memory properties, clocked commands, force/release and property assertions |

Module signatures are indexed before bodies, so an instance may name a module
defined later. Ordinary component references must follow their declaration and
stay within their lexical scope. All component declarations and command labels
share the module namespace, including declarations in mutually exclusive blocks;
command labels cannot be read as values. Type aliases have their own circuit-wide
namespace and may have the same name as a module. Alias cycles, unresolved
aliases, duplicate declarations and wrong module/class instance kinds are errors.

CHIRRTL has a specific exception: mport data uses the memory declaration's scope,
while its access remains at the original statement location. For example:

```firrtl
smem m : UInt<8>[16]
when en :
  read mport p = m[addr], clock
connect out, p
```

The AST keeps the `MPort` statement inside `When.body`. Its operands are the
indexed memory reference `m[addr]` and `clock`, and its `target` is `read`.
The following connect can reference p. Memory lowering uses the original nesting
when deriving enables. A node or wire declared in the same when cannot escape.
See the [CHIRRTL port design discussion](https://github.com/llvm/circt/issues/1561)
for this input convention; it is a semantic reference, not an implementation
dependency.

Syntax arenas contain flat vectors addressed by TypeId, ExprId and StmtId. A node
statement's single operand is its defining expression; a reference to that node
holds its interned name and does not copy the definition. Compound statements
keep ordered child IDs. Ranges use source byte offsets and one-based line/column;
compound statement ranges describe their headers, with child locations retained
individually. Each statement retains its original `@[...]` token.

Syntax failures include an owned filename, line, column, line snippet, original
source info and current module. Recovery continues at statement/block boundaries;
diagnostics are capped at 32. Syntax and alias nesting have explicit limits.
No failed index exposes a usable source, and callers must check a module result's
success before consuming its possibly partial body.
Regression cases also check CRLF, EOF without a final newline, unchanged 1024-bit
literal text, recovery up to the diagnostic limit, and excessive nesting.

## Shared-driver signatures

`describeFirrtlModule` converts an indexed typed interface into owning
`ModuleSignature` records. `scanFirrtlInterfaces` collects these records and
keeps all source owners. Logical source unit names must be unique within a
request. Relative input paths use `IngestRequest.inputRoot`.

Bundle fields become leaves, nested flips determine direction, and Vector axes
remain outer-to-inner shape dimensions. For example:

```firrtl
input packet : { valid : UInt<1>, data : UInt<32>[4] }[8]
```

| Shared leaf | fieldPath | elementWidth | shape | shapeFieldDepths |
| --- | --- | ---: | --- | --- |
| `packet__DOT__valid` | `[packet,valid]` | 1 | `[8]` | `[1]` |
| `packet__DOT__data` | `[packet,data]` | 32 | `[8,4]` | `[1,2]` |

`fieldPath` starts with the original port name. Flattened name collisions are
diagnosed. `sourceType`, `isConst`, and `requiresTypeResolution` retain distinctions
which GRH Logic alone cannot express. UInt without a width has `elementWidth=-1`;
zero-width leaves remain zero at this syntax boundary. Reset, Probe, Analog,
properties and other interfaces needing inference or ABI handling are marked for
later resolution. A successful scan does not grant permission to bind these as
plain SV Logic ports. The typed source index retains full nested types and layer
colors for subsequent stages.

`shapeFieldDepths` is port-layout information used by mixed-language ABI
matching. Each entry counts the `fieldPath` components before that Vector axis,
including the original port name. Thus `r[0].data` has depth 1 and
`r.data[0]` has depth 2. For the table's `packet.data`, the outer Vector is after
`packet` and the inner Vector is after `packet.data`, giving `[1,2]`. Adjacent
Vector axes may have the same depth. These coordinates retain source layout in
`PortLeafSignature`; they are not fields of a SemanticTree Node.

Parameters distinguish quoted strings from `RawStringParameter`, and preserve
integer values without a 64-bit truncation. Decimal parameter integers use signed
two's-complement bits, at least 32 bits and wider when needed; integer expressions
retain their original literal spelling and explicit FIRRTL type/width. Raw string
parameters must remain distinct during specialization because their SV meaning
differs from quoted strings.

## SemanticTree and mixed hierarchy boundaries

With `nodeAnnotations` enabled, FIRRTL lowering creates a SemanticTree containing
internal Paths and one kind of named leaf Node. A Node contains actual GRH op
members and necessary actual value anchors. Bundle and module/instance prefixes
are Paths before flattening, never executable Nodes. Arrays stay one leaf per
data field. Input and alias Nodes may have empty op sets; zero-width and empty
array endpoints may have neither ops nor anchors. Named boundaries remain
distinct even when their implementations share operations or values.

Module metadata `firrtl.semanticPortNames` and `firrtl.semanticPortNodes` publish
the formal-port-to-Node map. Instance attributes `semanticPortNames` and
`semanticPortNodes` publish its actual-port-to-Node map in the parent Graph.
Names use the shared `PortLeafSignature.name` encoding, including `__DOT__` for
Bundle fields. The parallel arrays include elided zero-width and empty-array
ports. Flatten uses these explicit mappings and checks real anchors for
materialized ports; it does not infer identity from a final display name.

`hier-flatten` copies Node members and anchors through the same op/value
relocation as the circuit. It reuses mapped instance-port placeholders and
attaches all resulting Nodes directly to the root Path, removing internal Paths.
Each Node's name is its escaped qualified instance/field path. There is no
Module, Declaration, Field or other Node-kind classification, and no source
endpoint binding-role schema. Register current/next, memory storage/pipelines
and effects use the same leaf structure; their complete behavior is carried by
their real IR members as described in [firrtl-combinational.md](firrtl-combinational.md).

An SV implementation bound to a FIRRTL external instance may have no
SemanticTree. Its internal operations, including state and DPI operations,
remain outside FIRRTL Node membership. Actual FIRRTL port Nodes and their
relocated boundary values remain available, but their presence does not assign
the SV implementation's internal computations to those Nodes. Complete op
coverage is enforced when publishing each annotated FIRRTL module; a mixed
flattened graph can also contain unannotated SV operations.

A mixed-language ABI adapter also publishes the explicit formal-port Node map,
including elided ports. It registers actual array offset constants, slices,
array construction and signedness-view assignments in the corresponding port
Node when creating each operation. These helpers carry `ingest-firrtl-abi`
source locations; they implement the FIRRTL interface and are not classified as
SV body operations. Only logical formal values become anchors. Physical SV
port values and implementation computations do not acquire additional anchors
or FIRRTL membership. The temporary implementation instance carries only
materialized port membership, allowing elided endpoints to remain empty leaves.

Flattening emits one external boundary record per such instance as graph metadata:

- `firrtl.externalInstancePaths`, `Modules`, `Implementations`, and `Lines`
  are parallel arrays containing the flattened instance path, original FIRRTL
  external module name, original implementation module, and originating FIRRTL
  instance line. ABI adapters preserve these identities in
  `ingest.link.sourceModule` and `ingest.link.implementationModule`; a direct
  implementation uses `ingest.sourceModule`, or its linked name when absent.
- `firrtl.externalInstanceInputPorts`, `OutputPorts`, and `InoutPorts` contain
  full flattened boundary names. Their corresponding `InputOffsets`,
  `OutputOffsets`, and `InoutOffsets` arrays have one extra entry; instance `i`
  owns ports in the half-open interval `[offsets[i], offsets[i+1])`.

For example, paths `["cpu$gate"]`, implementations `["ClockGate"]`, input ports
`["cpu$gate$CK", "cpu$gate$E", "cpu$gate$TE"]`, and input offsets `[0,3]`
identify the three inputs of one external implementation. These records preserve
source and boundary identity; they do not assert that internal SV operations are
individually equivalent to another frontend's external-module representation.
These external-boundary records contain no SemanticIds, so tree relocation does
not invalidate them. They are separate from the explicit semantic port maps
whose Node IDs are remapped when a graph is copied.

When an unannotated SV top contains FIRRTL descendants, flatten creates the target
SemanticTree on the first FIRRTL tree import. The tree takes `sourceUnit` and
`circuit` from that first source, and uses the actual top graph name as its root.
Intervening SV instance names contribute to the qualified name. A FIRRTL Node
`n` below `svtop.w.f` becomes the root's direct leaf named `w.f.n`; the reversible
`path()` query returns `["svtop", "w", "f", "n"]`. The query's tokens do not
imply retained internal Path records. Unrelated SV operations and values acquire
no membership from that instance prefix. JSON v5 exports the actual leaf op
sets and anchors directly; normal ingest and flatten need no reconstruction
script. Later passes without F28 Node-lifecycle support reject annotated input
without changing the graph.

## Resource behavior and validation

Files use a single read-only mapping on Unix; the fallback owns one file buffer.
`indexText` moves its string into the owner. Names are interned source views and
types are structurally interned. The persistent index contains no module body
AST or GRH design. Body arenas intern their own names and types and can be
discarded independently. The largest single module still determines peak AST
memory; this stage does not promise a fixed-memory parser independent of module
size.

Use the playground Makefile:

```sh
make test_firrtl_parser
make inspect_firrtl_inputs FIRRTL_INPUT=/path/to/SimTop.fir
make inspect_firrtl_inputs FIRRTL_INPUT=/path/to/SimTop.fir FIRRTL_INSPECT_FLAGS='--module SimTop'
make inspect_firrtl_inputs FIRRTL_INPUT=/path/to/SimTop.fir FIRRTL_INSPECT_FLAGS='--index-only'
make test_firrtl_parser_inputs
make verify_firrtl_inputs
make test_wolvrix
```

The inspection target prints input SHA-256, declaration/interface and body
counts, wall time and peak RSS. Its default mode checks every body and shared
interface signature; `--index-only` explicitly reports `interfaces-only`.
It returns nonzero for parse/signature errors. Logs and workflow temporary files
belong under `ptmp/firrtl-ingest/F03/`; checked-in fixtures are under
`tests/ingest/data/firrtl/`. Performance numbers and the exact local large-input
identities are recorded in the playground's F03 implementation record.

The playground's `pdocs/firrtl-ingest/inputs.json` freezes the acceptance input.
`verify_firrtl_inputs` checks the FIRRTL, parser fixtures, generated SV inventory,
generation log and cached specification before running the inspector. It requires
successful validation of every module body, compares all persistent syntax and
interface counters, then hashes the inputs again to detect changes during the
run. The full SV file inventory and inspection log are written under
`ptmp/firrtl-ingest/F03/input-verify/`. This gate checks syntax and input identity;
SV binding and behavioral equivalence remain later stages.

`freeze_firrtl_inputs` captures a new manifest after the same validation and
refuses to overwrite an existing one. To evaluate a different input, supply a new
`FIRRTL_INPUT_MANIFEST` path together with `FIRRTL_INPUT`,
`FIRRTL_INPUT_SV_FILELIST`, `FIRRTL_INPUT_GENERATION_LOG` and `FIRRTL_INPUT_SPEC`.
The specification cache must contain the pinned revision referenced above.
