#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/repcut.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[repcut-tests] " << message << '\n';
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

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width = 8,
                                         bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(name), width, isSigned);
    }

    wolvrix::lib::grh::OperationId makeBinaryOp(wolvrix::lib::grh::Graph &graph,
                                                 wolvrix::lib::grh::OperationKind kind,
                                                 const std::string &name,
                                                 wolvrix::lib::grh::ValueId lhs,
                                                 wolvrix::lib::grh::ValueId rhs,
                                                 wolvrix::lib::grh::ValueId out)
    {
        const auto op = graph.createOperation(kind, graph.internSymbol(name));
        graph.addOperand(op, lhs);
        graph.addOperand(op, rhs);
        graph.addResult(op, out);
        return op;
    }

    void populateBasicRepcutDesign(wolvrix::lib::grh::Design &design, const std::string &topName)
    {
        wolvrix::lib::grh::Graph &graph = design.createGraph(topName);

        const auto inA = makeValue(graph, "a");
        const auto inB = makeValue(graph, "b");
        const auto en = makeValue(graph, "en", 1, false);
        graph.bindInputPort("a", inA);
        graph.bindInputPort("b", inB);
        graph.bindInputPort("en", en);

        const auto shared = makeValue(graph, "shared");
        makeBinaryOp(graph, wolvrix::lib::grh::OperationKind::kAdd, "shared_add", inA, inB, shared);

        constexpr int kSinkCount = 6;
        for (int i = 0; i < kSinkCount; ++i)
        {
            const std::string suffix = std::to_string(i);
            const auto datav = makeValue(graph, "wr_data_" + suffix);
            const auto kind = (i % 2 == 0) ? wolvrix::lib::grh::OperationKind::kAdd
                                           : wolvrix::lib::grh::OperationKind::kSub;
            const std::string dataOpName = (i % 2 == 0 ? "sink_add_" : "sink_sub_") + suffix;
            makeBinaryOp(graph, kind, dataOpName, shared, (i % 2 == 0) ? inA : inB, datav);

            const std::string latchName = "lat_" + suffix;
            const auto latchDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                                         graph.internSymbol(latchName));
            graph.setAttr(latchDecl, "width", static_cast<int64_t>(8));
            graph.setAttr(latchDecl, "isSigned", false);

            const auto writeOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                                       graph.internSymbol(latchName + "_wr"));
            graph.addOperand(writeOp, en);
            graph.addOperand(writeOp, datav);
            graph.setAttr(writeOp, "latchSymbol", latchName);
        }

        design.markAsTop(topName);
    }

    std::filesystem::path findNativePartitionFile(const std::filesystem::path &outDir, std::string_view prefix)
    {
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(outDir, ec))
        {
            if (ec || !entry.is_regular_file())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0)
            {
                return entry.path();
            }
        }
        return {};
    }

    const std::string *findRepcutStatsDiagnostic(const PassDiagnostics &diags)
    {
        for (const auto &diag : diags.messages())
        {
            if (diag.passName == "repcut" &&
                diag.kind == PassDiagnosticKind::Info &&
                diag.message.find("\"pass\":\"repcut\"") != std::string::npos)
            {
                return &diag.message;
            }
        }
        return nullptr;
    }

    bool diagnosticsContain(const PassDiagnostics &diags, std::string_view pattern)
    {
        for (const auto &diag : diags.messages())
        {
            if (diag.message.find(pattern) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                             std::string_view key)
    {
        auto attr = op.attr(key);
        if (!attr)
        {
            return std::nullopt;
        }
        if (const auto *value = std::get_if<std::string>(&*attr))
        {
            return *value;
        }
        return std::nullopt;
    }

    wolvrix::lib::grh::OperationId findInstanceByName(const wolvrix::lib::grh::Graph &graph,
                                                      std::string_view instanceName)
    {
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const auto op = graph.getOperation(opId);
            if (op.kind() != wolvrix::lib::grh::OperationKind::kInstance)
            {
                continue;
            }
            const auto name = getAttrString(op, "instanceName");
            if (name && *name == instanceName)
            {
                return opId;
            }
        }
        return wolvrix::lib::grh::OperationId::invalid();
    }

    bool hasGraphWithPrefix(wolvrix::lib::grh::Design &design, std::string_view prefix)
    {
        if (design.findGraph(std::string(prefix)) != nullptr)
        {
            return true;
        }
        for (int suffix = 1; suffix <= 8; ++suffix)
        {
            if (design.findGraph(std::string(prefix) + "_" + std::to_string(suffix)) != nullptr)
            {
                return true;
            }
        }
        return false;
    }

    bool graphHasOpKind(const wolvrix::lib::grh::Graph &graph, wolvrix::lib::grh::OperationKind kind)
    {
        for (const auto opId : graph.operations())
        {
            if (graph.opKind(opId) == kind)
            {
                return true;
            }
        }
        return false;
    }

    bool graphHasOpSymbol(const wolvrix::lib::grh::Graph &graph, std::string_view symbol)
    {
        for (const auto opId : graph.operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            const auto op = graph.getOperation(opId);
            if (op.symbolText() == symbol)
            {
                return true;
            }
        }
        return false;
    }

    wolvrix::lib::grh::Graph *findGraphWithPrefixAndOpKind(wolvrix::lib::grh::Design &design,
                                                           std::string_view prefix,
                                                           wolvrix::lib::grh::OperationKind kind)
    {
        for (const auto &graphName : design.graphOrder())
        {
            if (graphName.rfind(prefix, 0) != 0)
            {
                continue;
            }
            auto *graph = design.findGraph(graphName);
            if (graph != nullptr && graphHasOpKind(*graph, kind))
            {
                return graph;
            }
        }
        return nullptr;
    }

    wolvrix::lib::grh::Graph *findGraphWithPrefixAndOpSymbol(wolvrix::lib::grh::Design &design,
                                                             std::string_view prefix,
                                                             std::string_view symbol)
    {
        for (const auto &graphName : design.graphOrder())
        {
            if (graphName.rfind(prefix, 0) != 0)
            {
                continue;
            }
            auto *graph = design.findGraph(graphName);
            if (graph != nullptr && graphHasOpSymbol(*graph, symbol))
            {
                return graph;
            }
        }
        return nullptr;
    }

    std::size_t countOpSymbolInGraphsWithPrefix(wolvrix::lib::grh::Design &design,
                                                 std::string_view prefix,
                                                 std::string_view symbol)
    {
        std::size_t count = 0;
        for (const auto &graphName : design.graphOrder())
        {
            if (graphName.rfind(prefix, 0) != 0)
            {
                continue;
            }
            auto *graph = design.findGraph(graphName);
            if (graph == nullptr)
            {
                continue;
            }
            for (const auto opId : graph->operations())
            {
                if (opId.valid() && graph->getOperation(opId).symbolText() == symbol)
                {
                    ++count;
                }
            }
        }
        return count;
    }

} // namespace

int main()
{
#if !WOLVRIX_HAVE_MT_KAHYPAR
    std::cout << "[repcut-tests] mt-kahypar backend unavailable, skip test.\n";
    return 0;
#else
    wolvrix::lib::grh::Design design;
    populateBasicRepcutDesign(design, "top");

    const std::filesystem::path outDir = std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "repcut_test";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec)
    {
        return fail("failed to create output directory: " + outDir.string());
    }

    PassManager manager;
    manager.options().verbosity = PassVerbosity::Info;
    RepcutOptions options;
    options.path = "top";
    options.partitionCount = 2;
    options.imbalanceFactor = 1.0;
    options.workDir = outDir.string();
    options.partitioner = "mt-kahypar";
    options.mtKaHyParPreset = "quality";
    options.mtKaHyParThreads = 0;
    options.keepIntermediateFiles = true;
    manager.addPass(std::make_unique<RepcutPass>(options));

    wolvrix::lib::grh::Design deterministicDesign1;
    wolvrix::lib::grh::Design deterministicDesign2;
    populateBasicRepcutDesign(deterministicDesign1, "top_repeat");
    populateBasicRepcutDesign(deterministicDesign2, "top_repeat");

    const std::filesystem::path deterministicOutDir1 = outDir / "deterministic_run1";
    const std::filesystem::path deterministicOutDir2 = outDir / "deterministic_run2";
    std::filesystem::create_directories(deterministicOutDir1, ec);
    std::filesystem::create_directories(deterministicOutDir2, ec);
    if (ec)
    {
        return fail("failed to create deterministic output directories under: " + outDir.string());
    }

    RepcutOptions deterministicOptions;
    deterministicOptions.path = "top_repeat";
    deterministicOptions.partitionCount = 2;
    deterministicOptions.imbalanceFactor = 1.0;
    deterministicOptions.partitioner = "mt-kahypar";
    deterministicOptions.mtKaHyParPreset = "quality";
    deterministicOptions.mtKaHyParThreads = 0;
    deterministicOptions.keepIntermediateFiles = true;

    auto runDeterministicCapture = [&](wolvrix::lib::grh::Design &currentDesign,
                                       const std::filesystem::path &currentOutDir,
                                       std::vector<std::string> &logs) -> int {
        PassManager currentManager;
        currentManager.options().verbosity = PassVerbosity::Info;
        currentManager.options().logLevel = wolvrix::lib::LogLevel::Info;
        currentManager.options().logSink = [&](wolvrix::lib::LogLevel, std::string_view, std::string_view message) {
            logs.emplace_back(message);
        };

        RepcutOptions currentOptions = deterministicOptions;
        currentOptions.workDir = currentOutDir.string();
        currentManager.addPass(std::make_unique<RepcutPass>(currentOptions));

        PassDiagnostics currentDiags;
        PassManagerResult currentResult{};
        try
        {
            currentResult = currentManager.run(currentDesign, currentDiags);
        }
        catch (const std::exception &ex)
        {
            return fail(std::string("deterministic repcut exception: ") + ex.what());
        }
        if (!currentResult.success && !currentDiags.hasError())
        {
            return fail("deterministic repcut reported failure without diagnostics");
        }
        return 0;
    };

    std::vector<std::string> deterministicLogs1;
    std::vector<std::string> deterministicLogs2;
    if (const int rc = runDeterministicCapture(deterministicDesign1, deterministicOutDir1, deterministicLogs1); rc != 0)
    {
        return rc;
    }
    if (const int rc = runDeterministicCapture(deterministicDesign2, deterministicOutDir2, deterministicLogs2); rc != 0)
    {
        return rc;
    }

    bool sawDeterministicOverride = false;
    for (const auto &message : deterministicLogs1)
    {
        if (message.find("effective_preset_token=deterministic-quality") != std::string::npos &&
            message.find("overridden=true") != std::string::npos)
        {
            sawDeterministicOverride = true;
            break;
        }
    }
    if (!sawDeterministicOverride)
    {
        return fail("expected quality preset to be resolved to deterministic-quality");
    }

    const std::filesystem::path deterministicPart1 =
        findNativePartitionFile(deterministicOutDir1, "top_repeat_repcut_k2.hgr.part");
    const std::filesystem::path deterministicPart2 =
        findNativePartitionFile(deterministicOutDir2, "top_repeat_repcut_k2.hgr.part");
    if (deterministicPart1.empty() || deterministicPart2.empty())
    {
        return fail("expected deterministic repcut runs to emit native partition files");
    }
    if (readFile(deterministicPart1) != readFile(deterministicPart2))
    {
        return fail("expected repeated repcut runs to emit identical mt-kahypar partition files");
    }

    PassDiagnostics diags;
    PassManagerResult result{};
    try
    {
        result = manager.run(design, diags);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("exception during repcut run: ") + ex.what());
    }

    const std::filesystem::path nativePartFile = findNativePartitionFile(outDir, "top_repcut_k2.hgr.part");
    if (nativePartFile.empty())
    {
        return fail("expected native mt-kahypar partition output file");
    }
    if (!result.success || diags.hasError())
    {
        bool hasExpectedDiagnostic = false;
        for (const auto &diag : diags.messages())
        {
            if (diag.message.find("repcut phase-e: detected forbidden cross-partition values") != std::string::npos ||
                diag.message.find("repcut phase-e: detected memory-read cross-partition usage") != std::string::npos)
            {
                hasExpectedDiagnostic = true;
                break;
            }
        }
        if (!hasExpectedDiagnostic)
        {
            return fail("repcut failed without explicit phase-e cross-partition diagnostic");
        }
        return 0;
    }

    if (!result.changed)
    {
        return fail("expected repcut pass to report graph changes");
    }

    const std::string *stats = findRepcutStatsDiagnostic(diags);
    if (stats == nullptr ||
        stats->find("\"partition_static_features\":[") == std::string::npos ||
        stats->find("\"part_name\":\"part_0\"") == std::string::npos ||
        stats->find("\"op_kind_counts\":{") == std::string::npos ||
        stats->find("\"width_bucket_counts\":{") == std::string::npos)
    {
        return fail("repcut stats missing canonical partition static feature fields");
    }

    wolvrix::lib::grh::Graph *newTop = design.findGraph("top");
    if (!newTop)
    {
        return fail("top graph missing after repcut");
    }

    std::size_t instanceCount = 0;
    for (const auto opId : newTop->operations())
    {
        if (newTop->getOperation(opId).kind() == wolvrix::lib::grh::OperationKind::kInstance)
        {
            ++instanceCount;
        }
    }
    if (instanceCount < 1)
    {
        return fail("expected at least one partition instance in reconstructed top");
    }

    if (!hasGraphWithPrefix(design, "top_repcut_part0"))
    {
        return fail("expected partition graph with top_repcut_part0 prefix");
    }

    // Regression: read-only register (initValue-only, no write port) must still
    // get a stable partition owner so reconstructed inputs can be fully mapped.
    wolvrix::lib::grh::Design readOnlyDesign;
    wolvrix::lib::grh::Graph &readOnlyGraph = readOnlyDesign.createGraph("top_read_only_reg");
    readOnlyDesign.markAsTop("top_read_only_reg");

    const auto roIn = makeValue(readOnlyGraph, "in_data", 32, false);
    const auto roEn = makeValue(readOnlyGraph, "en", 1, false);
    readOnlyGraph.bindInputPort("in_data", roIn);
    readOnlyGraph.bindInputPort("en", roEn);

    const auto roRegDecl =
        readOnlyGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                      readOnlyGraph.internSymbol("random_bits"));
    readOnlyGraph.setAttr(roRegDecl, "width", static_cast<int64_t>(32));
    readOnlyGraph.setAttr(roRegDecl, "isSigned", false);
    readOnlyGraph.setAttr(roRegDecl, "initValue", std::string("$random"));

    const auto roRead =
        readOnlyGraph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                      readOnlyGraph.internSymbol("random_bits_rd"));
    const auto roReadValue = makeValue(readOnlyGraph, "random_bits_rd_v", 32, false);
    readOnlyGraph.addResult(roRead, roReadValue);
    readOnlyGraph.setAttr(roRead, "regSymbol", std::string("random_bits"));

    const auto roData = makeValue(readOnlyGraph, "lat_data", 32, false);
    makeBinaryOp(readOnlyGraph, wolvrix::lib::grh::OperationKind::kAdd,
                 "lat_data_add", roReadValue, roIn, roData);

    const auto roLatchDecl =
        readOnlyGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                      readOnlyGraph.internSymbol("ro_lat"));
    readOnlyGraph.setAttr(roLatchDecl, "width", static_cast<int64_t>(32));
    readOnlyGraph.setAttr(roLatchDecl, "isSigned", false);

    const auto roLatchWrite =
        readOnlyGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                      readOnlyGraph.internSymbol("ro_lat_wr"));
    readOnlyGraph.addOperand(roLatchWrite, roEn);
    readOnlyGraph.addOperand(roLatchWrite, roData);
    readOnlyGraph.setAttr(roLatchWrite, "latchSymbol", std::string("ro_lat"));

    const std::filesystem::path roOutDir =
        std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "repcut_test_read_only_reg";
    std::filesystem::create_directories(roOutDir, ec);
    if (ec)
    {
        return fail("failed to create output directory: " + roOutDir.string());
    }

    PassManager roManager;
    roManager.options().verbosity = PassVerbosity::Info;
    RepcutOptions roOptions;
    roOptions.path = "top_read_only_reg";
    roOptions.partitionCount = 2;
    roOptions.imbalanceFactor = 1.0;
    roOptions.workDir = roOutDir.string();
    roOptions.partitioner = "mt-kahypar";
    roOptions.mtKaHyParPreset = "quality";
    roOptions.mtKaHyParThreads = 0;
    roOptions.keepIntermediateFiles = true;
    roManager.addPass(std::make_unique<RepcutPass>(roOptions));

    PassDiagnostics roDiags;
    PassManagerResult roResult{};
    try
    {
        roResult = roManager.run(readOnlyDesign, roDiags);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("read-only register repcut exception: ") + ex.what());
    }

    if (!roResult.success || roDiags.hasError())
    {
        return fail("read-only register repcut failed unexpectedly");
    }
    if (!roResult.changed)
    {
        return fail("read-only register repcut expected graph changes");
    }
    if (readOnlyDesign.findGraph("top_read_only_reg") == nullptr)
    {
        return fail("read-only register top graph missing after repcut");
    }

    const std::string *roStats = findRepcutStatsDiagnostic(roDiags);
    if (roStats == nullptr)
    {
        return fail("read-only register repcut missing stats artifact");
    }
    if (roStats->find("\"weight_partition_stats\":{") == std::string::npos)
    {
        return fail("read-only register repcut missing weight_partition_stats");
    }
    if (roStats->find("\"original_total_weight\":") == std::string::npos)
    {
        return fail("read-only register repcut missing original_total_weight");
    }
    if (roStats->find("\"max_partition_weight\":") == std::string::npos)
    {
        return fail("read-only register repcut missing max_partition_weight");
    }
    if (roStats->find("\"avg_partition_weight\":") == std::string::npos)
    {
        return fail("read-only register repcut missing avg_partition_weight");
    }
    if (roStats->find("\"original_over_max_weight_ratio\":") == std::string::npos)
    {
        return fail("read-only register repcut missing original_over_max_weight_ratio");
    }
    if (roStats->find("\"original_over_avg_weight_ratio\":") == std::string::npos)
    {
        return fail("read-only register repcut missing original_over_avg_weight_ratio");
    }
    if (roStats->find("\"max_partition_weight_fraction_of_original\":") == std::string::npos)
    {
        return fail("read-only register repcut missing max_partition_weight_fraction_of_original");
    }
    if (roStats->find("\"avg_partition_weight_fraction_of_original\":") == std::string::npos)
    {
        return fail("read-only register repcut missing avg_partition_weight_fraction_of_original");
    }
    if (roStats->find("\"partition_static_features\":[") == std::string::npos)
    {
        return fail("read-only register repcut missing partition_static_features");
    }
    if (roStats->find("\"weight\":") == std::string::npos)
    {
        return fail("read-only register repcut missing per-partition weight entries");
    }

    // Regression: effect sinks should be handled directly by repcut without
    // strip-debug pre-processing, and hierarchical ops should still be
    // rejected by repcut guard.
    wolvrix::lib::grh::Design effectDesign;
    wolvrix::lib::grh::Graph &effectGraph = effectDesign.createGraph("top_effect_sink");
    effectDesign.markAsTop("top_effect_sink");

    const auto effA = makeValue(effectGraph, "a", 32, false);
    const auto effB = makeValue(effectGraph, "b", 32, false);
    const auto effEn = makeValue(effectGraph, "en", 1, false);
    effectGraph.bindInputPort("a", effA);
    effectGraph.bindInputPort("b", effB);
    effectGraph.bindInputPort("en", effEn);

    const auto dpiImport =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func"));
    effectGraph.setAttr(dpiImport, "argsDirection", std::vector<std::string>{"input"});
    effectGraph.setAttr(dpiImport, "argsWidth", std::vector<int64_t>{32});
    effectGraph.setAttr(dpiImport, "argsName", std::vector<std::string>{"lhs"});
    effectGraph.setAttr(dpiImport, "argsSigned", std::vector<bool>{false});
    effectGraph.setAttr(dpiImport, "argsType", std::vector<std::string>{"logic"});
    effectGraph.setAttr(dpiImport, "hasReturn", true);
    effectGraph.setAttr(dpiImport, "returnWidth", static_cast<int64_t>(32));
    effectGraph.setAttr(dpiImport, "returnSigned", false);
    effectGraph.setAttr(dpiImport, "returnType", std::string("logic"));

    const auto dpiImportVoid =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func_void"));
    effectGraph.setAttr(dpiImportVoid, "argsDirection", std::vector<std::string>{"input"});
    effectGraph.setAttr(dpiImportVoid, "argsWidth", std::vector<int64_t>{32});
    effectGraph.setAttr(dpiImportVoid, "argsName", std::vector<std::string>{"rhs"});
    effectGraph.setAttr(dpiImportVoid, "argsSigned", std::vector<bool>{false});
    effectGraph.setAttr(dpiImportVoid, "argsType", std::vector<std::string>{"logic"});
    effectGraph.setAttr(dpiImportVoid, "hasReturn", false);

    const auto dpiImportVoidOutput =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func_void_output"));
    effectGraph.setAttr(dpiImportVoidOutput, "argsDirection", std::vector<std::string>{"input", "output"});
    effectGraph.setAttr(dpiImportVoidOutput, "argsWidth", std::vector<int64_t>{32, 32});
    effectGraph.setAttr(dpiImportVoidOutput, "argsName", std::vector<std::string>{"lhs", "data"});
    effectGraph.setAttr(dpiImportVoidOutput, "argsSigned", std::vector<bool>{false, false});
    effectGraph.setAttr(dpiImportVoidOutput, "argsType", std::vector<std::string>{"logic", "logic"});
    effectGraph.setAttr(dpiImportVoidOutput, "hasReturn", false);

    const auto dpiImportVoidInout =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func_void_inout"));
    effectGraph.setAttr(dpiImportVoidInout, "argsDirection", std::vector<std::string>{"inout"});
    effectGraph.setAttr(dpiImportVoidInout, "argsWidth", std::vector<int64_t>{32});
    effectGraph.setAttr(dpiImportVoidInout, "argsName", std::vector<std::string>{"state"});
    effectGraph.setAttr(dpiImportVoidInout, "argsSigned", std::vector<bool>{false});
    effectGraph.setAttr(dpiImportVoidInout, "argsType", std::vector<std::string>{"logic"});
    effectGraph.setAttr(dpiImportVoidInout, "hasReturn", false);

    const auto dpiImportVoidOutputUnused =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func_void_output_unused"));
    effectGraph.setAttr(dpiImportVoidOutputUnused, "argsDirection", std::vector<std::string>{"input", "output"});
    effectGraph.setAttr(dpiImportVoidOutputUnused, "argsWidth", std::vector<int64_t>{32, 32});
    effectGraph.setAttr(dpiImportVoidOutputUnused, "argsName", std::vector<std::string>{"lhs", "data"});
    effectGraph.setAttr(dpiImportVoidOutputUnused, "argsSigned", std::vector<bool>{false, false});
    effectGraph.setAttr(dpiImportVoidOutputUnused, "argsType", std::vector<std::string>{"logic", "logic"});
    effectGraph.setAttr(dpiImportVoidOutputUnused, "hasReturn", false);

    const auto dpiImportVoidInoutUnused =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport,
                                    effectGraph.internSymbol("dpi_func_void_inout_unused"));
    effectGraph.setAttr(dpiImportVoidInoutUnused, "argsDirection", std::vector<std::string>{"inout"});
    effectGraph.setAttr(dpiImportVoidInoutUnused, "argsWidth", std::vector<int64_t>{32});
    effectGraph.setAttr(dpiImportVoidInoutUnused, "argsName", std::vector<std::string>{"state"});
    effectGraph.setAttr(dpiImportVoidInoutUnused, "argsSigned", std::vector<bool>{false});
    effectGraph.setAttr(dpiImportVoidInoutUnused, "argsType", std::vector<std::string>{"logic"});
    effectGraph.setAttr(dpiImportVoidInoutUnused, "hasReturn", false);

    const auto dpiCall =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call"));
    effectGraph.addOperand(dpiCall, effEn);
    effectGraph.addOperand(dpiCall, effA);
    effectGraph.addOperand(dpiCall, effEn);
    effectGraph.setAttr(dpiCall, "targetImportSymbol", std::string("dpi_func"));
    effectGraph.setAttr(dpiCall, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCall, "inArgName", std::vector<std::string>{"lhs"});
    effectGraph.setAttr(dpiCall, "outArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCall, "hasReturn", true);
    const auto dpiRes = makeValue(effectGraph, "dpi_res", 32, false);
    effectGraph.addResult(dpiCall, dpiRes);

    const auto dpiCallVoid =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call_void"));
    effectGraph.addOperand(dpiCallVoid, effEn);
    effectGraph.addOperand(dpiCallVoid, effB);
    effectGraph.addOperand(dpiCallVoid, effEn);
    effectGraph.setAttr(dpiCallVoid, "targetImportSymbol", std::string("dpi_func_void"));
    effectGraph.setAttr(dpiCallVoid, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCallVoid, "inArgName", std::vector<std::string>{"rhs"});
    effectGraph.setAttr(dpiCallVoid, "outArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCallVoid, "hasReturn", false);

    const auto dpiCallVoidOutput =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call_void_output"));
    effectGraph.addOperand(dpiCallVoidOutput, effEn);
    effectGraph.addOperand(dpiCallVoidOutput, effA);
    effectGraph.addOperand(dpiCallVoidOutput, effEn);
    effectGraph.setAttr(dpiCallVoidOutput, "targetImportSymbol", std::string("dpi_func_void_output"));
    effectGraph.setAttr(dpiCallVoidOutput, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCallVoidOutput, "inArgName", std::vector<std::string>{"lhs"});
    effectGraph.setAttr(dpiCallVoidOutput, "outArgName", std::vector<std::string>{"data"});
    effectGraph.setAttr(dpiCallVoidOutput, "hasReturn", false);
    const auto dpiVoidOutputRes = makeValue(effectGraph, "dpi_void_output_res", 32, false);
    effectGraph.addResult(dpiCallVoidOutput, dpiVoidOutputRes);

    const auto dpiCallVoidInout =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call_void_inout"));
    effectGraph.addOperand(dpiCallVoidInout, effEn);
    effectGraph.addOperand(dpiCallVoidInout, effB);
    effectGraph.addOperand(dpiCallVoidInout, effEn);
    effectGraph.setAttr(dpiCallVoidInout, "targetImportSymbol", std::string("dpi_func_void_inout"));
    effectGraph.setAttr(dpiCallVoidInout, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCallVoidInout, "inArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCallVoidInout, "outArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCallVoidInout, "inoutArgName", std::vector<std::string>{"state"});
    effectGraph.setAttr(dpiCallVoidInout, "hasReturn", false);
    const auto dpiVoidInoutRes = makeValue(effectGraph, "dpi_void_inout_res", 32, false);
    effectGraph.addResult(dpiCallVoidInout, dpiVoidInoutRes);

    const auto dpiCallVoidOutputUnused =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call_void_output_unused"));
    effectGraph.addOperand(dpiCallVoidOutputUnused, effEn);
    effectGraph.addOperand(dpiCallVoidOutputUnused, effA);
    effectGraph.addOperand(dpiCallVoidOutputUnused, effEn);
    effectGraph.setAttr(dpiCallVoidOutputUnused,
                        "targetImportSymbol",
                        std::string("dpi_func_void_output_unused"));
    effectGraph.setAttr(dpiCallVoidOutputUnused, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCallVoidOutputUnused, "inArgName", std::vector<std::string>{"lhs"});
    effectGraph.setAttr(dpiCallVoidOutputUnused, "outArgName", std::vector<std::string>{"data"});
    effectGraph.setAttr(dpiCallVoidOutputUnused, "hasReturn", false);
    const auto dpiVoidOutputUnusedRes = makeValue(effectGraph, "dpi_void_output_unused_res", 32, false);
    effectGraph.addResult(dpiCallVoidOutputUnused, dpiVoidOutputUnusedRes);

    const auto dpiCallVoidInoutUnused =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kDpicCall,
                                    effectGraph.internSymbol("dpi_call_void_inout_unused"));
    effectGraph.addOperand(dpiCallVoidInoutUnused, effEn);
    effectGraph.addOperand(dpiCallVoidInoutUnused, effB);
    effectGraph.addOperand(dpiCallVoidInoutUnused, effEn);
    effectGraph.setAttr(dpiCallVoidInoutUnused,
                        "targetImportSymbol",
                        std::string("dpi_func_void_inout_unused"));
    effectGraph.setAttr(dpiCallVoidInoutUnused, "eventEdge", std::vector<std::string>{"posedge"});
    effectGraph.setAttr(dpiCallVoidInoutUnused, "inArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCallVoidInoutUnused, "outArgName", std::vector<std::string>{});
    effectGraph.setAttr(dpiCallVoidInoutUnused, "inoutArgName", std::vector<std::string>{"state"});
    effectGraph.setAttr(dpiCallVoidInoutUnused, "hasReturn", false);
    const auto dpiVoidInoutUnusedRes = makeValue(effectGraph, "dpi_void_inout_unused_res", 32, false);
    effectGraph.addResult(dpiCallVoidInoutUnused, dpiVoidInoutUnusedRes);

    const auto sysTask =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kSystemTask,
                                    effectGraph.internSymbol("sys_task"));
    effectGraph.addOperand(sysTask, dpiRes);
    effectGraph.setAttr(sysTask, "name", std::string("$display"));
    effectGraph.setAttr(sysTask, "hasReturn", false);

    const auto sysTaskRet =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kSystemTask,
                                    effectGraph.internSymbol("sys_task_ret"));
    effectGraph.addOperand(sysTaskRet, effEn);
    effectGraph.addOperand(sysTaskRet, effB);
    effectGraph.setAttr(sysTaskRet, "name", std::string("$ret_task"));
    effectGraph.setAttr(sysTaskRet, "hasReturn", true);
    const auto sysTaskRetRes = makeValue(effectGraph, "sys_task_ret_res", 32, false);
    effectGraph.addResult(sysTaskRet, sysTaskRetRes);

    const auto effData0 = makeValue(effectGraph, "lat_data0", 32, false);
    makeBinaryOp(effectGraph, wolvrix::lib::grh::OperationKind::kAdd,
                 "lat_add0", dpiRes, effA, effData0);

    const auto effLatch0 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                    effectGraph.internSymbol("lat0"));
    effectGraph.setAttr(effLatch0, "width", static_cast<int64_t>(32));
    effectGraph.setAttr(effLatch0, "isSigned", false);

    const auto effLatchWr0 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                    effectGraph.internSymbol("lat0_wr"));
    effectGraph.addOperand(effLatchWr0, effEn);
    effectGraph.addOperand(effLatchWr0, effData0);
    effectGraph.setAttr(effLatchWr0, "latchSymbol", std::string("lat0"));

    const auto effData1 = makeValue(effectGraph, "lat_data1", 32, false);
    makeBinaryOp(effectGraph, wolvrix::lib::grh::OperationKind::kSub,
                 "lat_sub1", effB, effA, effData1);

    const auto effLatch1 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                    effectGraph.internSymbol("lat1"));
    effectGraph.setAttr(effLatch1, "width", static_cast<int64_t>(32));
    effectGraph.setAttr(effLatch1, "isSigned", false);

    const auto effLatchWr1 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                    effectGraph.internSymbol("lat1_wr"));
    effectGraph.addOperand(effLatchWr1, effEn);
    effectGraph.addOperand(effLatchWr1, effData1);
    effectGraph.setAttr(effLatchWr1, "latchSymbol", std::string("lat1"));

    const auto effData2 = makeValue(effectGraph, "lat_data2", 32, false);
    makeBinaryOp(effectGraph, wolvrix::lib::grh::OperationKind::kAdd,
                 "lat_add2", sysTaskRetRes, effA, effData2);

    const auto effLatch2 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                    effectGraph.internSymbol("lat2"));
    effectGraph.setAttr(effLatch2, "width", static_cast<int64_t>(32));
    effectGraph.setAttr(effLatch2, "isSigned", false);

    const auto effLatchWr2 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                    effectGraph.internSymbol("lat2_wr"));
    effectGraph.addOperand(effLatchWr2, effEn);
    effectGraph.addOperand(effLatchWr2, effData2);
    effectGraph.setAttr(effLatchWr2, "latchSymbol", std::string("lat2"));

    const auto effData3 = makeValue(effectGraph, "lat_data3", 32, false);
    makeBinaryOp(effectGraph, wolvrix::lib::grh::OperationKind::kSub,
                 "lat_sub3", sysTaskRetRes, effB, effData3);

    const auto effLatch3 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                    effectGraph.internSymbol("lat3"));
    effectGraph.setAttr(effLatch3, "width", static_cast<int64_t>(32));
    effectGraph.setAttr(effLatch3, "isSigned", false);

    const auto effLatchWr3 =
        effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                    effectGraph.internSymbol("lat3_wr"));
    effectGraph.addOperand(effLatchWr3, effEn);
    effectGraph.addOperand(effLatchWr3, effData3);
    effectGraph.setAttr(effLatchWr3, "latchSymbol", std::string("lat3"));

    auto addEffectLatchSink = [&](int index,
                                  wolvrix::lib::grh::OperationKind kind,
                                  wolvrix::lib::grh::ValueId lhs,
                                  wolvrix::lib::grh::ValueId rhs) {
        const std::string suffix = std::to_string(index);
        const auto data = makeValue(effectGraph, "lat_data" + suffix, 32, false);
        makeBinaryOp(effectGraph, kind, "lat_data_op" + suffix, lhs, rhs, data);

        const std::string latchSymbol = "lat" + suffix;
        const auto latch = effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                                       effectGraph.internSymbol(latchSymbol));
        effectGraph.setAttr(latch, "width", static_cast<int64_t>(32));
        effectGraph.setAttr(latch, "isSigned", false);

        const auto write = effectGraph.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                                       effectGraph.internSymbol(latchSymbol + "_wr"));
        effectGraph.addOperand(write, effEn);
        effectGraph.addOperand(write, data);
        effectGraph.setAttr(write, "latchSymbol", latchSymbol);
    };

    addEffectLatchSink(4, wolvrix::lib::grh::OperationKind::kAdd, dpiVoidOutputRes, effA);
    addEffectLatchSink(5, wolvrix::lib::grh::OperationKind::kSub, dpiVoidOutputRes, effB);
    addEffectLatchSink(6, wolvrix::lib::grh::OperationKind::kAdd, dpiVoidInoutRes, effA);
    addEffectLatchSink(7, wolvrix::lib::grh::OperationKind::kSub, dpiVoidInoutRes, effB);

    const std::filesystem::path effectOutDir =
        std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "repcut_test_effect_sink";
    std::filesystem::create_directories(effectOutDir, ec);
    if (ec)
    {
        return fail("failed to create output directory: " + effectOutDir.string());
    }

    PassManager effectManager;
    effectManager.options().verbosity = PassVerbosity::Info;
    RepcutOptions effectOptions;
    effectOptions.path = "top_effect_sink";
    effectOptions.partitionCount = 2;
    effectOptions.imbalanceFactor = 1.0;
    effectOptions.workDir = effectOutDir.string();
    effectOptions.partitioner = "mt-kahypar";
    effectOptions.mtKaHyParPreset = "quality";
    effectOptions.mtKaHyParThreads = 0;
    effectOptions.keepIntermediateFiles = true;
    effectManager.addPass(std::make_unique<RepcutPass>(effectOptions));

    PassDiagnostics effectDiags;
    PassManagerResult effectResult{};
    try
    {
        effectResult = effectManager.run(effectDesign, effectDiags);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("effect-sink repcut exception: ") + ex.what());
    }

    if (!effectResult.success || effectDiags.hasError())
    {
        return fail("effect-sink repcut failed unexpectedly");
    }
    if (!effectResult.changed)
    {
        return fail("effect-sink repcut expected graph changes");
    }
    if (effectDesign.findGraph("top_effect_sink") == nullptr)
    {
        return fail("effect-sink top graph missing after repcut");
    }
    if (!hasGraphWithPrefix(effectDesign, "top_effect_sink_repcut_part0"))
    {
        return fail("expected partition graph for effect-sink repcut");
    }

    auto *returnedCallPart = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "dpi_call");
    if (returnedCallPart == nullptr)
    {
        return fail("expected partition graph containing returned kDpicCall after effect-sink repcut");
    }
    auto *sysTaskPart = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "sys_task");
    if (sysTaskPart == nullptr)
    {
        return fail("expected partition graph containing dependent system task after effect-sink repcut");
    }
    auto *lat0Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat0_wr");
    if (lat0Part == nullptr)
    {
        return fail("expected partition graph containing dependent latch sink after effect-sink repcut");
    }
    if (returnedCallPart != sysTaskPart || returnedCallPart != lat0Part)
    {
        return fail("returned kDpicCall and its dependent sinks should be merged into one ASC/partition");
    }
    if (!graphHasOpSymbol(*returnedCallPart, "dpi_func"))
    {
        return fail("returned kDpicCall partition missing matching kDpicImport after repcut");
    }
    auto *voidCallPart =
        findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "dpi_call_void");
    if (voidCallPart == nullptr)
    {
        return fail("expected partition graph containing no-return kDpicCall sink after effect-sink repcut");
    }
    if (!graphHasOpSymbol(*voidCallPart, "dpi_func_void"))
    {
        return fail("no-return kDpicCall partition missing matching kDpicImport after repcut");
    }

    auto *voidOutputCallPart =
        findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "dpi_call_void_output");
    auto *lat4Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat4_wr");
    auto *lat5Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat5_wr");
    if (voidOutputCallPart == nullptr || lat4Part == nullptr || lat5Part == nullptr)
    {
        return fail("expected void DPI output call and dependent latch sinks after effect-sink repcut");
    }
    if (voidOutputCallPart != lat4Part || voidOutputCallPart != lat5Part)
    {
        return fail("void DPI output call and its dependent sinks should be merged into one ASC/partition");
    }
    if (!graphHasOpSymbol(*voidOutputCallPart, "dpi_func_void_output"))
    {
        return fail("void DPI output call partition missing matching kDpicImport after repcut");
    }

    auto *voidInoutCallPart =
        findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "dpi_call_void_inout");
    auto *lat6Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat6_wr");
    auto *lat7Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat7_wr");
    if (voidInoutCallPart == nullptr || lat6Part == nullptr || lat7Part == nullptr)
    {
        return fail("expected void DPI inout call and dependent latch sinks after effect-sink repcut");
    }
    if (voidInoutCallPart != lat6Part || voidInoutCallPart != lat7Part)
    {
        return fail("void DPI inout call and its dependent sinks should be merged into one ASC/partition");
    }
    if (!graphHasOpSymbol(*voidInoutCallPart, "dpi_func_void_inout"))
    {
        return fail("void DPI inout call partition missing matching kDpicImport after repcut");
    }
    if (countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_call_void_output") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_func_void_output") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_call_void_inout") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_func_void_inout") != 1)
    {
        return fail("consumed void DPI result producer/import should each be kept exactly once after repcut");
    }
    if (countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_call_void_output_unused") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_func_void_output_unused") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_call_void_inout_unused") != 1 ||
        countOpSymbolInGraphsWithPrefix(effectDesign,
                                        "top_effect_sink_repcut_part",
                                        "dpi_func_void_inout_unused") != 1)
    {
        return fail("unused void DPI result producer/import should each be kept exactly once after repcut");
    }

    auto *returnedSysTaskPart =
        findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "sys_task_ret");
    if (returnedSysTaskPart == nullptr)
    {
        return fail("expected partition graph containing returned kSystemTask after effect-sink repcut");
    }
    auto *lat2Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat2_wr");
    if (lat2Part == nullptr)
    {
        return fail("expected partition graph containing first sink depending on returned kSystemTask");
    }
    auto *lat3Part = findGraphWithPrefixAndOpSymbol(effectDesign, "top_effect_sink_repcut_part", "lat3_wr");
    if (lat3Part == nullptr)
    {
        return fail("expected partition graph containing second sink depending on returned kSystemTask");
    }
    if (returnedSysTaskPart != lat2Part || returnedSysTaskPart != lat3Part)
    {
        return fail("returned kSystemTask and its dependent sinks should be merged into one ASC/partition");
    }

    wolvrix::lib::grh::Design hierGuardDesign;
    wolvrix::lib::grh::Graph &hierGraph = hierGuardDesign.createGraph("top_hier_guard");
    hierGuardDesign.markAsTop("top_hier_guard");

    const auto hierIn = makeValue(hierGraph, "a", 8, false);
    hierGraph.bindInputPort("a", hierIn);

    const auto badInst =
        hierGraph.createOperation(wolvrix::lib::grh::OperationKind::kInstance,
                                  hierGraph.internSymbol("u_child"));
    hierGraph.addOperand(badInst, hierIn);
    hierGraph.setAttr(badInst, "moduleName", std::string("child"));
    hierGraph.setAttr(badInst, "instanceName", std::string("u_child"));
    hierGraph.setAttr(badInst, "inputPortName", std::vector<std::string>{"a"});
    hierGraph.setAttr(badInst, "outputPortName", std::vector<std::string>{});

    const auto badBlackbox =
        hierGraph.createOperation(wolvrix::lib::grh::OperationKind::kBlackbox,
                                  hierGraph.internSymbol("u_bb"));
    hierGraph.addOperand(badBlackbox, hierIn);

    const std::filesystem::path hierGuardOutDir =
        std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "repcut_test_hier_guard";
    std::filesystem::create_directories(hierGuardOutDir, ec);
    if (ec)
    {
        return fail("failed to create output directory: " + hierGuardOutDir.string());
    }

    PassManager hierGuardManager;
    hierGuardManager.options().verbosity = PassVerbosity::Info;
    RepcutOptions hierGuardOptions;
    hierGuardOptions.path = "top_hier_guard";
    hierGuardOptions.partitionCount = 2;
    hierGuardOptions.imbalanceFactor = 1.0;
    hierGuardOptions.workDir = hierGuardOutDir.string();
    hierGuardOptions.partitioner = "mt-kahypar";
    hierGuardOptions.mtKaHyParPreset = "quality";
    hierGuardOptions.mtKaHyParThreads = 0;
    hierGuardOptions.keepIntermediateFiles = true;
    hierGuardManager.addPass(std::make_unique<RepcutPass>(hierGuardOptions));

    PassDiagnostics hierGuardDiags;
    PassManagerResult hierGuardResult{};
    try
    {
        hierGuardResult = hierGuardManager.run(hierGuardDesign, hierGuardDiags);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("hier-guard repcut exception: ") + ex.what());
    }

    if (hierGuardResult.success || !hierGuardDiags.hasError())
    {
        return fail("hier-guard repcut should fail on kInstance/kBlackbox");
    }
    if (!diagnosticsContain(hierGuardDiags, "kind=kInstance"))
    {
        return fail("hier-guard repcut missing kInstance diagnostic");
    }
    if (!diagnosticsContain(hierGuardDiags, "kind=kBlackbox"))
    {
        return fail("hier-guard repcut missing kBlackbox diagnostic");
    }

    // Path-mode regression: resolving "top.u_child" should partition the child module graph
    // rather than the top wrapper graph.
    wolvrix::lib::grh::Design pathDesign;
    wolvrix::lib::grh::Graph &pathChild = pathDesign.createGraph("child");
    wolvrix::lib::grh::Graph &pathTop = pathDesign.createGraph("top");
    pathDesign.markAsTop("top");

    const auto childIn = makeValue(pathChild, "in_data", 32, false);
    const auto childEn = makeValue(pathChild, "en", 1, false);
    pathChild.bindInputPort("in_data", childIn);
    pathChild.bindInputPort("en", childEn);

    const auto childRegDecl =
        pathChild.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                  pathChild.internSymbol("random_bits"));
    pathChild.setAttr(childRegDecl, "width", static_cast<int64_t>(32));
    pathChild.setAttr(childRegDecl, "isSigned", false);
    pathChild.setAttr(childRegDecl, "initValue", std::string("$random"));

    const auto childRead =
        pathChild.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                  pathChild.internSymbol("random_bits_rd"));
    const auto childReadValue = makeValue(pathChild, "random_bits_rd_v", 32, false);
    pathChild.addResult(childRead, childReadValue);
    pathChild.setAttr(childRead, "regSymbol", std::string("random_bits"));

    const auto childData = makeValue(pathChild, "lat_data", 32, false);
    makeBinaryOp(pathChild, wolvrix::lib::grh::OperationKind::kAdd,
                 "lat_data_add", childReadValue, childIn, childData);

    const auto childLatchDecl =
        pathChild.createOperation(wolvrix::lib::grh::OperationKind::kLatch,
                                  pathChild.internSymbol("ro_lat"));
    pathChild.setAttr(childLatchDecl, "width", static_cast<int64_t>(32));
    pathChild.setAttr(childLatchDecl, "isSigned", false);

    const auto childLatchWrite =
        pathChild.createOperation(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                                  pathChild.internSymbol("ro_lat_wr"));
    pathChild.addOperand(childLatchWrite, childEn);
    pathChild.addOperand(childLatchWrite, childData);
    pathChild.setAttr(childLatchWrite, "latchSymbol", std::string("ro_lat"));

    const auto topIn = makeValue(pathTop, "in_data", 32, false);
    const auto topEn = makeValue(pathTop, "en", 1, false);
    pathTop.bindInputPort("in_data", topIn);
    pathTop.bindInputPort("en", topEn);

    const auto childInst =
        pathTop.createOperation(wolvrix::lib::grh::OperationKind::kInstance, pathTop.makeInternalOpSym());
    pathTop.addOperand(childInst, topIn);
    pathTop.addOperand(childInst, topEn);
    pathTop.setAttr(childInst, "moduleName", std::string("child"));
    pathTop.setAttr(childInst, "instanceName", std::string("u_child"));
    pathTop.setAttr(childInst, "inputPortName", std::vector<std::string>{"in_data", "en"});
    pathTop.setAttr(childInst, "outputPortName", std::vector<std::string>{});

    const std::filesystem::path pathOutDir =
        std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "repcut_test_path_mode";
    std::filesystem::create_directories(pathOutDir, ec);
    if (ec)
    {
        return fail("failed to create output directory: " + pathOutDir.string());
    }

    PassManager pathManager;
    pathManager.options().verbosity = PassVerbosity::Info;
    RepcutOptions pathOptions;
    pathOptions.path = "top.u_child";
    pathOptions.partitionCount = 2;
    pathOptions.imbalanceFactor = 1.0;
    pathOptions.workDir = pathOutDir.string();
    pathOptions.partitioner = "mt-kahypar";
    pathOptions.mtKaHyParPreset = "quality";
    pathOptions.mtKaHyParThreads = 0;
    pathOptions.keepIntermediateFiles = true;
    pathManager.addPass(std::make_unique<RepcutPass>(pathOptions));

    PassDiagnostics pathDiags;
    PassManagerResult pathResult{};
    try
    {
        pathResult = pathManager.run(pathDesign, pathDiags);
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("path-mode repcut exception: ") + ex.what());
    }

    if (!pathResult.success || pathDiags.hasError())
    {
        return fail("path-mode repcut failed unexpectedly");
    }
    if (!pathResult.changed)
    {
        return fail("path-mode repcut expected graph changes");
    }
    if (pathDesign.findGraph("top") == nullptr)
    {
        return fail("path-mode top graph missing after repcut");
    }
    wolvrix::lib::grh::Graph *rebuiltChild = pathDesign.findGraph("child");
    if (rebuiltChild == nullptr)
    {
        return fail("path-mode child graph missing after repcut");
    }
    if (!findInstanceByName(pathTop, "u_child").valid())
    {
        return fail("path-mode top instance should remain after child repcut");
    }
    std::size_t childInstanceCount = 0;
    for (const auto opId : rebuiltChild->operations())
    {
        if (rebuiltChild->getOperation(opId).kind() == wolvrix::lib::grh::OperationKind::kInstance)
        {
            ++childInstanceCount;
        }
    }
    if (childInstanceCount < 1)
    {
        return fail("path-mode rebuilt child should contain partition instances");
    }
    if (!hasGraphWithPrefix(pathDesign, "child_repcut_part0"))
    {
        return fail("expected child_repcut_part0 graph after path-mode repcut");
    }

    return 0;
#endif
}
