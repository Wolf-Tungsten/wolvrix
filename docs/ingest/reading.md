# Ingest requests and source ownership

The SystemVerilog frontend lives in `lib/ingest/sv.cpp`. Include
`ingest/sv.hpp` for `ConvertDriver` and the SV planning/lowering types. The old
`core/ingest.hpp` header has been removed.

`ingest/ingest.hpp` defines the language-independent request and result types.
It exposes no slang AST or SourceManager types. `readSources` supports pure and
mixed SV/FIRRTL requests, hierarchy linking and SV specialization/binding; see
[mixed ingestion](mixed.md). FIRRTL interface scanning validates syntax
and returns typed source owners and shared leaf signatures; see
[FIRRTL parsing](firrtl-parser.md), [combinational lowering](firrtl-combinational.md)
and [state lowering](firrtl-state.md), plus [memory lowering](firrtl-memory.md)
and [effects/layers/probes](firrtl-effects.md). A failed read does not return a
partial design.

Node attribute utilities and primitive CSE isolation are implemented separately;
see [node-attributes.md](node-attributes.md) for the current scope and schema.

```cpp
#include "ingest/ingest.hpp"

using namespace wolvrix::lib::ingest;
IngestRequest request;
request.inputRoot = "/path/to/design";
request.sources.push_back({SourceLanguage::SystemVerilog, "top.sv", "main"});
request.tops = {"top"};
request.slangArgs = {"-DTEST_MODE"};
auto result = readSources(request);
if (result.success()) {
    // Publish the design together with result.sourceOwners.
}
```

`sources` preserves input order. A relative source path is resolved against
`inputRoot`; an empty root keeps the caller's working directory. SV option strings
are passed to slang unchanged, so include/file-list paths in `slangArgs` retain
slang's existing path semantics. `tops` adds explicit `--top` selections. An empty
source list can use files supplied through `slangArgs`, preserving `read_sv(None,
slang_args=...)`.

`ModuleSignature` describes a source unit, language, logical/implementation name,
typed parameters and leaf ports. A `PortLeafSignature` contains a field path,
direction, GRH value type, element width, signedness and array shape. A scalar has
an empty shape; a 32-bit leaf with `shape={8,4}` holds 1024 bits. These signatures
describe the shared boundary used by explicit ABI adapters.

`ParameterValue` distinguishes integer, string and real values. Integers carry an
explicit width, signedness and MSB-first bit string; they are not reduced to an
untyped decimal string. `ModuleSpecialization` identifies an implementation and
typed parameters. `ExternalBinding` pairs a logical module with a specialization
and named port mappings. The mixed driver uses these records for its module
registry before either language lowers cross-language instances.

`IngestResult` owns an optional design, diagnostics, module signatures and a
`SourceOwners` collection. A failed read has no publishable design. The SV owner
holds both slang's compilation and driver, keeping its SourceManager and syntax
trees alive. Clone/copy operations share all owners; loading GRH JSON needs no
frontend owner. Ingest diagnostics own their filename, line, column and source
snippet, independently of slang locations and the read result's lifetime.

Python adds `Session.read_sources` and `Session.read_firrtl`; `read_sv` is unchanged.
Each builds an `IngestRequest` and
publishes the design and owners only after a successful read and diagnostic
conversion. Failed reads with `replace=True` preserve the existing key. Successful
replacement also works when the old key contains a Python session value. Cloning
onto the same key preserves the source owners.

## Explicit pre-flatten endpoint

`IngestRequest::pipeline` defaults to `IngestPipeline::Ingest`. Selecting
`IngestPipeline::GrhPreFlatten` additionally runs these eight production stages,
in order: `xmr-resolve`, `mux-tree-fold`, `register-write-normalize`
(`semantics=2state`), `mux-tree-fold` (`guarded_writes=2state`),
`memory-read-retime`, `multidriven-guard`, `blackbox-guard`, and
`latch-transparent-read`. Declared symbols are retained. The returned design is
ready for `hier-flatten`; callers run that pass once after the read.

Both Python mixed-source entry points accept `pipeline="grh-pre-flatten"`:

```python
diagnostics = session.read_firrtl(
    "rtl/Top.fir",
    tops=["Top"],
    firrtl_options={"node_annotations": True, "lower_threads": 10},
    pipeline="grh-pre-flatten",
    out_design="design.main",
)
prefix = session.get("design.main.prefix")
session.run_pass("hier-flatten", design="design.main", keep_declared_symbols=True)
```

The report key is the output design key followed by `.prefix`. Its
`ingest_diagnostic_count` gives the number of initial frontend diagnostics in
the returned list; subsequent entries belong to the ordered prefix stages.
`stages` holds each stage's `pass` name, complete `diagnostics`, `overlapped`,
`wall_s`, and `worker_wall_s`. Diagnostics retain stage order, each pass's
original graph order, and local emission order. Overlapping graph-local stages
have `wall_s=None`; `worker_wall_s` is accumulated worker duration and must not
be added to wall time. `prefix_wall_s` measures the prefix after raw ingestion,
including its barriers and final source preparation. The complete read call
must be timed separately to measure ingestion plus prefix. C++ exposes the
same information through `prefixStages`, `ingestDiagnosticCount`, and
`prefixWallSeconds` in `IngestResult`.

XMR resolution, recursive inout-driver checks, and blackbox catalog updates run
at design barriers. Contiguous graph-local stages run as worker chains between
these barriers, using the `lower_threads` budget. This is a published-design
pipeline; it does not execute all eight stages inside each frontend lower task.
After retention metadata is published, eligible named-port source graphs are
compacted in parallel. Top graphs and sources with positional port references
keep their existing IDs. Mutation-invalidated preparation receipts let the
next flatten consume this preparation once while preserving its historical
source-freeze diagnostic counts. These receipts are not serialized.

The benchmark's `--fused-prefix` option selects this endpoint. Formal runs store
the observed joint read duration as `ingest_prefix_s` and set `ingest_s=None`.
Correctness mode independently reads the original sources once for the raw
ingest identity and again for this endpoint followed by flatten; JSON and
hashing time is excluded from formal timing runs. The default API endpoint
continues to return the raw ingest design.

Run validation through the playground Makefile:

```sh
make test_wolvrix
make py_install
make test_ingest_session SKIP_PY_INSTALL=1
make test_grhsim_flow SKIP_PY_INSTALL=1
make run_hdlbits_test DUT=001 SKIP_PY_INSTALL=1
```

The CTest `ingest-driver` case exercises SV dispatch, ownership after releasing
the original result, owned error locations, FIRRTL scanning and staged rejection
of unsupported constructs. The FIRRTL suites also exercise actual combinational/state
lowering and generated GrhSIM execution. The session
tests cover failed replacement, diagnostics, clone/copy and argument-only reads.
