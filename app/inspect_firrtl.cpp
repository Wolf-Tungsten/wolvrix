#include "ingest/firrtl.hpp"

#include <chrono>
#include <iostream>

namespace fi = wolvrix::lib::ingest::firrtl;
namespace in = wolvrix::lib::ingest;

namespace {
void diagnostics(const std::vector<in::IngestDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.location) {
            const auto& position = *diagnostic.location;
            std::cerr << position.file.string() << ':' << position.line << ':' << position.column << ": ";
        }
        std::cerr << diagnostic.message;
        if (!diagnostic.context.empty()) std::cerr << " [module " << diagnostic.context << ']';
        std::cerr << '\n';
        if (diagnostic.location) {
            std::cerr << diagnostic.location->sourceLine << '\n';
            if (!diagnostic.location->sourceInfo.empty()) std::cerr << diagnostic.location->sourceInfo << '\n';
        }
    }
}
}

int main(int argc, char** argv) {
    bool indexOnly = false;
    std::string module, unit = "inspection";
    in::FirrtlOptions options;
    std::filesystem::path path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--index-only") indexOnly = true;
        else if ((argument == "--module" || argument == "--unit" || argument == "--expected-version") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (argument == "--module") module = value;
            else if (argument == "--unit") unit = value;
            else options.expectedVersion = value;
        } else if (argument.starts_with('-') || !path.empty()) {
            std::cerr << "usage: inspect-firrtl [--index-only | --module NAME] [--unit UNIT] [--expected-version VERSION] INPUT\n";
            return 2;
        } else path = argument;
    }
    if (path.empty() || (indexOnly && !module.empty())) { std::cerr << "specify an input and a single validation mode\n"; return 2; }
    const auto start = std::chrono::steady_clock::now();
    auto indexed = fi::indexFile(path, unit, options);
    const auto indexEnd = std::chrono::steady_clock::now();
    diagnostics(indexed.diagnostics);
    if (!indexed.success()) return 1;
    const auto& source = *indexed.source;
    uint64_t ports = 0, external = 0;
    for (const auto& declaration : source.modules()) {
        ports += declaration.ports.size();
        external += declaration.kind == fi::ModuleKind::External;
    }
    std::cout << "file=" << source.path().string() << '\n'
              << "unit=" << source.unit() << '\n'
              << "version=" << source.version() << '\n'
              << "circuit=" << source.circuit() << '\n'
              << "bytes=" << source.text().size() << '\n'
              << "modules=" << source.modules().size() << '\n'
              << "external_modules=" << external << '\n'
              << "ports=" << ports << '\n'
              << "interface_types=" << source.arena().types.size() - 1 << '\n'
              << "annotation_bytes=" << source.annotations().size() << '\n'
              << "layer_roots=" << source.layers().size() << '\n';
    bool success = true;
    uint64_t leafPorts = 0, unresolvedPorts = 0;
    for (const auto& declaration : source.modules()) {
        try {
            const auto signature = in::describeFirrtlModule(source, declaration);
            leafPorts += signature.ports.size();
            for (const auto& leaf : signature.ports) unresolvedPorts += leaf.requiresTypeResolution;
        } catch (const std::exception& error) {
            std::cerr << "interface '" << declaration.name << "': " << error.what() << '\n'; success = false;
        }
    }
    std::cout << "leaf_ports=" << leafPorts << "\nunresolved_leaf_ports=" << unresolvedPorts << '\n';
    if (!module.empty()) {
        const auto id = source.findModule(module);
        if (!id) { std::cerr << "unknown module '" << module << "'\n"; return 1; }
        auto parsed = fi::parseModule(indexed.source, *id);
        diagnostics(parsed.diagnostics); success &= parsed.success();
        if (parsed.success()) std::cout << "module=" << module << "\nstatements=" << parsed.body->arena.statements.size()
                               << "\nexpressions=" << parsed.body->arena.expressions.size() << '\n';
    } else if (!indexOnly) {
        const auto validated = fi::validate(indexed.source);
        diagnostics(validated.diagnostics); success &= validated.success();
        std::cout << "validated_modules=" << validated.modules << "\nstatements=" << validated.statements
                  << "\nexpressions=" << validated.expressions << "\nmax_module_statements=" << validated.maxModuleStatements << '\n';
        for (const auto& [name, count] : validated.constructs) std::cout << name << '=' << count << '\n';
    }
    const auto end = std::chrono::steady_clock::now();
    std::cout << "index_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(indexEnd - start).count()
              << "\nbody_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(end - indexEnd).count()
              << "\nvalidation=" << (indexOnly ? "interfaces-only" : module.empty() ? "all-module-bodies" : "selected-module")
              << "\nstatus=" << (success ? "passed" : "failed") << '\n';
    return success ? 0 : 1;
}
