# Ingest requests and source ownership

The SystemVerilog frontend lives in `lib/ingest/sv.cpp`. Include
`ingest/sv.hpp` for `ConvertDriver` and the SV planning/lowering types. The old
`core/ingest.hpp` header has been removed.

`ingest/ingest.hpp` defines the language-independent request and result types.
It exposes no slang AST or SourceManager types. `readSources` supports SV requests
and pure FIRRTL combinational/state requests. FIRRTL interface scanning validates syntax
and returns typed source owners and shared leaf signatures; see
[FIRRTL parsing](firrtl-parser.md), [combinational lowering](firrtl-combinational.md)
and [state lowering](firrtl-state.md). Memories, effects, instances, explicit specialization, external binding
and cross-language linking return an error diagnostic at this stage. A failed read
does not return a partial design. Mixed elaboration is a later implementation stage.

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
describe the shared boundary; they do not implement ABI adapters yet.

`ParameterValue` distinguishes integer, string and real values. Integers carry an
explicit width, signedness and MSB-first bit string; they are not reduced to an
untyped decimal string. `ModuleSpecialization` identifies an implementation and
typed parameters. `ExternalBinding` pairs a logical module with a specialization
and named port mappings. The mixed driver will use these records for its module
registry before either language lowers cross-language instances.

`IngestResult` owns an optional design, diagnostics, module signatures and a
`SourceOwners` collection. A failed read has no publishable design. The SV owner
holds both slang's compilation and driver, keeping its SourceManager and syntax
trees alive. Clone/copy operations share all owners; loading GRH JSON needs no
frontend owner. Ingest diagnostics own their filename, line, column and source
snippet, independently of slang locations and the read result's lifetime.

The Python `Session.read_sv` API is unchanged. It builds an `IngestRequest` and
publishes the design and owners only after a successful read and diagnostic
conversion. Failed reads with `replace=True` preserve the existing key. Successful
replacement also works when the old key contains a Python session value. Cloning
onto the same key preserves the source owners.

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
