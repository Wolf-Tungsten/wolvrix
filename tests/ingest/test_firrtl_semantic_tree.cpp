#include "firrtl_test_fixture.hpp"
#include "transform/hier_flatten.hpp"

#include <fstream>

namespace {
using namespace firrtl_test;

bool hasPath(const grh::Graph& graph, std::span<const grh::SemanticId> ids,
             const std::vector<std::string>& path) {
    return std::any_of(ids.begin(), ids.end(), [&](grh::SemanticId id) {
        return graph.semanticTree()->path(id) == path;
    });
}

std::size_t countValues(const grh::Graph& graph, const std::vector<std::string>& path) {
    return std::count_if(graph.values().begin(), graph.values().end(), [&](grh::ValueId value) {
        return hasPath(graph, graph.valueSemanticIds(value), path);
    });
}

std::size_t countOperations(const grh::Graph& graph, const std::vector<std::string>& path) {
    return std::count_if(graph.operations().begin(), graph.operations().end(), [&](grh::OperationId op) {
        return hasPath(graph, graph.opSemanticIds(op), path);
    });
}

void checkFlattened(grh::Design& design, bool globalPlan) {
    tx::HierFlattenOptions options;
    options.useGlobalPlan = globalPlan;
    require(node_test::run(design, std::make_unique<tx::HierFlattenPass>(options)),
            globalPlan ? "global flatten failed" : "legacy flatten failed");
    require(design.graphs().size() == 1, "flatten did not delete the source definition");
    const auto& top = *design.findGraph("top");
    require(top.semanticTree(), "flatten lost SemanticTree");
    require(countValues(top, {"top", "c0", "child", "n"}) == 1,
            "first instance lost its node result");
    require(countValues(top, {"top", "c1", "child", "n"}) == 1,
            "second instance lost its node result");
    require(countOperations(top, {"top", "c0", "child", "n"}) >= 1 &&
            countOperations(top, {"top", "c1", "child", "n"}) >= 1,
            "flatten did not remap operation sources independently per instance");
    require(countValues(top, {"top", "a0"}) == 1 && countValues(top, {"top", "a1"}) == 1,
            "top input sources disappeared");
    require(countValues(top, {"top", "c0", "child", "a"}) == 1 &&
            countValues(top, {"top", "c1", "child", "a"}) == 1,
            "child input sources disappeared from their boundary values");
    const bool mergedPortSources = std::any_of(top.values().begin(), top.values().end(), [&](grh::ValueId value) {
        const auto ids = top.valueSemanticIds(value);
        return ids.size() > 1 && hasPath(top, ids, {"top", "c0", "child", "a"});
    });
    require(mergedPortSources, "port aliases did not retain multiple direct source associations");
    require(countValues(top, {"top", "w", "bits", "data"}) == 1,
            "bundle field path changed during flatten");
    for (const auto op : top.operations()) {
        for (const auto attr : top.opAttrRange(op)) {
            require(!node::isAttribute(attr.key()) && attr.key() != "originModules",
                    "flatten emitted a legacy source attribute");
        }
    }
}

} // namespace

int main() {
    try {
        const auto path = artifact("firrtl-semantic-tree") / "hierarchy.fir";
        std::ofstream(path) << R"FIR(FIRRTL version 6.0.0
circuit top :
  module child :
    input a : UInt<8>
    output y : UInt<8>
    node n = add(a, UInt<8>(1))
    connect y, n
  public module top :
    input a0 : UInt<8>
    input a1 : UInt<8>
    output y0 : UInt<8>
    output y1 : UInt<8>
    output wy : UInt<8>
    wire w : {bits : {data : UInt<8>}}
    connect w.bits.data, a0
    connect wy, w.bits.data
    inst c0 of child
    inst c1 of child
    connect c0.a, a0
    connect c1.a, a1
    connect y0, c0.y
    connect y1, c1.y
)FIR";
        auto base = firrtl_test::design(path);
        const auto& lowered = *base.findGraph("top");
        const auto aggregate = lowered.findValue("w__DOT__bits__DOT__data");
        require(aggregate.valid(), "lowered bundle value is missing");
        require(hasPath(lowered, lowered.valueSemanticIds(aggregate), {"top", "w", "bits", "data"}),
                "lowering did not retain the bundle field path");
        const auto& child = *base.findGraph("child");
        const auto childInput = child.findValue("a");
        require(childInput.valid() && hasPath(child, child.valueSemanticIds(childInput), {"child", "a"}),
                "lowering did not retain the child input source");
        auto legacy = base.clone();
        checkFlattened(legacy, false);
        auto global = base.clone();
        checkFlattened(global, true);
        ingest::IngestRequest request;
        request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
        request.firrtl.nodeAnnotations = true;
        request.pipeline = ingest::IngestPipeline::GrhPreFlatten;
        auto prefixed = ingest::readSources(request);
        require(prefixed.success() && prefixed.design, diagnostics(prefixed));
        checkFlattened(*prefixed.design, true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[firrtl-semantic-tree] " << error.what() << '\n';
        return 1;
    }
}
