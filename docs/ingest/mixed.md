# Mixed FIRRTL and SystemVerilog ingestion

The shared driver accepts both languages in one transaction. FIRRTL uses the
native C++ parser and lowering; SystemVerilog uses slang. No CIRCT library,
firtool invocation or FIRRTL-to-SV behavioral translation is required.

```python
with wolvrix.Session() as session:
    session.read_sources(
        sources=[
            {"language": "firrtl", "path": "rtl/Top.fir", "unit": "chip"},
            {"language": "sv", "path": "rtl/Helper.sv", "unit": "helpers"},
        ],
        tops=["Top"],
        slang_args=["-Irtl/include", "-DFUNCTIONAL"],
        firrtl_options={
            "node_annotations": True,
            "annotation_files": ["rtl/blackboxes.json"],
            "resource_roots": ["rtl/resources"],
        },
        out_design="design.main",
    )
```

`read_firrtl(path, sv_sources=[...], ...)` is a convenience entry point.
`read_sv` retains its existing arguments. Relative explicit source, annotation
and resource paths use `input_root`; slang option paths retain slang semantics.
An empty top list with FIRRTL inputs selects their circuit main modules.

## Elaboration and publication

The driver indexes and validates all FIRRTL bodies without retaining all body
ASTs, then lowers reachable modules in dependency order. Each extmodule uses
its `defname` and typed parameters to request a real SV specialization. A small
generated SV wrapper passes parameters to slang and is discarded after importing
the implementation. Integer width, sign and all bits are preserved; strings,
reals and raw SV string parameters remain distinct.

These are separate passes over the mapped input: indexing scans declarations
and body ranges; validation parses and discards one body arena at a time;
reachable lowering parses those bodies again. Dependency traversal may retain
the arenas of modules on the current recursion stack. The frontend does not
retain a whole-chip body AST, but it is not a single-pass streaming lowering.

SV references to FIRRTL modules receive generated interface declarations. These
carry no executable behavior. SV conversion preserves a binding request and
never lowers the empty declaration as an implementation. Linking resolves it
to the real FIRRTL graph. Alternating hierarchies work in both directions.
Equivalent SV specializations are imported once; recursion and duplicate real
definitions fail the transaction.

The completed hierarchy and boundary types are verified before publication.
Failed reads expose no partial design and preserve existing session values.
Clones retain all FIRRTL mappings and SV compilation owners. Ingest diagnostics
own their text and coordinates. Transform diagnostics use GRH-owned filenames
and line/column, avoiding buffer ID confusion across slang SourceManagers.
JSON graphs remain usable without frontend owners.

C++ provides `readSources`, `readFirrtl`, `specializeModule` and `linkModules`.
The last assembles already completed results; cross-language elaboration belongs
in one `readSources` request. Duplicate graphs, missing references, recursive
hierarchies and incompatible port signatures fail.

## Boundary ABI

Bundle and SV struct fields become `__DOT__` leaves. Arrays retain outer-to-inner
shape. An extmodule adapter first matches the exact leaf name and then tries
the emitted FIRRTL underscore ABI. For example:

```firrtl
extmodule External :
  input io : { a : UInt<8>, flip y : SInt<8> }
  defname = implementation
```

This maps input `io__DOT__a` to SV `io_a` and output `io__DOT__y` to SV `io_y`.
Flips accumulate along field paths. An SV struct can instead expose matching
field leaves. Signedness and flat/shaped differences use explicit equal-width
`kAssign` views marked `ingest.abi.view`: operand is the source representation,
result is the destination representation. No truncation, extension, lane
permutation or state change occurs. Lane zero occupies low bits after frontend
index normalization. Two shaped interfaces must have identical shape.

Unknown, duplicate, missing, wrong-direction and extra ports fail. Width, value
kind and logic domain must agree. Analog/inout and unresolved Reset/Probe interfaces
are rejected. Instances require declarations in their FIRRTL circuit; a reference
to another FIRRTL source uses a local extmodule and an explicit binding.

Nonstandard names and implementations use explicit bindings:

```python
external_bindings=[{
    "source_unit": "chip",
    "module": "External",
    "implementation": {
        "language": "sv", "source_unit": "helpers", "name": "implementation",
        "parameters": {
            "W": {"kind": "integer", "width": 32, "signed": True,
                  "bits": "00000000000000000000000000001000"},
            "LABEL": "eight", "SCALE": 1.0,
            "RAW": {"kind": "raw_string", "text": "8"},
        },
    },
    "ports": {"io__DOT__a": "data", "io__DOT__y": "result"},
}]
```

Bindings replace implementation selection and the parameter list. `source_unit`
constrains the selected source; it does not allow duplicate real definitions.
FIRRTL implementation modules do not accept specialization parameters. Host
effects use real SV DPI calls and the existing GRH effect mechanism.

## Sources and dependencies

Inline annotations and sidecar JSON accept `BlackBoxInlineAnno` (`name,text`),
`BlackBoxPathAnno` (`path`) and `BlackBoxResourceAnno` (`resourceId`). Every record
requires `class,target` and names an extmodule. Schemas and targets are validated
including unused records; unknown observable annotations fail. Only reachable
extmodule sources are passed to slang. Resource roots are explicit; missing or
ambiguous resources fail. Inline paths must be relative without parent traversal.
Sources materialize under `ptmp/firrtl-ingest/transactions/`, preserving relative
inline filenames. Identical records deduplicate; conflicting filenames fail.

`ingest.dependencies` records FIRRTL content, actual SV/include contents,
annotations, source content, slang arguments and specialization identities.
Generated source entries use content hashes instead of transaction paths.
Repeated reads have stable dependencies. FNV-1a hashes identify content; they
are not cryptographic integrity checks. `firrtl.boundExternalAnnotations` records
used payloads and `firrtl.pendingExternalAnnotations` retains unused payloads.

Node member/root attributes survive imports and hierarchy flattening, retaining
individual instance path components. Full GSIM-style node CSE and nonsink node
consumption remain F08/F09 work. Hierarchical DontTouch and unsupported source
semantics fail explicitly.

## Validation

Use playground Make targets `test_ingest_mixed`, `test_ingest_python`,
`test_firrtl_target`, `test_wolvrix` and `test_grhsim_flow`. Compiled fixtures
cover both alternating directions, typed parameters, aggregate/flip/array ABI,
macros/includes, all three blackbox transports and edge-sensitive DPI effects.
Negative cases require no design publication. The frozen target executes original
XiangShan `MbistClockGateCell` with its inline SV `ClockGate` latch, alongside
queue/SRAM/JTAG scoreboards. This establishes neither full-chip equivalence
nor a simulation speedup.
