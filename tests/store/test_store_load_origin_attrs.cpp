// originModules JSON round-trip (M2): the generic attribute mechanism must
// carry vector<string> op attrs through store -> load unchanged.
#include "core/grh.hpp"
#include "core/load.hpp"
#include "core/store.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{

int fail(const std::string &message)
{
    std::cerr << "[store-load-origin-attrs] " << message << '\n';
    return 1;
}

} // namespace

int main()
{
    using namespace wolvrix::lib;

    grh::Design design;
    grh::Graph &graph = design.createGraph("demo");
    design.markAsTop("demo");

    const grh::ValueId in = graph.createValue(graph.internSymbol("in"), 8, false);
    const grh::ValueId out = graph.createValue(graph.internSymbol("out"), 8, false);
    graph.bindInputPort("in", in);
    graph.bindOutputPort("out", out);

    const grh::OperationId add = graph.createOperation(grh::OperationKind::kAdd,
                                                       graph.internSymbol("add0"));
    graph.addOperand(add, in);
    graph.addOperand(add, in);
    graph.addResult(add, out);
    graph.setAttr(add, std::string(transform::kOriginModulesKey),
                  std::vector<std::string>{"u_core$u_exu$u_alu", "u_core$u_biu"});
    graph.setAttr(add, "weights", grh::AttributeValue(std::vector<int64_t>{1, 2}));

    // An op with a single origin and an op with no origin at all.
    const grh::OperationId invert = graph.createOperation(grh::OperationKind::kNot,
                                                          graph.internSymbol("inv0"));
    const grh::ValueId invVal = graph.createValue(graph.internSymbol("inv_val"), 8, false);
    graph.addOperand(invert, in);
    graph.addResult(invert, invVal);
    graph.setAttr(invert, std::string(transform::kOriginModulesKey),
                  std::vector<std::string>{"top"});

    store::StoreDiagnostics storeDiags;
    store::StoreJson store(&storeDiags);
    const std::optional<std::string> json = store.storeToString(design);
    if (!json)
    {
        return fail("storeToString failed");
    }

    load::LoadJson loader;
    grh::Design loaded = loader.load(*json);
    grh::Graph *loadedGraph = loaded.findGraph("demo");
    if (!loadedGraph)
    {
        return fail("loaded design missing graph 'demo'");
    }

    const grh::OperationId loadedAdd = loadedGraph->findOperation("add0");
    if (!loadedAdd.valid())
    {
        return fail("loaded graph missing op 'add0'");
    }
    const auto addOrigins = transform::getOriginModules(*loadedGraph, loadedAdd);
    if (!addOrigins || addOrigins->size() != 2 ||
        (*addOrigins)[0] != "u_core$u_exu$u_alu" || (*addOrigins)[1] != "u_core$u_biu")
    {
        return fail("originModules did not round-trip on 'add0'");
    }
    const auto weights = loadedGraph->getOperation(loadedAdd).attr("weights");
    if (!weights || *weights != grh::AttributeValue(std::vector<int64_t>{1, 2}))
    {
        return fail("neighboring attrs must survive the round-trip unchanged");
    }

    const grh::OperationId loadedInv = loadedGraph->findOperation("inv0");
    if (!loadedInv.valid())
    {
        return fail("loaded graph missing op 'inv0'");
    }
    const auto invOrigins = transform::getOriginModules(*loadedGraph, loadedInv);
    if (!invOrigins || invOrigins->size() != 1 || (*invOrigins)[0] != "top")
    {
        return fail("single-entry originModules did not round-trip on 'inv0'");
    }

    // Round-trip stability: storing the loaded design must reproduce the
    // same attributes byte-identically.
    store::StoreJson store2(&storeDiags);
    const std::optional<std::string> json2 = store2.storeToString(loaded);
    if (!json2 || *json2 != *json)
    {
        return fail("store -> load -> store is not byte-stable");
    }
    return 0;
}
