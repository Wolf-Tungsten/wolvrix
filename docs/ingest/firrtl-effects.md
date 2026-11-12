# FIRRTL effects, layers, probes, and annotations

The FIRRTL 6.0.0 frontend rejects unsupported active semantics and publishes no
partial design. Module linking, external SV binding and Python FIRRTL options
are described in [mixed ingestion](mixed.md). Parser support is broader than
executable support.

## Clocked effects

Effects use ordinary GRH `kSystemTask` operands
`[condition, argument0, ..., argumentN, clock]`, with
`eventEdge=["posedge"]`, `procKind="always"` and `hasTiming=true`. Arguments
sample old state. Source ordering is retained through the existing scheduler.
The condition includes enclosing when guards.

The base GRH printf format remains an ordinary String constant operand.
Simulation conversion can move a direct constant into the task's
[`grhsim.literalFormat`](../grh/grhsim-dialect.md#tasks-functions-and-dpi)
attribute before partitioning, preserving the frontend/SV operand contract.

| FIRRTL | Task | Condition and arguments |
| --- | --- | --- |
| printf | write | guard & enable; format String and argument values. |
| stop | finish | guard & enable; exit code truncated to 32 bits. |
| assert/assume | fatal | guard & enable & !predicate; severity 1, format, arguments. |
| circt_chisel_ifelsefatal | fatal | Clock/predicate/enable operands; required string format, optional string label; no result type. |
| cover | write | guard & enable & predicate; literal hit comment with escaped percent signs. |

For example `assert(clk,good,en,"bad=%d",value)` yields condition `en & !good`,
arguments `[1,"bad=%d",value]` and event `clk`. `firrtl.effectKind`,
`firrtl.effectLabel`, and the integer operation attribute `firrtl.line` retain
provenance. `firrtl.line` is the 1-based line of the originating FIRRTL
statement, matching GSIM's `lineno` field and its `ASSERT_<line>`,
`PRINTF_<line>`, and `STOP_<line>` special-node names. The attribute is copied
by graph transforms and is serialized with ordinary operation attributes.
Assume behaves as
assert in simulation. Cover produces a hit trace and a frontend warning that no
formal coverage model is generated.

Supported placeholders are `%b/%c/%d/%x/%%`; x maps to the existing hexadecimal
formatter. Argument count and ground hardware types are checked. Unknown
formats, time/hierarchy substitutions, fprintf/fflush, unknown intrinsics,
force/release, property/class execution and enum/match lowering fail explicitly.
These are outside the frozen target's required executable construct matrix.
Finish/fatal terminates before later effects. Cross-clock/module ordering is
unspecified by FIRRTL.

## Layers and probes

```cpp
request.firrtl.enabledLayers = {"Verification.Assert"};
```

Paths refer to declared layers. A selected child enables its ancestors, not
siblings; defaults select none. Unknown paths, incorrectly nested blocks and
unselected module enablelayer requirements fail. The exact list persists as
`firrtl.enabledLayers`. Only selected blocks are lowered; inactive colored Probe
leaves are elided. Active blocks cannot drive hardware outside the declaration
layer, except Probe definitions with matching color. Nodes, memories, mports,
wires and registers retain declaration layers.

Probe/RWProbe represent passive hardware views; `read` obtains the value. Bundle
fields flatten and Vectors retain shape. This standalone simulation view does
not settle the cross-language Probe ABI; lowered interface signatures retain
Probe/RWProbe and require ABI resolution. Definitions use `define`, never
connect (including within aggregates), and remain unconditional inside when
blocks. Invalidate skips Probe leaves and cannot replace a definition. Every local/output leaf or array
slot needs exactly one definition. Missing/overlapping definitions, dynamic
destinations, incompatible element types and read-only sources for RWProbe fail.
Layer colors constrain reads and definitions. Input probes come from the
environment. Force/release is rejected rather than silently losing source writes.

## Annotations and external references

The parser lexically validates annotation JSON. Executable reads decode one
record at a time with source locations and strict class/field schemas. Fields
are strings. Unknown classes and extra/missing/non-string fields fail.

| Class suffix | Additional fields | Treatment |
| --- | --- | --- |
| firrtl.transforms.DedupGroupAnnotation | group | Preserve optimization provenance; functional logic remains authoritative. |
| firrtl.transforms.DontTouchAnnotation | none | Anchor resolved declarations/fields; array selection anchors its containing array. |
| firrtl.passes.InlineAnnotation | none | Preserve optimization provenance; flattening uses the ordinary pass. |
| chisel3.util.experimental.decode.DecodeTableAnnotation | truthTable,minimizedTable | Preserve decoder provenance; do not resynthesize the existing logic. |
| firrtl.transforms.BlackBoxInlineAnno | name,text | Require extmodule; feed reachable inline SV implementations to slang. |
| firrtl.transforms.BlackBoxPathAnno | path | Resolve a reachable SV source path. |
| firrtl.transforms.BlackBoxResourceAnno | resourceId | Resolve a source under explicit resource roots. |

All records also require class/target. Circuit/module targets are checked.
Hierarchical DontTouch is currently rejected; unresolved direct targets fail.
Register targets anchor the state operation; its read result can use a generated
value name. Aggregate targets anchor each surviving field declaration.
Direct targets erased as zero-width leaves resolve through `firrtl.elidedLeaves`;
their annotations remain recorded without creating executable values or anchors. Metadata
retains `firrtl.annotationCounts`, selected `firrtl.annotations`, and
`firrtl.pendingExternalAnnotations` and `firrtl.boundExternalAnnotations` separating
unused and linked original blackbox JSON. Unicode
escapes and surrogate pairs are decoded. Unknown initialization/randomization
annotations are not discarded.

Selected instances resolve through the shared mixed linker. Instance operations
also carry `firrtl.line`, the line of the FIRRTL `inst` statement, for matching
external-module boundary nodes. A missing external
implementation fails without publishing a design. Pending payload metadata does
not constitute completed SV binding.

## Validation

```sh
make test_firrtl_effects
make test_firrtl_target
```

Compiled drivers check exact output, old-state sampling, edge/enable behavior,
repeated eval, order, hit traces and process termination. Syntax/semantic tests
cover Probe aggregates/arrays/aliases, layer selection/boundaries, annotation
preservation and located rejection. Target validation executes original
JtagBypassChain with Verification.Assert enabled, legal traces and an illegal
combination that must terminate. Models live under
`build/artifacts/firrtl-effects/` and `firrtl-target/`; logs under playground
`ptmp/firrtl-ingest/F06/`.
