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
    const std::size_t valsInlinePos = prettyCompactJson.find("{\"sym\": \"in\"");
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

    // Case 3: compact mode should differ from prettyCompact output and avoid newlines.
    StoreDiagnostics diagCompact;
    StoreJson emitterCompact(&diagCompact);
    StoreOptions compactOptions = prettyCompactOptions;
    compactOptions.jsonMode = JsonPrintMode::Compact;

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

    return 0;
}
