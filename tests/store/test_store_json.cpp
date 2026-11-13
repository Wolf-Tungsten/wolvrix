#include "core/store.hpp"
#include "core/grh.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace wolvrix::lib::store;
using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[store_json] " << message << '\n';
        return 1;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream stream(path);
        if (!stream.is_open())
        {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    Design buildDemoDesign()
    {
        Design design;
        Graph &graph = design.createGraph("demo");

        ValueId in = graph.createValue(graph.internSymbol("in"), 8, false);
        graph.bindInputPort("in", in);

        ValueId out = graph.createValue(graph.internSymbol("out"), 8, false);
        graph.bindOutputPort("out", out);

        OperationId add = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add0"));
        graph.addOperand(add, in);
        ValueId sum = graph.createValue(graph.internSymbol("sum"), 8, false);
        graph.addResult(add, sum);
        graph.setAttr(add, "weights", AttributeValue(std::vector<int64_t>{1, 2}));

        OperationId assign = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign0"));
        graph.addOperand(assign, sum);
        graph.addResult(assign, out);

        const std::vector<int64_t> memShape{4, 2};
        ValueId arr = graph.createValue(graph.internSymbol("arr"), 64, false, ValueType::Logic, memShape);

        // kAssign: bit-identical retype of the array value into a flat view.
        ValueId arrFlat = graph.createValue(graph.internSymbol("arr_flat"), 64, false);
        OperationId view = graph.createOperation(OperationKind::kAssign, graph.internSymbol("view0"));
        graph.addOperand(view, arr);
        graph.addResult(view, arrFlat);

        design.registerGraphAlias("demo_alias", graph);
        design.markAsTop(graph.symbol());
        return design;
    }

} // namespace

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

int main()
{
    // Case 1: missing top graphs should fail gracefully.
    StoreDiagnostics diagNoTop;
    StoreJson emitterNoTop(&diagNoTop);
    Design emptyDesign;
    StoreResult resultNoTop = emitterNoTop.store(emptyDesign);
    if (resultNoTop.success)
    {
        return fail("StoreJson should fail when no tops are present");
    }
    if (!diagNoTop.hasError())
    {
        return fail("Expected diagnostics to capture missing tops for StoreJson");
    }

    Design design = buildDemoDesign();

    // Case 2: prettyCompact JSON emission with compact keys.
    StoreDiagnostics diagPrettyCompact;
    StoreJson emitterPrettyCompact(&diagPrettyCompact);
    StoreOptions prettyCompactOptions;
    prettyCompactOptions.outputDir = std::string(WOLF_SV_EMIT_ARTIFACT_DIR);
    prettyCompactOptions.outputFilename = "store_json_pretty_compact.json";

    StoreResult prettyCompactResult = emitterPrettyCompact.store(design, prettyCompactOptions);
    if (!prettyCompactResult.success)
    {
        return fail("StoreJson prettyCompact path failed");
    }
    if (diagPrettyCompact.hasError())
    {
        return fail("Unexpected diagnostics errors for prettyCompact emit");
    }
    if (prettyCompactResult.artifacts.empty())
    {
        return fail("PrettyCompact emit did not report an artifact");
    }

    const std::filesystem::path prettyCompactPath = prettyCompactResult.artifacts.front();
    const std::string prettyCompactJson = readFile(prettyCompactPath);
    if (prettyCompactJson.find("\"vals\"") == std::string::npos || prettyCompactJson.find("\"ops\"") == std::string::npos)
    {
        return fail("Compressed keys vals/ops not found in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"tops\"") == std::string::npos)
    {
        return fail("Top graph list is missing in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"aliases\"") == std::string::npos ||
        prettyCompactJson.find("demo_alias") == std::string::npos)
    {
        return fail("Alias map is missing in prettyCompact JSON");
    }
    if (prettyCompactJson.find("\"attrs\"") == std::string::npos || prettyCompactJson.find("\"int[]\"") == std::string::npos)
    {
        return fail("Attribute payload missing expected compact layout");
    }
    // v5 graph-local references are JSON numbers, never strings.
    if (prettyCompactJson.find("\"val\": \"") != std::string::npos ||
        prettyCompactJson.find("\"def\": \"") != std::string::npos ||
        prettyCompactJson.find("\"op\": \"") != std::string::npos ||
        prettyCompactJson.find("\"in\": [\"") != std::string::npos ||
        prettyCompactJson.find("\"out\": [\"") != std::string::npos)
    {
        return fail("v5 graph-local references were emitted as JSON strings");
    }
    const std::size_t valsInlinePos = prettyCompactJson.find("{\"id\": 1, \"sym\": \"in\"");
    if (valsInlinePos == std::string::npos)
    {
        return fail("Value entry not rendered inline in prettyCompact mode");
    }
    const std::size_t newlineAfterVal = prettyCompactJson.find('\n', valsInlinePos);
    const std::size_t braceAfterVal = prettyCompactJson.find('}', valsInlinePos);
    if (newlineAfterVal != std::string::npos && newlineAfterVal < braceAfterVal)
    {
        return fail("Value entry spans multiple lines in prettyCompact mode");
    }

    Design parsed = Design::fromJsonString(prettyCompactJson);
    if (!parsed.findGraph("demo"))
    {
        return fail("Round-trip parsed design missing demo graph");
    }
    Graph *aliasGraph = parsed.findGraph("demo_alias");
    if (!aliasGraph || aliasGraph->symbol() != "demo")
    {
        return fail("Round-trip parsed design missing alias mapping");
    }
    Graph *parsedDemo = parsed.findGraph("demo");
    ValueId parsedArr = parsedDemo->findValue("arr");
    if (!parsedArr.valid())
    {
        return fail("Round-trip parsed design missing arr value");
    }
    const std::span<const int64_t> parsedShape = parsedDemo->valueShape(parsedArr);
    if (parsedShape.size() != 2 || parsedShape[0] != 4 || parsedShape[1] != 2)
    {
        return fail("Round-trip parsed design lost the arr shape");
    }
    if (prettyCompactJson.find("\"shape\"") == std::string::npos)
    {
        return fail("Shape key missing in prettyCompact JSON");
    }

    bool foundAssignView = false;
    for (const OperationId opId : parsedDemo->operations())
    {
        if (parsedDemo->opKind(opId) == OperationKind::kAssign)
        {
            const auto operands = parsedDemo->opOperands(opId);
            const auto results = parsedDemo->opResults(opId);
            if (operands.size() == 1 && results.size() == 1 &&
                parsedDemo->valueShape(operands.front()).size() == 2 &&
                parsedDemo->valueShape(results.front()).empty())
            {
                foundAssignView = true;
            }
        }
    }
    if (!foundAssignView)
    {
        return fail("Round-trip parsed design lost the kAssign view");
    }

    // Case 3: legacy formats are rejected rather than migrated.
    const std::string legacyJson = R"({
  "graphs": [{
    "symbol": "legacy",
    "formatVersion": 3,
    "dialects": {"grh": 2},
    "metadata": {},
    "declaredSymbols": [],
    "vals": [],
    "ports": {"in": [], "out": [], "inout": []},
    "ops": []
  }],
  "aliases": {},
  "declaredSymbols": [],
  "tops": ["legacy"]
})";
    bool legacyRejected = false;
    try
    {
        auto ignored = Design::fromJsonString(legacyJson);
        (void)ignored;
    }
    catch (const std::exception &)
    {
        legacyRejected = true;
    }
    if (!legacyRejected) return fail("v3 JSON was accepted by the v5-only loader");

    // Case 4: v5 local IDs are graph-local and must remain valid when the
    // operation array is reordered and its IDs are sparse.  In particular,
    // use-list validation must compare encoded local IDs rather than runtime
    // OperationId indices, which follow parse order.
    const std::string reorderedJson = R"({
  "formatVersion": 5,
  "graphs": [{
    "symbol": "reordered",
    "dialects": {"grh": 2},
    "metadata": {},
    "declaredSymbols": [],
    "vals": [
      {"id": 41, "sym": "in", "w": 1, "sgn": false, "type": "logic", "domain": "4state", "in": true, "out": false, "inout": false, "users": [{"op": 1001, "idx": 0}, {"op": 73, "idx": 0}]},
      {"id": 900, "w": 1, "sgn": false, "type": "logic", "domain": "4state", "in": false, "out": false, "inout": false, "def": 1001, "users": []},
      {"id": 901, "w": 1, "sgn": false, "type": "logic", "domain": "4state", "in": false, "out": false, "inout": false, "def": 73, "users": []}
    ],
    "ports": {"in": [{"name": "in", "val": 41}], "out": [], "inout": []},
    "ops": [
      {"id": 1001, "kind": "kAssign", "in": [41], "out": [900]},
      {"id": 73, "kind": "kAssign", "in": [41], "out": [901]}
    ]
  }],
  "aliases": {},
  "declaredSymbols": [],
  "tops": ["reordered"]
})";
    Design reordered = Design::fromJsonString(reorderedJson);
    Graph *reorderedGraph = reordered.findGraph("reordered");
    if (!reorderedGraph || reorderedGraph->operations().size() != 2 ||
        reorderedGraph->valueUsers(reorderedGraph->findValue("in")).size() != 2)
    {
        return fail("v5 sparse local IDs or reordered operations were not loaded");
    }

    const auto replaceOnce = [](std::string text, const std::string &from, const std::string &to) {
        const std::size_t position = text.find(from);
        if (position == std::string::npos) return std::string{};
        text.replace(position, from.size(), to);
        return text;
    };
    const auto expectLoadFailure = [&](const std::string &json, const char *label) {
        if (json.empty()) return fail(std::string("Could not construct malformed JSON for ") + label);
        try
        {
            auto ignored = Design::fromJsonString(json);
            (void)ignored;
        }
        catch (const std::exception &)
        {
            return 0;
        }
        return fail(std::string("Malformed v5 JSON was accepted: ") + label);
    };
    if (expectLoadFailure(replaceOnce(reorderedJson, "\"id\": 901", "\"id\": 900"),
                          "duplicate value ID") != 0 ||
        expectLoadFailure(replaceOnce(reorderedJson, "\"id\": 73", "\"id\": 1001"),
                          "duplicate operation ID") != 0 ||
        expectLoadFailure(replaceOnce(reorderedJson, "\"val\": 41", "\"val\": \"41\""),
                          "string value reference") != 0 ||
        expectLoadFailure(replaceOnce(reorderedJson, "\"in\": [41]", "\"in\": [1001]"),
                          "operation/value namespace mismatch") != 0 ||
        expectLoadFailure(replaceOnce(reorderedJson, "\"def\": 73", "\"def\": 9999"),
                          "unknown operation reference") != 0)
    {
        return 1;
    }

    // Case 6: compact mode should differ from prettyCompact output and avoid newlines.
    StoreDiagnostics diagCompact;
    StoreJson emitterCompact(&diagCompact);
    StoreOptions compactOptions = prettyCompactOptions;
    compactOptions.jsonMode = JsonPrintMode::Compact;
    compactOptions.outputFilename = "store_json_compact.json";

    StoreResult compactResult = emitterCompact.store(design, compactOptions);
    if (!compactResult.success || diagCompact.hasError())
    {
        return fail("Compact emit failed");
    }

    const std::string compactJson = readFile(compactResult.artifacts.front());
    if (compactJson == prettyCompactJson)
    {
        return fail("Compact emit should produce different layout from prettyCompact emit");
    }
    if (compactJson.find('\n') != std::string::npos)
    {
        return fail("Compact JSON should not contain newlines");
    }

    // Case 5: preserve the existing small-design byte layout independently of
    // the shared string/file writer.
    Design goldenDesign;
    goldenDesign.createGraph("empty");
    goldenDesign.markAsTop("empty");
    const std::string golden = R"({
  "formatVersion": 5,
  "graphs": [
    {
      "symbol": "empty",
      "dialects": {"grh": 2},
      "metadata": {},
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": []
    }
  ],
  "aliases": {},
  "declaredSymbols": [],
  "tops": [
    "empty"
  ]
})";
    const auto goldenJson = emitterPrettyCompact.storeToString(goldenDesign);
    if (!goldenJson || *goldenJson != golden)
    {
        return fail("PrettyCompact layout differs from the existing golden bytes");
    }

    // Case 6: metadata and attribute payloads span several output buffers,
    // including escapes that can straddle a buffer boundary. The same fixture
    // covers all eight AttributeValue branches (weights is the int64 list).
    std::string payload;
    const std::string escaped = std::string("quote\" slash\\\n\r\t\b\f") + char(1) + char(0);
    while (payload.size() < 3 * 65536)
    {
        payload.append(escaped);
    }
    Graph *richGraph = design.findGraph("demo");
    // The loader's JSON object traversal sorts metadata keys. Use that order
    // for exact load/store bytes; Compact preserves metadata insertion order.
    richGraph->setMetadata("a-first", std::string("metadata"));
    richGraph->setMetadata("z-last", std::vector<std::string>{payload, "tail"});
    const OperationId richAdd = richGraph->findOperation("add0");
    richGraph->setAttr(richAdd, "originModules", std::vector<std::string>{"top$core", "top$memory"});
    richGraph->setAttr(richAdd, "escaped", payload);
    richGraph->setAttr(richAdd, "flags", std::vector<bool>{true, false});
    richGraph->setAttr(richAdd, "ratios", std::vector<double>{1.25, -2.5});
    richGraph->setAttr(richAdd, "scale", 1.125);
    richGraph->setAttr(richAdd, "offset", int64_t(-17));
    richGraph->setAttr(richAdd, "enabled", true);
    const std::vector<std::string> expectedKeys{
        "enabled", "escaped", "flags", "offset", "originModules", "ratios", "scale", "weights"};
    std::vector<std::string> actualKeys;
    for (const auto attr : richGraph->opAttrRange(richAdd)) actualKeys.emplace_back(attr.key());
    if (actualKeys != expectedKeys)
    {
        return fail("Rich attributes were not stored in key text order");
    }
    SrcLoc location;
    location.file = "source\\with\"escapes.sv";
    location.line = 7;
    location.note = escaped;
    richGraph->setOpSrcLoc(richAdd, location);
    for (const JsonPrintMode mode : {JsonPrintMode::PrettyCompact, JsonPrintMode::Compact, JsonPrintMode::Pretty})
    {
        StoreDiagnostics diagnostics;
        StoreJson emitter(&diagnostics);
        StoreOptions options = prettyCompactOptions;
        options.jsonMode = mode;
        options.outputFilename = "store_json_rich_" + std::to_string(static_cast<int>(mode)) + ".json";
        const auto production = emitter.storeToString(design, options);
        if (!production || production->find("\"def\"") != std::string::npos ||
            production->find("\"users\"") != std::string::npos ||
            production->find("\"loc\"") != std::string::npos)
        {
            return fail("Default JSON retained def/users/loc");
        }
        options.emitDefUsers = true;
        options.emitSourceLocations = true;
        const auto expected = emitter.storeToString(design, options);
        const StoreResult result = emitter.store(design, options);
        if (!expected || !result.success || diagnostics.hasError() || result.artifacts.size() != 1)
        {
            return fail("Rich string/file JSON store failed");
        }
        const std::string actual = readFile(result.artifacts.front());
        if (actual != *expected)
        {
            return fail("File output differs byte-for-byte from storeToString");
        }
        Design roundtrip = Design::fromJsonString(actual);
        const auto restoredJson = emitter.storeToString(roundtrip, options);
        if (!restoredJson || *restoredJson != actual || diagnostics.hasError())
        {
            if (restoredJson)
            {
                const auto path = std::filesystem::path(*options.outputDir) /
                    ("store_json_rich_restored_" + std::to_string(static_cast<int>(mode)) + ".json");
                std::ofstream restoredFile(path, std::ios::binary);
                restoredFile.write(restoredJson->data(), static_cast<std::streamsize>(restoredJson->size()));
                std::size_t firstDifference = 0;
                while (firstDifference < actual.size() && firstDifference < restoredJson->size() &&
                       actual[firstDifference] == (*restoredJson)[firstDifference]) ++firstDifference;
                std::cerr << "[store_json] mode=" << static_cast<int>(mode)
                          << " first differing byte=" << firstDifference
                          << " original bytes=" << actual.size() << " restored bytes=" << restoredJson->size()
                          << " restored artifact=" << path << '\n';
            }
            return fail("Rich JSON load/store changed bytes in the same print mode");
        }
        const Graph *restoredGraph = roundtrip.findGraph("demo");
        if (!restoredGraph)
        {
            return fail("Rich JSON round-trip lost the graph");
        }
        for (const AttrKV &attr : richGraph->metadata())
        {
            if (restoredGraph->metadata(attr.key) != std::optional<AttributeValue>(attr.value))
            {
                return fail("Rich JSON round-trip changed graph metadata");
            }
        }
        const Operation restoredAdd = restoredGraph->getOperation(restoredGraph->findOperation("add0"));
        actualKeys.clear();
        for (const auto attr : restoredGraph->opAttrRange(restoredAdd.id())) actualKeys.emplace_back(attr.key());
        if (actualKeys != expectedKeys)
        {
            return fail("Rich JSON round-trip changed attribute key text order");
        }
        for (const auto attr : richGraph->opAttrRange(richAdd))
        {
            if (restoredAdd.attr(attr.key()) != std::optional<AttributeValue>(attr.value().toOwned()))
            {
                return fail("Rich JSON round-trip changed an attribute or origin");
            }
        }
        const auto restoredLocation = restoredGraph->opSrcLoc(restoredAdd.id());
        if (!restoredLocation || restoredLocation->file != location.file ||
            restoredLocation->line != location.line || restoredLocation->note != location.note)
        {
            return fail("Rich JSON round-trip changed escaped source locations");
        }
    }

    // Case 6: failed output must not publish a successful artifact.
    StoreDiagnostics openDiagnostics;
    StoreJson openEmitter(&openDiagnostics);
    StoreOptions failedOptions = prettyCompactOptions;
    failedOptions.outputFilename = "store_json_output_directory";
    std::filesystem::create_directories(std::filesystem::path(*failedOptions.outputDir) / *failedOptions.outputFilename);
    const StoreResult openFailure = openEmitter.store(design, failedOptions);
    if (openFailure.success || !openFailure.artifacts.empty() || !openDiagnostics.hasError())
    {
        return fail("Opening a directory must fail without publishing an artifact");
    }

    if (std::filesystem::exists("/dev/full"))
    {
        const std::filesystem::path fullPath = std::filesystem::path(*failedOptions.outputDir) / "store_json_dev_full";
        std::filesystem::remove(fullPath);
        std::filesystem::create_symlink("/dev/full", fullPath);
        failedOptions.outputFilename = fullPath.filename().string();
        for (const JsonPrintMode mode : {JsonPrintMode::PrettyCompact, JsonPrintMode::Compact, JsonPrintMode::Pretty})
        {
            failedOptions.jsonMode = mode;
            // The small design fails when flushing the final buffer; the
            // rich design fails while writing an intermediate buffer.
            for (const Design *source : {&goldenDesign, &design})
            {
                StoreDiagnostics diagnostics;
                StoreJson emitter(&diagnostics);
                const StoreResult result = emitter.store(*source, failedOptions);
                if (result.success || !result.artifacts.empty() || !diagnostics.hasError())
                {
                    return fail("A failed JSON write/flush must not publish an artifact");
                }
            }
        }
        std::filesystem::remove(fullPath);
    }

    // Case 7: serialization can fail after earlier graphs have been written.
    // A partial file is not a successful checkpoint artifact.
    Graph &brokenGraph = design.createGraph("z-broken");
    brokenGraph.createOperation(static_cast<OperationKind>(-1), brokenGraph.internSymbol("unknown"));
    StoreDiagnostics serializationDiagnostics;
    StoreJson serializationEmitter(&serializationDiagnostics);
    failedOptions.jsonMode = JsonPrintMode::PrettyCompact;
    failedOptions.outputFilename = "store_json_partial_failure.json";
    const StoreResult serializationFailure = serializationEmitter.store(design, failedOptions);
    if (serializationFailure.success || !serializationFailure.artifacts.empty() ||
        !serializationDiagnostics.hasError())
    {
        return fail("A serialization exception must not publish an artifact");
    }

    return 0;
}
