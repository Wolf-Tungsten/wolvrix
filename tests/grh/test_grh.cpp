#include "core/store.hpp"
#include "core/grh.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::store;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grh_tests] " << message << '\n';
        return 1;
    }

    template <typename Func>
    bool expectThrows(Func &&func)
    {
        try
        {
            func();
        }
        catch (const std::exception &)
        {
            return true;
        }
        return false;
    }

} // namespace

int main()
{
    try
    {
        Design design;
        Graph &graph = design.createGraph("demo");

        SymbolId symA = graph.internSymbol("a");
        SymbolId symB = graph.internSymbol("b");
        SymbolId symSum = graph.internSymbol("sum");
        SymbolId symSumCopy = graph.internSymbol("sum_copy");
        SymbolId symLane1 = graph.internSymbol("lane1");
        SymbolId symLane0 = graph.internSymbol("lane0");
        SymbolId symPacked = graph.internSymbol("packed_lanes");
        SymbolId symPackedIndex = graph.internSymbol("packed_index");
        SymbolId symPackedOut = graph.internSymbol("packed_out");
        ValueId a = graph.createValue(symA, 8, false);
        ValueId b = graph.createValue(symB, 8, false);
        ValueId sum = graph.createValue(symSum, 8, false);
        ValueId sumCopy = graph.createValue(symSumCopy, 8, false);
        ValueId lane1 = graph.createValue(symLane1, 3, false);
        ValueId lane0 = graph.createValue(symLane0, 3, false);
        ValueId packed = graph.createValue(symPacked, 6, false);
        ValueId packedIndex = graph.createValue(symPackedIndex, 1, false);
        ValueId packedOut = graph.createValue(symPackedOut, 3, false);

        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("lane1", lane1);
        graph.bindInputPort("lane0", lane0);
        graph.bindInputPort("packed_index", packedIndex);
        graph.bindOutputPort("sum", sum);
        graph.bindOutputPort("sum_copy", sumCopy);
        graph.bindOutputPort("packed_out", packedOut);
        graph.addDeclaredSymbol(symA);
        graph.addDeclaredSymbol(symSum);
        design.addDeclaredSymbol(design.internSymbol("demo"));

        OperationId op = graph.createOperation(OperationKind::kAdd, graph.internSymbol("add0"));
        graph.addOperand(op, a);
        graph.addOperand(op, b);
        graph.addResult(op, sum);

        bool threwOnNaNAttribute = expectThrows([&]
                                                { graph.setAttr(op, "invalid_nan",
                                                                AttributeValue(std::numeric_limits<double>::quiet_NaN())); });
        if (!threwOnNaNAttribute)
        {
            return fail("Expected NaN attribute to throw");
        }

        bool threwOnDoubleArrayInfinity = expectThrows([&]
                                                       { graph.setAttr(op, "invalid_array",
                                                                       AttributeValue(std::vector<double>{0.25, std::numeric_limits<double>::infinity()})); });
        if (!threwOnDoubleArrayInfinity)
        {
            return fail("Expected double array with infinity to throw");
        }

        OperationId assignOp = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign0"));
        graph.addOperand(assignOp, sum);
        graph.addResult(assignOp, sumCopy);
        if (graph.getValue(sumCopy).definingOp() != assignOp)
        {
            return fail("Assign result defining operation not set");
        }

        OperationId packedConcat = graph.createOperation(OperationKind::kConcat, graph.internSymbol("packed_concat"));
        graph.addOperand(packedConcat, lane1);
        graph.addOperand(packedConcat, lane0);
        graph.addResult(packedConcat, packed);
        graph.setAttr(packedConcat, "svPackedArray.version", AttributeValue(int64_t(1)));
        graph.setAttr(packedConcat, "svPackedArray.elementWidth", AttributeValue(int64_t(3)));
        graph.setAttr(packedConcat, "svPackedArray.elementCount", AttributeValue(int64_t(2)));
        graph.setAttr(packedConcat, "svPackedArray.indexLow", AttributeValue(int64_t(0)));
        graph.setAttr(packedConcat, "svPackedArray.indexHigh", AttributeValue(int64_t(1)));
        graph.setAttr(packedConcat, "svPackedArray.indexDirection", AttributeValue(std::string("downto")));
        graph.setAttr(packedConcat, "svPackedArray.laneOrder", AttributeValue(std::string("lsb_index_low")));
        graph.setAttr(packedConcat, "svPackedArray.concat.operand0Index", AttributeValue(int64_t(1)));
        graph.setAttr(packedConcat, "svPackedArray.concat.operandStride", AttributeValue(int64_t(-1)));

        OperationId packedSlice = graph.createOperation(OperationKind::kSliceArray, graph.internSymbol("packed_slice"));
        graph.addOperand(packedSlice, packed);
        graph.addOperand(packedSlice, packedIndex);
        graph.addResult(packedSlice, packedOut);
        graph.setAttr(packedSlice, "sliceWidth", AttributeValue(int64_t(3)));

        const Value aValue = graph.getValue(a);
        const auto aUsers = aValue.users();
        if (aUsers.size() != 1 || aUsers[0].operation != op)
        {
            return fail("Operand usage tracking failed");
        }
        if (graph.getValue(sum).definingOp() != op)
        {
            return fail("Result defining operation not set");
        }

        ValueId c = graph.createValue(graph.internSymbol("c"), 8, false);
        graph.bindInputPort("c", c);

        graph.replaceOperand(op, 1, c);
        if (graph.getOperation(op).operands()[1] != c)
        {
            return fail("Operand replacement did not update slot");
        }
        if (!graph.getValue(b).users().empty())
        {
            return fail("Old operand still listed as user after replacement");
        }
        const Value cValue = graph.getValue(c);
        const auto cUsers = cValue.users();
        if (cUsers.size() != 1 || cUsers[0].operation != op || cUsers[0].operandIndex != 1)
        {
            return fail("New operand usage tracking incorrect after replacement");
        }

        Graph &auxGraph = design.createGraph("aux");
        ValueId foreignValue = auxGraph.createValue(auxGraph.internSymbol("foreign"), 8, false);
        bool threwOnForeignOperand = expectThrows([&]
                                                  { graph.replaceOperand(op, 0, foreignValue); });
        if (!threwOnForeignOperand)
        {
            return fail("Expected replacing operand with foreign value to throw");
        }

        OperationId passThrough = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign1"));
        graph.addOperand(passThrough, c);
        ValueId passResult = graph.createValue(graph.internSymbol("passthrough"), 8, false);
        graph.addResult(passThrough, passResult);

        ValueId passResultAlt = graph.createValue(graph.internSymbol("passthrough_alt"), 8, false);
        graph.replaceResult(passThrough, 0, passResultAlt);
        if (graph.getValue(passResult).definingOp().valid())
        {
            return fail("Old result still records defining operation after replacement");
        }
        if (graph.getValue(passResultAlt).definingOp() != passThrough)
        {
            return fail("New result defining operation incorrect after replacement");
        }
        if (graph.getOperation(passThrough).results()[0] != passResultAlt)
        {
            return fail("Result replacement did not update slot");
        }

        bool threwOnForeignResult = expectThrows([&]
                                                 { graph.replaceResult(passThrough, 0, foreignValue); });
        if (!threwOnForeignResult)
        {
            return fail("Expected replacing result with foreign value to throw");
        }

        ValueId existingResult = graph.createValue(graph.internSymbol("existing_result"), 8, false);
        OperationId existingProducer = graph.createOperation(OperationKind::kAssign, graph.internSymbol("assign_existing"));
        graph.addOperand(existingProducer, c);
        graph.addResult(existingProducer, existingResult);

        bool threwOnExistingResult = expectThrows([&]
                                                  { graph.replaceResult(passThrough, 0, existingResult); });
        if (!threwOnExistingResult)
        {
            return fail("Expected replacing result with already-defined value to throw");
        }

        bool threwOnDuplicateValue = expectThrows([&]
                                                  { graph.createValue(graph.internSymbol("a"), 1, false); });
        if (!threwOnDuplicateValue)
        {
            return fail("Expected duplicate value symbol to throw");
        }

        bool threwOnInvalidWidth = expectThrows([&]
                                                { graph.createValue(graph.internSymbol("invalid"), 0, false); });
        if (!threwOnInvalidWidth)
        {
            return fail("Expected zero width to throw");
        }

        design.markAsTop("demo");

        // Generate group annotation: two per-round copies of one declaration.
        const SymbolId symGenScope = graph.internSymbol("gen_loop");
        const SymbolId symGenName = graph.internSymbol("sig");
        const SymbolId symGen0 = graph.internSymbol("gen_loop$0$sig");
        const SymbolId symGen1 = graph.internSymbol("gen_loop$1$sig");
        graph.createValue(symGen0, 8, false);
        graph.createValue(symGen1, 8, false);
        graph.addDeclaredSymbol(symGen0);
        graph.addDeclaredSymbol(symGen1);
        const std::size_t genGroup = graph.addGenerateGroup(symGenScope, symGenName);
        graph.addGenerateGroupSymbol(genGroup, symGen0);
        graph.addGenerateGroupSymbol(genGroup, symGen1);
        // Post-flatten shape: instance prefix folded into the scope, the bare
        // declaration name symbol shared, members renamed hierarchically.
        const SymbolId symHierScope = graph.internSymbol("u_child$gen_loop");
        const SymbolId symHierGen0 = graph.internSymbol("u_child$gen_loop$0$sig");
        const SymbolId symHierGen1 = graph.internSymbol("u_child$gen_loop$1$sig");
        graph.createValue(symHierGen0, 8, false);
        graph.createValue(symHierGen1, 8, false);
        graph.addDeclaredSymbol(symHierGen0);
        graph.addDeclaredSymbol(symHierGen1);
        const std::size_t hierGroup = graph.addGenerateGroup(symHierScope, symGenName);
        graph.addGenerateGroupSymbol(hierGroup, symHierGen0);
        graph.addGenerateGroupSymbol(hierGroup, symHierGen1);
        if (graph.generateGroups().size() != 2 ||
            graph.generateGroups().front().symbols.size() != 2 ||
            graph.generateGroups().back().symbols.size() != 2)
        {
            return fail("Generate group registration failed");
        }

        // eraseOp/eraseValue must drop the erased entity's symbol from every
        // generate group while keeping the (possibly empty) group itself.
        Graph &eraseGraph = design.createGraph("erase_case");
        const SymbolId eraseScope = eraseGraph.internSymbol("gen");
        const SymbolId eraseName = eraseGraph.internSymbol("w");
        const SymbolId eraseSymVal = eraseGraph.internSymbol("gen$0$w");
        const SymbolId eraseSymOp = eraseGraph.internSymbol("gen$1$w");
        const SymbolId eraseSymKeep = eraseGraph.internSymbol("gen$2$w");
        const ValueId eraseVal = eraseGraph.createValue(eraseSymVal, 8, false);
        eraseGraph.createValue(eraseSymKeep, 8, false);
        const OperationId eraseOp =
            eraseGraph.createOperation(OperationKind::kConstant, eraseSymOp);
        eraseGraph.setAttr(eraseOp, "constValue", AttributeValue(std::string("8'h0")));
        eraseGraph.addDeclaredSymbol(eraseSymVal);
        eraseGraph.addDeclaredSymbol(eraseSymOp);
        eraseGraph.addDeclaredSymbol(eraseSymKeep);
        const std::size_t eraseGroup =
            eraseGraph.addGenerateGroup(eraseScope, eraseName);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymVal);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymOp);
        eraseGraph.addGenerateGroupSymbol(eraseGroup, eraseSymKeep);
        if (!eraseGraph.eraseValue(eraseVal))
        {
            return fail("eraseValue failed for generate group member");
        }
        if (!eraseGraph.eraseOp(eraseOp))
        {
            return fail("eraseOp failed for generate group member");
        }
        if (eraseGraph.isDeclaredSymbol(eraseSymVal) || eraseGraph.isDeclaredSymbol(eraseSymOp))
        {
            return fail("Erased symbols still marked declared");
        }
        if (eraseGraph.generateGroups().size() != 1)
        {
            return fail("Erase removed the generate group itself");
        }
        const auto &eraseGroupSymbols = eraseGraph.generateGroups().front().symbols;
        if (eraseGroupSymbols.size() != 1 || eraseGroupSymbols.front() != eraseSymKeep)
        {
            return fail("Generate group membership not cleaned on erase");
        }
        if (eraseGraph.lookupSymbol("gen$0$w").valid() &&
            eraseGraph.findValue("gen$0$w").valid())
        {
            return fail("Erased value still resolvable");
        }
        if (!eraseGraph.validateDeclaredSymbols().empty())
        {
            return fail("validateDeclaredSymbols reports issues on a consistent graph");
        }

        // Renaming a declared value leaves its old symbol dangling in the
        // annotations; validateDeclaredSymbols must flag every entry.
        {
            Design danglingDesign;
            Graph &danglingGraph = danglingDesign.createGraph("dangling_case");
            const SymbolId danglingSym = danglingGraph.internSymbol("sig");
            const ValueId danglingVal = danglingGraph.createValue(danglingSym, 8, false);
            danglingGraph.addDeclaredSymbol(danglingSym);
            const std::size_t danglingGroup = danglingGraph.addGenerateGroup(
                danglingGraph.internSymbol("gen"), danglingGraph.internSymbol("w"));
            danglingGraph.addGenerateGroupSymbol(danglingGroup, danglingSym);
            if (!danglingGraph.validateDeclaredSymbols().empty())
            {
                return fail("validateDeclaredSymbols false positive before rename");
            }
            danglingGraph.setValueSymbol(danglingVal, danglingGraph.internSymbol("renamed"));
            if (danglingGraph.validateDeclaredSymbols().size() != 2)
            {
                return fail("validateDeclaredSymbols missed dangling entries");
            }
        }

        StoreDiagnostics emitDiagnostics;
        StoreJson emitter(&emitDiagnostics);
        StoreOptions emitOptions;
        auto jsonOpt = emitter.storeToString(design, emitOptions);
        if (!jsonOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to emit JSON for design");
        }
        std::string json = *jsonOpt;

#ifdef GRH_STAGE1_JSON_PATH
        try
        {
            namespace fs = std::filesystem;
            const fs::path artifactPath = fs::path(GRH_STAGE1_JSON_PATH);
            const fs::path artifactDir = artifactPath.parent_path();

            if (!artifactDir.empty())
            {
                std::error_code ec;
                fs::create_directories(artifactDir, ec);
                if (ec)
                {
                    return fail(std::string("Failed to create artifact directory: ") + ec.message());
                }
            }

            std::ofstream outFile(artifactPath);
            if (!outFile)
            {
                return fail("Failed to open artifact file for writing");
            }
            outFile << json;
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("Failed to persist GRH stage1 JSON: ") + ex.what());
        }
#endif

        Design parsed = Design::fromJsonString(json);
        if (parsed.topGraphs().size() != 1 || parsed.topGraphs()[0] != "demo")
        {
            return fail("Top graph round-trip failed");
        }

        const Graph *parsedGraph = parsed.findGraph("demo");
        if (!parsedGraph)
        {
            return fail("Parsed graph missing");
        }
        SymbolId parsedSymA = parsedGraph->lookupSymbol("a");
        if (!parsedSymA.valid() || !parsedGraph->isDeclaredSymbol(parsedSymA))
        {
            return fail("Declared symbol not preserved in graph");
        }
        SymbolId parsedModuleSym = parsed.lookupSymbol("demo");
        if (!parsedModuleSym.valid() || !parsed.isDeclaredSymbol(parsedModuleSym))
        {
            return fail("Declared symbol not preserved in design");
        }

        if (parsedGraph->generateGroups().size() != 2)
        {
            return fail("Generate group not preserved in graph");
        }
        const auto &parsedGroup = parsedGraph->generateGroups().front();
        if (parsedGraph->symbolText(parsedGroup.scope) != "gen_loop" ||
            parsedGraph->symbolText(parsedGroup.name) != "sig" ||
            parsedGroup.symbols.size() != 2 ||
            parsedGraph->symbolText(parsedGroup.symbols[0]) != "gen_loop$0$sig" ||
            parsedGraph->symbolText(parsedGroup.symbols[1]) != "gen_loop$1$sig")
        {
            return fail("Generate group content mismatch after round-trip");
        }
        const auto &parsedHierGroup = parsedGraph->generateGroups().back();
        if (parsedGraph->symbolText(parsedHierGroup.scope) != "u_child$gen_loop" ||
            parsedGraph->symbolText(parsedHierGroup.name) != "sig" ||
            parsedHierGroup.symbols.size() != 2 ||
            parsedGraph->symbolText(parsedHierGroup.symbols[0]) != "u_child$gen_loop$0$sig" ||
            parsedGraph->symbolText(parsedHierGroup.symbols[1]) != "u_child$gen_loop$1$sig")
        {
            return fail("Hierarchical generate group content mismatch after round-trip");
        }
        const Graph *parsedEraseGraph = parsed.findGraph("erase_case");
        if (!parsedEraseGraph || parsedEraseGraph->generateGroups().size() != 1 ||
            parsedEraseGraph->generateGroups().front().symbols.size() != 1 ||
            parsedEraseGraph->symbolText(parsedEraseGraph->generateGroups().front().symbols[0]) !=
                "gen$2$w")
        {
            return fail("Generate group erase cleanup not preserved after round-trip");
        }
        if (json.find("\"generateGroups\"") == std::string::npos)
        {
            return fail("Emitted JSON missing generateGroups field");
        }

        // Compact mode writes through Graph::writeJson; groups must survive too.
        emitDiagnostics.clear();
        StoreOptions compactOptions;
        compactOptions.jsonMode = JsonPrintMode::Compact;
        auto compactJsonOpt = emitter.storeToString(design, compactOptions);
        if (!compactJsonOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to emit compact JSON for design");
        }
        if (compactJsonOpt->find("\"generateGroups\"") == std::string::npos)
        {
            return fail("Compact JSON missing generateGroups field");
        }
        Design compactParsed = Design::fromJsonString(*compactJsonOpt);
        const Graph *compactGraph = compactParsed.findGraph("demo");
        if (!compactGraph || compactGraph->generateGroups().size() != 2 ||
            compactGraph->generateGroups().front().symbols.size() != 2 ||
            compactGraph->generateGroups().back().symbols.size() != 2)
        {
            return fail("Generate group not preserved in compact round-trip");
        }

        // JSON written before the field existed omits it; it must still load.
        const std::string legacyJson = R"({
  "graphs": [
    {
      "symbol": "legacy",
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
  "declaredSymbols": [],
  "tops": ["legacy"]
})";
        Design legacy = Design::fromJsonString(legacyJson);
        const Graph *legacyGraph = legacy.findGraph("legacy");
        if (!legacyGraph)
        {
            return fail("Legacy JSON without generateGroups failed to load");
        }
        if (!legacyGraph->generateGroups().empty())
        {
            return fail("Legacy JSON produced unexpected generate groups");
        }

        const OperationId parsedOpId = parsedGraph->findOperation("add0");
        if (!parsedOpId.valid())
        {
            return fail("Parsed operation missing");
        }
        const Operation parsedOp = parsedGraph->getOperation(parsedOpId);
        if (parsedOp.operands().size() != 2 || parsedOp.results().size() != 1)
        {
            return fail("Parsed operation connectivity mismatch");
        }

        const OperationId parsedAssignId = parsedGraph->findOperation("assign0");
        if (!parsedAssignId.valid())
        {
            return fail("Parsed assign operation missing");
        }
        const Operation parsedAssign = parsedGraph->getOperation(parsedAssignId);
        if (parsedAssign.kind() != OperationKind::kAssign)
        {
            return fail("Assign operation kind mismatch");
        }
        if (parsedAssign.operands().size() != 1 || parsedAssign.results().size() != 1)
        {
            return fail("Assign operation connectivity mismatch");
        }

        const OperationId parsedPackedConcatId = parsedGraph->findOperation("packed_concat");
        if (!parsedPackedConcatId.valid())
        {
            return fail("Parsed packed concat operation missing");
        }
        const Operation parsedPackedConcat = parsedGraph->getOperation(parsedPackedConcatId);
        auto parsedElementWidth = parsedPackedConcat.attr("svPackedArray.elementWidth");
        auto parsedElementCount = parsedPackedConcat.attr("svPackedArray.elementCount");
        auto parsedLaneOrder = parsedPackedConcat.attr("svPackedArray.laneOrder");
        if (!parsedElementWidth || !parsedElementCount || !parsedLaneOrder ||
            *parsedElementWidth != AttributeValue(int64_t(3)) ||
            *parsedElementCount != AttributeValue(int64_t(2)) ||
            *parsedLaneOrder != AttributeValue(std::string("lsb_index_low")))
        {
            return fail("Packed array producer attrs did not round-trip");
        }
        const OperationId parsedPackedSliceId = parsedGraph->findOperation("packed_slice");
        if (!parsedPackedSliceId.valid())
        {
            return fail("Parsed packed slice operation missing");
        }
        const Operation parsedPackedSlice = parsedGraph->getOperation(parsedPackedSliceId);
        auto parsedSliceWidth = parsedPackedSlice.attr("sliceWidth");
        if (parsedPackedSlice.kind() != OperationKind::kSliceArray ||
            parsedPackedSlice.operands().size() != 2 ||
            parsedPackedSlice.results().size() != 1 ||
            !parsedSliceWidth ||
            *parsedSliceWidth != AttributeValue(int64_t(3)))
        {
            return fail("Packed kSliceArray did not round-trip");
        }

        auto parsedAssignKind = parseOperationKind("kAssign");
        if (!parsedAssignKind || *parsedAssignKind != OperationKind::kAssign)
        {
            return fail("Failed to parse kAssign operation kind");
        }

        emitDiagnostics.clear();
        auto jsonAgainOpt = emitter.storeToString(parsed, emitOptions);
        if (!jsonAgainOpt || emitDiagnostics.hasError())
        {
            return fail("Failed to re-emit JSON after parse");
        }
        std::string jsonAgain = *jsonAgainOpt;
        if (json != jsonAgain)
        {
            return fail("JSON serialization not stable");
        }

        const std::string nestedArrayJson = R"({
  "graphs": [
    {
      "symbol": "illegal_nested_array",
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": [
        {
          "sym": "bad",
          "kind": "kAdd",
          "in": [],
          "out": [],
          "attrs": {
            "illegal": {
              "k": "int[]",
              "vs": [
                1,
                [2, 3]
              ]
            }
          }
        }
      ]
    }
  ],
  "declaredSymbols": [],
  "tops": []
})";
        bool threwOnNestedArrayAttribute = expectThrows([&]
                                                        { Design::fromJsonString(nestedArrayJson); });
        if (!threwOnNestedArrayAttribute)
        {
            return fail("Expected nested array attribute to throw during parse");
        }

        const std::string objectAttributeJson = R"({
  "graphs": [
    {
      "symbol": "illegal_object_attr",
      "declaredSymbols": [],
      "vals": [],
      "ports": {
        "in": [],
        "out": [],
        "inout": []
      },
      "ops": [
        {
          "sym": "bad_obj",
          "kind": "kAdd",
          "in": [],
          "out": [],
          "attrs": {
            "illegal": {
              "k": "string",
              "v": {
                "unexpected": "object"
              }
            }
          }
        }
      ]
    }
  ],
  "declaredSymbols": [],
  "tops": []
})";
        bool threwOnObjectAttribute = expectThrows([&]
                                                   { Design::fromJsonString(objectAttributeJson); });
        if (!threwOnObjectAttribute)
        {
            return fail("Expected object attribute to throw during parse");
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::DesignSymbolTable designSymbols;
            auto demoSym = designSymbols.intern("demo");
            if (!designSymbols.valid(demoSym))
            {
                return fail("DesignSymbolTable did not mark interned symbol as valid");
            }
            if (!designSymbols.contains("demo"))
            {
                return fail("DesignSymbolTable contains() failed for interned symbol");
            }
            auto demoLookup = designSymbols.lookup("demo");
            if (!demoLookup.valid() || demoLookup != demoSym)
            {
                return fail("DesignSymbolTable lookup failed for interned symbol");
            }
            if (designSymbols.text(demoSym) != "demo")
            {
                return fail("DesignSymbolTable text() mismatch");
            }
            if (designSymbols.lookup("missing").valid())
            {
                return fail("DesignSymbolTable lookup should miss unknown symbol");
            }
            auto dupSym = designSymbols.intern("demo");
            if (!dupSym.valid() || dupSym != demoSym)
            {
                return fail("Expected duplicate intern to return existing SymbolId");
            }
            if (designSymbols.valid(grh_ir::SymbolId::invalid()))
            {
                return fail("Invalid SymbolId reported as valid");
            }
            bool threwOnInvalidText = expectThrows([&]
                                                   { designSymbols.text(grh_ir::SymbolId::invalid()); });
            if (!threwOnInvalidText)
            {
                return fail("Expected invalid SymbolId text() to throw");
            }

            grh_ir::GraphSymbolTable graphSymbols;
            auto valueSym = graphSymbols.intern("value0");
            if (!graphSymbols.contains("value0") || graphSymbols.text(valueSym) != "value0")
            {
                return fail("GraphSymbolTable did not roundtrip symbol");
            }

            grh_ir::GraphId graphA{1, 0};
            grh_ir::GraphId graphB{2, 0};
            if (!graphA.valid() || grh_ir::GraphId::invalid().valid())
            {
                return fail("GraphId valid/invalid check failed");
            }

            grh_ir::ValueId valueId{1, 0, graphA};
            if (!valueId.valid())
            {
                return fail("ValueId valid check failed");
            }
            bool threwOnCrossGraphValue = expectThrows([&]
                                                       { valueId.assertGraph(graphB); });
            if (!threwOnCrossGraphValue)
            {
                return fail("Expected ValueId cross-graph assertion to throw");
            }
            bool threwOnInvalidValue = expectThrows([&]
                                                    { grh_ir::ValueId::invalid().assertGraph(graphA); });
            if (!threwOnInvalidValue)
            {
                return fail("Expected invalid ValueId to throw on assertGraph");
            }

            grh_ir::OperationId opId{1, 0, graphA};
            bool threwOnCrossGraphOp = expectThrows([&]
                                                    { opId.assertGraph(graphB); });
            if (!threwOnCrossGraphOp)
            {
                return fail("Expected OperationId cross-graph assertion to throw");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::GraphSymbolTable graphSymbols;
            grh_ir::GraphBuilder builder(graphSymbols);

            auto symA = graphSymbols.intern("a");
            auto symB = graphSymbols.intern("b");
            auto symSum = graphSymbols.intern("sum");
            auto symOut = graphSymbols.intern("out");
            auto symAdd = graphSymbols.intern("add0");
            auto symAssign = graphSymbols.intern("assign0");

            auto vA = builder.addValue(symA, 1, false);
            auto vB = builder.addValue(symB, 1, false);
            auto vSum = builder.addValue(symSum, 1, false);
            auto vOut = builder.addValue(symOut, 1, false);

            auto opAdd = builder.addOp(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.addOp(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            grh_ir::GraphView view = builder.freeze();

            auto values = view.values();
            if (values.size() != 4 || values[0] != vA || values[1] != vB || values[2] != vSum || values[3] != vOut)
            {
                return fail("GraphView value order mismatch");
            }

            auto ops = view.operations();
            if (ops.size() != 2 || ops[0] != opAdd || ops[1] != opAssign)
            {
                return fail("GraphView operation order mismatch");
            }

            auto addOperands = view.opOperands(opAdd);
            if (addOperands.size() != 2 || addOperands[0] != vA || addOperands[1] != vB)
            {
                return fail("GraphView operand range for add op mismatch");
            }
            auto addResults = view.opResults(opAdd);
            if (addResults.size() != 1 || addResults[0] != vSum)
            {
                return fail("GraphView result range for add op mismatch");
            }

            auto assignOperands = view.opOperands(opAssign);
            if (assignOperands.size() != 1 || assignOperands[0] != vSum)
            {
                return fail("GraphView operand range for assign op mismatch");
            }
            auto assignResults = view.opResults(opAssign);
            if (assignResults.size() != 1 || assignResults[0] != vOut)
            {
                return fail("GraphView result range for assign op mismatch");
            }

            if (view.valueDef(vSum) != opAdd)
            {
                return fail("GraphView valueDef mismatch for sum");
            }
            if (view.valueDef(vA).valid() || view.valueDef(vB).valid())
            {
                return fail("GraphView valueDef should be invalid for non-results");
            }
            if (view.valueDef(vOut) != opAssign)
            {
                return fail("GraphView valueDef mismatch for output value");
            }
            if (view.valueWidth(vSum) != 1 || view.valueWidth(vOut) != 1)
            {
                return fail("GraphView valueWidth mismatch");
            }

            auto usersA = view.valueUsers(vA);
            if (usersA.size() != 1 || usersA[0].operation != opAdd || usersA[0].operandIndex != 0)
            {
                return fail("GraphView useList mismatch for value a");
            }
            auto usersB = view.valueUsers(vB);
            if (usersB.size() != 1 || usersB[0].operation != opAdd || usersB[0].operandIndex != 1)
            {
                return fail("GraphView useList mismatch for value b");
            }
            auto usersSum = view.valueUsers(vSum);
            if (usersSum.size() != 1 || usersSum[0].operation != opAssign || usersSum[0].operandIndex != 0)
            {
                return fail("GraphView useList mismatch for value sum");
            }
            if (!view.valueUsers(vOut).empty())
            {
                return fail("GraphView useList should be empty for output value");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::GraphSymbolTable graphSymbols;
            grh_ir::GraphBuilder builder(graphSymbols);

            auto symPortA = std::string("a");
            auto symPortOut = std::string("out");
            auto symVa = graphSymbols.intern("va");
            auto symVb = graphSymbols.intern("vb");
            auto symSum = graphSymbols.intern("sum");
            auto symOutVal = graphSymbols.intern("out_val");
            auto symOutTmp = graphSymbols.intern("_val_test_out_tmp");
            auto symAdd = graphSymbols.intern("add0");
            auto symAssign = graphSymbols.intern("assign0");
            auto symAssignTmp = graphSymbols.intern("_op_test_assign_tmp");
            auto vA = builder.addValue(symVa, 1, false);
            auto vB = builder.addValue(symVb, 1, false);
            auto vSum = builder.addValue(symSum, 1, false);
            auto vOut = builder.addValue(symOutTmp, 1, false);

            auto opAdd = builder.addOp(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.addOp(OperationKind::kAssign, symAssignTmp);
            builder.setOpSymbol(opAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            builder.setValueSymbol(vOut, symOutVal);

            builder.bindInputPort("b", vB);
            builder.bindInputPort(symPortA, vA);
            builder.bindOutputPort(symPortOut, vOut);

            builder.setAttr(opAdd, "delay", AttributeValue(int64_t(5)));

            SrcLoc opLoc;
            opLoc.file = "demo.sv";
            opLoc.line = 10;
            builder.setOpSrcLoc(opAdd, opLoc);

            SrcLoc valueLoc;
            valueLoc.file = "demo.sv";
            valueLoc.line = 12;
            builder.setValueSrcLoc(vA, valueLoc);

            grh_ir::GraphView view = builder.freeze();

            auto inPorts = view.inputPorts();
            if (inPorts.size() != 2 || inPorts[0].name != "a" || inPorts[1].name != "b")
            {
                return fail("GraphView input port ordering mismatch");
            }
            auto outPorts = view.outputPorts();
            if (outPorts.size() != 1 || outPorts[0].name != "out")
            {
                return fail("GraphView output port ordering mismatch");
            }

            if (view.opSymbol(opAdd) != symAdd || view.opSymbol(opAssign) != symAssign)
            {
                return fail("GraphView opSymbol mismatch");
            }
            if (view.valueSymbol(vOut) != symOutVal)
            {
                return fail("GraphView valueSymbol mismatch");
            }

            auto attr = view.opAttr(opAdd, "delay");
            if (!attr || std::get<int64_t>(*attr) != 5)
            {
                return fail("GraphView opAttr lookup mismatch");
            }
            if (view.opAttrs(opAdd).size() != 1)
            {
                return fail("GraphView opAttrs size mismatch");
            }

            auto opSrcLoc = view.opSrcLoc(opAdd);
            if (!opSrcLoc || opSrcLoc->line != 10)
            {
                return fail("GraphView opSrcLoc mismatch");
            }
            auto valSrcLoc = view.valueSrcLoc(vA);
            if (!valSrcLoc || valSrcLoc->line != 12)
            {
                return fail("GraphView valueSrcLoc mismatch");
            }

            if (!view.valueIsInput(vA) || view.valueIsInput(vSum) || !view.valueIsOutput(vOut))
            {
                return fail("GraphView port flags mismatch");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::GraphSymbolTable graphSymbols;
            grh_ir::GraphBuilder builder(graphSymbols);

            auto symX = graphSymbols.intern("x");
            auto symY = graphSymbols.intern("y");
            auto symTmp = graphSymbols.intern("tmp");
            auto symOut = graphSymbols.intern("out");
            auto symAlt = graphSymbols.intern("alt");
            auto symAdd = graphSymbols.intern("add0");
            auto symAssign = graphSymbols.intern("assign0");

            auto vX = builder.addValue(symX, 1, false);
            auto vY = builder.addValue(symY, 1, false);
            auto vTmp = builder.addValue(symTmp, 1, false);
            auto vOut = builder.addValue(symOut, 1, false);
            auto vAlt = builder.addValue(symAlt, 1, false);

            auto opAdd = builder.addOp(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vX);
            builder.addOperand(opAdd, vY);
            builder.addResult(opAdd, vTmp);

            auto opAssign = builder.addOp(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vTmp);
            builder.addResult(opAssign, vOut);

            builder.replaceOperand(opAdd, 1, vX);
            builder.replaceAllUses(vTmp, vX);
            builder.replaceResult(opAssign, 0, vAlt);

            grh_ir::GraphView view = builder.freeze();
            auto addOperands = view.opOperands(opAdd);
            if (addOperands.size() != 2 || addOperands[0] != vX || addOperands[1] != vX)
            {
                return fail("GraphBuilder replaceOperand mismatch");
            }
            auto assignOperands = view.opOperands(opAssign);
            if (assignOperands.size() != 1 || assignOperands[0] != vX)
            {
                return fail("GraphBuilder replaceAllUses mismatch");
            }
            auto assignResults = view.opResults(opAssign);
            if (assignResults.size() != 1 || assignResults[0] != vAlt)
            {
                return fail("GraphBuilder replaceResult mismatch");
            }
            if (view.valueDef(vOut).valid())
            {
                return fail("GraphBuilder replaceResult did not clear old definition");
            }
        }

        {
            namespace grh_ir = wolvrix::lib::grh;

            grh_ir::GraphSymbolTable graphSymbols;
            grh_ir::GraphBuilder builder(graphSymbols);

            auto symA = graphSymbols.intern("a");
            auto symB = graphSymbols.intern("b");
            auto symSum = graphSymbols.intern("sum");
            auto symOut = graphSymbols.intern("out");
            auto symDead = graphSymbols.intern("dead");
            auto symAdd = graphSymbols.intern("add0");
            auto symAssign = graphSymbols.intern("assign0");
            auto symDeadOp = graphSymbols.intern("dead_op");

            auto vA = builder.addValue(symA, 1, false);
            auto vB = builder.addValue(symB, 1, false);
            auto vSum = builder.addValue(symSum, 1, false);
            auto vOut = builder.addValue(symOut, 1, false);
            auto vDead = builder.addValue(symDead, 1, false);

            auto opAdd = builder.addOp(OperationKind::kAdd, symAdd);
            builder.addOperand(opAdd, vA);
            builder.addOperand(opAdd, vB);
            builder.addResult(opAdd, vSum);

            auto opAssign = builder.addOp(OperationKind::kAssign, symAssign);
            builder.addOperand(opAssign, vSum);
            builder.addResult(opAssign, vOut);

            auto opDead = builder.addOp(OperationKind::kAssign, symDeadOp);
            builder.addOperand(opDead, vA);
            builder.addResult(opDead, vDead);

            if (!builder.eraseOperand(opAdd, 1))
            {
                return fail("GraphBuilder eraseOperand failed");
            }

            if (!builder.eraseResult(opDead, 0))
            {
                return fail("GraphBuilder eraseResult failed");
            }
            if (!builder.eraseValue(vDead))
            {
                return fail("GraphBuilder eraseValue failed");
            }

            std::array<grh_ir::ValueId, 1> replacements = {vA};
            if (!builder.eraseOp(opAdd, replacements))
            {
                return fail("GraphBuilder eraseOp with replacement failed");
            }

            grh_ir::GraphView view = builder.freeze();
            auto assignOperands = view.opOperands(opAssign);
            if (assignOperands.size() != 1 || assignOperands[0] != vA)
            {
                return fail("GraphBuilder eraseOp replacement did not update uses");
            }
        }

        {
            Graph &symbolGraph = design.createGraph("symbol_checks");
            SymbolId sym = symbolGraph.internSymbol("dup_symbol");
            ValueId val = symbolGraph.createValue(sym, 1, false);
            (void)val;

            if (symbolGraph.internSymbol("dup_symbol").valid())
            {
                return fail("internSymbol should return invalid for bound symbol");
            }
            if (!symbolGraph.lookupSymbol("dup_symbol").valid())
            {
                return fail("lookupSymbol should resolve existing symbol");
            }

            bool threwInvalid = expectThrows([&]
                                             { symbolGraph.createValue(SymbolId::invalid(), 1, false); });
            if (!threwInvalid)
            {
                return fail("Expected createValue to throw on invalid SymbolId");
            }

            bool threwConflict = expectThrows([&]
                                              { symbolGraph.createOperation(OperationKind::kAdd, sym); });
            if (!threwConflict)
            {
                return fail("Expected createOperation to throw on bound SymbolId");
            }

            ValueId portValue = symbolGraph.createValue(symbolGraph.internSymbol("port_value"), 1, false);
            symbolGraph.bindInputPort("port_only", portValue);
            if (symbolGraph.lookupSymbol("port_only").valid())
            {
                return fail("Port name should not be interned into symbol table");
            }
            if (symbolGraph.findValue("port_only").valid())
            {
                return fail("Port name should not resolve as a value symbol");
            }
        }

    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
