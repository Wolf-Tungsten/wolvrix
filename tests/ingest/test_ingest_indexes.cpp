#include "firrtl_test_fixture.hpp"
#include "../../lib/ingest/firrtl/annotations.hpp"
#include "../../lib/ingest/firrtl/semantic.hpp"
#include "../../lib/ingest/mixed.hpp"

#include <chrono>

namespace {
using namespace firrtl_test;
namespace fi = ingest::firrtl;

void semanticPorts(std::size_t count) {
    std::string text = "FIRRTL version 6.0.0\ncircuit top :\n  module leaf :\n";
    for (std::size_t i = 0; i < count; ++i) text += "    input p" + std::to_string(i) + " : UInt\n";
    text += "    skip\n  public module top :\n    inst u of leaf\n";
    auto indexed = fi::indexText(std::move(text), "indexed-ports.fir", "fixture");
    require(indexed.success(), "large interface index failed");
    auto leaf = fi::parseModule(indexed.source, *indexed.source->findModule("leaf"));
    auto top = fi::parseModule(indexed.source, *indexed.source->findModule("top"));
    require(leaf.success() && top.success(), "large interface parse failed");
    auto signature = ingest::describeFirrtlModule(*indexed.source, indexed.source->modules()[0]);
    for (std::size_t i = 0; i < count; ++i) signature.ports[i].elementWidth = static_cast<int32_t>(1 + i % 15);
    std::reverse(signature.ports.begin(), signature.ports.end());
    auto duplicate = signature.ports.back(); duplicate.elementWidth = 37;
    signature.ports.push_back(std::move(duplicate));
    std::map<std::string, ingest::ModuleSignature> interfaces{{"leaf", std::move(signature)}};
    ingest::FirrtlOptions options;
    {
        fi::detail::Semantics semantics(*indexed.source, *leaf.body, options, &interfaces);
        semantics.analyze();
        for (std::size_t i = 0; i < count; ++i)
            require(semantics.width(semantics.declarations.at("p" + std::to_string(i)).type) == int64_t(1 + i % 15),
                "reordered or duplicate own interface changed the first matching width");
    }
    {
        fi::detail::Semantics semantics(*indexed.source, *top.body, options, &interfaces);
        semantics.analyze();
        const auto leaves = semantics.leaves(semantics.declarations.at("u").type);
        require(leaves.size() == count, "instance interface lost ports");
        for (std::size_t i = 0; i < count; ++i)
            require(semantics.width(leaves[i].type) == int64_t(1 + i % 15),
                "reordered or duplicate instance interface changed the first matching width");
    }
    // A later inference round replaces the signature after the prior analysis dies.
    interfaces.at("leaf").ports.front().shape = {2};
    try {
        fi::detail::Semantics semantics(*indexed.source, *top.body, options, &interfaces);
        semantics.analyze();
        require(false, "incompatible resolved instance geometry was accepted");
    } catch (const fi::detail::Error& error) {
        require(std::string(error.what()).find("array geometry differs") != std::string::npos,
            "resolved instance geometry diagnostic changed");
    }
}

enum class BadPort { None, Unknown, Duplicate, Type };
void linkedPorts(std::size_t count, BadPort bad = BadPort::None) {
    grh::Design design;
    auto& child = design.createGraph("child");
    auto& parent = design.createGraph("parent"); design.markAsTop("parent");
    std::vector<grh::ValueId> values;
    for (std::size_t i = 0; i < count; ++i) {
        const auto name = "p" + std::to_string(i);
        const auto input = child.createValue(child.internSymbol(name), {grh::ValueType::Logic, 1, false});
        child.bindInputPort(name, input);
        const auto value = parent.createValue(parent.internSymbol(name), {grh::ValueType::Logic,
            bad == BadPort::Type && i + 1 == count ? 2 : 1, false});
        parent.bindInputPort(name, value); values.push_back(value);
    }
    const auto instance = parent.createOperation(OperationKind::kInstance);
    parent.setAttr(instance, "moduleName", std::string("child"));
    std::vector<std::string> names;
    for (std::size_t i = count; i > 0; --i) {
        names.push_back("p" + std::to_string(i - 1)); parent.addOperand(instance, values[i - 1]);
    }
    if (bad == BadPort::Unknown) names.front() = "missing";
    if (bad == BadPort::Duplicate) names.front() = names[1];
    parent.setAttr(instance, "inputPortName", std::move(names));
    parent.setAttr(instance, "outputPortName", std::vector<std::string>{});
    parent.setAttr(instance, "inoutPortName", std::vector<std::string>{});
    std::vector<ingest::IngestDiagnostic> diagnostics;
    const bool success = ingest::verifyLinkedDesign(design, diagnostics);
    require(success == (bad == BadPort::None), "linked port validation accepted an invalid ABI or rejected reordered ports");
    if (!success) {
        const auto expected = bad == BadPort::Type ? "incompatible linked instance port ABI: child.p" :
            "unknown or duplicate linked instance port: child.";
        require(!diagnostics.empty() && diagnostics.front().message.starts_with(expected), "linked port diagnostic changed");
    }
}

void unprovedLocalCycleIsChecked() {
    grh::Design design;
    auto& graph = design.createGraph("cycle");
    design.markAsTop("cycle");
    const auto value = graph.createValue(graph.internSymbol("feedback"), 1, false);
    const auto op = graph.createOperation(grh::OperationKind::kNot);
    graph.addOperand(op, value);
    graph.addResult(op, value);
    require(!graph.localCyclesChecked(), "cycle fixture unexpectedly carried a proof");

    std::vector<ingest::IngestDiagnostic> diagnostics;
    require(!ingest::verifyLinkedDesign(design, diagnostics),
        "unproved local combinational cycle was skipped by linked verification");
    require(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.passName == "comb-loop-elim" &&
            diagnostic.message.find("comb loop detected") != std::string::npos;
    }), "unproved local cycle did not publish comb-loop diagnostic");
}

void moduleAnnotations(std::size_t modules, std::size_t perModule) {
    std::string json = "[";
    for (std::size_t record = 0; record < perModule; ++record) for (std::size_t module = 0; module < modules; ++module) {
        if (record || module) json += ',';
        json += "{\"class\":\"firrtl.transforms.DedupGroupAnnotation\",\"target\":\"~|m" + std::to_string(module) +
            "\",\"group\":\"g" + std::to_string(record) + "\"}";
    }
    json += ']';
    const auto annotations = fi::detail::parseAnnotations(json);
    auto indexed = fi::indexText("FIRRTL version 6.0.0\ncircuit top :\n  public module top :\n    skip\n", "annotations.fir", "fixture");
    require(indexed.success(), "annotation source index failed");
    grh::Design design;
    for (std::size_t i = 0; i < modules; ++i) {
        const auto name = "m" + std::to_string(i);
        fi::Module module; module.name = name;
        auto& graph = design.createGraph(name);
        fi::detail::applyAnnotations(*indexed.source, module, annotations, graph);
        const auto stored = graph.metadata("firrtl.annotations");
        require(stored.has_value(), "module annotation metadata lost");
        const auto& records = std::get<std::vector<std::string>>(*stored);
        require(records.size() == perModule, "interleaved annotations crossed module boundaries");
        for (std::size_t j = 0; j < perModule; ++j)
            require(records[j] == annotations.records[j * modules + i].raw,
                "module annotation order or duplicate record preservation changed");
    }
}

template<class Callback> void timed(std::string_view name, Callback callback) {
    const auto start = std::chrono::steady_clock::now(); callback();
    std::cout << name << "_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            timed("semantic_ports_8192", [] { semanticPorts(8192); });
            timed("linked_ports_8192", [] { linkedPorts(8192); });
            timed("annotations_1024x128", [] { moduleAnnotations(1024, 128); });
        } else {
            semanticPorts(256); linkedPorts(256);
            linkedPorts(4, BadPort::Unknown); linkedPorts(4, BadPort::Duplicate); linkedPorts(4, BadPort::Type);
            unprovedLocalCycleIsChecked();
            moduleAnnotations(16, 8);
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
