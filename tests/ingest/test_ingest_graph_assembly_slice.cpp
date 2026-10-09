#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-graph-assembly-slice] " << message << '\n';
    return 1;
}

struct CompilationBundle {
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

std::optional<int64_t> intAttr(const wolvrix::lib::grh::Operation& op,
                               std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<int64_t>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::optional<std::string> stringAttr(const wolvrix::lib::grh::Operation& op,
                                      std::string_view key) {
    auto attr = op.attr(key);
    if (!attr) {
        return std::nullopt;
    }
    if (const auto* value = std::get_if<std::string>(&*attr)) {
        return *value;
    }
    return std::nullopt;
}

std::unique_ptr<CompilationBundle> compileInput(const std::filesystem::path& sourcePath,
                                                std::string_view topModule) {
    auto bundle = std::make_unique<CompilationBundle>();
    auto& driver = bundle->driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    if (!topModule.empty()) {
        driver.options.topModules.emplace_back(topModule);
    }

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-graph-assembly-slice");
    argStorage.emplace_back(sourcePath.string());
    std::vector<const char*> argv;
    argv.reserve(argStorage.size());
    for (const std::string& arg : argStorage) {
        argv.push_back(arg.c_str());
    }

    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data())) {
        return nullptr;
    }
    if (!driver.processOptions()) {
        return nullptr;
    }
    if (!driver.parseAllSources()) {
        return nullptr;
    }

    bundle->compilation = driver.createCompilation();
    if (!bundle->compilation) {
        return nullptr;
    }
    driver.reportCompilation(*bundle->compilation, /* quiet */ true);
    driver.runAnalysis(*bundle->compilation);
    return bundle;
}

int testGraphAssemblySlice(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath, "graph_assembly_slice");
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    if (design.topGraphs().size() != 1) {
        return fail("Expected exactly one top graph");
    }
    if (design.topGraphs().front() != "graph_assembly_slice") {
        return fail("Unexpected top graph name");
    }

    const wolvrix::lib::grh::Graph* graph = design.findGraph("graph_assembly_slice");
    if (!graph) {
        return fail("Missing graph_assembly_slice graph");
    }

    bool hasConcat = false;
    bool hasSlice = false;
    bool hasConst = false;
    bool hasPackedLane9Slice = false;
    bool hasPackedDynamicLaneSliceArray = false;
    bool hasSmallDynamicLaneSliceArray = false;
    bool hasUptoDynamicLaneSliceArray = false;
    bool hasSmallPackedArrayConcatAttrs = false;
    bool hasUptoPackedArrayConcatAttrs = false;
    bool hasSixBitShiftOffset = false;

    for (wolvrix::lib::grh::OperationId opId : graph->operations()) {
        wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        switch (op.kind()) {
        case wolvrix::lib::grh::OperationKind::kConcat:
            hasConcat = true;
            break;
        case wolvrix::lib::grh::OperationKind::kLShr:
            if (op.operands().size() == 2) {
                const wolvrix::lib::grh::Value shiftAmount =
                    graph->getValue(op.operands()[1]);
                const wolvrix::lib::grh::OperationId def = shiftAmount.definingOp();
                if (def.valid()) {
                    const wolvrix::lib::grh::Operation subOp = graph->getOperation(def);
                    if (subOp.kind() == wolvrix::lib::grh::OperationKind::kSub &&
                        shiftAmount.width() == 6) {
                        hasSixBitShiftOffset = true;
                    }
                }
            }
            break;
        case wolvrix::lib::grh::OperationKind::kSliceStatic:
            hasSlice = true;
            if (op.operands().size() == 1) {
                const wolvrix::lib::grh::Value operand = graph->getValue(op.operands()[0]);
                const auto sliceStart = intAttr(op, "sliceStart");
                const auto sliceEnd = intAttr(op, "sliceEnd");
                if (operand.symbolText() == "packed_data" &&
                    sliceStart && sliceEnd &&
                    *sliceStart == 72 && *sliceEnd == 79) {
                    hasPackedLane9Slice = true;
                }
            }
            break;
        case wolvrix::lib::grh::OperationKind::kSliceArray:
            if (op.operands().size() == 2) {
                const wolvrix::lib::grh::Value operand = graph->getValue(op.operands()[0]);
                const auto baseShape = graph->valueShape(op.operands()[0]);
                if (baseShape.empty() || baseShape.front() <= 0 ||
                    operand.width() % baseShape.front() != 0) {
                    break;
                }
                const int64_t elementWidth = operand.width() / baseShape.front();
                if (operand.symbolText() == "packed_data" && elementWidth == 8) {
                    hasPackedDynamicLaneSliceArray = true;
                }
                if (operand.symbolText() == "small_lanes" && elementWidth == 3) {
                    hasSmallDynamicLaneSliceArray = true;
                }
                if (operand.symbolText() == "upto_lanes" && elementWidth == 3) {
                    const wolvrix::lib::grh::Value index =
                        graph->getValue(op.operands()[1]);
                    const wolvrix::lib::grh::OperationId def = index.definingOp();
                    if (def.valid()) {
                        const wolvrix::lib::grh::Operation sub = graph->getOperation(def);
                        hasUptoDynamicLaneSliceArray =
                            sub.kind() == wolvrix::lib::grh::OperationKind::kSub;
                    }
                }
            }
            break;
        case wolvrix::lib::grh::OperationKind::kAssign:
            if (op.operands().size() == 1 && op.results().size() == 1) {
                const wolvrix::lib::grh::Value result = graph->getValue(op.results()[0]);
                if (result.symbolText() == "small_lanes") {
                    // stage-04: the declaration shape replaces
                    // svPackedArray.* — the net value itself carries the
                    // array view [elementCount].
                    const std::span<const int64_t> shape = result.shape();
                    hasSmallPackedArrayConcatAttrs =
                        result.width() == 6 && shape.size() == 1 && shape[0] == 2;
                    const wolvrix::lib::grh::Value rhs = graph->getValue(op.operands()[0]);
                    const wolvrix::lib::grh::OperationId def = rhs.definingOp();
                    if (!def.valid() ||
                        graph->getOperation(def).kind() !=
                            wolvrix::lib::grh::OperationKind::kConcat) {
                        hasSmallPackedArrayConcatAttrs = false;
                    }
                }
                if (result.symbolText() == "upto_lanes") {
                    const std::span<const int64_t> shape = result.shape();
                    hasUptoPackedArrayConcatAttrs =
                        result.width() == 6 && shape.size() == 1 && shape[0] == 2;
                    const wolvrix::lib::grh::Value rhs = graph->getValue(op.operands()[0]);
                    const wolvrix::lib::grh::OperationId def = rhs.definingOp();
                    if (!def.valid() ||
                        graph->getOperation(def).kind() !=
                            wolvrix::lib::grh::OperationKind::kConcat) {
                        hasUptoPackedArrayConcatAttrs = false;
                    }
                }
            }
            break;
        case wolvrix::lib::grh::OperationKind::kConstant:
            hasConst = true;
            break;
        default:
            break;
        }
    }

    if (!hasConcat) {
        return fail("Missing kConcat op in graph");
    }
    if (!hasSlice) {
        return fail("Missing kSliceStatic op in graph");
    }
    if (!hasConst) {
        return fail("Missing kConstant op in graph");
    }
    if (!hasPackedLane9Slice) {
        return fail("Missing packed_data[9] kSliceStatic [72:79]");
    }
    if (!hasPackedDynamicLaneSliceArray) {
        return fail("Missing packed_data[dyn_idx] kSliceArray with element width 8 from shape");
    }
    if (!hasSmallDynamicLaneSliceArray) {
        return fail("Missing small_lanes[small_idx] kSliceArray with element width 3 from shape");
    }
    if (!hasUptoDynamicLaneSliceArray) {
        return fail("Missing upto_lanes[upto_idx] kSliceArray with high-index lane adjustment");
    }
    if (!hasSmallPackedArrayConcatAttrs) {
        return fail("Missing declaration shape [2] on small_lanes value");
    }
    if (!hasUptoPackedArrayConcatAttrs) {
        return fail("Missing declaration shape [2] on upto_lanes value");
    }
    if (!hasSixBitShiftOffset) {
        return fail("Missing 6-bit shift offset for first_rot >> (6'hB - start)");
    }
    return 0;
}

} // namespace

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_GRAPH_ASSEMBLY_SLICE_DATA_PATH;
    return testGraphAssemblySlice(sourcePath);
}
