#include "core/grh.hpp"
#include "core/load.hpp"
#include "core/store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef WOLF_SV_EMIT_ARTIFACT_DIR
#error "WOLF_SV_EMIT_ARTIFACT_DIR must be defined"
#endif

namespace {

using namespace wolvrix::lib;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path &path, const std::string &text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    require(!stream.fail(), "failed to write fixture");
}

std::string stored(const grh::Design &design, store::JsonPrintMode mode = store::JsonPrintMode::PrettyCompact) {
    store::StoreJson writer;
    store::StoreOptions options;
    options.jsonMode = mode;
    const auto text = writer.storeToString(design, options);
    require(text.has_value(), "failed to serialize fixture");
    return *text;
}

void checkBoth(const std::filesystem::path &path, const std::string &text) {
    writeFile(path, text);
    load::LoadJson loader;
    const auto expected = loader.load(text);
    const auto actual = loader.loadFile(path);
    require(stored(actual) == stored(expected), "file/text load differ: " + path.string());
}

void checkRejected(const std::filesystem::path &path, const std::string &text) {
    writeFile(path, text);
    load::LoadJson loader;
    std::string expectedError;
    try { auto design = loader.load(text); }
    catch (const std::exception &error) { expectedError = error.what(); }
    require(!expectedError.empty(), "text loader unexpectedly accepted invalid fixture");
    std::string actualError;
    try { auto design = loader.loadFile(path); }
    catch (const std::exception &error) { actualError = error.what(); }
    require(!actualError.empty(), "file loader unexpectedly accepted invalid fixture");
    require(actualError == expectedError, "file/text diagnostics differ: " + actualError + " / " + expectedError);
}

grh::Design fixture() {
    grh::Design design;
    auto &graph = design.createGraph("top");
    design.createGraph("empty");
    design.registerGraphAlias("top_alias", graph);
    design.markAsTop("top");
    design.addDeclaredSymbol(design.internSymbol("design_decl"));
    graph.addDeclaredSymbol(graph.internSymbol("graph_decl"));
    const auto z = graph.createValue(graph.internSymbol("z"), 8, false);
    const auto a = graph.createValue(graph.internSymbol("a"), 8, false);
    const auto mid = graph.createValue(graph.internSymbol("mid"), 8, false);
    const auto out = graph.createValue(graph.internSymbol("out"), 8, false);
    graph.bindInputPort("z", z);
    graph.bindInputPort("a", a);
    graph.bindOutputPort("out", out);
    const auto consumer = graph.createOperation(grh::OperationKind::kAssign, graph.internSymbol("consumer"));
    graph.addOperand(consumer, mid);
    graph.addResult(consumer, out);
    const auto producer = graph.createOperation(grh::OperationKind::kAdd, graph.internSymbol("producer"));
    graph.addOperand(producer, z);
    graph.addOperand(producer, a);
    graph.addResult(producer, mid);
    const auto group = graph.addGenerateGroup(graph.internSymbol("scope"), graph.internSymbol("group"));
    graph.addGenerateGroupSymbol(group, graph.valueSymbol(out));
    graph.addGenerateGroupSymbol(group, graph.operationSymbol(producer));

    std::string longText(64 * 1024 - 2, 'x');
    longText += "\n\"\\\t unicode \xC3\xA9";
    longText.append(64 * 1024, 'y');
    graph.setMetadata("long-text", longText);
    graph.setMetadata("bools", std::vector<bool>{false, true, false});
    graph.setMetadata("ints", std::vector<int64_t>{-7, 0, 9223372036854775807LL});
    graph.setMetadata("doubles", std::vector<double>{-0.5, 1.25, 1.0e100});
    graph.setMetadata("scalar-bool", true);
    graph.setMetadata("scalar-int", int64_t{-123});
    graph.setMetadata("scalar-double", 3.125);
    std::vector<std::string> inventory;
    for (unsigned i = 0; i < 30000; ++i)
        inventory.push_back("[\"top\",\"instance_" + std::to_string(i) + "\",\"node\"]");
    graph.setMetadata("firrtl.nodeInventory", std::move(inventory));
    graph.setAttr(producer, "originModules", std::vector<std::string>{"u_top$u_producer", "u_alias"});
    graph.setAttr(producer, "note", longText);
    graph.setAttr(producer, "empty", std::vector<std::string>{});
    grh::SrcLoc location;
    location.file = "test\\file\".fir";
    location.line = 123;
    location.origin = "origin\nline";
    location.note = longText;
    graph.setValueSrcLoc(mid, location);
    graph.setOpSrcLoc(producer, location);

    const auto ioIn = graph.createValue(graph.internSymbol("io_in"), 1, false);
    const auto ioOut = graph.createValue(graph.internSymbol("io_out"), 1, false);
    const auto ioOe = graph.createValue(graph.internSymbol("io_oe"), 1, false);
    graph.bindInoutPort("io", ioIn, ioOut, ioOe);
    graph.createValue(graph.internSymbol("shaped"), 48, true, grh::ValueType::Logic,
                      std::vector<int64_t>{2, 3}, grh::LogicDomain::TwoState);
    for (unsigned i = 0; i < 5000; ++i)
        graph.createValue(graph.internSymbol("unused_" + std::to_string(i)), 1, false);
    return design;
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::path(WOLF_SV_EMIT_ARTIFACT_DIR) / "load-json-file";
        std::filesystem::create_directories(directory);
        auto design = fixture();
        unsigned index = 0;
        for (const auto mode : {store::JsonPrintMode::Compact, store::JsonPrintMode::PrettyCompact,
                                store::JsonPrintMode::Pretty}) {
            checkBoth(directory / ("mode-" + std::to_string(index++) + ".json"), stored(design, mode));
        }

        const std::string reordered = R"json({
          "tops":["g"], "aliases":{"alias":"g"}, "declaredSymbols":["design"],
          "graphs":[{
            "ops":[{"out":["out"],"in":["input"],"attrs":{
              "text":{"value":"\u00e9\n\\\"","kind":"str"},
              "items":{"values":[1,2,3],"k":"int_array"}},"sym":"assign","kind":"kAssign"}],
            "ports":{"out":[{"val":"out","name":"out"}],"in":[{"val":"input","name":"input"}]},
            "metadata":{"opaque":{"values":["a","b"],"kind":"string_array"}},
            "generateGroups":[{"symbols":["assign","out"],"name":"grp","scope":"scope"}],
            "vals":[{"out":false,"in":true,"sgn":false,"w":8,"sym":"input"},
                    {"out":true,"in":false,"sgn":false,"w":8,"sym":"out"}],
            "declaredSymbols":["decl"],"symbol":"g","symbol":"ignored duplicate"
          }]
        })json";
        checkBoth(directory / "reordered-legacy.json", reordered);
        checkRejected(directory / "trailing.json", reordered + "garbage");
        checkRejected(directory / "truncated.json", reordered.substr(0, reordered.size() - 4));
        checkRejected(directory / "invalid-escape.json", "{\"graphs\":[],\"declaredSymbols\":[],\"unknown\":[\"\\q\"]}");
        checkRejected(directory / "invalid-number.json", "{\"graphs\":[],\"declaredSymbols\":[],\"unknown\":[1e99999]}");
        checkRejected(directory / "unknown-value.json", R"json({"graphs":[{
            "symbol":"g","declaredSymbols":[],"vals":[],
            "ops":[{"sym":"bad","kind":"kAssign","in":["missing"],"out":[]}]
          }],"declaredSymbols":[]})json");
        checkRejected(directory / "version.json", R"json({"graphs":[{
            "symbol":"g","formatVersion":999,"declaredSymbols":[],"vals":[]
          }],"declaredSymbols":[]})json");
        bool missingRejected = false;
        try { auto result = load::LoadJson().loadFile(directory / "missing.json"); }
        catch (const std::exception &) { missingRejected = true; }
        require(missingRejected, "missing file was accepted");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "[load-json-file] " << error.what() << '\n';
        return 1;
    }
}
