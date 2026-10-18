#include "core/ingest.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"

namespace {

using wolvrix::lib::grh::OperationKind;

int fail(const std::string& message) {
    std::cerr << "[ingest-expression-width] " << message << '\n';
    return 1;
}

struct CompilationBundle {
    slang::driver::Driver driver;
    std::unique_ptr<slang::ast::Compilation> compilation;
};

std::unique_ptr<CompilationBundle> compileInput(const std::filesystem::path& sourcePath) {
    auto bundle = std::make_unique<CompilationBundle>();
    auto& driver = bundle->driver;
    driver.addStandardArgs();
    driver.languageVersion = slang::LanguageVersion::v1800_2023;
    driver.options.topModules.emplace_back("expression_width");
    const std::string path = sourcePath.string();
    const char* argv[] = {"ingest-expression-width", path.c_str()};
    if (!driver.parseCommandLine(2, argv) || !driver.processOptions() ||
        !driver.parseAllSources()) {
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

int testExpressionWidths(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath);
    if (!bundle) {
        return fail("Failed to compile " + sourcePath.string());
    }
    wolvrix::lib::ingest::ConvertDriver driver;
    const wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        return fail("Unexpected ingest diagnostics");
    }
    const wolvrix::lib::grh::Graph* graph = design.findGraph("expression_width");
    if (!graph) {
        return fail("Missing expression_width graph");
    }

    constexpr std::array booleanKinds = {
        OperationKind::kEq, OperationKind::kNe, OperationKind::kCaseEq,
        OperationKind::kCaseNe, OperationKind::kWildcardEq, OperationKind::kWildcardNe,
        OperationKind::kLt, OperationKind::kLe, OperationKind::kGt, OperationKind::kGe,
        OperationKind::kLogicAnd, OperationKind::kLogicOr, OperationKind::kLogicNot,
        OperationKind::kReduceAnd, OperationKind::kReduceOr, OperationKind::kReduceXor,
        OperationKind::kReduceNand, OperationKind::kReduceNor, OperationKind::kReduceXnor
    };
    std::array<unsigned, booleanKinds.size()> counts{};
    unsigned targetOrs = 0;
    unsigned targetMuxes = 0;
    unsigned predicateMuxes = 0;
    unsigned products = 0;
    for (const wolvrix::lib::grh::OperationId id : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(id);
        if (op.kind() == OperationKind::kMul) {
            ++products;
            if (op.results().size() != 1 || graph->getValue(op.results()[0]).width() != 22) {
                return fail("Product must wrap at its elaborated 22-bit operand width");
            }
        }
        if (op.kind() == OperationKind::kOr) {
            ++targetOrs;
            if (op.results().size() != 1 || graph->getValue(op.results()[0]).width() != 22) {
                return fail("Target OR must preserve 22 bits before boolean evaluation: " +
                            std::string(op.symbolText()));
            }
            for (const auto operand : op.operands()) {
                if (graph->getValue(operand).width() != 22) {
                    return fail("Target OR operand was truncated");
                }
            }
        }
        if (op.kind() == OperationKind::kMux) {
            if (op.operands().size() != 3 || op.results().size() != 1 ||
                graph->getValue(op.operands()[0]).width() != 1) {
                return fail("Mux must have a one-bit condition and two value operands");
            }
            const auto width = graph->getValue(op.results()[0]).width();
            if (width == 22) {
                ++targetMuxes;
            } else if (width == 1) {
                ++predicateMuxes;
                const auto predicate = graph->getValue(op.operands()[0]);
                if (!predicate.definingOp().valid() ||
                    graph->getOperation(predicate.definingOp()).kind() != OperationKind::kReduceOr) {
                    return fail("Wide conditional predicate must test all 22 bits");
                }
            } else {
                return fail("Unexpected target Mux width " + std::to_string(width));
            }
        }
        for (std::size_t i = 0; i < booleanKinds.size(); ++i) {
            if (op.kind() != booleanKinds[i]) {
                continue;
            }
            ++counts[i];
            if (op.results().size() != 1 || graph->getValue(op.results()[0]).width() != 1) {
                return fail("Boolean result must be one bit");
            }
            for (const auto operand : op.operands()) {
                if (graph->getValue(operand).width() != 22) {
                    return fail(std::string(wolvrix::lib::grh::toString(op.kind())) +
                                " operand lost target bits");
                }
            }
        }
    }
    if (targetOrs != 23 || targetMuxes != 46 || predicateMuxes != 2 || products != 4) {
        return fail("Missing target selections or conditional predicate coverage");
    }
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] == 0) {
            return fail("Missing coverage for " +
                        std::string(wolvrix::lib::grh::toString(booleanKinds[i])));
        }
    }
    return 0;
}

} // namespace

int main() {
    return testExpressionWidths(WOLVRIX_INGEST_EXPRESSION_WIDTH_DATA_PATH);
}
