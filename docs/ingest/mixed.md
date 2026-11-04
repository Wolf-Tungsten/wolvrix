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
`node_annotations` defaults to `True`; the bounded bringup configuration below
shows how to disable these optional source associations explicitly.
`lower_threads` defaults to `1`. Values greater than one create a fixed worker
pool for the transaction. Workers take reachable FIRRTL modules from a shared
queue and lower each directly into its own graph in the final design; a worker
takes the next module as soon as it finishes. The coordinator creates all target
graphs with their final design owner and IDs before starting workers, and keeps
them outside the graph catalog until publication. A scoped graph mutation batch advances the
design revision at its boundaries instead of writing a shared revision counter
for every operation. The scope outlives the pool and its worker joins; no
revision-based analyses or design catalog mutations run inside it.
Results and failures are published in dependency order, including interleaved
SV implementations and ABI adapters. Publication transfers the graph's
`unique_ptr` into the design catalog without copying graph contents. This also
preserves hash table insertion order used by verification diagnostics.
Interface resolution, mixed-language binding, hierarchy checks, and diagnostic
publication remain coordinated. Linked-design verification and required
combinational-loop checks use independent graph workers and merge their
results in the original traversal order. A lowering proof tied to the graph's
mutation epoch can avoid repeating a local loop check; missing or invalidated
proofs still require the check.

## Elaboration and publication

The driver indexes declarations and validates FIRRTL bodies through a shared
immutable module parse cache. Interface summaries consume the cached bodies
and resolve boundary widths and reset types to a fixed point. Strict semantic
analysis runs once for each lowered module, immediately followed by lowering
in the same worker. Dependency and cover summaries survive publication;
completed lowering releases its body AST and semantic state. Parsing remains
separate from indexing, and the input mappings and cached bodies still consume
memory while their users are active. Each extmodule uses
its `defname` and typed parameters to request a real SV specialization. A small
generated SV wrapper passes parameters to slang and is discarded after importing
the implementation. Integer width, sign and all bits are preserved; strings,
reals and raw SV string parameters remain distinct.

The validation, interface-summary and lowering paths share the same parsed
module body within a transaction. Cached parse failures retain their original
source-aware diagnostics. The public owning parser API can still return a
copy for callers that need independent mutable storage.

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

An explicit `pipeline="grh-pre-flatten"` option on `read_sources` or
`read_firrtl` selects the production eight-pass endpoint before hierarchy
flattening. It uses local worker chains between XMR, recursive-driver and
blackbox catalog barriers. The default `pipeline="ingest"` selects the original
raw ingest endpoint. See [endpoint and report fields](reading.md#explicit-pre-flatten-endpoint)
for diagnostics, overlapped stage timing, final source preparation, and the
additional `<out_design>.prefix` report.

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

When a complete leaf port is absent, a shaped FIRRTL leaf can bind to its
complete set of scalar SV ports. The signature's `shapeFieldDepths` records
where each Vector dimension occurs among `fieldPath` components: depth counts
fields including the root port. For `r[0].data`, `fieldPath={r,data}`,
`shape={1}` and `shapeFieldDepths={1}`, so its SV name is `r_0_data`.
For `r.data[0]`, the depth is 2 and the name is `r_data_0`.
For `io[1].data[0]`, depths `{1,2}` produce `io_1_data_0`.
An existing complete array/packed port takes priority. Explicit port bindings
also select complete leaves; element positions are never guessed from names.

The adapter preserves the original leaf's shape, including singleton extents.
On inputs, each `kSliceArray` takes the current array value and a constant
element index, producing one element with the leading shape dimension removed.
On outputs, each `kArrayCreate` takes elements in increasing index order and
produces an array whose shape prepends that dimension. Multidimensional arrays
repeat this extraction/reassembly per dimension. For `[2][2]`, element order is
`[0][0], [0][1], [1][0], [1][1]`; `[0][0]` occupies the low bits. All scalar
ports must match element width, kind, domain and direction, be unshaped, and
cover the implementation exactly once. Missing elements, extra ports and
collisions such as array `a[0]` and scalar `a_0` fail the transaction.

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

Hierarchy flattening collects qualified node inventories and eliminated roots
across instances, then merges and deduplicates each list once before returning
to the pass manager's retention census. The batch owns its keys so source
graphs can be released first. An explicit failed-pass return also publishes
imports for the attempted expansion; it does not certify that partial graph.
Inventory imports without a source node policy remain a no-op. Per-operation
node attributes and the generated simulator's operations are unchanged.

FIRRTL array element connections can form apparent cycles at whole-value
granularity. Before publication, range analysis resolves these module-local
dependencies and rejects real local cycles. During this check, instances are
boundaries: treating every child input as a dependency of every child output
would falsely reject ready/enqueue handshakes. The simulation flow runs
`hier-flatten` followed by `comb-loop-elim` to check the actual cross-module
dependencies before conversion.

## Node annotations and bounded bringup

The XiangShan Make entry defaults `XS_FIRRTL_NODE_ANNOTATIONS=on` and records
`node_annotations=true` in the sources manifest. Explicit `off` omits optional
source node associations and their policy, inventory and retention metadata.
External implementation annotations such as BlackBox records, source locations,
and type, linking and semantic checks continue to apply.

The default pipeline profile remains `full`; downstream defaults remain
`XS_GRHSIM_FIRRTL_NODE_CSE=gsim` and `XS_GRHSIM_FIRRTL_NODES=auto`. An explicit
functional bringup can disable source annotations together with both consumers:

```sh
XS_GRHSIM_PIPELINE_PROFILE=bringup make xs_firrtl_grhsim \
  XS_FIRRTL_INPUT=ptmp/xs/rtl/rtl/SimTop.fir \
  XS_FIRRTL_BUILD=ptmp/firrtl-ingest/functional-bringup \
  XS_FIRRTL_NODE_ANNOTATIONS=off \
  XS_GRHSIM_FIRRTL_NODE_CSE=off \
  XS_GRHSIM_FIRRTL_NODES=off
```

Use the SV resources, include paths and harness configuration belonging to the
frozen input. The separate output directory distinguishes this attempt from
the full profile. The two downstream settings disable source node CSE and
nonsink source hints respectively; the upstream annotation switch alone does
not select both downstream policies.

The bringup ordinary GRH pipeline contains exactly seven stages, in order:
`xmr-resolve`, `multidriven-guard`, `blackbox-guard`, `latch-transparent-read`,
`hier-flatten`, `comb-loop-elim`, and `memory-init-check`. It omits ordinary GRH
`simplify` and its two surrounding explicit `grh.verify` calls. Conversion still
runs complete structural verification before and after `grh-to-grhsim`; later
GrhSIM optimization, partitioning and emit follow the shared pipeline.

The Make target bounds the complete FIRRTL-to-C++ producer with
`timeout --signal=KILL 20m`. Starting at producer launch, FIRRTL input, ingest,
all GRH passes, checkpoints, conversion, partitioning and C++ emit share that
single deadline. Expiry sends SIGKILL immediately; checkpoints and profiles do
not reset the budget. This documents a bringup configuration, without claiming
completed full-chip generation, simulation or equivalence. The original
annotated full-profile/prefer validation and performance comparisons remain
separate work.

## Generated source identity

Successful SV-only `readSources()` requests publish graph traversal, design
declarations, module signatures, dependencies and diagnostics in deterministic
order, independently of parallel conversion completion. Graph objects and their
IDs, graph contents, top selection order and duplicate diagnostics are preserved.
Failed requests retain their original diagnostic priority.

Linking and explicit source-unit checks use physical source paths. Before a
successful mixed design is published, generated SV paths in `ingest.sourceFile`
and operation/value source locations become
`generated-sv:fnv1a64:<content-hash>/<transaction-relative-name>`. For example,
an inline blackbox may use `generated-sv:fnv1a64:<hash>/inline/ClockGate.sv`.
This is a logical source identity, not a filesystem path. Original input paths,
line/column numbers, pass names and source notes are preserved. Diagnostics can
still name the physical transaction file used by the frontend.

This prevents request-local temporary directories from changing serialized
graph identity across otherwise identical ingests. The mixed fixture compares
complete JSON from repeated reads for inline, path and resource blackboxes.

## Ingest implementation

FIRRTL interface inference revisits modules through width/Reset dependencies.
Each analysis indexes only the interfaces it uses; parent invalidation checks
only direct children. Parsed module bodies are cached within the transaction
and released after lowering. Type/leaf caches live within one module analysis.
Source notes cached during lowering own their text, and public source positions
continue to own their file, source line and source-info strings.

Temporary F12 counters, timing scopes and profiling/cache-disable environment
switches have been removed. External benchmarks can still measure the complete
ingest call and compare serialized designs without changing the native path.

### Allocator configuration

`SLANG_USE_MIMALLOC` defaults to `ON`; Slang's integration replaces global C++
`new`/`delete`, so it also affects graph construction and transformation. For
the private bundled static allocator, `WOLVRIX_MIMALLOC_GENERIC_COLLECT`
defaults to `1000000` generic slow allocations between whole-heap scans and
accepts integers in `[1, 1000000]`. The library sets this default once during
initialization. Explicit `MIMALLOC_GENERIC_COLLECT` settings retain precedence;
the bundled allocator clamps `0` to `1`, which increases scan frequency.
Delayed-free maintenance between these scans remains enabled.

Imported/shared allocator defaults are managed by the embedding application.
`SLANG_USE_MIMALLOC=OFF` disables Slang's global replacement while retaining
its arena allocation strategy. Allocator policy is not changed by an ingest,
graph, transformation or worker. FIRRTL declaration and Driver lookup caches
remain local to one module analysis/lowering; sorted containers still determine
publication and diagnostic order.

## Validation

Use playground Make targets `test_ingest_mixed`, `test_ingest_python`,
`test_firrtl_target`, `test_wolvrix` and `test_grhsim_flow`. Compiled fixtures
cover both alternating directions, typed parameters, aggregate/flip/array ABI,
macros/includes, all three blackbox transports and edge-sensitive DPI effects.
Negative cases require no design publication. The frozen target executes original
XiangShan `MbistClockGateCell` with its inline SV `ClockGate` latch, alongside
queue/SRAM/JTAG scoreboards. This establishes neither full-chip equivalence
nor a simulation speedup.
