#include "core/ingest.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/Driver.h"

namespace {

int fail(const std::string& message) {
    std::cerr << "[ingest-generate-scope] " << message << '\n';
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

    std::vector<std::string> argStorage;
    argStorage.emplace_back("ingest-generate-scope");
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

std::set<std::string> declaredSymbolTexts(const wolvrix::lib::grh::Graph& graph) {
    std::set<std::string> out;
    for (const wolvrix::lib::grh::SymbolId sym : graph.declaredSymbols()) {
        if (sym.valid()) {
            out.emplace(graph.symbolText(sym));
        }
    }
    return out;
}

const wolvrix::lib::grh::Graph::GenerateGroup* findGroup(const wolvrix::lib::grh::Graph& graph,
                                                         std::string_view scope,
                                                         std::string_view name) {
    for (const auto& group : graph.generateGroups()) {
        if (graph.symbolText(group.scope) == scope && graph.symbolText(group.name) == name) {
            return &group;
        }
    }
    return nullptr;
}

bool groupMembersMatch(const wolvrix::lib::grh::Graph& graph,
                       const wolvrix::lib::grh::Graph::GenerateGroup& group,
                       const std::vector<std::string>& expected) {
    if (group.symbols.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (graph.symbolText(group.symbols[i]) != expected[i]) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation& op,
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

bool hasAssignResult(const wolvrix::lib::grh::Graph& graph, std::string_view resultName) {
    for (const wolvrix::lib::grh::OperationId opId : graph.operations()) {
        const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kAssign) {
            continue;
        }
        for (const wolvrix::lib::grh::ValueId result : op.results()) {
            if (graph.getValue(result).symbolText() == resultName) {
                return true;
            }
        }
    }
    return false;
}

int testBasic(const wolvrix::lib::grh::Design& design) {
    const wolvrix::lib::grh::Graph* graph = design.findGraph("generate_scope_basic");
    if (!graph) {
        return fail("Missing generate_scope_basic graph");
    }

    const std::set<std::string> declared = declaredSymbolTexts(*graph);
    for (int i = 0; i < 4; ++i) {
        for (const char* leaf : {"sig", "tmp"}) {
            const std::string name =
                "gen_loop$" + std::to_string(i) + "$" + leaf;
            if (!declared.count(name)) {
                return fail("Missing declared symbol: " + name);
            }
            if (!graph->findValue(name).valid()) {
                return fail("Generate-scope signal not resolvable: " + name);
            }
        }
    }

    if (graph->generateGroups().size() != 2) {
        return fail("Expected exactly 2 generate groups in generate_scope_basic");
    }
    for (const char* leaf : {"sig", "tmp"}) {
        const auto* group = findGroup(*graph, "gen_loop", leaf);
        if (!group) {
            return fail(std::string("Missing generate group gen_loop/") + leaf);
        }
        std::vector<std::string> expected;
        for (int i = 0; i < 4; ++i) {
            expected.push_back("gen_loop$" + std::to_string(i) + "$" + leaf);
        }
        if (!groupMembersMatch(*graph, *group, expected)) {
            return fail(std::string("Generate group member order mismatch for ") + leaf);
        }
    }
    return 0;
}

int testCollision(const wolvrix::lib::grh::Design& design) {
    const wolvrix::lib::grh::Graph* graph = design.findGraph("generate_scope_collision");
    if (!graph) {
        return fail("Missing generate_scope_collision graph");
    }

    const std::set<std::string> declared = declaredSymbolTexts(*graph);
    if (!declared.count("sig")) {
        return fail("Missing top-level declared symbol: sig");
    }
    const wolvrix::lib::grh::ValueId topSig = graph->findValue("sig");
    if (!topSig.valid()) {
        return fail("Top-level sig not resolvable");
    }
    for (int i = 0; i < 4; ++i) {
        const std::string name = "gen_loop$" + std::to_string(i) + "$sig";
        if (!declared.count(name)) {
            return fail("Missing declared symbol: " + name);
        }
        const wolvrix::lib::grh::ValueId copy = graph->findValue(name);
        if (!copy.valid() || copy == topSig) {
            return fail("Generate-scope copy does not resolve distinctly: " + name);
        }
        if (!hasAssignResult(*graph, name)) {
            return fail("In-block assign did not target the generate-scope copy: " + name);
        }
    }
    if (!hasAssignResult(*graph, "sig")) {
        return fail("Top-level assign to sig missing");
    }
    return 0;
}

int testInit(const wolvrix::lib::grh::Design& design) {
    const wolvrix::lib::grh::Graph* graph = design.findGraph("generate_scope_init");
    if (!graph) {
        return fail("Missing generate_scope_init graph");
    }

    int found = 0;
    for (const wolvrix::lib::grh::OperationId opId : graph->operations()) {
        const wolvrix::lib::grh::Operation op = graph->getOperation(opId);
        if (op.kind() != wolvrix::lib::grh::OperationKind::kRegister) {
            continue;
        }
        const std::string_view sym = op.symbolText();
        if (sym != "gen_init$0$acc" && sym != "gen_init$1$acc") {
            continue;
        }
        const std::optional<std::string> initValue = getAttrString(op, "initValue");
        if (!initValue || *initValue != "8'h3c") {
            return fail("kRegister " + std::string(sym) + " missing initValue=8'h3c");
        }
        ++found;
    }
    if (found != 2) {
        return fail("Expected 2 generate-scope kRegister ops with initializer");
    }
    return 0;
}

int testGenerateScope(const std::filesystem::path& sourcePath) {
    auto bundle = compileInput(sourcePath);
    if (!bundle || !bundle->compilation) {
        return fail("Failed to compile " + sourcePath.string());
    }

    wolvrix::lib::ingest::ConvertDriver driver;
    wolvrix::lib::grh::Design design = driver.convert(bundle->compilation->getRoot());

    if (int rc = testBasic(design)) {
        return rc;
    }
    if (int rc = testCollision(design)) {
        return rc;
    }
    if (int rc = testInit(design)) {
        return rc;
    }
    return 0;
}

} // namespace

#ifndef WOLF_SV_INGEST_GENERATE_SCOPE_DATA_PATH
#error "WOLF_SV_INGEST_GENERATE_SCOPE_DATA_PATH must be defined"
#endif

int main() {
    const std::filesystem::path sourcePath = WOLF_SV_INGEST_GENERATE_SCOPE_DATA_PATH;
    return testGenerateScope(sourcePath);
}
