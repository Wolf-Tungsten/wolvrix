#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-instance-struct-array-field-read] " << message << '\n';
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
    driver.options.topModules.emplace_back("ingest_instance_struct_array_field_read");

    std::vector<std::string> args = {
        "ingest-instance-struct-array-field-read", sourcePath.string()};
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    if (!driver.parseCommandLine(static_cast<int>(argv.size()), argv.data()) ||
        !driver.processOptions() || !driver.parseAllSources()) {
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

int testInstanceStructArrayFieldRead(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath);
    if (!bundle) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());
    if (!driver.diagnostics().empty()) {
        std::string messages;
        for (const auto& diagnostic : driver.diagnostics().messages()) {
            messages += diagnostic.message + ";";
        }
        return fail("Unexpected ingest diagnostics: " + messages);
    }

    const wolvrix::lib::grh::Graph* graph =
        design.findGraph("ingest_instance_struct_array_field_read");
    if (!graph) {
        return fail("Missing parent graph");
    }

    bool found = false;
    for (const wolvrix::lib::grh::OperationId opId : graph->operations()) {
        const wolvrix::lib::grh::Operation instance = graph->getOperation(opId);
        if (instance.kind() != wolvrix::lib::grh::OperationKind::kInstance) {
            continue;
        }
        const auto module = instance.attr("moduleName");
        if (!module || std::get_if<std::string>(&*module) == nullptr ||
            *std::get_if<std::string>(&*module) != "read_addr_child") {
            continue;
        }
        found = true;
        const auto names = instance.attr("inputPortName");
        const auto* portNames = names ? std::get_if<std::vector<std::string>>(&*names) : nullptr;
        if (!portNames || portNames->size() != instance.operands().size()) {
            return fail("Missing instance input port names");
        }
        bool sawAddress = false;
        for (std::size_t i = 0; i < portNames->size(); ++i) {
            if ((*portNames)[i] != "addr") {
                continue;
            }
            sawAddress = true;
            const wolvrix::lib::grh::Value value = graph->getValue(instance.operands()[i]);
            if (value.width() != 24 || !value.definingOp().valid()) {
                return fail("Packed address port is not 24 bits");
            }
            const wolvrix::lib::grh::Operation concat = graph->getOperation(value.definingOp());
            if (concat.kind() != wolvrix::lib::grh::OperationKind::kConcat ||
                concat.operands().size() != 4) {
                return fail("Packed address port must concatenate four field reads");
            }
            for (const wolvrix::lib::grh::ValueId lane : concat.operands()) {
                if (graph->getValue(lane).width() != 6) {
                    return fail("Struct-array field read lost address bits");
                }
            }
        }
        if (!sawAddress) {
            return fail("Missing packed address input");
        }
    }
    if (!found) {
        return fail("Missing read_addr_child instance");
    }
    return 0;
}

} // namespace

int main() {
    return testInstanceStructArrayFieldRead(
        WOLF_SV_INGEST_INSTANCE_STRUCT_ARRAY_FIELD_READ_DATA_PATH);
}
