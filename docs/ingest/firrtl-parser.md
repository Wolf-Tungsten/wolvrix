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
combinational modules as described in [firrtl-combinational.md](firrtl-combinational.md).
Unsupported behavioral constructs return located errors and no design. Parsing
alone does not publish an executable design or create GRH node attributes.

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
The following connect can reference p. F06 must use the original nesting when
deriving enables. A node or wire declared in the same when cannot escape.
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

| Shared leaf | fieldPath | elementWidth | shape |
| --- | --- | ---: | --- |
| `packet__DOT__valid` | `[packet,valid]` | 1 | `[8]` |
| `packet__DOT__data` | `[packet,data]` | 32 | `[8,4]` |

`fieldPath` starts with the original port name. Flattened name collisions are
diagnosed. `sourceType`, `isConst`, and `requiresTypeResolution` retain distinctions
which GRH Logic alone cannot express. UInt without a width has `elementWidth=-1`;
zero-width leaves remain zero at this syntax boundary. Reset, Probe, Analog,
properties and other interfaces needing inference or ABI handling are marked for
later resolution. A successful scan does not grant permission to bind these as
plain SV Logic ports. The typed source index retains full nested types and layer
colors for subsequent stages.

Parameters distinguish quoted strings from `RawStringParameter`, and preserve
integer values without a 64-bit truncation. Decimal parameter integers use signed
two's-complement bits, at least 32 bits and wider when needed; integer expressions
retain their original literal spelling and explicit FIRRTL type/width. Raw string
parameters must remain distinct during specialization because their SV meaning
differs from quoted strings.

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
